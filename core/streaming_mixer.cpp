#include "streaming_mixer.h"
#include "surround_panner.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace radium {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kStretchAutomationMinRatio = 0.01;
constexpr double kStretchAutomationMaxRatio = 8.0;
constexpr int kStereoChannelCount = 2;
constexpr int kSurround50ChannelCount = 5;
constexpr int kSurround51ChannelCount = 6;
constexpr int kSurround70ChannelCount = 7;
constexpr int kSurround71ChannelCount = 8;
constexpr int kMaxRecordBusChannelCount = kSurround71ChannelCount;
constexpr int kChannelL = 0;
constexpr int kChannelR = 1;
constexpr int kChannelC = 2;
constexpr int kChannelLfe = 3;
constexpr int kChannelLs50 = 3;
constexpr int kChannelRs50 = 4;
constexpr int kChannelLs51 = 4;
constexpr int kChannelRs51 = 5;
constexpr int kChannelLs70 = 3;
constexpr int kChannelRs70 = 4;
constexpr int kChannelLrs70 = 5;
constexpr int kChannelRrs70 = 6;
constexpr int kChannelLs71 = 4;
constexpr int kChannelRs71 = 5;
constexpr int kChannelLrs71 = 6;
constexpr int kChannelRrs71 = 7;

float read_buffer_sample(const AudioBuffer& buffer, double frame_position, int channel) {
    if (buffer.frame_count() == 0) {
        return 0.0f;
    }
    const double clamped = std::clamp(frame_position, 0.0, static_cast<double>(buffer.frame_count() - 1));
    const auto frame_a = static_cast<std::size_t>(clamped);
    const auto frame_b = std::min(frame_a + 1, buffer.frame_count() - 1);
    const double frac = clamped - static_cast<double>(frame_a);
    const int clamped_channel = std::clamp(channel, 0, std::max(0, buffer.channels - 1));
    const float sample_a = buffer.sample_at(frame_a, clamped_channel);
    const float sample_b = buffer.sample_at(frame_b, clamped_channel);
    return static_cast<float>(sample_a + (sample_b - sample_a) * frac);
}

double compute_envelope(
    const PlaybackEnvelope& env,
    std::size_t sample_index,
    std::size_t note_off_sample,
    bool released,
    int sample_rate
) {
    const std::size_t attack = static_cast<std::size_t>(std::max(0.0, env.attack_seconds) * sample_rate);
    const std::size_t decay = static_cast<std::size_t>(std::max(0.0, env.decay_seconds) * sample_rate);
    const std::size_t release = static_cast<std::size_t>(std::max(0.0, env.release_seconds) * sample_rate);

    auto level_before_release = [&](std::size_t s) {
        if (attack > 0 && s < attack) {
            return static_cast<double>(s) / static_cast<double>(attack);
        }
        if (decay > 0 && s < attack + decay) {
            const auto progress = static_cast<double>(s - attack) / static_cast<double>(decay);
            return 1.0 + (env.sustain_level - 1.0) * progress;
        }
        return env.sustain_level;
    };

    if (!released || sample_index < note_off_sample) {
        return level_before_release(sample_index);
    }

    if (release == 0) {
        return 0.0;
    }

    const auto progress = static_cast<double>(sample_index - note_off_sample) / static_cast<double>(release);
    if (progress >= 1.0) {
        return 0.0;
    }
    return level_before_release(note_off_sample) * (1.0 - progress);
}

float evaluate_stretch_ratio(const StreamingMixer::LayerState& layer, double normalizedTimeline) {
    const float fallback =
        std::clamp(layer.params->time_stretch_ratio.load(std::memory_order_relaxed),
                   static_cast<float>(kStretchAutomationMinRatio),
                   static_cast<float>(kStretchAutomationMaxRatio));
    if (!layer.stretch_automation_enabled || layer.stretch_automation_points.empty()) {
        return fallback;
    }

    const auto& points = layer.stretch_automation_points;
    const double clamped = std::clamp(normalizedTimeline, 0.0, 1.0);
    if (points.size() == 1) {
        return std::clamp(points.front().ratio,
                          static_cast<float>(kStretchAutomationMinRatio),
                          static_cast<float>(kStretchAutomationMaxRatio));
    }
    if (clamped <= points.front().timeline_position) {
        return std::clamp(points.front().ratio,
                          static_cast<float>(kStretchAutomationMinRatio),
                          static_cast<float>(kStretchAutomationMaxRatio));
    }
    if (clamped >= points.back().timeline_position) {
        return std::clamp(points.back().ratio,
                          static_cast<float>(kStretchAutomationMinRatio),
                          static_cast<float>(kStretchAutomationMaxRatio));
    }

    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const auto& left = points[i];
        const auto& right = points[i + 1];
        if (clamped >= left.timeline_position && clamped <= right.timeline_position) {
            const double span = std::max(0.000001, right.timeline_position - left.timeline_position);
            const double t = std::clamp((clamped - left.timeline_position) / span, 0.0, 1.0);
            return std::clamp(static_cast<float>(left.ratio + (right.ratio - left.ratio) * t),
                              static_cast<float>(kStretchAutomationMinRatio),
                              static_cast<float>(kStretchAutomationMaxRatio));
        }
    }

    return fallback;
}

float evaluate_pan_points(const std::vector<StreamingMixer::LayerState::PanAutomationPoint>& points,
                          bool enabled,
                          double normalizedTimeline,
                          float fallback) {
    const float clampedFallback = std::clamp(fallback, -1.0f, 1.0f);
    if (!enabled || points.empty()) {
        return clampedFallback;
    }
    const double clamped = std::clamp(normalizedTimeline, 0.0, 1.0);
    if (points.size() == 1) {
        return std::clamp(points.front().value, -1.0f, 1.0f);
    }
    if (clamped <= points.front().timeline_position) {
        return std::clamp(points.front().value, -1.0f, 1.0f);
    }
    if (clamped >= points.back().timeline_position) {
        return std::clamp(points.back().value, -1.0f, 1.0f);
    }
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const auto& left = points[i];
        const auto& right = points[i + 1];
        if (clamped >= left.timeline_position && clamped <= right.timeline_position) {
            const double span = std::max(0.000001, right.timeline_position - left.timeline_position);
            const double t = std::clamp((clamped - left.timeline_position) / span, 0.0, 1.0);
            return std::clamp(static_cast<float>(left.value + (right.value - left.value) * t), -1.0f, 1.0f);
        }
    }
    return clampedFallback;
}

double apply_doppler_curve(double t, int curveType, float curveAmount) {
    const double clampedT = std::clamp(t, 0.0, 1.0);
    const double amount = std::clamp(static_cast<double>(curveAmount), 0.0, 1.0);
    switch (curveType) {
        case 1: {
            const double smooth = clampedT * clampedT * (3.0 - 2.0 * clampedT);
            return std::clamp(clampedT + (smooth - clampedT) * amount, 0.0, 1.0);
        }
        case 2: {
            const double exponent = 1.0 + amount * 4.0;
            return std::clamp(std::pow(clampedT, exponent), 0.0, 1.0);
        }
        case 3: {
            const double exponent = 1.0 + amount * 4.0;
            return std::clamp(1.0 - std::pow(1.0 - clampedT, exponent), 0.0, 1.0);
        }
        case 0:
        default:
            return clampedT;
    }
}

