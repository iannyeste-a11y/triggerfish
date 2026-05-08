# AAX SDK path — set via -DAAX_SDK_PATH=... or environment variable
if(DEFINED ENV{AAX_SDK_PATH} AND NOT DEFINED AAX_SDK_PATH)
    set(AAX_SDK_PATH $ENV{AAX_SDK_PATH})
endif()

if(AAX_SDK_PATH)
    juce_set_aax_sdk_path(${AAX_SDK_PATH})
    set(TRIGGERFISH_FORMATS Standalone AAX)
else()
    set(TRIGGERFISH_FORMATS Standalone)
endif()

juce_add_binary_data(TriggerfishAssets
    SOURCES
        assets/triggerfish_logo.png
)

juce_add_plugin(Triggerfish
    COMPANY_NAME "Triggerfish"
    PLUGIN_MANUFACTURER_CODE Tgfh
    PLUGIN_CODE Tgrf
    FORMATS ${TRIGGERFISH_FORMATS}
    PRODUCT_NAME "Triggerfish 1.0"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/assets/triggerfish_icon.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/assets/triggerfish_icon.png"
    NEEDS_MIDI_INPUT TRUE
    IS_SYNTH TRUE
    AAX_IDENTIFIER "com.triggerfish.app"
    COPY_PLUGIN_AFTER_BUILD FALSE
)

target_sources(Triggerfish PRIVATE
    PluginProcessor.cpp
    PluginEditor.cpp
    PortableDataLocation.cpp
    AudioFileLoader.cpp
    LibraryDatabase.cpp
    JuceVst3Host.cpp
    Vst3PluginScanner.cpp
    LookAndFeel_Radium.cpp
    components/LibrarySearchWindow.cpp
    components/ToolbarComponent.cpp
    components/LayerListComponent.cpp
    components/PianoKeyboardComponent.cpp
    components/WaveformComponent.cpp
    components/Vst3InsertRack.cpp
    components/SurroundPannerComponent.cpp
    components/MasterControlsComponent.cpp
    components/SessionRecorderComponent.cpp
    components/StereoLevelMeterComponent.cpp
    components/PictureWindow.cpp
)

target_include_directories(Triggerfish PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(Triggerfish
    PRIVATE
        radium_core
        TriggerfishAssets
        ${RADIUM_SQLITE_TARGET}
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_gui_extra
        juce::juce_video
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
)

if(WIN32)
    target_link_libraries(Triggerfish PRIVATE
        mf
        mfplat
        mfplay
        mfuuid
        evr
    )
endif()

target_compile_definitions(Triggerfish
    PUBLIC
        JUCE_WEB_BROWSER=1
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_PLUGINHOST_VST3=1
)
