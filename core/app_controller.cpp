#include "app_controller.h"
#include "project_file.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <unordered_set>

namespace radium {
namespace {

constexpr double kAutomationMaxDb = 12.0;
constexpr double kAutomationMinDb = -70.0;
constexpr double kStretchAutomationMinRatio = 0.01;
constexpr double kStretchAutomationMaxRatio = 8.0;
constexpr double kPanAutomationMinValue = -1.0;
constexpr double kPanAutomationMaxValue = 1.0;
constexpr double kAuxBassCutDb = -24.0;
constexpr double kAuxBassBoostDb = 12.0;
constexpr double kLayerEqCutDb = -24.0;
constexpr double kLayerEqBoostDb = 12.0;

double automation_db_to_gain(double db) {
    return std::pow(10.0, db / 20.0);
}

double max_automation_gain() {
    static const double value = automation_db_to_gain(kAutomationMaxDb);
    return value;
}

std::string join_lines(const std::vector<std::string>& lines) {
    std::ostringstream out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        out << lines[i];
        if (i + 1 < lines.size()) {
            out << '\n';
        }
    }
    return out.str();
}

double clamp01(double value) {
    return std::clamp(value, 0.0, 1.0);
}

std::string sanitize_recording_name(const std::string& text) {
    std::string sanitized;
    sanitized.reserve(text.size());
    for (char ch : text) {
        const bool ok =
            (ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '_' || ch == '-' || ch == ' ';
        sanitized.push_back(ok ? ch : '_');
    }
    while (!sanitized.empty() && sanitized.front() == ' ') {
        sanitized.erase(sanitized.begin());
    }
    while (!sanitized.empty() && sanitized.back() == ' ') {
        sanitized.pop_back();
    }
    return sanitized.empty() ? "take" : sanitized;
}

std::string unique_session_suffix() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

void sort_trigger_regions(std::vector<AppController::LayerOverride::TriggerRegion>* regions) {
    if (regions == nullptr) {
        return;
    }
    std::sort(regions->begin(), regions->end(), [](const auto& a, const auto& b) {
        if (a.start != b.start) {
            return a.start < b.start;
        }
        return a.end < b.end;
    });
}

AppController::LayerEffectState default_effect_state_for_layer(const Layer& layer) {
    AppController::LayerEffectState effects;
    effects.reverse = layer.reverse;
    return effects;
}

std::vector<AppController::LayerOverride::TriggerRegion> merge_regions(
    std::vector<AppController::LayerOverride::TriggerRegion> regions,
    double merge_distance
) {
    if (regions.empty()) {
        return regions;
    }
    std::sort(regions.begin(), regions.end(), [](const auto& a, const auto& b) {
        return a.start < b.start;
    });

    std::vector<AppController::LayerOverride::TriggerRegion> merged;
    merged.push_back(regions.front());
    for (std::size_t i = 1; i < regions.size(); ++i) {
        auto& current = merged.back();
        if (regions[i].start <= current.end + merge_distance) {
            current.start = std::min(current.start, regions[i].start);
            current.end = std::max(current.end, regions[i].end);
        } else {
            merged.push_back(regions[i]);
        }
    }
    return merged;
}


std::vector<AppController::LayerOverride::TriggerRegion> detect_trigger_regions(const AudioBuffer& buffer) {
    std::vector<AppController::LayerOverride::TriggerRegion> regions;
    if (buffer.frame_count() == 0) {
        return regions;
    }

    const std::size_t frame_count = buffer.frame_count();
    const int sr = std::max(1, buffer.sample_rate);

    // --- Step 1: Compute per-sample absolute mono amplitude ---
    std::vector<float> mono(frame_count, 0.0f);
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        float value = 0.0f;
        for (int channel = 0; channel < buffer.channels; ++channel) {
            value += std::fabs(buffer.sample_at(frame, channel));
        }
        mono[frame] = value / static_cast<float>(std::max(1, buffer.channels));
    }

    // --- Step 2: Compute RMS energy in overlapping windows ---
    // Window size ~5ms, hop ~2.5ms — gives smooth energy envelope
    const std::size_t rms_window = static_cast<std::size_t>(std::max(64, sr / 200));  // ~5ms
    const std::size_t rms_hop = std::max<std::size_t>(1, rms_window / 2);
    const std::size_t num_blocks = (frame_count + rms_hop - 1) / rms_hop;

    std::vector<float> rms_energy(num_blocks, 0.0f);
    float peak_rms = 0.0f;
    for (std::size_t b = 0; b < num_blocks; ++b) {
        const std::size_t center = b * rms_hop;
        const std::size_t win_start = center > rms_window / 2 ? center - rms_window / 2 : 0;
        const std::size_t win_end = std::min(frame_count, win_start + rms_window);
        double sum_sq = 0.0;
        for (std::size_t i = win_start; i < win_end; ++i) {
            sum_sq += static_cast<double>(mono[i]) * static_cast<double>(mono[i]);
        }
        const float rms = static_cast<float>(std::sqrt(sum_sq / static_cast<double>(std::max<std::size_t>(1, win_end - win_start))));
        rms_energy[b] = rms;
        peak_rms = std::max(peak_rms, rms);
    }

    if (peak_rms <= 1e-6f) {
        return regions;
    }

    // --- Step 3: Classify each block as sound or silence ---
    // Onset threshold: 0.8% of peak RMS (enter sound state)
    // Offset threshold: 0.25% of peak RMS (exit sound state — hysteresis)
    // These are deliberately low so that quieter hits in a series (e.g. softer
    // footsteps mixed with louder ones) still get their own regions.
    const float onset_threshold = peak_rms * 0.008f;
    const float offset_threshold = peak_rms * 0.0025f;

    // Minimum silence gap between regions: ~30ms in blocks
    const std::size_t min_silence_blocks = std::max<std::size_t>(2, static_cast<std::size_t>(0.030 * sr) / rms_hop);
    // Minimum region length: ~15ms in blocks
    const std::size_t min_region_blocks = std::max<std::size_t>(2, static_cast<std::size_t>(0.015 * sr) / rms_hop);

    struct BlockRegion { std::size_t start_block; std::size_t end_block; };
    std::vector<BlockRegion> block_regions;

    bool in_sound = false;
    std::size_t region_start = 0;
    std::size_t last_above_offset = 0;
    std::size_t silence_count = min_silence_blocks;  // start in silence state

    for (std::size_t b = 0; b < num_blocks; ++b) {
        if (!in_sound) {
            if (rms_energy[b] >= onset_threshold && silence_count >= min_silence_blocks) {
                in_sound = true;
                region_start = b;
                last_above_offset = b;
                silence_count = 0;
            } else if (rms_energy[b] < offset_threshold) {
                silence_count += 1;
            }
            // Note: energy between offset and onset does NOT reset silence_count.
            // This prevents stray low-level noise from blocking the next onset.
        } else {
            if (rms_energy[b] >= offset_threshold) {
                last_above_offset = b;
                silence_count = 0;
            } else {
                silence_count += 1;
                if (silence_count >= min_silence_blocks) {
                    const std::size_t end_block = last_above_offset + 1;
                    if (end_block > region_start + min_region_blocks) {
                        block_regions.push_back({region_start, end_block});
                    }
                    in_sound = false;
                }
            }
        }
    }
    // Close any open region at end of file
    if (in_sound) {
        const std::size_t end_block = last_above_offset + 1;
        if (end_block > region_start + min_region_blocks) {
            block_regions.push_back({region_start, end_block});
        }
    }

    if (block_regions.empty()) {
        return regions;
    }

    // --- Step 4: Convert block indices to sample-accurate boundaries ---
    // Walk backwards from each block onset to find the true first sample above noise floor.
    // Walk forwards from each block offset to find where signal truly fades.
    const float sample_onset_threshold = peak_rms * 0.002f;  // very low — catch the attack transient
    // Pre-roll: add ~2ms before detected onset so we don't clip the attack
    const std::size_t pre_roll = static_cast<std::size_t>(std::max(1, sr / 500));

    for (const auto& br : block_regions) {
        // Rough frame boundaries from block indices
        std::size_t start_frame = br.start_block * rms_hop;
        std::size_t end_frame = std::min(frame_count, br.end_block * rms_hop);

        // Refine start: scan backwards from rough start to find first sample above threshold
        while (start_frame > 0 && mono[start_frame] >= sample_onset_threshold) {
            --start_frame;
        }
        // Then scan forward to the actual first above-threshold sample
        while (start_frame < end_frame && mono[start_frame] < sample_onset_threshold) {
            ++start_frame;
        }
        // Apply pre-roll
        start_frame = start_frame > pre_roll ? start_frame - pre_roll : 0;

        // Refine end: scan backwards from rough end to last sample above threshold
        while (end_frame > start_frame + 1 && mono[end_frame - 1] < sample_onset_threshold) {
            --end_frame;
        }
        // Add a small tail (~2ms) so the release isn't chopped
        end_frame = std::min(frame_count, end_frame + pre_roll);

        if (end_frame > start_frame) {
            regions.push_back({
                static_cast<double>(start_frame) / static_cast<double>(frame_count),
                static_cast<double>(end_frame) / static_cast<double>(frame_count)
            });
        }
    }

    // Final merge pass — regions that nearly touch (<5ms gap) get combined
    regions = merge_regions(std::move(regions), static_cast<double>(sr / 200) / static_cast<double>(frame_count));

    return regions;
}

bool layers_still_reference_buffer(const std::vector<Layer>& layers, const std::string& buffer_id) {
    for (const auto& layer : layers) {
        if (layer.embedded_media_reference.has_value() &&
            *layer.embedded_media_reference == buffer_id) {
            return true;
        }
        for (const auto& source : layer.sources) {
            if (source.buffer_id.has_value() && *source.buffer_id == buffer_id) {
                return true;
            }
        }
    }
    return false;
}

bool layer_is_locked(const std::vector<AppController::LayerOverride>& overrides, std::size_t layer_index) {
    return layer_index < overrides.size() && overrides[layer_index].locked;
}

void erase_unreferenced_project_buffers(
    std::unordered_map<std::string, AudioBuffer>* buffers,
    const std::vector<Layer>& layers,
    const std::vector<std::string>& candidate_buffer_ids
) {
    if (buffers == nullptr) {
        return;
    }
    for (const auto& buffer_id : candidate_buffer_ids) {
        if (!layers_still_reference_buffer(layers, buffer_id)) {
            buffers->erase(buffer_id);
        }
    }
}

RenderedAudio mix_rendered_layers(const std::vector<RenderedAudio>& layers, int sample_rate, int channels) {
    RenderedAudio mixed;
    mixed.sample_rate = sample_rate;
    mixed.channels = channels;
    std::size_t max_frames = 0;
    for (const auto& layer : layers) {
        max_frames = std::max(max_frames, layer.frame_count());
    }
    mixed.samples.assign(max_frames * static_cast<std::size_t>(channels), 0.0f);
    for (const auto& layer : layers) {
        const auto frame_count = layer.frame_count();
        for (std::size_t frame = 0; frame < frame_count; ++frame) {
            for (int channel = 0; channel < channels; ++channel) {
                mixed.samples[frame * static_cast<std::size_t>(channels) + static_cast<std::size_t>(channel)] +=
                    layer.samples[frame * static_cast<std::size_t>(layer.channels) + static_cast<std::size_t>(std::min(channel, layer.channels - 1))];
            }
        }
    }
    for (auto& sample : mixed.samples) {
        sample = std::clamp(sample, -1.0f, 1.0f);
    }
    return mixed;
}

double layer_edit_clip_duration_seconds(const AppController::LayerEditClip& clip) {
    return std::max(0.0, clip.source_end_seconds - clip.source_start_seconds);
}

double layer_edit_clip_end_time(const AppController::LayerEditClip& clip) {
    return clip.timeline_start_seconds + layer_edit_clip_duration_seconds(clip);
}

std::optional<std::pair<double, double>> layer_edit_content_span_normalized(
    const AppController::LayerEditState& state
) {
    if (state.clips.empty()) {
        return std::nullopt;
    }

    double firstClipStart = std::numeric_limits<double>::max();
    double lastClipEnd = 0.0;
    for (const auto& clip : state.clips) {
        const double clipDuration = layer_edit_clip_duration_seconds(clip);
        if (clipDuration <= 0.0) {
            continue;
        }
        firstClipStart = std::min(firstClipStart, clip.timeline_start_seconds);
        lastClipEnd = std::max(lastClipEnd, layer_edit_clip_end_time(clip));
    }

    if (firstClipStart == std::numeric_limits<double>::max() ||
        lastClipEnd <= firstClipStart + 0.000001) {
        return std::nullopt;
    }

    const double totalDuration = std::max(
        lastClipEnd + state.tail_padding_seconds,
        state.head_padding_seconds + state.tail_padding_seconds);
    if (totalDuration <= 0.0) {
        return std::nullopt;
    }

    return std::pair<double, double>{
        std::clamp(firstClipStart / totalDuration, 0.0, 1.0),
        std::clamp(lastClipEnd / totalDuration, 0.0, 1.0)
    };
}

std::string layer_edit_rendered_buffer_id(std::size_t layer_index) {
    return "__layer_edit_render_" + std::to_string(layer_index);
}

float clip_fade_gain(std::size_t frame_index,
                     std::size_t clip_frames,
                     std::size_t fade_in_frames,
                     std::size_t fade_out_frames) {
    float gain = 1.0f;
    if (fade_in_frames > 0) {
        gain = std::min(gain, static_cast<float>(frame_index) / static_cast<float>(fade_in_frames));
    }
    if (fade_out_frames > 0 && clip_frames > 0) {
        const auto frames_from_end = clip_frames > frame_index ? (clip_frames - frame_index) : std::size_t(0);
        gain = std::min(gain, static_cast<float>(frames_from_end) / static_cast<float>(fade_out_frames));
    }
    return std::clamp(gain, 0.0f, 1.0f);
}

void clamp_layer_edit_fades(AppController::LayerEditClip* clip) {
    if (clip == nullptr) {
        return;
    }

    const double duration = layer_edit_clip_duration_seconds(*clip);
    clip->fade_in_seconds = std::clamp(clip->fade_in_seconds, 0.0, duration);
    clip->fade_out_seconds = std::clamp(clip->fade_out_seconds, 0.0, duration);
}

double layer_edit_overlap_seconds(const AppController::LayerEditClip& left,
                                  const AppController::LayerEditClip& right) {
    return layer_edit_clip_end_time(left) - right.timeline_start_seconds;
}

bool clips_form_crossfade(const AppController::LayerEditClip& left,
                          const AppController::LayerEditClip& right) {
    const double overlap = layer_edit_overlap_seconds(left, right);
    if (overlap <= 0.0005) {
        return false;
    }

    constexpr double kToleranceSeconds = 0.02;
    return std::abs(left.fade_out_seconds - overlap) <= kToleranceSeconds &&
           std::abs(right.fade_in_seconds - overlap) <= kToleranceSeconds;
}

void preserve_crossfade_pair_after_move(
    AppController::LayerEditState* state,
    std::size_t left_index,
    std::size_t right_index
) {
    if (state == nullptr || left_index >= state->clips.size() || right_index >= state->clips.size() ||
        left_index >= right_index) {
        return;
    }

    auto& left = state->clips[left_index];
    auto& right = state->clips[right_index];

    // Once the clip moves outside the original ordering, let the pair fall back
    // to separate fades instead of forcing a crossfade.
    if (left.timeline_start_seconds > right.timeline_start_seconds) {
        return;
    }

    const double overlap = layer_edit_overlap_seconds(left, right);
    if (overlap <= 0.0005) {
        return;
    }

    const double max_crossfade =
        std::min(layer_edit_clip_duration_seconds(left), layer_edit_clip_duration_seconds(right));
    const double crossfade = std::clamp(overlap, 0.0, max_crossfade);
    left.fade_out_seconds = crossfade;
    right.fade_in_seconds = crossfade;
    clamp_layer_edit_fades(&left);
    clamp_layer_edit_fades(&right);
}

std::vector<AppController::LayerEditClip> subtract_overlap_from_clip(
    const AppController::LayerEditClip& clip,
    double overlap_start,
    double overlap_end
) {
    std::vector<AppController::LayerEditClip> pieces;

    const double clip_start = clip.timeline_start_seconds;
    const double clip_end = layer_edit_clip_end_time(clip);
    const double trim_start = std::max(clip_start, overlap_start);
    const double trim_end = std::min(clip_end, overlap_end);
    if (trim_end <= trim_start + 0.0005) {
        pieces.push_back(clip);
        return pieces;
    }

    if (trim_start <= clip_start + 0.0005 && trim_end >= clip_end - 0.0005) {
        return pieces;
    }

    if (trim_start > clip_start + 0.0005) {
        auto left = clip;
        left.source_end_seconds = clip.source_start_seconds + (trim_start - clip_start);
        clamp_layer_edit_fades(&left);
        pieces.push_back(std::move(left));
    }

    if (trim_end < clip_end - 0.0005) {
        auto right = clip;
        const double source_offset = trim_end - clip_start;
        right.timeline_start_seconds = trim_end;
        right.source_start_seconds = clip.source_start_seconds + source_offset;
        clamp_layer_edit_fades(&right);
        pieces.push_back(std::move(right));
    }

    return pieces;
}

void resolve_layer_edit_overlaps(
    AppController::LayerEditState* state,
    const std::vector<std::size_t>& priority_clip_indices
) {
    if (state == nullptr || state->clips.empty()) {
        return;
    }

    std::vector<bool> is_priority(state->clips.size(), false);
    for (const auto index : priority_clip_indices) {
        if (index < is_priority.size()) {
            is_priority[index] = true;
        }
    }

    std::vector<AppController::LayerEditClip> ordered;
    ordered.reserve(state->clips.size());
    for (std::size_t i = 0; i < state->clips.size(); ++i) {
        if (!is_priority[i]) {
            ordered.push_back(state->clips[i]);
        }
    }
    for (std::size_t i = 0; i < state->clips.size(); ++i) {
        if (is_priority[i]) {
            ordered.push_back(state->clips[i]);
        }
    }

    std::vector<AppController::LayerEditClip> resolved;
    resolved.reserve(ordered.size());
    for (const auto& clip : ordered) {
        const double clip_start = clip.timeline_start_seconds;
        const double clip_end = layer_edit_clip_end_time(clip);
        std::vector<AppController::LayerEditClip> updated;
        for (const auto& existing : resolved) {
            if (clips_form_crossfade(existing, clip)) {
                updated.push_back(existing);
                continue;
            }
            auto trimmed = subtract_overlap_from_clip(existing, clip_start, clip_end);
            updated.insert(updated.end(),
                           std::make_move_iterator(trimmed.begin()),
                           std::make_move_iterator(trimmed.end()));
        }
        updated.push_back(clip);
        resolved = std::move(updated);
    }

    std::sort(resolved.begin(), resolved.end(), [](const auto& a, const auto& b) {
        if (a.timeline_start_seconds == b.timeline_start_seconds) {
            return a.source_start_seconds < b.source_start_seconds;
        }
        return a.timeline_start_seconds < b.timeline_start_seconds;
    });

    for (auto& clip : resolved) {
        clamp_layer_edit_fades(&clip);
    }
    state->clips = std::move(resolved);
}

void sort_volume_automation_points(
    std::vector<AppController::LayerEditState::VolumeAutomationPoint>* points
) {
    if (points == nullptr) {
        return;
    }
    std::sort(points->begin(), points->end(), [](const auto& a, const auto& b) {
        if (a.timeline_seconds != b.timeline_seconds) {
            return a.timeline_seconds < b.timeline_seconds;
        }
        return a.gain < b.gain;
    });
}

std::size_t next_volume_automation_point_id(const AppController::LayerEditState& state) {
    std::size_t nextId = 1;
    for (const auto& point : state.volume_automation_points) {
        nextId = std::max(nextId, point.point_id + 1);
    }
    return nextId;
}

void sort_stretch_automation_points(
    std::vector<AppController::LayerEditState::StretchAutomationPoint>* points
) {
    if (points == nullptr) {
        return;
    }
    std::sort(points->begin(), points->end(), [](const auto& a, const auto& b) {
        if (a.timeline_seconds != b.timeline_seconds) {
            return a.timeline_seconds < b.timeline_seconds;
        }
        return a.ratio < b.ratio;
    });
}

std::size_t next_stretch_automation_point_id(const AppController::LayerEditState& state) {
    std::size_t nextId = 1;
    for (const auto& point : state.stretch_automation_points) {
        nextId = std::max(nextId, point.point_id + 1);
    }
    return nextId;
}

void sort_pan_automation_points(
    std::vector<AppController::LayerEditState::PanAutomationPoint>* points
) {
    if (points == nullptr) {
        return;
    }
    std::sort(points->begin(), points->end(), [](const auto& a, const auto& b) {
        if (a.timeline_seconds != b.timeline_seconds) {
            return a.timeline_seconds < b.timeline_seconds;
        }
        return a.value < b.value;
    });
}

std::size_t next_pan_automation_point_id(const AppController::LayerEditState& state) {
    std::size_t nextId = 1;
    const auto visit = [&](const auto& points) {
        for (const auto& point : points) {
            nextId = std::max(nextId, point.point_id + 1);
        }
    };
    visit(state.pan_position_automation_points);
    visit(state.pan_front_back_automation_points);
    visit(state.pan_right_position_automation_points);
    visit(state.pan_right_front_back_automation_points);
    visit(state.doppler_automation_points);
    return nextId;
}

std::pair<bool*, std::vector<AppController::LayerEditState::PanAutomationPoint>*> pan_automation_storage(
    AppController::LayerEditState* state,
    AppController::PanAutomationTarget target
) {
    if (state == nullptr) {
        return {nullptr, nullptr};
    }
    switch (target) {
        case AppController::PanAutomationTarget::Position:
            return {&state->pan_position_automation_enabled, &state->pan_position_automation_points};
        case AppController::PanAutomationTarget::FrontBack:
            return {&state->pan_front_back_automation_enabled, &state->pan_front_back_automation_points};
        case AppController::PanAutomationTarget::RightPosition:
            return {&state->pan_right_position_automation_enabled, &state->pan_right_position_automation_points};
        case AppController::PanAutomationTarget::RightFrontBack:
            return {&state->pan_right_front_back_automation_enabled, &state->pan_right_front_back_automation_points};
    }
    return {nullptr, nullptr};
}

std::pair<const bool*, const std::vector<AppController::LayerEditState::PanAutomationPoint>*> pan_automation_storage(
    const AppController::LayerEditState* state,
    AppController::PanAutomationTarget target
) {
    if (state == nullptr) {
        return {nullptr, nullptr};
    }
    switch (target) {
        case AppController::PanAutomationTarget::Position:
            return {&state->pan_position_automation_enabled, &state->pan_position_automation_points};
        case AppController::PanAutomationTarget::FrontBack:
            return {&state->pan_front_back_automation_enabled, &state->pan_front_back_automation_points};
        case AppController::PanAutomationTarget::RightPosition:
            return {&state->pan_right_position_automation_enabled, &state->pan_right_position_automation_points};
        case AppController::PanAutomationTarget::RightFrontBack:
            return {&state->pan_right_front_back_automation_enabled, &state->pan_right_front_back_automation_points};
    }
    return {nullptr, nullptr};
}

std::pair<bool*, std::vector<AppController::LayerEditState::PanAutomationPoint>*> doppler_automation_storage(
    AppController::LayerEditState* state
) {
    if (state == nullptr) {
        return {nullptr, nullptr};
    }
    return {&state->doppler_automation_enabled, &state->doppler_automation_points};
}

std::pair<const bool*, const std::vector<AppController::LayerEditState::PanAutomationPoint>*> doppler_automation_storage(
    const AppController::LayerEditState* state
) {
    if (state == nullptr) {
        return {nullptr, nullptr};
    }
    return {&state->doppler_automation_enabled, &state->doppler_automation_points};
}

DopplerSettings sanitize_doppler_settings(DopplerSettings settings) {
    settings.edge_gain_db = std::clamp(settings.edge_gain_db, -70.0, 0.0);
    settings.center_gain_db = std::clamp(settings.center_gain_db, -24.0, 12.0);
    settings.edge_pitch_semitones = std::clamp(settings.edge_pitch_semitones, -24.0, 0.0);
    settings.center_pitch_semitones = std::clamp(settings.center_pitch_semitones, 0.0, 24.0);
    return settings;
}

VolumeRandomSettings sanitize_volume_random_settings(VolumeRandomSettings settings) {
    settings.loudest_db = std::clamp(settings.loudest_db, 0.0, 12.0);
    settings.quietest_db = std::clamp(settings.quietest_db, -70.0, 0.0);
    settings.period_shortest_seconds = std::clamp(settings.period_shortest_seconds, 0.02, 20.0);
    settings.period_longest_seconds = std::clamp(settings.period_longest_seconds, 0.02, 20.0);
    if (settings.period_longest_seconds < settings.period_shortest_seconds) {
        settings.period_longest_seconds = settings.period_shortest_seconds;
    }
    settings.smoothing = std::clamp(settings.smoothing, 0.0, 1.0);
    return settings;
}

PanRandomSettings sanitize_pan_random_settings(PanRandomSettings settings) {
    settings.farthest_left = std::clamp(settings.farthest_left, -1.0, 1.0);
    settings.farthest_right = std::clamp(settings.farthest_right, -1.0, 1.0);
    if (settings.farthest_right < settings.farthest_left) {
        settings.farthest_right = settings.farthest_left;
    }
    settings.farthest_back = std::clamp(settings.farthest_back, -1.0, 1.0);
    settings.farthest_front = std::clamp(settings.farthest_front, -1.0, 1.0);
    if (settings.farthest_front < settings.farthest_back) {
        settings.farthest_front = settings.farthest_back;
    }
    settings.speed = std::clamp(settings.speed, 0.0, 1.0);
    settings.smoothing = std::clamp(settings.smoothing, 0.0, 1.0);
    return settings;
}

StretchRandomSettings sanitize_stretch_random_settings(StretchRandomSettings settings) {
    settings.lowest_percent = std::clamp(settings.lowest_percent, 1.0, 800.0);
    settings.highest_percent = std::clamp(settings.highest_percent, 1.0, 800.0);
    if (settings.highest_percent < settings.lowest_percent) {
        settings.highest_percent = settings.lowest_percent;
    }
    settings.speed = std::clamp(settings.speed, 0.0, 1.0);
    settings.smoothing = std::clamp(settings.smoothing, 0.0, 1.0);
    return settings;
}

double evaluate_volume_automation_gain(
    const AppController::LayerEditState& state,
    double timeline_seconds
) {
    if (!state.volume_automation_enabled || state.volume_automation_points.empty()) {
        return 1.0;
    }

    const auto clamped_time = std::max(0.0, timeline_seconds);
    const auto& points = state.volume_automation_points;
    if (points.size() == 1) {
        return std::clamp(points.front().gain, 0.0, max_automation_gain());
    }

    if (clamped_time <= points.front().timeline_seconds) {
        return std::clamp(points.front().gain, 0.0, max_automation_gain());
    }
    if (clamped_time >= points.back().timeline_seconds) {
        return std::clamp(points.back().gain, 0.0, max_automation_gain());
    }

    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const auto& left = points[i];
        const auto& right = points[i + 1];
        if (clamped_time >= left.timeline_seconds && clamped_time <= right.timeline_seconds) {
            const double span = std::max(0.000001, right.timeline_seconds - left.timeline_seconds);
            const double t = std::clamp((clamped_time - left.timeline_seconds) / span, 0.0, 1.0);
            return std::clamp(left.gain + (right.gain - left.gain) * t, 0.0, max_automation_gain());
        }
    }

