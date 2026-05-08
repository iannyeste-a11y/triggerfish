#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace radium {

enum class DecodedFieldType {
    kNumber,
    kInteger,
    kBoolean,
    kString
};

struct DecodedField {
    std::string key;
    DecodedFieldType type = DecodedFieldType::kString;
    double number_value = 0.0;
    std::int64_t integer_value = 0;
    bool boolean_value = false;
    std::string string_value;
};

struct TableInventory {
    std::string name;
    std::vector<std::string> columns;
    std::size_t row_count = 0;
};

struct MediaAssetSummary {
    std::int64_t id = 0;
    std::int64_t preset_id = 0;
    std::string reference;
    std::size_t byte_count = 0;
    bool is_flac = false;
    std::string extracted_path;
};

struct PresetChunkSummary {
    std::size_t sequence_index = 0;
    std::size_t offset = 0;
    std::size_t compressed_size = 0;
    std::size_t inflated_size = 0;
    std::string label;
    std::string preview;
    std::vector<DecodedField> decoded_fields;
};

struct SlotGroupSummary {
    std::size_t group_index = 0;
    bool active = false;
    std::vector<std::string> chunk_labels;
    std::vector<std::size_t> chunk_sequence_indices;
    std::size_t compressed_chunk_count = 0;
};

struct PresetSummary {
    std::int64_t id = 0;
    std::string name;
    std::string date_modified;
    bool encrypted = false;
    std::size_t blob_size = 0;
    std::vector<PresetChunkSummary> chunks;
    std::vector<SlotGroupSummary> slot_groups;
    std::vector<std::string> ordered_media_references;
};

struct FileSummary {
    std::string format = "radium_summary_v1";
    std::filesystem::path input_path;
    bool opened_as_sqlite = false;
    std::string sqlite_schema_name;
    std::int64_t sqlite_schema_version = 0;
    std::vector<TableInventory> tables;
    std::vector<PresetSummary> presets;
    std::vector<MediaAssetSummary> media_assets;
    std::vector<std::string> uncertain_assumptions;
};

struct ParseOptions {
    std::filesystem::path output_root;
    bool write_extracted_media = true;
};

std::vector<std::filesystem::path> collect_inputs(const std::filesystem::path& input_path);
FileSummary parse_radium_file(const std::filesystem::path& input_path, const ParseOptions& options);
std::string to_json(const FileSummary& summary);
const DecodedField* find_decoded_field(const PresetChunkSummary& chunk, const std::string& key);
std::optional<double> decoded_field_as_number(const PresetChunkSummary& chunk, const std::string& key);
std::optional<std::int64_t> decoded_field_as_integer(const PresetChunkSummary& chunk, const std::string& key);
std::optional<bool> decoded_field_as_boolean(const PresetChunkSummary& chunk, const std::string& key);
std::optional<std::string> decoded_field_as_string(const PresetChunkSummary& chunk, const std::string& key);

}  // namespace radium
