#pragma once

#include "playback_engine.h"
#include "plugin_host_interface.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <vector>

namespace radium {

// Real-time streaming mixer that generates audio in small blocks
// via render_block(), reading atomic LiveParams each call so UI
// changes (gain, mute, solo) are heard immediately.
class StreamingMixer {
public:
    // Per-voice playback state (set once at trigger time)
    struct Voice {
        const AudioBuffer* buffer = nullptr;
        std::size_t start_frame = 0;
        std::size_t end_frame = 0;
        double position = 0.0;
        double pitch_ratio = 1.0;
        bool reverse = false;
        bool finished = false;
        PlaybackRegion region;
        PlaybackEnvelope envelope;
        std::size_t samples_elapsed = 0;
        std::size_t delay_remaining = 0;
        bool loop_enabled = false;
        double loop_start_frame = 0.0;
        double loop_end_frame = 0.0;
        // For continuous mode
        bool continuous = false;
        std::size_t hold_samples = 0;
        bool released = false;
        bool loop_retrigger_enabled = false;
        std::vector<PlaybackRegion> loop_regions;
        int last_loop_region_index = -1;
    };

    // Atomic parameters updated from the UI thread, read by audio thread
    struct LiveParams {
        std::atomic<float> gain{1.0f};
        std::atomic<float> pan_x{0.0f};
        std::atomic<float> pan_y{1.0f};
        std::atomic<float> pan_x_right{0.0f};
        std::atomic<float> pan_y_right{1.0f};
        std::atomic<bool> mute{false};
        std::atomic<bool> solo{false};
        std::atomic<bool> active{true};
        std::atomic<float> time_stretch_ratio{1.0f};
        std::atomic<float> bass_lfe_gain_db{0.0f};
        std::atomic<float> eq_low_gain_db{0.0f};
        std::atomic<float> eq_mid_gain_db{0.0f};
        std::atomic<float> eq_high_gain_db{0.0f};
        std::atomic<double> playback_frame{-1.0};
    };

    static constexpr std::size_t kMaxPluginSlots = 5;

    struct LayerState {
        struct StretchAutomationPoint {
            double timeline_position = 0.0;
            float ratio = 1.0f;
        };

        struct PanAutomationPoint {
            std::size_t point_id = 0;
            double timeline_position = 0.0;
            float value = 0.0f;
        };

        struct DopplerSegmentShape {
            std::size_t left_point_id = 0;
            int curve_type = 0;
            float curve_amount = 0.5f;
        };

        struct DopplerSettings {
            float edge_gain_db = -24.0f;
            float center_gain_db = 0.0f;
            float edge_pitch_semitones = -4.0f;
            float center_pitch_semitones = 4.0f;
        };

        struct VolumeRandomSettings {
            bool enabled = false;
            float loudest_db = 0.0f;
            float quietest_db = -12.0f;
            float period_longest_seconds = 2.0f;
            float period_shortest_seconds = 0.35f;
            float smoothing = 0.7f;
        };

        struct VolumeRandomRuntime {
            bool initialized = false;
            float current_db = 0.0f;
            float start_db = 0.0f;
            float target_db = 0.0f;
            std::size_t segment_samples = 0;
            std::size_t segment_progress = 0;
            std::minstd_rand rng{1u};
        };

        struct PanRandomSettings {
            bool enabled = false;
            float farthest_left = -1.0f;
            float farthest_right = 1.0f;
            float farthest_front = 1.0f;
            float farthest_back = -1.0f;
            float speed = 0.5f;
            float smoothing = 0.7f;
        };

        struct PanRandomRuntime {
            bool initialized = false;
            float current_x = 0.0f;
            float current_y = 1.0f;
            float start_x = 0.0f;
            float start_y = 1.0f;
            float target_x = 0.0f;
            float target_y = 1.0f;
            std::size_t segment_samples = 0;
            std::size_t segment_progress = 0;
            std::minstd_rand rng{1u};
        };

        struct StretchRandomSettings {
            bool enabled = false;
            float lowest_percent = 100.0f;
            float highest_percent = 100.0f;
            float speed = 0.5f;
            float smoothing = 0.7f;
        };

        struct StretchRandomRuntime {
            bool initialized = false;
            float current_ratio = 1.0f;
            float start_ratio = 1.0f;
            float target_ratio = 1.0f;
            std::size_t segment_samples = 0;
            std::size_t segment_progress = 0;
            std::minstd_rand rng{1u};
        };

