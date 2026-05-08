#include "playback_engine.h"
#include "surround_panner.h"
#include <cstring>
#include "signalsmith-stretch.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace radium {
namespace {

constexpr double kPi = 3.14159265358979323846;

struct VoicePlan {
    const PlaybackLayer* layer = nullptr;
    const AudioBuffer* buffer = nullptr;
    PlaybackRegion region;
    double gain = 1.0;
    double pan_x = 0.0;
    double pan_y = 1.0;
    double pan_x_right = 0.0;
    double pan_y_right = 1.0;
    std::size_t delay_samples = 0;
    double pitch_ratio = 1.0;
    std::size_t start_frame = 0;
    std::size_t end_frame = 0;
};

std::vector<float> apply_time_stretch(const std::vector<float>& input, double ratio) {
    if (input.empty() || std::abs(ratio - 1.0) < 0.001) {
        return input;
    }

    const double clamped_ratio = std::clamp(ratio, 0.25, 4.0);
    const std::size_t grain_size = 1024;
    const std::size_t overlap = 256;
    const std::size_t output_hop = grain_size - overlap;
    const double input_hop = static_cast<double>(output_hop) / clamped_ratio;
    const std::size_t estimated_size =
        std::max<std::size_t>(grain_size, static_cast<std::size_t>(std::ceil(input.size() * clamped_ratio)) + grain_size);

    std::vector<float> output(estimated_size, 0.0f);
    std::vector<float> norm(estimated_size, 0.0f);
    std::vector<float> window(grain_size, 0.0f);
    for (std::size_t i = 0; i < grain_size; ++i) {
        window[i] = static_cast<float>(0.5 - 0.5 * std::cos((2.0 * kPi * i) / std::max<std::size_t>(1, grain_size - 1)));
    }

    double input_pos = 0.0;
    std::size_t output_pos = 0;
    while (output_pos + grain_size <= output.size() && input_pos < static_cast<double>(input.size())) {
        const std::size_t input_base = static_cast<std::size_t>(std::floor(input_pos));
        for (std::size_t i = 0; i < grain_size; ++i) {
            const std::size_t source_index = std::min(input_base + i, input.size() - 1);
            const float sample = input[source_index] * window[i];
            output[output_pos + i] += sample;
            norm[output_pos + i] += window[i];
        }
        input_pos += input_hop;
        output_pos += output_hop;
    }

    for (std::size_t i = 0; i < output.size(); ++i) {
        if (norm[i] > 1e-6f) {
            output[i] /= norm[i];
        }
    }
    while (!output.empty() && std::fabs(output.back()) < 1e-6f) {
        output.pop_back();
    }
    return output;
}

std::vector<float> apply_signalsmith_pitch_time(
    const std::vector<float>& input,
    int sample_rate,
    double pitch_shift_semitones,
    double time_ratio
) {
    if (input.empty()) {
        return input;
    }
    const double clamped_time = std::clamp(time_ratio, 0.01, 8.0);
    if (std::abs(pitch_shift_semitones) < 0.001 && std::abs(clamped_time - 1.0) < 0.001) {
        return input;
    }

    const int output_samples = std::max(1, static_cast<int>(std::lround(input.size() * clamped_time)));
    signalsmith::stretch::SignalsmithStretch<float> stretch;
    stretch.presetDefault(1, static_cast<float>(sample_rate));
    stretch.setTransposeSemitones(static_cast<float>(pitch_shift_semitones), static_cast<float>(8000.0 / std::max(1, sample_rate)));

    const float* input_channels[1] = {input.data()};
    std::vector<float> output(static_cast<std::size_t>(output_samples), 0.0f);
    float* output_channels[1] = {output.data()};
    if (!stretch.exact(input_channels, static_cast<int>(input.size()), output_channels, output_samples)) {
        return apply_time_stretch(input, clamped_time);
    }
    return output;
}

void apply_bass_boost(std::vector<float>* mono, int sample_rate, double amount) {
    if (mono == nullptr || mono->empty() || amount <= 0.0) {
        return;
    }
    const double alpha = std::exp(-2.0 * kPi * 180.0 / static_cast<double>(std::max(1, sample_rate)));
    float low = 0.0f;
    for (std::size_t i = 0; i < mono->size(); ++i) {
        low = static_cast<float>((1.0 - alpha) * (*mono)[i] + alpha * low);
        (*mono)[i] += low * static_cast<float>(amount);
    }
}

void apply_compressor(std::vector<float>* mono, double threshold, double ratio) {
    if (mono == nullptr || mono->empty()) {
        return;
    }
    const float clamped_threshold = static_cast<float>(std::clamp(threshold, 0.05, 1.0));
    const float clamped_ratio = static_cast<float>(std::clamp(ratio, 1.0, 20.0));
    for (float& sample : *mono) {
        const float sign = sample < 0.0f ? -1.0f : 1.0f;
        const float magnitude = std::fabs(sample);
        if (magnitude <= clamped_threshold) {
            continue;
        }
        const float excess = magnitude - clamped_threshold;
        const float compressed = clamped_threshold + excess / clamped_ratio;
        sample = sign * compressed;
    }
}

void apply_delay_fx(std::vector<float>* mono, int sample_rate, double time_seconds, double feedback, double mix) {
    if (mono == nullptr || mono->empty()) {
        return;
    }
    const std::size_t delay_samples =
        static_cast<std::size_t>(std::max(1.0, std::round(std::clamp(time_seconds, 0.01, 1.5) * sample_rate)));
    const float clamped_feedback = static_cast<float>(std::clamp(feedback, 0.0, 0.95));
    const float clamped_mix = static_cast<float>(std::clamp(mix, 0.0, 1.0));
    const std::vector<float> dry = *mono;
    std::vector<float> wet(mono->size() + delay_samples * 4, 0.0f);
    for (std::size_t i = 0; i < dry.size(); ++i) {
        wet[i] += dry[i];
        if (i + delay_samples < wet.size()) {
            wet[i + delay_samples] += wet[i] * clamped_feedback;
        }
    }
    mono->resize(wet.size(), 0.0f);
    for (std::size_t i = 0; i < mono->size(); ++i) {
        const float dry_sample = i < dry.size() ? dry[i] : 0.0f;
        const float delayed = wet[i];
        (*mono)[i] = dry_sample * (1.0f - clamped_mix) + delayed * clamped_mix;
    }
}

void apply_reverb(std::vector<float>* mono, int sample_rate, double wet, double decay) {
    if (mono == nullptr || mono->empty()) {
        return;
    }
    const float wet_mix = static_cast<float>(std::clamp(wet, 0.0, 1.0));
    const float feedback = static_cast<float>(std::clamp(decay, 0.05, 0.98));
    const std::array<double, 3> times{0.031, 0.043, 0.057};
    std::vector<float> dry = *mono;
    std::vector<float> accum(mono->size() + static_cast<std::size_t>(sample_rate * 0.12), 0.0f);
    for (double time : times) {
        const std::size_t delay_samples = static_cast<std::size_t>(std::max(1.0, std::round(time * sample_rate)));
        std::vector<float> line(accum.size(), 0.0f);
        for (std::size_t i = 0; i < dry.size(); ++i) {
            line[i] += dry[i];
            if (i + delay_samples < line.size()) {
                line[i + delay_samples] += line[i] * feedback;
            }
        }
        for (std::size_t i = 0; i < accum.size(); ++i) {
            accum[i] += line[i] / static_cast<float>(times.size());
        }
    }
    mono->resize(accum.size(), 0.0f);
    for (std::size_t i = 0; i < mono->size(); ++i) {
        const float dry_sample = i < dry.size() ? dry[i] : 0.0f;
        (*mono)[i] = dry_sample * (1.0f - wet_mix) + accum[i] * wet_mix;
    }
}

void apply_limiter(std::vector<float>* mono, double ceiling) {
    if (mono == nullptr || mono->empty()) {
        return;
    }
    const float limit = static_cast<float>(std::clamp(ceiling, 0.1, 1.0));
    for (float& sample : *mono) {
        sample = std::clamp(sample, -limit, limit);
    }
}

bool any_solo(const PlaybackPreset& preset) {
    for (const auto& layer : preset.layers) {
        if (layer.active && layer.solo && !layer.mute) {
            return true;
        }
    }
    return false;
}

double clamp01(double value) {
    return std::clamp(value, 0.0, 1.0);
}

double clamp_pan(double value) {
    return std::clamp(value, -1.0, 1.0);
}

std::size_t estimate_effect_tail_samples(const PlaybackLayer& layer, int sample_rate) {
    std::size_t tail = 0;
    if (layer.effects.delay.enabled) {
        const auto delay_samples = static_cast<std::size_t>(
            std::max(1.0, std::round(std::clamp(layer.effects.delay.time_seconds, 0.01, 1.5) * sample_rate))
        );
        tail = std::max(tail, delay_samples * 4);
    }
    if (layer.effects.reverb.enabled) {
        tail = std::max(tail, static_cast<std::size_t>(sample_rate * 0.12));
    }
    return tail;
}

double compute_envelope_level(
    const PlaybackEnvelope& envelope,
    std::size_t sample_index,
    std::size_t note_off_sample,
    bool released,
    int sample_rate
) {
    const std::size_t attack_samples = static_cast<std::size_t>(std::max(0.0, envelope.attack_seconds) * sample_rate);
    const std::size_t decay_samples = static_cast<std::size_t>(std::max(0.0, envelope.decay_seconds) * sample_rate);
    const std::size_t release_samples = static_cast<std::size_t>(std::max(0.0, envelope.release_seconds) * sample_rate);

    auto level_before_release = [&](std::size_t local_sample) {
        if (attack_samples > 0 && local_sample < attack_samples) {
            return static_cast<double>(local_sample) / static_cast<double>(attack_samples);
        }
        if (decay_samples > 0 && local_sample < attack_samples + decay_samples) {
            const auto decay_progress =
                static_cast<double>(local_sample - attack_samples) / static_cast<double>(decay_samples);
            return 1.0 + (envelope.sustain_level - 1.0) * decay_progress;
        }
        return envelope.sustain_level;
    };

    if (!released || sample_index < note_off_sample) {
        return level_before_release(sample_index);
    }

    if (release_samples == 0) {
        return 0.0;
    }

    const auto release_progress =
        static_cast<double>(sample_index - note_off_sample) / static_cast<double>(release_samples);
    if (release_progress >= 1.0) {
        return 0.0;
    }
    const auto note_off_level = level_before_release(note_off_sample);
    return note_off_level * (1.0 - release_progress);
}

float read_buffer_sample_channel(const AudioBuffer& buffer, double frame_position, int channel, bool reverse) {
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
    (void) reverse;
    return static_cast<float>(sample_a + (sample_b - sample_a) * frac);
}

void apply_effect_chain(std::vector<float>* channel_audio, const PlaybackLayer& layer, int sample_rate) {
    if (channel_audio == nullptr) {
        return;
    }
    *channel_audio = apply_signalsmith_pitch_time(
        *channel_audio,
        sample_rate,
        layer.effects.pitch_shift_semitones,
        layer.effects.time_stretch_ratio
    );
    apply_bass_boost(channel_audio, sample_rate, layer.effects.bass_boost.enabled ? layer.effects.bass_boost.amount : 0.0);
    if (layer.effects.compressor.enabled) {
        apply_compressor(channel_audio, layer.effects.compressor.threshold, layer.effects.compressor.ratio);
    }
    if (layer.effects.delay.enabled) {
        apply_delay_fx(
            channel_audio,
            sample_rate,
            layer.effects.delay.time_seconds,
            layer.effects.delay.feedback,
            layer.effects.delay.mix
        );
    }
    if (layer.effects.reverb.enabled) {
        apply_reverb(channel_audio, sample_rate, layer.effects.reverb.wet, layer.effects.reverb.decay);
    }
    if (layer.effects.limiter.enabled) {
        apply_limiter(channel_audio, layer.effects.limiter.ceiling);
    }
}

VoicePlan build_voice_plan(
    const PlaybackLayer& layer,
    const std::unordered_map<std::string, AudioBuffer>& buffers,
    PlaybackEngine::LayerHistory& history,
    std::mt19937& rng,
    int sample_rate
) {
    VoicePlan plan;
    plan.layer = &layer;

    if (layer.sources.empty()) {
        return plan;
    }

    std::uniform_int_distribution<int> source_dist(0, static_cast<int>(layer.sources.size() - 1));
    int source_index = source_dist(rng);
    if (layer.no_immediate_repeat && layer.sources.size() > 1 && source_index == history.last_source_index) {
        source_index = (source_index + 1) % static_cast<int>(layer.sources.size());
    }
    history.last_source_index = source_index;
    const auto& source = layer.sources[static_cast<std::size_t>(source_index)];

    const auto buffer_it = buffers.find(source.buffer_id);
    if (buffer_it == buffers.end()) {
        return plan;
    }
    plan.buffer = &buffer_it->second;

    std::vector<PlaybackRegion> fallback_regions;
    const std::vector<PlaybackRegion>* regions = &source.regions;
    if (regions->empty()) {
        fallback_regions.push_back(PlaybackRegion{});
        regions = &fallback_regions;
    }
    std::uniform_int_distribution<int> region_dist(0, static_cast<int>(regions->size() - 1));
    int region_index = region_dist(rng);
    if (layer.no_immediate_repeat && regions->size() > 1 && region_index == history.last_region_index) {
        region_index = (region_index + 1) % static_cast<int>(regions->size());
    }
    history.last_region_index = region_index;
    plan.region = (*regions)[static_cast<std::size_t>(region_index)];

    const auto frames = plan.buffer->frame_count();
    if (frames == 0) {
        return plan;
    }

    const double region_start = std::max(clamp01(layer.start_offset), clamp01(plan.region.start));
    const double region_end = std::min(clamp01(layer.stop_offset), clamp01(plan.region.end));
    plan.start_frame = std::min<std::size_t>(static_cast<std::size_t>(region_start * frames), frames - 1);
    plan.end_frame = std::max(plan.start_frame + 1, static_cast<std::size_t>(region_end * frames));
    plan.end_frame = std::min(plan.end_frame, frames);

    std::uniform_real_distribution<double> unit(-1.0, 1.0);
    const double randomized_gain = layer.gain + unit(rng) * layer.random_gain.raw_amount;
    const double pan_random_offset = unit(rng) * layer.random_pan.raw_amount;
    plan.gain = std::max(0.0, randomized_gain);
    plan.pan_x = clamp_pan(layer.pan_x + pan_random_offset);
    plan.pan_y = layer.pan_y;
    plan.pan_x_right = clamp_pan(layer.pan_x_right + pan_random_offset);
    plan.pan_y_right = layer.pan_y_right;
    plan.delay_samples = static_cast<std::size_t>(std::max(0.0, layer.delay_seconds) * sample_rate);
    plan.pitch_ratio =
        std::pow(2.0, (layer.coarse_semitones + (layer.fine_cents / 100.0)) / 12.0) *
        (static_cast<double>(plan.buffer->sample_rate) / static_cast<double>(sample_rate));

    if (plan.pitch_ratio <= 0.0) {
        plan.pitch_ratio = 1.0;
    }

    return plan;
}

RenderedAudio render_impl(
    const PlaybackPreset& preset,
    const std::unordered_map<std::string, AudioBuffer>& buffers,
    std::vector<PlaybackEngine::LayerHistory>& histories,
    std::mt19937& rng,
    std::size_t hold_samples,
    bool continuous
) {
    RenderedAudio rendered;
    rendered.sample_rate = preset.output_sample_rate;
    rendered.channels = 2;

    if (histories.size() < preset.layers.size()) {
        histories.resize(preset.layers.size());
    }

    const bool solo_active = any_solo(preset);
    std::vector<VoicePlan> voices;
    voices.reserve(preset.layers.size());

    std::size_t total_frames = 0;
    std::size_t max_release = 0;

    for (std::size_t i = 0; i < preset.layers.size(); ++i) {
        const auto& layer = preset.layers[i];
        if (!layer.active || layer.mute || (solo_active && !layer.solo)) {
            continue;
        }
        auto plan = build_voice_plan(layer, buffers, histories[i], rng, preset.output_sample_rate);
        if (plan.buffer == nullptr || plan.end_frame <= plan.start_frame) {
            continue;
        }

        const std::size_t release_samples =
            static_cast<std::size_t>(std::max(0.0, layer.envelope.release_seconds) * preset.output_sample_rate);
        max_release = std::max(max_release, release_samples);

        const std::size_t region_frames = plan.end_frame - plan.start_frame;
        std::size_t voice_frames = 0;
        const double stretch_ratio = std::clamp(layer.effects.time_stretch_ratio, 0.01, 8.0);
        if (continuous) {
            voice_frames = plan.delay_samples + static_cast<std::size_t>((hold_samples + release_samples) * stretch_ratio);
        } else {
            const auto pitched_frames = static_cast<std::size_t>(std::ceil(region_frames / plan.pitch_ratio));
            voice_frames = plan.delay_samples + static_cast<std::size_t>(std::ceil(pitched_frames * stretch_ratio));
        }
        voice_frames += estimate_effect_tail_samples(layer, preset.output_sample_rate);
        total_frames = std::max(total_frames, voice_frames);
        voices.push_back(plan);
    }

    if (total_frames == 0) {
        return rendered;
    }

    rendered.samples.assign(total_frames * 2, 0.0f);

    for (const auto& voice : voices) {
        const auto& layer = *voice.layer;
        const auto [l_gain_l, r_gain_l] = surround_folddown(
            static_cast<float>(voice.pan_x), static_cast<float>(voice.pan_y));
        const bool voice_stereo = voice.buffer->channels > 1;
        const auto [l_gain_r, r_gain_r] = voice_stereo
            ? surround_folddown(static_cast<float>(voice.pan_x_right), static_cast<float>(voice.pan_y_right))
            : std::pair<float, float>{l_gain_l, r_gain_l};
        const auto voice_gain = static_cast<float>(voice.gain);
        const double loop_start_frame =
            std::clamp(voice.region.loop_start, 0.0, 1.0) * voice.buffer->frame_count();
        const double loop_end_frame =
            std::clamp(voice.region.loop_end, 0.0, 1.0) * voice.buffer->frame_count();
        const double effective_loop_start = std::clamp(loop_start_frame, static_cast<double>(voice.start_frame), static_cast<double>(voice.end_frame));
        const double effective_loop_end = std::clamp(loop_end_frame, static_cast<double>(voice.start_frame), static_cast<double>(voice.end_frame));

        double position = layer.reverse ? static_cast<double>(voice.end_frame - 1) : static_cast<double>(voice.start_frame);
        std::vector<float> voice_left(total_frames > voice.delay_samples ? total_frames - voice.delay_samples : 0, 0.0f);
        std::vector<float> voice_right = voice_left;

        for (std::size_t frame = 0; frame < voice_left.size(); ++frame) {
            const std::size_t local_sample = frame;
            const bool released = continuous && local_sample >= hold_samples;
            const double env = compute_envelope_level(layer.envelope, local_sample, hold_samples, released, preset.output_sample_rate);
            if (env <= 0.0) {
                if (released) {
                    break;
                }
                continue;
            }

            if (!continuous) {
                if ((!layer.reverse && position >= static_cast<double>(voice.end_frame)) ||
                    (layer.reverse && position < static_cast<double>(voice.start_frame))) {
                    break;
                }
            } else if (!released) {
                if ((!layer.reverse && position >= static_cast<double>(voice.end_frame)) ||
                    (layer.reverse && position < static_cast<double>(voice.start_frame))) {
                    if (voice.region.loop_enabled &&
                        effective_loop_end > effective_loop_start &&
                        effective_loop_end <= static_cast<double>(voice.end_frame)) {
                        position = layer.reverse ? effective_loop_end - 1.0 : effective_loop_start;
                    } else {
                        break;
                    }
                }
            } else if ((!layer.reverse && position >= static_cast<double>(voice.end_frame)) ||
                       (layer.reverse && position < static_cast<double>(voice.start_frame))) {
                break;
            }

            const float left_sample = read_buffer_sample_channel(*voice.buffer, position, 0, layer.reverse);
            const int right_channel = voice.buffer->channels > 1 ? 1 : 0;
            const float right_sample = read_buffer_sample_channel(*voice.buffer, position, right_channel, layer.reverse);
            voice_left[frame] += static_cast<float>(left_sample * env);
            voice_right[frame] += static_cast<float>(right_sample * env);

            position += layer.reverse ? -voice.pitch_ratio : voice.pitch_ratio;
        }

        apply_effect_chain(&voice_left, layer, preset.output_sample_rate);
        apply_effect_chain(&voice_right, layer, preset.output_sample_rate);

        const std::size_t voice_frames = std::max(voice_left.size(), voice_right.size());
        const std::size_t mixable = std::min(voice_frames, total_frames - std::min(voice.delay_samples, total_frames));
        for (std::size_t i = 0; i < mixable; ++i) {
            const std::size_t frame = voice.delay_samples + i;
            const float left = i < voice_left.size() ? voice_left[i] : 0.0f;
            const float right = i < voice_right.size() ? voice_right[i] : 0.0f;
            if (voice_stereo) {
                rendered.samples[frame * 2]     += (left * l_gain_l + right * l_gain_r) * voice_gain;
                rendered.samples[frame * 2 + 1] += (left * r_gain_l + right * r_gain_r) * voice_gain;
            } else {
                rendered.samples[frame * 2]     += left * l_gain_l * voice_gain;
                rendered.samples[frame * 2 + 1] += left * r_gain_l * voice_gain;
            }
        }
    }

    return rendered;
}

void write_u16(std::ofstream& stream, std::uint16_t value) {
    stream.put(static_cast<char>(value & 0xff));
    stream.put(static_cast<char>((value >> 8) & 0xff));
}

void write_u32(std::ofstream& stream, std::uint32_t value) {
    stream.put(static_cast<char>(value & 0xff));
    stream.put(static_cast<char>((value >> 8) & 0xff));
    stream.put(static_cast<char>((value >> 16) & 0xff));
    stream.put(static_cast<char>((value >> 24) & 0xff));
}

}  // namespace

