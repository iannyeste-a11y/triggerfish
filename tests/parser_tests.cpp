#include "radium_parser.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path repo_root() {
    return std::filesystem::current_path();
}

void run_fixture_regression() {
    const auto fixtures_dir = repo_root() / "fixtures" / "radium";
    const auto output_dir = repo_root() / "artifacts" / "test_output";
    std::filesystem::create_directories(output_dir);

    const auto inputs = radium::collect_inputs(fixtures_dir);
    require(!inputs.empty(), "no fixtures discovered");

    for (const auto& input : inputs) {
        radium::ParseOptions options;
        options.output_root = output_dir;
        const auto summary = radium::parse_radium_file(input, options);

        require(summary.opened_as_sqlite, input.string() + ": did not open as sqlite");
        const bool known_schema_name =
            summary.sqlite_schema_name == "Soundminer v5Pro" ||
            summary.sqlite_schema_name == "plugin";
        require(known_schema_name, input.string() + ": unexpected schema name");
        require(summary.sqlite_schema_version == 1, input.string() + ": unexpected schema version");
        require(summary.presets.size() == 1, input.string() + ": expected one preset row");
        require(!summary.media_assets.empty(), input.string() + ": expected embedded media");

        bool saw_schema = false;
        bool saw_radiumpresets = false;
        bool saw_radiumdata = false;
        for (const auto& table : summary.tables) {
            if (table.name == "schema") {
                saw_schema = true;
            }
            if (table.name == "radiumpresets") {
                saw_radiumpresets = true;
            }
            if (table.name == "radiumdata") {
                saw_radiumdata = true;
            }
        }
        require(saw_schema && saw_radiumpresets && saw_radiumdata, input.string() + ": missing core table inventory");

        const auto& preset = summary.presets.front();
        require(preset.slot_groups.size() == 8, input.string() + ": expected 8 slot groups");
        require(!preset.chunks.empty(), input.string() + ": expected chunk inventory");

        std::size_t active_slots = 0;
        for (const auto& group : preset.slot_groups) {
            if (group.active) {
                ++active_slots;
            }
        }
        require(active_slots > 0, input.string() + ": expected at least one active slot");

        std::size_t flac_count = 0;
        for (const auto& asset : summary.media_assets) {
            if (asset.is_flac) {
                ++flac_count;
            }
            require(!asset.extracted_path.empty(), input.string() + ": missing extracted path");
            require(std::filesystem::exists(asset.extracted_path), input.string() + ": extracted asset file missing");
        }
        require(flac_count == summary.media_assets.size(), input.string() + ": expected all embedded media to be FLAC");

        const std::string json_a = radium::to_json(summary);
        const std::string json_b = radium::to_json(summary);
        require(json_a == json_b, input.string() + ": JSON summary was not deterministic");
    }
}

}  // namespace

int main() {
    try {
        run_fixture_regression();
        std::cout << "parser_tests: ok\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "parser_tests: " << ex.what() << '\n';
        return 1;
    }
}
