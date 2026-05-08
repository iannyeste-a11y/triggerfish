#pragma once

#include "app_controller.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace triggerfish {

// Returns an AudioDecodeFunc that uses JUCE's AudioFormatManager to decode
// audio files (FLAC, WAV, AIFF, OGG, MP3). Thread-safe — each call creates
// its own reader.
radium::AudioDecodeFunc make_juce_audio_decode_func();

// Returns an AudioEncodeFunc that compresses an in-memory AudioBuffer to FLAC
// bytes for embedding inside a project file. Sample rate and channel count are
// preserved.
radium::AudioEncodeFunc make_juce_flac_encode_func();

// Returns an AudioDecodeBytesFunc that decodes a FLAC blob (as produced by
// make_juce_flac_encode_func) back into a FixtureAudioDecodeResult.
radium::AudioDecodeBytesFunc make_juce_flac_decode_bytes_func();

}  // namespace triggerfish
