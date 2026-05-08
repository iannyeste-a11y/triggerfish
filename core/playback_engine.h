#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace radium {

struct AudioBuffer {
    int sample_rate = 48000;
    int channels = 1;
    std::vector<float> samples;

    std::size_t frame_count() const;
    float sample_at(std::size_t frame_index, int channel) const;
};

struct PlaybackRegion {
    double start = 0.0;
    double end = 1.0;
    double loop_start = 0.0;
    double loop_end = 1.0;
    bool loop_enabled = false;
};

struct PlaybackSource {
    std::string buffer_id;
    std::vector<PlaybackRegion> regions;
};

struct PlaybackRandomization {
    double raw_amount = 0.0;
};

struct PlaybackEnvelope {
    double attack_seconds = 0.0;
    double decay_seconds = 0.0;
    double sustain_level = 1.0;
    double release_seconds = 0.0;
};

struct PlaybackEffects {
    struct Reverb {
        bool enabled = false;
        double wet = 0.15;
        double decay = 0.35;
    };

    struct Delay {
        bool enabled = false;
        double time_seconds = 0.18;
        double feedback = 0.25;
        double mix = 0.2;
    };

    struct BassBoost {
        bool enabled = false;
        double amount = 0.0;
    };

    struct Compressor {
        bool enabled = false;
        double threshold = 0.75;
        double ratio = 2.0;
    };

    struct Limiter {
        bool enabled = false;
        double ceiling = 0.95;
    };

    double pitch_shift_semitones = 0.0;
    double time_stretch_ratio = 1.0;
    Reverb reverb;
    Delay delay;
    BassBoost bass_boost;
    Compressor compressor;
    Limiter limiter;
};

struct PlaybackLayer {
    std::size_t index = 0;
    bool active = true;
    bool mute = false;
    bool solo = false;
    bool reverse = false;
    bool no_immediate_repeat = false;
    double gain = 1.0;
    double pan_x = 0.0;
    double pan_y = 1.0;
    double pan_x_right = 0.0;
    double pan_y_right = 1.0;
    double delay_seconds = 0.0;
    double start_offset = 0.0;
    double stop_offset = 1.0;
    double coarse_semitones = 0.0;
    double fine_cents = 0.0;
    PlaybackRandomization random_gain;
    PlaybackRandomization random_pan;
    PlaybackEnvelope envelope;
    PlaybackEffects effects;
    std::vector<PlaybackSource> sources;
};

struct PlaybackPreset {
    int output_sample_rate = 48000;
    std::vector<PlaybackLayer> layers;
    std::vector<std::string> assumptions;
};

struct RenderedAudio {
    int sample_rate = 48000;
    int channels = 2;
    std::vector<float> samples;

    std::size_t frame_count() const;
};

class PlaybackEngine {
public:
    struct LayerHistory {
        int last_source_index = -1;
        int last_region_index = -1;
    };

    explicit PlaybackEngine(std::uint32_t seed = 0);

    RenderedAudio render_one_shot(
        const PlaybackPreset& preset,
        const std::unordered_map<std::string, AudioBuffer>& buffers
    );

    RenderedAudio render_continuous(
        const PlaybackPreset& preset,
        const std::unordered_map<std::string, AudioBuffer>& buffers,
        std::size_t hold_samples
    );

    static void write_wav_24(
        const std::filesystem::path& output_path,
        const RenderedAudio& audio
    );

    static void write_wav_16(
        const std::filesystem::path& output_path,
        const RenderedAudio& audio
    );

private:
    std::vector<LayerHistory> histories_;
    std::mt19937 rng_;
};

}  // namespace radium
