#include "import_model.h"

#include <algorithm>
#include <map>
#include <set>

namespace radium {
namespace {

std::vector<const PresetChunkSummary*> collect_group_chunks(
    const PresetSummary& preset,
    const SlotGroupSummary& group,
    const std::string& label
) {
    std::vector<const PresetChunkSummary*> chunks;
    for (const auto sequence_index : group.chunk_sequence_indices) {
        if (sequence_index < preset.chunks.size() && preset.chunks[sequence_index].label == label) {
            chunks.push_back(&preset.chunks[sequence_index]);
        }
    }
    return chunks;
}

const PresetChunkSummary* first_group_chunk(
    const PresetSummary& preset,
    const SlotGroupSummary& group,
    const std::string& label
) {
    const auto chunks = collect_group_chunks(preset, group, label);
    return chunks.empty() ? nullptr : chunks.front();
}

void append_unique(std::vector<std::string>& target, const std::string& value) {
    if (std::find(target.begin(), target.end(), value) == target.end()) {
        target.push_back(value);
    }
}

std::optional<double> decode_switch_as_boolish(const PresetChunkSummary& chunk, const std::string& key) {
    if (const auto boolean_value = decoded_field_as_boolean(chunk, key); boolean_value.has_value()) {
        return *boolean_value ? 1.0 : 0.0;
    }
    return decoded_field_as_number(chunk, key);
}

void map_effects(const PresetChunkSummary& params_chunk, EffectSettings& effects) {
    const auto add_mapped = [&effects](const std::string& key) {
        append_unique(effects.mapped_parameters, key);
    };

    if (const auto value = decode_switch_as_boolish(params_chunk, "reverb_on"); value.has_value()) {
        effects.reverb.enabled = *value > 0.5;
        add_mapped("reverb_on");
    }
    if (const auto value = decoded_field_as_number(params_chunk, "reverb_wet"); value.has_value()) {
        effects.reverb.wet = value;
        add_mapped("reverb_wet");
    }
    if (const auto value = decoded_field_as_number(params_chunk, "reverb_dry"); value.has_value()) {
        effects.reverb.dry = value;
        add_mapped("reverb_dry");
    }
    if (const auto value = decoded_field_as_number(params_chunk, "reverb_width"); value.has_value()) {
        effects.reverb.width = value;
        add_mapped("reverb_width");
    }

    if (const auto value = decode_switch_as_boolish(params_chunk, "distortion_on"); value.has_value()) {
        effects.distortion.enabled = *value > 0.5;
        add_mapped("distortion_on");
    }
    if (const auto value = decoded_field_as_number(params_chunk, "distortion_drive"); value.has_value()) {
        effects.distortion.drive = value;
        add_mapped("distortion_drive");
    }
    if (const auto value = decoded_field_as_number(params_chunk, "distortion_output"); value.has_value()) {
        effects.distortion.output = value;
        add_mapped("distortion_output");
    }

    if (const auto value = decode_switch_as_boolish(params_chunk, "eq_on"); value.has_value()) {
        effects.equalizer.enabled = *value > 0.5;
        add_mapped("eq_on");
    }
    if (const auto value = decoded_field_as_number(params_chunk, "eq_mid"); value.has_value()) {
        effects.equalizer.mid = value;
        add_mapped("eq_mid");
    }
    if (const auto value = decoded_field_as_number(params_chunk, "eq_freqhigh"); value.has_value()) {
        effects.equalizer.high_frequency = value;
        add_mapped("eq_freqhigh");
    }
    if (const auto value = decoded_field_as_number(params_chunk, "eq_output"); value.has_value()) {
        effects.equalizer.output = value;
        add_mapped("eq_output");
    }

    if (const auto value = decode_switch_as_boolish(params_chunk, "filterone_on"); value.has_value()) {
        effects.filter.enabled = *value > 0.5;
        add_mapped("filterone_on");
    }
    if (const auto value = decoded_field_as_number(params_chunk, "filterone_drive"); value.has_value()) {
        effects.filter.drive = value;
        add_mapped("filterone_drive");
    }
    if (const auto value = decoded_field_as_number(params_chunk, "filterone_blend"); value.has_value()) {
        effects.filter.blend = value;
        add_mapped("filterone_blend");
    }
    if (const auto value = decoded_field_as_number(params_chunk, "lhp_freqhigh"); value.has_value()) {
        effects.filter.high_frequency = value;
        add_mapped("lhp_freqhigh");
    }

    if (const auto value = decoded_field_as_number(params_chunk, "limiter_in"); value.has_value()) {
        effects.limiter.input = value;
        add_mapped("limiter_in");
    }
    if (const auto value = decoded_field_as_number(params_chunk, "limiter_out"); value.has_value()) {
        effects.limiter.output = value;
        add_mapped("limiter_out");
    }
    if (const auto value = decoded_field_as_number(params_chunk, "limiter_limit"); value.has_value()) {
        effects.limiter.limit = value;
        add_mapped("limiter_limit");
    }

    if (const auto value = decoded_field_as_number(params_chunk, "saturator_amount"); value.has_value()) {
        effects.saturation.amount = value;
        add_mapped("saturator_amount");
    }
}

Layer build_layer(const PresetSummary& preset, const SlotGroupSummary& group) {
    Layer layer;
    layer.index = group.group_index;
    layer.active = group.active;

    const auto* params_chunk = first_group_chunk(preset, group, "params");
    const auto* sample_chunk = first_group_chunk(preset, group, "sample");
    const auto* sound_chunk = first_group_chunk(preset, group, "sound");

    if (params_chunk != nullptr) {
        if (const auto value = decoded_field_as_number(*params_chunk, "layer_gain"); value.has_value()) {
            layer.gain = value;
            append_unique(layer.mapped_parameters, "layer_gain");
        }
        if (const auto value = decoded_field_as_number(*params_chunk, "layer_pausedelay"); value.has_value()) {
            layer.delay = value;
            append_unique(layer.mapped_parameters, "layer_pausedelay");
        }
        if (const auto value = decoded_field_as_number(*params_chunk, "layer_startoffset"); value.has_value()) {
            layer.start_offset = value;
            append_unique(layer.mapped_parameters, "layer_startoffset");
        }
        if (const auto value = decoded_field_as_number(*params_chunk, "layer_stopoffset"); value.has_value()) {
            layer.stop_offset = value;
            append_unique(layer.mapped_parameters, "layer_stopoffset");
        }
        if (const auto value = decoded_field_as_number(*params_chunk, "layer_centspitch"); value.has_value()) {
            layer.fine_pitch = value;
            append_unique(layer.mapped_parameters, "layer_centspitch");
        }

        if (const auto value = decode_switch_as_boolish(*params_chunk, "layer_mute"); value.has_value()) {
            layer.mute = *value > 0.5;
            append_unique(layer.mapped_parameters, "layer_mute");
        }
        if (const auto value = decode_switch_as_boolish(*params_chunk, "layer_solo"); value.has_value()) {
            layer.solo = *value > 0.5;
            append_unique(layer.mapped_parameters, "layer_solo");
        }
        if (const auto value = decode_switch_as_boolish(*params_chunk, "layer_reverse"); value.has_value()) {
            layer.reverse = *value > 0.5;
            append_unique(layer.mapped_parameters, "layer_reverse");
        }

        if (const auto value = decoded_field_as_number(*params_chunk, "layer_rndgain"); value.has_value()) {
            layer.random_gain.source_parameter = "layer_rndgain";
            layer.random_gain.amount = value;
            append_unique(layer.mapped_parameters, "layer_rndgain");
        }
        if (const auto value = decoded_field_as_number(*params_chunk, "layer_rndpan"); value.has_value()) {
            layer.random_pan.source_parameter = "layer_rndpan";
            layer.random_pan.amount = value;
            append_unique(layer.mapped_parameters, "layer_rndpan");
        }

        layer.envelope.attack = decoded_field_as_number(*params_chunk, "layer_attack");
        layer.envelope.decay = decoded_field_as_number(*params_chunk, "layer_decay");
        layer.envelope.sustain = decoded_field_as_number(*params_chunk, "layer_sustain");
        layer.envelope.hold = decoded_field_as_number(*params_chunk, "layer_hold");
        layer.envelope.release = decoded_field_as_number(*params_chunk, "layer_release");
        if (layer.envelope.attack.has_value()) append_unique(layer.mapped_parameters, "layer_attack");
        if (layer.envelope.decay.has_value()) append_unique(layer.mapped_parameters, "layer_decay");
        if (layer.envelope.sustain.has_value()) append_unique(layer.mapped_parameters, "layer_sustain");
        if (layer.envelope.hold.has_value()) append_unique(layer.mapped_parameters, "layer_hold");
        if (layer.envelope.release.has_value()) append_unique(layer.mapped_parameters, "layer_release");

        map_effects(*params_chunk, layer.effects);

        static const std::set<std::string> supported_params = {
            "layer_gain", "layer_pausedelay", "layer_startoffset", "layer_stopoffset", "layer_centspitch",
            "layer_mute", "layer_solo", "layer_reverse", "layer_rndgain", "layer_rndpan",
            "layer_attack", "layer_decay", "layer_sustain", "layer_hold", "layer_release",
            "reverb_on", "reverb_wet", "reverb_dry", "reverb_width",
            "distortion_on", "distortion_drive", "distortion_output",
            "eq_on", "eq_mid", "eq_freqhigh", "eq_output",
            "filterone_on", "filterone_drive", "filterone_blend",
            "lhp_freqhigh", "limiter_in", "limiter_out", "limiter_limit", "saturator_amount"
        };
        for (const auto& field : params_chunk->decoded_fields) {
            if (supported_params.find(field.key) == supported_params.end()) {
                append_unique(layer.unsupported_parameters, field.key);
            }
        }
    }

    Region region;
    if (sample_chunk != nullptr) {
        region.start_offset = decoded_field_as_number(*sample_chunk, "startOffset");
        region.end_offset = decoded_field_as_number(*sample_chunk, "stopOffset");
        region.in_point = decoded_field_as_number(*sample_chunk, "InPoint");
        region.out_point = decoded_field_as_number(*sample_chunk, "OutPoint");
        region.loop_start = decoded_field_as_number(*sample_chunk, "loopStart");
        region.loop_end = decoded_field_as_number(*sample_chunk, "loopStop");
        region.loop_crossfade = decoded_field_as_number(*sample_chunk, "loopXfade");
        layer.source_sample_rate = decoded_field_as_number(*sample_chunk, "sourceSampleRate");
        layer.source_midi_root_note = decoded_field_as_integer(*sample_chunk, "midiRootNote");
        layer.source_name = decoded_field_as_string(*sample_chunk, "name");

        const auto loop_type = decoded_field_as_integer(*sample_chunk, "loopType");
        region.loop_enabled =
            (loop_type.has_value() && *loop_type != 0) ||
            (region.loop_end.has_value() && region.loop_start.has_value() && *region.loop_end > *region.loop_start);
    }

    if (sound_chunk != nullptr) {
        layer.source_path = decoded_field_as_string(*sound_chunk, "sourcePath");
        layer.source_file = decoded_field_as_string(*sound_chunk, "sourceFile");
        if (!layer.source_path.has_value()) {
            layer.source_path = decoded_field_as_string(*sound_chunk, "dbPath");
        }
    }

    if (region.start_offset.has_value() || region.end_offset.has_value() ||
        region.in_point.has_value() || region.out_point.has_value()) {
        layer.regions.push_back(region);
    }

    LayerSource source;
    source.name = layer.source_name;
    source.path = layer.source_path;
    source.file = layer.source_file;
    if (layer.embedded_media_reference.has_value()) {
        source.buffer_id = layer.embedded_media_reference;
        source.embedded = true;
    } else if (layer.source_path.has_value()) {
        source.buffer_id = layer.source_path;
    } else if (layer.source_name.has_value()) {
        source.buffer_id = layer.source_name;
    }
    source.regions = layer.regions;
    if (source.name.has_value() || source.path.has_value() || source.file.has_value() || source.buffer_id.has_value()) {
        layer.sources.push_back(std::move(source));
    }

    return layer;
}

}  // namespace

Preset build_import_preset(const FileSummary& summary) {
    Preset imported;
    imported.source_file = summary.input_path;
    imported.diagnostics.uncertain_assumptions = summary.uncertain_assumptions;

    if (summary.presets.empty()) {
        imported.diagnostics.warnings.push_back("No preset rows were found in radiumpresets.");
        return imported;
    }

    const auto& preset = summary.presets.front();
    imported.id = preset.id;
    imported.name = preset.name;
    imported.date_modified = preset.date_modified;
    imported.encrypted = preset.encrypted;
    imported.slot_group_count = preset.slot_groups.size();

    std::map<std::string, const MediaAssetSummary*> assets_by_reference;
    for (const auto& asset : summary.media_assets) {
        assets_by_reference.emplace(asset.reference, &asset);
    }

    for (const auto& group : preset.slot_groups) {
        auto layer = build_layer(preset, group);
        if (layer.active) {
            imported.active_layer_count += 1;
        }
        for (const auto& unsupported : layer.unsupported_parameters) {
            append_unique(imported.diagnostics.unsupported_parameters, unsupported);
        }
        imported.layers.push_back(std::move(layer));
    }

    std::vector<Layer*> active_layers;
    for (auto& layer : imported.layers) {
        if (layer.active) {
            active_layers.push_back(&layer);
        }
    }

    if (preset.ordered_media_references.size() == active_layers.size()) {
        for (std::size_t i = 0; i < active_layers.size(); ++i) {
            const auto& reference = preset.ordered_media_references[i];
            const auto asset_it = assets_by_reference.find(reference);
            if (asset_it != assets_by_reference.end()) {
                active_layers[i]->embedded_media_reference = reference;
                active_layers[i]->embedded_media_path = asset_it->second->extracted_path;
                if (!active_layers[i]->sources.empty()) {
                    active_layers[i]->sources.front().buffer_id = reference;
                    active_layers[i]->sources.front().embedded = true;
                } else {
                    LayerSource source;
                    source.name = active_layers[i]->source_name;
                    source.path = active_layers[i]->source_path;
                    source.file = active_layers[i]->source_file;
                    source.buffer_id = reference;
                    source.embedded = true;
                    source.regions = active_layers[i]->regions;
                    active_layers[i]->sources.push_back(std::move(source));
                }
            } else {
                imported.diagnostics.unmapped_media_references.push_back(reference);
                imported.diagnostics.unmapped_layer_indices.push_back(std::to_string(active_layers[i]->index));
            }
        }
    } else {
        for (auto* layer : active_layers) {
            imported.diagnostics.unmapped_layer_indices.push_back(std::to_string(layer->index));
        }
        for (const auto& reference : preset.ordered_media_references) {
            imported.diagnostics.unmapped_media_references.push_back(reference);
        }
        imported.diagnostics.warnings.push_back(
            "Embedded media assets could not be mapped one-to-one onto active layers using explicit preset reference ordering."
        );
    }

    return imported;
}

}  // namespace radium
