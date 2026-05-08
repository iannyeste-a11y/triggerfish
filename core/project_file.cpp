#include "project_file.h"

#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>

namespace radium {
namespace {

constexpr const char* kHeaderV1 = "RADIUM_IMPORTER_PROJECT_V1";
constexpr const char* kHeaderV2 = "RADIUM_IMPORTER_PROJECT_V2";
constexpr const char* kHeaderV3 = "RADIUM_IMPORTER_PROJECT_V3";
constexpr const char* kHeaderV4 = "RADIUM_IMPORTER_PROJECT_V4";
constexpr const char* kHeaderV5 = "RADIUM_IMPORTER_PROJECT_V5";
constexpr const char* kHeaderV6 = "RADIUM_IMPORTER_PROJECT_V6";
constexpr const char* kHeaderV7 = "RADIUM_IMPORTER_PROJECT_V7";
constexpr const char* kHeaderV8 = "RADIUM_IMPORTER_PROJECT_V8";
constexpr const char* kHeaderV9 = "RADIUM_IMPORTER_PROJECT_V9";
constexpr const char* kHeaderV10 = "RADIUM_IMPORTER_PROJECT_V10";
constexpr const char* kHeaderV11 = "RADIUM_IMPORTER_PROJECT_V11";
constexpr const char* kHeaderV12 = "RADIUM_IMPORTER_PROJECT_V12";
constexpr const char* kHeaderV13 = "RADIUM_IMPORTER_PROJECT_V13";
constexpr const char* kHeaderV14 = "RADIUM_IMPORTER_PROJECT_V14";
constexpr const char* kHeaderV15 = "RADIUM_IMPORTER_PROJECT_V15";
constexpr const char* kHeaderV16 = "RADIUM_IMPORTER_PROJECT_V16";
constexpr const char* kHeaderV17 = "RADIUM_IMPORTER_PROJECT_V17";
constexpr const char* kHeaderV18 = "RADIUM_IMPORTER_PROJECT_V18";
constexpr const char* kHeaderV19 = "RADIUM_IMPORTER_PROJECT_V19";
constexpr const char* kHeaderV20 = "RADIUM_IMPORTER_PROJECT_V20";
constexpr const char* kHeaderV21 = "RADIUM_IMPORTER_PROJECT_V21";
constexpr const char* kHeaderV22 = "RADIUM_IMPORTER_PROJECT_V22";
constexpr const char* kHeaderV23 = "RADIUM_IMPORTER_PROJECT_V23";
constexpr const char* kHeaderV24 = "RADIUM_IMPORTER_PROJECT_V24";

template <typename T>
void write_optional(std::ostream& out, const std::optional<T>& value) {
    if (value.has_value()) {
        out << 1 << ' ' << *value;
    } else {
        out << 0 << ' ' << 0;
    }
}

void write_optional_string(std::ostream& out, const std::optional<std::string>& value) {
    if (value.has_value()) {
        out << 1 << ' ' << std::quoted(*value);
    } else {
        out << 0 << ' ' << std::quoted(std::string());
    }
}

template <typename T>
bool read_optional(std::istringstream& in, std::optional<T>& value) {
    int present = 0;
    T parsed{};
    if (!(in >> present >> parsed)) {
        return false;
    }
    value = present ? std::optional<T>(parsed) : std::nullopt;
    return true;
}

bool read_optional_string(std::istringstream& in, std::optional<std::string>& value) {
    int present = 0;
    std::string parsed;
    if (!(in >> present >> std::quoted(parsed))) {
        return false;
    }
    value = present ? std::optional<std::string>(parsed) : std::nullopt;
    return true;
}

std::string bytes_to_hex(const std::vector<std::uint8_t>& bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2);
    for (std::uint8_t byte : bytes) {
        text.push_back(kHex[(byte >> 4) & 0x0f]);
        text.push_back(kHex[byte & 0x0f]);
    }
    return text;
}

bool hex_to_bytes(const std::string& text, std::vector<std::uint8_t>* bytes) {
    if (bytes == nullptr || text.size() % 2 != 0) {
        return false;
    }
    auto decode_nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return 10 + (c - 'a');
        }
        if (c >= 'A' && c <= 'F') {
            return 10 + (c - 'A');
        }
        return -1;
    };

    bytes->clear();
    bytes->reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int hi = decode_nibble(text[i]);
        const int lo = decode_nibble(text[i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        bytes->push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return true;
}

}  // namespace

