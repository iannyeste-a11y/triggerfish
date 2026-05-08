#include "radium_parser.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {

void print_usage() {
    std::cerr
        << "Usage: radium_parser_cli <input-path> [--output-dir <dir>] [--summary-dir <dir>]\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            print_usage();
            return 1;
        }

        std::filesystem::path input_path;
        std::filesystem::path output_dir = "artifacts/extracted";
        std::filesystem::path summary_dir;

        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--output-dir" && i + 1 < argc) {
                output_dir = argv[++i];
            } else if (arg == "--summary-dir" && i + 1 < argc) {
                summary_dir = argv[++i];
            } else if (arg == "--help" || arg == "-h") {
                print_usage();
                return 0;
            } else if (input_path.empty()) {
                input_path = arg;
            } else {
                throw std::runtime_error("unexpected argument: " + arg);
            }
        }

        if (input_path.empty()) {
            throw std::runtime_error("missing input path");
        }

        const auto inputs = radium::collect_inputs(input_path);
        if (inputs.empty()) {
            throw std::runtime_error("no .radium inputs found");
        }

        std::filesystem::create_directories(output_dir);
        if (!summary_dir.empty()) {
            std::filesystem::create_directories(summary_dir);
        }

        for (const auto& path : inputs) {
            radium::ParseOptions options;
            options.output_root = output_dir;
            const auto summary = radium::parse_radium_file(path, options);
            const std::string json = radium::to_json(summary);

            if (summary_dir.empty()) {
                std::cout << json;
            } else {
                const auto summary_path =
                    summary_dir / (path.stem().string() + ".summary.json");
                std::ofstream stream(summary_path);
                stream << json;
            }
        }
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
