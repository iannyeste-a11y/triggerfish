#pragma once

#include "plugin_host_interface.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <vector>

namespace triggerfish {

// Implements radium::PluginHostSession using JUCE's AudioPluginInstance.
// Each instance wraps one loaded VST3 plugin for a single insert slot.
class JuceVst3Host : public radium::PluginHostSession,
                     public juce::AudioPlayHead {
public:
    JuceVst3Host() = default;
    ~JuceVst3Host() override;

    // Load a VST3 plugin from the given file path + class ID.
    // pluginName is the display name used for matching inside shell plugins.
    // Returns true on success. Must be called from the message thread.
    bool loadPlugin(const juce::String& filePath,
                    const juce::String& classId,
                    double sampleRate,
                    int blockSize,
                    juce::String& errorMessage,
                    const juce::String& pluginName = {},
                    bool allowFallbackScan = true);

    // Unload the current plugin. Safe to call if nothing is loaded.
    void unloadPlugin();

    // PluginHostSession interface
    bool process_block(float* interleaved_stereo, int frame_count, int sample_rate) override;
    bool is_configured() const override;
    void reset_processing() override;

    // Access the underlying plugin instance (for editor, state, etc.)
    juce::AudioPluginInstance* getPluginInstance() const { return plugin_.get(); }

    // State save/restore
    void getState(juce::MemoryBlock& destData) const;
    void setState(const void* data, int sizeInBytes);

    // Plugin info
    juce::String getPluginName() const;

    // Set the BPM for tempo-synced plugins (default 120)
    void setTempo(double bpm) { bpm_ = bpm; }

    // AudioPlayHead
    juce::Optional<juce::AudioPlayHead::PositionInfo> getPosition() const override;

private:
    double bpm_ = 120.0;
    int64_t samplePosition_ = 0;
    std::unique_ptr<juce::AudioPluginInstance> plugin_;
    juce::AudioBuffer<float> deinterleavedBuffer_;
    juce::MidiBuffer emptyMidi_;
    double currentSampleRate_ = 44100.0;
    int currentBlockSize_ = 512;
};

}  // namespace triggerfish
