#include "radium_parser.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>

namespace radium {
namespace {

using sqlite3 = struct sqlite3;
using sqlite3_stmt = struct sqlite3_stmt;

extern "C" {
int sqlite3_open_v2(const char* filename, sqlite3** ppDb, int flags, const char* zVfs);
int sqlite3_close(sqlite3* db);
const char* sqlite3_errmsg(sqlite3* db);
int sqlite3_prepare_v2(sqlite3* db, const char* zSql, int nByte, sqlite3_stmt** ppStmt, const char** pzTail);
int sqlite3_step(sqlite3_stmt* pStmt);
int sqlite3_finalize(sqlite3_stmt* pStmt);
int sqlite3_column_count(sqlite3_stmt* pStmt);
const char* sqlite3_column_name(sqlite3_stmt* pStmt, int N);
int sqlite3_column_type(sqlite3_stmt* pStmt, int iCol);
const unsigned char* sqlite3_column_text(sqlite3_stmt* pStmt, int iCol);
const void* sqlite3_column_blob(sqlite3_stmt* pStmt, int iCol);
int sqlite3_column_bytes(sqlite3_stmt* pStmt, int iCol);
long long sqlite3_column_int64(sqlite3_stmt* pStmt, int iCol);
int sqlite3_column_int(sqlite3_stmt* pStmt, int iCol);
using alloc_func = void* (*)(void*, unsigned int, unsigned int);
using free_func = void (*)(void*, void*);
struct z_stream_s {
    unsigned char* next_in;
    unsigned int avail_in;
    unsigned long total_in;
    unsigned char* next_out;
    unsigned int avail_out;
    unsigned long total_out;
    char* msg;
    void* state;
    alloc_func zalloc;
    free_func zfree;
    void* opaque;
    int data_type;
    unsigned long adler;
    unsigned long reserved;
};
const char* zlibVersion(void);
int inflateInit_(z_stream_s* strm, const char* version, int stream_size);
int inflate(z_stream_s* strm, int flush);
int inflateEnd(z_stream_s* strm);
}

constexpr int SQLITE_OK = 0;
constexpr int SQLITE_ROW = 100;
constexpr int SQLITE_DONE = 101;
constexpr int SQLITE_OPEN_READONLY = 0x00000001;

constexpr int SQLITE_INTEGER = 1;
constexpr int SQLITE_TEXT = 3;

constexpr int Z_OK = 0;
constexpr int Z_STREAM_END = 1;
constexpr int Z_NO_FLUSH = 0;
constexpr int Z_BUF_ERROR = -5;

class SqliteStatement {
public:
    SqliteStatement(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            throw std::runtime_error("sqlite prepare failed");
        }
    }

    ~SqliteStatement() {
        if (stmt_ != nullptr) {
            sqlite3_finalize(stmt_);
        }
    }

