#include "app_controller.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool has_region_start_near(
    const std::vector<radium::LayerWaveformOverview::AuthoredRegion>& regions,
    double min_start,
    double max_start
) {
    for (const auto& region : regions) {
        if (region.start > min_start && region.start < max_start) {
            return true;
        }
    }
    return false;
}

void write_pcm16_wav(
    const std::filesystem::path& path,
    const std::vector<std::int16_t>& samples,
    int sample_rate = 48000,
    int channels = 2
) {
    std::ofstream stream(path, std::ios::binary);
    require(stream.good(), "failed to create test wav");
    const std::uint32_t data_size = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
    const std::uint32_t riff_size = 36 + data_size;

    auto write_u16 = [&](std::uint16_t value) {
        stream.put(static_cast<char>(value & 0xff));
        stream.put(static_cast<char>((value >> 8) & 0xff));
    };
    auto write_u32 = [&](std::uint32_t value) {
        stream.put(static_cast<char>(value & 0xff));
        stream.put(static_cast<char>((value >> 8) & 0xff));
        stream.put(static_cast<char>((value >> 16) & 0xff));
        stream.put(static_cast<char>((value >> 24) & 0xff));
    };

    stream.write("RIFF", 4);
    write_u32(riff_size);
    stream.write("WAVE", 4);
    stream.write("fmt ", 4);
    write_u32(16);
    write_u16(1);
    write_u16(static_cast<std::uint16_t>(channels));
    write_u32(static_cast<std::uint32_t>(sample_rate));
    write_u32(static_cast<std::uint32_t>(sample_rate * channels * 2));
    write_u16(static_cast<std::uint16_t>(channels * 2));
    write_u16(16);
    stream.write("data", 4);
    write_u32(data_size);
    stream.write(reinterpret_cast<const char*>(samples.data()), static_cast<std::streamsize>(data_size));
}

std::vector<char> read_file_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

void test_controller_fixture_smoke() {
    const auto workdir = std::filesystem::current_path() / "artifacts" / "controller_test";
    radium::AppController controller(workdir);
    const auto fixture = std::filesystem::current_path() / "fixtures" / "radium" / "NJR LARGE FACTORY POWER DOWN.radium";

    require(controller.import_file(fixture), "controller import failed");
    require(controller.has_imported_preset(), "controller did not retain imported preset");
    require(controller.preset_summary_text().find("NJR LARGE FACTORY POWER DOWN") != std::string::npos ||
            controller.preset_summary_text().find("Test Preset") != std::string::npos,
            "preset summary text looked wrong");

    controller.set_trigger_mode(radium::TriggerMode::kOneShot);
    auto trigger = controller.trigger_note_on(60);
    require(trigger.success, "controller one-shot trigger failed");
    require(std::filesystem::exists(trigger.audio_path), "one-shot trigger did not write temp wav");

    const auto export_path = workdir / "export.wav";
    std::string error;
    require(controller.export_one_shot_wav(export_path, &error), "controller export failed: " + error);
    require(std::filesystem::exists(export_path), "controller export file missing");

    const auto record_path = workdir / "record.wav";
    require(controller.record_last_trigger_to_wav(record_path, &error), "controller record-last failed: " + error);
    require(std::filesystem::exists(record_path), "controller record file missing");

    const auto selected = controller.selected_layer_index();
    require(selected.has_value(), "controller did not select a mapped layer by default");
    const auto waveform = controller.layer_waveform(*selected, 64);
    require(waveform.has_value(), "controller did not return waveform metadata");
    require(waveform->available, "waveform metadata was unavailable for selected layer");
    require(waveform->peaks.size() == 64, "waveform bucket count was wrong");

    require(controller.set_layer_audition_start(*selected, 0.25), "failed to set layer audition start");
    require(controller.clear_layer_audition_loop(*selected), "failed to clear layer audition loop");
    error.clear();
    require(controller.start_streaming_audition(*selected, &error), "isolated layer audition failed: " + error);

    require(controller.set_layer_audition_loop(*selected, 0.10, 0.20), "failed to set layer audition loop");
    const auto looped_waveform = controller.layer_waveform(*selected, 32);
    require(looped_waveform.has_value() && looped_waveform->loop_start.has_value(), "loop selection was not reflected in waveform metadata");
    require(controller.start_streaming_audition(*selected, &error), "looped isolated layer audition failed: " + error);
}

