cmake_minimum_required(VERSION 3.22)
project(Triggerfish VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- JUCE ---
include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4
)
FetchContent_MakeAvailable(JUCE)

# --- External dependencies ---
find_package(ZLIB REQUIRED)
if(WIN32)
    find_package(unofficial-sqlite3 CONFIG QUIET)
    if(TARGET unofficial::sqlite3::sqlite3)
        set(RADIUM_SQLITE_TARGET unofficial::sqlite3::sqlite3)
    else()
        find_package(SQLite3 REQUIRED)
        set(RADIUM_SQLITE_TARGET SQLite::SQLite3)
    endif()
else()
    find_package(SQLite3 REQUIRED)
    set(RADIUM_SQLITE_TARGET SQLite::SQLite3)
endif()

# --- Core engine library (no JUCE dependency) ---
add_subdirectory(core)

# --- JUCE plugin (Standalone + AAX) ---
add_subdirectory(plugin)

# --- Tests ---
enable_testing()

add_executable(radium_parser_tests tests/parser_tests.cpp)
target_link_libraries(radium_parser_tests PRIVATE radium_core)
add_test(NAME radium_parser_tests COMMAND radium_parser_tests)
set_tests_properties(radium_parser_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(import_model_tests tests/import_model_tests.cpp)
target_link_libraries(import_model_tests PRIVATE radium_core)
add_test(NAME import_model_tests COMMAND import_model_tests)
set_tests_properties(import_model_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(playback_engine_tests tests/playback_engine_tests.cpp)
target_link_libraries(playback_engine_tests PRIVATE radium_core)
add_test(NAME playback_engine_tests COMMAND playback_engine_tests)
set_tests_properties(playback_engine_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(fixture_audio_bridge_tests tests/fixture_audio_bridge_tests.cpp)
target_link_libraries(fixture_audio_bridge_tests PRIVATE radium_core)
add_test(NAME fixture_audio_bridge_tests COMMAND fixture_audio_bridge_tests)
set_tests_properties(fixture_audio_bridge_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(app_controller_tests
    tests/app_controller_tests.cpp
)
target_link_libraries(app_controller_tests PRIVATE radium_core)
add_test(NAME app_controller_tests COMMAND app_controller_tests)
set_tests_properties(app_controller_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# --- Legacy CLI tool ---
add_executable(radium_parser_cli src/main.cpp)
target_link_libraries(radium_parser_cli PRIVATE radium_core)
