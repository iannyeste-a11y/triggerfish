#include "fixture_audio_bridge.h"
#include "import_to_playback.h"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_flac_decode() {
    const auto fixture = std::filesystem::current_path() / "fixtures" / "radium" / "NJR LARGE FACTORY POWER DOWN.radium";
    const auto output_dir = std::filesystem::current_path() / "artifacts" / "fixture_audio_decode";
    std::filesystem::create_directories(output_dir);

    radium::ParseOptions options;
    options.output_root = output_dir;
    const auto summary = radium::parse_radium_file(fixture, options);
    const auto imported = radium::build_import_preset(summary);

    require(!imported.layers.empty(), "imported preset had no layers");
    const auto& layer = imported.layers.front();
    require(layer.embedded_media_path.has_value(), "fixture layer had no mapped embedded media path");

    const auto decode = radium::decode_embedded_flac(*layer.embedded_media_path, output_dir);
    require(decode.success, "embedded flac decode failed");
    require(decode.audio.sample_rate == 48000, "decoded audio was not resampled to 48 kHz");
    require(decode.audio.channels == 1 || decode.audio.channels == 2, "decoded audio did not preserve mono/stereo channel count");
    require(decode.audio.frame_count() > 0, "decoded audio was empty");
}

void test_real_fixture_render() {
    const auto fixture = std::filesystem::current_path() / "fixtures" / "radium" / "NJR LARGE FACTORY POWER DOWN.radium";
    const auto output_dir = std::filesystem::current_path() / "artifacts" / "fixture_audio_render";
    std::filesystem::create_directories(output_dir);

    radium::ParseOptions options;
    options.output_root = output_dir;
    const auto summary = radium::parse_radium_file(fixture, options);
    const auto imported = radium::build_import_preset(summary);
    const auto playback = radium::build_playback_preset(imported);
    const auto resolved = radium::resolve_fixture_audio(imported, output_dir);

    require(!resolved.buffers_by_reference.empty(), "no real embedded audio was resolved");

    radium::PlaybackEngine engine(123);
    const auto rendered = engine.render_one_shot(playback, resolved.buffers_by_reference);
    require(rendered.frame_count() > 0, "real fixture render was empty");

    bool any_nonzero = false;
    for (float sample : rendered.samples) {
        if (std::fabs(sample) > 1e-5f) {
            any_nonzero = true;
            break;
        }
    }
    require(any_nonzero, "real fixture render was silent");
}

void test_fixture_mapping_corpus() {
    const auto fixtures_dir = std::filesystem::current_path() / "fixtures" / "radium";
    const auto output_dir = std::filesystem::current_path() / "artifacts" / "fixture_audio_corpus";
    std::filesystem::create_directories(output_dir);

    const auto inputs = radium::collect_inputs(fixtures_dir);
    require(!inputs.empty(), "no fixtures discovered");

    std::size_t mapped_fixture_count = 0;
    for (const auto& fixture : inputs) {
        radium::ParseOptions options;
        options.output_root = output_dir;
        const auto summary = radium::parse_radium_file(fixture, options);
        const auto imported = radium::build_import_preset(summary);
        const auto resolved = radium::resolve_fixture_audio(imported, output_dir);

        if (!resolved.buffers_by_reference.empty() && resolved.unmapped_layer_indices.empty()) {
            mapped_fixture_count += 1;
        }
    }

    require(mapped_fixture_count >= 1, "expected at least one fully mapped fixture with real audio");
}

}  // namespace

int main() {
    try {
        test_flac_decode();
        test_real_fixture_render();
        test_fixture_mapping_corpus();
        std::cout << "fixture_audio_bridge_tests: ok\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "fixture_audio_bridge_tests: " << ex.what() << '\n';
        return 1;
    }
}