float evaluate_doppler_points(const StreamingMixer::LayerState& layer,
                              double normalizedTimeline,
                              float fallback) {
    const float clampedFallback = std::clamp(fallback, -1.0f, 1.0f);
    const auto& points = layer.doppler_automation_points;
    if (!layer.doppler_automation_enabled || points.empty()) {
        return clampedFallback;
    }
    const double clamped = std::clamp(normalizedTimeline, 0.0, 1.0);
    if (points.size() == 1) {
        return std::clamp(points.front().value, -1.0f, 1.0f);
    }
    if (clamped <= points.front().timeline_position) {
        return std::clamp(points.front().value, -1.0f, 1.0f);
    }
    if (clamped >= points.back().timeline_position) {
        return std::clamp(points.back().value, -1.0f, 1.0f);
    }
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const auto& left = points[i];
        const auto& right = points[i + 1];
        if (clamped >= left.timeline_position && clamped <= right.timeline_position) {
            const double span = std::max(0.000001, right.timeline_position - left.timeline_position);
            double t = std::clamp((clamped - left.timeline_position) / span, 0.0, 1.0);
            const auto shapeIt = std::find_if(
                layer.doppler_segment_shapes.begin(),
                layer.doppler_segment_shapes.end(),
                [&left](const auto& shape) { return shape.left_point_id == left.point_id; });
            if (shapeIt != layer.doppler_segment_shapes.end()) {
                t = apply_doppler_curve(t, shapeIt->curve_type, shapeIt->curve_amount);
            }
            return std::clamp(static_cast<float>(left.value + (right.value - left.value) * t), -1.0f, 1.0f);
        }
    }
    return clampedFallback;
}

float doppler_gain_multiplier(
    float dopplerValue,
    const StreamingMixer::LayerState::DopplerSettings& settings
) {
    const float magnitude = std::clamp(std::abs(dopplerValue), 0.0f, 1.0f);
    const double edgeDb = std::clamp(static_cast<double>(settings.edge_gain_db), -70.0, 0.0);
    const double centerDb = std::clamp(static_cast<double>(settings.center_gain_db), -24.0, 12.0);
    const double db = centerDb + (edgeDb - centerDb) * std::pow(static_cast<double>(magnitude), 2.2);
    return static_cast<float>(std::pow(10.0, db / 20.0));
}

float doppler_pitch_ratio(
    float dopplerValue,
    const StreamingMixer::LayerState::DopplerSettings& settings
) {
    const float edgeSemitones = std::clamp(settings.edge_pitch_semitones, -24.0f, 0.0f);
    const float centerSemitones = std::clamp(settings.center_pitch_semitones, 0.0f, 24.0f);
    const float magnitude = std::clamp(std::abs(dopplerValue), 0.0f, 1.0f);
    const float semitones = edgeSemitones + (1.0f - magnitude) * (centerSemitones - edgeSemitones);
    return static_cast<float>(std::pow(2.0, static_cast<double>(semitones) / 12.0));
}

float random_between(std::minstd_rand& rng, float low, float high) {
    if (!(high > low)) {
        return low;
    }
    std::uniform_real_distribution<float> distribution(low, high);
    return distribution(rng);
}

float db_to_linear_gain(float db) {
    if (db <= -120.0f) {
        return 0.0f;
    }
    return static_cast<float>(std::pow(10.0, static_cast<double>(db) / 20.0));
}

float process_layer_eq_sample(StreamingMixer::LayerState& layer,
                              float sample,
                              int channel,
                              int sample_rate) {
    const auto safeChannel = static_cast<std::size_t>(
        std::clamp(channel, 0, static_cast<int>(layer.eq_low_state.size() - 1)));
    const float lowGain = db_to_linear_gain(std::clamp(
        layer.params->eq_low_gain_db.load(std::memory_order_relaxed), -24.0f, 12.0f));
    const float midGain = db_to_linear_gain(std::clamp(
        layer.params->eq_mid_gain_db.load(std::memory_order_relaxed), -24.0f, 12.0f));
    const float highGain = db_to_linear_gain(std::clamp(
        layer.params->eq_high_gain_db.load(std::memory_order_relaxed), -24.0f, 12.0f));
    const float lowAlpha =
        1.0f - std::exp(static_cast<float>(-2.0 * kPi * 240.0 / std::max(1, sample_rate)));
    const float highAlpha =
        1.0f - std::exp(static_cast<float>(-2.0 * kPi * 4000.0 / std::max(1, sample_rate)));

    auto& lowState = layer.eq_low_state[safeChannel];
    auto& highState = layer.eq_high_state[safeChannel];
    lowState += lowAlpha * (sample - lowState);
    highState += highAlpha * (sample - highState);

    const float low = lowState;
    const float high = sample - highState;
    const float mid = sample - low - high;
    return low * lowGain + mid * midGain + high * highGain;
}

float evaluate_volume_random_gain(
    StreamingMixer::LayerState& layer,
    int sample_rate
) {
    const auto& settings = layer.volume_random_settings;
    if (!settings.enabled) {
        return 1.0f;
    }

    auto& runtime = layer.volume_random_runtime;
    const float loudestDb = std::clamp(settings.loudest_db, 0.0f, 12.0f);
    const float quietestDb = std::clamp(settings.quietest_db, -70.0f, 0.0f);
    const float shortestSeconds = std::clamp(settings.period_shortest_seconds, 0.02f, 20.0f);
    const float longestSeconds = std::max(shortestSeconds,
                                          std::clamp(settings.period_longest_seconds, 0.02f, 20.0f));
    const float smoothing = std::clamp(settings.smoothing, 0.0f, 1.0f);
    const float rangeDb = std::max(0.0f, loudestDb - quietestDb);

    auto chooseTargetDb = [&](float currentDb) {
        if (rangeDb <= 0.001f) {
            return loudestDb;
        }
        const float minSeparation = std::min(rangeDb * 0.35f, std::max(3.0f, rangeDb * 0.15f));
        for (int attempt = 0; attempt < 8; ++attempt) {
            const float candidate = random_between(runtime.rng, quietestDb, loudestDb);
            if (std::abs(candidate - currentDb) >= minSeparation) {
                return candidate;
            }
        }
        return (std::abs(quietestDb - currentDb) > std::abs(loudestDb - currentDb))
            ? quietestDb
            : loudestDb;
    };

    auto beginSegment = [&]() {
        if (!runtime.initialized) {
            runtime.current_db = random_between(runtime.rng, quietestDb, loudestDb);
            runtime.initialized = true;
        } else {
            runtime.current_db = runtime.target_db;
        }
        runtime.start_db = runtime.current_db;
        runtime.target_db = chooseTargetDb(runtime.current_db);
        const float periodSeconds = random_between(runtime.rng, shortestSeconds, longestSeconds);
        runtime.segment_samples = std::max<std::size_t>(
            1, static_cast<std::size_t>(std::llround(periodSeconds * static_cast<float>(std::max(sample_rate, 1)))));
        runtime.segment_progress = 0;
    };

    if (!runtime.initialized || runtime.segment_progress >= runtime.segment_samples) {
        beginSegment();
    }

    const double denominator = static_cast<double>(std::max<std::size_t>(1, runtime.segment_samples));
    const double linearT = std::clamp(static_cast<double>(runtime.segment_progress) / denominator, 0.0, 1.0);
    const double smoothT = linearT * linearT * (3.0 - 2.0 * linearT);
    const double shapedT = linearT + (smoothT - linearT) * static_cast<double>(smoothing);
    const float currentDb = static_cast<float>(
        runtime.start_db + (runtime.target_db - runtime.start_db) * static_cast<float>(shapedT));
    runtime.current_db = currentDb;
    ++runtime.segment_progress;
    return db_to_linear_gain(currentDb);
}

