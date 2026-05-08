#include "import_to_playback.h"

#include <algorithm>

namespace radium {

PlaybackPreset build_playback_preset(const Preset& imported_preset) {
    PlaybackPreset preset;
    preset.output_sample_rate = 48000;
    preset.assumptions = imported_preset.diagnostics.uncertain_assumptions;
    preset.assumptions.push_back(
        "Imported layer gain, delay, envelope, and randomization values are rendered as raw normalized values."
    );
    preset.assumptions.push_back(
        "Imported fine-pitch values are preserved in the import model but not converted into cents or semitones for playback because their units remain uncertain."
    );
    preset.assumptions.push_back(
        "Only a conservative subset of imported FX currently maps into playback: reverb wet state and limiter ceiling where directly available."
    );

    for (const auto& imported_layer : imported_preset.layers) {
        PlaybackLayer layer;
        layer.index = imported_layer.index;
        layer.active = imported_layer.active;
        layer.mute = imported_layer.mute;
        layer.solo = imported_layer.solo;
        layer.reverse = imported_layer.reverse;
        layer.gain = imported_layer.gain.value_or(1.0);
        layer.delay_seconds = imported_layer.delay.value_or(0.0);
        layer.start_offset = std::clamp(imported_layer.start_offset.value_or(0.0), 0.0, 1.0);
        layer.stop_offset = std::clamp(imported_layer.stop_offset.value_or(1.0), 0.0, 1.0);
        layer.random_gain.raw_amount = imported_layer.random_gain.amount.value_or(0.0);
        layer.random_pan.raw_amount = imported_layer.random_pan.amount.value_or(0.0);
        layer.envelope.attack_seconds = imported_layer.envelope.attack.value_or(0.0);
        layer.envelope.decay_seconds = imported_layer.envelope.decay.value_or(0.0);
        layer.envelope.sustain_level = imported_layer.envelope.sustain.value_or(1.0);
        layer.envelope.release_seconds = imported_layer.envelope.release.value_or(0.0);
        layer.effects.reverb.enabled = imported_layer.effects.reverb.enabled;
        layer.effects.reverb.wet = imported_layer.effects.reverb.wet.value_or(0.15);
        layer.effects.limiter.enabled =
            imported_layer.effects.limiter.limit.has_value() ||
            imported_layer.effects.limiter.input.has_value() ||
            imported_layer.effects.limiter.output.has_value();
        layer.effects.limiter.ceiling = imported_layer.effects.limiter.limit.value_or(0.95);

        auto append_regions = [](PlaybackSource& source, const std::vector<Region>& imported_regions) {
            for (const auto& imported_region : imported_regions) {
                PlaybackRegion region;
                region.start = std::clamp(imported_region.in_point.value_or(imported_region.start_offset.value_or(0.0)), 0.0, 1.0);
                region.end = std::clamp(imported_region.out_point.value_or(imported_region.end_offset.value_or(1.0)), 0.0, 1.0);
                region.loop_start = std::clamp(imported_region.loop_start.value_or(region.start), 0.0, 1.0);
                region.loop_end = std::clamp(imported_region.loop_end.value_or(region.end), 0.0, 1.0);
                region.loop_enabled = imported_region.loop_enabled;
                source.regions.push_back(region);
            }
        };

        if (!imported_layer.sources.empty()) {
            for (const auto& imported_source : imported_layer.sources) {
                PlaybackSource source;
                if (imported_source.buffer_id.has_value()) {
                    source.buffer_id = *imported_source.buffer_id;
                } else if (imported_source.path.has_value()) {
                    source.buffer_id = *imported_source.path;
                } else if (imported_source.name.has_value()) {
                    source.buffer_id = *imported_source.name;
                } else {
                    source.buffer_id = "layer_" + std::to_string(imported_layer.index);
                }
                append_regions(source, imported_source.regions.empty() ? imported_layer.regions : imported_source.regions);
                if (source.regions.empty()) {
                    source.regions.push_back(PlaybackRegion{});
                }
                if (source.regions.size() > 1) {
                    layer.no_immediate_repeat = true;
                }
                layer.sources.push_back(std::move(source));
            }
        } else {
            PlaybackSource source;
            if (imported_layer.embedded_media_reference.has_value()) {
                source.buffer_id = *imported_layer.embedded_media_reference;
            } else if (imported_layer.source_path.has_value()) {
                source.buffer_id = *imported_layer.source_path;
            } else if (imported_layer.source_name.has_value()) {
                source.buffer_id = *imported_layer.source_name;
            } else {
                source.buffer_id = "layer_" + std::to_string(imported_layer.index);
            }
            append_regions(source, imported_layer.regions);
            if (source.regions.empty()) {
                source.regions.push_back(PlaybackRegion{});
            }
            if (source.regions.size() > 1) {
                layer.no_immediate_repeat = true;
            }
            layer.sources.push_back(std::move(source));
        }
        preset.layers.push_back(std::move(layer));
    }

    return preset;
}

}  // namespace radium