    return 1.0;
}

double evaluate_stretch_automation_ratio(
    const AppController::LayerEditState& state,
    double timeline_seconds,
    double fallback_ratio
) {
    if (!state.stretch_automation_enabled || state.stretch_automation_points.empty()) {
        return std::clamp(fallback_ratio, kStretchAutomationMinRatio, kStretchAutomationMaxRatio);
    }

    const auto clamped_time = std::max(0.0, timeline_seconds);
    const auto& points = state.stretch_automation_points;
    if (points.size() == 1) {
        return std::clamp(points.front().ratio, kStretchAutomationMinRatio, kStretchAutomationMaxRatio);
    }

    if (clamped_time <= points.front().timeline_seconds) {
        return std::clamp(points.front().ratio, kStretchAutomationMinRatio, kStretchAutomationMaxRatio);
    }
    if (clamped_time >= points.back().timeline_seconds) {
        return std::clamp(points.back().ratio, kStretchAutomationMinRatio, kStretchAutomationMaxRatio);
    }

    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const auto& left = points[i];
        const auto& right = points[i + 1];
        if (clamped_time >= left.timeline_seconds && clamped_time <= right.timeline_seconds) {
            const double span = std::max(0.000001, right.timeline_seconds - left.timeline_seconds);
            const double t = std::clamp((clamped_time - left.timeline_seconds) / span, 0.0, 1.0);
            return std::clamp(left.ratio + (right.ratio - left.ratio) * t,
                              kStretchAutomationMinRatio,
                              kStretchAutomationMaxRatio);
        }
    }

    return std::clamp(fallback_ratio, kStretchAutomationMinRatio, kStretchAutomationMaxRatio);
}

double evaluate_pan_automation_value(
    const AppController::LayerEditState& state,
    AppController::PanAutomationTarget target,
    double timeline_seconds,
    double fallback_value
) {
    const auto [enabled, points] = pan_automation_storage(&state, target);
    if (enabled == nullptr || points == nullptr || !*enabled || points->empty()) {
        return std::clamp(fallback_value, kPanAutomationMinValue, kPanAutomationMaxValue);
    }

    const auto clamped_time = std::max(0.0, timeline_seconds);
    if (points->size() == 1) {
        return std::clamp(points->front().value, kPanAutomationMinValue, kPanAutomationMaxValue);
    }
    if (clamped_time <= points->front().timeline_seconds) {
        return std::clamp(points->front().value, kPanAutomationMinValue, kPanAutomationMaxValue);
    }
    if (clamped_time >= points->back().timeline_seconds) {
        return std::clamp(points->back().value, kPanAutomationMinValue, kPanAutomationMaxValue);
    }

    for (std::size_t i = 0; i + 1 < points->size(); ++i) {
        const auto& left = (*points)[i];
        const auto& right = (*points)[i + 1];
        if (clamped_time >= left.timeline_seconds && clamped_time <= right.timeline_seconds) {
            const double span = std::max(0.000001, right.timeline_seconds - left.timeline_seconds);
            const double t = std::clamp((clamped_time - left.timeline_seconds) / span, 0.0, 1.0);
            return std::clamp(left.value + (right.value - left.value) * t,
                              kPanAutomationMinValue,
                              kPanAutomationMaxValue);
        }
    }

    return std::clamp(fallback_value, kPanAutomationMinValue, kPanAutomationMaxValue);
}

double apply_doppler_curve(double t, DopplerCurveType curve_type, double curve_amount) {
    const double clampedT = std::clamp(t, 0.0, 1.0);
    const double amount = std::clamp(curve_amount, 0.0, 1.0);
    switch (curve_type) {
        case DopplerCurveType::SCurve: {
            const double smooth = clampedT * clampedT * (3.0 - 2.0 * clampedT);
            return std::clamp(clampedT + (smooth - clampedT) * amount, 0.0, 1.0);
        }
        case DopplerCurveType::Convex: {
            const double exponent = 1.0 + amount * 4.0;
            return std::clamp(std::pow(clampedT, exponent), 0.0, 1.0);
        }
        case DopplerCurveType::Concave: {
            const double exponent = 1.0 + amount * 4.0;
            return std::clamp(1.0 - std::pow(1.0 - clampedT, exponent), 0.0, 1.0);
        }
        case DopplerCurveType::Linear:
        default:
            return clampedT;
    }
}

std::optional<DopplerSegmentShape> find_doppler_segment_shape(
    const AppController::LayerEditState& state,
    std::size_t left_point_id
) {
    const auto it = std::find_if(
        state.doppler_segment_shapes.begin(),
        state.doppler_segment_shapes.end(),
        [left_point_id](const auto& shape) { return shape.left_point_id == left_point_id; });
    if (it == state.doppler_segment_shapes.end()) {
        return std::nullopt;
    }
    return *it;
}

void prune_doppler_segment_shapes(AppController::LayerEditState* state) {
    if (state == nullptr) {
        return;
    }
    state->doppler_segment_shapes.erase(
        std::remove_if(
            state->doppler_segment_shapes.begin(),
            state->doppler_segment_shapes.end(),
            [state](const auto& shape) {
                const auto it = std::find_if(
                    state->doppler_automation_points.begin(),
                    state->doppler_automation_points.end(),
                    [&shape](const auto& point) { return point.point_id == shape.left_point_id; });
                if (it == state->doppler_automation_points.end()) {
                    return true;
                }
                return std::next(it) == state->doppler_automation_points.end();
            }),
        state->doppler_segment_shapes.end());
}

double evaluate_doppler_automation_value(
    const AppController::LayerEditState& state,
    double timeline_seconds,
    double fallback_value
) {
    const auto [enabled, points] = doppler_automation_storage(&state);
    if (enabled == nullptr || points == nullptr || !*enabled || points->empty()) {
        return std::clamp(fallback_value, kPanAutomationMinValue, kPanAutomationMaxValue);
    }

    const auto clamped_time = std::max(0.0, timeline_seconds);
    if (points->size() == 1) {
        return std::clamp(points->front().value, kPanAutomationMinValue, kPanAutomationMaxValue);
    }
    if (clamped_time <= points->front().timeline_seconds) {
        return std::clamp(points->front().value, kPanAutomationMinValue, kPanAutomationMaxValue);
    }
    if (clamped_time >= points->back().timeline_seconds) {
        return std::clamp(points->back().value, kPanAutomationMinValue, kPanAutomationMaxValue);
    }

    for (std::size_t i = 0; i + 1 < points->size(); ++i) {
        const auto& left = (*points)[i];
        const auto& right = (*points)[i + 1];
        if (clamped_time >= left.timeline_seconds && clamped_time <= right.timeline_seconds) {
            const double span = std::max(0.000001, right.timeline_seconds - left.timeline_seconds);
            double t = std::clamp((clamped_time - left.timeline_seconds) / span, 0.0, 1.0);
            if (const auto shape = find_doppler_segment_shape(state, left.point_id); shape.has_value()) {
                t = apply_doppler_curve(t, shape->curve_type, shape->curve_amount);
            }
            return std::clamp(left.value + (right.value - left.value) * t,
                              kPanAutomationMinValue,
                              kPanAutomationMaxValue);
        }
    }

    return std::clamp(fallback_value, kPanAutomationMinValue, kPanAutomationMaxValue);
}

}  // namespace

AppController::AppController(std::filesystem::path working_directory)
    : working_directory_(std::move(working_directory)) {
    std::filesystem::create_directories(working_directory_);
    reset_session_recordings_directory();
}

void AppController::set_audio_decode_func(AudioDecodeFunc func) {
    audio_decode_func_ = std::move(func);
}

void AppController::set_audio_encode_func(AudioEncodeFunc func) {
    audio_encode_func_ = std::move(func);
}

void AppController::set_audio_decode_bytes_func(AudioDecodeBytesFunc func) {
    audio_decode_bytes_func_ = std::move(func);
}

void AppController::set_output_sample_rate(int sample_rate) {
    output_sample_rate_ = sample_rate;
}

FixtureAudioDecodeResult AppController::decode_audio(
    const std::filesystem::path& path,
    const std::filesystem::path& workdir
) {
    if (audio_decode_func_) {
        return audio_decode_func_(path, workdir);
    }
    // Fallback to ffmpeg-based decode (used by tests and legacy builds)
    if (path.extension() == ".flac" || path.extension() == ".FLAC") {
        return decode_embedded_flac(path, workdir);
    }
    return decode_audio_file(path, workdir);
}