std::pair<float, float> evaluate_pan_random_offset(
    StreamingMixer::LayerState& layer,
    int sample_rate
) {
    const auto& settings = layer.pan_random_settings;
    if (!settings.enabled) {
        return {0.0f, 0.0f};
    }

    auto& runtime = layer.pan_random_runtime;
    const float left = std::clamp(settings.farthest_left, -1.0f, 1.0f);
    const float right = std::max(left, std::clamp(settings.farthest_right, -1.0f, 1.0f));
    const float back = std::clamp(settings.farthest_back, -1.0f, 1.0f);
    const float front = std::max(back, std::clamp(settings.farthest_front, -1.0f, 1.0f));
    const float speed = std::clamp(settings.speed, 0.0f, 1.0f);
    const float smoothing = std::clamp(settings.smoothing, 0.0f, 1.0f);
    const float periodSeconds = 3.0f - speed * 2.88f;

    auto chooseTarget = [&](std::minstd_rand& rng, float low, float high, float current) {
        if (!(high > low)) {
            return low;
        }
        const float range = high - low;
        const float minSeparation = std::min(range * 0.3f, std::max(0.12f, range * 0.12f));
        for (int attempt = 0; attempt < 8; ++attempt) {
            const float candidate = random_between(rng, low, high);
            if (std::abs(candidate - current) >= minSeparation) {
                return candidate;
            }
        }
        return (std::abs(low - current) > std::abs(high - current)) ? low : high;
    };

    auto beginSegment = [&]() {
        if (!runtime.initialized) {
            runtime.current_x = random_between(runtime.rng, left, right);
            runtime.current_y = random_between(runtime.rng, back, front);
            runtime.initialized = true;
        } else {
            runtime.current_x = runtime.target_x;
            runtime.current_y = runtime.target_y;
        }
        runtime.start_x = runtime.current_x;
        runtime.start_y = runtime.current_y;
        runtime.target_x = chooseTarget(runtime.rng, left, right, runtime.current_x);
        runtime.target_y = chooseTarget(runtime.rng, back, front, runtime.current_y);
        runtime.segment_samples = std::max<std::size_t>(
            1, static_cast<std::size_t>(std::llround(periodSeconds * static_cast<float>(std::max(sample_rate, 1)))));
        runtime.segment_progress = 0;
    };

    if (!runtime.initialized || runtime.segment_progress >= runtime.segment_samples) {
        beginSegment();
    }

    const double denominator = static_cast<double>(std::max<std::size_t>(1, runtime.segment_samples));
    const double linearT = std::clamp(static_cast<double>(runtime.segment_progress) / denominator, 0.0, 1.0);
    const double smoothT = linearT * linearT * (3.0 - 2.0 * linearT);
    const double shapedT = linearT + (smoothT - linearT) * static_cast<double>(smoothing);
    const float currentX = static_cast<float>(runtime.start_x + (runtime.target_x - runtime.start_x) * shapedT);
    const float currentY = static_cast<float>(runtime.start_y + (runtime.target_y - runtime.start_y) * shapedT);
    runtime.current_x = currentX;
    runtime.current_y = currentY;
    ++runtime.segment_progress;
    return {currentX, currentY};
}

float evaluate_stretch_random_ratio(
    StreamingMixer::LayerState& layer,
    int sample_rate
) {
    const auto& settings = layer.stretch_random_settings;
    if (!settings.enabled) {
        return 1.0f;
    }

    auto& runtime = layer.stretch_random_runtime;
    const float lowestPercent = std::clamp(settings.lowest_percent, 1.0f, 800.0f);
    const float highestPercent = std::max(lowestPercent, std::clamp(settings.highest_percent, 1.0f, 800.0f));
    const float speed = std::clamp(settings.speed, 0.0f, 1.0f);
    const float smoothing = std::clamp(settings.smoothing, 0.0f, 1.0f);
    const float periodSeconds = 3.0f - speed * 2.88f;

    auto chooseTarget = [&](std::minstd_rand& rng, float low, float high, float current) {
        if (!(high > low)) {
            return low;
        }
        const float range = high - low;
        const float minSeparation = std::min(range * 0.3f, std::max(8.0f, range * 0.12f));
        for (int attempt = 0; attempt < 8; ++attempt) {
            const float candidate = random_between(rng, low, high);
            if (std::abs(candidate - current) >= minSeparation) {
                return candidate;
            }
        }
        return (std::abs(low - current) > std::abs(high - current)) ? low : high;
    };

    auto beginSegment = [&]() {
        if (!runtime.initialized) {
            runtime.current_ratio = std::clamp(random_between(runtime.rng, lowestPercent, highestPercent) / 100.0f,
                                               static_cast<float>(kStretchAutomationMinRatio),
                                               static_cast<float>(kStretchAutomationMaxRatio));
            runtime.start_ratio = runtime.current_ratio;
            runtime.target_ratio = runtime.current_ratio;
            runtime.initialized = true;
        } else {
            runtime.start_ratio = runtime.current_ratio;
            runtime.target_ratio = std::clamp(
                chooseTarget(runtime.rng, lowestPercent, highestPercent, runtime.current_ratio * 100.0f) / 100.0f,
                static_cast<float>(kStretchAutomationMinRatio),
                static_cast<float>(kStretchAutomationMaxRatio));
        }
        runtime.segment_samples = std::max<std::size_t>(
            1, static_cast<std::size_t>(std::llround(periodSeconds * static_cast<float>(std::max(sample_rate, 1)))));
        runtime.segment_progress = 0;
    };

    if (!runtime.initialized || runtime.segment_progress >= runtime.segment_samples) {
        beginSegment();
    }

    const double denominator = static_cast<double>(std::max<std::size_t>(1, runtime.segment_samples));
    const double linearT = std::clamp(static_cast<double>(runtime.segment_progress) / denominator, 0.0, 1.0);
    const double smoothT = linearT * linearT * (3.0 - 2.0 * linearT);
    const double shapedT = linearT + (smoothT - linearT) * static_cast<double>(smoothing);
    const float currentRatio = static_cast<float>(
        runtime.start_ratio + (runtime.target_ratio - runtime.start_ratio) * static_cast<float>(shapedT));
    runtime.current_ratio = currentRatio;
    ++runtime.segment_progress;
    return std::clamp(currentRatio,
                      static_cast<float>(kStretchAutomationMinRatio),
                      static_cast<float>(kStretchAutomationMaxRatio));
}

