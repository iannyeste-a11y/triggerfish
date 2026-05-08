#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "AudioFileLoader.h"
#include "PortableDataLocation.h"
#include <fstream>
#include <sstream>

static juce::File getWorkingDirectory() {
    return triggerfish::portableDataRoot().getChildFile("working");
}

TriggerfishProcessor::TriggerfishProcessor()
    : AudioProcessor(BusesProperties()
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      controller_(getWorkingDirectory().getFullPathName().toStdString()) {
    controller_.set_audio_decode_func(triggerfish::make_juce_audio_decode_func());
    controller_.new_empty_project();
}

TriggerfishProcessor::~TriggerfishProcessor() {
    controller_.stop_streaming_playback();
}

const juce::String TriggerfishProcessor::getName() const { return "Triggerfish"; }

bool TriggerfishProcessor::acceptsMidi() const { return true; }
bool TriggerfishProcessor::producesMidi() const { return false; }
bool TriggerfishProcessor::isMidiEffect() const { return false; }
double TriggerfishProcessor::getTailLengthSeconds() const { return 0.0; }

int TriggerfishProcessor::getNumPrograms() { return 1; }
int TriggerfishProcessor::getCurrentProgram() { return 0; }
void TriggerfishProcessor::setCurrentProgram(int) {}
const juce::String TriggerfishProcessor::getProgramName(int) { return {}; }
void TriggerfishProcessor::changeProgramName(int, const juce::String&) {}

void TriggerfishProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    controller_.set_output_sample_rate(static_cast<int>(sampleRate));
    controller_.streaming_mixer().prepare(static_cast<int>(sampleRate));
    interleavedBuffer_.resize(static_cast<std::size_t>(samplesPerBlock) * 8, 0.0f);
}

void TriggerfishProcessor::releaseResources() {
    controller_.stop_streaming_playback();
}

bool TriggerfishProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto output = layouts.getMainOutputChannelSet();
    return output == juce::AudioChannelSet::stereo()
        || output == juce::AudioChannelSet::create5point0()
        || output == juce::AudioChannelSet::create5point1()
        || output == juce::AudioChannelSet::create7point0()
        || output == juce::AudioChannelSet::create7point1();
}

bool TriggerfishProcessor::setRecordBusSurroundEnabled(bool enabled) {
    return setRecordBusMode(enabled ? radium::RecordBusMode::Surround51
                                    : radium::RecordBusMode::Stereo);
}

bool TriggerfishProcessor::setRecordBusMode(radium::RecordBusMode mode) {
    controller_.set_record_bus_mode(mode);
    BusesLayout layout = getBusesLayout();
    layout.outputBuses.clear();
    if (mode == radium::RecordBusMode::Surround71) {
        layout.outputBuses.add(juce::AudioChannelSet::create7point1());
    } else if (mode == radium::RecordBusMode::Surround70) {
        layout.outputBuses.add(juce::AudioChannelSet::create7point0());
    } else if (mode == radium::RecordBusMode::Surround51) {
        layout.outputBuses.add(juce::AudioChannelSet::create5point1());
    } else if (mode == radium::RecordBusMode::Surround50) {
        layout.outputBuses.add(juce::AudioChannelSet::create5point0());
    } else {
        layout.outputBuses.add(juce::AudioChannelSet::stereo());
    }
    setBusesLayout(layout);
    return true;
}

void TriggerfishProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                        juce::MidiBuffer& midiMessages) {
    juce::ScopedNoDenormals noDenormals;

    // Handle MIDI
    for (const auto metadata : midiMessages) {
        const auto msg = metadata.getMessage();
        if (msg.isNoteOn()) {
            midiActivityCounter_.fetch_add(1, std::memory_order_relaxed);
            std::string err;
            controller_.start_streaming_playback(msg.getNoteNumber(), &err);
        } else if (msg.isNoteOff()) {
            midiActivityCounter_.fetch_add(1, std::memory_order_relaxed);
            controller_.trigger_note_off(msg.getNoteNumber());
        }
    }

    const auto numSamples = buffer.getNumSamples();
    const auto numChannels = std::max(0, buffer.getNumChannels());

    // Ensure interleaved buffer is large enough
    const auto needed = static_cast<std::size_t>(numSamples) * static_cast<std::size_t>(std::max(1, numChannels));
    if (interleavedBuffer_.size() < needed) {
        interleavedBuffer_.resize(needed);
    }

    buffer.clear();

    // Render from StreamingMixer (interleaved device-channel output)
    controller_.streaming_mixer().render_block(interleavedBuffer_.data(),
                                               static_cast<std::size_t>(numSamples),
                                               numChannels);

    // Deinterleave into JUCE buffer and apply master gain
    const float gain = masterGain_.load(std::memory_order_relaxed);
    for (int channel = 0; channel < numChannels; ++channel) {
        auto* dest = buffer.getWritePointer(channel);
        for (int i = 0; i < numSamples; ++i) {
            dest[i] = interleavedBuffer_[static_cast<std::size_t>(i) * static_cast<std::size_t>(numChannels)
                                       + static_cast<std::size_t>(channel)] * gain;
        }
    }
}

bool TriggerfishProcessor::hasEditor() const { return true; }

juce::AudioProcessorEditor* TriggerfishProcessor::createEditor() {
    return new TriggerfishEditor(*this);
}

void TriggerfishProcessor::getStateInformation(juce::MemoryBlock& destData) {
    if (wrapperType == juce::AudioProcessor::wrapperType_Standalone) {
        destData.reset();
        return;
    }

    // Save project to a temp file, read it into the memory block
    auto tempFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                        .getChildFile("triggerfish_state.tfproj");
    std::string err;
    if (controller_.save_project(tempFile.getFullPathName().toStdString(), &err)) {
        juce::FileInputStream stream(tempFile);
        if (stream.openedOk()) {
            destData.reset();
            destData.setSize(static_cast<std::size_t>(stream.getTotalLength()));
            stream.read(destData.getData(), static_cast<int>(destData.getSize()));
        }
        tempFile.deleteFile();
    }

    // Append master gain as 4 bytes at the end
    float gain = masterGain_.load(std::memory_order_relaxed);
    destData.append(&gain, sizeof(float));
}

void TriggerfishProcessor::setStateInformation(const void* data, int sizeInBytes) {
    if (wrapperType == juce::AudioProcessor::wrapperType_Standalone) {
        controller_.new_empty_project();
        masterGain_.store(1.0f, std::memory_order_relaxed);
        return;
    }

    if (sizeInBytes <= static_cast<int>(sizeof(float))) return;

    // Last 4 bytes are master gain
    int projectSize = sizeInBytes - static_cast<int>(sizeof(float));
    float gain = 1.0f;
    std::memcpy(&gain, static_cast<const char*>(data) + projectSize, sizeof(float));
    masterGain_.store(gain, std::memory_order_relaxed);

    // Write project data to temp file, then load
    auto tempFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                        .getChildFile("triggerfish_state_load.tfproj");
    {
        juce::FileOutputStream stream(tempFile);
        if (stream.openedOk()) {
            stream.write(data, static_cast<size_t>(projectSize));
            stream.flush();
        }
    }
    std::string err;
    controller_.load_project(tempFile.getFullPathName().toStdString(), &err);
    tempFile.deleteFile();
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new TriggerfishProcessor();
}
