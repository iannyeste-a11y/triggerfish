add_library(radium_core
    radium_parser.cpp
    json_writer.cpp
    import_model.cpp
    import_to_playback.cpp
    playback_engine.cpp
    streaming_mixer.cpp
    fixture_audio_bridge.cpp
    project_file.cpp
    app_controller.cpp
)

target_include_directories(radium_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third_party/signalsmith-stretch
)

target_link_libraries(radium_core PRIVATE ${RADIUM_SQLITE_TARGET} ZLIB::ZLIB)