    sqlite3_stmt* get() const { return stmt_; }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

class SqliteConnection {
public:
    explicit SqliteConnection(const std::filesystem::path& path) {
        if (sqlite3_open_v2(path.string().c_str(), &db_, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            std::string message = db_ ? sqlite3_errmsg(db_) : "unable to open sqlite database";
            if (db_ != nullptr) {
                sqlite3_close(db_);
            }
            throw std::runtime_error(message);
        }
    }

    ~SqliteConnection() {
        if (db_ != nullptr) {
            sqlite3_close(db_);
        }
    }

    sqlite3* get() const { return db_; }

private:
    sqlite3* db_ = nullptr;
};

std::string as_text(sqlite3_stmt* stmt, int column) {
    const auto* value = sqlite3_column_text(stmt, column);
    return value ? reinterpret_cast<const char*>(value) : std::string();
}

std::vector<std::byte> as_blob(sqlite3_stmt* stmt, int column) {
    const auto* data = static_cast<const std::byte*>(sqlite3_column_blob(stmt, column));
    const int size = sqlite3_column_bytes(stmt, column);
    if (data == nullptr || size <= 0) {
        return {};
    }
    return std::vector<std::byte>(data, data + size);
}

std::string sanitize_component(const std::string& value) {
    std::string sanitized;
    sanitized.reserve(value.size());
    for (char ch : value) {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_') {
            sanitized.push_back(ch);
        } else {
            sanitized.push_back('_');
        }
    }
    return sanitized;
}

std::string preview_text(std::string_view bytes) {
    std::string preview;
    preview.reserve(std::min<std::size_t>(bytes.size(), 96));
    for (char ch : bytes) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (u == 0) {
            if (!preview.empty() && preview.back() != ' ') {
                preview.push_back(' ');
            }
            continue;
        }
        if (std::isprint(u)) {
            preview.push_back(ch);
        } else {
            preview.push_back('.');
        }
        if (preview.size() >= 96) {
            break;
        }
    }
    while (!preview.empty() && preview.back() == ' ') {
        preview.pop_back();
    }
    return preview;
}

std::optional<std::vector<std::byte>> inflate_stream(const std::byte* data, std::size_t size, std::size_t* consumed) {
    z_stream_s stream{};
    if (inflateInit_(&stream, zlibVersion(), sizeof(stream)) != Z_OK) {
        return std::nullopt;
    }

    stream.next_in = reinterpret_cast<unsigned char*>(const_cast<std::byte*>(data));
    stream.avail_in = static_cast<unsigned int>(size);

    std::vector<std::byte> output;
    std::array<std::byte, 8192> buffer{};

    while (true) {
        stream.next_out = reinterpret_cast<unsigned char*>(buffer.data());
        stream.avail_out = static_cast<unsigned int>(buffer.size());
        const int result = inflate(&stream, Z_NO_FLUSH);
        const std::size_t produced = buffer.size() - stream.avail_out;
        output.insert(output.end(), buffer.data(), buffer.data() + produced);

        if (result == Z_STREAM_END) {
            *consumed = stream.total_in;
            inflateEnd(&stream);
            return output;
        }
        if (result != Z_OK && result != Z_BUF_ERROR) {
            break;
        }
        if (result == Z_BUF_ERROR && stream.avail_in == 0) {
            break;
        }
    }

    inflateEnd(&stream);
    return std::nullopt;
}

std::string classify_chunk_label(const std::vector<std::byte>& inflated) {
    const std::string_view text(reinterpret_cast<const char*>(inflated.data()), inflated.size());
    static constexpr std::array<const char*, 6> known_labels = {
        "params", "misc", "modsettings", "pluginParams", "sample", "sound"
    };
    for (const char* label : known_labels) {
        if (text.rfind(label, 0) == 0) {
            return label;
        }
    }
    return "unknown";
}

std::string trim_c_string_bytes(const char* data, std::size_t size) {
    std::size_t length = 0;
    while (length < size && data[length] != '\0') {
        ++length;
    }
    return std::string(data, data + length);
}

std::vector<DecodedField> decode_scalar_fields(const std::vector<std::byte>& inflated) {
    std::vector<DecodedField> fields;
    const auto* bytes = reinterpret_cast<const unsigned char*>(inflated.data());
    const std::size_t size = inflated.size();
    std::size_t pos = 0;
    while (pos < size && bytes[pos] != 0) {
        ++pos;
    }
    if (pos >= size) {
        return fields;
    }
    ++pos;

    while (pos < size) {
        const std::size_t key_start = pos;
        while (pos < size && bytes[pos] != 0) {
            ++pos;
        }
        if (pos >= size) {
            break;
        }

        const std::string key(reinterpret_cast<const char*>(bytes + key_start), pos - key_start);
        ++pos;
        if (key.empty()) {
            continue;
        }
        if (std::any_of(key.begin(), key.end(), [](unsigned char ch) {
                return ch < 32 || ch > 126;
            })) {
            continue;
        }
        if (pos + 2 >= size || bytes[pos] != 0x01) {
            continue;
        }

        const unsigned char size_code = bytes[pos + 1];
        const unsigned char type_code = bytes[pos + 2];
        pos += 3;

        DecodedField field;
        field.key = key;

        if (size_code == 0x09 && type_code == 0x04 && pos + 8 <= size) {
            double value = 0.0;
            std::memcpy(&value, bytes + pos, sizeof(value));
            field.type = DecodedFieldType::kNumber;
            field.number_value = value;
            pos += 8;
        } else if (size_code == 0x05 && type_code == 0x01 && pos + 4 <= size) {
            std::int32_t value = 0;
            std::memcpy(&value, bytes + pos, sizeof(value));
            field.type = DecodedFieldType::kInteger;
            field.integer_value = value;
            pos += 4;
        } else if (type_code == 0x05 && pos + size_code <= size) {
            field.type = DecodedFieldType::kString;
            field.string_value = trim_c_string_bytes(reinterpret_cast<const char*>(bytes + pos), size_code);
            pos += size_code;
        } else if (size_code == 0x01 && (type_code == 0x02 || type_code == 0x03)) {
            field.type = DecodedFieldType::kBoolean;
            field.boolean_value = type_code == 0x02;
        } else {
            break;
        }

        fields.push_back(std::move(field));
    }

    return fields;
}

std::vector<TableInventory> read_table_inventory(sqlite3* db) {
    std::vector<TableInventory> tables;
    SqliteStatement list_tables(db, "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name");
    while (sqlite3_step(list_tables.get()) == SQLITE_ROW) {
        TableInventory table;
        table.name = as_text(list_tables.get(), 0);

        SqliteStatement pragma(db, ("PRAGMA table_info(" + table.name + ")").c_str());
        while (sqlite3_step(pragma.get()) == SQLITE_ROW) {
            table.columns.push_back(as_text(pragma.get(), 1));
        }

        SqliteStatement count_stmt(db, ("SELECT COUNT(*) FROM " + table.name).c_str());
        if (sqlite3_step(count_stmt.get()) == SQLITE_ROW) {
            table.row_count = static_cast<std::size_t>(sqlite3_column_int64(count_stmt.get(), 0));
        }
        tables.push_back(std::move(table));
    }
    return tables;
}

void read_schema(sqlite3* db, FileSummary& summary) {
    SqliteStatement stmt(db, "SELECT schema, version FROM schema LIMIT 1");
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        summary.sqlite_schema_name = as_text(stmt.get(), 0);
        summary.sqlite_schema_version = sqlite3_column_int64(stmt.get(), 1);
    }
}

