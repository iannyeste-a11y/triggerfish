#pragma once

#include "app_controller.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <vector>

class TriggerfishProcessor : public juce::AudioProcessor {
public:
    TriggerfishProcessor();
    ~TriggerfishProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    radium::AppController& controller() { return controller_; }
    float masterGain() const { return masterGain_.load(std::memory_order_relaxed); }
    void setMasterGain(float g) { masterGain_.store(g, std::memory_order_relaxed); }
    std::uint64_t midiActivityCounter() const { return midiActivityCounter_.load(std::memory_order_relaxed); }
    bool recordBusSurroundEnabled() const { return controller_.record_bus_surround_enabled(); }
    bool setRecordBusSurroundEnabled(bool enabled);
    radium::RecordBusMode recordBusMode() const { return controller_.record_bus_mode(); }
    bool setRecordBusMode(radium::RecordBusMode mode);

private:
    radium::AppController controller_;
    std::vector<float> interleavedBuffer_;
    std::atomic<float> masterGain_{1.0f};
    std::atomic<std::uint64_t> midiActivityCounter_{0};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TriggerfishProcessor)
};
