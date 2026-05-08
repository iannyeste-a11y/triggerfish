#include "import_to_playback.h"
#include "playback_engine.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unordered_map>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

radium::AudioBuffer make_impulse(std::size_t frame_count, std::size_t impulse_frame) {
    radium::AudioBuffer buffer;
    buffer.sample_rate = 48000;
    buffer.channels = 1;
    buffer.samples.assign(frame_count, 0.0f);
    if (impulse_frame < frame_count) {
        buffer.samples[impulse_frame] = 1.0f;
    }
    return buffer;
}

radium::AudioBuffer make_tone(std::size_t frame_count, double frequency, double amplitude = 0.5) {
    radium::AudioBuffer buffer;
    buffer.sample_rate = 48000;
    buffer.channels = 1;
    buffer.samples.resize(frame_count);
    for (std::size_t i = 0; i < frame_count; ++i) {
        buffer.samples[i] = static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * frequency * i / 48000.0) * amplitude);
    }
    return buffer;
}

radium::AudioBuffer make_stereo_lr_impulses(std::size_t frame_count, std::size_t left_frame, std::size_t right_frame) {
    radium::AudioBuffer buffer;
    buffer.sample_rate = 48000;
    buffer.channels = 2;
    buffer.samples.assign(frame_count * 2, 0.0f);
    if (left_frame < frame_count) {
        buffer.samples[left_frame * 2] = 1.0f;
    }
    if (right_frame < frame_count) {
        buffer.samples[right_frame * 2 + 1] = 1.0f;
    }
    return buffer;
}

float max_abs(const radium::RenderedAudio& audio, std::size_t frame) {
    const auto left = std::fabs(audio.samples[frame * 2]);
    const auto right = std::fabs(audio.samples[frame * 2 + 1]);
    return std::max(left, right);
}

void test_one_shot_sync() {
    radium::PlaybackPreset preset;
    preset.output_sample_rate = 48000;

    radium::PlaybackLayer layer_a;
    layer_a.index = 0;
    layer_a.sources.push_back({"a", {radium::PlaybackRegion{}}});

    radium::PlaybackLayer layer_b;
    layer_b.index = 1;
    layer_b.sources.push_back({"b", {radium::PlaybackRegion{}}});

    preset.layers = {layer_a, layer_b};

    std::unordered_map<std::string, radium::AudioBuffer> buffers;
    buffers.emplace("a", make_impulse(64, 0));
    buffers.emplace("b", make_impulse(64, 0));

    radium::PlaybackEngine engine(7);
    const auto audio = engine.render_one_shot(preset, buffers);
    require(audio.frame_count() >= 1, "one-shot render was empty");
    require(max_abs(audio, 0) > 0.9f, "expected synced impulse at trigger boundary");
}

void test_continuous_release() {
    radium::PlaybackPreset preset;
    preset.output_sample_rate = 48000;

    radium::PlaybackLayer layer;
    layer.index = 0;
    layer.envelope.attack_seconds = 0.0;
    layer.envelope.decay_seconds = 0.0;
    layer.envelope.sustain_level = 1.0;
    layer.envelope.release_seconds = 0.01;
    layer.sources.push_back({
        "loop",
        {radium::PlaybackRegion{0.0, 1.0, 0.1, 0.9, true}}
    });

    preset.layers.push_back(layer);

    std::unordered_map<std::string, radium::AudioBuffer> buffers;
    buffers.emplace("loop", make_tone(4800, 220.0, 0.75));

    radium::PlaybackEngine engine(11);
    const auto audio = engine.render_continuous(preset, buffers, 480);
    require(audio.frame_count() > 480, "continuous render did not include release tail");
    require(max_abs(audio, 100) > 0.1f, "continuous render did not sustain while held");
    require(max_abs(audio, audio.frame_count() - 1) < 0.01f, "release tail did not decay toward silence");
}

void test_no_immediate_repeat() {
    radium::PlaybackPreset preset;
    preset.output_sample_rate = 48000;

    radium::PlaybackLayer layer;
    layer.index = 0;
    layer.no_immediate_repeat = true;
    layer.sources.push_back({"first", {radium::PlaybackRegion{}}});
    layer.sources.push_back({"second", {radium::PlaybackRegion{}}});
    preset.layers.push_back(layer);

    std::unordered_map<std::string, radium::AudioBuffer> buffers;
    buffers.emplace("first", make_impulse(32, 0));
    buffers.emplace("second", make_impulse(32, 4));

    radium::PlaybackEngine engine(1);
    const auto a = engine.render_one_shot(preset, buffers);
    const auto b = engine.render_one_shot(preset, buffers);
    require(a.samples != b.samples, "no-immediate-repeat did not vary source selection across triggers");
}

void test_wav_export() {
    radium::RenderedAudio audio;
    audio.sample_rate = 48000;
    audio.channels = 2;
    audio.samples = {0.0f, 0.0f, 0.5f, -0.5f, 0.25f, -0.25f};

    const auto path = std::filesystem::current_path() / "artifacts" / "playback_test.wav";
    std::filesystem::create_directories(path.parent_path());
    radium::PlaybackEngine::write_wav_24(path, audio);

    std::ifstream stream(path, std::ios::binary);
    require(stream.good(), "failed to read rendered wav");
    char header[44]{};
    stream.read(header, sizeof(header));
    require(std::string(header, header + 4) == "RIFF", "wav header missing RIFF");
    require(std::string(header + 8, header + 12) == "WAVE", "wav header missing WAVE");
    require(static_cast<unsigned char>(header[34]) == 24, "wav export was not 24-bit");
}

