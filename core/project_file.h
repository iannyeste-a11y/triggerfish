#pragma once

#include "app_controller.h"

#include <filesystem>
#include <string>

namespace radium {

bool save_project_file(
    const std::filesystem::path& output_path,
    const Preset& preset,
    const std::vector<AppController::LayerOverride>& layer_overrides,
    const std::vector<std::optional<AppController::LayerEditState>>& layer_edit_states,
    const AppController::AuxTrackState& aux_track_state,
    RecordBusMode record_bus_mode,
    TriggerMode trigger_mode,
    int octave,
    const std::optional<std::filesystem::path>& project_picture_path,
    const std::filesystem::path& session_recordings_directory,
    const std::vector<SessionRecordingInfo>& session_recordings,
    std::optional<std::size_t> selected_session_recording_index,
    std::string* error_message
);

bool load_project_file(
    const std::filesystem::path& input_path,
    Preset& preset,
    std::vector<AppController::LayerOverride>& layer_overrides,
    std::vector<std::optional<AppController::LayerEditState>>& layer_edit_states,
    AppController::AuxTrackState& aux_track_state,
    RecordBusMode& record_bus_mode,
    TriggerMode& trigger_mode,
    int& octave,
    std::optional<std::filesystem::path>& project_picture_path,
    std::filesystem::path& session_recordings_directory,
    std::vector<SessionRecordingInfo>& session_recordings,
    std::optional<std::size_t>& selected_session_recording_index,
    std::string* error_message
);

}  // namespace radium
