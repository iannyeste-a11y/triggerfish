#pragma once

#include "radium_parser.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace radium {

struct RandomizationRange {
    std::string source_parameter;
    std::optional<double> amount;
    std::optional<double> minimum;
    std::optional<double> maximum;
};

struct Envelope {
    std::optional<double> attack;
    std::optional<double> decay;
    std::optional<double> sustain;
    std::optional<double> hold;
    std::optional<double> release;
};

struct EffectSettings {
    struct Reverb {
        bool enabled = false;
        std::optional<double> wet;
        std::optional<double> dry;
        std::optional<double> width;
    };

    struct Distortion {
        bool enabled = false;
        std::optional<double> drive;
        std::optional<double> output;
    };

    struct Equalizer {
        bool enabled = false;
        std::optional<double> mid;
        std::optional<double> high_frequency;
        std::optional<double> output;
    };

    struct Filter {
        bool enabled = false;
        std::optional<double> drive;
        std::optional<double> blend;
        std::optional<double> high_frequency;
    };

    struct Limiter {
        std::optional<double> input;
        std::optional<double> output;
        std::optional<double> limit;
    };

    struct Saturation {
        std::optional<double> amount;
    };

    Reverb reverb;
    Distortion distortion;
    Equalizer equalizer;
    Filter filter;
    Limiter limiter;
    Saturation saturation;
    std::vector<std::string> mapped_parameters;
};

struct Region {
    std::optional<double> start_offset;
    std::optional<double> end_offset;
    std::optional<double> in_point;
    std::optional<double> out_point;
    std::optional<double> loop_start;
    std::optional<double> loop_end;
    std::optional<double> loop_crossfade;
    bool loop_enabled = false;
};

struct LayerSource {
    std::optional<std::string> name;
    std::optional<std::string> path;
    std::optional<std::string> file;
    std::optional<std::string> buffer_id;
    bool embedded = false;
    std::vector<Region> regions;
};

struct ImportDiagnostics {
    std::vector<std::string> warnings;
    std::vector<std::string> uncertain_assumptions;
    std::vector<std::string> unmapped_media_references;
    std::vector<std::string> unmapped_layer_indices;
    std::vector<std::string> unsupported_parameters;
};

struct Layer {
    std::size_t index = 0;
    bool active = false;
    std::optional<std::int64_t> source_midi_root_note;
    std::optional<double> source_sample_rate;
    std::optional<std::string> custom_name;
    std::optional<std::string> source_name;
    std::optional<std::string> source_path;
    std::optional<std::string> source_file;
    std::optional<std::string> embedded_media_reference;
    std::optional<std::string> embedded_media_path;
    std::vector<LayerSource> sources;
    std::optional<double> gain;
    std::optional<double> delay;
    std::optional<double> start_offset;
    std::optional<double> stop_offset;
    std::optional<double> fine_pitch;
    bool mute = false;
    bool solo = false;
    bool reverse = false;
    RandomizationRange random_gain;
    RandomizationRange random_pan;
    Envelope envelope;
    EffectSettings effects;
    std::vector<Region> regions;
    std::vector<std::string> mapped_parameters;
    std::vector<std::string> unsupported_parameters;
};

struct Preset {
    std::filesystem::path source_file;
    std::int64_t id = 0;
    std::string name;
    std::string date_modified;
    bool encrypted = false;
    std::size_t slot_group_count = 0;
    std::size_t active_layer_count = 0;
    std::vector<Layer> layers;
    ImportDiagnostics diagnostics;
};

Preset build_import_preset(const FileSummary& summary);

}  // namespace radium
