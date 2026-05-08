#pragma once

#include "fixture_audio_bridge.h"
#include "import_to_playback.h"
#include "streaming_mixer.h"

#include <chrono>
#include <array>
#include <filesystem>
#include <functional>
#include <random>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <utility>

namespace radium {

enum class DopplerCurveType {
    Linear = 0,
    SCurve = 1,
    Convex = 2,
    Concave = 3
};

struct DopplerSettings {
    double edge_gain_db = -24.0;
    double center_gain_db = 0.0;
    double edge_pitch_semitones = -4.0;
    double center_pitch_semitones = 4.0;
};

struct VolumeRandomSettings {
    bool enabled = false;
    double loudest_db = 0.0;
    double quietest_db = -12.0;
    double period_longest_seconds = 2.0;
    double period_shortest_seconds = 0.35;
    double smoothing = 0.7;
};

struct PanRandomSettings {
    bool enabled = false;
    double farthest_left = -1.0;
    double farthest_right = 1.0;
    double farthest_front = 1.0;
    double farthest_back = -1.0;
    double speed = 0.5;
    double smoothing = 0.7;
};

struct StretchRandomSettings {
    bool enabled = false;
    double lowest_percent = 100.0;
    double highest_percent = 100.0;
    double speed = 0.5;
    double smoothing = 0.7;
};

struct DopplerSegmentShape {
    std::size_t left_point_id = 0;
    DopplerCurveType curve_type = DopplerCurveType::Linear;
    double curve_amount = 0.5;
};

enum class TriggerMode {
    kOneShot,
    kContinuous
};

enum class RecordBusMode {
    Stereo = 2,
    Surround50 = 5,
    Surround51 = 6,
    Surround70 = 7,
    Surround71 = 8
};

struct VisibleLayerState {
    std::size_t index = 0;
    bool active = false;
    bool mute = false;
    bool solo = false;
    bool locked = false;
    double gain = 1.0;
    std::string label;
    bool has_audio = false;
    bool selected = false;
    bool has_waveform = false;
    std::size_t source_count = 0;
};

struct LayerWaveformOverview {
    struct AuthoredRegion {
        double start = 0.0;
        double end = 1.0;
    };

    struct EditableClip {
        std::size_t clip_index = 0;
        double start = 0.0;
        double end = 1.0;
        double fade_in_end = 0.0;
        double fade_out_start = 1.0;
    };

    struct VolumeAutomationPoint {
        std::size_t point_id = 0;
        double timeline_position = 0.0;
        double gain = 1.0;
    };

    struct StretchAutomationPoint {
        std::size_t point_id = 0;
        double timeline_position = 0.0;
        double ratio = 1.0;
    };

    struct PanAutomationPoint {
        std::size_t point_id = 0;
        double timeline_position = 0.0;
        double value = 0.0;
    };