void test_delay_and_limiter_fx() {
    radium::PlaybackPreset preset;
    preset.output_sample_rate = 48000;

    radium::PlaybackLayer layer;
    layer.index = 0;
    layer.sources.push_back({"impulse", {radium::PlaybackRegion{}}});
    layer.effects.delay.enabled = true;
    layer.effects.delay.time_seconds = 0.01;
    layer.effects.delay.feedback = 0.5;
    layer.effects.delay.mix = 0.5;
    layer.effects.limiter.enabled = true;
    layer.effects.limiter.ceiling = 0.4;
    preset.layers.push_back(layer);

    std::unordered_map<std::string, radium::AudioBuffer> buffers;
    buffers.emplace("impulse", make_impulse(256, 0));

    radium::PlaybackEngine engine(3);
    const auto audio = engine.render_one_shot(preset, buffers);
    require(audio.frame_count() > 480, "delay fx did not extend render");
    bool found_echo = false;
    float peak = 0.0f;
    for (std::size_t frame = 0; frame < audio.frame_count(); ++frame) {
        peak = std::max(peak, max_abs(audio, frame));
        if (frame >= 470 && frame <= 490 && max_abs(audio, frame) > 0.05f) {
            found_echo = true;
        }
    }
    require(found_echo, "delay fx did not produce an audible echo");
    require(peak <= 0.401f, "limiter fx did not clamp the output peak");
}

void test_time_stretch_fx() {
    radium::PlaybackPreset preset;
    preset.output_sample_rate = 48000;

    radium::PlaybackLayer layer;
    layer.index = 0;
    layer.sources.push_back({"tone", {radium::PlaybackRegion{}}});
    layer.effects.time_stretch_ratio = 2.0;
    preset.layers.push_back(layer);

    std::unordered_map<std::string, radium::AudioBuffer> buffers;
    buffers.emplace("tone", make_tone(2400, 220.0, 0.3));

    radium::PlaybackEngine engine(5);
    const auto stretched = engine.render_one_shot(preset, buffers);

    layer.effects.time_stretch_ratio = 1.0;
    preset.layers[0] = layer;
    const auto normal = engine.render_one_shot(preset, buffers);
    require(stretched.frame_count() > normal.frame_count(), "time stretch did not lengthen the render");
}

void test_stereo_source_is_not_folded_to_mono() {
    radium::PlaybackPreset preset;
    preset.output_sample_rate = 48000;

    radium::PlaybackLayer layer;
    layer.index = 0;
    layer.pan_x = -1.0;
    layer.pan_y = 1.0;
    layer.pan_x_right = 1.0;
    layer.pan_y_right = 1.0;
    layer.sources.push_back({"stereo", {radium::PlaybackRegion{}}});
    preset.layers.push_back(layer);

    std::unordered_map<std::string, radium::AudioBuffer> buffers;
    buffers.emplace("stereo", make_stereo_lr_impulses(64, 2, 10));

    radium::PlaybackEngine engine(17);
    const auto audio = engine.render_one_shot(preset, buffers);
    require(audio.frame_count() >= 11, "stereo preservation render was too short");
    require(std::fabs(audio.samples[2 * 2]) > 0.6f, "left channel impulse was missing");
    require(std::fabs(audio.samples[2 * 2 + 1]) < 0.01f, "left channel impulse leaked into right output");
    require(std::fabs(audio.samples[10 * 2]) < 0.01f, "right channel impulse leaked into left output");
    require(std::fabs(audio.samples[10 * 2 + 1]) > 0.6f, "right channel impulse was missing");
}

void test_fixture_render_smoke() {
    const auto fixture = std::filesystem::current_path() / "fixtures" / "radium" / "NJR BRACHI STEP STARTING POINT.radium";
    const auto output_dir = std::filesystem::current_path() / "artifacts" / "playback_fixture_output";
    std::filesystem::create_directories(output_dir);

    radium::ParseOptions options;
    options.output_root = output_dir;
    const auto summary = radium::parse_radium_file(fixture, options);
    const auto imported = radium::build_import_preset(summary);
    const auto playback = radium::build_playback_preset(imported);

    std::unordered_map<std::string, radium::AudioBuffer> buffers;
    for (const auto& layer : playback.layers) {
        if (!layer.active || layer.sources.empty()) {
            continue;
        }
        buffers.emplace(layer.sources.front().buffer_id, make_tone(2400, 220.0 + 20.0 * layer.index, 0.2));
    }

    radium::PlaybackEngine engine(99);
    const auto audio = engine.render_one_shot(playback, buffers);
    require(audio.frame_count() > 0, "fixture smoke render was empty");
    bool any_nonzero = false;
    for (float sample : audio.samples) {
        if (std::fabs(sample) > 1e-4f) {
            any_nonzero = true;
            break;
        }
    }
    require(any_nonzero, "fixture smoke render was silent");
}

}  // namespace

int main() {
    try {
        test_one_shot_sync();
        test_continuous_release();
        test_no_immediate_repeat();
        test_wav_export();
        test_delay_and_limiter_fx();
        test_time_stretch_fx();
        test_stereo_source_is_not_folded_to_mono();
        test_fixture_render_smoke();
        std::cout << "playback_engine_tests: ok\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "playback_engine_tests: " << ex.what() << '\n';
        return 1;
    }
}