bool layout_has_lfe(int channel_count) {
    return channel_count == kSurround51ChannelCount ||
           channel_count == kSurround71ChannelCount;
}

void add_surround5_sample(float* target,
                          std::size_t frame,
                          int channel_count,
                          const Surround51Gains& gains,
                          float sample) {
    const std::size_t base = frame * static_cast<std::size_t>(channel_count);
    target[base + kChannelL] += sample * gains.left;
    target[base + kChannelR] += sample * gains.right;
    target[base + kChannelC] += sample * gains.center;
    if (channel_count == kSurround50ChannelCount) {
        target[base + kChannelLs50] += sample * gains.left_surround;
        target[base + kChannelRs50] += sample * gains.right_surround;
    } else {
        target[base + kChannelLs51] += sample * gains.left_surround;
        target[base + kChannelRs51] += sample * gains.right_surround;
    }
}

float lfe_feed_from_51(const Surround51Gains& gains, float sample) {
    return sample * (gains.left +
                     gains.right +
                     gains.center +
                     gains.left_surround +
                     gains.right_surround) / 5.0f;
}

float lfe_feed_from_70(const Surround70Gains& gains, float sample) {
    return sample * (gains.left +
                     gains.right +
                     gains.center +
                     gains.left_side +
                     gains.right_side +
                     gains.left_rear +
                     gains.right_rear) / 7.0f;
}

void add_surround7_sample(float* target,
                          std::size_t frame,
                          int channel_count,
                          const Surround70Gains& gains,
                          float sample) {
    const std::size_t base = frame * static_cast<std::size_t>(channel_count);
    target[base + kChannelL] += sample * gains.left;
    target[base + kChannelR] += sample * gains.right;
    target[base + kChannelC] += sample * gains.center;
    if (channel_count == kSurround70ChannelCount) {
        target[base + kChannelLs70] += sample * gains.left_side;
        target[base + kChannelRs70] += sample * gains.right_side;
        target[base + kChannelLrs70] += sample * gains.left_rear;
        target[base + kChannelRrs70] += sample * gains.right_rear;
    } else {
        target[base + kChannelLs71] += sample * gains.left_side;
        target[base + kChannelRs71] += sample * gains.right_side;
        target[base + kChannelLrs71] += sample * gains.left_rear;
        target[base + kChannelRrs71] += sample * gains.right_rear;
    }
}

std::pair<float, float> fold_frame_to_stereo(const float* frame, int channel_count) {
    constexpr float k = 0.7071067811865476f;
    float left = frame[kChannelL] + k * frame[kChannelC];
    float right = frame[kChannelR] + k * frame[kChannelC];
    if (channel_count == kSurround70ChannelCount) {
        left += k * frame[kChannelLs70];
        right += k * frame[kChannelRs70];
        left += k * frame[kChannelLrs70];
        right += k * frame[kChannelRrs70];
    } else if (channel_count == kSurround71ChannelCount) {
        left += k * frame[kChannelLs71];
        right += k * frame[kChannelRs71];
        left += k * frame[kChannelLrs71];
        right += k * frame[kChannelRrs71];
    } else if (channel_count == kSurround50ChannelCount) {
        left += k * frame[kChannelLs50];
        right += k * frame[kChannelRs50];
    } else {
        left += k * frame[kChannelLs51];
        right += k * frame[kChannelRs51];
    }
    return {std::clamp(left, -1.0f, 1.0f), std::clamp(right, -1.0f, 1.0f)};
}

}  // namespace

void StreamingMixer::prepare(int sample_rate) {
    sample_rate_ = sample_rate;
    playing_ = false;
    any_solo_ = false;
    for (auto& level : aux_.meter_levels) {
        level.store(0.0f, std::memory_order_relaxed);
    }
    aux_.meter_channel_count.store(2, std::memory_order_relaxed);
    lfe_lowpass_state_ = 0.0f;
    aux_bass_lowpass_state_.fill(0.0f);
}

void StreamingMixer::set_layers(std::vector<LayerState>* layers) {
    layers_ = layers;
    if (layers_ != nullptr && !layers_->empty()) {
        playing_ = true;
        update_solo_flag();
    } else {
        playing_ = false;
    }
}

void StreamingMixer::set_record_bus_surround_enabled(bool enabled) {
    set_record_bus_channel_count(enabled ? kSurround51ChannelCount : kStereoChannelCount);
}

void StreamingMixer::set_record_bus_channel_count(int channel_count) {
    int normalized = kStereoChannelCount;
    if (channel_count >= kSurround71ChannelCount) {
        normalized = kSurround71ChannelCount;
    } else if (channel_count >= kSurround70ChannelCount) {
        normalized = kSurround70ChannelCount;
    } else if (channel_count >= kSurround51ChannelCount) {
        normalized = kSurround51ChannelCount;
    } else if (channel_count >= kSurround50ChannelCount) {
        normalized = kSurround50ChannelCount;
    }
    record_bus_channels_.store(normalized, std::memory_order_relaxed);
    if (!layout_has_lfe(normalized)) {
        lfe_lowpass_state_ = 0.0f;
    }
    aux_bass_lowpass_state_.fill(0.0f);
}

bool StreamingMixer::record_bus_surround_enabled() const {
    return record_bus_channels_.load(std::memory_order_relaxed) > kStereoChannelCount;
}

int StreamingMixer::record_bus_channel_count() const {
    return record_bus_channels_.load(std::memory_order_relaxed);
}

void StreamingMixer::stop() {
    playing_.store(false, std::memory_order_seq_cst);
    // Wait for audio thread to finish any in-progress render_block() call
    // before we allow the caller to free/clear the layers vector.
    // rendering_ is set BEFORE playing_ is checked in render_block(),
    // so if the audio thread is about to access layers_, we'll see it here.
    int spins = 0;
    while (rendering_.load(std::memory_order_seq_cst)) {
        if (++spins > 1000000) {
            // Safety valve: don't deadlock the UI thread forever
            break;
        }
        std::this_thread::yield();
    }
    layers_ = nullptr;
}

bool StreamingMixer::is_playing() const {
    return playing_.load(std::memory_order_relaxed);
}

void StreamingMixer::update_solo_flag() {
    if (layers_ == nullptr) {
        any_solo_ = false;
        return;
    }
    bool found_solo = false;
    for (const auto& layer : *layers_) {
        if (layer.params->active.load(std::memory_order_relaxed) &&
            layer.params->solo.load(std::memory_order_relaxed) &&
            !layer.params->mute.load(std::memory_order_relaxed)) {
            found_solo = true;
            break;
        }
    }
    any_solo_.store(found_solo, std::memory_order_relaxed);
}