    bool available = false;
    bool reversed = false;
    std::size_t layer_index = 0;
    std::size_t frame_count = 0;
    int channels = 1;
    int sample_rate = 48000;
    double layer_delay_seconds = 0.0;
    double audition_start = 0.0;
    std::optional<double> loop_start;
    std::optional<double> loop_end;
    std::optional<AuthoredRegion> active_trigger_region;
    std::vector<AuthoredRegion> authored_regions;
    std::vector<EditableClip> editable_clips;
    bool volume_automation_enabled = false;
    std::vector<VolumeAutomationPoint> volume_automation_points;
    bool stretch_automation_enabled = false;
    std::vector<StretchAutomationPoint> stretch_automation_points;
    bool pan_position_automation_enabled = false;
    std::vector<PanAutomationPoint> pan_position_automation_points;
    bool pan_front_back_automation_enabled = false;
    std::vector<PanAutomationPoint> pan_front_back_automation_points;
    bool pan_right_position_automation_enabled = false;
    std::vector<PanAutomationPoint> pan_right_position_automation_points;
    bool pan_right_front_back_automation_enabled = false;
    std::vector<PanAutomationPoint> pan_right_front_back_automation_points;
    bool doppler_automation_enabled = false;
    std::vector<PanAutomationPoint> doppler_automation_points;
    DopplerSettings doppler_settings;
    std::vector<DopplerSegmentShape> doppler_segment_shapes;
    std::vector<float> peaks;       // max sample value per bucket
    std::vector<float> peaks_min;    // min sample value per bucket
    std::vector<float> peaks_right;
    std::vector<float> peaks_right_min;
};

struct TriggerResult {
    bool success = false;
    std::filesystem::path audio_path;
    double duration_seconds = 0.0;
    std::string message;
};

struct RenderedLayerAudio {
    std::size_t layer_index = 0;
    RenderedAudio audio;
};

struct SessionRecordingInfo {
    std::string name;
    std::filesystem::path path;
    std::optional<double> picture_start_seconds;
    int channels = 2;
};

// Callback for decoding audio files. When set on AppController, overrides the
// default ffmpeg-based decoding with a platform-specific implementation (e.g. JUCE).
using AudioDecodeFunc = std::function<FixtureAudioDecodeResult(
    const std::filesystem::path& audio_path,
    const std::filesystem::path& working_directory)>;

class AppController {
public:
    static constexpr std::size_t kPluginInsertSlotCount = 5;
    static constexpr double kLayerEditHeadPaddingSeconds = 0.001;
    static constexpr double kLayerEditTailPaddingSeconds = 4.0;

    enum class PanAutomationTarget {
        Position,
        FrontBack,
        RightPosition,
        RightFrontBack
    };

    struct LayerEffectState {
        bool reverse = false;
        double pitch_shift_semitones = 0.0;
        double time_stretch_ratio = 1.0;
        double bass_lfe_gain_db = 0.0;
        double eq_low_gain_db = 0.0;
        double eq_mid_gain_db = 0.0;
        double eq_high_gain_db = 0.0;
    };

    struct LayerOverride {
        struct Vst3PluginState {
            std::string module_path;
            std::string class_id;
            std::string display_name;
            std::vector<std::uint8_t> component_state;
            std::vector<std::uint8_t> controller_state;
            bool bypassed = false;
        };

        struct TriggerRegion {
            double start = 0.0;
            double end = 1.0;
        };

        bool mute = false;
        bool solo = false;
        bool locked = false;
        double gain = 1.0;
        double pan_x = 0.0;
        double pan_y = 1.0;
        double pan_x_right = 0.0;
        double pan_y_right = 1.0;
        double audition_start = 0.0;
        std::optional<double> audition_loop_start;
        std::optional<double> audition_loop_end;
        std::vector<TriggerRegion> trigger_regions;
        std::optional<TriggerRegion> active_trigger_region;
        std::optional<std::string> active_trigger_buffer_id;
        std::optional<double> active_trigger_duration_seconds;
        int last_trigger_region_index = -1;
        LayerEffectState effects;
        std::array<std::optional<Vst3PluginState>, kPluginInsertSlotCount> vst3_plugins{};
    };

    struct AuxTrackState {
        double gain = 1.0;
        double bass_gain_db = 0.0;
        std::array<std::optional<LayerOverride::Vst3PluginState>, kPluginInsertSlotCount> vst3_plugins{};
    };

    struct LayerEditClip {
        std::string source_buffer_id;
        std::optional<std::string> source_path;
        std::string source_label;
        double source_start_seconds = 0.0;
        double source_end_seconds = 0.0;
        double timeline_start_seconds = 0.0;
        double fade_in_seconds = 0.0;
        double fade_out_seconds = 0.0;
    };

    struct LayerEditState {
        struct VolumeAutomationPoint {
            std::size_t point_id = 0;
            double timeline_seconds = 0.0;
            double gain = 1.0;
        };