bool save_project_file(
    const std::filesystem::path& output_path,
    const Preset& preset,
    const std::vector<AppController::LayerOverride>& layer_overrides,
    const std::vector<std::optional<AppController::LayerEditState>>& layer_edit_states,
    const AppController::AuxTrackState& aux_track_state,
    RecordBusMode record_bus_mode,
    TriggerMode trigger_mode,
    int octave,
    const std::optional<std::filesystem::path>& project_picture_path,
    const std::filesystem::path& session_recordings_directory,
    const std::vector<SessionRecordingInfo>& session_recordings,
    std::optional<std::size_t> selected_session_recording_index,
    const std::map<std::string, EmbeddedAudioBlob>& embedded_audio,
    std::string* error_message
) {
    std::ofstream out(output_path);
    if (!out) {
        if (error_message != nullptr) {
            *error_message = "Unable to open project file for writing.";
        }
        return false;
    }

    // Always write the V24 header. Files without embedded audio simply have
    // an empty embedded_audio_count line — same on-disk size as V23.
    out << kHeaderV24 << '\n';
    out << "preset_name " << std::quoted(preset.name) << '\n';
    out << "source_file " << std::quoted(preset.source_file.string()) << '\n';
    out << "trigger_mode " << (trigger_mode == TriggerMode::kOneShot ? "one_shot" : "continuous") << '\n';
    out << "octave " << octave << '\n';
    out << "project_picture_path ";
    if (project_picture_path.has_value()) {
        out << 1 << ' ' << std::quoted(project_picture_path->string());
    } else {
        out << 0 << ' ' << std::quoted(std::string());
    }
    out << '\n';
    out << "session_recordings_directory " << std::quoted(session_recordings_directory.string()) << '\n';
    out << "session_recordings_selected "
        << (selected_session_recording_index.has_value() ? 1 : 0) << ' '
        << (selected_session_recording_index.has_value() ? *selected_session_recording_index : 0) << '\n';
    out << "session_recordings_count " << session_recordings.size() << '\n';
    for (const auto& recording : session_recordings) {
        out << "session_recording " << std::quoted(recording.name) << ' '
            << std::quoted(recording.path.string()) << ' ';
        write_optional(out, recording.picture_start_seconds);
        out << ' ' << recording.channels;
        out << '\n';
    }
    out << "aux_gain " << aux_track_state.gain << '\n';
    out << "aux_bass_gain_db " << aux_track_state.bass_gain_db << '\n';
    out << "record_bus_surround_enabled " << (record_bus_mode != RecordBusMode::Stereo ? 1 : 0) << '\n';
    out << "record_bus_channel_count " << static_cast<int>(record_bus_mode) << '\n';
    for (std::size_t slot_index = 0; slot_index < AppController::kPluginInsertSlotCount; ++slot_index) {
        out << "aux_vst3_slot " << slot_index << ' ';
        if (aux_track_state.vst3_plugins[slot_index].has_value()) {
            out << 1 << ' '
                << std::quoted(aux_track_state.vst3_plugins[slot_index]->module_path) << ' '
                << std::quoted(aux_track_state.vst3_plugins[slot_index]->class_id) << ' '
                << std::quoted(aux_track_state.vst3_plugins[slot_index]->display_name) << ' '
                << std::quoted(bytes_to_hex(aux_track_state.vst3_plugins[slot_index]->component_state)) << ' '
                << std::quoted(bytes_to_hex(aux_track_state.vst3_plugins[slot_index]->controller_state)) << ' '
                << (aux_track_state.vst3_plugins[slot_index]->bypassed ? 1 : 0);
        } else {
            out << 0 << ' '
                << std::quoted(std::string()) << ' '
                << std::quoted(std::string()) << ' '
                << std::quoted(std::string()) << ' '
                << std::quoted(std::string()) << ' '
                << std::quoted(std::string()) << ' '
                << 0;
        }
        out << '\n';
    }
    out << "layer_count " << preset.layers.size() << '\n';

    for (std::size_t i = 0; i < preset.layers.size(); ++i) {
        const auto& layer = preset.layers[i];
        const auto& override_state = layer_overrides[i];
        out << "layer "
            << layer.index << ' ' << layer.active << ' ' << layer.mute << ' ' << layer.solo << ' '
            << override_state.mute << ' ' << override_state.solo << ' ' << override_state.gain << ' ';
        write_optional(out, layer.gain);
        out << ' ';
        write_optional(out, layer.delay);
        out << ' ';
        write_optional(out, layer.start_offset);
        out << ' ';
        write_optional(out, layer.stop_offset);
        out << ' ';
        out << layer.reverse << '\n';

        out << "layer_name ";
        write_optional_string(out, layer.custom_name);
        out << '\n';
        out << "layer_embedded_media ";
        write_optional_string(out, layer.embedded_media_reference);
        out << ' ';
        write_optional_string(out, layer.embedded_media_path);
        out << '\n';

        out << "override_audition " << override_state.audition_start << ' ';
        write_optional(out, override_state.audition_loop_start);
        out << ' ';
        write_optional(out, override_state.audition_loop_end);
        out << '\n';
        out << "override_pan "
            << override_state.pan_x << ' '
            << override_state.pan_y << ' '
            << override_state.pan_x_right << ' '
            << override_state.pan_y_right << '\n';
        out << "override_locked " << (override_state.locked ? 1 : 0) << '\n';
        out << "override_fx "
            << override_state.effects.reverse << ' '
            << override_state.effects.pitch_shift_semitones << ' '
            << override_state.effects.time_stretch_ratio << ' '
            << override_state.effects.bass_lfe_gain_db << ' '
            << override_state.effects.eq_low_gain_db << ' '
            << override_state.effects.eq_mid_gain_db << ' '
            << override_state.effects.eq_high_gain_db << '\n';
        for (std::size_t slot_index = 0; slot_index < AppController::kPluginInsertSlotCount; ++slot_index) {
            out << "override_vst3_slot " << slot_index << ' ';
            if (override_state.vst3_plugins[slot_index].has_value()) {
                out << 1 << ' '
                    << std::quoted(override_state.vst3_plugins[slot_index]->module_path) << ' '
                    << std::quoted(override_state.vst3_plugins[slot_index]->class_id) << ' '
                    << std::quoted(override_state.vst3_plugins[slot_index]->display_name) << ' '
                    << std::quoted(bytes_to_hex(override_state.vst3_plugins[slot_index]->component_state)) << ' '
                    << std::quoted(bytes_to_hex(override_state.vst3_plugins[slot_index]->controller_state)) << ' '
                    << (override_state.vst3_plugins[slot_index]->bypassed ? 1 : 0);
            } else {
                out << 0 << ' '
                    << std::quoted(std::string()) << ' '
                    << std::quoted(std::string()) << ' '
                    << std::quoted(std::string()) << ' '
                    << std::quoted(std::string()) << ' '
                    << std::quoted(std::string()) << ' '
                    << 0;
            }
            out << '\n';
        }
        out << "override_trigger_regions " << override_state.trigger_regions.size() << '\n';
        for (const auto& region : override_state.trigger_regions) {
            out << "override_trigger_region " << region.start << ' ' << region.end << '\n';
        }

        out << "source_count " << layer.sources.size() << '\n';
        for (const auto& source : layer.sources) {
            out << "source ";
            write_optional_string(out, source.name);
            out << ' ';
            write_optional_string(out, source.path);
            out << ' ';
            write_optional_string(out, source.file);
            out << ' ';
            write_optional_string(out, source.buffer_id);
            out << ' ' << source.embedded << ' ' << source.regions.size() << '\n';
            for (const auto& region : source.regions) {
                out << "region ";
                write_optional(out, region.start_offset);
                out << ' ';
                write_optional(out, region.end_offset);
                out << ' ';
                write_optional(out, region.in_point);
                out << ' ';
                write_optional(out, region.out_point);
                out << ' ';
                write_optional(out, region.loop_start);
                out << ' ';
                write_optional(out, region.loop_end);
                out << ' ';
                write_optional(out, region.loop_crossfade);
                out << ' ' << region.loop_enabled << '\n';
            }
        }

        if (i < layer_edit_states.size() && layer_edit_states[i].has_value()) {
            const auto& edit_state = *layer_edit_states[i];
            out << "layer_edit " << edit_state.head_padding_seconds << ' '
                << edit_state.tail_padding_seconds << ' '
                << edit_state.clips.size() << '\n';
            out << "layer_edit_volume_automation "
                << (edit_state.volume_automation_enabled ? 1 : 0) << ' '
                << edit_state.volume_automation_points.size() << '\n';
            out << "layer_edit_volume_random_settings "
                << (edit_state.volume_random_settings.enabled ? 1 : 0) << ' '
                << edit_state.volume_random_settings.loudest_db << ' '
                << edit_state.volume_random_settings.quietest_db << ' '
                << edit_state.volume_random_settings.period_longest_seconds << ' '
                << edit_state.volume_random_settings.period_shortest_seconds << ' '
                << edit_state.volume_random_settings.smoothing << '\n';
            out << "layer_edit_pan_random_settings "
                << (edit_state.pan_random_settings.enabled ? 1 : 0) << ' '
                << edit_state.pan_random_settings.farthest_left << ' '
                << edit_state.pan_random_settings.farthest_right << ' '
                << edit_state.pan_random_settings.farthest_front << ' '
                << edit_state.pan_random_settings.farthest_back << ' '
                << edit_state.pan_random_settings.speed << ' '
                << edit_state.pan_random_settings.smoothing << '\n';
            out << "layer_edit_stretch_random_settings "
                << (edit_state.stretch_random_settings.enabled ? 1 : 0) << ' '
                << edit_state.stretch_random_settings.lowest_percent << ' '
                << edit_state.stretch_random_settings.highest_percent << ' '
                << edit_state.stretch_random_settings.speed << ' '
                << edit_state.stretch_random_settings.smoothing << '\n';
            for (const auto& point : edit_state.volume_automation_points) {
                out << "layer_edit_volume_point "
                    << point.point_id << ' '
                    << point.timeline_seconds << ' '
                    << point.gain << '\n';
            }
            out << "layer_edit_stretch_automation "
                << (edit_state.stretch_automation_enabled ? 1 : 0) << ' '
                << edit_state.stretch_automation_points.size() << '\n';
            for (const auto& point : edit_state.stretch_automation_points) {
                out << "layer_edit_stretch_point "
                    << point.point_id << ' '
                    << point.timeline_seconds << ' '
                    << point.ratio << '\n';
            }
            out << "layer_edit_pan_position_automation "
                << (edit_state.pan_position_automation_enabled ? 1 : 0) << ' '
                << edit_state.pan_position_automation_points.size() << '\n';
            for (const auto& point : edit_state.pan_position_automation_points) {
                out << "layer_edit_pan_position_point "
                    << point.point_id << ' '
                    << point.timeline_seconds << ' '
                    << point.value << '\n';
            }
            out << "layer_edit_pan_front_back_automation "
                << (edit_state.pan_front_back_automation_enabled ? 1 : 0) << ' '
                << edit_state.pan_front_back_automation_points.size() << '\n';
            for (const auto& point : edit_state.pan_front_back_automation_points) {
                out << "layer_edit_pan_front_back_point "
                    << point.point_id << ' '
                    << point.timeline_seconds << ' '
                    << point.value << '\n';
            }
            out << "layer_edit_pan_right_position_automation "
                << (edit_state.pan_right_position_automation_enabled ? 1 : 0) << ' '
                << edit_state.pan_right_position_automation_points.size() << '\n';
            for (const auto& point : edit_state.pan_right_position_automation_points) {
                out << "layer_edit_pan_right_position_point "
                    << point.point_id << ' '
                    << point.timeline_seconds << ' '
                    << point.value << '\n';
            }
            out << "layer_edit_pan_right_front_back_automation "
                << (edit_state.pan_right_front_back_automation_enabled ? 1 : 0) << ' '
                << edit_state.pan_right_front_back_automation_points.size() << '\n';
            for (const auto& point : edit_state.pan_right_front_back_automation_points) {
                out << "layer_edit_pan_right_front_back_point "
                    << point.point_id << ' '
                    << point.timeline_seconds << ' '
                    << point.value << '\n';
            }
            out << "layer_edit_doppler_automation "
                << (edit_state.doppler_automation_enabled ? 1 : 0) << ' '
                << edit_state.doppler_automation_points.size() << '\n';
            out << "layer_edit_doppler_settings "
                << edit_state.doppler_settings.edge_gain_db << ' '
                << edit_state.doppler_settings.center_gain_db << ' '
                << edit_state.doppler_settings.edge_pitch_semitones << ' '
                << edit_state.doppler_settings.center_pitch_semitones << '\n';
            for (const auto& point : edit_state.doppler_automation_points) {
                out << "layer_edit_doppler_point "
                    << point.point_id << ' '
                    << point.timeline_seconds << ' '
                    << point.value << '\n';
            }
            out << "layer_edit_doppler_segment_shapes "
                << edit_state.doppler_segment_shapes.size() << '\n';
            for (const auto& shape : edit_state.doppler_segment_shapes) {
                out << "layer_edit_doppler_segment_shape "
                    << shape.left_point_id << ' '
                    << static_cast<int>(shape.curve_type) << ' '
                    << shape.curve_amount << '\n';
            }
            for (const auto& clip : edit_state.clips) {
                out << "layer_edit_clip "
                    << std::quoted(clip.source_buffer_id) << ' ';
                write_optional_string(out, clip.source_path);
                out << ' ' << std::quoted(clip.source_label) << ' '
                    << clip.source_start_seconds << ' '
                    << clip.source_end_seconds << ' '
                    << clip.timeline_start_seconds << ' '
                    << clip.fade_in_seconds << ' '
                    << clip.fade_out_seconds << '\n';
            }
        } else {
            out << "layer_edit " << AppController::kLayerEditHeadPaddingSeconds << ' '
                << AppController::kLayerEditTailPaddingSeconds << ' ' << 0 << '\n';
            out << "layer_edit_volume_automation 0 0\n";
            out << "layer_edit_volume_random_settings 0 0 -12 2 0.35 0.7\n";
            out << "layer_edit_pan_random_settings 0 -1 1 1 -1 0.5 0.7\n";
            out << "layer_edit_stretch_random_settings 0 100 100 0.5 0.7\n";
            out << "layer_edit_stretch_automation 0 0\n";
            out << "layer_edit_pan_position_automation 0 0\n";
            out << "layer_edit_pan_front_back_automation 0 0\n";
            out << "layer_edit_pan_right_position_automation 0 0\n";
            out << "layer_edit_pan_right_front_back_automation 0 0\n";
            out << "layer_edit_doppler_automation 0 0\n";
            out << "layer_edit_doppler_settings -24 0 -4 4\n";
            out << "layer_edit_doppler_segment_shapes 0\n";
        }
    }

    // V24: optional embedded audio section. The count is always written so
    // readers can rely on its presence; a count of 0 means "no embedded audio".
    out << "embedded_audio_count " << embedded_audio.size() << '\n';
    for (const auto& [buffer_id, blob] : embedded_audio) {
        out << "embedded_audio "
            << std::quoted(buffer_id) << ' '
            << blob.sample_rate << ' '
            << blob.channels << ' '
            << std::quoted(bytes_to_hex(blob.bytes)) << '\n';
    }

    return true;
}