FixtureAudioResolution AppController::resolve_audio(
    const Preset& preset,
    const std::filesystem::path& workdir
) {
    if (!audio_decode_func_) {
        return resolve_fixture_audio(preset, workdir);
    }
    // Use the injected decode function for each layer
    FixtureAudioResolution resolution;
    for (const auto& layer : preset.layers) {
        if (!layer.active) continue;
        if (!layer.embedded_media_reference.has_value() || !layer.embedded_media_path.has_value()) {
            resolution.unmapped_layer_indices.push_back(std::to_string(layer.index));
            resolution.diagnostics.push_back(
                "Layer " + std::to_string(layer.index) + " has no explicit embedded media mapping.");
            continue;
        }
        if (resolution.buffers_by_reference.find(*layer.embedded_media_reference) == resolution.buffers_by_reference.end()) {
            const auto decode = audio_decode_func_(*layer.embedded_media_path, workdir);
            resolution.diagnostics.insert(resolution.diagnostics.end(),
                decode.diagnostics.begin(), decode.diagnostics.end());
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

bool AppController::import_file(const std::filesystem::path& radium_path) {
    try {
        ParseOptions options;
        options.output_root = working_directory_ / "imports";
        summary_ = parse_radium_file(radium_path, options);
        imported_preset_ = build_import_preset(summary_);
        fixture_audio_ = resolve_audio(imported_preset_, working_directory_ / "decoded");
        project_audio_buffers_ = fixture_audio_.buffers_by_reference;
        layer_overrides_.clear();
        layer_overrides_.resize(imported_preset_.layers.size());
        layer_edit_states_.assign(imported_preset_.layers.size(), std::nullopt);
        for (std::size_t i = 0; i < imported_preset_.layers.size(); ++i) {
            layer_overrides_[i].mute = imported_preset_.layers[i].mute;
            layer_overrides_[i].solo = imported_preset_.layers[i].solo;
            layer_overrides_[i].gain = imported_preset_.layers[i].gain.value_or(1.0);
            layer_overrides_[i].effects = default_effect_state_for_layer(imported_preset_.layers[i]);
        }
        // Set stereo pan defaults after audio buffers are available
        for (std::size_t i = 0; i < layer_overrides_.size(); ++i) {
            if (layer_is_stereo(i)) {
                layer_overrides_[i].pan_x = -1.0;
                layer_overrides_[i].pan_y = 1.0;
                layer_overrides_[i].pan_x_right = 1.0;
                layer_overrides_[i].pan_y_right = 1.0;
            }
        }
        bootstrap_layer_edit_states_for_audio_layers();
        selected_layer_index_.reset();
        for (std::size_t i = 0; i < imported_preset_.layers.size(); ++i) {
            if (layer_audio_buffer(i) != nullptr) {
                selected_layer_index_ = i;
                break;
            }
        }
        last_rendered_audio_.reset();
        session_recordings_.clear();
        selected_session_recording_index_.reset();
        project_picture_path_.reset();
        record_bus_mode_ = RecordBusMode::Stereo;
        streaming_mixer_.set_record_bus_channel_count(static_cast<int>(record_bus_mode_));
        reset_session_recordings_directory(radium_path);
        held_note_.reset();
        return true;
    } catch (...) {
        return false;
    }
}

bool AppController::new_empty_project(const std::string& name) {
    summary_ = FileSummary{};
    imported_preset_ = Preset{};
    imported_preset_.source_file.clear();
    imported_preset_.name = name;
    imported_preset_.slot_group_count = 8;
    imported_preset_.layers.resize(8);
    for (std::size_t i = 0; i < imported_preset_.layers.size(); ++i) {
        imported_preset_.layers[i].index = i;
        imported_preset_.layers[i].active = false;
        imported_preset_.layers[i].gain = 1.0;
    }
    fixture_audio_ = FixtureAudioResolution{};
    project_audio_buffers_.clear();
    layer_overrides_.assign(imported_preset_.layers.size(), LayerOverride{});
    layer_edit_states_.assign(imported_preset_.layers.size(), std::nullopt);
    for (auto& override_state : layer_overrides_) {
        override_state.effects = LayerEffectState{};
    }
    last_rendered_audio_.reset();
    session_recordings_.clear();
    selected_session_recording_index_.reset();
    project_picture_path_.reset();
    aux_track_state_ = AuxTrackState{};
    streaming_mixer_.clear_aux_plugin_sessions();
    streaming_mixer_.set_aux_gain(static_cast<float>(aux_track_state_.gain));
    streaming_mixer_.set_aux_bass_gain_db(static_cast<float>(aux_track_state_.bass_gain_db));
    record_bus_mode_ = RecordBusMode::Stereo;
    streaming_mixer_.set_record_bus_channel_count(static_cast<int>(record_bus_mode_));
    reset_session_recordings_directory();
    held_note_.reset();
    selected_layer_index_ = 0;
    return true;
}

bool AppController::has_imported_preset() const {
    return !imported_preset_.layers.empty();
}

std::string AppController::preset_summary_text() const {
    if (!has_imported_preset()) {
        return "No preset loaded.";
    }

    std::ostringstream out;
    out << "Preset: " << imported_preset_.name << "\n";
    out << "Source: " << imported_preset_.source_file.generic_string() << "\n";
    out << "Active layers: " << imported_preset_.active_layer_count << "/" << imported_preset_.slot_group_count << "\n";
    out << "Mapped audio layers: " << fixture_audio_.mapped_layer_indices.size() << "\n";
    out << "Loaded audio buffers: " << project_audio_buffers_.size() << "\n";
    out << "Trigger mode: " << (trigger_mode_ == TriggerMode::kOneShot ? "One-shot" : "Continuous");
    return out.str();
}

std::string AppController::diagnostics_text() const {
    if (!has_imported_preset()) {
        return "Import diagnostics will appear here.";
    }

    std::vector<std::string> lines;
    lines.insert(lines.end(), imported_preset_.diagnostics.warnings.begin(), imported_preset_.diagnostics.warnings.end());
    lines.insert(lines.end(), imported_preset_.diagnostics.uncertain_assumptions.begin(), imported_preset_.diagnostics.uncertain_assumptions.end());
    lines.insert(lines.end(), fixture_audio_.diagnostics.begin(), fixture_audio_.diagnostics.end());
    if (lines.empty()) {
        lines.push_back("No current diagnostics.");
    }
    return join_lines(lines);
}

std::string AppController::layer_debug_text(std::size_t layer_index) const {
    if (layer_index >= imported_preset_.layers.size() || layer_index >= layer_overrides_.size()) {
        return "layer-debug=invalid";
    }
    const auto& override_state = layer_overrides_[layer_index];
    std::ostringstream out;
    out << "layer-debug buffer=";
    if (override_state.active_trigger_buffer_id.has_value()) {
        out << *override_state.active_trigger_buffer_id;
    } else {
        out << "none";
    }
    out << " region=";
    if (override_state.active_trigger_region.has_value()) {
        out << override_state.active_trigger_region->start << "-" << override_state.active_trigger_region->end;
    } else {
        out << "none";
    }
    out << " duration=";
    if (override_state.active_trigger_duration_seconds.has_value()) {
        out << *override_state.active_trigger_duration_seconds;
    } else {
        out << "none";
    }
    return out.str();
}

std::vector<VisibleLayerState> AppController::visible_layers(std::size_t count) const {
    return visible_layers_from(0, count);
}

std::vector<VisibleLayerState> AppController::visible_layers_from(std::size_t start_index, std::size_t count) const {
    std::vector<VisibleLayerState> states;
    if (!has_imported_preset()) {
        return states;
    }

    if (start_index >= imported_preset_.layers.size()) {
        return states;
    }
    const auto visible_count = std::min(count, imported_preset_.layers.size() - start_index);
    states.reserve(visible_count);
    for (std::size_t visible_index = 0; visible_index < visible_count; ++visible_index) {
        const std::size_t i = start_index + visible_index;
        const auto& layer = imported_preset_.layers[i];
        const auto& override_state = layer_overrides_[i];
        VisibleLayerState state;
        state.index = layer.index;
        state.active = layer.active;
        state.mute = override_state.mute;
        state.solo = override_state.solo;
        state.locked = override_state.locked;
        state.gain = override_state.gain;
        state.has_audio = layer_audio_buffer(i) != nullptr;
        state.has_waveform = layer_audio_buffer(i) != nullptr;
        state.selected = selected_layer_index_.has_value() && *selected_layer_index_ == i;
        state.source_count = layer.sources.size();
        state.label = layer.custom_name.value_or("Layer " + std::to_string(layer.index + 1));
        states.push_back(std::move(state));
    }
    return states;
}

std::size_t AppController::layer_count() const {
    return imported_preset_.layers.size();
}

const std::vector<AppController::LayerOverride>& AppController::layer_overrides() const {
    return layer_overrides_;
}

std::optional<std::size_t> AppController::selected_layer_index() const {
    return selected_layer_index_;
}

bool AppController::select_layer(std::size_t layer_index) {
    if (layer_index >= imported_preset_.layers.size()) {
        return false;
    }
    selected_layer_index_ = layer_index;
    return true;
}

std::optional<LayerWaveformOverview> AppController::layer_waveform(
    std::size_t layer_index,
    std::size_t bucket_count,
    double view_start,
    double view_end
) const {
    if (layer_index >= imported_preset_.layers.size() || bucket_count == 0) {
        return std::nullopt;
    }
    const auto* buffer = layer_audio_buffer(layer_index);
    LayerWaveformOverview overview;
    overview.layer_index = layer_index;
    overview.audition_start = layer_overrides_[layer_index].audition_start;
    overview.loop_start = layer_overrides_[layer_index].audition_loop_start;
    overview.loop_end = layer_overrides_[layer_index].audition_loop_end;
    if (layer_overrides_[layer_index].active_trigger_region.has_value()) {
        overview.active_trigger_region = LayerWaveformOverview::AuthoredRegion{
            layer_overrides_[layer_index].active_trigger_region->start,
            layer_overrides_[layer_index].active_trigger_region->end
        };
    }
    auto sorted_regions = layer_overrides_[layer_index].trigger_regions;
    sort_trigger_regions(&sorted_regions);
    for (const auto& region : sorted_regions) {
        if (region.end > region.start + 0.001) {
            overview.authored_regions.push_back({region.start, region.end});
        }
    }
    if (buffer == nullptr || buffer->frame_count() == 0) {
        return overview;
    }

    overview.available = true;
    overview.frame_count = buffer->frame_count();
    overview.channels = buffer->channels;
    overview.sample_rate = buffer->sample_rate;
    overview.layer_delay_seconds = imported_preset_.layers[layer_index].delay.value_or(0.0);
    if (const auto* editState = layer_edit_state_ptr(layer_index)) {
        const double totalDuration = std::max(
            layer_edit_total_duration_seconds(*editState),
            static_cast<double>(buffer->frame_count()) / static_cast<double>(std::max(1, buffer->sample_rate)));
        overview.volume_automation_enabled = editState->volume_automation_enabled;
        overview.stretch_automation_enabled = editState->stretch_automation_enabled;
        overview.pan_position_automation_enabled = editState->pan_position_automation_enabled;
        overview.pan_front_back_automation_enabled = editState->pan_front_back_automation_enabled;
        overview.pan_right_position_automation_enabled = editState->pan_right_position_automation_enabled;
        overview.pan_right_front_back_automation_enabled = editState->pan_right_front_back_automation_enabled;
        overview.doppler_automation_enabled = editState->doppler_automation_enabled;
        overview.doppler_settings = sanitize_doppler_settings(editState->doppler_settings);
        overview.doppler_segment_shapes = editState->doppler_segment_shapes;
        if (totalDuration > 0.0) {
            overview.editable_clips.reserve(editState->clips.size());
            for (std::size_t clipIndex = 0; clipIndex < editState->clips.size(); ++clipIndex) {
                const auto& clip = editState->clips[clipIndex];
                const double clipStart = clip.timeline_start_seconds / totalDuration;
                const double clipEnd = layer_edit_clip_end_time(clip) / totalDuration;
                const double fadeInEnd = (clip.timeline_start_seconds + clip.fade_in_seconds) / totalDuration;
                const double fadeOutStart = (layer_edit_clip_end_time(clip) - clip.fade_out_seconds) / totalDuration;
                overview.editable_clips.push_back(LayerWaveformOverview::EditableClip{
                    clipIndex,
                    clamp01(clipStart),
                    clamp01(clipEnd),
                    clamp01(fadeInEnd),
                    clamp01(fadeOutStart)
                });
            }
            overview.volume_automation_points.reserve(editState->volume_automation_points.size());
            for (const auto& point : editState->volume_automation_points) {
                overview.volume_automation_points.push_back(
                    LayerWaveformOverview::VolumeAutomationPoint{
                        point.point_id,
                        clamp01(point.timeline_seconds / totalDuration),
                        std::clamp(point.gain, 0.0, max_automation_gain())
                    });
            }
            overview.stretch_automation_points.reserve(editState->stretch_automation_points.size());
            for (const auto& point : editState->stretch_automation_points) {
                overview.stretch_automation_points.push_back(
                    LayerWaveformOverview::StretchAutomationPoint{
                        point.point_id,
                        clamp01(point.timeline_seconds / totalDuration),
                        std::clamp(point.ratio, kStretchAutomationMinRatio, kStretchAutomationMaxRatio)
                    });
            }
            auto appendPanPoints = [totalDuration](const auto& sourcePoints, auto* destPoints) {
                destPoints->reserve(sourcePoints.size());
                for (const auto& point : sourcePoints) {
                    destPoints->push_back(LayerWaveformOverview::PanAutomationPoint{
                        point.point_id,
                        clamp01(point.timeline_seconds / totalDuration),
                        std::clamp(point.value, kPanAutomationMinValue, kPanAutomationMaxValue)
                    });
                }
            };
            appendPanPoints(editState->pan_position_automation_points,
                            &overview.pan_position_automation_points);
            appendPanPoints(editState->pan_front_back_automation_points,
                            &overview.pan_front_back_automation_points);
            appendPanPoints(editState->pan_right_position_automation_points,
                            &overview.pan_right_position_automation_points);
            appendPanPoints(editState->pan_right_front_back_automation_points,
                            &overview.pan_right_front_back_automation_points);
            appendPanPoints(editState->doppler_automation_points,
                            &overview.doppler_automation_points);
        }
    }
    overview.peaks.assign(bucket_count, 0.0f);
    overview.peaks_min.assign(bucket_count, 0.0f);
    if (buffer->channels >= 2) {
        overview.peaks_right.assign(bucket_count, 0.0f);
        overview.peaks_right_min.assign(bucket_count, 0.0f);
    }
    const double clamped_view_start = clamp01(std::min(view_start, view_end));
    const double clamped_view_end = clamp01(std::max(view_start, view_end));
    const std::size_t view_start_frame = std::min<std::size_t>(
        static_cast<std::size_t>(clamped_view_start * buffer->frame_count()),
        buffer->frame_count() - 1
    );
    const std::size_t view_end_frame = std::max<std::size_t>(
        view_start_frame + 1,
        std::min<std::size_t>(static_cast<std::size_t>(clamped_view_end * buffer->frame_count()), buffer->frame_count())
    );
    for (std::size_t bucket = 0; bucket < bucket_count; ++bucket) {
        const std::size_t start_frame = view_start_frame + bucket * (view_end_frame - view_start_frame) / bucket_count;
        const std::size_t end_frame = std::max(
            start_frame + 1,
            view_start_frame + (bucket + 1) * (view_end_frame - view_start_frame) / bucket_count
        );
        float max_left = -1.0f;
        float min_left = 1.0f;
        float max_right = -1.0f;
        float min_right = 1.0f;
        for (std::size_t frame = start_frame; frame < end_frame && frame < view_end_frame && frame < buffer->frame_count(); ++frame) {
            float sl = buffer->sample_at(frame, 0);
            max_left = std::max(max_left, sl);
            min_left = std::min(min_left, sl);
            if (buffer->channels >= 2) {
                float sr = buffer->sample_at(frame, 1);
                max_right = std::max(max_right, sr);
                min_right = std::min(min_right, sr);
            }
        }
        overview.peaks[bucket] = max_left;
        overview.peaks_min[bucket] = min_left;
        if (buffer->channels >= 2) {
            overview.peaks_right[bucket] = max_right;
            overview.peaks_right_min[bucket] = min_right;
        }
    }

    // Flip the overview into visual space when the layer is reversed so
    // waveform peaks, overlays, and playheads all read left-to-right.
    if (layer_index < layer_overrides_.size() && layer_overrides_[layer_index].effects.reverse) {
        overview.reversed = true;
        std::reverse(overview.peaks.begin(), overview.peaks.end());
        std::reverse(overview.peaks_min.begin(), overview.peaks_min.end());
        if (!overview.peaks_right.empty()) {
            std::reverse(overview.peaks_right.begin(), overview.peaks_right.end());
            std::reverse(overview.peaks_right_min.begin(), overview.peaks_right_min.end());
        }

        const auto flip_point = [](double value) {
            return clamp01(1.0 - value);
        };
        const auto flip_range = [&](double& start, double& end) {
            const double flipped_start = flip_point(end);
            const double flipped_end = flip_point(start);
            start = std::min(flipped_start, flipped_end);
            end = std::max(flipped_start, flipped_end);
        };

        overview.audition_start = flip_point(overview.audition_start);

        if (overview.loop_start.has_value() && overview.loop_end.has_value()) {
            double loop_start = *overview.loop_start;
            double loop_end = *overview.loop_end;
            flip_range(loop_start, loop_end);
            overview.loop_start = loop_start;
            overview.loop_end = loop_end;
        }

        if (overview.active_trigger_region.has_value()) {
            auto region = *overview.active_trigger_region;
            flip_range(region.start, region.end);
            overview.active_trigger_region = region;
        }

        for (auto& region : overview.authored_regions) {
            flip_range(region.start, region.end);
        }

        for (auto& clip : overview.editable_clips) {
            flip_range(clip.start, clip.end);
            const double flippedFadeIn = flip_point(clip.fade_out_start);
            const double flippedFadeOut = flip_point(clip.fade_in_end);
            clip.fade_in_end = std::clamp(std::min(flippedFadeIn, clip.end), clip.start, clip.end);
            clip.fade_out_start = std::clamp(std::max(flippedFadeOut, clip.start), clip.start, clip.end);
        }
        for (auto& point : overview.volume_automation_points) {
            point.timeline_position = flip_point(point.timeline_position);
        }
        std::sort(overview.volume_automation_points.begin(), overview.volume_automation_points.end(),
                  [](const auto& a, const auto& b) {
                      if (a.timeline_position != b.timeline_position) {
                          return a.timeline_position < b.timeline_position;
                      }
                       return a.point_id < b.point_id;
                   });
        for (auto& point : overview.stretch_automation_points) {
            point.timeline_position = flip_point(point.timeline_position);
        }
        std::sort(overview.stretch_automation_points.begin(), overview.stretch_automation_points.end(),
                  [](const auto& a, const auto& b) {
                      if (a.timeline_position != b.timeline_position) {
                          return a.timeline_position < b.timeline_position;
                      }
                      return a.point_id < b.point_id;
                  });
        auto flipPanPoints = [&](auto* points) {
            for (auto& point : *points) {
                point.timeline_position = flip_point(point.timeline_position);
            }
            std::sort(points->begin(), points->end(), [](const auto& a, const auto& b) {
                if (a.timeline_position != b.timeline_position) {
                    return a.timeline_position < b.timeline_position;
                }
                return a.point_id < b.point_id;
            });
        };
        flipPanPoints(&overview.pan_position_automation_points);
        flipPanPoints(&overview.pan_front_back_automation_points);
        flipPanPoints(&overview.pan_right_position_automation_points);
        flipPanPoints(&overview.pan_right_front_back_automation_points);
        flipPanPoints(&overview.doppler_automation_points);
    }

    return overview;
}

std::uint64_t AppController::layer_waveform_revision(std::size_t layer_index) const {
    if (layer_index >= layer_waveform_revisions_.size()) {
        return 0;
    }
    return layer_waveform_revisions_[layer_index];
}

void AppController::touch_layer_waveform_revision(std::size_t layer_index) {
    const auto requiredSize = std::max(imported_preset_.layers.size(), layer_index + 1);
    if (layer_waveform_revisions_.size() < requiredSize) {
        layer_waveform_revisions_.resize(requiredSize, 0);
    }
    ++layer_waveform_revisions_[layer_index];
}

std::optional<double> AppController::layer_timeline_duration_seconds(std::size_t layer_index) const {
    if (layer_index >= imported_preset_.layers.size()) {
        return std::nullopt;
    }
    const auto* buffer = layer_audio_buffer(layer_index);
    if (buffer == nullptr || buffer->sample_rate <= 0) {
        return std::nullopt;
    }

    double durationSeconds =
        static_cast<double>(buffer->frame_count()) / static_cast<double>(std::max(1, buffer->sample_rate));
    if (const auto* editState = layer_edit_state_ptr(layer_index)) {
        durationSeconds = std::max(durationSeconds, layer_edit_total_duration_seconds(*editState));
    }
    return durationSeconds;
}

std::optional<AppController::LayerEffectState> AppController::layer_effect_state(std::size_t layer_index) const {
    if (layer_index >= layer_overrides_.size()) {
        return std::nullopt;
    }
    return layer_overrides_[layer_index].effects;
}

std::optional<AppController::LayerOverride::Vst3PluginState> AppController::layer_vst3_plugin(std::size_t layer_index) const {
    if (layer_index >= layer_overrides_.size()) {
        return std::nullopt;
    }
    return layer_overrides_[layer_index].vst3_plugins[0];
}

std::array<std::optional<AppController::LayerOverride::Vst3PluginState>, AppController::kPluginInsertSlotCount>
AppController::layer_vst3_plugins(std::size_t layer_index) const {
    if (layer_index >= layer_overrides_.size()) {
        return {};
    }
    return layer_overrides_[layer_index].vst3_plugins;
}

void AppController::set_trigger_mode(TriggerMode mode) {
    trigger_mode_ = mode;
}

TriggerMode AppController::trigger_mode() const {
    return trigger_mode_;
}

std::optional<int> AppController::held_note_midi() const {
    if (!held_note_.has_value()) {
        return std::nullopt;
    }
    return held_note_->midi_note;
}

int AppController::octave() const {
    return octave_;
}

void AppController::set_octave(int octave) {
    octave_ = std::clamp(octave, 0, 8);
}

bool AppController::set_layer_mute(std::size_t layer_index, bool mute) {
    if (layer_index >= layer_overrides_.size()) {
        return false;
    }
    layer_overrides_[layer_index].mute = mute;
    push_live_mute(layer_index);
    return true;
}

bool AppController::set_layer_solo(std::size_t layer_index, bool solo) {
    if (layer_index >= layer_overrides_.size()) {
        return false;
    }
    layer_overrides_[layer_index].solo = solo;
    push_live_solo();
    return true;
}

bool AppController::set_layer_locked(std::size_t layer_index, bool locked) {
    if (layer_index >= layer_overrides_.size()) {
        return false;
    }
    layer_overrides_[layer_index].locked = locked;
    return true;
}

bool AppController::layer_locked(std::size_t layer_index) const {
    return layer_is_locked(layer_overrides_, layer_index);
}

bool AppController::set_layer_custom_name(std::size_t layer_index, std::optional<std::string> custom_name) {
    if (layer_index >= imported_preset_.layers.size()) {
        return false;
    }

    if (!custom_name.has_value()) {
        imported_preset_.layers[layer_index].custom_name.reset();
        return true;
    }

    auto& name = *custom_name;
    const auto first = name.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        imported_preset_.layers[layer_index].custom_name.reset();
        return true;
    }
    const auto last = name.find_last_not_of(" \t\r\n");
    imported_preset_.layers[layer_index].custom_name = name.substr(first, last - first + 1);
    return true;
}

bool AppController::set_layer_gain(std::size_t layer_index, double gain) {
    if (layer_index >= layer_overrides_.size()) {
        return false;
    }
    layer_overrides_[layer_index].gain = std::clamp(gain, 0.0, max_automation_gain());
    push_live_gain(layer_index);
    return true;
}

bool AppController::set_layer_pan(std::size_t layer_index, double x, double y) {
    if (layer_index >= layer_overrides_.size()) {
        return false;
    }
    layer_overrides_[layer_index].pan_x = std::clamp(x, -1.0, 1.0);
    layer_overrides_[layer_index].pan_y = std::clamp(y, -1.0, 1.0);
    push_live_pan(layer_index);
    return true;
}

bool AppController::set_layer_pan_right(std::size_t layer_index, double x, double y) {
    if (layer_index >= layer_overrides_.size()) {
        return false;
    }
    layer_overrides_[layer_index].pan_x_right = std::clamp(x, -1.0, 1.0);
    layer_overrides_[layer_index].pan_y_right = std::clamp(y, -1.0, 1.0);
    push_live_pan(layer_index);
    return true;
}

bool AppController::layer_is_stereo(std::size_t layer_index) const {
    const auto* buffer = layer_audio_buffer(layer_index);
    return buffer != nullptr && buffer->channels > 1;
}

bool AppController::set_layer_audition_start(std::size_t layer_index, double normalized_start) {
    if (layer_index >= layer_overrides_.size()) {
        return false;
    }
    layer_overrides_[layer_index].audition_start = clamp01(normalized_start);
    touch_layer_waveform_revision(layer_index);
    return true;
}

bool AppController::set_layer_audition_loop(std::size_t layer_index, double normalized_start, double normalized_end) {
    if (layer_index >= layer_overrides_.size()) {
        return false;
    }
    const double start = clamp01(std::min(normalized_start, normalized_end));
    const double end = clamp01(std::max(normalized_start, normalized_end));
    if (end - start < 0.002) {
        return clear_layer_audition_loop(layer_index);
    }
    layer_overrides_[layer_index].audition_start = start;
    layer_overrides_[layer_index].audition_loop_start = start;
    layer_overrides_[layer_index].audition_loop_end = end;
    touch_layer_waveform_revision(layer_index);
    return true;
}

bool AppController::clear_layer_audition_loop(std::size_t layer_index) {
    if (layer_index >= layer_overrides_.size()) {
        return false;
    }
    layer_overrides_[layer_index].audition_loop_start.reset();
    layer_overrides_[layer_index].audition_loop_end.reset();
    touch_layer_waveform_revision(layer_index);
    return true;
}

bool AppController::add_layer_trigger_region(std::size_t layer_index, double normalized_start, double normalized_end) {
    if (layer_index >= imported_preset_.layers.size()) {
        return false;
    }
    auto& layer = imported_preset_.layers[layer_index];
    if (layer.sources.empty()) {
        return false;
    }

    const double start = clamp01(std::min(normalized_start, normalized_end));
    const double end = clamp01(std::max(normalized_start, normalized_end));
    if (end - start < 0.002) {
        return false;
    }

    layer_overrides_[layer_index].trigger_regions.push_back({start, end});
    sort_trigger_regions(&layer_overrides_[layer_index].trigger_regions);
    layer_overrides_[layer_index].active_trigger_region = AppController::LayerOverride::TriggerRegion{start, end};
    layer_overrides_[layer_index].last_trigger_region_index = -1;
    touch_layer_waveform_revision(layer_index);
    layer.active = true;
    imported_preset_.active_layer_count = 0;
    for (const auto& current_layer : imported_preset_.layers) {
        if (current_layer.active) {
            ++imported_preset_.active_layer_count;
        }
    }
    return true;
}

bool AppController::update_layer_trigger_region(
    std::size_t layer_index,
    std::size_t region_index,
    double normalized_start,
    double normalized_end
) {
    if (layer_index >= imported_preset_.layers.size()) {
        return false;
    }
    auto& layer = imported_preset_.layers[layer_index];
    if (layer.sources.empty()) {
        return false;
    }
    auto& regions = layer_overrides_[layer_index].trigger_regions;
    if (region_index >= regions.size()) {
        return false;
    }

    const double start = clamp01(std::min(normalized_start, normalized_end));
    const double end = clamp01(std::max(normalized_start, normalized_end));
    if (end - start < 0.002) {
        return false;
    }

    regions[region_index] = {start, end};
    sort_trigger_regions(&regions);
    layer_overrides_[layer_index].active_trigger_region = {start, end};
    layer_overrides_[layer_index].last_trigger_region_index = -1;
    touch_layer_waveform_revision(layer_index);
    return true;
}

bool AppController::set_layer_effect_state(std::size_t layer_index, const LayerEffectState& effects) {
    if (layer_index >= layer_overrides_.size()) {
        return false;
    }
    auto clamped = effects;
    clamped.time_stretch_ratio = std::clamp(clamped.time_stretch_ratio, 0.01, 8.0);
    clamped.bass_lfe_gain_db = std::clamp(clamped.bass_lfe_gain_db, -24.0, 12.0);
    clamped.eq_low_gain_db = std::clamp(clamped.eq_low_gain_db, kLayerEqCutDb, kLayerEqBoostDb);
    clamped.eq_mid_gain_db = std::clamp(clamped.eq_mid_gain_db, kLayerEqCutDb, kLayerEqBoostDb);
    clamped.eq_high_gain_db = std::clamp(clamped.eq_high_gain_db, kLayerEqCutDb, kLayerEqBoostDb);
    layer_overrides_[layer_index].effects = clamped;
    return true;
}

bool AppController::set_layer_vst3_plugin(
    std::size_t layer_index,
    const LayerOverride::Vst3PluginState& plugin_state
) {
    return set_layer_vst3_plugin(layer_index, 0, plugin_state);
}

bool AppController::set_layer_vst3_plugin(
    std::size_t layer_index,
    std::size_t slot_index,
    const LayerOverride::Vst3PluginState& plugin_state
) {
    if (layer_index >= layer_overrides_.size() ||
        slot_index >= kPluginInsertSlotCount ||
        plugin_state.module_path.empty() ||
        plugin_state.class_id.empty()) {
        return false;
    }
    auto normalized = plugin_state;
    if (normalized.display_name.empty()) {
        normalized.display_name = std::filesystem::path(normalized.module_path).filename().string();
    }
    layer_overrides_[layer_index].vst3_plugins[slot_index] = std::move(normalized);
    return true;
}

bool AppController::clear_layer_vst3_plugin(std::size_t layer_index) {
    return clear_layer_vst3_plugin(layer_index, 0);
}

bool AppController::clear_layer_vst3_plugin(std::size_t layer_index, std::size_t slot_index) {
    if (layer_index >= layer_overrides_.size()) {
        return false;
    }
    if (slot_index >= kPluginInsertSlotCount) {
        return false;
    }
    layer_overrides_[layer_index].vst3_plugins[slot_index].reset();
    return true;
}

bool AppController::toggle_layer_vst3_bypass(std::size_t layer_index, std::size_t slot_index) {
    if (layer_index >= layer_overrides_.size() || slot_index >= kPluginInsertSlotCount) {
        return false;
    }
    auto& plugin = layer_overrides_[layer_index].vst3_plugins[slot_index];
    if (!plugin.has_value()) {
        return false;
    }
    plugin->bypassed = !plugin->bypassed;
    return true;
}

const AppController::AuxTrackState& AppController::aux_track_state() const {
    return aux_track_state_;
}

double AppController::aux_gain() const {
    return aux_track_state_.gain;
}

bool AppController::set_aux_gain(double gain) {
    aux_track_state_.gain = std::clamp(gain, 0.0, 2.0);
    streaming_mixer_.set_aux_gain(static_cast<float>(aux_track_state_.gain));
    return true;
}

double AppController::aux_bass_gain_db() const {
    return aux_track_state_.bass_gain_db;
}

bool AppController::set_aux_bass_gain_db(double gain_db) {
    aux_track_state_.bass_gain_db = std::clamp(gain_db, kAuxBassCutDb, kAuxBassBoostDb);
    streaming_mixer_.set_aux_bass_gain_db(static_cast<float>(aux_track_state_.bass_gain_db));
    return true;
}

std::array<std::optional<AppController::LayerOverride::Vst3PluginState>,
           AppController::kPluginInsertSlotCount>
AppController::aux_vst3_plugins() const {
    return aux_track_state_.vst3_plugins;
}

bool AppController::set_aux_vst3_plugin(std::size_t slot_index,
                                        const LayerOverride::Vst3PluginState& plugin_state) {
    if (slot_index >= kPluginInsertSlotCount ||
        plugin_state.module_path.empty() ||
        plugin_state.class_id.empty()) {
        return false;
    }
    auto normalized = plugin_state;
    if (normalized.display_name.empty()) {
        normalized.display_name = std::filesystem::path(normalized.module_path).filename().string();
    }
    aux_track_state_.vst3_plugins[slot_index] = std::move(normalized);
    return true;
}

bool AppController::clear_aux_vst3_plugin(std::size_t slot_index) {
    if (slot_index >= kPluginInsertSlotCount) {
        return false;
    }
    aux_track_state_.vst3_plugins[slot_index].reset();
    return true;
}

bool AppController::toggle_aux_vst3_bypass(std::size_t slot_index) {
    if (slot_index >= kPluginInsertSlotCount) {
        return false;
    }
    auto& plugin = aux_track_state_.vst3_plugins[slot_index];
    if (!plugin.has_value()) {
        return false;
    }
    plugin->bypassed = !plugin->bypassed;
    return true;
}

bool AppController::clear_layer_trigger_regions(std::size_t layer_index) {
    if (layer_index >= imported_preset_.layers.size()) {
        return false;
    }
    auto& layer = imported_preset_.layers[layer_index];
    if (layer.sources.empty()) {
        return false;
    }

    layer_overrides_[layer_index].trigger_regions.clear();
    layer_overrides_[layer_index].active_trigger_region.reset();
    layer_overrides_[layer_index].last_trigger_region_index = -1;
    touch_layer_waveform_revision(layer_index);
    return true;
}

bool AppController::auto_split_layer_regions(std::size_t layer_index, std::string* error_message) {
    if (layer_index >= imported_preset_.layers.size()) {
        if (error_message != nullptr) {
            *error_message = "Invalid layer selection.";
        }
        return false;
    }
    const auto* buffer = layer_audio_buffer(layer_index);
    if (buffer == nullptr) {
        if (error_message != nullptr) {
            *error_message = "Selected layer has no decoded audio.";
        }
        return false;
    }

    const auto regions = detect_trigger_regions(*buffer);
    if (regions.empty()) {
        if (error_message != nullptr) {
            *error_message = "Auto-split could not find repeated sound starts on this layer.";
        }
        return false;
    }

    auto& override_state = layer_overrides_[layer_index];
    override_state.trigger_regions = regions;
    sort_trigger_regions(&override_state.trigger_regions);
    override_state.active_trigger_region = override_state.trigger_regions.front();
    override_state.last_trigger_region_index = regions.size() > 1 ? 0 : -1;
    touch_layer_waveform_revision(layer_index);
    if (error_message != nullptr) {
        *error_message = "Detected " + std::to_string(regions.size()) + " trigger regions.";
    }
    return true;
}

PlaybackPreset AppController::build_playback_preset_for_note(int midi_note) {
    auto playback = build_playback_preset(imported_preset_);
    const int semitone_offset = midi_note - 60;
    for (std::size_t i = 0; i < playback.layers.size() && i < layer_overrides_.size(); ++i) {
        playback.layers[i].mute = layer_overrides_[i].mute;
        playback.layers[i].solo = layer_overrides_[i].solo;
        playback.layers[i].gain = layer_overrides_[i].gain;
        playback.layers[i].pan_x = layer_overrides_[i].pan_x;
        playback.layers[i].pan_y = layer_overrides_[i].pan_y;
        playback.layers[i].pan_x_right = layer_overrides_[i].pan_x_right;
        playback.layers[i].pan_y_right = layer_overrides_[i].pan_y_right;
        playback.layers[i].reverse = layer_overrides_[i].effects.reverse;
        playback.layers[i].coarse_semitones += static_cast<double>(semitone_offset);
        playback.layers[i].effects = PlaybackEffects{};
        playback.layers[i].effects.time_stretch_ratio = layer_overrides_[i].effects.time_stretch_ratio;
        if (const auto editedBufferId = layer_audio_buffer_id(i); editedBufferId.has_value()) {
            playback.layers[i].sources.clear();
            PlaybackSource editedSource;
            editedSource.buffer_id = *editedBufferId;
            editedSource.regions.push_back(PlaybackRegion{0.0, 1.0, 0.0, 1.0, false});
            playback.layers[i].sources.push_back(std::move(editedSource));
        }
        if (!layer_overrides_[i].trigger_regions.empty() && !playback.layers[i].sources.empty()) {
            const auto& authored_regions = layer_overrides_[i].trigger_regions;
            std::uniform_int_distribution<int> dist(0, static_cast<int>(authored_regions.size() - 1));
            int region_index = dist(region_rng_);
            if (authored_regions.size() > 1 && region_index == layer_overrides_[i].last_trigger_region_index) {
                region_index = (region_index + 1) % static_cast<int>(authored_regions.size());
            }
            layer_overrides_[i].last_trigger_region_index = region_index;
            layer_overrides_[i].active_trigger_region = authored_regions[static_cast<std::size_t>(region_index)];
            playback.layers[i].start_offset = 0.0;
            playback.layers[i].stop_offset = 1.0;
            playback.layers[i].no_immediate_repeat = false;
            PlaybackSource source;
            source.buffer_id = layer_audio_buffer_id(i).value_or(playback.layers[i].sources.front().buffer_id);
            source.regions.clear();
            const auto& region = *layer_overrides_[i].active_trigger_region;
            source.regions.push_back(PlaybackRegion{
                region.start,
                region.end,
                region.start,
                region.end,
                false
            });
            layer_overrides_[i].active_trigger_buffer_id = source.buffer_id;
            const auto buffer_it = project_audio_buffers_.find(source.buffer_id);
            if (buffer_it != project_audio_buffers_.end()) {
                const auto frames = static_cast<double>(buffer_it->second.frame_count());
                const auto sample_rate = static_cast<double>(std::max(1, buffer_it->second.sample_rate));
                layer_overrides_[i].active_trigger_duration_seconds =
                    std::max(0.0, (region.end - region.start) * frames / sample_rate);
            } else {
                layer_overrides_[i].active_trigger_duration_seconds.reset();
            }
            playback.layers[i].sources.clear();
            playback.layers[i].sources.push_back(std::move(source));
        } else {
            // No trigger regions: one-shot still sees the full rendered layer,
            // but loop mode skips the automatic edit handles at the edges.
            layer_overrides_[i].active_trigger_region.reset();
            layer_overrides_[i].active_trigger_buffer_id.reset();
            layer_overrides_[i].active_trigger_duration_seconds.reset();
            playback.layers[i].start_offset = 0.0;
            playback.layers[i].stop_offset = 1.0;
            PlaybackRegion fullRegion{0.0, 1.0, 0.0, 1.0, false};
            if (trigger_mode_ == TriggerMode::kContinuous) {
                if (const auto* editState = layer_edit_state_ptr(i)) {
                    if (auto span = layer_edit_content_span_normalized(*editState)) {
                        fullRegion = PlaybackRegion{
                            span->first,
                            span->second,
                            span->first,
                            span->second,
                            false
                        };
                    }
                }
            }
            for (auto& source : playback.layers[i].sources) {
                source.regions.clear();
                source.regions.push_back(fullRegion);
            }
        }
    }
    playback.assumptions.push_back(
        "Virtual keyboard transposition currently offsets all layers relative to MIDI note 60 without a decoded per-preset key mapping."
    );
    return playback;
}

PlaybackPreset AppController::build_single_layer_audition_preset(std::size_t layer_index, bool loop_selection) const {
    auto playback = build_playback_preset(imported_preset_);
    for (std::size_t i = 0; i < playback.layers.size() && i < layer_overrides_.size(); ++i) {
        auto& layer = playback.layers[i];
        const auto& override_state = layer_overrides_[i];
        layer.active = i == layer_index && imported_preset_.layers[i].active;
        layer.mute = false;
        layer.solo = false;
        layer.gain = override_state.gain;
        layer.pan_x = override_state.pan_x;
        layer.pan_y = override_state.pan_y;
        layer.pan_x_right = override_state.pan_x_right;
        layer.pan_y_right = override_state.pan_y_right;
        layer.reverse = override_state.effects.reverse;
        layer.coarse_semitones = 0.0;
        layer.fine_cents = 0.0;
        layer.effects = PlaybackEffects{};
        layer.effects.time_stretch_ratio = override_state.effects.time_stretch_ratio;
        if (const auto editedBufferId = layer_audio_buffer_id(i); editedBufferId.has_value()) {
            layer.sources.clear();
            PlaybackSource editedSource;
            editedSource.buffer_id = *editedBufferId;
            editedSource.regions.push_back(PlaybackRegion{0.0, 1.0, 0.0, 1.0, false});
            layer.sources.push_back(std::move(editedSource));
        }
        if (!layer.active) {
            continue;
        }

        const double audition_start = override_state.audition_start;
        layer.start_offset = audition_start;
        layer.stop_offset = 1.0;
        for (auto& source : layer.sources) {
            source.regions.clear();
            source.regions.push_back(PlaybackRegion{
                audition_start,
                1.0,
                audition_start,
                1.0,
                false
            });
        }
        if (loop_selection && override_state.audition_loop_start.has_value() && override_state.audition_loop_end.has_value()) {
            const double loop_start = *override_state.audition_loop_start;
            const double loop_end = *override_state.audition_loop_end;
            layer.start_offset = loop_start;
            layer.stop_offset = loop_end;
            for (auto& source : layer.sources) {
                source.regions.clear();
                source.regions.push_back(PlaybackRegion{
                    loop_start,
                    loop_end,
                    loop_start,
                    loop_end,
                    true
                });
            }
        }
    }
    playback.assumptions.push_back(
        "Layer audition currently renders the selected layer in isolation and applies audition click/loop points as normalized offsets over the resolved embedded audio."
    );
    return playback;
}

const AudioBuffer* AppController::layer_audio_buffer(std::size_t layer_index) const {
    if (layer_index >= imported_preset_.layers.size()) {
        return nullptr;
    }
    if (const auto* state = layer_edit_state_ptr(layer_index)) {
        if (!state->rendered_buffer_id.empty()) {
            const auto renderedIt = project_audio_buffers_.find(state->rendered_buffer_id);
            if (renderedIt != project_audio_buffers_.end()) {
                return &renderedIt->second;
            }
        }
    }
    const auto& layer = imported_preset_.layers[layer_index];
    for (const auto& source : layer.sources) {
        if (!source.buffer_id.has_value()) {
            continue;
        }
        const auto it = project_audio_buffers_.find(*source.buffer_id);
        if (it != project_audio_buffers_.end()) {
            return &it->second;
        }
    }
    if (layer.embedded_media_reference.has_value()) {
        const auto it = project_audio_buffers_.find(*layer.embedded_media_reference);
        if (it != project_audio_buffers_.end()) {
            return &it->second;
        }
    }
    return nullptr;
}

std::optional<std::string> AppController::layer_audio_buffer_id(std::size_t layer_index) const {
    if (layer_index >= imported_preset_.layers.size()) {
        return std::nullopt;
    }
    if (const auto* state = layer_edit_state_ptr(layer_index)) {
        if (!state->rendered_buffer_id.empty() &&
            project_audio_buffers_.find(state->rendered_buffer_id) != project_audio_buffers_.end()) {
            return state->rendered_buffer_id;
        }
    }
    const auto& layer = imported_preset_.layers[layer_index];
    for (const auto& source : layer.sources) {
        if (!source.buffer_id.has_value()) {
            continue;
        }
        if (project_audio_buffers_.find(*source.buffer_id) != project_audio_buffers_.end()) {
            return source.buffer_id;
        }
    }
    if (layer.embedded_media_reference.has_value()) {
        const auto it = project_audio_buffers_.find(*layer.embedded_media_reference);
        if (it != project_audio_buffers_.end()) {
            return layer.embedded_media_reference;
        }
    }
    return std::nullopt;
}

const AppController::LayerEditState* AppController::layer_edit_state_ptr(std::size_t layer_index) const {
    if (layer_index >= layer_edit_states_.size() || !layer_edit_states_[layer_index].has_value()) {
        return nullptr;
    }
    return &*layer_edit_states_[layer_index];
}

AppController::LayerEditState* AppController::layer_edit_state_ptr(std::size_t layer_index) {
    if (layer_index >= layer_edit_states_.size() || !layer_edit_states_[layer_index].has_value()) {
        return nullptr;
    }
    return &*layer_edit_states_[layer_index];
}

std::optional<AppController::LayerEditState> AppController::layer_edit_state(std::size_t layer_index) const {
    if (const auto* state = layer_edit_state_ptr(layer_index)) {
        return *state;
    }
    return std::nullopt;
}

std::optional<AppController::LayerEditState> AppController::layer_edit_state_snapshot(std::size_t layer_index) const {
    if (const auto* state = layer_edit_state_ptr(layer_index)) {
        return *state;
    }
    return bootstrap_layer_edit_state(layer_index);
}

bool AppController::restore_layer_edit_state(std::size_t layer_index, const LayerEditState& state) {
    if (layer_index >= imported_preset_.layers.size()) {
        return false;
    }
    if (layer_index >= layer_edit_states_.size()) {
        layer_edit_states_.resize(imported_preset_.layers.size());
    }

    clear_layer_edit_render(layer_index);
    layer_edit_states_[layer_index] = state;
    const bool rebuilt = rebuild_layer_edit_render(layer_index, nullptr);
    if (rebuilt) {
        touch_layer_waveform_revision(layer_index);
    }
    return rebuilt;
}

std::optional<AppController::LayerEditState> AppController::bootstrap_layer_edit_state(std::size_t layer_index) const {
    if (layer_index >= imported_preset_.layers.size()) {
        return std::nullopt;
    }

    const auto& layer = imported_preset_.layers[layer_index];
    std::optional<std::string> buffer_id;
    std::optional<std::string> source_path;
    std::string source_label;

    for (const auto& source : layer.sources) {
        if (source.buffer_id.has_value() &&
            project_audio_buffers_.find(*source.buffer_id) != project_audio_buffers_.end()) {
            buffer_id = *source.buffer_id;
            source_path = source.path;
            source_label = source.name.value_or(layer.source_name.value_or("Clip"));
            break;
        }
    }

    if (!buffer_id.has_value() &&
        layer.embedded_media_reference.has_value() &&
        project_audio_buffers_.find(*layer.embedded_media_reference) != project_audio_buffers_.end()) {
        buffer_id = *layer.embedded_media_reference;
        source_path = layer.embedded_media_path;
        source_label = layer.source_name.value_or("Clip");
    }

    if (!buffer_id.has_value()) {
        return std::nullopt;
    }

    const auto bufferIt = project_audio_buffers_.find(*buffer_id);
    if (bufferIt == project_audio_buffers_.end() || bufferIt->second.frame_count() == 0) {
        return std::nullopt;
    }

    const auto durationSeconds =
        static_cast<double>(bufferIt->second.frame_count()) /
        static_cast<double>(std::max(1, bufferIt->second.sample_rate));

    LayerEditState state;
    state.rendered_buffer_id = layer_edit_rendered_buffer_id(layer_index);
    state.clips.push_back(LayerEditClip{
        *buffer_id,
        source_path,
        source_label.empty() ? "Clip" : source_label,
        0.0,
        durationSeconds,
        kLayerEditHeadPaddingSeconds,
        0.0,
        0.0
    });
    return state;
}

bool AppController::bootstrap_layer_edit_state_if_available(std::size_t layer_index, std::string* error_message) {
    if (layer_index >= imported_preset_.layers.size()) {
        if (error_message != nullptr) {
            *error_message = "Invalid layer selection.";
        }
        return false;
    }
    if (layer_index >= layer_edit_states_.size()) {
        layer_edit_states_.resize(imported_preset_.layers.size());
    }
    if (layer_edit_states_[layer_index].has_value()) {
        return true;
    }

    auto state = bootstrap_layer_edit_state(layer_index);
    if (!state.has_value()) {
        if (error_message != nullptr) {
            *error_message = "Selected layer has no editable audio.";
        }
        return false;
    }

    layer_edit_states_[layer_index] = std::move(*state);
    std::string rebuild_error;
    if (!rebuild_layer_edit_render(layer_index, &rebuild_error)) {
        clear_layer_clip_edits(layer_index);
        if (error_message != nullptr) {
            *error_message = rebuild_error.empty()
                ? "Could not initialize editable layer audio."
                : rebuild_error;
        }
        return false;
    }
    return true;
}

void AppController::bootstrap_layer_edit_states_for_audio_layers() {
    if (layer_edit_states_.size() < imported_preset_.layers.size()) {
        layer_edit_states_.resize(imported_preset_.layers.size());
    }

    for (std::size_t i = 0; i < imported_preset_.layers.size(); ++i) {
        if (layer_edit_states_[i].has_value()) {
            continue;
        }
        if (layer_audio_buffer(i) == nullptr) {
            continue;
        }

        std::string error;
        if (!bootstrap_layer_edit_state_if_available(i, &error) && !error.empty()) {
            fixture_audio_.diagnostics.push_back(
                "Editable layer bootstrap failed for layer " + std::to_string(i) + ": " + error);
        }
    }
}

bool AppController::ensure_layer_edit_state(std::size_t layer_index, std::string* error_message) {
    if (layer_index >= imported_preset_.layers.size()) {
        if (error_message != nullptr) {
            *error_message = "Invalid layer selection.";
        }
        return false;
    }
    if (layer_index >= layer_edit_states_.size()) {
        layer_edit_states_.resize(imported_preset_.layers.size());
    }
    if (!layer_edit_states_[layer_index].has_value()) {
        return bootstrap_layer_edit_state_if_available(layer_index, error_message);
    }
    return true;
}

double AppController::layer_edit_total_duration_seconds(const LayerEditState& state) const {
    double maxEnd = state.head_padding_seconds;
    for (const auto& clip : state.clips) {
        maxEnd = std::max(maxEnd, layer_edit_clip_end_time(clip));
    }
    return std::max(maxEnd + state.tail_padding_seconds, state.head_padding_seconds + state.tail_padding_seconds);
}

std::optional<std::size_t> AppController::find_layer_edit_clip_at(std::size_t layer_index, double timeline_seconds) const {
    const auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr) {
        return std::nullopt;
    }

    for (std::size_t i = 0; i < state->clips.size(); ++i) {
        const auto& clip = state->clips[i];
        if (timeline_seconds >= clip.timeline_start_seconds &&
            timeline_seconds <= layer_edit_clip_end_time(clip)) {
            return i;
        }
    }
    return std::nullopt;
}

RenderedAudio AppController::render_layer_edit_audio(std::size_t layer_index, std::string* error_message) const {
    RenderedAudio rendered;
    const auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr || state->clips.empty()) {
        if (error_message != nullptr) {
            *error_message = "Layer has no clip edits.";
        }
        return rendered;
    }

    const auto firstBufferIt = project_audio_buffers_.find(state->clips.front().source_buffer_id);
    if (firstBufferIt == project_audio_buffers_.end() || firstBufferIt->second.frame_count() == 0) {
        if (error_message != nullptr) {
            *error_message = "Edited layer source audio is missing.";
        }
        return rendered;
    }

    rendered.sample_rate = firstBufferIt->second.sample_rate;
    rendered.channels = firstBufferIt->second.channels;
    const auto totalDurationSeconds = layer_edit_total_duration_seconds(*state);
    const auto totalFrames = static_cast<std::size_t>(std::ceil(
        totalDurationSeconds * static_cast<double>(std::max(1, rendered.sample_rate))));
    rendered.samples.assign(totalFrames * static_cast<std::size_t>(rendered.channels), 0.0f);

    for (const auto& clip : state->clips) {
        const auto bufferIt = project_audio_buffers_.find(clip.source_buffer_id);
        if (bufferIt == project_audio_buffers_.end()) {
            continue;
        }

        const auto& sourceBuffer = bufferIt->second;
        if (sourceBuffer.sample_rate != rendered.sample_rate || sourceBuffer.channels != rendered.channels) {
            if (error_message != nullptr) {
                *error_message = "Edited clips currently require matching sample rates and channel counts.";
            }
            return RenderedAudio{};
        }

        const auto sourceStartFrame = std::min<std::size_t>(
            static_cast<std::size_t>(std::floor(std::max(0.0, clip.source_start_seconds) * sourceBuffer.sample_rate)),
            sourceBuffer.frame_count());
        const auto sourceEndFrame = std::min<std::size_t>(
            static_cast<std::size_t>(std::ceil(std::max(clip.source_start_seconds, clip.source_end_seconds) * sourceBuffer.sample_rate)),
            sourceBuffer.frame_count());

        if (sourceEndFrame <= sourceStartFrame) {
            continue;
        }

        const auto clipFrames = sourceEndFrame - sourceStartFrame;
        const auto destStartFrame = static_cast<std::size_t>(std::round(
            std::max(0.0, clip.timeline_start_seconds) * static_cast<double>(rendered.sample_rate)));
        const auto fadeInFrames = static_cast<std::size_t>(std::round(
            std::clamp(clip.fade_in_seconds, 0.0, layer_edit_clip_duration_seconds(clip)) *
            static_cast<double>(rendered.sample_rate)));
        const auto fadeOutFrames = static_cast<std::size_t>(std::round(
            std::clamp(clip.fade_out_seconds, 0.0, layer_edit_clip_duration_seconds(clip)) *
            static_cast<double>(rendered.sample_rate)));

        for (std::size_t frame = 0; frame < clipFrames; ++frame) {
            const auto destFrame = destStartFrame + frame;
            if (destFrame >= totalFrames) {
                break;
            }

            const float gain = clip_fade_gain(frame, clipFrames, fadeInFrames, fadeOutFrames);
            for (int channel = 0; channel < rendered.channels; ++channel) {
                const auto destIndex =
                    destFrame * static_cast<std::size_t>(rendered.channels) + static_cast<std::size_t>(channel);
                rendered.samples[destIndex] +=
                    sourceBuffer.sample_at(sourceStartFrame + frame, channel) * gain;
            }
        }
    }

    if (state->volume_automation_enabled && !state->volume_automation_points.empty()) {
        for (std::size_t frame = 0; frame < totalFrames; ++frame) {
            const double timelineSeconds =
                static_cast<double>(frame) / static_cast<double>(std::max(1, rendered.sample_rate));
            const float automationGain = static_cast<float>(evaluate_volume_automation_gain(*state, timelineSeconds));
            for (int channel = 0; channel < rendered.channels; ++channel) {
                const auto sampleIndex =
                    frame * static_cast<std::size_t>(rendered.channels) + static_cast<std::size_t>(channel);
                rendered.samples[sampleIndex] *= automationGain;
            }
        }
    }

    for (auto& sample : rendered.samples) {
        sample = std::clamp(sample, -1.0f, 1.0f);
    }
    return rendered;
}