        struct StretchAutomationPoint {
            std::size_t point_id = 0;
            double timeline_seconds = 0.0;
            double ratio = 1.0;
        };

        struct PanAutomationPoint {
            std::size_t point_id = 0;
            double timeline_seconds = 0.0;
            double value = 0.0;
        };

        std::vector<LayerEditClip> clips;
        double head_padding_seconds = kLayerEditHeadPaddingSeconds;
        double tail_padding_seconds = kLayerEditTailPaddingSeconds;
        bool volume_automation_enabled = false;
        std::vector<VolumeAutomationPoint> volume_automation_points;
        VolumeRandomSettings volume_random_settings;
        PanRandomSettings pan_random_settings;
        StretchRandomSettings stretch_random_settings;
        bool stretch_automation_enabled = false;
        std::vector<StretchAutomationPoint> stretch_automation_points;
        bool pan_position_automation_enabled = false;
        std::vector<PanAutomationPoint> pan_position_automation_points;
        bool pan_front_back_automation_enabled = false;
        std::vector<PanAutomationPoint> pan_front_back_automation_points;
        bool pan_right_position_automation_enabled = false;
        std::vector<PanAutomationPoint> pan_right_position_automation_points;
        bool pan_right_front_back_automation_enabled = false;
        std::vector<PanAutomationPoint> pan_right_front_back_automation_points;
        bool doppler_automation_enabled = false;
        std::vector<PanAutomationPoint> doppler_automation_points;
        DopplerSettings doppler_settings;
        std::vector<DopplerSegmentShape> doppler_segment_shapes;
        std::string rendered_buffer_id;
    };

    explicit AppController(std::filesystem::path working_directory);
    void set_audio_decode_func(AudioDecodeFunc func);
    void set_output_sample_rate(int sample_rate);

    bool import_file(const std::filesystem::path& radium_path);

    bool has_imported_preset() const;
    std::string preset_summary_text() const;
    std::string diagnostics_text() const;
    std::vector<VisibleLayerState> visible_layers(std::size_t count = 5) const;
    std::vector<VisibleLayerState> visible_layers_from(std::size_t start_index, std::size_t count = 5) const;
    std::size_t layer_count() const;
    const std::vector<LayerOverride>& layer_overrides() const;
    std::optional<std::size_t> selected_layer_index() const;
    bool select_layer(std::size_t layer_index);
    std::optional<LayerWaveformOverview> layer_waveform(
        std::size_t layer_index,
        std::size_t bucket_count = 256,
        double view_start = 0.0,
        double view_end = 1.0
    ) const;
    std::uint64_t layer_waveform_revision(std::size_t layer_index) const;
    std::optional<double> layer_timeline_duration_seconds(std::size_t layer_index) const;
    std::optional<LayerEffectState> layer_effect_state(std::size_t layer_index) const;
    std::optional<LayerOverride::Vst3PluginState> layer_vst3_plugin(std::size_t layer_index) const;
    std::array<std::optional<LayerOverride::Vst3PluginState>, kPluginInsertSlotCount> layer_vst3_plugins(std::size_t layer_index) const;
    std::string layer_debug_text(std::size_t layer_index) const;
    std::vector<RenderedLayerAudio> render_one_shot_layer_audio(int midi_note, std::string* error_message);
    std::optional<RenderedAudio> render_layer_audition_audio(std::size_t layer_index, std::string* error_message) const;
    std::filesystem::path write_preview_audio_file(const RenderedAudio& audio, const std::string& stem) const;
    void set_last_rendered_audio(const RenderedAudio& audio);
    std::optional<RenderedAudio> last_rendered_audio() const;

    void set_trigger_mode(TriggerMode mode);
    TriggerMode trigger_mode() const;
    std::optional<int> held_note_midi() const;

    int octave() const;
    void set_octave(int octave);