std::vector<MediaAssetSummary> read_media_assets(
    sqlite3* db,
    const std::filesystem::path& input_path,
    const ParseOptions& options
) {
    std::vector<MediaAssetSummary> assets;
    SqliteStatement stmt(
        db,
        "SELECT id, reference, data, date_modified, preset_id FROM radiumdata ORDER BY id"
    );

    const std::filesystem::path asset_root =
        options.output_root / sanitize_component(input_path.stem().string()) / "media";
    if (options.write_extracted_media) {
        std::filesystem::create_directories(asset_root);
    }

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        MediaAssetSummary asset;
        asset.id = sqlite3_column_int64(stmt.get(), 0);
        asset.reference = as_text(stmt.get(), 1);
        asset.preset_id = sqlite3_column_int64(stmt.get(), 4);
        auto blob = as_blob(stmt.get(), 2);
        asset.byte_count = blob.size();
        asset.is_flac = blob.size() >= 4 &&
            std::memcmp(blob.data(), "fLaC", 4) == 0;

        if (options.write_extracted_media) {
            const std::string filename =
                std::to_string(asset.id) + "_" + sanitize_component(asset.reference) +
                (asset.is_flac ? ".flac" : ".bin");
            const std::filesystem::path target = asset_root / filename;
            std::ofstream stream(target, std::ios::binary);
            stream.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
            asset.extracted_path = target.generic_string();
        }
        assets.push_back(std::move(asset));
    }
    return assets;
}