bool load_project_file(
    const std::filesystem::path& input_path,
    Preset& preset,
    std::vector<AppController::LayerOverride>& layer_overrides,
    std::vector<std::optional<AppController::LayerEditState>>& layer_edit_states,
    AppController::AuxTrackState& aux_track_state,
    RecordBusMode& record_bus_mode,
    TriggerMode& trigger_mode,
    int& octave,
    std::optional<std::filesystem::path>& project_picture_path,
    std::filesystem::path& session_recordings_directory,
    std::vector<SessionRecordingInfo>& session_recordings,
    std::optional<std::size_t>& selected_session_recording_index,
    std::map<std::string, EmbeddedAudioBlob>& embedded_audio,
    std::string* error_message
) {
    embedded_audio.clear();
    std::ifstream in(input_path);
    if (!in) {
        if (error_message != nullptr) {
            *error_message = "Unable to open project file.";
        }
        return false;
    }

    std::string line;
    if (!std::getline(in, line) || (line != kHeaderV1 && line != kHeaderV2 && line != kHeaderV3 && line != kHeaderV4 && line != kHeaderV5 && line != kHeaderV6 && line != kHeaderV7 && line != kHeaderV8 && line != kHeaderV9 && line != kHeaderV10 && line != kHeaderV11 && line != kHeaderV12 && line != kHeaderV13 && line != kHeaderV14 && line != kHeaderV15 && line != kHeaderV16 && line != kHeaderV17 && line != kHeaderV18 && line != kHeaderV19 && line != kHeaderV20 && line != kHeaderV21 && line != kHeaderV22 && line != kHeaderV23 && line != kHeaderV24)) {
        if (error_message != nullptr) {
            *error_message = "Project file header was invalid.";
        }
        return false;
    }
    const std::string header = line;

    preset = Preset{};
    preset.source_file = input_path;
    layer_overrides.clear();
    layer_edit_states.clear();
    aux_track_state = AppController::AuxTrackState{};
    record_bus_mode = RecordBusMode::Stereo;
    project_picture_path.reset();
    session_recordings_directory.clear();
    session_recordings.clear();
    selected_session_recording_index.reset();

    std::size_t expected_layers = 0;
    Layer* current_layer = nullptr;
    AppController::LayerOverride* current_override = nullptr;
    std::size_t expected_sources = 0;
    LayerSource* current_source = nullptr;
    std::optional<AppController::LayerEditState>* current_edit_state = nullptr;

    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        std::istringstream parser(line);
        std::string keyword;
        parser >> keyword;

        if (keyword == "preset_name") {
            parser >> std::quoted(preset.name);
        } else if (keyword == "source_file") {
            std::string path;
            parser >> std::quoted(path);
            preset.source_file = path;
        } else if (keyword == "trigger_mode") {
            std::string mode;
            parser >> mode;
            trigger_mode = mode == "continuous" ? TriggerMode::kContinuous : TriggerMode::kOneShot;
        } else if (keyword == "octave") {
            parser >> octave;
        } else if (keyword == "project_picture_path") {
            int present = 0;
            std::string path;
            if (!(parser >> present >> std::quoted(path))) {
                if (error_message != nullptr) {
                    *error_message = "Project file project_picture_path entry was invalid.";
                }
                return false;
            }
            project_picture_path = present ? std::optional<std::filesystem::path>(path) : std::nullopt;
        } else if (keyword == "session_recordings_directory") {
            std::string directory;
            parser >> std::quoted(directory);
            session_recordings_directory = directory;
        } else if (keyword == "session_recordings_selected") {
            int present = 0;
            std::size_t index = 0;
            if (!(parser >> present >> index)) {
                if (error_message != nullptr) {
                    *error_message = "Project file session_recordings_selected entry was invalid.";
                }
                return false;
            }
            selected_session_recording_index = present ? std::optional<std::size_t>(index) : std::nullopt;
        } else if (keyword == "session_recordings_count") {
            continue;
        } else if (keyword == "session_recording") {
            SessionRecordingInfo recording;
            std::string path;
            if (!(parser >> std::quoted(recording.name) >> std::quoted(path))) {
                if (error_message != nullptr) {
                    *error_message = "Project file session_recording entry was invalid.";
                }
                return false;
            }
            recording.path = path;
            std::optional<double> pictureStart;
            if (parser.good()) {
                read_optional(parser, pictureStart);
            }
            recording.picture_start_seconds = pictureStart;
            if (!(parser >> recording.channels)) {
                recording.channels = 2;
            }
            session_recordings.push_back(std::move(recording));
        } else if (keyword == "aux_gain") {
            parser >> aux_track_state.gain;
        } else if (keyword == "aux_bass_gain_db") {
            parser >> aux_track_state.bass_gain_db;
        } else if (keyword == "record_bus_surround_enabled") {
            int enabled = 0;
            parser >> enabled;
            record_bus_mode = enabled != 0 ? RecordBusMode::Surround51 : RecordBusMode::Stereo;
        } else if (keyword == "record_bus_channel_count") {
            int channel_count = 2;
            parser >> channel_count;
            if (channel_count >= 8) {
                record_bus_mode = RecordBusMode::Surround71;
            } else if (channel_count >= 7) {
                record_bus_mode = RecordBusMode::Surround70;
            } else if (channel_count >= 6) {
                record_bus_mode = RecordBusMode::Surround51;
            } else if (channel_count >= 5) {
                record_bus_mode = RecordBusMode::Surround50;
            } else {
                record_bus_mode = RecordBusMode::Stereo;
            }
        } else if (keyword == "aux_vst3_slot") {
            std::size_t slot_index = 0;
            int present = 0;
            std::string module_path;
            std::string class_id;
            std::string display_name;
            std::string component_state_hex;
            std::string controller_state_hex;
            int bypassed = 0;
            if (!(parser >> slot_index >> present
                    >> std::quoted(module_path)
                    >> std::quoted(class_id)
                    >> std::quoted(display_name)
                    >> std::quoted(component_state_hex)
                    >> std::quoted(controller_state_hex)
                    >> bypassed)) {
                if (error_message != nullptr) {
                    *error_message = "Project file aux_vst3_slot entry was invalid.";
                }
                return false;
            }
            if (present && slot_index < AppController::kPluginInsertSlotCount) {
                AppController::LayerOverride::Vst3PluginState state;
                state.module_path = module_path;
                state.class_id = class_id;
                state.display_name = display_name;
                state.bypassed = (bypassed != 0);
                if (!component_state_hex.empty()) {
                    hex_to_bytes(component_state_hex, &state.component_state);
                }
                if (!controller_state_hex.empty()) {
                    hex_to_bytes(controller_state_hex, &state.controller_state);
                }
                aux_track_state.vst3_plugins[slot_index] = std::move(state);
            }
        } else if (keyword == "layer_count") {
            parser >> expected_layers;
            preset.layers.clear();
            preset.layers.reserve(expected_layers);
            layer_overrides.reserve(expected_layers);
            layer_edit_states.reserve(expected_layers);
        } else if (keyword == "layer") {
            Layer layer;
            AppController::LayerOverride override_state;
            parser >> layer.index >> layer.active >> layer.mute >> layer.solo
                >> override_state.mute >> override_state.solo >> override_state.gain;
            if (!read_optional(parser, layer.gain) ||
                !read_optional(parser, layer.delay) ||
                !read_optional(parser, layer.start_offset) ||
                !read_optional(parser, layer.stop_offset) ||
                !(parser >> layer.reverse)) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer entry was invalid.";
                }
                return false;
            }
            preset.layers.push_back(std::move(layer));
            layer_overrides.push_back(std::move(override_state));
            layer_edit_states.push_back(std::nullopt);
            current_layer = &preset.layers.back();
            current_override = &layer_overrides.back();
            current_edit_state = &layer_edit_states.back();
            expected_sources = 0;
            current_source = nullptr;
        } else if (keyword == "layer_name") {
            if (current_layer == nullptr || !read_optional_string(parser, current_layer->custom_name)) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_name entry was invalid.";
                }
                return false;
            }
        } else if (keyword == "layer_embedded_media") {
            if (current_layer == nullptr) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_embedded_media entry appeared before a layer.";
                }
                return false;
            }
            std::optional<std::string> reference;
            std::optional<std::string> path;
            if (!read_optional_string(parser, reference) ||
                !read_optional_string(parser, path)) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_embedded_media entry was invalid.";
                }
                return false;
            }
            current_layer->embedded_media_reference = reference;
            current_layer->embedded_media_path = path;
        } else if (keyword == "override_audition") {
            if (current_override == nullptr ||
                !(parser >> current_override->audition_start) ||
                !read_optional(parser, current_override->audition_loop_start) ||
                !read_optional(parser, current_override->audition_loop_end)) {
                if (error_message != nullptr) {
                    *error_message = "Project file override_audition entry was invalid.";
                }
                return false;
            }
        } else if (keyword == "override_pan") {
            if (current_override == nullptr ||
                !(parser >> current_override->pan_x) ||
                !(parser >> current_override->pan_y) ||
                !(parser >> current_override->pan_x_right) ||
                !(parser >> current_override->pan_y_right)) {
                if (error_message != nullptr) {
                    *error_message = "Project file override_pan entry was invalid.";
                }
                return false;
            }
        } else if (keyword == "override_locked") {
            if (current_override == nullptr) {
                if (error_message != nullptr) {
                    *error_message = "Project file override_locked entry appeared before a layer.";
                }
                return false;
            }
            int locked = 0;
            if (!(parser >> locked)) {
                if (error_message != nullptr) {
                    *error_message = "Project file override_locked entry was invalid.";
                }
                return false;
            }
            current_override->locked = (locked != 0);
        } else if (keyword == "override_fx") {
            if (current_override == nullptr ||
                !(parser >> current_override->effects.reverse) ||
                !(parser >> current_override->effects.pitch_shift_semitones) ||
                !(parser >> current_override->effects.time_stretch_ratio)) {
                if (error_message != nullptr) {
                    *error_message = "Project file override_fx entry was invalid.";
                }
                return false;
            }
            double bass_lfe_gain_db = 0.0;
            if (parser >> bass_lfe_gain_db) {
                current_override->effects.bass_lfe_gain_db =
                    std::clamp(bass_lfe_gain_db, -24.0, 12.0);
            } else {
                current_override->effects.bass_lfe_gain_db = 0.0;
            }
            double eq_low_gain_db = 0.0;
            double eq_mid_gain_db = 0.0;
            double eq_high_gain_db = 0.0;
            if (parser >> eq_low_gain_db >> eq_mid_gain_db >> eq_high_gain_db) {
                current_override->effects.eq_low_gain_db =
                    std::clamp(eq_low_gain_db, -24.0, 12.0);
                current_override->effects.eq_mid_gain_db =
                    std::clamp(eq_mid_gain_db, -24.0, 12.0);
                current_override->effects.eq_high_gain_db =
                    std::clamp(eq_high_gain_db, -24.0, 12.0);
            } else {
                current_override->effects.eq_low_gain_db = 0.0;
                current_override->effects.eq_mid_gain_db = 0.0;
                current_override->effects.eq_high_gain_db = 0.0;
            }
            // Skip any remaining legacy effect fields on this line (backward compat)
            std::string rest_of_line;
            std::getline(parser, rest_of_line);
        } else if (keyword == "override_vst3") {
            if (current_override == nullptr) {
                if (error_message != nullptr) {
                    *error_message = "Project file override_vst3 entry appeared before a layer.";
                }
                return false;
            }
            int present = 0;
            std::string module_path;
            std::string class_id;
            std::string display_name;
            std::string component_hex;
            std::string controller_hex;
            if (!(parser >> present >> std::quoted(module_path) >> std::quoted(class_id) >>
                  std::quoted(display_name) >> std::quoted(component_hex) >>
                  std::quoted(controller_hex))) {
                if (error_message != nullptr) {
                    *error_message = "Project file override_vst3 entry was invalid.";
                }
                return false;
            }
            if (present != 0) {
                AppController::LayerOverride::Vst3PluginState plugin_state;
                plugin_state.module_path = std::move(module_path);
                plugin_state.class_id = std::move(class_id);
                plugin_state.display_name = std::move(display_name);
                if (!hex_to_bytes(component_hex, &plugin_state.component_state) ||
                    !hex_to_bytes(controller_hex, &plugin_state.controller_state)) {
                    if (error_message != nullptr) {
                        *error_message = "Project file override_vst3 state data was invalid.";
                    }
                    return false;
                }
                int bypassed_flag = 0;
                parser >> bypassed_flag; // optional, defaults to 0
                plugin_state.bypassed = (bypassed_flag != 0);
                current_override->vst3_plugins[0] = std::move(plugin_state);
            } else {
                current_override->vst3_plugins[0].reset();
            }
        } else if (keyword == "override_vst3_slot") {
            if (current_override == nullptr) {
                if (error_message != nullptr) {
                    *error_message = "Project file override_vst3_slot entry appeared before a layer.";
                }
                return false;
            }
            std::size_t slot_index = 0;
            int present = 0;
            std::string module_path;
            std::string class_id;
            std::string display_name;
            std::string component_hex;
            std::string controller_hex;
            if (!(parser >> slot_index >> present >> std::quoted(module_path) >> std::quoted(class_id) >>
                  std::quoted(display_name) >> std::quoted(component_hex) >>
                  std::quoted(controller_hex))) {
                if (error_message != nullptr) {
                    *error_message = "Project file override_vst3_slot entry was invalid.";
                }
                return false;
            }
            if (slot_index >= AppController::kPluginInsertSlotCount) {
                if (error_message != nullptr) {
                    *error_message = "Project file override_vst3_slot index was invalid.";
                }
                return false;
            }
            if (present != 0) {
                AppController::LayerOverride::Vst3PluginState plugin_state;
                plugin_state.module_path = std::move(module_path);
                plugin_state.class_id = std::move(class_id);
                plugin_state.display_name = std::move(display_name);
                if (!hex_to_bytes(component_hex, &plugin_state.component_state) ||
                    !hex_to_bytes(controller_hex, &plugin_state.controller_state)) {
                    if (error_message != nullptr) {
                        *error_message = "Project file override_vst3_slot state data was invalid.";
                    }
                    return false;
                }
                int bypassed_flag = 0;
                parser >> bypassed_flag; // optional, defaults to 0
                plugin_state.bypassed = (bypassed_flag != 0);
                current_override->vst3_plugins[slot_index] = std::move(plugin_state);
            } else {
                current_override->vst3_plugins[slot_index].reset();
            }
        } else if (keyword == "override_trigger_regions") {
            continue;
        } else if (keyword == "override_trigger_region") {
            if (current_override == nullptr) {
                if (error_message != nullptr) {
                    *error_message = "Project file override_trigger_region entry appeared before a layer.";
                }
                return false;
            }
            AppController::LayerOverride::TriggerRegion region;
            if (!(parser >> region.start >> region.end)) {
                if (error_message != nullptr) {
                    *error_message = "Project file override_trigger_region entry was invalid.";
                }
                return false;
            }
            current_override->trigger_regions.push_back(region);
        } else if (keyword == "source_count") {
            parser >> expected_sources;
            (void) expected_sources;
        } else if (keyword == "source") {
            if (current_layer == nullptr) {
                if (error_message != nullptr) {
                    *error_message = "Project file source entry appeared before a layer.";
                }
                return false;
            }
            LayerSource source;
            std::size_t region_count = 0;
            if (!read_optional_string(parser, source.name) ||
                !read_optional_string(parser, source.path) ||
                !read_optional_string(parser, source.file) ||
                !read_optional_string(parser, source.buffer_id) ||
                !(parser >> source.embedded >> region_count)) {
                if (error_message != nullptr) {
                    *error_message = "Project file source entry was invalid.";
                }
                return false;
            }
            current_layer->sources.push_back(std::move(source));
            current_source = &current_layer->sources.back();
        } else if (keyword == "region") {
            if (current_source == nullptr) {
                if (error_message != nullptr) {
                    *error_message = "Project file region entry appeared before a source.";
                }
                return false;
            }
            Region region;
            if (!read_optional(parser, region.start_offset) ||
                !read_optional(parser, region.end_offset) ||
                !read_optional(parser, region.in_point) ||
                !read_optional(parser, region.out_point) ||
                !read_optional(parser, region.loop_start) ||
                !read_optional(parser, region.loop_end) ||
                !read_optional(parser, region.loop_crossfade) ||
                !(parser >> region.loop_enabled)) {
                if (error_message != nullptr) {
                    *error_message = "Project file region entry was invalid.";
                }
                return false;
            }
            current_source->regions.push_back(std::move(region));
        } else if (keyword == "layer_edit") {
            if (current_edit_state == nullptr) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit entry appeared before a layer.";
                }
                return false;
            }
            AppController::LayerEditState state;
            std::size_t clip_count = 0;
            if (!(parser >> state.head_padding_seconds >> state.tail_padding_seconds >> clip_count)) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit entry was invalid.";
                }
                return false;
            }
            state.rendered_buffer_id.clear();
            state.clips.reserve(clip_count);
            *current_edit_state = std::move(state);
        } else if (keyword == "layer_edit_volume_automation") {
            if (current_edit_state == nullptr || !current_edit_state->has_value()) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_volume_automation entry appeared before layer_edit.";
                }
                return false;
            }
            int enabled = 0;
            std::size_t point_count = 0;
            if (!(parser >> enabled >> point_count)) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_volume_automation entry was invalid.";
                }
                return false;
            }
            current_edit_state->value().volume_automation_enabled = (enabled != 0);
            current_edit_state->value().volume_automation_points.clear();
            current_edit_state->value().volume_automation_points.reserve(point_count);
        } else if (keyword == "layer_edit_volume_random_settings") {
            if (current_edit_state == nullptr || !current_edit_state->has_value()) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_volume_random_settings entry appeared before layer_edit.";
                }
                return false;
            }
            int enabled = 0;
            auto& settings = current_edit_state->value().volume_random_settings;
            if (!(parser >> enabled
                         >> settings.loudest_db
                         >> settings.quietest_db
                         >> settings.period_longest_seconds
                         >> settings.period_shortest_seconds
                         >> settings.smoothing)) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_volume_random_settings entry was invalid.";
                }
                return false;
            }
            settings.enabled = (enabled != 0);
        } else if (keyword == "layer_edit_pan_random_settings") {
            if (current_edit_state == nullptr || !current_edit_state->has_value()) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_pan_random_settings entry appeared before layer_edit.";
                }
                return false;
            }
            int enabled = 0;
            auto& settings = current_edit_state->value().pan_random_settings;
            if (!(parser >> enabled
                         >> settings.farthest_left
                         >> settings.farthest_right
                         >> settings.farthest_front
                         >> settings.farthest_back)) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_pan_random_settings entry was invalid.";
                }
                return false;
            }
            if (header == kHeaderV21 || header == kHeaderV22) {
                if (!(parser >> settings.speed >> settings.smoothing)) {
                    if (error_message != nullptr) {
                        *error_message = "Project file layer_edit_pan_random_settings entry was invalid.";
                    }
                    return false;
                }
            } else {
                settings.speed = 0.5;
                if (!(parser >> settings.smoothing)) {
                    if (error_message != nullptr) {
                        *error_message = "Project file layer_edit_pan_random_settings entry was invalid.";
                    }
                    return false;
                }
            }
            settings.enabled = (enabled != 0);
        } else if (keyword == "layer_edit_stretch_random_settings") {
            if (current_edit_state == nullptr || !current_edit_state->has_value()) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_stretch_random_settings entry appeared before layer_edit.";
                }
                return false;
            }
            int enabled = 0;
            auto& settings = current_edit_state->value().stretch_random_settings;
            if (!(parser >> enabled
                         >> settings.lowest_percent
                         >> settings.highest_percent
                         >> settings.speed
                         >> settings.smoothing)) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_stretch_random_settings entry was invalid.";
                }
                return false;
            }
            settings.enabled = (enabled != 0);
        } else if (keyword == "layer_edit_volume_point") {
            if (current_edit_state == nullptr || !current_edit_state->has_value()) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_volume_point entry appeared before layer_edit.";
                }
                return false;
            }
            AppController::LayerEditState::VolumeAutomationPoint point;
            if (!(parser >> point.point_id >> point.timeline_seconds >> point.gain)) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_volume_point entry was invalid.";
                }
                return false;
            }
            current_edit_state->value().volume_automation_points.push_back(std::move(point));
        } else if (keyword == "layer_edit_stretch_automation") {
            if (current_edit_state == nullptr || !current_edit_state->has_value()) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_stretch_automation entry appeared before layer_edit.";
                }
                return false;
            }
            int enabled = 0;
            std::size_t point_count = 0;
            if (!(parser >> enabled >> point_count)) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_stretch_automation entry was invalid.";
                }
                return false;
            }
            current_edit_state->value().stretch_automation_enabled = (enabled != 0);
            current_edit_state->value().stretch_automation_points.clear();
            current_edit_state->value().stretch_automation_points.reserve(point_count);
        } else if (keyword == "layer_edit_stretch_point") {
            if (current_edit_state == nullptr || !current_edit_state->has_value()) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_stretch_point entry appeared before layer_edit.";
                }
                return false;
            }
            AppController::LayerEditState::StretchAutomationPoint point;
            if (!(parser >> point.point_id >> point.timeline_seconds >> point.ratio)) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_stretch_point entry was invalid.";
                }
                return false;
            }
            current_edit_state->value().stretch_automation_points.push_back(std::move(point));
        } else if (keyword == "layer_edit_pan_position_automation") {
            if (current_edit_state == nullptr || !current_edit_state->has_value()) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_pan_position_automation entry appeared before layer_edit.";
                }
                return false;
            }
            int enabled = 0;
            std::size_t point_count = 0;
            if (!(parser >> enabled >> point_count)) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_pan_position_automation entry was invalid.";
                }
                return false;
            }
            current_edit_state->value().pan_position_automation_enabled = (enabled != 0);
            current_edit_state->value().pan_position_automation_points.clear();
            current_edit_state->value().pan_position_automation_points.reserve(point_count);
        } else if (keyword == "layer_edit_pan_position_point") {
            if (current_edit_state == nullptr || !current_edit_state->has_value()) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_pan_position_point entry appeared before layer_edit.";
                }
                return false;
            }
            AppController::LayerEditState::PanAutomationPoint point;
            if (!(parser >> point.point_id >> point.timeline_seconds >> point.value)) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_pan_position_point entry was invalid.";
                }
                return false;
            }
            current_edit_state->value().pan_position_automation_points.push_back(std::move(point));
        } else if (keyword == "layer_edit_pan_front_back_automation") {
            if (current_edit_state == nullptr || !current_edit_state->has_value()) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_pan_front_back_automation entry appeared before layer_edit.";
                }
                return false;
            }
            int enabled = 0;
            std::size_t point_count = 0;
            if (!(parser >> enabled >> point_count)) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_pan_front_back_automation entry was invalid.";
                }
                return false;
            }
            current_edit_state->value().pan_front_back_automation_enabled = (enabled != 0);
            current_edit_state->value().pan_front_back_automation_points.clear();
            current_edit_state->value().pan_front_back_automation_points.reserve(point_count);
        } else if (keyword == "layer_edit_pan_front_back_point") {
            if (current_edit_state == nullptr || !current_edit_state->has_value()) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_pan_front_back_point entry appeared before layer_edit.";
                }
                return false;
            }
            AppController::LayerEditState::PanAutomationPoint point;
            if (!(parser >> point.point_id >> point.timeline_seconds >> point.value)) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_pan_front_back_point entry was invalid.";
                }
                return false;
            }
            current_edit_state->value().pan_front_back_automation_points.push_back(std::move(point));
        } else if (keyword == "layer_edit_pan_right_position_automation") {
            if (current_edit_state == nullptr || !current_edit_state->has_value()) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_pan_right_position_automation entry appeared before layer_edit.";
                }
                return false;
            }
            int enabled = 0;
            std::size_t point_count = 0;
            if (!(parser >> enabled >> point_count)) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_pan_right_position_automation entry was invalid.";
                }
                return false;
            }
            current_edit_state->value().pan_right_position_automation_enabled = (enabled != 0);
            current_edit_state->value().pan_right_position_automation_points.clear();
            current_edit_state->value().pan_right_position_automation_points.reserve(point_count);
        } else if (keyword == "layer_edit_pan_right_position_point") {
            if (current_edit_state == nullptr || !current_edit_state->has_value()) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_pan_right_position_point entry appeared before layer_edit.";
                }
                return false;
            }
            AppController::LayerEditState::PanAutomationPoint point;
            if (!(parser >> point.point_id >> point.timeline_seconds >> point.value)) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_pan_right_position_point entry was invalid.";
                }
                return false;
            }
            current_edit_state->value().pan_right_position_automation_points.push_back(std::move(point));
        } else if (keyword == "layer_edit_pan_right_front_back_automation") {
            if (current_edit_state == nullptr || !current_edit_state->has_value()) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_pan_right_front_back_automation entry appeared before layer_edit.";
                }
                return false;
            }
            int enabled = 0;
            std::size_t point_count = 0;
            if (!(parser >> enabled >> point_count)) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_pan_right_front_back_automation entry was invalid.";
                }
                return false;
            }
            current_edit_state->value().pan_right_front_back_automation_enabled = (enabled != 0);
            current_edit_state->value().pan_right_front_back_automation_points.clear();
            current_edit_state->value().pan_right_front_back_automation_points.reserve(point_count);
        } else if (keyword == "layer_edit_pan_right_front_back_point") {
            if (current_edit_state == nullptr || !current_edit_state->has_value()) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_pan_right_front_back_point entry appeared before layer_edit.";
                }
                return false;
            }
            AppController::LayerEditState::PanAutomationPoint point;
            if (!(parser >> point.point_id >> point.timeline_seconds >> point.value)) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_pan_right_front_back_point entry was invalid.";
                }
                return false;
            }
            current_edit_state->value().pan_right_front_back_automation_points.push_back(std::move(point));
        } else if (keyword == "layer_edit_doppler_automation") {
            if (current_edit_state == nullptr || !current_edit_state->has_value()) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_doppler_automation entry appeared before layer_edit.";
                }
                return false;
            }
            int enabled = 0;
            std::size_t point_count = 0;
            if (!(parser >> enabled >> point_count)) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_doppler_automation entry was invalid.";
                }
                return false;
            }
            current_edit_state->value().doppler_automation_enabled = (enabled != 0);
            current_edit_state->value().doppler_automation_points.clear();
            current_edit_state->value().doppler_automation_points.reserve(point_count);
        } else if (keyword == "layer_edit_doppler_settings") {
            if (current_edit_state == nullptr || !current_edit_state->has_value()) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_doppler_settings entry appeared before layer_edit.";
                }
                return false;
            }
            auto& settings = current_edit_state->value().doppler_settings;
            if (!(parser >> settings.edge_gain_db
                         >> settings.center_gain_db
                         >> settings.edge_pitch_semitones
                         >> settings.center_pitch_semitones)) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_doppler_settings entry was invalid.";
                }
                return false;
            }
        } else if (keyword == "layer_edit_doppler_point") {
            if (current_edit_state == nullptr || !current_edit_state->has_value()) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_doppler_point entry appeared before layer_edit.";
                }
                return false;
            }
            AppController::LayerEditState::PanAutomationPoint point;
            if (!(parser >> point.point_id >> point.timeline_seconds >> point.value)) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_doppler_point entry was invalid.";
                }
                return false;
            }
            current_edit_state->value().doppler_automation_points.push_back(std::move(point));
        } else if (keyword == "layer_edit_doppler_segment_shapes") {
            if (current_edit_state == nullptr || !current_edit_state->has_value()) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_doppler_segment_shapes entry appeared before layer_edit.";
                }
                return false;
            }
            std::size_t shape_count = 0;
            if (!(parser >> shape_count)) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_doppler_segment_shapes entry was invalid.";
                }
                return false;
            }
            current_edit_state->value().doppler_segment_shapes.clear();
            current_edit_state->value().doppler_segment_shapes.reserve(shape_count);
        } else if (keyword == "layer_edit_doppler_segment_shape") {
            if (current_edit_state == nullptr || !current_edit_state->has_value()) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_doppler_segment_shape entry appeared before layer_edit.";
                }
                return false;
            }
            radium::DopplerSegmentShape shape;
            int curve_type = 0;
            if (!(parser >> shape.left_point_id >> curve_type >> shape.curve_amount)) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_doppler_segment_shape entry was invalid.";
                }
                return false;
            }
            shape.curve_type = static_cast<radium::DopplerCurveType>(std::clamp(curve_type, 0, 3));
            current_edit_state->value().doppler_segment_shapes.push_back(shape);
        } else if (keyword == "layer_edit_clip") {
            if (current_edit_state == nullptr || !current_edit_state->has_value()) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_clip entry appeared before layer_edit.";
                }
                return false;
            }
            AppController::LayerEditClip clip;
            if (!(parser >> std::quoted(clip.source_buffer_id)) ||
                !read_optional_string(parser, clip.source_path) ||
                !(parser >> std::quoted(clip.source_label)
                    >> clip.source_start_seconds
                    >> clip.source_end_seconds
                    >> clip.timeline_start_seconds
                    >> clip.fade_in_seconds
                    >> clip.fade_out_seconds)) {
                if (error_message != nullptr) {
                    *error_message = "Project file layer_edit_clip entry was invalid.";
                }
                return false;
            }
            current_edit_state->value().clips.push_back(std::move(clip));
        } else if (keyword == "embedded_audio_count") {
            // Just announces how many entries follow; no per-entry state needed.
        } else if (keyword == "embedded_audio") {
            std::string buffer_id;
            int sample_rate = 0;
            int channels = 0;
            std::string hex;
            if (!(parser >> std::quoted(buffer_id)
                         >> sample_rate
                         >> channels
                         >> std::quoted(hex))) {
                if (error_message != nullptr) {
                    *error_message = "Project file embedded_audio entry was invalid.";
                }
                return false;
            }
            EmbeddedAudioBlob blob;
            blob.sample_rate = sample_rate;
            blob.channels = channels;
            if (!hex.empty() && !hex_to_bytes(hex, &blob.bytes)) {
                if (error_message != nullptr) {
                    *error_message = "Project file embedded_audio bytes were invalid.";
                }
                return false;
            }
            embedded_audio[buffer_id] = std::move(blob);
        }
    }

    preset.slot_group_count = preset.layers.size();
    preset.active_layer_count = 0;
    for (auto& layer : preset.layers) {
        if (layer.active) {
            ++preset.active_layer_count;
        }
        if (!layer.sources.empty()) {
            const auto& source = layer.sources.front();
            layer.source_name = source.name;
            layer.source_path = source.path;
            layer.source_file = source.file;
            if (source.embedded) {
                layer.embedded_media_reference = source.buffer_id;
            }
            if (layer.regions.empty()) {
                layer.regions = source.regions;
            }
        }
    }

    return true;
}

}  // namespace radium