void test_controller_project_authoring() {
    const auto workdir = std::filesystem::current_path() / "artifacts" / "controller_project_test";
    std::filesystem::create_directories(workdir);
    radium::AppController controller(workdir);

    require(controller.new_empty_project("Authoring Test"), "failed to create empty project");
    std::string error;
    const auto source = std::filesystem::current_path() / "fixtures" / "radium" / "LASER 1.wav";
    require(controller.add_audio_file_to_layer(0, source, &error), "failed to add local audio: " + error);
    require(controller.add_audio_file_to_layer(0, source, &error), "failed to add second local audio: " + error);
    auto effects = controller.layer_effect_state(0);
    require(effects.has_value(), "missing layer effect state");
    effects->reverse = true;
    effects->pitch_shift_semitones = 3.0;
    effects->time_stretch_ratio = 1.5;
    require(controller.set_layer_effect_state(0, *effects), "failed to set layer effects");

    const auto layers = controller.visible_layers(5);
    require(!layers.empty(), "project layers were unavailable");
    require(layers[0].has_audio, "authored layer did not report audio");
    require(layers[0].source_count == 2, "authored layer did not retain multiple sources");

    auto trigger = controller.trigger_note_on(60);
    require(trigger.success, "authored project trigger failed");
    require(std::filesystem::exists(trigger.audio_path), "authored project temp wav missing");
    require(controller.last_rendered_audio().has_value(), "missing last rendered audio after authored trigger");
    require(controller.commit_session_recording(*controller.last_rendered_audio(), "authoring_take", std::nullopt, &error).has_value(),
            "failed to commit session recording: " + error);
    require(!controller.session_recordings().empty(), "session recording list was empty after commit");

    radium::AppController::LayerOverride::Vst3PluginState plugin_state;
    plugin_state.module_path = "C:/Program Files/Common Files/VST3/WaveShell1-VST3 16.0_x64.vst3";
    plugin_state.class_id = "5653544f-4b50-706d-7065722d76616c75";
    plugin_state.display_name = "OneKnob Pumper";
    plugin_state.component_state = {0x01, 0x02, 0x03};
    plugin_state.controller_state = {0xaa, 0xbb};
    require(controller.set_layer_vst3_plugin(0, plugin_state), "failed to assign test vst3 plugin");

    const auto project_path = workdir / "authoring_test.riproj";
    require(controller.save_project(project_path, &error), "failed to save project: " + error);
    require(std::filesystem::exists(project_path), "project file missing after save");

    radium::AppController reloaded(workdir / "reloaded");
    require(reloaded.load_project(project_path, &error), "failed to reload project: " + error);
    const auto reloaded_layers = reloaded.visible_layers(5);
    require(!reloaded_layers.empty(), "reloaded layers were unavailable");
    require(reloaded_layers[0].has_audio, "reloaded layer did not restore audio");
    require(reloaded_layers[0].source_count == 2, "reloaded layer did not restore multiple sources");
    const auto reloaded_effects = reloaded.layer_effect_state(0);
    require(reloaded_effects.has_value(), "reloaded layer effects missing");
    require(reloaded_effects->reverse, "reloaded reverse effect did not persist");
    const auto reloaded_plugin = reloaded.layer_vst3_plugin(0);
    require(reloaded_plugin.has_value(), "reloaded vst3 plugin assignment missing");
    require(reloaded_plugin->display_name == "OneKnob Pumper", "reloaded vst3 plugin name did not persist");
    require(reloaded_plugin->component_state == plugin_state.component_state, "reloaded vst3 component state did not persist");
    require(reloaded_plugin->controller_state == plugin_state.controller_state, "reloaded vst3 controller state did not persist");
    require(!reloaded.session_recordings().empty(), "reloaded session recordings were missing");
}

void test_controller_authored_regions() {
    const auto workdir = std::filesystem::current_path() / "artifacts" / "controller_region_test";
    std::filesystem::create_directories(workdir);
    const auto wav_path = workdir / "two_hits.wav";

    std::vector<std::int16_t> samples;
    samples.reserve(48000 * 2);
    for (int i = 0; i < 12000; ++i) {
        const std::int16_t value = (i < 4000) ? 12000 : 0;
        samples.push_back(value);
        samples.push_back(value);
    }
    for (int i = 0; i < 12000; ++i) {
        const std::int16_t value = (i >= 2000 && i < 6000) ? -12000 : 0;
        samples.push_back(value);
        samples.push_back(value);
    }
    write_pcm16_wav(wav_path, samples);

    radium::AppController controller(workdir);
    controller.new_empty_project("Region Test");
    std::string error;
    require(controller.add_audio_file_to_layer(0, wav_path, &error), "failed to add authored region wav: " + error);
    require(controller.clear_layer_trigger_regions(0), "failed to clear default regions");
    require(controller.add_layer_trigger_region(0, 0.0, 0.45), "failed to add first region");
    require(controller.add_layer_trigger_region(0, 0.50, 1.0), "failed to add second region");
    require(controller.update_layer_trigger_region(0, 0, 0.05, 0.40), "failed to resize first region");

    const auto waveform = controller.layer_waveform(0, 64);
    require(waveform.has_value(), "missing waveform overview for authored regions");
    require(waveform->authored_regions.size() == 2, "authored region count was wrong");
    require(waveform->authored_regions.front().start >= 0.049 && waveform->authored_regions.front().start <= 0.051,
            "resized region start was wrong");

    const auto zoomed_waveform = controller.layer_waveform(0, 64, 0.0, 0.5);
    require(zoomed_waveform.has_value(), "missing zoomed waveform overview");
    require(zoomed_waveform->peaks.size() == 64, "zoomed waveform bucket count was wrong");

    const auto first = controller.trigger_note_on(60);
    const auto first_bytes = read_file_bytes(first.audio_path);
    const auto second = controller.trigger_note_on(60);
    require(first.success && second.success, "authored region trigger failed");
    require(first_bytes != read_file_bytes(second.audio_path), "region selection did not vary across triggers");
}