bool AppController::rebuild_layer_edit_render(std::size_t layer_index, std::string* error_message) {
    if (!ensure_layer_edit_state(layer_index, error_message)) {
        return false;
    }

    if (streaming_active_) {
        stop_streaming_playback();
    }

    auto rendered = render_layer_edit_audio(layer_index, error_message);
    if (rendered.frame_count() == 0) {
        return false;
    }

    AudioBuffer buffer;
    buffer.sample_rate = rendered.sample_rate;
    buffer.channels = rendered.channels;
    buffer.samples = rendered.samples;

    auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr) {
        return false;
    }
    project_audio_buffers_[state->rendered_buffer_id] = std::move(buffer);
    touch_layer_waveform_revision(layer_index);
    return true;
}

bool AppController::commit_layer_edit_changes(std::size_t layer_index, std::optional<std::size_t> priority_clip_index) {
    std::vector<std::size_t> priority_indices;
    if (priority_clip_index.has_value()) {
        priority_indices.push_back(*priority_clip_index);
    }
    return commit_layer_edit_changes(layer_index, priority_indices);
}

bool AppController::commit_layer_edit_changes(std::size_t layer_index, const std::vector<std::size_t>& priority_clip_indices) {
    auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr) {
        return false;
    }

    resolve_layer_edit_overlaps(state, priority_clip_indices);
    return rebuild_layer_edit_render(layer_index, nullptr);
}

void AppController::clear_layer_edit_render(std::size_t layer_index) {
    if (const auto* state = layer_edit_state_ptr(layer_index)) {
        if (!state->rendered_buffer_id.empty()) {
            project_audio_buffers_.erase(state->rendered_buffer_id);
        }
    }
    touch_layer_waveform_revision(layer_index);
}

bool AppController::layer_has_clip_edits(std::size_t layer_index) const {
    const auto* state = layer_edit_state_ptr(layer_index);
    return state != nullptr && !state->clips.empty();
}

void AppController::clear_layer_clip_edits(std::size_t layer_index) {
    if (layer_index >= layer_edit_states_.size()) {
        return;
    }
    clear_layer_edit_render(layer_index);
    layer_edit_states_[layer_index].reset();
    touch_layer_waveform_revision(layer_index);
}

bool AppController::split_layer_edit_clip(std::size_t layer_index, double normalized_timeline) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }

    auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr || state->clips.empty()) {
        return false;
    }

    const double totalDuration = layer_edit_total_duration_seconds(*state);
    const double timelineSeconds = std::clamp(normalized_timeline, 0.0, 1.0) * totalDuration;
    const auto clipIndex = find_layer_edit_clip_at(layer_index, timelineSeconds);
    if (!clipIndex.has_value()) {
        return false;
    }

    auto clip = state->clips[*clipIndex];
    const double clipEnd = layer_edit_clip_end_time(clip);
    if (timelineSeconds <= clip.timeline_start_seconds + 0.005 || timelineSeconds >= clipEnd - 0.005) {
        return false;
    }

    const double splitOffset = timelineSeconds - clip.timeline_start_seconds;
    const double splitSource = clip.source_start_seconds + splitOffset;

    LayerEditClip left = clip;
    LayerEditClip right = clip;
    left.source_end_seconds = splitSource;
    left.fade_out_seconds = 0.0;
    right.source_start_seconds = splitSource;
    right.timeline_start_seconds = timelineSeconds;
    right.fade_in_seconds = 0.0;

    state->clips.erase(state->clips.begin() + static_cast<std::ptrdiff_t>(*clipIndex));
    state->clips.insert(state->clips.begin() + static_cast<std::ptrdiff_t>(*clipIndex), right);
    state->clips.insert(state->clips.begin() + static_cast<std::ptrdiff_t>(*clipIndex), left);
    return rebuild_layer_edit_render(layer_index, nullptr);
}

bool AppController::trim_layer_edit_left(std::size_t layer_index, double normalized_timeline) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr) {
        return false;
    }

    const double totalDuration = layer_edit_total_duration_seconds(*state);
    const double timelineSeconds = std::clamp(normalized_timeline, 0.0, 1.0) * totalDuration;
    const auto clipIndex = find_layer_edit_clip_at(layer_index, timelineSeconds);
    if (!clipIndex.has_value()) {
        return false;
    }

    auto& clip = state->clips[*clipIndex];
    const double clipEnd = layer_edit_clip_end_time(clip);
    if (timelineSeconds >= clipEnd - 0.005) {
        return false;
    }

    const double delta = std::max(0.0, timelineSeconds - clip.timeline_start_seconds);
    clip.timeline_start_seconds = timelineSeconds;
    clip.source_start_seconds = std::min(clip.source_end_seconds, clip.source_start_seconds + delta);
    clip.fade_in_seconds = std::min(clip.fade_in_seconds, layer_edit_clip_duration_seconds(clip));
    clip.fade_out_seconds = std::min(clip.fade_out_seconds, layer_edit_clip_duration_seconds(clip));
    return rebuild_layer_edit_render(layer_index, nullptr);
}

bool AppController::trim_layer_edit_right(std::size_t layer_index, double normalized_timeline) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr) {
        return false;
    }

    const double totalDuration = layer_edit_total_duration_seconds(*state);
    const double timelineSeconds = std::clamp(normalized_timeline, 0.0, 1.0) * totalDuration;
    const auto clipIndex = find_layer_edit_clip_at(layer_index, timelineSeconds);
    if (!clipIndex.has_value()) {
        return false;
    }

    auto& clip = state->clips[*clipIndex];
    if (timelineSeconds <= clip.timeline_start_seconds + 0.005) {
        return false;
    }

    const double delta = std::max(0.0, layer_edit_clip_end_time(clip) - timelineSeconds);
    clip.source_end_seconds = std::max(clip.source_start_seconds, clip.source_end_seconds - delta);
    clip.fade_in_seconds = std::min(clip.fade_in_seconds, layer_edit_clip_duration_seconds(clip));
    clip.fade_out_seconds = std::min(clip.fade_out_seconds, layer_edit_clip_duration_seconds(clip));
    return rebuild_layer_edit_render(layer_index, nullptr);
}

bool AppController::set_layer_edit_fade_in(std::size_t layer_index, double normalized_timeline) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr) {
        return false;
    }

    const double totalDuration = layer_edit_total_duration_seconds(*state);
    const double timelineSeconds = std::clamp(normalized_timeline, 0.0, 1.0) * totalDuration;
    const auto clipIndex = find_layer_edit_clip_at(layer_index, timelineSeconds);
    if (!clipIndex.has_value()) {
        return false;
    }

    auto& clip = state->clips[*clipIndex];
    clip.fade_in_seconds = std::clamp(
        timelineSeconds - clip.timeline_start_seconds,
        0.0,
        layer_edit_clip_duration_seconds(clip));
    return rebuild_layer_edit_render(layer_index, nullptr);
}