float StreamingMixer::read_sample(const Voice& voice, int channel) const {
    if (voice.buffer == nullptr || voice.finished) {
        return 0.0f;
    }
    return read_buffer_sample(*voice.buffer, voice.position, channel);
}

void StreamingMixer::advance_voice(Voice& voice) {
    advance_voice_step(voice, voice.pitch_ratio);
}

void StreamingMixer::advance_voice_step(Voice& voice, double step_size) {
    if (voice.finished || voice.buffer == nullptr) {
        return;
    }

    // Handle delay
    if (voice.delay_remaining > 0) {
        --voice.delay_remaining;
        ++voice.samples_elapsed;
        return;
    }

    // Advance position
    const double step = voice.reverse ? -step_size : step_size;
    voice.position += step;
    ++voice.samples_elapsed;

    auto choose_next_loop_region = [this, &voice]() -> bool {
        if (voice.loop_regions.empty() || voice.buffer == nullptr) {
            return false;
        }

        std::uniform_int_distribution<int> region_dist(0, static_cast<int>(voice.loop_regions.size() - 1));
        int region_index = region_dist(rng_);
        if (voice.loop_regions.size() > 1 && region_index == voice.last_loop_region_index) {
            region_index = (region_index + 1) % static_cast<int>(voice.loop_regions.size());
        }
        voice.last_loop_region_index = region_index;
        voice.region = voice.loop_regions[static_cast<std::size_t>(region_index)];

        const auto frames = voice.buffer->frame_count();
        if (frames == 0) {
            return false;
        }

        const double region_start =
            std::clamp(voice.region.start, 0.0, 1.0);
        const double region_end =
            std::clamp(voice.region.end, 0.0, 1.0);
        voice.start_frame = std::min<std::size_t>(
            static_cast<std::size_t>(region_start * frames), frames - 1);
        voice.end_frame = std::max(
            voice.start_frame + 1,
            std::min(static_cast<std::size_t>(region_end * frames), frames));

        voice.loop_enabled = false;
        voice.loop_start_frame = std::clamp(voice.region.loop_start, 0.0, 1.0) * frames;
        voice.loop_end_frame = std::clamp(voice.region.loop_end, 0.0, 1.0) * frames;
        voice.position = voice.reverse
            ? static_cast<double>(voice.end_frame - 1)
            : static_cast<double>(voice.start_frame);
        return true;
    };

    // Check bounds and looping
    if (!voice.reverse) {
        if (voice.position >= static_cast<double>(voice.end_frame)) {
            if (voice.loop_enabled && !voice.released &&
                voice.loop_end_frame > voice.loop_start_frame) {
                voice.position = voice.loop_start_frame;
            } else if (voice.continuous && !voice.released && voice.loop_retrigger_enabled &&
                       choose_next_loop_region()) {
                // Region chain looping handled above.
            } else {
                voice.finished = true;
            }
        }
    } else {
        if (voice.position < static_cast<double>(voice.start_frame)) {
            if (voice.loop_enabled && !voice.released &&
                voice.loop_end_frame > voice.loop_start_frame) {
                voice.position = voice.loop_end_frame - 1.0;
            } else if (voice.continuous && !voice.released && voice.loop_retrigger_enabled &&
                       choose_next_loop_region()) {
                // Region chain looping handled above.
            } else {
                voice.finished = true;
            }
        }
    }

    // Check release completion for continuous mode
    if (voice.continuous && voice.released) {
        const std::size_t release_samples = static_cast<std::size_t>(
            std::max(0.0, voice.envelope.release_seconds) * sample_rate_
        );
        const std::size_t samples_since_release = voice.samples_elapsed > voice.hold_samples
            ? voice.samples_elapsed - voice.hold_samples
            : 0;
        if (samples_since_release >= release_samples) {
            voice.finished = true;
        }
    }
}

