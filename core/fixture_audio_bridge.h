#pragma once

#include "import_model.h"
#include "playback_engine.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace radium {

struct FixtureAudioDecodeResult {
    bool success = false;
    AudioBuffer audio;
    std::vector<std::string> diagnostics;
};

struct FixtureAudioResolution {
    std::unordered_map<std::string, AudioBuffer> buffers_by_reference;
    std::vector<std::string> mapped_layer_indices;
    std::vector<std::string> unmapped_layer_indices;
    std::vector<std::string> diagnostics;
};

FixtureAudioDecodeResult decode_embedded_flac(
    const std::filesystem::path& flac_path,
    const std::filesystem::path& working_directory
);

FixtureAudioDecodeResult decode_audio_file(
    const std::filesystem::path& audio_path,
    const std::filesystem::path& working_directory
);

FixtureAudioResolution resolve_fixture_audio(
    const Preset& imported_preset,
    const std::filesystem::path& working_directory
);

}  // namespace radium