bool AppController::set_layer_edit_fade_out(std::size_t layer_index, double normalized_timeline) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr) {
        return false;
    }

    const double totalDuration = layer_edit_total_duration_seconds(*state);
    const double timelineSeconds = std::clamp(normalized_timeline, 0.0, 1.0) * totalDuration;
    const auto clipIndex = find_layer_edit_clip_at(layer_index, timelineSeconds);
    if (!clipIndex.has_value()) {
        return false;
    }

    auto& clip = state->clips[*clipIndex];
    clip.fade_out_seconds = std::clamp(
        layer_edit_clip_end_time(clip) - timelineSeconds,
        0.0,
        layer_edit_clip_duration_seconds(clip));
    return rebuild_layer_edit_render(layer_index, nullptr);
}

bool AppController::apply_layer_edit_crossfade(std::size_t layer_index, double normalized_start, double normalized_end) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr || state->clips.size() < 2) {
        return false;
    }

    const double totalDuration = layer_edit_total_duration_seconds(*state);
    double selectionStart = std::clamp(std::min(normalized_start, normalized_end), 0.0, 1.0) * totalDuration;
    double selectionEnd = std::clamp(std::max(normalized_start, normalized_end), 0.0, 1.0) * totalDuration;
    if (selectionEnd <= selectionStart + 0.005) {
        return false;
    }

    std::sort(state->clips.begin(), state->clips.end(), [](const auto& a, const auto& b) {
        return a.timeline_start_seconds < b.timeline_start_seconds;
    });

    for (std::size_t i = 0; i + 1 < state->clips.size(); ++i) {
        auto& left = state->clips[i];
        auto& right = state->clips[i + 1];
        const double boundary = layer_edit_clip_end_time(left);
        if (selectionStart <= boundary && selectionEnd >= right.timeline_start_seconds) {
            const double overlap = selectionEnd - selectionStart;
            left.source_end_seconds = std::min(left.source_end_seconds + std::max(0.0, selectionEnd - boundary),
                                               left.source_end_seconds + overlap);
            right.source_start_seconds = std::max(0.0, right.source_start_seconds - std::max(0.0, right.timeline_start_seconds - selectionStart));
            right.timeline_start_seconds = selectionStart;
            left.fade_out_seconds = std::clamp(layer_edit_clip_end_time(left) - selectionStart, 0.0, layer_edit_clip_duration_seconds(left));
            right.fade_in_seconds = std::clamp(selectionEnd - right.timeline_start_seconds, 0.0, layer_edit_clip_duration_seconds(right));
            return rebuild_layer_edit_render(layer_index, nullptr);
        }
    }

    return false;
}

bool AppController::enable_layer_volume_automation(std::size_t layer_index) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr) {
        return false;
    }
    state->volume_automation_enabled = true;
    sort_volume_automation_points(&state->volume_automation_points);
    return true;
}

bool AppController::layer_volume_automation_enabled(std::size_t layer_index) const {
    const auto* state = layer_edit_state_ptr(layer_index);
    return state != nullptr && state->volume_automation_enabled;
}

std::optional<std::size_t> AppController::add_layer_volume_automation_point(
    std::size_t layer_index,
    double normalized_timeline
) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return std::nullopt;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr) {
        return std::nullopt;
    }

    state->volume_automation_enabled = true;
    const double totalDuration = layer_edit_total_duration_seconds(*state);
    const double timelineSeconds = std::clamp(normalized_timeline, 0.0, 1.0) * totalDuration;
    const double currentGain = evaluate_volume_automation_gain(*state, timelineSeconds);
    const auto createdId = next_volume_automation_point_id(*state);
    state->volume_automation_points.push_back(
        LayerEditState::VolumeAutomationPoint{createdId, timelineSeconds, currentGain});
    sort_volume_automation_points(&state->volume_automation_points);

    for (const auto& point : state->volume_automation_points) {
        if (std::abs(point.timeline_seconds - timelineSeconds) < 0.0005 &&
            std::abs(point.gain - currentGain) < 0.0005 &&
            point.point_id == createdId) {
            return point.point_id;
        }
    }
    return std::nullopt;
}

bool AppController::move_layer_volume_automation_point(
    std::size_t layer_index,
    std::size_t point_id,
    double normalized_timeline,
    double gain,
    bool rebuild_render
) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr) {
        return false;
    }

    auto pointIt = std::find_if(state->volume_automation_points.begin(),
                                state->volume_automation_points.end(),
                                [point_id](const auto& point) { return point.point_id == point_id; });
    if (pointIt == state->volume_automation_points.end()) {
        return false;
    }

    const double totalDuration = layer_edit_total_duration_seconds(*state);
    state->volume_automation_enabled = true;
    pointIt->timeline_seconds =
        std::clamp(normalized_timeline, 0.0, 1.0) * totalDuration;
    pointIt->gain = std::clamp(gain, 0.0, max_automation_gain());
    sort_volume_automation_points(&state->volume_automation_points);

    if (!rebuild_render) {
        return true;
    }
    return rebuild_layer_edit_render(layer_index, nullptr);
}

bool AppController::delete_layer_volume_automation_point(std::size_t layer_index, std::size_t point_id) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr) {
        return false;
    }

    const auto oldSize = state->volume_automation_points.size();
    state->volume_automation_points.erase(
        std::remove_if(state->volume_automation_points.begin(),
                       state->volume_automation_points.end(),
                       [point_id](const auto& point) { return point.point_id == point_id; }),
        state->volume_automation_points.end());
    if (state->volume_automation_points.size() == oldSize) {
        return false;
    }

    if (state->volume_automation_points.empty()) {
        state->volume_automation_enabled = false;
    }
    return rebuild_layer_edit_render(layer_index, nullptr);
}

bool AppController::commit_layer_volume_automation(std::size_t layer_index) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    return rebuild_layer_edit_render(layer_index, nullptr);
}

bool AppController::reset_layer_volume_automation(std::size_t layer_index) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr) {
        return false;
    }

    const double totalDuration = std::max(layer_edit_total_duration_seconds(*state), 0.001);
    const double baseGain =
        layer_index < layer_overrides_.size() ? static_cast<double>(layer_overrides_[layer_index].gain) : 1.0;
    const auto firstId = next_volume_automation_point_id(*state);
    state->volume_automation_enabled = true;
    state->volume_automation_points = {
        LayerEditState::VolumeAutomationPoint{firstId, 0.0, baseGain},
        LayerEditState::VolumeAutomationPoint{firstId + 1, totalDuration, baseGain},
    };
    sort_volume_automation_points(&state->volume_automation_points);
    return rebuild_layer_edit_render(layer_index, nullptr);
}

bool AppController::remove_layer_volume_automation(std::size_t layer_index) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr) {
        return false;
    }

    state->volume_automation_enabled = false;
    state->volume_automation_points.clear();
    return rebuild_layer_edit_render(layer_index, nullptr);
}

std::optional<VolumeRandomSettings> AppController::layer_volume_random_settings(
    std::size_t layer_index
) const {
    if (layer_index >= imported_preset_.layers.size()) {
        return std::nullopt;
    }
    if (const auto* state = layer_edit_state_ptr(layer_index)) {
        return sanitize_volume_random_settings(state->volume_random_settings);
    }
    return sanitize_volume_random_settings(VolumeRandomSettings{});
}

bool AppController::set_layer_volume_random_settings(
    std::size_t layer_index,
    const VolumeRandomSettings& settings
) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr) {
        return false;
    }
    state->volume_random_settings = sanitize_volume_random_settings(settings);

    if (streaming_active_ && layer_index < streaming_layers_.size()) {
        auto& streamingLayer = streaming_layers_[layer_index];
        streamingLayer.volume_random_settings.enabled = state->volume_random_settings.enabled;
        streamingLayer.volume_random_settings.loudest_db =
            static_cast<float>(state->volume_random_settings.loudest_db);
        streamingLayer.volume_random_settings.quietest_db =
            static_cast<float>(state->volume_random_settings.quietest_db);
        streamingLayer.volume_random_settings.period_longest_seconds =
            static_cast<float>(state->volume_random_settings.period_longest_seconds);
        streamingLayer.volume_random_settings.period_shortest_seconds =
            static_cast<float>(state->volume_random_settings.period_shortest_seconds);
        streamingLayer.volume_random_settings.smoothing =
            static_cast<float>(state->volume_random_settings.smoothing);
        streamingLayer.volume_random_runtime = {};
        streamingLayer.volume_random_runtime.rng.seed(
            static_cast<std::uint32_t>(0x9E3779B9u ^ static_cast<std::uint32_t>(layer_index * 2654435761u)));
    }
    return true;
}

std::optional<PanRandomSettings> AppController::layer_pan_random_settings(
    std::size_t layer_index
) const {
    if (layer_index >= imported_preset_.layers.size()) {
        return std::nullopt;
    }
    if (const auto* state = layer_edit_state_ptr(layer_index)) {
        return sanitize_pan_random_settings(state->pan_random_settings);
    }
    return sanitize_pan_random_settings(PanRandomSettings{});
}

bool AppController::set_layer_pan_random_settings(
    std::size_t layer_index,
    const PanRandomSettings& settings
) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr) {
        return false;
    }
    state->pan_random_settings = sanitize_pan_random_settings(settings);

    if (streaming_active_ && layer_index < streaming_layers_.size()) {
        auto& streamingLayer = streaming_layers_[layer_index];
        streamingLayer.pan_random_settings.enabled = state->pan_random_settings.enabled;
        streamingLayer.pan_random_settings.farthest_left =
            static_cast<float>(state->pan_random_settings.farthest_left);
        streamingLayer.pan_random_settings.farthest_right =
            static_cast<float>(state->pan_random_settings.farthest_right);
        streamingLayer.pan_random_settings.farthest_front =
            static_cast<float>(state->pan_random_settings.farthest_front);
        streamingLayer.pan_random_settings.farthest_back =
            static_cast<float>(state->pan_random_settings.farthest_back);
        streamingLayer.pan_random_settings.speed =
            static_cast<float>(state->pan_random_settings.speed);
        streamingLayer.pan_random_settings.smoothing =
            static_cast<float>(state->pan_random_settings.smoothing);
        streamingLayer.pan_random_runtime = {};
        streamingLayer.pan_random_runtime.rng.seed(
            static_cast<std::uint32_t>(0x85EBCA6Bu ^ static_cast<std::uint32_t>(layer_index * 2246822519u)));
    }
    return true;
}

std::optional<StretchRandomSettings> AppController::layer_stretch_random_settings(
    std::size_t layer_index
) const {
    if (layer_index >= imported_preset_.layers.size()) {
        return std::nullopt;
    }
    if (const auto* state = layer_edit_state_ptr(layer_index)) {
        return sanitize_stretch_random_settings(state->stretch_random_settings);
    }
    return sanitize_stretch_random_settings(StretchRandomSettings{});
}

bool AppController::set_layer_stretch_random_settings(
    std::size_t layer_index,
    const StretchRandomSettings& settings
) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr) {
        return false;
    }
    state->stretch_random_settings = sanitize_stretch_random_settings(settings);

    if (streaming_active_ && layer_index < streaming_layers_.size()) {
        auto& streamingLayer = streaming_layers_[layer_index];
        streamingLayer.stretch_random_settings.enabled = state->stretch_random_settings.enabled;
        streamingLayer.stretch_random_settings.lowest_percent =
            static_cast<float>(state->stretch_random_settings.lowest_percent);
        streamingLayer.stretch_random_settings.highest_percent =
            static_cast<float>(state->stretch_random_settings.highest_percent);
        streamingLayer.stretch_random_settings.speed =
            static_cast<float>(state->stretch_random_settings.speed);
        streamingLayer.stretch_random_settings.smoothing =
            static_cast<float>(state->stretch_random_settings.smoothing);
        streamingLayer.stretch_random_runtime = {};
        streamingLayer.stretch_random_runtime.rng.seed(
            static_cast<std::uint32_t>(0xC2B2AE35u ^ static_cast<std::uint32_t>(layer_index * 3266489917u)));
    }
    return true;
}

bool AppController::enable_layer_stretch_automation(std::size_t layer_index) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr) {
        return false;
    }
    state->stretch_automation_enabled = true;
    sort_stretch_automation_points(&state->stretch_automation_points);
    return true;
}

bool AppController::layer_stretch_automation_enabled(std::size_t layer_index) const {
    const auto* state = layer_edit_state_ptr(layer_index);
    return state != nullptr && state->stretch_automation_enabled;
}

std::optional<std::size_t> AppController::add_layer_stretch_automation_point(
    std::size_t layer_index,
    double normalized_timeline
) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return std::nullopt;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr) {
        return std::nullopt;
    }

    state->stretch_automation_enabled = true;
    const double totalDuration = layer_edit_total_duration_seconds(*state);
    const double timelineSeconds = std::clamp(normalized_timeline, 0.0, 1.0) * totalDuration;
    const double fallbackRatio =
        (layer_index < layer_overrides_.size())
            ? layer_overrides_[layer_index].effects.time_stretch_ratio
            : 1.0;
    const double currentRatio = evaluate_stretch_automation_ratio(*state, timelineSeconds, fallbackRatio);
    const auto createdId = next_stretch_automation_point_id(*state);
    state->stretch_automation_points.push_back(
        LayerEditState::StretchAutomationPoint{createdId, timelineSeconds, currentRatio});
    sort_stretch_automation_points(&state->stretch_automation_points);

    for (const auto& point : state->stretch_automation_points) {
        if (std::abs(point.timeline_seconds - timelineSeconds) < 0.0005 &&
            std::abs(point.ratio - currentRatio) < 0.0005 &&
            point.point_id == createdId) {
            return point.point_id;
        }
    }
    return std::nullopt;
}

bool AppController::move_layer_stretch_automation_point(
    std::size_t layer_index,
    std::size_t point_id,
    double normalized_timeline,
    double ratio
) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr) {
        return false;
    }

    auto pointIt = std::find_if(state->stretch_automation_points.begin(),
                                state->stretch_automation_points.end(),
                                [point_id](const auto& point) { return point.point_id == point_id; });
    if (pointIt == state->stretch_automation_points.end()) {
        return false;
    }

    const double totalDuration = layer_edit_total_duration_seconds(*state);
    state->stretch_automation_enabled = true;
    pointIt->timeline_seconds = std::clamp(normalized_timeline, 0.0, 1.0) * totalDuration;
    pointIt->ratio = std::clamp(ratio, kStretchAutomationMinRatio, kStretchAutomationMaxRatio);
    sort_stretch_automation_points(&state->stretch_automation_points);

    if (streaming_active_ && layer_index < streaming_layers_.size()) {
        auto& streamingLayer = streaming_layers_[layer_index];
        streamingLayer.stretch_automation_enabled = state->stretch_automation_enabled;
        streamingLayer.stretch_automation_points.clear();
        if (totalDuration > 0.0) {
            streamingLayer.stretch_automation_points.reserve(state->stretch_automation_points.size());
            for (const auto& point : state->stretch_automation_points) {
                streamingLayer.stretch_automation_points.push_back(
                    StreamingMixer::LayerState::StretchAutomationPoint{
                        std::clamp(point.timeline_seconds / totalDuration, 0.0, 1.0),
                        static_cast<float>(std::clamp(point.ratio,
                                                      kStretchAutomationMinRatio,
                                                      kStretchAutomationMaxRatio))
                    });
            }
        }
    }

    return true;
}

bool AppController::delete_layer_stretch_automation_point(std::size_t layer_index, std::size_t point_id) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr) {
        return false;
    }

    const auto oldSize = state->stretch_automation_points.size();
    state->stretch_automation_points.erase(
        std::remove_if(state->stretch_automation_points.begin(),
                       state->stretch_automation_points.end(),
                       [point_id](const auto& point) { return point.point_id == point_id; }),
        state->stretch_automation_points.end());
    if (state->stretch_automation_points.size() == oldSize) {
        return false;
    }

    if (state->stretch_automation_points.empty()) {
        state->stretch_automation_enabled = false;
    }

    if (streaming_active_ && layer_index < streaming_layers_.size()) {
        auto& streamingLayer = streaming_layers_[layer_index];
        streamingLayer.stretch_automation_enabled = state->stretch_automation_enabled;
        streamingLayer.stretch_automation_points.clear();
        const double totalDuration = layer_edit_total_duration_seconds(*state);
        if (totalDuration > 0.0) {
            streamingLayer.stretch_automation_points.reserve(state->stretch_automation_points.size());
            for (const auto& point : state->stretch_automation_points) {
                streamingLayer.stretch_automation_points.push_back(
                    StreamingMixer::LayerState::StretchAutomationPoint{
                        std::clamp(point.timeline_seconds / totalDuration, 0.0, 1.0),
                        static_cast<float>(std::clamp(point.ratio,
                                                      kStretchAutomationMinRatio,
                                                      kStretchAutomationMaxRatio))
                    });
            }
        }
    }
    return true;
}

bool AppController::commit_layer_stretch_automation(std::size_t layer_index) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }

    auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr) {
        return false;
    }

    sort_stretch_automation_points(&state->stretch_automation_points);
    if (streaming_active_ && layer_index < streaming_layers_.size()) {
        auto& streamingLayer = streaming_layers_[layer_index];
        streamingLayer.stretch_automation_enabled = state->stretch_automation_enabled;
        streamingLayer.stretch_automation_points.clear();
        const double totalDuration = layer_edit_total_duration_seconds(*state);
        if (totalDuration > 0.0) {
            streamingLayer.stretch_automation_points.reserve(state->stretch_automation_points.size());
            for (const auto& point : state->stretch_automation_points) {
                streamingLayer.stretch_automation_points.push_back(
                    StreamingMixer::LayerState::StretchAutomationPoint{
                        std::clamp(point.timeline_seconds / totalDuration, 0.0, 1.0),
                        static_cast<float>(std::clamp(point.ratio,
                                                      kStretchAutomationMinRatio,
                                                      kStretchAutomationMaxRatio))
                    });
            }
        }
    }
    return true;
}

bool AppController::reset_layer_stretch_automation(std::size_t layer_index) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr) {
        return false;
    }

    const double totalDuration = std::max(layer_edit_total_duration_seconds(*state), 0.001);
    const auto fx = layer_effect_state(layer_index);
    const double baseRatio = fx.has_value() ? fx->time_stretch_ratio : 1.0;
    const auto firstId = next_stretch_automation_point_id(*state);
    state->stretch_automation_enabled = true;
    state->stretch_automation_points = {
        LayerEditState::StretchAutomationPoint{firstId, 0.0, baseRatio},
        LayerEditState::StretchAutomationPoint{firstId + 1, totalDuration, baseRatio},
    };
    sort_stretch_automation_points(&state->stretch_automation_points);
    return commit_layer_stretch_automation(layer_index);
}

bool AppController::remove_layer_stretch_automation(std::size_t layer_index) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr) {
        return false;
    }

    state->stretch_automation_enabled = false;
    state->stretch_automation_points.clear();
    return commit_layer_stretch_automation(layer_index);
}

bool AppController::enable_layer_pan_automation(std::size_t layer_index, PanAutomationTarget target) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    auto [enabled, points] = pan_automation_storage(state, target);
    if (enabled == nullptr || points == nullptr) {
        return false;
    }
    *enabled = true;
    sort_pan_automation_points(points);
    return true;
}

bool AppController::layer_pan_automation_enabled(std::size_t layer_index, PanAutomationTarget target) const {
    const auto* state = layer_edit_state_ptr(layer_index);
    const auto [enabled, points] = pan_automation_storage(state, target);
    return enabled != nullptr && points != nullptr && *enabled;
}

std::optional<std::size_t> AppController::add_layer_pan_automation_point(
    std::size_t layer_index,
    PanAutomationTarget target,
    double normalized_timeline
) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return std::nullopt;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    auto [enabled, points] = pan_automation_storage(state, target);
    if (enabled == nullptr || points == nullptr) {
        return std::nullopt;
    }

    *enabled = true;
    const double totalDuration = layer_edit_total_duration_seconds(*state);
    const double timelineSeconds = std::clamp(normalized_timeline, 0.0, 1.0) * totalDuration;
    double fallbackValue = 0.0;
    if (layer_index < layer_overrides_.size()) {
        const auto& overrideState = layer_overrides_[layer_index];
        switch (target) {
            case PanAutomationTarget::Position: fallbackValue = overrideState.pan_x; break;
            case PanAutomationTarget::FrontBack: fallbackValue = overrideState.pan_y; break;
            case PanAutomationTarget::RightPosition: fallbackValue = overrideState.pan_x_right; break;
            case PanAutomationTarget::RightFrontBack: fallbackValue = overrideState.pan_y_right; break;
        }
    }
    const double currentValue = evaluate_pan_automation_value(*state, target, timelineSeconds, fallbackValue);
    const auto createdId = next_pan_automation_point_id(*state);
    points->push_back(LayerEditState::PanAutomationPoint{createdId, timelineSeconds, currentValue});
    sort_pan_automation_points(points);

    for (const auto& point : *points) {
        if (std::abs(point.timeline_seconds - timelineSeconds) < 0.0005 &&
            std::abs(point.value - currentValue) < 0.0005 &&
            point.point_id == createdId) {
            return point.point_id;
        }
    }
    return std::nullopt;
}

bool AppController::move_layer_pan_automation_point(
    std::size_t layer_index,
    PanAutomationTarget target,
    std::size_t point_id,
    double normalized_timeline,
    double value
) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    auto [enabled, points] = pan_automation_storage(state, target);
    if (enabled == nullptr || points == nullptr) {
        return false;
    }

    auto pointIt = std::find_if(points->begin(), points->end(),
                                [point_id](const auto& point) { return point.point_id == point_id; });
    if (pointIt == points->end()) {
        return false;
    }

    *enabled = true;
    const double totalDuration = layer_edit_total_duration_seconds(*state);
    pointIt->timeline_seconds = std::clamp(normalized_timeline, 0.0, 1.0) * totalDuration;
    pointIt->value = std::clamp(value, kPanAutomationMinValue, kPanAutomationMaxValue);
    sort_pan_automation_points(points);

    return commit_layer_pan_automation(layer_index, target);
}

bool AppController::delete_layer_pan_automation_point(
    std::size_t layer_index,
    PanAutomationTarget target,
    std::size_t point_id
) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    auto [enabled, points] = pan_automation_storage(state, target);
    if (enabled == nullptr || points == nullptr) {
        return false;
    }

    const auto oldSize = points->size();
    points->erase(std::remove_if(points->begin(), points->end(),
                                 [point_id](const auto& point) { return point.point_id == point_id; }),
                  points->end());
    if (points->size() == oldSize) {
        return false;
    }
    if (points->empty()) {
        *enabled = false;
    }
    return commit_layer_pan_automation(layer_index, target);
}

bool AppController::commit_layer_pan_automation(std::size_t layer_index, PanAutomationTarget target) {
    auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr) {
        return false;
    }
    auto [enabled, points] = pan_automation_storage(state, target);
    if (enabled == nullptr || points == nullptr) {
        return false;
    }
    sort_pan_automation_points(points);

    if (streaming_active_ && layer_index < streaming_layers_.size()) {
        auto* streamingLayer = &streaming_layers_[layer_index];
        const double totalDuration = std::max(0.000001, layer_edit_total_duration_seconds(*state));
        auto sync = [&](bool enabledFlag,
                        const auto& sourcePoints,
                        bool* targetEnabled,
                        auto* targetPoints) {
            *targetEnabled = enabledFlag;
            targetPoints->clear();
            targetPoints->reserve(sourcePoints.size());
            for (const auto& point : sourcePoints) {
                targetPoints->push_back(StreamingMixer::LayerState::PanAutomationPoint{
                    point.point_id,
                    point.timeline_seconds / totalDuration,
                    static_cast<float>(std::clamp(point.value, kPanAutomationMinValue, kPanAutomationMaxValue))
                });
            }
        };
        sync(state->pan_position_automation_enabled,
             state->pan_position_automation_points,
             &streamingLayer->pan_position_automation_enabled,
             &streamingLayer->pan_position_automation_points);
        sync(state->pan_front_back_automation_enabled,
             state->pan_front_back_automation_points,
             &streamingLayer->pan_front_back_automation_enabled,
             &streamingLayer->pan_front_back_automation_points);
        sync(state->pan_right_position_automation_enabled,
             state->pan_right_position_automation_points,
             &streamingLayer->pan_right_position_automation_enabled,
             &streamingLayer->pan_right_position_automation_points);
        sync(state->pan_right_front_back_automation_enabled,
             state->pan_right_front_back_automation_points,
             &streamingLayer->pan_right_front_back_automation_enabled,
             &streamingLayer->pan_right_front_back_automation_points);
    }
    return true;
}