    bool set_layer_mute(std::size_t layer_index, bool mute);
    bool set_layer_solo(std::size_t layer_index, bool solo);
    bool set_layer_locked(std::size_t layer_index, bool locked);
    bool layer_locked(std::size_t layer_index) const;
    bool set_layer_custom_name(std::size_t layer_index, std::optional<std::string> custom_name);
    bool set_layer_gain(std::size_t layer_index, double gain);
    bool set_layer_pan(std::size_t layer_index, double x, double y);
    bool set_layer_pan_right(std::size_t layer_index, double x, double y);
    bool layer_is_stereo(std::size_t layer_index) const;
    bool set_layer_audition_start(std::size_t layer_index, double normalized_start);
    bool set_layer_audition_loop(std::size_t layer_index, double normalized_start, double normalized_end);
    bool clear_layer_audition_loop(std::size_t layer_index);
    bool add_layer_trigger_region(std::size_t layer_index, double normalized_start, double normalized_end);
    bool update_layer_trigger_region(
        std::size_t layer_index,
        std::size_t region_index,
        double normalized_start,
        double normalized_end
    );
    bool set_layer_effect_state(std::size_t layer_index, const LayerEffectState& effects);
    bool set_layer_vst3_plugin(std::size_t layer_index, const LayerOverride::Vst3PluginState& plugin_state);
    bool set_layer_vst3_plugin(std::size_t layer_index, std::size_t slot_index, const LayerOverride::Vst3PluginState& plugin_state);
    bool clear_layer_vst3_plugin(std::size_t layer_index);
    bool clear_layer_vst3_plugin(std::size_t layer_index, std::size_t slot_index);
    bool toggle_layer_vst3_bypass(std::size_t layer_index, std::size_t slot_index);
    const AuxTrackState& aux_track_state() const;
    double aux_gain() const;
    bool set_aux_gain(double gain);
    double aux_bass_gain_db() const;
    bool set_aux_bass_gain_db(double gain_db);
    std::array<std::optional<LayerOverride::Vst3PluginState>, kPluginInsertSlotCount> aux_vst3_plugins() const;
    bool set_aux_vst3_plugin(std::size_t slot_index, const LayerOverride::Vst3PluginState& plugin_state);
    bool clear_aux_vst3_plugin(std::size_t slot_index);
    bool toggle_aux_vst3_bypass(std::size_t slot_index);
    bool clear_layer_trigger_regions(std::size_t layer_index);
    bool auto_split_layer_regions(std::size_t layer_index, std::string* error_message);
    bool layer_has_clip_edits(std::size_t layer_index) const;
    bool split_layer_edit_clip(std::size_t layer_index, double normalized_timeline);
    bool trim_layer_edit_left(std::size_t layer_index, double normalized_timeline);
    bool trim_layer_edit_right(std::size_t layer_index, double normalized_timeline);
    bool set_layer_edit_fade_in(std::size_t layer_index, double normalized_timeline);
    bool set_layer_edit_fade_out(std::size_t layer_index, double normalized_timeline);
    bool apply_layer_edit_crossfade(std::size_t layer_index, double normalized_start, double normalized_end);
    bool enable_layer_volume_automation(std::size_t layer_index);
    bool layer_volume_automation_enabled(std::size_t layer_index) const;
    std::optional<std::size_t> add_layer_volume_automation_point(std::size_t layer_index, double normalized_timeline);
    bool move_layer_volume_automation_point(std::size_t layer_index, std::size_t point_id, double normalized_timeline, double gain, bool rebuild_render = true);
    bool delete_layer_volume_automation_point(std::size_t layer_index, std::size_t point_id);
    bool commit_layer_volume_automation(std::size_t layer_index);
    bool reset_layer_volume_automation(std::size_t layer_index);
    bool remove_layer_volume_automation(std::size_t layer_index);
    std::optional<VolumeRandomSettings> layer_volume_random_settings(std::size_t layer_index) const;
    bool set_layer_volume_random_settings(std::size_t layer_index, const VolumeRandomSettings& settings);
    std::optional<PanRandomSettings> layer_pan_random_settings(std::size_t layer_index) const;
    bool set_layer_pan_random_settings(std::size_t layer_index, const PanRandomSettings& settings);
    std::optional<StretchRandomSettings> layer_stretch_random_settings(std::size_t layer_index) const;
    bool set_layer_stretch_random_settings(std::size_t layer_index, const StretchRandomSettings& settings);
    bool enable_layer_stretch_automation(std::size_t layer_index);
    bool layer_stretch_automation_enabled(std::size_t layer_index) const;
    std::optional<std::size_t> add_layer_stretch_automation_point(std::size_t layer_index, double normalized_timeline);
    bool move_layer_stretch_automation_point(std::size_t layer_index, std::size_t point_id, double normalized_timeline, double ratio);
    bool delete_layer_stretch_automation_point(std::size_t layer_index, std::size_t point_id);
    bool commit_layer_stretch_automation(std::size_t layer_index);
    bool reset_layer_stretch_automation(std::size_t layer_index);
    bool remove_layer_stretch_automation(std::size_t layer_index);
    bool enable_layer_pan_automation(std::size_t layer_index, PanAutomationTarget target);
    bool layer_pan_automation_enabled(std::size_t layer_index, PanAutomationTarget target) const;
    std::optional<std::size_t> add_layer_pan_automation_point(std::size_t layer_index, PanAutomationTarget target, double normalized_timeline);
    bool move_layer_pan_automation_point(std::size_t layer_index, PanAutomationTarget target, std::size_t point_id, double normalized_timeline, double value);
    bool delete_layer_pan_automation_point(std::size_t layer_index, PanAutomationTarget target, std::size_t point_id);
    bool commit_layer_pan_automation(std::size_t layer_index, PanAutomationTarget target);
    bool reset_layer_pan_automation(std::size_t layer_index, PanAutomationTarget target);
    bool remove_layer_pan_automation(std::size_t layer_index, PanAutomationTarget target);
    bool enable_layer_doppler_automation(std::size_t layer_index);
    bool layer_doppler_automation_enabled(std::size_t layer_index) const;
    std::optional<std::size_t> add_layer_doppler_automation_point(std::size_t layer_index, double normalized_timeline);
    bool move_layer_doppler_automation_point(
        std::size_t layer_index,
        std::size_t point_id,
        double normalized_timeline,
        double value
    );
    bool delete_layer_doppler_automation_point(std::size_t layer_index, std::size_t point_id);
    bool commit_layer_doppler_automation(std::size_t layer_index);
    bool replace_layer_doppler_automation_points(
        std::size_t layer_index,
        const std::vector<std::pair<double, double>>& points);
    bool reset_layer_doppler_automation(std::size_t layer_index);
    bool remove_layer_doppler_automation(std::size_t layer_index);
    std::optional<DopplerSettings> layer_doppler_settings(std::size_t layer_index) const;
    bool set_layer_doppler_settings(std::size_t layer_index, const DopplerSettings& settings);
    std::optional<DopplerSegmentShape> layer_doppler_segment_shape(
        std::size_t layer_index,
        std::size_t left_point_id
    ) const;
    bool set_layer_doppler_segment_shape(
        std::size_t layer_index,
        std::size_t left_point_id,
        DopplerCurveType curve_type,
        double curve_amount
    );
    bool move_layer_edit_clip(std::size_t layer_index, std::size_t clip_index, double timeline_delta_seconds, bool rebuild_render = true);
    bool move_layer_edit_clips(std::size_t layer_index, const std::vector<std::size_t>& clip_indices, double timeline_delta_seconds, bool rebuild_render = true);
    bool trim_layer_edit_clip_edge(std::size_t layer_index, std::size_t clip_index, bool trim_left_edge, double normalized_timeline, bool rebuild_render = true);
    bool set_layer_edit_clip_fade(std::size_t layer_index, std::size_t clip_index, bool fade_in, double normalized_timeline, bool rebuild_render = true);
    bool clear_layer_edit_clip_fade(std::size_t layer_index, std::size_t clip_index, bool fade_in);
    bool commit_layer_edit_changes(std::size_t layer_index, std::optional<std::size_t> priority_clip_index = std::nullopt);
    bool commit_layer_edit_changes(std::size_t layer_index, const std::vector<std::size_t>& priority_clip_indices);
    void clear_layer_clip_edits(std::size_t layer_index);
    std::optional<LayerEditState> layer_edit_state(std::size_t layer_index) const;
    std::optional<LayerEditState> layer_edit_state_snapshot(std::size_t layer_index) const;
    bool restore_layer_edit_state(std::size_t layer_index, const LayerEditState& state);