void test_controller_auto_split_regions() {
    const auto workdir = std::filesystem::current_path() / "artifacts" / "controller_auto_split_test";
    std::filesystem::create_directories(workdir);
    const auto wav_path = workdir / "split_hits.wav";

    std::vector<std::int16_t> samples;
    auto append_frames = [&](int frames, std::int16_t value) {
        for (int i = 0; i < frames; ++i) {
            samples.push_back(value);
            samples.push_back(value);
        }
    };
    append_frames(3000, 0);
    append_frames(2500, 14000);
    append_frames(3000, 0);
    append_frames(2500, -12000);
    append_frames(3000, 0);
    append_frames(2500, 10000);
    append_frames(3000, 0);
    write_pcm16_wav(wav_path, samples);

    radium::AppController controller(workdir);
    controller.new_empty_project("Auto Split Test");
    std::string error;
    require(controller.add_audio_file_to_layer(0, wav_path, &error), "failed to add auto-split wav: " + error);
    require(controller.auto_split_layer_regions(0, &error), "auto-split failed: " + error);

    const auto waveform = controller.layer_waveform(0, 64);
    require(waveform.has_value(), "missing waveform for auto-split layer");
    require(!waveform->authored_regions.empty(), "auto-split created no regions");
    for (std::size_t i = 1; i < waveform->authored_regions.size(); ++i) {
        require(waveform->authored_regions[i - 1].start < waveform->authored_regions[i].start,
                "auto-split regions were not ordered");
    }
    require(has_region_start_near(waveform->authored_regions, 0.05, 0.86),
            "auto-split did not find any plausible region start");
}

void test_controller_auto_split_soft_attacks() {
    const auto workdir = std::filesystem::current_path() / "artifacts" / "controller_auto_split_soft_test";
    std::filesystem::create_directories(workdir);
    const auto wav_path = workdir / "soft_hits.wav";

    std::vector<std::int16_t> samples;
    auto append_stereo = [&](std::int16_t value) {
        samples.push_back(value);
        samples.push_back(value);
    };
    auto append_silence = [&](int frames) {
        for (int i = 0; i < frames; ++i) {
            append_stereo(0);
        }
    };
    auto append_ramped_hit = [&](int ramp_frames, int hold_frames, std::int16_t peak_value) {
        for (int i = 0; i < ramp_frames; ++i) {
            const double t = static_cast<double>(i + 1) / static_cast<double>(ramp_frames);
            append_stereo(static_cast<std::int16_t>(peak_value * t));
        }
        for (int i = 0; i < hold_frames; ++i) {
            append_stereo(peak_value);
        }
        for (int i = 0; i < ramp_frames; ++i) {
            const double t = static_cast<double>(ramp_frames - i - 1) / static_cast<double>(ramp_frames);
            append_stereo(static_cast<std::int16_t>(peak_value * t));
        }
    };

    append_silence(3200);
    append_ramped_hit(900, 1600, 9000);
    append_silence(2600);
    append_ramped_hit(700, 1400, -11000);
    append_silence(2600);
    append_ramped_hit(1100, 1500, 8000);
    append_silence(3000);
    write_pcm16_wav(wav_path, samples);

    radium::AppController controller(workdir);
    controller.new_empty_project("Auto Split Soft Test");
    std::string error;
    require(controller.add_audio_file_to_layer(0, wav_path, &error), "failed to add soft auto-split wav: " + error);
    require(controller.auto_split_layer_regions(0, &error), "soft auto-split failed: " + error);

    const auto waveform = controller.layer_waveform(0, 64);
    require(waveform.has_value(), "missing waveform for soft auto-split layer");
    require(!waveform->authored_regions.empty(), "soft auto-split created no regions");
    for (std::size_t i = 1; i < waveform->authored_regions.size(); ++i) {
        require(waveform->authored_regions[i - 1].start < waveform->authored_regions[i].start,
                "soft auto-split regions were not ordered");
    }
    require(has_region_start_near(waveform->authored_regions, 0.10, 0.84),
            "soft auto-split did not find any plausible region start");
}

}  // namespace

int main() {
    try {
        test_controller_fixture_smoke();
        test_controller_project_authoring();
        test_controller_authored_regions();
        test_controller_auto_split_regions();
        test_controller_auto_split_soft_attacks();
        std::cout << "app_controller_tests: ok\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "app_controller_tests: " << ex.what() << '\n';
        return 1;
    }
}