std::size_t AudioBuffer::frame_count() const {
    return channels > 0 ? samples.size() / static_cast<std::size_t>(channels) : 0;
}

float AudioBuffer::sample_at(std::size_t frame_index, int channel) const {
    if (frame_index >= frame_count() || channel < 0 || channel >= channels) {
        return 0.0f;
    }
    return samples[frame_index * static_cast<std::size_t>(channels) + static_cast<std::size_t>(channel)];
}

std::size_t RenderedAudio::frame_count() const {
    return channels > 0 ? samples.size() / static_cast<std::size_t>(channels) : 0;
}

PlaybackEngine::PlaybackEngine(std::uint32_t seed)
    : rng_(seed) {
}

RenderedAudio PlaybackEngine::render_one_shot(
    const PlaybackPreset& preset,
    const std::unordered_map<std::string, AudioBuffer>& buffers
) {
    return render_impl(preset, buffers, histories_, rng_, 0, false);
}

RenderedAudio PlaybackEngine::render_continuous(
    const PlaybackPreset& preset,
    const std::unordered_map<std::string, AudioBuffer>& buffers,
    std::size_t hold_samples
) {
    return render_impl(preset, buffers, histories_, rng_, hold_samples, true);
}

void PlaybackEngine::write_wav_24(
    const std::filesystem::path& output_path,
    const RenderedAudio& audio
) {
    if (audio.channels <= 0 || audio.sample_rate <= 0) {
        throw std::runtime_error("WAV export requires valid channel and sample-rate data.");
    }

    std::ofstream stream(output_path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Unable to open WAV output file.");
    }

    const std::uint32_t data_size = static_cast<std::uint32_t>(audio.frame_count() * audio.channels * 3);
    const std::uint32_t riff_size = 36 + data_size;

    stream.write("RIFF", 4);
    write_u32(stream, riff_size);
    stream.write("WAVE", 4);
    stream.write("fmt ", 4);
    write_u32(stream, 16);
    write_u16(stream, 1);
    write_u16(stream, static_cast<std::uint16_t>(audio.channels));
    write_u32(stream, static_cast<std::uint32_t>(audio.sample_rate));
    write_u32(stream, static_cast<std::uint32_t>(audio.sample_rate * audio.channels * 3));
    write_u16(stream, static_cast<std::uint16_t>(audio.channels * 3));
    write_u16(stream, 24);
    stream.write("data", 4);
    write_u32(stream, data_size);

    for (float sample : audio.samples) {
        const auto clamped = std::clamp(sample, -1.0f, 0.999999f);
        const auto scaled = static_cast<std::int32_t>(std::lrint(clamped * 8388607.0f));
        stream.put(static_cast<char>(scaled & 0xff));
        stream.put(static_cast<char>((scaled >> 8) & 0xff));
        stream.put(static_cast<char>((scaled >> 16) & 0xff));
    }
}