    TriggerResult trigger_note_on(int midi_note);
    TriggerResult trigger_note_off(int midi_note);

    // Called after streaming layers are rebuilt so plugin hosts can be wired.
    std::function<void()> on_streaming_layers_rebuilt;

    // Streaming playback (real-time mixer path)
    bool start_streaming_playback(int midi_note, std::string* error_message);
    void stop_streaming_playback();
    bool is_streaming() const;
    std::optional<double> layer_streaming_position(std::size_t layer_index) const;
    StreamingMixer& streaming_mixer();
    std::vector<StreamingMixer::LayerState>& streaming_layer_states();
    void push_live_gain(std::size_t layer_index);
    void push_live_pan(std::size_t layer_index);
    void push_live_mute(std::size_t layer_index);
    void push_live_solo();
    void push_live_stretch(std::size_t layer_index);
    void push_live_bass_lfe_gain(std::size_t layer_index);
    void push_live_layer_eq(std::size_t layer_index);
    void set_session_recording_armed(bool armed);
    bool session_recording_armed() const;
    bool record_bus_surround_enabled() const;
    void set_record_bus_surround_enabled(bool enabled);
    RecordBusMode record_bus_mode() const;
    void set_record_bus_mode(RecordBusMode mode);
    std::optional<SessionRecordingInfo> commit_session_recording(
        const RenderedAudio& audio,
        const std::string& stem,
        std::optional<std::pair<double, double>> punch_in_region,
        std::optional<double> picture_start_seconds,
        std::string* error_message
    );
    const std::vector<SessionRecordingInfo>& session_recordings() const;
    std::optional<std::size_t> selected_session_recording_index() const;
    bool select_session_recording(std::size_t index);
    std::optional<SessionRecordingInfo> selected_session_recording() const;
    bool rename_selected_session_recording(const std::string& new_name, std::string* error_message);
    bool export_selected_session_recording(const std::filesystem::path& output_path, std::string* error_message) const;
    bool delete_session_recording(std::size_t index);
    const std::filesystem::path& session_recordings_directory() const;
    void set_project_picture_path(const std::filesystem::path& path);
    void clear_project_picture_path();
    std::optional<std::filesystem::path> project_picture_path() const;