bool AppController::reset_layer_pan_automation(std::size_t layer_index, PanAutomationTarget target) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    auto [enabled, points] = pan_automation_storage(state, target);
    if (enabled == nullptr || points == nullptr) {
        return false;
    }

    const double totalDuration = std::max(layer_edit_total_duration_seconds(*state), 0.001);
    double baseValue = 0.0;
    if (layer_index < layer_overrides_.size()) {
        switch (target) {
            case PanAutomationTarget::Position: baseValue = layer_overrides_[layer_index].pan_x; break;
            case PanAutomationTarget::FrontBack: baseValue = layer_overrides_[layer_index].pan_y; break;
            case PanAutomationTarget::RightPosition: baseValue = layer_overrides_[layer_index].pan_x_right; break;
            case PanAutomationTarget::RightFrontBack: baseValue = layer_overrides_[layer_index].pan_y_right; break;
        }
    }

    const auto firstId = next_pan_automation_point_id(*state);
    *enabled = true;
    *points = {
        LayerEditState::PanAutomationPoint{firstId, 0.0, baseValue},
        LayerEditState::PanAutomationPoint{firstId + 1, totalDuration, baseValue},
    };
    sort_pan_automation_points(points);
    return commit_layer_pan_automation(layer_index, target);
}

bool AppController::remove_layer_pan_automation(std::size_t layer_index, PanAutomationTarget target) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    auto [enabled, points] = pan_automation_storage(state, target);
    if (enabled == nullptr || points == nullptr) {
        return false;
    }

    *enabled = false;
    points->clear();
    return commit_layer_pan_automation(layer_index, target);
}

bool AppController::enable_layer_doppler_automation(std::size_t layer_index) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    auto [enabled, points] = doppler_automation_storage(state);
    if (enabled == nullptr || points == nullptr) {
        return false;
    }
    *enabled = true;
    sort_pan_automation_points(points);
    return true;
}

bool AppController::layer_doppler_automation_enabled(std::size_t layer_index) const {
    const auto* state = layer_edit_state_ptr(layer_index);
    const auto [enabled, points] = doppler_automation_storage(state);
    return enabled != nullptr && points != nullptr && *enabled;
}

std::optional<std::size_t> AppController::add_layer_doppler_automation_point(
    std::size_t layer_index,
    double normalized_timeline
) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return std::nullopt;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    auto [enabled, points] = doppler_automation_storage(state);
    if (enabled == nullptr || points == nullptr) {
        return std::nullopt;
    }

    *enabled = true;
    const double totalDuration = layer_edit_total_duration_seconds(*state);
    const double timelineSeconds = std::clamp(normalized_timeline, 0.0, 1.0) * totalDuration;
    const double currentValue = evaluate_doppler_automation_value(*state, timelineSeconds, 0.0);
    const auto createdId = next_pan_automation_point_id(*state);
    points->push_back(LayerEditState::PanAutomationPoint{createdId, timelineSeconds, currentValue});
    sort_pan_automation_points(points);

    for (const auto& point : *points) {
        if (std::abs(point.timeline_seconds - timelineSeconds) < 0.0005 &&
            std::abs(point.value - currentValue) < 0.0005 &&
            point.point_id == createdId) {
            return point.point_id;
        }
    }
    return std::nullopt;
}

bool AppController::move_layer_doppler_automation_point(
    std::size_t layer_index,
    std::size_t point_id,
    double normalized_timeline,
    double value
) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    auto [enabled, points] = doppler_automation_storage(state);
    if (enabled == nullptr || points == nullptr) {
        return false;
    }

    auto pointIt = std::find_if(points->begin(), points->end(),
                                [point_id](const auto& point) { return point.point_id == point_id; });
    if (pointIt == points->end()) {
        return false;
    }

    *enabled = true;
    const double totalDuration = layer_edit_total_duration_seconds(*state);
    pointIt->timeline_seconds = std::clamp(normalized_timeline, 0.0, 1.0) * totalDuration;
    pointIt->value = std::clamp(value, kPanAutomationMinValue, kPanAutomationMaxValue);
    sort_pan_automation_points(points);
    return commit_layer_doppler_automation(layer_index);
}

bool AppController::delete_layer_doppler_automation_point(std::size_t layer_index, std::size_t point_id) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    auto [enabled, points] = doppler_automation_storage(state);
    if (enabled == nullptr || points == nullptr) {
        return false;
    }

    const auto oldSize = points->size();
    points->erase(std::remove_if(points->begin(), points->end(),
                                 [point_id](const auto& point) { return point.point_id == point_id; }),
                  points->end());
    if (points->size() == oldSize) {
        return false;
    }
    if (points->empty()) {
        *enabled = false;
    }
    return commit_layer_doppler_automation(layer_index);
}

bool AppController::commit_layer_doppler_automation(std::size_t layer_index) {
    auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr) {
        return false;
    }
    prune_doppler_segment_shapes(state);
    auto [enabled, points] = doppler_automation_storage(state);
    if (enabled == nullptr || points == nullptr) {
        return false;
    }
    sort_pan_automation_points(points);

    if (streaming_active_ && layer_index < streaming_layers_.size()) {
        auto& streamingLayer = streaming_layers_[layer_index];
        streamingLayer.doppler_automation_enabled = *enabled;
        streamingLayer.doppler_settings.edge_gain_db =
            static_cast<float>(state->doppler_settings.edge_gain_db);
        streamingLayer.doppler_settings.center_gain_db =
            static_cast<float>(state->doppler_settings.center_gain_db);
        streamingLayer.doppler_settings.edge_pitch_semitones =
            static_cast<float>(state->doppler_settings.edge_pitch_semitones);
        streamingLayer.doppler_settings.center_pitch_semitones =
            static_cast<float>(state->doppler_settings.center_pitch_semitones);
        streamingLayer.doppler_segment_shapes.clear();
        streamingLayer.doppler_segment_shapes.reserve(state->doppler_segment_shapes.size());
        for (const auto& shape : state->doppler_segment_shapes) {
            streamingLayer.doppler_segment_shapes.push_back(
                StreamingMixer::LayerState::DopplerSegmentShape{
                    shape.left_point_id,
                    static_cast<int>(shape.curve_type),
                    static_cast<float>(std::clamp(shape.curve_amount, 0.0, 1.0))});
        }
        streamingLayer.doppler_automation_points.clear();
        const double totalDuration = std::max(0.000001, layer_edit_total_duration_seconds(*state));
        streamingLayer.doppler_automation_points.reserve(points->size());
        for (const auto& point : *points) {
            streamingLayer.doppler_automation_points.push_back(
                StreamingMixer::LayerState::PanAutomationPoint{
                    point.point_id,
                    std::clamp(point.timeline_seconds / totalDuration, 0.0, 1.0),
                    static_cast<float>(std::clamp(point.value, kPanAutomationMinValue, kPanAutomationMaxValue))
                });
        }
    }
    return true;
}

bool AppController::replace_layer_doppler_automation_points(
    std::size_t layer_index,
    const std::vector<std::pair<double, double>>& points
) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    auto [enabled, storedPoints] = doppler_automation_storage(state);
    if (enabled == nullptr || storedPoints == nullptr) {
        return false;
    }

    if (points.empty()) {
        *enabled = false;
        storedPoints->clear();
        state->doppler_segment_shapes.clear();
        return commit_layer_doppler_automation(layer_index);
    }

    *enabled = true;
    storedPoints->clear();
    state->doppler_segment_shapes.clear();

    const double totalDuration = std::max(layer_edit_total_duration_seconds(*state), 0.001);
    std::size_t nextId = next_pan_automation_point_id(*state);
    storedPoints->reserve(points.size());
    for (const auto& point : points) {
        storedPoints->push_back(LayerEditState::PanAutomationPoint{
            nextId++,
            std::clamp(point.first, 0.0, 1.0) * totalDuration,
            std::clamp(point.second, kPanAutomationMinValue, kPanAutomationMaxValue)});
    }

    sort_pan_automation_points(storedPoints);
    storedPoints->erase(
        std::unique(
            storedPoints->begin(),
            storedPoints->end(),
            [](const auto& left, const auto& right) {
                return std::abs(left.timeline_seconds - right.timeline_seconds) < 0.0005;
            }),
        storedPoints->end());

    return commit_layer_doppler_automation(layer_index);
}

bool AppController::reset_layer_doppler_automation(std::size_t layer_index) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    auto [enabled, points] = doppler_automation_storage(state);
    if (enabled == nullptr || points == nullptr) {
        return false;
    }

    const double totalDuration = std::max(layer_edit_total_duration_seconds(*state), 0.001);
    const auto firstId = next_pan_automation_point_id(*state);
    *enabled = true;
    *points = {
        LayerEditState::PanAutomationPoint{firstId, 0.0, 0.0},
        LayerEditState::PanAutomationPoint{firstId + 1, totalDuration, 0.0},
    };
    state->doppler_segment_shapes.clear();
    sort_pan_automation_points(points);
    return commit_layer_doppler_automation(layer_index);
}

bool AppController::remove_layer_doppler_automation(std::size_t layer_index) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    auto [enabled, points] = doppler_automation_storage(state);
    if (enabled == nullptr || points == nullptr) {
        return false;
    }

    *enabled = false;
    points->clear();
    state->doppler_segment_shapes.clear();
    return commit_layer_doppler_automation(layer_index);
}

std::optional<DopplerSettings> AppController::layer_doppler_settings(
    std::size_t layer_index
) const {
    if (layer_index >= imported_preset_.layers.size()) {
        return std::nullopt;
    }
    if (const auto* state = layer_edit_state_ptr(layer_index)) {
        return sanitize_doppler_settings(state->doppler_settings);
    }
    return sanitize_doppler_settings(DopplerSettings{});
}

bool AppController::set_layer_doppler_settings(
    std::size_t layer_index,
    const DopplerSettings& settings
) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr) {
        return false;
    }
    state->doppler_settings = sanitize_doppler_settings(settings);
    return commit_layer_doppler_automation(layer_index);
}

std::optional<DopplerSegmentShape> AppController::layer_doppler_segment_shape(
    std::size_t layer_index,
    std::size_t left_point_id
) const {
    const auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr) {
        return std::nullopt;
    }
    const auto it = std::find_if(state->doppler_segment_shapes.begin(),
                                 state->doppler_segment_shapes.end(),
                                 [left_point_id](const auto& shape) {
                                     return shape.left_point_id == left_point_id;
                                 });
    if (it == state->doppler_segment_shapes.end()) {
        return std::nullopt;
    }
    return *it;
}

bool AppController::set_layer_doppler_segment_shape(
    std::size_t layer_index,
    std::size_t left_point_id,
    DopplerCurveType curve_type,
    double curve_amount
) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr) {
        return false;
    }

    const double clampedAmount = std::clamp(curve_amount, 0.0, 1.0);
    auto it = std::find_if(state->doppler_segment_shapes.begin(),
                           state->doppler_segment_shapes.end(),
                           [left_point_id](const auto& shape) {
                               return shape.left_point_id == left_point_id;
                           });
    if (it == state->doppler_segment_shapes.end()) {
        state->doppler_segment_shapes.push_back(
            DopplerSegmentShape{left_point_id, curve_type, clampedAmount});
    } else {
        it->curve_type = curve_type;
        it->curve_amount = clampedAmount;
    }
    return commit_layer_doppler_automation(layer_index);
}

bool AppController::move_layer_edit_clip(std::size_t layer_index,
                                         std::size_t clip_index,
                                         double timeline_delta_seconds,
                                         bool rebuild_render) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr || clip_index >= state->clips.size()) {
        return false;
    }

    const bool had_previous_crossfade =
        clip_index > 0 && clips_form_crossfade(state->clips[clip_index - 1], state->clips[clip_index]);
    const bool had_next_crossfade =
        clip_index + 1 < state->clips.size() &&
        clips_form_crossfade(state->clips[clip_index], state->clips[clip_index + 1]);

    auto& clip = state->clips[clip_index];
    clip.timeline_start_seconds = std::max(0.0, clip.timeline_start_seconds + timeline_delta_seconds);

    if (had_previous_crossfade) {
        preserve_crossfade_pair_after_move(state, clip_index - 1, clip_index);
    }
    if (had_next_crossfade) {
        preserve_crossfade_pair_after_move(state, clip_index, clip_index + 1);
    }

    if (!rebuild_render) {
        return true;
    }
    return commit_layer_edit_changes(layer_index, clip_index);
}

bool AppController::move_layer_edit_clips(std::size_t layer_index,
                                          const std::vector<std::size_t>& clip_indices,
                                          double timeline_delta_seconds,
                                          bool rebuild_render) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr || clip_indices.empty()) {
        return false;
    }

    std::vector<std::size_t> valid_indices;
    valid_indices.reserve(clip_indices.size());
    std::vector<bool> seen(state->clips.size(), false);
    for (const auto clip_index : clip_indices) {
        if (clip_index < state->clips.size() && !seen[clip_index]) {
            seen[clip_index] = true;
            valid_indices.push_back(clip_index);
        }
    }
    if (valid_indices.empty()) {
        return false;
    }

    double minimum_start = std::numeric_limits<double>::max();
    for (const auto clip_index : valid_indices) {
        minimum_start = std::min(minimum_start, state->clips[clip_index].timeline_start_seconds);
    }
    const double effective_delta = std::max(timeline_delta_seconds, -minimum_start);

    std::vector<std::pair<std::size_t, std::size_t>> preserved_crossfade_pairs;
    for (std::size_t i = 0; i + 1 < state->clips.size(); ++i) {
        if ((seen[i] || seen[i + 1]) && clips_form_crossfade(state->clips[i], state->clips[i + 1])) {
            preserved_crossfade_pairs.emplace_back(i, i + 1);
        }
    }

    for (const auto clip_index : valid_indices) {
        state->clips[clip_index].timeline_start_seconds =
            std::max(0.0, state->clips[clip_index].timeline_start_seconds + effective_delta);
    }

    for (const auto& pair : preserved_crossfade_pairs) {
        preserve_crossfade_pair_after_move(state, pair.first, pair.second);
    }

    if (!rebuild_render) {
        return true;
    }
    return commit_layer_edit_changes(layer_index, valid_indices);
}

bool AppController::trim_layer_edit_clip_edge(std::size_t layer_index,
                                              std::size_t clip_index,
                                              bool trim_left_edge,
                                              double normalized_timeline,
                                              bool rebuild_render) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr || clip_index >= state->clips.size()) {
        return false;
    }

    const double totalDuration = layer_edit_total_duration_seconds(*state);
    const double timelineSeconds = std::clamp(normalized_timeline, 0.0, 1.0) * totalDuration;
    auto& clip = state->clips[clip_index];

    if (trim_left_edge) {
        const double clipEnd = layer_edit_clip_end_time(clip);
        if (timelineSeconds >= clipEnd - 0.005) {
            return false;
        }
        const double delta = timelineSeconds - clip.timeline_start_seconds;
        clip.timeline_start_seconds = std::max(0.0, timelineSeconds);
        clip.source_start_seconds = std::clamp(
            clip.source_start_seconds + delta,
            0.0,
            clip.source_end_seconds - 0.005);
        clip.fade_in_seconds = std::min(clip.fade_in_seconds, layer_edit_clip_duration_seconds(clip));
    } else {
        if (timelineSeconds <= clip.timeline_start_seconds + 0.005) {
            return false;
        }
        const double desiredDuration = timelineSeconds - clip.timeline_start_seconds;
        clip.source_end_seconds = std::max(clip.source_start_seconds + 0.005,
                                           clip.source_start_seconds + desiredDuration);
        clip.fade_out_seconds = std::min(clip.fade_out_seconds, layer_edit_clip_duration_seconds(clip));
    }

    if (!rebuild_render) {
        return true;
    }
    return commit_layer_edit_changes(layer_index);
}

bool AppController::set_layer_edit_clip_fade(std::size_t layer_index,
                                             std::size_t clip_index,
                                             bool fade_in,
                                             double normalized_timeline,
                                             bool rebuild_render) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr || clip_index >= state->clips.size()) {
        return false;
    }

    const double totalDuration = layer_edit_total_duration_seconds(*state);
    const double timelineSeconds = std::clamp(normalized_timeline, 0.0, 1.0) * totalDuration;
    auto& clip = state->clips[clip_index];
    const double duration = layer_edit_clip_duration_seconds(clip);

    if (!fade_in && clip_index + 1 < state->clips.size() &&
        clips_form_crossfade(clip, state->clips[clip_index + 1])) {
        auto& right = state->clips[clip_index + 1];
        const double leftEnd = layer_edit_clip_end_time(clip);
        const double maxCrossfade = std::min(duration, layer_edit_clip_duration_seconds(right));
        const double desiredOverlap = std::clamp(leftEnd - timelineSeconds, 0.0, maxCrossfade);
        right.timeline_start_seconds = leftEnd - desiredOverlap;
        clip.fade_out_seconds = desiredOverlap;
        right.fade_in_seconds = desiredOverlap;
        if (!rebuild_render) {
            return true;
        }
        return commit_layer_edit_changes(layer_index);
    }

    if (fade_in && clip_index > 0 &&
        clips_form_crossfade(state->clips[clip_index - 1], clip)) {
        auto& left = state->clips[clip_index - 1];
        const double leftEnd = layer_edit_clip_end_time(left);
        const double maxCrossfade = std::min(layer_edit_clip_duration_seconds(left), duration);
        const double desiredOverlap = std::clamp(leftEnd - timelineSeconds, 0.0, maxCrossfade);
        clip.timeline_start_seconds = leftEnd - desiredOverlap;
        left.fade_out_seconds = desiredOverlap;
        clip.fade_in_seconds = desiredOverlap;
        if (!rebuild_render) {
            return true;
        }
        return commit_layer_edit_changes(layer_index);
    }

    if (fade_in) {
        clip.fade_in_seconds = std::clamp(timelineSeconds - clip.timeline_start_seconds, 0.0, duration);
    } else {
        clip.fade_out_seconds = std::clamp(layer_edit_clip_end_time(clip) - timelineSeconds, 0.0, duration);
    }
    if (!rebuild_render) {
        return true;
    }
    return commit_layer_edit_changes(layer_index);
}

bool AppController::clear_layer_edit_clip_fade(std::size_t layer_index,
                                               std::size_t clip_index,
                                               bool fade_in) {
    std::string error;
    if (!ensure_layer_edit_state(layer_index, &error)) {
        return false;
    }
    auto* state = layer_edit_state_ptr(layer_index);
    if (state == nullptr || clip_index >= state->clips.size()) {
        return false;
    }

    if (fade_in) {
        state->clips[clip_index].fade_in_seconds = 0.0;
    } else {
        state->clips[clip_index].fade_out_seconds = 0.0;
    }
    return commit_layer_edit_changes(layer_index);
}

std::optional<RenderedAudio> AppController::render_one_shot_for_note(int midi_note, std::string* error_message) {
    if (fixture_audio_.buffers_by_reference.empty()) {
        if (project_audio_buffers_.empty()) {
            if (error_message != nullptr) {
                *error_message = "No decoded audio is available for playback.";
            }
            return std::nullopt;
        }
    }
    const auto playback = build_playback_preset_for_note(midi_note);
    return playback_engine_.render_one_shot(playback, project_audio_buffers_);
}

std::optional<RenderedAudio> AppController::render_continuous_for_note(
    int midi_note,
    std::size_t hold_samples,
    std::string* error_message
) {
    if (project_audio_buffers_.empty()) {
        if (error_message != nullptr) {
            *error_message = "No decoded audio is available for playback.";
        }
        return std::nullopt;
    }
    const auto playback = build_playback_preset_for_note(midi_note);
    return playback_engine_.render_continuous(playback, project_audio_buffers_, hold_samples);
}

bool AppController::start_streaming_audition(std::size_t layer_index, std::string* error_message) {
    if (!has_imported_preset()) {
        if (error_message != nullptr) {
            *error_message = "No preset loaded.";
        }
        return false;
    }
    if (project_audio_buffers_.empty()) {
        if (error_message != nullptr) {
            *error_message = "No decoded audio is available for playback.";
        }
        return false;
    }

    const bool has_loop =
        layer_overrides_[layer_index].audition_loop_start.has_value() &&
        layer_overrides_[layer_index].audition_loop_end.has_value();

    const auto playback = build_single_layer_audition_preset(layer_index, has_loop);
    return start_streaming_preset(playback);
}

std::vector<RenderedLayerAudio> AppController::render_one_shot_layer_audio(
    int midi_note,
    std::string* error_message
) {
    std::vector<RenderedLayerAudio> rendered_layers;
    if (fixture_audio_.buffers_by_reference.empty() && project_audio_buffers_.empty()) {
        if (error_message != nullptr) {
            *error_message = "No decoded audio is available for playback.";
        }
        return rendered_layers;
    }

    const auto playback = build_playback_preset_for_note(midi_note);
    const bool any_solo = std::any_of(playback.layers.begin(), playback.layers.end(), [](const auto& layer) {
        return layer.active && layer.solo;
    });
    rendered_layers.reserve(playback.layers.size());
    for (std::size_t i = 0; i < playback.layers.size(); ++i) {
        if (!playback.layers[i].active || playback.layers[i].mute || (any_solo && !playback.layers[i].solo)) {
            continue;
        }
        PlaybackPreset single = playback;
        for (std::size_t layer_index = 0; layer_index < single.layers.size(); ++layer_index) {
            single.layers[layer_index].active = layer_index == i && single.layers[layer_index].active;
        }
        rendered_layers.push_back(RenderedLayerAudio{
            i,
            playback_engine_.render_one_shot(single, project_audio_buffers_)
        });
    }
    if (rendered_layers.empty() && error_message != nullptr) {
        *error_message = "No active layers were available for rendering.";
    }
    return rendered_layers;
}

std::filesystem::path AppController::write_preview_audio_file(const RenderedAudio& audio, const std::string& stem) const {
    return write_temp_audio(audio, stem, true);
}

void AppController::set_last_rendered_audio(const RenderedAudio& audio) {
    last_rendered_audio_ = audio;
}

std::optional<RenderedAudio> AppController::last_rendered_audio() const {
    return last_rendered_audio_;
}


std::filesystem::path AppController::write_temp_audio(const RenderedAudio& audio, const std::string& stem, bool preview_compatible) const {
    const auto target = working_directory_ / (stem + ".wav");
    if (preview_compatible) {
        PlaybackEngine::write_wav_16(target, audio);
    } else {
        PlaybackEngine::write_wav_24(target, audio);
    }
    return target;
}

RenderedAudio AppController::slice_audio_from_frame(const RenderedAudio& audio, std::size_t start_frame) {
    RenderedAudio sliced;
    sliced.sample_rate = audio.sample_rate;
    sliced.channels = audio.channels;
    if (start_frame >= audio.frame_count()) {
        return sliced;
    }
    const auto start_index = start_frame * static_cast<std::size_t>(audio.channels);
    sliced.samples.assign(audio.samples.begin() + static_cast<std::ptrdiff_t>(start_index), audio.samples.end());
    return sliced;
}

TriggerResult AppController::trigger_note_on(int midi_note) {
    TriggerResult result;
    if (!has_imported_preset()) {
        result.message = "No preset loaded.";
        return result;
    }

    std::string error;
    if (trigger_mode_ == TriggerMode::kOneShot) {
        auto audio = render_one_shot_for_note(midi_note, &error);
        if (!audio.has_value()) {
            result.message = error;
            return result;
        }
        last_rendered_audio_ = *audio;
        result.audio_path = write_temp_audio(*audio, "last_trigger", true);
        result.duration_seconds = static_cast<double>(audio->frame_count()) / static_cast<double>(audio->sample_rate);
        result.success = true;
        result.message = "Triggered one-shot.";
        return result;
    }

    constexpr std::size_t kMaxHoldSamples = 48000 * 30;
    auto audio = render_continuous_for_note(midi_note, kMaxHoldSamples, &error);
    if (!audio.has_value()) {
        result.message = error;
        return result;
    }
    last_rendered_audio_ = *audio;
    held_note_ = HeldNoteState{midi_note, std::chrono::steady_clock::now()};
    result.audio_path = write_temp_audio(*audio, "continuous_preview", true);
    result.duration_seconds = static_cast<double>(audio->frame_count()) / static_cast<double>(audio->sample_rate);
    result.success = true;
    result.message = "Continuous trigger started.";
    return result;
}