void PlaybackEngine::write_wav_16(
    const std::filesystem::path& output_path,
    const RenderedAudio& audio
) {
    if (audio.channels <= 0 || audio.sample_rate <= 0) {
        throw std::runtime_error("WAV export requires valid channel and sample-rate data.");
    }

    std::ofstream stream(output_path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Unable to open WAV output file.");
    }

    const std::uint32_t data_size = static_cast<std::uint32_t>(audio.frame_count() * audio.channels * 2);
    const std::uint32_t riff_size = 36 + data_size;

    stream.write("RIFF", 4);
    write_u32(stream, riff_size);
    stream.write("WAVE", 4);
    stream.write("fmt ", 4);
    write_u32(stream, 16);
    write_u16(stream, 1);
    write_u16(stream, static_cast<std::uint16_t>(audio.channels));
    write_u32(stream, static_cast<std::uint32_t>(audio.sample_rate));
    write_u32(stream, static_cast<std::uint32_t>(audio.sample_rate * audio.channels * 2));
    write_u16(stream, static_cast<std::uint16_t>(audio.channels * 2));
    write_u16(stream, 16);
    stream.write("data", 4);
    write_u32(stream, data_size);

    for (float sample : audio.samples) {
        const auto clamped = std::clamp(sample, -1.0f, 0.9999695f);
        const auto scaled = static_cast<std::int16_t>(std::lrint(clamped * 32767.0f));
        stream.put(static_cast<char>(scaled & 0xff));
        stream.put(static_cast<char>((scaled >> 8) & 0xff));
    }
}

}  // namespace radium