    bool export_one_shot_wav(const std::filesystem::path& output_path, std::string* error_message);
    bool record_last_trigger_to_wav(const std::filesystem::path& output_path, std::string* error_message);

    bool new_empty_project(const std::string& name = "Untitled Project");
    bool add_audio_file_to_layer(std::size_t layer_index, const std::filesystem::path& audio_path, std::string* error_message);
    bool replace_layer_audio(std::size_t layer_index, const std::filesystem::path& audio_path, std::string* error_message);
    bool clear_layer_audio(std::size_t layer_index, std::string* error_message);
    bool save_project(const std::filesystem::path& output_path, std::string* error_message) const;
    bool load_project(const std::filesystem::path& input_path, std::string* error_message);

    bool start_streaming_audition(std::size_t layer_index, std::string* error_message);
    bool play_session_take(std::size_t take_index, std::string* error_message, double start_normalized = 0.0);

private:
    struct HeldNoteState {
        int midi_note = 60;
        std::chrono::steady_clock::time_point started_at;
    };

    FixtureAudioDecodeResult decode_audio(const std::filesystem::path& path, const std::filesystem::path& workdir);
    FixtureAudioResolution resolve_audio(const Preset& preset, const std::filesystem::path& workdir);
    PlaybackPreset build_playback_preset_for_note(int midi_note);
    PlaybackPreset build_single_layer_audition_preset(std::size_t layer_index, bool loop_selection) const;
    const AudioBuffer* layer_audio_buffer(std::size_t layer_index) const;
    std::optional<std::string> layer_audio_buffer_id(std::size_t layer_index) const;
    const LayerEditState* layer_edit_state_ptr(std::size_t layer_index) const;
    LayerEditState* layer_edit_state_ptr(std::size_t layer_index);
    std::optional<LayerEditState> bootstrap_layer_edit_state(std::size_t layer_index) const;
    bool bootstrap_layer_edit_state_if_available(std::size_t layer_index, std::string* error_message);
    void bootstrap_layer_edit_states_for_audio_layers();
    bool ensure_layer_edit_state(std::size_t layer_index, std::string* error_message);
    bool rebuild_layer_edit_render(std::size_t layer_index, std::string* error_message);
    void touch_layer_waveform_revision(std::size_t layer_index);
    RenderedAudio render_layer_edit_audio(std::size_t layer_index, std::string* error_message) const;
    double layer_edit_total_duration_seconds(const LayerEditState& state) const;
    std::optional<std::size_t> find_layer_edit_clip_at(std::size_t layer_index, double timeline_seconds) const;
    void clear_layer_edit_render(std::size_t layer_index);
    std::optional<RenderedAudio> render_one_shot_for_note(int midi_note, std::string* error_message);
    std::optional<RenderedAudio> render_continuous_for_note(
        int midi_note,
        std::size_t hold_samples,
        std::string* error_message
    );
    void reset_session_recordings_directory(const std::filesystem::path& project_hint = {});
    std::filesystem::path write_temp_audio(const RenderedAudio& audio, const std::string& stem, bool preview_compatible) const;
    static RenderedAudio slice_audio_from_frame(const RenderedAudio& audio, std::size_t start_frame);

