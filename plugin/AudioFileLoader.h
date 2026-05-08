#pragma once

#include "app_controller.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace triggerfish {

// Returns an AudioDecodeFunc that uses JUCE's AudioFormatManager to decode
// audio files (FLAC, WAV, AIFF, OGG, MP3). Thread-safe — each call creates
// its own reader.
radium::AudioDecodeFunc make_juce_audio_decode_func();

}  // namespace triggerfish