void StreamingMixer::render_block(float* output, std::size_t frame_count, int device_output_channels) {
    const int safeDeviceChannels = std::max(1, device_output_channels);
    std::memset(output, 0, frame_count * static_cast<std::size_t>(safeDeviceChannels) * sizeof(float));

    rendering_.store(true, std::memory_order_seq_cst);

    const bool is_playing = playing_.load(std::memory_order_seq_cst);
    const bool is_recording = recording_.load(std::memory_order_relaxed);
    const int render_channels = std::clamp(
        record_bus_channels_.load(std::memory_order_relaxed),
        kStereoChannelCount,
        kMaxRecordBusChannelCount);
    const bool renderHasLfe = layout_has_lfe(render_channels);

    if ((!is_playing || layers_ == nullptr) && !is_recording) {
        for (auto& level : aux_.meter_levels) {
            level.store(0.0f, std::memory_order_relaxed);
        }
        aux_.meter_channel_count.store(render_channels, std::memory_order_relaxed);
        rendering_.store(false, std::memory_order_seq_cst);
        return;
    }

    std::vector<float> record_mix(frame_count * static_cast<std::size_t>(render_channels), 0.0f);
    std::vector<float> direct_mix(frame_count * static_cast<std::size_t>(render_channels), 0.0f);
    std::vector<float> lfe_feed_mix(renderHasLfe ? frame_count : 0, 0.0f);

    if (!is_playing || layers_ == nullptr) {
        for (auto& level : aux_.meter_levels) {
            level.store(0.0f, std::memory_order_relaxed);
        }
        aux_.meter_channel_count.store(render_channels, std::memory_order_relaxed);
        rendering_.store(false, std::memory_order_seq_cst);
        if (is_recording) {
            std::lock_guard<std::mutex> lock(record_mutex_);
            record_buffer_.insert(record_buffer_.end(), record_mix.begin(), record_mix.end());
        }
        return;
    }

    const bool solo_active = any_solo_.load(std::memory_order_relaxed);
    bool any_voice_alive = false;

    constexpr std::size_t kStackFrames = 2048;
    float stack_buf[kStackFrames * kStereoChannelCount];
    std::vector<float> heap_buf;

    for (auto& layer : *layers_) {
        auto& voice = layer.voice;
        if (voice.finished || voice.buffer == nullptr) {
            layer.params->playback_frame.store(-1.0, std::memory_order_relaxed);
            continue;
        }

        const bool muted = layer.params->mute.load(std::memory_order_relaxed);
        const bool soloed = layer.params->solo.load(std::memory_order_relaxed);
        const bool active = layer.params->active.load(std::memory_order_relaxed);
        const float gain = layer.params->gain.load(std::memory_order_relaxed);
        const float basePx  = layer.params->pan_x.load(std::memory_order_relaxed);
        const float basePy  = layer.params->pan_y.load(std::memory_order_relaxed);
        const float basePxr = layer.params->pan_x_right.load(std::memory_order_relaxed);
        const float basePyr = layer.params->pan_y_right.load(std::memory_order_relaxed);
        const float bassLfeGain = db_to_linear_gain(std::clamp(
            layer.params->bass_lfe_gain_db.load(std::memory_order_relaxed),
            -24.0f,
            12.0f));
        if (!active || muted || (solo_active && !soloed)) {
            for (std::size_t frame = 0; frame < frame_count; ++frame) {
                const double normalizedTimeline =
                    (voice.buffer != nullptr && voice.buffer->frame_count() > 1)
                        ? std::clamp(voice.position / static_cast<double>(voice.buffer->frame_count() - 1), 0.0, 1.0)
                        : 0.0;
                const float stretch_ratio =
                    evaluate_stretch_ratio(layer, normalizedTimeline) * evaluate_stretch_random_ratio(layer, sample_rate_);
                advance_voice_step(voice, voice.pitch_ratio * static_cast<double>(stretch_ratio));
            }
            if (!voice.finished) {
                any_voice_alive = true;
            }
            layer.params->playback_frame.store(voice.finished ? -1.0 : voice.position, std::memory_order_relaxed);
            continue;
        }

        bool has_plugins = false;
        for (auto* session : layer.plugin_sessions) {
            if (session != nullptr) { has_plugins = true; break; }
        }
        const bool can_process_plugins = has_plugins && render_channels == kStereoChannelCount;
        float* layer_buf = stack_buf;
        if (can_process_plugins) {
            if (frame_count > kStackFrames) {
                heap_buf.resize(frame_count * kStereoChannelCount);
                layer_buf = heap_buf.data();
            }
            std::memset(layer_buf, 0, frame_count * kStereoChannelCount * sizeof(float));
        }

        float* bus_target = layer.route_to_record_bus ? record_mix.data() : direct_mix.data();
        float* mix_target = can_process_plugins ? layer_buf : bus_target;
        const bool is_stereo = voice.buffer != nullptr && voice.buffer->channels > 1 && voice.buffer->channels < kSurround50ChannelCount;
        const bool is_raw_surround = voice.buffer != nullptr
            && voice.buffer->channels >= kSurround50ChannelCount
            && !layer.route_to_record_bus
            && render_channels >= kSurround50ChannelCount;

        for (std::size_t frame = 0; frame < frame_count; ++frame) {
            if (voice.finished) {
                break;
            }

            const bool released = voice.continuous && voice.released;
            const std::size_t note_off = voice.continuous ? voice.hold_samples : 0;
            const double env = compute_envelope(
                voice.envelope, voice.samples_elapsed, note_off, released, sample_rate_
            );

            if (env <= 0.0 && released) {
                voice.finished = true;
                break;
            }

            const double normalizedTimeline =
                (voice.buffer != nullptr && voice.buffer->frame_count() > 1)
                    ? std::clamp(voice.position / static_cast<double>(voice.buffer->frame_count() - 1), 0.0, 1.0)
                    : 0.0;

            const float px = evaluate_pan_points(layer.pan_position_automation_points,
                                                 layer.pan_position_automation_enabled,
                                                 normalizedTimeline,
                                                 basePx);
            const float py = evaluate_pan_points(layer.pan_front_back_automation_points,
                                                 layer.pan_front_back_automation_enabled,
                                                 normalizedTimeline,
                                                 basePy);
            const float pxr = evaluate_pan_points(layer.pan_right_position_automation_points,
                                                  layer.pan_right_position_automation_enabled,
                                                  normalizedTimeline,
                                                  basePxr);
            const float pyr = evaluate_pan_points(layer.pan_right_front_back_automation_points,
                                                  layer.pan_right_front_back_automation_enabled,
                                                  normalizedTimeline,
                                                  basePyr);
            const auto [randomPanX, randomPanY] = evaluate_pan_random_offset(layer, sample_rate_);
            const float doppler = evaluate_doppler_points(layer, normalizedTimeline, 0.0f);
            const float dopplerGain = doppler_gain_multiplier(doppler, layer.doppler_settings);
            const float dopplerPitch = doppler_pitch_ratio(doppler, layer.doppler_settings);
            const float randomVolumeGain = evaluate_volume_random_gain(layer, sample_rate_);
            const auto env_f = static_cast<float>(env) * gain * dopplerGain * randomVolumeGain;

            if (is_raw_surround) {
                if (voice.delay_remaining == 0) {
                    const std::size_t base = frame * static_cast<std::size_t>(render_channels);
                    for (int channel = 0; channel < render_channels; ++channel) {
                        const float eqSample = process_layer_eq_sample(
                            layer, read_sample(voice, channel), channel, sample_rate_);
                        mix_target[base + static_cast<std::size_t>(channel)] += eqSample * env_f;
                    }
                }
            } else {
                const float basePxCenter = is_stereo ? 0.5f * (px + pxr) : px;
                const float basePyCenter = is_stereo ? 0.5f * (py + pyr) : py;
                const float halfWidth = is_stereo ? 0.5f * (pxr - px) : 0.0f;
                const float halfDepth = is_stereo ? 0.5f * (pyr - py) : 0.0f;
                const float dopplerStereoCollapse = 1.0f - std::clamp(std::abs(doppler), 0.0f, 1.0f);
                const float effectiveHalfWidth = halfWidth * dopplerStereoCollapse;
                const float effectiveHalfDepth = halfDepth * dopplerStereoCollapse;
                const float effectiveCenter = std::clamp(basePxCenter + doppler + randomPanX, -1.0f, 1.0f);
                const float effectivePyCenter = std::clamp(basePyCenter + randomPanY, -1.0f, 1.0f);
                const float effectivePx = std::clamp(effectiveCenter - effectiveHalfWidth, -1.0f, 1.0f);
                const float effectivePxr = is_stereo
                    ? std::clamp(effectiveCenter + effectiveHalfWidth, -1.0f, 1.0f)
                    : effectivePx;
                const float effectivePy = std::clamp(effectivePyCenter - effectiveHalfDepth, -1.0f, 1.0f);
                const float effectivePyr = is_stereo
                    ? std::clamp(effectivePyCenter + effectiveHalfDepth, -1.0f, 1.0f)
                    : effectivePy;

                float sample_l = 0.0f;
                float sample_r = 0.0f;
                if (voice.delay_remaining == 0) {
                    sample_l = process_layer_eq_sample(layer, read_sample(voice, 0), 0, sample_rate_);
                    const int right_channel = voice.buffer->channels > 1 ? 1 : 0;
                    sample_r = process_layer_eq_sample(layer, read_sample(voice, right_channel), right_channel, sample_rate_);
                }

                if (render_channels >= kSurround70ChannelCount) {
                    const auto gainsLeft = surround_70_gains(effectivePx, effectivePy);
                    const auto gainsRight = is_stereo
                        ? surround_70_gains(effectivePxr, effectivePyr)
                        : gainsLeft;
                    const float leftSample = sample_l * env_f;
                    add_surround7_sample(mix_target, frame, render_channels, gainsLeft, leftSample);
                    if (layer.route_to_record_bus && !lfe_feed_mix.empty()) {
                        lfe_feed_mix[frame] += lfe_feed_from_70(gainsLeft, leftSample) * bassLfeGain;
                    }
                    if (is_stereo) {
                        const float rightSample = sample_r * env_f;
                        add_surround7_sample(mix_target, frame, render_channels, gainsRight, rightSample);
                        if (layer.route_to_record_bus && !lfe_feed_mix.empty()) {
                            lfe_feed_mix[frame] += lfe_feed_from_70(gainsRight, rightSample) * bassLfeGain;
                        }
                    }
                } else if (render_channels >= kSurround50ChannelCount) {
                    const auto gainsLeft = fold_70_to_51(surround_70_gains(effectivePx, effectivePy));
                    const auto gainsRight = is_stereo
                        ? fold_70_to_51(surround_70_gains(effectivePxr, effectivePyr))
                        : gainsLeft;
                    const float leftSample = sample_l * env_f;
                    add_surround5_sample(mix_target, frame, render_channels, gainsLeft, leftSample);
                    if (layer.route_to_record_bus && !lfe_feed_mix.empty()) {
                        lfe_feed_mix[frame] += lfe_feed_from_51(gainsLeft, leftSample) * bassLfeGain;
                    }
                    if (is_stereo) {
                        const float rightSample = sample_r * env_f;
                        add_surround5_sample(mix_target, frame, render_channels, gainsRight, rightSample);
                        if (layer.route_to_record_bus && !lfe_feed_mix.empty()) {
                            lfe_feed_mix[frame] += lfe_feed_from_51(gainsRight, rightSample) * bassLfeGain;
                        }
                    }
                } else {
                    const auto [l_gain_l, r_gain_l] = surround_folddown(effectivePx, effectivePy);
                    const auto [l_gain_r, r_gain_r] = is_stereo
                        ? surround_folddown(effectivePxr, effectivePyr)
                        : std::pair<float, float>{l_gain_l, r_gain_l};
                    const std::size_t base = frame * static_cast<std::size_t>(kStereoChannelCount);
                    if (is_stereo) {
                        mix_target[base] += (sample_l * l_gain_l + sample_r * l_gain_r) * env_f;
                        mix_target[base + 1] += (sample_l * r_gain_l + sample_r * r_gain_r) * env_f;
                    } else {
                        mix_target[base] += sample_l * l_gain_l * env_f;
                        mix_target[base + 1] += sample_l * r_gain_l * env_f;
                    }
                }
            }

            const float stretch_ratio =
                evaluate_stretch_ratio(layer, normalizedTimeline) *
                evaluate_stretch_random_ratio(layer, sample_rate_) *
                (is_raw_surround ? 1.0f : dopplerPitch);
            const double effective_step = voice.pitch_ratio * static_cast<double>(stretch_ratio);
            advance_voice_step(voice, effective_step);
        }

        if (can_process_plugins) {
            for (std::size_t si = 0; si < layer.plugin_sessions.size(); ++si) {
                if (layer.plugin_sessions[si] != nullptr && !layer.plugin_bypassed[si]) {
                    layer.plugin_sessions[si]->process_block(layer_buf, static_cast<int>(frame_count), sample_rate_);
                }
            }
            for (std::size_t i = 0; i < frame_count * static_cast<std::size_t>(kStereoChannelCount); ++i) {
                bus_target[i] += layer_buf[i];
            }
        }

        if (!voice.finished) {
            any_voice_alive = true;
        }
        layer.params->playback_frame.store(voice.finished ? -1.0 : voice.position, std::memory_order_relaxed);
    }

    if (render_channels == kStereoChannelCount) {
        for (std::size_t si = 0; si < aux_.plugin_sessions.size(); ++si) {
            if (aux_.plugin_sessions[si] != nullptr && !aux_.plugin_bypassed[si]) {
                aux_.plugin_sessions[si]->process_block(record_mix.data(), static_cast<int>(frame_count), sample_rate_);
            }
        }
    }

    const float auxGain = aux_.gain.load(std::memory_order_relaxed);
    const float auxBassGain = db_to_linear_gain(std::clamp(
        aux_.bass_gain_db.load(std::memory_order_relaxed),
        -24.0f,
        12.0f));
    const float lfeAlpha =
        1.0f - std::exp(static_cast<float>(-2.0 * kPi * 90.0 / std::max(1, sample_rate_)));
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const std::size_t base = frame * static_cast<std::size_t>(render_channels);
        for (int channel = 0; channel < render_channels; ++channel) {
            record_mix[base + static_cast<std::size_t>(channel)] *= auxGain;
        }
        if (renderHasLfe) {
            const float lfeFeed = !lfe_feed_mix.empty()
                ? lfe_feed_mix[frame] * auxGain
                : 0.0f;
            lfe_lowpass_state_ += lfeAlpha * (lfeFeed - lfe_lowpass_state_);
            record_mix[base + kChannelLfe] = lfe_lowpass_state_;
        }
        for (int channel = 0; channel < render_channels; ++channel) {
            auto& sample = record_mix[base + static_cast<std::size_t>(channel)];
            auto& lowpassState = aux_bass_lowpass_state_[static_cast<std::size_t>(channel)];
            lowpassState += lfeAlpha * (sample - lowpassState);
            sample = (sample - lowpassState) + (lowpassState * auxBassGain);
            sample = std::clamp(sample, -1.0f, 1.0f);
        }
    }

    if (recording_.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(record_mutex_);
        record_buffer_.insert(record_buffer_.end(), record_mix.begin(), record_mix.end());
    }

    std::array<float, kMaxRecordBusChannelCount> meterLevels{};
    std::vector<float> final_mix(frame_count * static_cast<std::size_t>(render_channels), 0.0f);
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const std::size_t base = frame * static_cast<std::size_t>(render_channels);
        if (render_channels > kStereoChannelCount) {
            for (int channel = 0; channel < render_channels; ++channel) {
                final_mix[base + static_cast<std::size_t>(channel)] =
                    std::clamp(record_mix[base + static_cast<std::size_t>(channel)] +
                                   direct_mix[base + static_cast<std::size_t>(channel)],
                               -1.0f,
                               1.0f);
                meterLevels[static_cast<std::size_t>(channel)] = std::max(
                    meterLevels[static_cast<std::size_t>(channel)],
                    std::abs(final_mix[base + static_cast<std::size_t>(channel)]));
            }
        } else {
            final_mix[base] = std::clamp(record_mix[base] + direct_mix[base], -1.0f, 1.0f);
            final_mix[base + 1] = std::clamp(record_mix[base + 1] + direct_mix[base + 1], -1.0f, 1.0f);
            meterLevels[0] = std::max(meterLevels[0], std::abs(final_mix[base]));
            meterLevels[1] = std::max(meterLevels[1], std::abs(final_mix[base + 1]));
        }
    }
    for (int channel = 0; channel < kMaxRecordBusChannelCount; ++channel) {
        const float level = channel < render_channels ? meterLevels[static_cast<std::size_t>(channel)] : 0.0f;
        aux_.meter_levels[static_cast<std::size_t>(channel)].store(level, std::memory_order_relaxed);
    }
    aux_.meter_channel_count.store(render_channels, std::memory_order_relaxed);

    if (render_channels > kStereoChannelCount && safeDeviceChannels >= render_channels) {
        std::copy(final_mix.begin(), final_mix.end(), output);
    } else if (render_channels > kStereoChannelCount) {
        for (std::size_t frame = 0; frame < frame_count; ++frame) {
            const auto [left, right] =
                fold_frame_to_stereo(final_mix.data() + frame * static_cast<std::size_t>(render_channels), render_channels);
            const std::size_t outBase = frame * static_cast<std::size_t>(safeDeviceChannels);
            output[outBase] = left;
            if (safeDeviceChannels > 1) {
                output[outBase + 1] = right;
            }
        }
    } else {
        for (std::size_t frame = 0; frame < frame_count; ++frame) {
            const std::size_t inBase = frame * static_cast<std::size_t>(kStereoChannelCount);
            const std::size_t outBase = frame * static_cast<std::size_t>(safeDeviceChannels);
            output[outBase] = final_mix[inBase];
            if (safeDeviceChannels > 1) {
                output[outBase + 1] = final_mix[inBase + 1];
            }
        }
    }

    if (!any_voice_alive) {
        playing_.store(false, std::memory_order_relaxed);
    }

    rendering_.store(false, std::memory_order_seq_cst);
}