    AudioDecodeFunc audio_decode_func_;
    int output_sample_rate_ = 48000;
    std::filesystem::path working_directory_;
    std::filesystem::path session_recordings_directory_;
    FileSummary summary_;
    Preset imported_preset_;
    FixtureAudioResolution fixture_audio_;
    std::unordered_map<std::string, AudioBuffer> project_audio_buffers_;
    std::vector<LayerOverride> layer_overrides_;
    std::vector<std::optional<LayerEditState>> layer_edit_states_;
    std::vector<std::uint64_t> layer_waveform_revisions_;
    TriggerMode trigger_mode_ = TriggerMode::kOneShot;
    int octave_ = 4;
    mutable PlaybackEngine playback_engine_{1234};
    std::mt19937 region_rng_{5678};
    std::optional<RenderedAudio> last_rendered_audio_;
    std::vector<SessionRecordingInfo> session_recordings_;
    std::optional<std::size_t> selected_session_recording_index_;
    std::optional<std::filesystem::path> project_picture_path_;
    AuxTrackState aux_track_state_;
    bool session_recording_armed_ = false;
    RecordBusMode record_bus_mode_ = RecordBusMode::Stereo;
    std::optional<HeldNoteState> held_note_;
    std::optional<std::size_t> selected_layer_index_;
    StreamingMixer streaming_mixer_;
    std::vector<StreamingMixer::LayerState> streaming_layers_;
    bool streaming_active_ = false;
    std::optional<AudioBuffer> take_playback_buffer_;

    bool start_streaming_preset(const PlaybackPreset& playback);
};

}  // namespace radium