std::vector<std::string> collect_ordered_media_references(
    const std::vector<std::byte>& blob,
    const std::vector<std::string>& references
) {
    struct Hit {
        std::size_t position = 0;
        std::string reference;
    };

    std::vector<Hit> hits;
    const std::string_view blob_view(reinterpret_cast<const char*>(blob.data()), blob.size());
    for (const auto& reference : references) {
        const auto pos = blob_view.find(reference);
        if (pos != std::string_view::npos) {
            hits.push_back(Hit{pos, reference});
        }
    }

    std::sort(hits.begin(), hits.end(), [](const Hit& lhs, const Hit& rhs) {
        return lhs.position < rhs.position;
    });

    std::vector<std::string> ordered;
    ordered.reserve(hits.size());
    for (const auto& hit : hits) {
        ordered.push_back(hit.reference);
    }
    return ordered;
}

PresetSummary parse_preset_row(sqlite3_stmt* stmt, const std::vector<std::string>& media_references) {
    PresetSummary preset;
    preset.id = sqlite3_column_int64(stmt, 0);
    preset.name = as_text(stmt, 1);
    auto blob = as_blob(stmt, 2);
    preset.blob_size = blob.size();
    preset.ordered_media_references = collect_ordered_media_references(blob, media_references);
    preset.date_modified = as_text(stmt, 3);
    preset.encrypted = sqlite3_column_int(stmt, 4) != 0;

    std::size_t offset = 0;
    std::size_t sequence_index = 0;
    SlotGroupSummary current_group;
    bool have_current_group = false;

    while (offset + 2 < blob.size()) {
        if (blob[offset] != std::byte{0x78}) {
            ++offset;
            continue;
        }
        std::size_t consumed = 0;
        auto inflated = inflate_stream(blob.data() + offset, blob.size() - offset, &consumed);
        if (!inflated.has_value() || consumed == 0) {
            ++offset;
            continue;
        }

        PresetChunkSummary chunk;
        chunk.sequence_index = sequence_index++;
        chunk.offset = offset;
        chunk.compressed_size = consumed;
        chunk.inflated_size = inflated->size();
        chunk.label = classify_chunk_label(*inflated);
        chunk.preview = preview_text(
            std::string_view(reinterpret_cast<const char*>(inflated->data()), inflated->size())
        );
        chunk.decoded_fields = decode_scalar_fields(*inflated);
        preset.chunks.push_back(chunk);

        if (chunk.label == "params") {
            if (have_current_group) {
                preset.slot_groups.push_back(current_group);
            }
            current_group = SlotGroupSummary{};
            current_group.group_index = preset.slot_groups.size();
            have_current_group = true;
        }

        if (have_current_group) {
            current_group.chunk_labels.push_back(chunk.label);
            current_group.chunk_sequence_indices.push_back(chunk.sequence_index);
            current_group.compressed_chunk_count += 1;
            if (chunk.label == "sample" || chunk.label == "sound") {
                current_group.active = true;
            }
        }

        offset += consumed;
    }

    if (have_current_group) {
        preset.slot_groups.push_back(current_group);
    }

    return preset;
}

std::vector<PresetSummary> read_presets(sqlite3* db, const std::vector<MediaAssetSummary>& media_assets) {
    std::vector<PresetSummary> presets;
    std::vector<std::string> media_references;
    media_references.reserve(media_assets.size());
    for (const auto& asset : media_assets) {
        media_references.push_back(asset.reference);
    }
    SqliteStatement stmt(
        db,
        "SELECT id, preset_name, data, date_modified, encrypted, security_hash, tags "
        "FROM radiumpresets ORDER BY id"
    );
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        presets.push_back(parse_preset_row(stmt.get(), media_references));
    }
    return presets;
}

}  // namespace