void StreamingMixer::set_recording(bool enabled) {
    if (enabled) {
        std::lock_guard<std::mutex> lock(record_mutex_);
        record_buffer_.clear();
        last_recording_channels_ = std::clamp(
            record_bus_channels_.load(std::memory_order_relaxed),
            kStereoChannelCount,
            kMaxRecordBusChannelCount);
    }
    recording_.store(enabled, std::memory_order_relaxed);
}

void StreamingMixer::set_aux_gain(float gain) {
    aux_.gain.store(std::clamp(gain, 0.0f, 2.0f), std::memory_order_relaxed);
}

void StreamingMixer::set_aux_bass_gain_db(float gain_db) {
    aux_.bass_gain_db.store(std::clamp(gain_db, -24.0f, 12.0f), std::memory_order_relaxed);
}

void StreamingMixer::set_aux_plugin_session(std::size_t slot_index,
                                            PluginHostSession* session,
                                            bool bypassed) {
    if (slot_index >= aux_.plugin_sessions.size()) {
        return;
    }
    aux_.plugin_sessions[slot_index] = session;
    aux_.plugin_bypassed[slot_index] = bypassed;
}

void StreamingMixer::clear_aux_plugin_sessions() {
    for (std::size_t i = 0; i < aux_.plugin_sessions.size(); ++i) {
        aux_.plugin_sessions[i] = nullptr;
        aux_.plugin_bypassed[i] = false;
    }
    for (auto& level : aux_.meter_levels) {
        level.store(0.0f, std::memory_order_relaxed);
    }
    aux_.meter_channel_count.store(2, std::memory_order_relaxed);
}