        Voice voice;
        std::unique_ptr<LiveParams> params;
        std::size_t layer_index = 0;
        bool route_to_record_bus = true;
        VolumeRandomSettings volume_random_settings;
        VolumeRandomRuntime volume_random_runtime;
        PanRandomSettings pan_random_settings;
        PanRandomRuntime pan_random_runtime;
        StretchRandomSettings stretch_random_settings;
        StretchRandomRuntime stretch_random_runtime;
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
        std::array<PluginHostSession*, kMaxPluginSlots> plugin_sessions{};
        std::array<bool, kMaxPluginSlots> plugin_bypassed{};
        std::array<float, 8> eq_low_state{};
        std::array<float, 8> eq_high_state{};

        LayerState() : params(std::make_unique<LiveParams>()), plugin_sessions{}, plugin_bypassed{} {}
    };

    struct AuxState {
        std::atomic<float> gain{1.0f};
        std::atomic<float> bass_gain_db{0.0f};
        std::array<PluginHostSession*, kMaxPluginSlots> plugin_sessions{};
        std::array<bool, kMaxPluginSlots> plugin_bypassed{};
        std::array<std::atomic<float>, 8> meter_levels{};
        std::atomic<int> meter_channel_count{2};
    };

    StreamingMixer() = default;
    ~StreamingMixer() = default;

    StreamingMixer(const StreamingMixer&) = delete;
    StreamingMixer& operator=(const StreamingMixer&) = delete;

    // Setup: called from the main/UI thread before streaming starts
    void prepare(int sample_rate);
    void set_layers(std::vector<LayerState>* layers);
    void set_record_bus_surround_enabled(bool enabled);
    bool record_bus_surround_enabled() const;
    void set_record_bus_channel_count(int channel_count);
    int record_bus_channel_count() const;

    // Called from the audio thread to fill output with
    // interleaved float samples matching the current device output channel count.
    void render_block(float* output, std::size_t frame_count, int device_output_channels);

    // Playback state
    bool is_playing() const;
    void stop();

    // Recording support: captures mixed output into a side buffer
    void set_recording(bool enabled);
    std::optional<RenderedAudio> take_recording();
    bool is_recording() const;
    std::size_t recording_frame_count() const;
    int sample_rate() const { return sample_rate_; }
    void recording_peaks(std::vector<float>& peaks_l, std::vector<float>& peaks_r,
                         std::size_t bucket_count) const;
    void recording_peaks_for_range(std::vector<float>& peaks_l, std::vector<float>& peaks_r,
                                   std::size_t start_frame, std::size_t end_frame,
                                   std::size_t bucket_count) const;
    void recording_channel_peaks(std::vector<std::vector<float>>& peaks_by_channel,
                                 std::size_t bucket_count) const;
    void recording_channel_peaks_for_range(std::vector<std::vector<float>>& peaks_by_channel,
                                           std::size_t start_frame,
                                           std::size_t end_frame,
                                           std::size_t bucket_count) const;
    void set_aux_gain(float gain);
    void set_aux_bass_gain_db(float gain_db);
    void set_aux_plugin_session(std::size_t slot_index, PluginHostSession* session, bool bypassed);
    void clear_aux_plugin_sessions();
    void aux_meter_levels(float& left, float& right) const;
    void aux_meter_levels(std::vector<float>& levels) const;

    // Called from UI thread when solo state changes
    void update_solo_flag();

private:
    float read_sample(const Voice& voice, int channel) const;
    void advance_voice(Voice& voice);
    void advance_voice_step(Voice& voice, double step_size);

    std::vector<LayerState>* layers_ = nullptr;
    std::atomic<bool> playing_{false};
    std::atomic<bool> any_solo_{false};
    std::atomic<bool> rendering_{false};  // true while audio thread is in render_block
    int sample_rate_ = 48000;


    // Recording
    std::atomic<bool> recording_{false};
    std::vector<float> record_buffer_;
    mutable std::mutex record_mutex_;
    AuxState aux_;
    std::mt19937 rng_{5678};
    std::atomic<int> record_bus_channels_{2};
    int last_recording_channels_ = 2;
    float lfe_lowpass_state_ = 0.0f;
    std::array<float, 8> aux_bass_lowpass_state_{};
};

}  // namespace radium
