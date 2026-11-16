cmake_minimum_required(VERSION 3.22)

if(APPLE)
    if(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
    endif()
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()

project(GLoveKitRipper VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(KR_BUILD_TESTS "Compila il test da riga di comando del motore di analisi" OFF)
option(KR_BUILD_PLUGIN "Compila il plugin" ON)

# JUCE: usa una copia locale se indicata (-DJUCE_DIR=...), altrimenti la scarica da GitHub
if(DEFINED JUCE_DIR AND EXISTS "${JUCE_DIR}/CMakeLists.txt")
    add_subdirectory("${JUCE_DIR}" juce)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.8
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

if(KR_BUILD_PLUGIN)
    if(APPLE)
        set(KR_FORMATS AU)
    else()
        set(KR_FORMATS VST3 Standalone)
    endif()

    juce_add_plugin(GLoveKitRipper
        COMPANY_NAME "G.LOVE"
        BUNDLE_ID "net.glove.kitripper"
        PLUGIN_MANUFACTURER_CODE Glov
        PLUGIN_CODE Gkrp
        PRODUCT_NAME "G.LOVE Kit Ripper"
        DESCRIPTION "Drum kit + MIDI from any loop"
        IS_SYNTH TRUE
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        AU_MAIN_TYPE kAudioUnitType_MusicDevice
        COPY_PLUGIN_AFTER_BUILD TRUE
        FORMATS ${KR_FORMATS})

    target_sources(GLoveKitRipper PRIVATE
        Source/Analyzer.cpp
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp)

    target_compile_definitions(GLoveKitRipper PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

    target_link_libraries(GLoveKitRipper
        PRIVATE
            juce::juce_audio_utils
            juce::juce_audio_formats
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
endif()

if(KR_BUILD_TESTS)
    juce_add_console_app(AnalyzerTest PRODUCT_NAME "AnalyzerTest")
    target_sources(AnalyzerTest PRIVATE Tests/AnalyzerTest.cpp Source/Analyzer.cpp)
    target_compile_definitions(AnalyzerTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(AnalyzerTest PRIVATE
        juce::juce_audio_formats juce::juce_dsp
        juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endif()

if(KR_BUILD_TESTS AND KR_BUILD_PLUGIN)
    juce_add_console_app(ProcessorTest PRODUCT_NAME "ProcessorTest")
    target_sources(ProcessorTest PRIVATE Tests/ProcessorTest.cpp Source/Analyzer.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
    target_compile_definitions(ProcessorTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JucePlugin_Name="KitRipperTest")
    target_link_libraries(ProcessorTest PRIVATE
        juce::juce_audio_utils juce::juce_audio_formats juce::juce_dsp
        juce::juce_recommended_config_flags)
endif()