std::vector<std::filesystem::path> collect_inputs(const std::filesystem::path& input_path) {
    std::vector<std::filesystem::path> inputs;
    if (std::filesystem::is_regular_file(input_path)) {
        inputs.push_back(input_path);
    } else if (std::filesystem::is_directory(input_path)) {
        for (const auto& entry : std::filesystem::directory_iterator(input_path)) {
            if (entry.is_regular_file() && entry.path().extension() == ".radium") {
                inputs.push_back(entry.path());
            }
        }
        std::sort(inputs.begin(), inputs.end());
    }
    return inputs;
}

FileSummary parse_radium_file(const std::filesystem::path& input_path, const ParseOptions& options) {
    FileSummary summary;
    summary.input_path = std::filesystem::absolute(input_path);
    summary.uncertain_assumptions = {
        "Slot-group reconstruction currently treats each repeated zlib sequence beginning with 'params' as one logical group.",
        "Active versus inactive slot detection is currently inferred from the presence of 'sample' or 'sound' chunks, not a fully decoded semantic flag.",
        "Chunk payloads beyond the known labels are preserved as raw previews and sizes; field-level decoding remains incomplete.",
        "The parser assumes the preset blob uses zlib-compressed members when a valid stream can be inflated."
    };

    SqliteConnection db(input_path);
    summary.opened_as_sqlite = true;
    summary.tables = read_table_inventory(db.get());
    read_schema(db.get(), summary);
    summary.media_assets = read_media_assets(db.get(), input_path, options);
    summary.presets = read_presets(db.get(), summary.media_assets);
    return summary;
}

const DecodedField* find_decoded_field(const PresetChunkSummary& chunk, const std::string& key) {
    for (const auto& field : chunk.decoded_fields) {
        if (field.key == key) {
            return &field;
        }
    }
    return nullptr;
}

std::optional<double> decoded_field_as_number(const PresetChunkSummary& chunk, const std::string& key) {
    const auto* field = find_decoded_field(chunk, key);
    if (field == nullptr) {
        return std::nullopt;
    }
    if (field->type == DecodedFieldType::kNumber) {
        return field->number_value;
    }
    if (field->type == DecodedFieldType::kBoolean) {
        return field->boolean_value ? 1.0 : 0.0;
    }
    if (field->type == DecodedFieldType::kInteger) {
        return static_cast<double>(field->integer_value);
    }
    return std::nullopt;
}

std::optional<std::int64_t> decoded_field_as_integer(const PresetChunkSummary& chunk, const std::string& key) {
    const auto* field = find_decoded_field(chunk, key);
    if (field == nullptr) {
        return std::nullopt;
    }
    if (field->type == DecodedFieldType::kInteger) {
        return field->integer_value;
    }
    if (field->type == DecodedFieldType::kBoolean) {
        return field->boolean_value ? 1 : 0;
    }
    return std::nullopt;
}

std::optional<bool> decoded_field_as_boolean(const PresetChunkSummary& chunk, const std::string& key) {
    const auto* field = find_decoded_field(chunk, key);
    if (field == nullptr) {
        return std::nullopt;
    }
    if (field->type == DecodedFieldType::kBoolean) {
        return field->boolean_value;
    }
    if (field->type == DecodedFieldType::kInteger) {
        return field->integer_value != 0;
    }
    if (field->type == DecodedFieldType::kNumber) {
        return field->number_value > 0.5;
    }
    return std::nullopt;
}

std::optional<std::string> decoded_field_as_string(const PresetChunkSummary& chunk, const std::string& key) {
    const auto* field = find_decoded_field(chunk, key);
    if (field == nullptr || field->type != DecodedFieldType::kString) {
        return std::nullopt;
    }
    return field->string_value;
}

}  // namespace radium
