#include "JuceVst3Host.h"

namespace triggerfish {

JuceVst3Host::~JuceVst3Host() {
    unloadPlugin();
}

bool JuceVst3Host::loadPlugin(const juce::String& filePath,
                               const juce::String& classId,
                               double sampleRate,
                               int blockSize,
                               juce::String& errorMessage,
                               const juce::String& pluginName,
                               bool allowFallbackScan) {
    unloadPlugin();

    juce::VST3PluginFormat vst3Format;

    // Build a PluginDescription directly so JUCE's createInstanceFromDescription
    // can find the right class in the factory WITHOUT calling findAllTypesForFile
    // (which initializes ALL components and triggers auth dialogs on shell plugins).
    juce::PluginDescription desc;
    desc.fileOrIdentifier = filePath;
    desc.pluginFormatName = "VST3";
    if (pluginName.isNotEmpty())
        desc.name = pluginName;
    if (classId.isNotEmpty()) {
        int uid = classId.getIntValue();
        if (uid != 0) {
            desc.uniqueId = uid;
            desc.deprecatedUid = uid;
        }
    }

    plugin_ = vst3Format.createInstanceFromDescription(desc, sampleRate, blockSize, errorMessage);

    // If direct creation failed (e.g. loaded from old project with different classId format),
    // fall back to findAllTypesForFile and match by name.
    if (plugin_ == nullptr && allowFallbackScan) {
        juce::OwnedArray<juce::PluginDescription> descriptions;
        vst3Format.findAllTypesForFile(descriptions, filePath);
        for (auto* d : descriptions) {
            if ((!pluginName.isEmpty() && d->name.trim() == pluginName.trim()) ||
                (!classId.isEmpty() && d->uniqueId == classId.getIntValue())) {
                plugin_ = vst3Format.createInstanceFromDescription(
                    *d, sampleRate, blockSize, errorMessage);
                if (plugin_ != nullptr) break;
            }
        }
    }

    if (plugin_ == nullptr) {
        if (errorMessage.isEmpty())
            errorMessage = "Failed to load plugin from: " + filePath;
        return false;
    }

    currentSampleRate_ = sampleRate;
    currentBlockSize_ = blockSize;
    samplePosition_ = 0;

    plugin_->setPlayHead(this);
    plugin_->prepareToPlay(sampleRate, blockSize);
    deinterleavedBuffer_.setSize(2, blockSize);

    return true;
}

void JuceVst3Host::unloadPlugin() {
    if (plugin_) {
        plugin_->releaseResources();
        plugin_.reset();
    }
}

bool JuceVst3Host::process_block(float* interleaved_stereo, int frame_count, int sample_rate) {
    if (!plugin_) return false;

    // Re-prepare if sample rate changed
    if (static_cast<double>(sample_rate) != currentSampleRate_ ||
        frame_count > currentBlockSize_) {
        currentSampleRate_ = static_cast<double>(sample_rate);
        currentBlockSize_ = frame_count;
        plugin_->prepareToPlay(currentSampleRate_, currentBlockSize_);
        deinterleavedBuffer_.setSize(2, currentBlockSize_);
    }

    deinterleavedBuffer_.setSize(2, frame_count, false, false, true);

    // Deinterleave: interleaved LRLRLR -> separate L and R channels
    auto* left = deinterleavedBuffer_.getWritePointer(0);
    auto* right = deinterleavedBuffer_.getWritePointer(1);
    for (int i = 0; i < frame_count; ++i) {
        left[i] = interleaved_stereo[i * 2];
        right[i] = interleaved_stereo[i * 2 + 1];
    }

    emptyMidi_.clear();
    plugin_->processBlock(deinterleavedBuffer_, emptyMidi_);
    samplePosition_ += frame_count;

    // Re-interleave back
    const auto* outL = deinterleavedBuffer_.getReadPointer(0);
    const auto* outR = deinterleavedBuffer_.getReadPointer(1);
    for (int i = 0; i < frame_count; ++i) {
        interleaved_stereo[i * 2] = outL[i];
        interleaved_stereo[i * 2 + 1] = outR[i];
    }

    return true;
}

bool JuceVst3Host::is_configured() const {
    return plugin_ != nullptr;
}

void JuceVst3Host::reset_processing() {
    if (!plugin_) {
        return;
    }
    samplePosition_ = 0;
    plugin_->reset();
}

void JuceVst3Host::getState(juce::MemoryBlock& destData) const {
    if (plugin_) {
        plugin_->getStateInformation(destData);
    }
}

void JuceVst3Host::setState(const void* data, int sizeInBytes) {
    if (plugin_) {
        plugin_->setStateInformation(data, sizeInBytes);
    }
}

juce::String JuceVst3Host::getPluginName() const {
    if (plugin_) {
        return plugin_->getName();
    }
    return {};
}

juce::Optional<juce::AudioPlayHead::PositionInfo> JuceVst3Host::getPosition() const {
    juce::AudioPlayHead::PositionInfo info;
    info.setBpm(bpm_);
    info.setTimeInSamples(samplePosition_);
    info.setTimeInSeconds(static_cast<double>(samplePosition_) / currentSampleRate_);
    info.setIsPlaying(true);
    info.setIsRecording(false);

    juce::AudioPlayHead::TimeSignature timeSig;
    timeSig.numerator = 4;
    timeSig.denominator = 4;
    info.setTimeSignature(timeSig);

    // Compute bar/beat position from sample position
    double beatsPerSecond = bpm_ / 60.0;
    double totalBeats = (static_cast<double>(samplePosition_) / currentSampleRate_) * beatsPerSecond;
    info.setPpqPosition(totalBeats);

    return info;
}

}  // namespace triggerfish
