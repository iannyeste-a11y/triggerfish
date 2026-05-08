#include "import_model.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void run_import_model_regression() {
    const auto fixtures_dir = std::filesystem::current_path() / "fixtures" / "radium";
    const auto output_dir = std::filesystem::current_path() / "artifacts" / "model_test_output";
    std::filesystem::create_directories(output_dir);

    const auto inputs = radium::collect_inputs(fixtures_dir);
    require(!inputs.empty(), "no fixtures discovered");

    for (const auto& input : inputs) {
        radium::ParseOptions options;
        options.output_root = output_dir;
        const auto summary = radium::parse_radium_file(input, options);
        const auto preset = radium::build_import_preset(summary);

        require(!preset.name.empty(), input.string() + ": preset name was empty");
        require(preset.slot_group_count == 8, input.string() + ": expected 8 layer groups");
        require(preset.layers.size() == 8, input.string() + ": expected 8 layers in import model");
        require(preset.active_layer_count > 0, input.string() + ": expected at least one active layer");
        require(!preset.diagnostics.uncertain_assumptions.empty(), input.string() + ": expected uncertainty diagnostics");

        std::size_t active_with_sources = 0;
        for (const auto& layer : preset.layers) {
            if (!layer.active) {
                continue;
            }
            if (layer.source_name.has_value()) {
                require(!layer.source_name->empty(), input.string() + ": active layer source name was empty");
            }
            if (layer.source_path.has_value()) {
                require(layer.source_path->find(".wav") != std::string::npos, input.string() + ": active layer source path missing wav hint");
            }
            require(layer.envelope.attack.has_value(), input.string() + ": missing attack value");
            require(layer.envelope.release.has_value(), input.string() + ": missing release value");
            require(layer.gain.has_value(), input.string() + ": missing gain value");
            require(!layer.effects.mapped_parameters.empty(), input.string() + ": missing mapped effect parameters");
            require(!layer.regions.empty(), input.string() + ": missing region data for active layer");
            require(layer.random_gain.amount.has_value(), input.string() + ": missing gain randomization amount");
            require(layer.random_pan.amount.has_value(), input.string() + ": missing pan randomization amount");
            require(layer.embedded_media_reference.has_value(), input.string() + ": missing embedded media reference mapping");
            require(layer.embedded_media_path.has_value(), input.string() + ": missing embedded media path mapping");
            if (layer.source_name.has_value() || layer.source_path.has_value()) {
                active_with_sources += 1;
            }
        }

        require(active_with_sources > 0, input.string() + ": no active layers exposed source metadata");
        require(!preset.diagnostics.unsupported_parameters.empty(), input.string() + ": expected unmapped parameter diagnostics");
        require(preset.diagnostics.unmapped_layer_indices.empty(), input.string() + ": expected active layers to map to embedded media");
    }
}

}  // namespace

int main() {
    try {
        run_import_model_regression();
        std::cout << "import_model_tests: ok\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "import_model_tests: " << ex.what() << '\n';
        return 1;
    }
}
