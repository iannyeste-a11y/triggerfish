#include "radium_parser.h"

#include <sstream>

namespace radium {
namespace {

std::string escape_json(const std::string& value) {
    std::ostringstream out;
    for (unsigned char ch : value) {
        switch (ch) {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out << "\\u00" << hex[(ch >> 4) & 0x0f] << hex[ch & 0x0f];
                } else {
                    out << static_cast<char>(ch);
                }
                break;
        }
    }
    return out.str();
}

void indent(std::ostringstream& out, int depth) {
    for (int i = 0; i < depth; ++i) {
        out << "  ";
    }
}

void write_string(std::ostringstream& out, const std::string& value) {
    out << '"' << escape_json(value) << '"';
}

void write_string_array(std::ostringstream& out, const std::vector<std::string>& values, int depth) {
    out << "[\n";
    for (std::size_t i = 0; i < values.size(); ++i) {
        indent(out, depth + 1);
        write_string(out, values[i]);
        out << (i + 1 < values.size() ? ",\n" : "\n");
    }
    indent(out, depth);
    out << "]";
}

}  // namespace

std::string to_json(const FileSummary& summary) {
    std::ostringstream out;
    out << "{\n";
    indent(out, 1);
    out << "\"format\": ";
    write_string(out, summary.format);
    out << ",\n";
    indent(out, 1);
    out << "\"input_path\": ";
    write_string(out, summary.input_path.generic_string());
    out << ",\n";
    indent(out, 1);
    out << "\"opened_as_sqlite\": " << (summary.opened_as_sqlite ? "true" : "false") << ",\n";
    indent(out, 1);
    out << "\"schema\": {\n";
    indent(out, 2);
    out << "\"name\": ";
    write_string(out, summary.sqlite_schema_name);
    out << ",\n";
    indent(out, 2);
    out << "\"version\": " << summary.sqlite_schema_version << "\n";
    indent(out, 1);
    out << "},\n";

    indent(out, 1);
    out << "\"tables\": [\n";
    for (std::size_t i = 0; i < summary.tables.size(); ++i) {
        const auto& table = summary.tables[i];
        indent(out, 2);
        out << "{\n";
        indent(out, 3);
        out << "\"name\": ";
        write_string(out, table.name);
        out << ",\n";
        indent(out, 3);
        out << "\"row_count\": " << table.row_count << ",\n";
        indent(out, 3);
        out << "\"columns\": ";
        write_string_array(out, table.columns, 3);
        out << "\n";
        indent(out, 2);
        out << "}" << (i + 1 < summary.tables.size() ? ",\n" : "\n");
    }
    indent(out, 1);
    out << "],\n";

    indent(out, 1);
    out << "\"presets\": [\n";
    for (std::size_t i = 0; i < summary.presets.size(); ++i) {
        const auto& preset = summary.presets[i];
        indent(out, 2);
        out << "{\n";
        indent(out, 3);
        out << "\"id\": " << preset.id << ",\n";
        indent(out, 3);
        out << "\"name\": ";
        write_string(out, preset.name);
        out << ",\n";
        indent(out, 3);
        out << "\"date_modified\": ";
        write_string(out, preset.date_modified);
        out << ",\n";
        indent(out, 3);
        out << "\"encrypted\": " << (preset.encrypted ? "true" : "false") << ",\n";
        indent(out, 3);
        out << "\"blob_size\": " << preset.blob_size << ",\n";

        indent(out, 3);
        out << "\"chunks\": [\n";
        for (std::size_t j = 0; j < preset.chunks.size(); ++j) {
            const auto& chunk = preset.chunks[j];
            indent(out, 4);
            out << "{\n";
            indent(out, 5);
            out << "\"sequence_index\": " << chunk.sequence_index << ",\n";
            indent(out, 5);
            out << "\"offset\": " << chunk.offset << ",\n";
            indent(out, 5);
            out << "\"compressed_size\": " << chunk.compressed_size << ",\n";
            indent(out, 5);
            out << "\"inflated_size\": " << chunk.inflated_size << ",\n";
            indent(out, 5);
            out << "\"label\": ";
            write_string(out, chunk.label);
            out << ",\n";
            indent(out, 5);
            out << "\"preview\": ";
            write_string(out, chunk.preview);
            out << "\n";
            indent(out, 4);
            out << "}" << (j + 1 < preset.chunks.size() ? ",\n" : "\n");
        }
        indent(out, 3);
        out << "],\n";

        indent(out, 3);
        out << "\"slot_groups\": [\n";
        for (std::size_t j = 0; j < preset.slot_groups.size(); ++j) {
            const auto& group = preset.slot_groups[j];
            indent(out, 4);
            out << "{\n";
            indent(out, 5);
            out << "\"group_index\": " << group.group_index << ",\n";
            indent(out, 5);
            out << "\"active\": " << (group.active ? "true" : "false") << ",\n";
            indent(out, 5);
            out << "\"compressed_chunk_count\": " << group.compressed_chunk_count << ",\n";
            indent(out, 5);
            out << "\"chunk_labels\": ";
            write_string_array(out, group.chunk_labels, 5);
            out << "\n";
            indent(out, 4);
            out << "}" << (j + 1 < preset.slot_groups.size() ? ",\n" : "\n");
        }
        indent(out, 3);
        out << "]\n";
        indent(out, 2);
        out << "}" << (i + 1 < summary.presets.size() ? ",\n" : "\n");
    }
    indent(out, 1);
    out << "],\n";

    indent(out, 1);
    out << "\"media_assets\": [\n";
    for (std::size_t i = 0; i < summary.media_assets.size(); ++i) {
        const auto& asset = summary.media_assets[i];
        indent(out, 2);
        out << "{\n";
        indent(out, 3);
        out << "\"id\": " << asset.id << ",\n";
        indent(out, 3);
        out << "\"preset_id\": " << asset.preset_id << ",\n";
        indent(out, 3);
        out << "\"reference\": ";
        write_string(out, asset.reference);
        out << ",\n";
        indent(out, 3);
        out << "\"byte_count\": " << asset.byte_count << ",\n";
        indent(out, 3);
        out << "\"is_flac\": " << (asset.is_flac ? "true" : "false") << ",\n";
        indent(out, 3);
        out << "\"extracted_path\": ";
        write_string(out, asset.extracted_path);
        out << "\n";
        indent(out, 2);
        out << "}" << (i + 1 < summary.media_assets.size() ? ",\n" : "\n");
    }
    indent(out, 1);
    out << "],\n";

    indent(out, 1);
    out << "\"uncertain_assumptions\": ";
    write_string_array(out, summary.uncertain_assumptions, 1);
    out << "\n";
    out << "}\n";
    return out.str();
}

}  // namespace radium
