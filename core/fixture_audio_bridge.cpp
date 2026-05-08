#include "fixture_audio_bridge.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace radium {
namespace {

std::string quote_single(const std::string& value) {
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back('\'');
    for (char ch : value) {
        if (ch == '\'') {
            quoted += "''";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

std::string quote_double(const std::string& value) {
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back('"');
    for (char ch : value) {
        if (ch == '"') {
            quoted += "\\\"";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('"');
    return quoted;
}

std::string sanitize_component(const std::string& value) {
    std::string sanitized;
    sanitized.reserve(value.size());
    for (char ch : value) {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_') {
            sanitized.push_back(ch);
        } else {
            sanitized.push_back('_');
        }
    }
    return sanitized;
}

std::string to_windows_path(const std::filesystem::path& path) {
#ifdef _WIN32
    return std::filesystem::absolute(path).wstring().empty()
        ? std::string()
        : std::filesystem::absolute(path).string();
#else
    const std::string value = std::filesystem::absolute(path).generic_string();
    if (value.rfind("/mnt/", 0) == 0 && value.size() > 6 && value[6] == '/') {
        std::string converted;
        converted.reserve(value.size());
        converted.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(value[5]))));
        converted += ":";
        for (std::size_t i = 6; i < value.size(); ++i) {
            converted.push_back(value[i] == '/' ? '\\' : value[i]);
        }
        return converted;
    }
    throw std::runtime_error("Only /mnt/<drive>/ paths are supported for Windows-host FLAC decode.");
#endif
}

std::uint16_t read_u16(std::ifstream& stream) {
    const auto b0 = static_cast<std::uint8_t>(stream.get());
    const auto b1 = static_cast<std::uint8_t>(stream.get());
    return static_cast<std::uint16_t>(b0 | (b1 << 8));
}

std::uint32_t read_u32(std::ifstream& stream) {
    const auto b0 = static_cast<std::uint8_t>(stream.get());
    const auto b1 = static_cast<std::uint8_t>(stream.get());
    const auto b2 = static_cast<std::uint8_t>(stream.get());
    const auto b3 = static_cast<std::uint8_t>(stream.get());
    return static_cast<std::uint32_t>(b0 | (b1 << 8) | (b2 << 16) | (b3 << 24));
}

AudioBuffer load_pcm16_wav(const std::filesystem::path& wav_path) {
    std::ifstream stream(wav_path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Unable to open decoded WAV file.");
    }

    char riff[4]{};
    char wave[4]{};
    stream.read(riff, 4);
    (void) read_u32(stream);
    stream.read(wave, 4);
    if (std::string(riff, 4) != "RIFF" || std::string(wave, 4) != "WAVE") {
        throw std::runtime_error("Decoded file is not a RIFF/WAVE file.");
    }

    std::uint16_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint16_t bits_per_sample = 0;
    std::vector<std::int16_t> pcm16;

    while (stream && !stream.eof()) {
        char chunk_id[4]{};
        stream.read(chunk_id, 4);
        if (stream.gcount() != 4) {
            break;
        }
        const auto chunk_size = read_u32(stream);
        const std::string id(chunk_id, 4);
        if (id == "fmt ") {
            const auto audio_format = read_u16(stream);
            channels = read_u16(stream);
            sample_rate = read_u32(stream);
            (void) read_u32(stream);
            (void) read_u16(stream);
            bits_per_sample = read_u16(stream);
            if (chunk_size > 16) {
                stream.seekg(chunk_size - 16, std::ios::cur);
            }
            if (audio_format != 1 || bits_per_sample != 16) {
                throw std::runtime_error("Decoded WAV is not PCM16.");
            }
        } else if (id == "data") {
            pcm16.resize(chunk_size / sizeof(std::int16_t));
            stream.read(reinterpret_cast<char*>(pcm16.data()), static_cast<std::streamsize>(chunk_size));
        } else {
            stream.seekg(chunk_size, std::ios::cur);
        }
    }

    if (channels == 0 || sample_rate == 0 || pcm16.empty()) {
        throw std::runtime_error("Decoded WAV was missing required audio data.");
    }

    AudioBuffer buffer;
    buffer.sample_rate = static_cast<int>(sample_rate);
    buffer.channels = static_cast<int>(channels);
    buffer.samples.resize(pcm16.size());
    for (std::size_t i = 0; i < pcm16.size(); ++i) {
        buffer.samples[i] = static_cast<float>(pcm16[i] / 32768.0f);
    }
    return buffer;
}

}  // namespace

FixtureAudioDecodeResult decode_embedded_flac(
    const std::filesystem::path& flac_path,
    const std::filesystem::path& working_directory
) {
    FixtureAudioDecodeResult result;

    try {
        std::filesystem::create_directories(working_directory);
        const auto output_wav =
            working_directory / (sanitize_component(flac_path.stem().string()) + "_decoded.wav");

        const std::string input_win = to_windows_path(flac_path);
        const std::string output_win = to_windows_path(output_wav);
        std::ostringstream command;
#ifdef _WIN32
        command
            << "ffmpeg -y -v error -i " << quote_double(input_win)
            << " -ar 48000 -c:a pcm_s16le " << quote_double(output_win);
#else
        command
            << "/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe -NoProfile -Command "
            << quote_single(
                   "ffmpeg -y -v error -i " + quote_single(input_win) +
                   " -ar 48000 -c:a pcm_s16le " + quote_single(output_win)
               );
#endif

        const int exit_code = std::system(command.str().c_str());
        if (exit_code != 0) {
            result.diagnostics.push_back("Windows host ffmpeg decode failed for " + flac_path.generic_string());
            return result;
        }

        result.audio = load_pcm16_wav(output_wav);
        result.success = true;
    } catch (const std::exception& ex) {
        result.diagnostics.push_back(ex.what());
    }

    return result;
}

FixtureAudioDecodeResult decode_audio_file(
    const std::filesystem::path& audio_path,
    const std::filesystem::path& working_directory
) {
    FixtureAudioDecodeResult result;

    try {
        if (audio_path.extension() == ".wav" || audio_path.extension() == ".WAV") {
            try {
                result.audio = load_pcm16_wav(audio_path);
                result.success = true;
                return result;
            } catch (...) {
            }
        }

        std::filesystem::create_directories(working_directory);
        const auto output_wav =
            working_directory / (sanitize_component(audio_path.stem().string()) + "_decoded.wav");

        const std::string input_win = to_windows_path(audio_path);
        const std::string output_win = to_windows_path(output_wav);
        std::ostringstream command;
#ifdef _WIN32
        command
            << "ffmpeg -y -v error -i " << quote_double(input_win)
            << " -ar 48000 -c:a pcm_s16le " << quote_double(output_win);
#else
        command
            << "/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe -NoProfile -Command "
            << quote_single(
                   "ffmpeg -y -v error -i " + quote_single(input_win) +
                   " -ar 48000 -c:a pcm_s16le " + quote_single(output_win)
               );
#endif

        const int exit_code = std::system(command.str().c_str());
        if (exit_code != 0) {
            result.diagnostics.push_back("ffmpeg decode failed for " + audio_path.generic_string());
            return result;
        }

        result.audio = load_pcm16_wav(output_wav);
        result.success = true;
    } catch (const std::exception& ex) {
        result.diagnostics.push_back(ex.what());
    }

    return result;
}

FixtureAudioResolution resolve_fixture_audio(
    const Preset& imported_preset,
    const std::filesystem::path& working_directory
) {
    FixtureAudioResolution resolution;

    for (const auto& layer : imported_preset.layers) {
        if (!layer.active) {
            continue;
        }
        if (!layer.embedded_media_reference.has_value() || !layer.embedded_media_path.has_value()) {
            resolution.unmapped_layer_indices.push_back(std::to_string(layer.index));
            resolution.diagnostics.push_back(
                "Layer " + std::to_string(layer.index) + " has no explicit embedded media mapping."
            );
            continue;
        }

        if (resolution.buffers_by_reference.find(*layer.embedded_media_reference) == resolution.buffers_by_reference.end()) {
            const auto decode = decode_embedded_flac(*layer.embedded_media_path, working_directory);
            resolution.diagnostics.insert(
                resolution.diagnostics.end(),
                decode.diagnostics.begin(),
                decode.diagnostics.end()
            );
            if (!decode.success) {
                resolution.unmapped_layer_indices.push_back(std::to_string(layer.index));
                continue;
            }
            resolution.buffers_by_reference.emplace(*layer.embedded_media_reference, decode.audio);
        }
        resolution.mapped_layer_indices.push_back(std::to_string(layer.index));
    }

    return resolution;
}

}  // namespace radium
