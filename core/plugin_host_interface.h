#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace radium {

// Abstract interface for plugin processing, implemented by the JUCE layer.
// This allows core/ to remain independent of any plugin SDK.
class PluginHostSession {
public:
    virtual ~PluginHostSession() = default;

    // Process a block of interleaved stereo float audio in-place.
    virtual bool process_block(float* interleaved_stereo, int frame_count, int sample_rate) = 0;

    // Returns true if the plugin is loaded and ready to process audio.
    virtual bool is_configured() const = 0;

    // Reset transient plugin processing state for a fresh playback start.
    virtual void reset_processing() = 0;
};

}  // namespace radium