TriggerResult AppController::trigger_note_off(int midi_note) {
    TriggerResult result;
    if (trigger_mode_ != TriggerMode::kContinuous || !held_note_.has_value() || held_note_->midi_note != midi_note) {
        result.message = "No matching held note.";
        return result;
    }

    const auto held_duration = std::chrono::steady_clock::now() - held_note_->started_at;
    const auto hold_samples = static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(held_duration).count() * 48
    );

    std::string error;
    auto full_audio = render_continuous_for_note(midi_note, hold_samples, &error);
    held_note_.reset();
    if (!full_audio.has_value()) {
        result.message = error;
        return result;
    }

    auto release_tail = slice_audio_from_frame(*full_audio, hold_samples);
    last_rendered_audio_ = *full_audio;
    result.audio_path = write_temp_audio(release_tail, "continuous_release", true);
    result.duration_seconds = static_cast<double>(release_tail.frame_count()) / static_cast<double>(release_tail.sample_rate);
    result.success = true;
    result.message = "Continuous trigger released.";
    return result;
}

bool AppController::export_one_shot_wav(const std::filesystem::path& output_path, std::string* error_message) {
    auto audio = render_one_shot_for_note(60 + (octave_ - 4) * 12, error_message);
    if (!audio.has_value()) {
        return false;
    }
    PlaybackEngine::write_wav_24(output_path, *audio);
    last_rendered_audio_ = *audio;
    return true;
}

bool AppController::record_last_trigger_to_wav(const std::filesystem::path& output_path, std::string* error_message) {
    if (!last_rendered_audio_.has_value()) {
        if (error_message != nullptr) {
            *error_message = "No trigger has been rendered yet.";
        }
        return false;
    }
    PlaybackEngine::write_wav_24(output_path, *last_rendered_audio_);
    return true;
}

bool AppController::add_audio_file_to_layer(
    std::size_t layer_index,
    const std::filesystem::path& audio_path,
    std::string* error_message
) {
    if (!has_imported_preset()) {
        new_empty_project("Untitled Project");
    }
    if (layer_index >= imported_preset_.layers.size()) {
        if (error_message != nullptr) {
            *error_message = "Invalid layer selection.";
        }
        return false;
    }

    clear_layer_clip_edits(layer_index);

    const std::string buffer_id = std::filesystem::absolute(audio_path).string();
    if (project_audio_buffers_.find(buffer_id) == project_audio_buffers_.end()) {
        const auto decode = decode_audio(audio_path, working_directory_ / "decoded_local");
        fixture_audio_.diagnostics.insert(
            fixture_audio_.diagnostics.end(),
            decode.diagnostics.begin(),
            decode.diagnostics.end()
        );
        if (!decode.success) {
            if (error_message != nullptr) {
                *error_message = decode.diagnostics.empty() ? "Audio decode failed." : decode.diagnostics.front();
            }
            return false;
        }
        project_audio_buffers_[buffer_id] = decode.audio;
    }

    auto& layer = imported_preset_.layers[layer_index];
    layer.active = true;
    if (!layer.gain.has_value()) {
        layer.gain = 1.0;
    }
    LayerSource source;
    source.name = audio_path.stem().string();
    source.path = std::filesystem::absolute(audio_path).string();
    source.file = audio_path.filename().string();
    source.buffer_id = buffer_id;
    source.regions.push_back(Region{});
    layer.sources.push_back(std::move(source));
    layer.source_name = audio_path.stem().string();
    layer.source_path = std::filesystem::absolute(audio_path).string();
    layer.source_file = audio_path.filename().string();
    if (layer.regions.empty()) {
        layer.regions.push_back(Region{});
    }

    imported_preset_.active_layer_count = 0;
    for (const auto& current_layer : imported_preset_.layers) {
        if (current_layer.active) {
            ++imported_preset_.active_layer_count;
        }
    }
    // Set stereo pan defaults if the added audio is stereo
    if (layer_is_stereo(layer_index) && layer_overrides_[layer_index].pan_x == 0.0 && layer_overrides_[layer_index].pan_y == 1.0) {
        layer_overrides_[layer_index].pan_x = -1.0;
        layer_overrides_[layer_index].pan_x_right = 1.0;
        layer_overrides_[layer_index].pan_y_right = 1.0;
    }
    if (!bootstrap_layer_edit_state_if_available(layer_index, error_message)) {
        return false;
    }
    selected_layer_index_ = layer_index;
    return true;
}

bool AppController::replace_layer_audio(
    std::size_t layer_index,
    const std::filesystem::path& audio_path,
    std::string* error_message
) {
    if (!has_imported_preset()) {
        new_empty_project("Untitled Project");
    }
    if (layer_index >= imported_preset_.layers.size()) {
        if (error_message != nullptr) {
            *error_message = "Invalid layer selection.";
        }
        return false;
    }

    // Streaming voices reference decoded buffers directly, so stop playback
    // before we mutate a layer's source list or reclaim any shared buffers.
    if (streaming_active_) {
        stop_streaming_playback();
    }

    clear_layer_clip_edits(layer_index);

    // Remove this layer's source references, but only reclaim decoded buffers
    // that are no longer referenced by any other layer.
    auto& layer = imported_preset_.layers[layer_index];
    std::vector<std::string> candidate_buffer_ids;
    candidate_buffer_ids.reserve(layer.sources.size() + 1);
    for (const auto& src : layer.sources) {
        if (src.buffer_id.has_value() && !src.buffer_id->empty()) {
            candidate_buffer_ids.push_back(*src.buffer_id);
        }
    }
    if (layer.embedded_media_reference.has_value() && !layer.embedded_media_reference->empty()) {
        candidate_buffer_ids.push_back(*layer.embedded_media_reference);
    }

    layer.sources.clear();
    layer.regions.clear();
    layer.source_name.reset();
    layer.source_path.reset();
    layer.source_file.reset();
    layer.embedded_media_reference.reset();
    layer.embedded_media_path.reset();

    erase_unreferenced_project_buffers(&project_audio_buffers_, imported_preset_.layers, candidate_buffer_ids);

    // Reset layer overrides for pan (will be re-set by add_audio_file_to_layer if stereo)
    layer_overrides_[layer_index].pan_x = 0.0;
    layer_overrides_[layer_index].pan_y = 1.0;
    layer_overrides_[layer_index].pan_x_right = 0.0;
    layer_overrides_[layer_index].pan_y_right = 1.0;

    return add_audio_file_to_layer(layer_index, audio_path, error_message);
}

bool AppController::clear_layer_audio(std::size_t layer_index, std::string* error_message) {
    if (!has_imported_preset()) {
        if (error_message != nullptr) {
            *error_message = "No preset or project is loaded.";
        }
        return false;
    }
    if (layer_index >= imported_preset_.layers.size() || layer_index >= layer_overrides_.size()) {
        if (error_message != nullptr) {
            *error_message = "Invalid layer selection.";
        }
        return false;
    }

    // Streaming voices reference audio buffers directly, so stop playback before
    // removing a layer's backing audio or resetting its live state.
    if (streaming_active_) {
        stop_streaming_playback();
    }

    clear_layer_clip_edits(layer_index);

    auto& layer = imported_preset_.layers[layer_index];
    std::vector<std::string> candidate_buffer_ids;
    candidate_buffer_ids.reserve(layer.sources.size() + 1);

    for (const auto& source : layer.sources) {
        if (source.buffer_id.has_value() && !source.buffer_id->empty()) {
            candidate_buffer_ids.push_back(*source.buffer_id);
        }
    }
    if (layer.embedded_media_reference.has_value() && !layer.embedded_media_reference->empty()) {
        candidate_buffer_ids.push_back(*layer.embedded_media_reference);
    }

    layer.active = false;
    layer.mute = false;
    layer.solo = false;
    layer.reverse = false;
    layer.gain = 1.0;
    layer.delay.reset();
    layer.start_offset.reset();
    layer.stop_offset.reset();
    layer.fine_pitch.reset();
    layer.source_name.reset();
    layer.source_path.reset();
    layer.source_file.reset();
    layer.embedded_media_reference.reset();
    layer.embedded_media_path.reset();
    layer.sources.clear();
    layer.regions.clear();

    layer_overrides_[layer_index] = LayerOverride{};
    layer_overrides_[layer_index].effects = LayerEffectState{};

    imported_preset_.active_layer_count = 0;
    for (const auto& current_layer : imported_preset_.layers) {
        if (current_layer.active) {
            ++imported_preset_.active_layer_count;
        }
    }

    for (const auto& buffer_id : candidate_buffer_ids) {
        bool still_referenced = false;
        for (const auto& other_layer : imported_preset_.layers) {
            if (other_layer.embedded_media_reference.has_value() &&
                *other_layer.embedded_media_reference == buffer_id) {
                still_referenced = true;
                break;
            }
            for (const auto& other_source : other_layer.sources) {
                if (other_source.buffer_id.has_value() && *other_source.buffer_id == buffer_id) {
                    still_referenced = true;
                    break;
                }
            }
            if (still_referenced) {
                break;
            }
        }

        if (!still_referenced) {
            project_audio_buffers_.erase(buffer_id);
        }
    }

    if (selected_layer_index_.has_value() && *selected_layer_index_ == layer_index) {
        selected_layer_index_.reset();
        for (std::size_t i = 0; i < imported_preset_.layers.size(); ++i) {
            if (layer_audio_buffer(i) != nullptr) {
                selected_layer_index_ = i;
                break;
            }
        }
        if (!selected_layer_index_.has_value() && !imported_preset_.layers.empty()) {
            selected_layer_index_ = std::min(layer_index, imported_preset_.layers.size() - 1);
        }
    }

    return true;
}

bool AppController::save_project(const std::filesystem::path& output_path, std::string* error_message) const {
    if (!has_imported_preset()) {
        if (error_message != nullptr) {
            *error_message = "No preset or project is loaded.";
        }
        return false;
    }
    const std::map<std::string, EmbeddedAudioBlob> empty_embedded;
    return save_project_file(
        output_path,
        imported_preset_,
        layer_overrides_,
        layer_edit_states_,
        aux_track_state_,
        record_bus_mode_,
        trigger_mode_,
        octave_,
        project_picture_path_,
        session_recordings_directory_,
        session_recordings_,
        selected_session_recording_index_,
        empty_embedded,
        error_message
    );
}

bool AppController::save_project_with_audio(const std::filesystem::path& output_path, std::string* error_message) const {
    if (!has_imported_preset()) {
        if (error_message != nullptr) {
            *error_message = "No preset or project is loaded.";
        }
        return false;
    }
    if (!audio_encode_func_) {
        if (error_message != nullptr) {
            *error_message = "Audio encoder is not configured.";
        }
        return false;
    }

    // Collect every buffer_id referenced as a *source* (skip rendered buffers
    // since the receiver can re-render edit states from the source clips).
    std::unordered_set<std::string> source_ids;
    for (const auto& layer : imported_preset_.layers) {
        for (const auto& src : layer.sources) {
            if (src.buffer_id.has_value() && !src.buffer_id->empty()) {
                source_ids.insert(*src.buffer_id);
            }
        }
    }
    for (const auto& edit_state : layer_edit_states_) {
        if (!edit_state.has_value()) continue;
        for (const auto& clip : edit_state->clips) {
            if (!clip.source_buffer_id.empty()) {
                source_ids.insert(clip.source_buffer_id);
            }
        }
    }

    std::map<std::string, EmbeddedAudioBlob> embedded;
    for (const auto& buffer_id : source_ids) {
        const auto it = project_audio_buffers_.find(buffer_id);
        if (it == project_audio_buffers_.end() || it->second.frame_count() == 0) {
            continue;
        }
        embedded[buffer_id] = audio_encode_func_(it->second);
    }

    return save_project_file(
        output_path,
        imported_preset_,
        layer_overrides_,
        layer_edit_states_,
        aux_track_state_,
        record_bus_mode_,
        trigger_mode_,
        octave_,
        project_picture_path_,
        session_recordings_directory_,
        session_recordings_,
        selected_session_recording_index_,
        embedded,
        error_message
    );
}

bool AppController::load_project(const std::filesystem::path& input_path, std::string* error_message) {
    Preset preset;
    std::vector<LayerOverride> overrides;
    std::vector<std::optional<LayerEditState>> layer_edit_states;
    AuxTrackState auxTrackState;
    RecordBusMode record_bus_mode = RecordBusMode::Stereo;
    TriggerMode trigger_mode = TriggerMode::kOneShot;
    int octave = 4;
    std::optional<std::filesystem::path> project_picture_path;
    std::filesystem::path recordings_directory;
    std::vector<SessionRecordingInfo> session_recordings;
    std::optional<std::size_t> selected_session_recording_index;
    std::map<std::string, EmbeddedAudioBlob> embedded_audio;
    if (!load_project_file(
            input_path,
            preset,
            overrides,
            layer_edit_states,
            auxTrackState,
            record_bus_mode,
            trigger_mode,
            octave,
            project_picture_path,
            recordings_directory,
            session_recordings,
            selected_session_recording_index,
            embedded_audio,
            error_message)) {
        return false;
    }

    auto resolve_media_path = [&input_path](const std::string& rawPath) {
        std::filesystem::path resolved(rawPath);
        if (resolved.is_relative()) {
            resolved = input_path.parent_path() / resolved;
        }
        return resolved.lexically_normal();
    };

    std::unordered_map<std::string, AudioBuffer> buffers;
    std::vector<std::string> diagnostics;
    for (auto& layer : preset.layers) {
        if (layer.embedded_media_path.has_value()) {
            std::filesystem::path resolvedEmbeddedPath(*layer.embedded_media_path);
            if (resolvedEmbeddedPath.is_relative()) {
                resolvedEmbeddedPath = input_path.parent_path() / resolvedEmbeddedPath;
            }
            layer.embedded_media_path = resolvedEmbeddedPath.lexically_normal().string();
        }

        if (layer.sources.empty() && layer.source_path.has_value()) {
            LayerSource source;
            source.name = layer.source_name;
            source.path = layer.source_path;
            source.file = layer.source_file;
            source.buffer_id = layer.source_path;
            source.embedded = false;
            source.regions = layer.regions;
            if (source.regions.empty()) {
                source.regions.push_back(Region{});
            }
            layer.sources.push_back(std::move(source));
        }

        if (layer.source_path.has_value()) {
            const auto resolvedLayerPath = resolve_media_path(*layer.source_path);
            layer.source_path = resolvedLayerPath.string();
            if (!layer.source_file.has_value()) {
                layer.source_file = resolvedLayerPath.filename().string();
            }
        }

        for (const auto& source : layer.sources) {
            std::optional<std::filesystem::path> mediaPath;
            std::string bufferId;

            if (source.embedded && layer.embedded_media_path.has_value()) {
                mediaPath = std::filesystem::path(*layer.embedded_media_path);
                bufferId = source.buffer_id.has_value()
                    ? *source.buffer_id
                    : layer.embedded_media_reference.value_or(mediaPath->string());
            } else if (source.path.has_value()) {
                mediaPath = resolve_media_path(*source.path);
                bufferId = mediaPath->string();
            } else if (layer.embedded_media_path.has_value()) {
                mediaPath = std::filesystem::path(*layer.embedded_media_path);
                bufferId = layer.embedded_media_reference.value_or(mediaPath->string());
            } else {
                continue;
            }

            if (buffers.find(bufferId) != buffers.end()) {
                continue;
            }

            // V24 files may have audio embedded directly. Try that first so
            // the project opens fully even when the original file is missing.
            const auto embIt = embedded_audio.find(bufferId);
            if (embIt != embedded_audio.end() && audio_decode_bytes_func_) {
                auto decode_emb = audio_decode_bytes_func_(embIt->second);
                diagnostics.insert(diagnostics.end(),
                                   decode_emb.diagnostics.begin(),
                                   decode_emb.diagnostics.end());
                if (decode_emb.success) {
                    buffers.emplace(bufferId, std::move(decode_emb.audio));
                    continue;
                }
            }

            const auto decode = decode_audio(*mediaPath, working_directory_ / "decoded_local");
            diagnostics.insert(diagnostics.end(), decode.diagnostics.begin(), decode.diagnostics.end());
            if (decode.success) {
                buffers.emplace(bufferId, decode.audio);
            }
        }
    }

    for (auto& layer : preset.layers) {
        for (auto& source : layer.sources) {
            if (source.embedded) {
                if (!source.buffer_id.has_value() && layer.embedded_media_reference.has_value()) {
                    source.buffer_id = *layer.embedded_media_reference;
                }
            } else if (source.path.has_value()) {
                const auto resolvedSourcePath = resolve_media_path(*source.path);
                source.path = resolvedSourcePath.string();
                if (!source.file.has_value()) {
                    source.file = resolvedSourcePath.filename().string();
                }
                source.buffer_id = resolvedSourcePath.string();
            }
        }
    }

    imported_preset_ = std::move(preset);
    layer_overrides_ = std::move(overrides);
    layer_edit_states_ = std::move(layer_edit_states);
    if (layer_edit_states_.size() < imported_preset_.layers.size()) {
        layer_edit_states_.resize(imported_preset_.layers.size());
    }
    aux_track_state_ = std::move(auxTrackState);
    aux_track_state_.gain = std::clamp(aux_track_state_.gain, 0.0, 2.0);
    aux_track_state_.bass_gain_db = std::clamp(aux_track_state_.bass_gain_db, kAuxBassCutDb, kAuxBassBoostDb);
    record_bus_mode_ = record_bus_mode;
    trigger_mode_ = trigger_mode;
    octave_ = octave;
    project_picture_path_ = std::move(project_picture_path);
    project_audio_buffers_ = std::move(buffers);
    fixture_audio_ = FixtureAudioResolution{};
    fixture_audio_.diagnostics = std::move(diagnostics);
    last_rendered_audio_.reset();
    session_recordings_directory_ = recordings_directory.empty() ? (working_directory_ / "session_recordings" / unique_session_suffix()) : recordings_directory;
    std::filesystem::create_directories(session_recordings_directory_);
    session_recordings_.clear();
    for (const auto& recording : session_recordings) {
        if (!recording.path.empty() && std::filesystem::exists(recording.path)) {
            session_recordings_.push_back(recording);
        }
    }
    if (selected_session_recording_index.has_value() &&
        *selected_session_recording_index < session_recordings_.size()) {
        selected_session_recording_index_ = *selected_session_recording_index;
    } else if (!session_recordings_.empty()) {
        selected_session_recording_index_ = session_recordings_.size() - 1;
    } else {
        selected_session_recording_index_.reset();
    }
    session_recording_armed_ = false;
    streaming_mixer_.clear_aux_plugin_sessions();
    streaming_mixer_.set_aux_gain(static_cast<float>(aux_track_state_.gain));
    streaming_mixer_.set_aux_bass_gain_db(static_cast<float>(aux_track_state_.bass_gain_db));
    streaming_mixer_.set_record_bus_channel_count(static_cast<int>(record_bus_mode_));
    for (std::size_t i = 0; i < layer_edit_states_.size(); ++i) {
        if (!layer_edit_states_[i].has_value()) {
            continue;
        }
        if (layer_edit_states_[i]->rendered_buffer_id.empty()) {
            layer_edit_states_[i]->rendered_buffer_id = layer_edit_rendered_buffer_id(i);
        }
        std::string rebuild_error;
        if (!rebuild_layer_edit_render(i, &rebuild_error)) {
            clear_layer_clip_edits(i);
            fixture_audio_.diagnostics.push_back(
                "Layer edit render rebuild failed for layer " + std::to_string(i) + ": " + rebuild_error);
        }
    }
    bootstrap_layer_edit_states_for_audio_layers();
    held_note_.reset();
    selected_layer_index_.reset();
    for (std::size_t i = 0; i < imported_preset_.layers.size(); ++i) {
        if (layer_audio_buffer(i) != nullptr) {
            selected_layer_index_ = i;
            break;
        }
    }
    return true;
}

void AppController::set_project_picture_path(const std::filesystem::path& path) {
    project_picture_path_ = path;
}

void AppController::clear_project_picture_path() {
    project_picture_path_.reset();
}

std::optional<std::filesystem::path> AppController::project_picture_path() const {
    return project_picture_path_;
}

bool AppController::start_streaming_playback(int midi_note, std::string* error_message) {
    if (!has_imported_preset()) {
        if (error_message != nullptr) {
            *error_message = "No preset loaded.";
        }
        return false;
    }
    if (project_audio_buffers_.empty()) {
        if (error_message != nullptr) {
            *error_message = "No decoded audio is available for playback.";
        }
        return false;
    }

    const auto playback = build_playback_preset_for_note(midi_note);
    if (trigger_mode_ == TriggerMode::kContinuous) {
        held_note_ = HeldNoteState{midi_note, std::chrono::steady_clock::now()};
    }
    return start_streaming_preset(playback);
}

bool AppController::play_session_take(std::size_t take_index, std::string* error_message, double start_normalized) {
    if (take_index >= session_recordings_.size()) {
        if (error_message) *error_message = "Invalid take index.";
        return false;
    }
    const auto& info = session_recordings_[take_index];
    if (!std::filesystem::exists(info.path)) {
        if (error_message) *error_message = "Take file not found.";
        return false;
    }

    auto decoded = decode_audio(info.path, working_directory_);
    if (!decoded.success || decoded.audio.frame_count() == 0) {
        if (error_message) *error_message = "Failed to decode take audio.";
        return false;
    }

    // Stop any active playback
    if (streaming_active_) {
        streaming_mixer_.stop();
        streaming_layers_.clear();
        streaming_active_ = false;
    }

    // Store the decoded audio
    take_playback_buffer_ = std::move(decoded.audio);
    const auto& buf = *take_playback_buffer_;
    const auto frames = buf.frame_count();
    const auto startFrame = static_cast<std::size_t>(std::clamp(
        start_normalized, 0.0, 1.0) * static_cast<double>(frames > 0 ? frames - 1 : 0));

    // Build a single streaming layer for the take
    streaming_layers_.clear();
    StreamingMixer::LayerState state;
    state.layer_index = 0;
    state.route_to_record_bus = false;
    state.params->gain.store(1.0f, std::memory_order_relaxed);
    state.params->active.store(true, std::memory_order_relaxed);
    state.params->mute.store(false, std::memory_order_relaxed);
    state.params->solo.store(false, std::memory_order_relaxed);
    if (buf.channels > 1) {
        state.params->pan_x.store(-1.0f, std::memory_order_relaxed);
        state.params->pan_y.store(1.0f, std::memory_order_relaxed);
        state.params->pan_x_right.store(1.0f, std::memory_order_relaxed);
        state.params->pan_y_right.store(1.0f, std::memory_order_relaxed);
    } else {
        state.params->pan_x.store(0.0f, std::memory_order_relaxed);
        state.params->pan_y.store(1.0f, std::memory_order_relaxed);
        state.params->pan_x_right.store(0.0f, std::memory_order_relaxed);
        state.params->pan_y_right.store(1.0f, std::memory_order_relaxed);
    }

    auto& voice = state.voice;
    voice.buffer = &buf;
    voice.start_frame = 0;
    voice.end_frame = frames;
    voice.position = static_cast<double>(startFrame);
    voice.pitch_ratio = static_cast<double>(buf.sample_rate) / static_cast<double>(output_sample_rate_);
    voice.reverse = false;
    voice.finished = false;

    streaming_layers_.push_back(std::move(state));
    streaming_mixer_.set_aux_gain(static_cast<float>(aux_track_state_.gain));
    streaming_mixer_.set_aux_bass_gain_db(static_cast<float>(aux_track_state_.bass_gain_db));
    streaming_mixer_.set_layers(&streaming_layers_);
    streaming_active_ = true;

    if (on_streaming_layers_rebuilt) {
        on_streaming_layers_rebuilt();
    }
    return true;
}