void StreamingMixer::aux_meter_levels(float& left, float& right) const {
    left = aux_.meter_levels[0].load(std::memory_order_relaxed);
    right = aux_.meter_levels[1].load(std::memory_order_relaxed);
}

void StreamingMixer::aux_meter_levels(std::vector<float>& levels) const {
    const int channelCount = std::clamp(
        aux_.meter_channel_count.load(std::memory_order_relaxed),
        kStereoChannelCount,
        kMaxRecordBusChannelCount);
    levels.assign(static_cast<std::size_t>(channelCount), 0.0f);
    for (int i = 0; i < channelCount; ++i) {
        levels[static_cast<std::size_t>(i)] =
            aux_.meter_levels[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
    }
}

std::optional<RenderedAudio> StreamingMixer::take_recording() {
    recording_ = false;
    std::lock_guard<std::mutex> lock(record_mutex_);
    if (record_buffer_.empty()) {
        return std::nullopt;
    }
    RenderedAudio audio;
    audio.sample_rate = sample_rate_;
    audio.channels = last_recording_channels_;
    audio.samples = std::move(record_buffer_);
    record_buffer_.clear();
    return audio;
}

bool StreamingMixer::is_recording() const {
    return recording_.load(std::memory_order_relaxed);
}

std::size_t StreamingMixer::recording_frame_count() const {
    std::lock_guard<std::mutex> lock(record_mutex_);
    return record_buffer_.size() / static_cast<std::size_t>(std::max(1, last_recording_channels_));
}

void StreamingMixer::recording_peaks(std::vector<float>& peaks_l, std::vector<float>& peaks_r,
                                     std::size_t bucket_count) const {
    recording_peaks_for_range(peaks_l, peaks_r, 0, recording_frame_count(), bucket_count);
}

void StreamingMixer::recording_channel_peaks(std::vector<std::vector<float>>& peaks_by_channel,
                                             std::size_t bucket_count) const {
    recording_channel_peaks_for_range(peaks_by_channel, 0, recording_frame_count(), bucket_count);
}

void StreamingMixer::recording_peaks_for_range(std::vector<float>& peaks_l, std::vector<float>& peaks_r,
                                               std::size_t start_frame, std::size_t end_frame,
                                               std::size_t bucket_count) const {
    std::vector<std::vector<float>> peaksByChannel;
    recording_channel_peaks_for_range(peaksByChannel, start_frame, end_frame, bucket_count);
    peaks_l = !peaksByChannel.empty() ? peaksByChannel[0] : std::vector<float>(bucket_count, 0.0f);
    peaks_r = peaksByChannel.size() > 1 ? peaksByChannel[1] : peaks_l;
}

void StreamingMixer::recording_channel_peaks_for_range(std::vector<std::vector<float>>& peaks_by_channel,
                                                       std::size_t start_frame,
                                                       std::size_t end_frame,
                                                       std::size_t bucket_count) const {
    // Snapshot the buffer under lock to minimize contention with the audio thread.
    // Only copy — compute peaks outside the lock.
    std::vector<float> snapshot;
    {
        std::lock_guard<std::mutex> lock(record_mutex_);
        snapshot = record_buffer_;
    }
    const std::size_t stride = static_cast<std::size_t>(std::max(1, last_recording_channels_));
    const std::size_t total_frames = snapshot.size() / stride;
    peaks_by_channel.assign(stride, std::vector<float>(bucket_count, 0.0f));
    if (total_frames == 0 || bucket_count == 0) {
        return;
    }
    const std::size_t clamped_start = std::min(start_frame, total_frames);
    const std::size_t clamped_end = std::min(std::max(end_frame, clamped_start), total_frames);
    const std::size_t range_frames = clamped_end - clamped_start;
    if (range_frames == 0) {
        return;
    }
    for (std::size_t b = 0; b < bucket_count; ++b) {
        const std::size_t start = clamped_start + (b * range_frames / bucket_count);
        const std::size_t end = clamped_start + ((b + 1) * range_frames / bucket_count);
        for (std::size_t f = start; f < end; ++f) {
            const std::size_t base = f * stride;
            for (std::size_t channel = 0; channel < stride; ++channel) {
                peaks_by_channel[channel][b] = std::max(
                    peaks_by_channel[channel][b],
                    std::abs(snapshot[base + channel]));
            }
        }
    }
}

}  // namespace radium