bool AppController::start_streaming_preset(const PlaybackPreset& playback) {
    // Stop any active playback first to ensure the audio thread isn't reading
    // streaming_layers_ while we rebuild it.
    if (streaming_active_) {
        streaming_mixer_.stop();
        streaming_layers_.clear();
        streaming_active_ = false;
    }

    // Build streaming layer states
    streaming_layers_.reserve(playback.layers.size());

    const bool has_solo = std::any_of(playback.layers.begin(), playback.layers.end(), [](const auto& layer) {
        return layer.active && layer.solo && !layer.mute;
    });

    for (std::size_t i = 0; i < playback.layers.size() && i < layer_overrides_.size(); ++i) {
        const auto& layer = playback.layers[i];

        StreamingMixer::LayerState state;
        state.layer_index = i;
        state.route_to_record_bus = true;

        // Set LiveParams from current override state
        state.params->gain.store(static_cast<float>(layer_overrides_[i].gain), std::memory_order_relaxed);
        state.params->pan_x.store(static_cast<float>(layer_overrides_[i].pan_x), std::memory_order_relaxed);
        state.params->pan_y.store(static_cast<float>(layer_overrides_[i].pan_y), std::memory_order_relaxed);
        state.params->pan_x_right.store(static_cast<float>(layer_overrides_[i].pan_x_right), std::memory_order_relaxed);
        state.params->pan_y_right.store(static_cast<float>(layer_overrides_[i].pan_y_right), std::memory_order_relaxed);
        state.params->mute.store(layer_overrides_[i].mute, std::memory_order_relaxed);
        state.params->solo.store(layer_overrides_[i].solo, std::memory_order_relaxed);
        state.params->active.store(layer.active, std::memory_order_relaxed);

        if (!layer.active || layer.sources.empty()) {
            state.voice.finished = true;
            streaming_layers_.push_back(std::move(state));
            continue;
        }

        const auto& source = layer.sources.front();
        const auto buffer_it = project_audio_buffers_.find(source.buffer_id);
        if (buffer_it == project_audio_buffers_.end()) {
            state.voice.finished = true;
            streaming_layers_.push_back(std::move(state));
            continue;
        }

        const auto& buffer = buffer_it->second;
        const auto frames = buffer.frame_count();
        if (frames == 0) {
            state.voice.finished = true;
            streaming_layers_.push_back(std::move(state));
            continue;
        }

        PlaybackRegion region;
        if (!source.regions.empty()) {
            region = source.regions.front();
        }

        const double region_start = std::max(std::clamp(layer.start_offset, 0.0, 1.0), std::clamp(region.start, 0.0, 1.0));
        const double region_end = std::min(std::clamp(layer.stop_offset, 0.0, 1.0), std::clamp(region.end, 0.0, 1.0));
        const std::size_t start_frame = std::min<std::size_t>(static_cast<std::size_t>(region_start * frames), frames - 1);
        const std::size_t end_frame = std::max(start_frame + 1, std::min(static_cast<std::size_t>(region_end * frames), frames));

        auto& voice = state.voice;
        voice.buffer = &buffer;
        voice.start_frame = start_frame;
        voice.end_frame = end_frame;
        voice.reverse = layer.reverse;
        voice.position = layer.reverse ? static_cast<double>(end_frame - 1) : static_cast<double>(start_frame);
        voice.pitch_ratio = std::pow(2.0, (layer.coarse_semitones + (layer.fine_cents / 100.0)) / 12.0) *
            (static_cast<double>(buffer.sample_rate) / static_cast<double>(output_sample_rate_));
        // Time stretch is handled in real-time by signalsmith-stretch in the mixer;
        // store the ratio in LiveParams so it can be changed during playback.
        state.params->time_stretch_ratio.store(
            static_cast<float>(std::clamp(layer.effects.time_stretch_ratio, 0.01, 8.0)),
            std::memory_order_relaxed);
        state.params->bass_lfe_gain_db.store(
            static_cast<float>(std::clamp(layer_overrides_[i].effects.bass_lfe_gain_db, -24.0, 12.0)),
            std::memory_order_relaxed);
        state.params->eq_low_gain_db.store(
            static_cast<float>(std::clamp(layer_overrides_[i].effects.eq_low_gain_db, kLayerEqCutDb, kLayerEqBoostDb)),
            std::memory_order_relaxed);
        state.params->eq_mid_gain_db.store(
            static_cast<float>(std::clamp(layer_overrides_[i].effects.eq_mid_gain_db, kLayerEqCutDb, kLayerEqBoostDb)),
            std::memory_order_relaxed);
        state.params->eq_high_gain_db.store(
            static_cast<float>(std::clamp(layer_overrides_[i].effects.eq_high_gain_db, kLayerEqCutDb, kLayerEqBoostDb)),
            std::memory_order_relaxed);
        if (const auto* editState = layer_edit_state_ptr(i);
            editState != nullptr && editState->stretch_automation_enabled) {
            const double totalDuration = std::max(
                layer_edit_total_duration_seconds(*editState),
                static_cast<double>(buffer.frame_count()) /
                    static_cast<double>(std::max(1, buffer.sample_rate)));
            state.stretch_automation_enabled = editState->stretch_automation_enabled;
            if (totalDuration > 0.0) {
                state.stretch_automation_points.reserve(editState->stretch_automation_points.size());
                for (const auto& point : editState->stretch_automation_points) {
                    state.stretch_automation_points.push_back(
                        StreamingMixer::LayerState::StretchAutomationPoint{
                            std::clamp(point.timeline_seconds / totalDuration, 0.0, 1.0),
                            static_cast<float>(std::clamp(point.ratio,
                                                          kStretchAutomationMinRatio,
                                                          kStretchAutomationMaxRatio))
                        });
                }
            }
        }
        if (const auto* editState = layer_edit_state_ptr(i); editState != nullptr) {
            const double totalDuration = std::max(
                layer_edit_total_duration_seconds(*editState),
                static_cast<double>(buffer.frame_count()) /
                    static_cast<double>(std::max(1, buffer.sample_rate)));
            const auto volumeRandomSettings = sanitize_volume_random_settings(editState->volume_random_settings);
            state.volume_random_settings.enabled = volumeRandomSettings.enabled;
            state.volume_random_settings.loudest_db = static_cast<float>(volumeRandomSettings.loudest_db);
            state.volume_random_settings.quietest_db = static_cast<float>(volumeRandomSettings.quietest_db);
            state.volume_random_settings.period_longest_seconds =
                static_cast<float>(volumeRandomSettings.period_longest_seconds);
            state.volume_random_settings.period_shortest_seconds =
                static_cast<float>(volumeRandomSettings.period_shortest_seconds);
            state.volume_random_settings.smoothing = static_cast<float>(volumeRandomSettings.smoothing);
            state.volume_random_runtime = {};
            state.volume_random_runtime.rng.seed(
                static_cast<std::uint32_t>(0x9E3779B9u ^ static_cast<std::uint32_t>(i * 2654435761u)));
            const auto panRandomSettings = sanitize_pan_random_settings(editState->pan_random_settings);
            state.pan_random_settings.enabled = panRandomSettings.enabled;
            state.pan_random_settings.farthest_left = static_cast<float>(panRandomSettings.farthest_left);
            state.pan_random_settings.farthest_right = static_cast<float>(panRandomSettings.farthest_right);
            state.pan_random_settings.farthest_front = static_cast<float>(panRandomSettings.farthest_front);
            state.pan_random_settings.farthest_back = static_cast<float>(panRandomSettings.farthest_back);
            state.pan_random_settings.speed = static_cast<float>(panRandomSettings.speed);
            state.pan_random_settings.smoothing = static_cast<float>(panRandomSettings.smoothing);
            state.pan_random_runtime = {};
            state.pan_random_runtime.rng.seed(
                static_cast<std::uint32_t>(0x85EBCA6Bu ^ static_cast<std::uint32_t>(i * 2246822519u)));
            const auto stretchRandomSettings = sanitize_stretch_random_settings(editState->stretch_random_settings);
            state.stretch_random_settings.enabled = stretchRandomSettings.enabled;
            state.stretch_random_settings.lowest_percent =
                static_cast<float>(stretchRandomSettings.lowest_percent);
            state.stretch_random_settings.highest_percent =
                static_cast<float>(stretchRandomSettings.highest_percent);
            state.stretch_random_settings.speed = static_cast<float>(stretchRandomSettings.speed);
            state.stretch_random_settings.smoothing = static_cast<float>(stretchRandomSettings.smoothing);
            state.stretch_random_runtime = {};
            state.stretch_random_runtime.rng.seed(
                static_cast<std::uint32_t>(0xC2B2AE35u ^ static_cast<std::uint32_t>(i * 3266489917u)));
            auto copyPanPoints = [totalDuration](bool enabled,
                                                 const auto& sourcePoints,
                                                 bool* targetEnabled,
                                                auto* targetPoints) {
                *targetEnabled = enabled;
                targetPoints->clear();
                if (totalDuration <= 0.0) {
                    return;
                }
                targetPoints->reserve(sourcePoints.size());
                for (const auto& point : sourcePoints) {
                    targetPoints->push_back(StreamingMixer::LayerState::PanAutomationPoint{
                        point.point_id,
                        std::clamp(point.timeline_seconds / totalDuration, 0.0, 1.0),
                        static_cast<float>(std::clamp(point.value,
                                                      kPanAutomationMinValue,
                                                      kPanAutomationMaxValue))
                    });
                }
            };
            copyPanPoints(editState->pan_position_automation_enabled,
                          editState->pan_position_automation_points,
                          &state.pan_position_automation_enabled,
                          &state.pan_position_automation_points);
            copyPanPoints(editState->pan_front_back_automation_enabled,
                          editState->pan_front_back_automation_points,
                          &state.pan_front_back_automation_enabled,
                          &state.pan_front_back_automation_points);
            copyPanPoints(editState->pan_right_position_automation_enabled,
                          editState->pan_right_position_automation_points,
                          &state.pan_right_position_automation_enabled,
                          &state.pan_right_position_automation_points);
            copyPanPoints(editState->pan_right_front_back_automation_enabled,
                          editState->pan_right_front_back_automation_points,
                          &state.pan_right_front_back_automation_enabled,
                          &state.pan_right_front_back_automation_points);
            copyPanPoints(editState->doppler_automation_enabled,
                          editState->doppler_automation_points,
                          &state.doppler_automation_enabled,
                          &state.doppler_automation_points);
            state.doppler_settings.edge_gain_db =
                static_cast<float>(editState->doppler_settings.edge_gain_db);
            state.doppler_settings.center_gain_db =
                static_cast<float>(editState->doppler_settings.center_gain_db);
            state.doppler_settings.edge_pitch_semitones =
                static_cast<float>(editState->doppler_settings.edge_pitch_semitones);
            state.doppler_settings.center_pitch_semitones =
                static_cast<float>(editState->doppler_settings.center_pitch_semitones);
            state.doppler_segment_shapes.clear();
            state.doppler_segment_shapes.reserve(editState->doppler_segment_shapes.size());
            for (const auto& shape : editState->doppler_segment_shapes) {
                state.doppler_segment_shapes.push_back(
                    StreamingMixer::LayerState::DopplerSegmentShape{
                        shape.left_point_id,
                        static_cast<int>(shape.curve_type),
                        static_cast<float>(std::clamp(shape.curve_amount, 0.0, 1.0))
                    });
            }
        }
        if (voice.pitch_ratio <= 0.0) {
            voice.pitch_ratio = 1.0;
        }
        voice.finished = false;
        voice.region = region;
        voice.envelope = layer.envelope;
        voice.samples_elapsed = 0;
        voice.delay_remaining = static_cast<std::size_t>(std::max(0.0, layer.delay_seconds) * output_sample_rate_);

        voice.loop_enabled = region.loop_enabled;
        voice.loop_start_frame = std::clamp(region.loop_start, 0.0, 1.0) * frames;
        voice.loop_end_frame = std::clamp(region.loop_end, 0.0, 1.0) * frames;

        voice.continuous = trigger_mode_ == TriggerMode::kContinuous;
        voice.loop_retrigger_enabled = false;
        voice.loop_regions.clear();
        voice.last_loop_region_index = -1;
        if (voice.continuous) {
            if (!layer_overrides_[i].trigger_regions.empty()) {
                voice.loop_retrigger_enabled = true;
                voice.loop_regions.reserve(layer_overrides_[i].trigger_regions.size());
                for (const auto& authored : layer_overrides_[i].trigger_regions) {
                    voice.loop_regions.push_back(PlaybackRegion{
                        authored.start,
                        authored.end,
                        authored.start,
                        authored.end,
                        false
                    });
                }
            } else {
                voice.loop_enabled = true;
                voice.loop_start_frame = static_cast<double>(start_frame);
                voice.loop_end_frame = static_cast<double>(end_frame);
            }
        }
        voice.hold_samples = 0;
        voice.released = false;

        streaming_layers_.push_back(std::move(state));
    }

    // Use the actual device sample rate, not the preset default
    streaming_mixer_.prepare(output_sample_rate_);
    streaming_mixer_.set_aux_gain(static_cast<float>(aux_track_state_.gain));
    streaming_mixer_.set_aux_bass_gain_db(static_cast<float>(aux_track_state_.bass_gain_db));
    streaming_mixer_.set_layers(&streaming_layers_);
    streaming_active_ = true;

    // Let the UI wire plugin host sessions into the new layer states
    if (on_streaming_layers_rebuilt) {
        on_streaming_layers_rebuilt();
    }

    return true;
}

void AppController::stop_streaming_playback() {
    if (streaming_active_) {
        // Only capture recording if NOT in session recording mode —
        // session recording is managed by the arm/disarm flow.
        if (!session_recording_armed_) {
            auto recording = streaming_mixer_.take_recording();
            if (recording.has_value()) {
                last_rendered_audio_ = std::move(*recording);
            } else {
                last_rendered_audio_.reset();
            }
        }
        streaming_mixer_.stop();
        streaming_layers_.clear();
        streaming_active_ = false;
        held_note_.reset();
    }
}

bool AppController::is_streaming() const {
    return streaming_active_;
}

StreamingMixer& AppController::streaming_mixer() {
    return streaming_mixer_;
}

std::vector<StreamingMixer::LayerState>& AppController::streaming_layer_states() {
    return streaming_layers_;
}

void AppController::push_live_gain(std::size_t layer_index) {
    if (!streaming_active_ || layer_index >= streaming_layers_.size()) {
        return;
    }
    streaming_layers_[layer_index].params->gain.store(
        static_cast<float>(layer_overrides_[layer_index].gain),
        std::memory_order_relaxed
    );
}

void AppController::push_live_pan(std::size_t layer_index) {
    if (!streaming_active_ || layer_index >= streaming_layers_.size()) {
        return;
    }
    const auto& o = layer_overrides_[layer_index];
    auto& p = *streaming_layers_[layer_index].params;
    p.pan_x.store(static_cast<float>(o.pan_x), std::memory_order_relaxed);
    p.pan_y.store(static_cast<float>(o.pan_y), std::memory_order_relaxed);
    p.pan_x_right.store(static_cast<float>(o.pan_x_right), std::memory_order_relaxed);
    p.pan_y_right.store(static_cast<float>(o.pan_y_right), std::memory_order_relaxed);
}

std::optional<double> AppController::layer_streaming_position(std::size_t layer_index) const {
    if (!streaming_active_ || layer_index >= streaming_layers_.size()) {
        return std::nullopt;
    }
    const double frame = streaming_layers_[layer_index].params->playback_frame.load(std::memory_order_relaxed);
    if (frame < 0.0) {
        return std::nullopt;
    }

    const auto& voice = streaming_layers_[layer_index].voice;
    if (voice.buffer == nullptr || voice.buffer->frame_count() == 0) {
        return std::nullopt;
    }
    
    double pos = std::clamp(frame / static_cast<double>(voice.buffer->frame_count()), 0.0, 1.0);

    // When reversed, flip the position so the playhead moves left-to-right
    // on the visually-flipped waveform
    if (voice.reverse) {
        pos = 1.0 - pos;
    }

    return pos;
}

void AppController::push_live_mute(std::size_t layer_index) {
    if (!streaming_active_ || layer_index >= streaming_layers_.size()) {
        return;
    }
    streaming_layers_[layer_index].params->mute.store(
        layer_overrides_[layer_index].mute,
        std::memory_order_relaxed
    );
}

void AppController::push_live_solo() {
    if (!streaming_active_) {
        return;
    }
    for (std::size_t i = 0; i < streaming_layers_.size() && i < layer_overrides_.size(); ++i) {
        streaming_layers_[i].params->solo.store(
            layer_overrides_[i].solo,
            std::memory_order_relaxed
        );
    }
    streaming_mixer_.update_solo_flag();
}

void AppController::push_live_stretch(std::size_t layer_index) {
    if (!streaming_active_ || layer_index >= streaming_layers_.size() ||
        layer_index >= layer_overrides_.size()) {
        return;
    }
    streaming_layers_[layer_index].params->time_stretch_ratio.store(
        static_cast<float>(std::clamp(layer_overrides_[layer_index].effects.time_stretch_ratio, 0.01, 8.0)),
        std::memory_order_relaxed
    );
}

void AppController::push_live_bass_lfe_gain(std::size_t layer_index) {
    if (!streaming_active_ || layer_index >= streaming_layers_.size() ||
        layer_index >= layer_overrides_.size()) {
        return;
    }
    streaming_layers_[layer_index].params->bass_lfe_gain_db.store(
        static_cast<float>(std::clamp(layer_overrides_[layer_index].effects.bass_lfe_gain_db, -24.0, 12.0)),
        std::memory_order_relaxed
    );
}

void AppController::push_live_layer_eq(std::size_t layer_index) {
    if (!streaming_active_ || layer_index >= streaming_layers_.size() ||
        layer_index >= layer_overrides_.size()) {
        return;
    }
    const auto& effects = layer_overrides_[layer_index].effects;
    auto& params = *streaming_layers_[layer_index].params;
    params.eq_low_gain_db.store(
        static_cast<float>(std::clamp(effects.eq_low_gain_db, kLayerEqCutDb, kLayerEqBoostDb)),
        std::memory_order_relaxed);
    params.eq_mid_gain_db.store(
        static_cast<float>(std::clamp(effects.eq_mid_gain_db, kLayerEqCutDb, kLayerEqBoostDb)),
        std::memory_order_relaxed);
    params.eq_high_gain_db.store(
        static_cast<float>(std::clamp(effects.eq_high_gain_db, kLayerEqCutDb, kLayerEqBoostDb)),
        std::memory_order_relaxed);
}

void AppController::set_session_recording_armed(bool armed) {
    session_recording_armed_ = armed;
}

bool AppController::session_recording_armed() const {
    return session_recording_armed_;
}

bool AppController::record_bus_surround_enabled() const {
    return record_bus_mode_ != RecordBusMode::Stereo;
}

void AppController::set_record_bus_surround_enabled(bool enabled) {
    set_record_bus_mode(enabled ? RecordBusMode::Surround51 : RecordBusMode::Stereo);
}

RecordBusMode AppController::record_bus_mode() const {
    return record_bus_mode_;
}

void AppController::set_record_bus_mode(RecordBusMode mode) {
    record_bus_mode_ = mode;
    streaming_mixer_.set_record_bus_channel_count(static_cast<int>(mode));
}

std::optional<SessionRecordingInfo> AppController::commit_session_recording(
    const RenderedAudio& audio,
    const std::string& stem,
    std::optional<std::pair<double, double>> punch_in_region,
    std::optional<double> picture_start_seconds,
    std::string* error_message
) {
    if (audio.samples.empty() || audio.frame_count() == 0) {
        if (error_message != nullptr) {
            *error_message = "Nothing was recorded.";
        }
        return std::nullopt;
    }

    RenderedAudio final_audio = audio;

    if (punch_in_region.has_value() && selected_session_recording_index_.has_value() &&
        *selected_session_recording_index_ < session_recordings_.size()) {
        const auto& target_info = session_recordings_[*selected_session_recording_index_];
        if (!picture_start_seconds.has_value()) {
            picture_start_seconds = target_info.picture_start_seconds;
        }
        auto decoded = decode_audio(target_info.path, working_directory_);
        if (decoded.success && decoded.audio.frame_count() > 0) {
            RenderedAudio original;
            original.sample_rate = decoded.audio.sample_rate;
            original.channels = decoded.audio.channels;
            original.samples = std::move(decoded.audio.samples);

            const std::size_t start_frame = static_cast<std::size_t>(
                std::clamp(punch_in_region->first, 0.0, 1.0) * static_cast<double>(original.frame_count())
            );
            const std::size_t end_frame = static_cast<std::size_t>(
                std::clamp(punch_in_region->second, 0.0, 1.0) * static_cast<double>(original.frame_count())
            );
            const std::size_t punch_frames = (end_frame > start_frame) ? (end_frame - start_frame) : 0;

            final_audio = std::move(original);

            const std::size_t samples_to_copy = std::min(
                punch_frames * static_cast<std::size_t>(final_audio.channels),
                audio.samples.size()
            );

            if (audio.channels == final_audio.channels) {
                const std::size_t dest_start = start_frame * static_cast<std::size_t>(final_audio.channels);
                for (std::size_t i = 0; i < samples_to_copy && (dest_start + i) < final_audio.samples.size(); ++i) {
                    final_audio.samples[dest_start + i] = audio.samples[i];
                }
            }
        }
    }
    std::filesystem::create_directories(session_recordings_directory_);
    const std::string base = sanitize_recording_name(stem);
    std::filesystem::path path;
    for (int i = 1; i <= 9999; ++i) {
        const std::string numbered = base + "_" + std::to_string(i);
        path = session_recordings_directory_ / (numbered + ".wav");
        if (!std::filesystem::exists(path)) {
            SessionRecordingInfo info;
            info.name = numbered;
            info.path = path;
            info.picture_start_seconds = picture_start_seconds;
            info.channels = final_audio.channels;
            try {
                PlaybackEngine::write_wav_24(path, final_audio);
            } catch (const std::exception& ex) {
                if (error_message != nullptr) {
                    *error_message = ex.what();
                }
                return std::nullopt;
            }
            session_recordings_.push_back(info);
            selected_session_recording_index_ = session_recordings_.size() - 1;
            return info;
        }
    }
    if (error_message != nullptr) {
        *error_message = "Unable to allocate a session recording filename.";
    }
    return std::nullopt;
}

const std::vector<SessionRecordingInfo>& AppController::session_recordings() const {
    return session_recordings_;
}

std::optional<std::size_t> AppController::selected_session_recording_index() const {
    return selected_session_recording_index_;
}

bool AppController::select_session_recording(std::size_t index) {
    if (index >= session_recordings_.size()) {
        return false;
    }
    selected_session_recording_index_ = index;
    return true;
}

std::optional<SessionRecordingInfo> AppController::selected_session_recording() const {
    if (!selected_session_recording_index_.has_value() ||
        *selected_session_recording_index_ >= session_recordings_.size()) {
        return std::nullopt;
    }
    return session_recordings_[*selected_session_recording_index_];
}

bool AppController::rename_selected_session_recording(const std::string& new_name, std::string* error_message) {
    const auto selected = selected_session_recording();
    if (!selected.has_value() || !selected_session_recording_index_.has_value()) {
        if (error_message != nullptr) {
            *error_message = "No session recording is selected.";
        }
        return false;
    }
    const std::string sanitized = sanitize_recording_name(new_name);
    const auto target = selected->path.parent_path() / (sanitized + selected->path.extension().string());
    if (selected->path == target) {
        session_recordings_[*selected_session_recording_index_].name = sanitized;
        return true;
    }
    std::error_code ec;
    std::filesystem::rename(selected->path, target, ec);
    if (ec) {
        if (error_message != nullptr) {
            *error_message = "Unable to rename session recording.";
        }
        return false;
    }
    session_recordings_[*selected_session_recording_index_].name = sanitized;
    session_recordings_[*selected_session_recording_index_].path = target;
    return true;
}

bool AppController::export_selected_session_recording(const std::filesystem::path& output_path, std::string* error_message) const {
    const auto selected = selected_session_recording();
    if (!selected.has_value()) {
        if (error_message != nullptr) {
            *error_message = "No session recording is selected.";
        }
        return false;
    }
    std::error_code ec;
    std::filesystem::copy_file(selected->path, output_path, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        if (error_message != nullptr) {
            *error_message = "Unable to export the selected session recording.";
        }
        return false;
    }
    return true;
}

bool AppController::delete_session_recording(std::size_t index) {
    if (index >= session_recordings_.size()) return false;
    std::error_code ec;
    std::filesystem::remove(session_recordings_[index].path, ec);
    session_recordings_.erase(session_recordings_.begin() + static_cast<std::ptrdiff_t>(index));
    if (selected_session_recording_index_.has_value()) {
        if (*selected_session_recording_index_ == index) {
            selected_session_recording_index_ = session_recordings_.empty()
                ? std::nullopt
                : std::optional<std::size_t>(std::min(index, session_recordings_.size() - 1));
        } else if (*selected_session_recording_index_ > index) {
            --(*selected_session_recording_index_);
        }
    }
    return true;
}

const std::filesystem::path& AppController::session_recordings_directory() const {
    return session_recordings_directory_;
}

void AppController::reset_session_recordings_directory(const std::filesystem::path& project_hint) {
    std::string base = "session";
    if (!project_hint.empty()) {
        const auto stem = sanitize_recording_name(project_hint.stem().string());
        if (!stem.empty()) {
            base = stem;
        }
    } else if (!imported_preset_.name.empty()) {
        base = sanitize_recording_name(imported_preset_.name);
    }
    session_recordings_directory_ = working_directory_ / "session_recordings" / (base + "_" + unique_session_suffix());
    std::filesystem::create_directories(session_recordings_directory_);
}

}  // namespace radium
