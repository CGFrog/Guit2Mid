cmake_minimum_required(VERSION 3.22)

project(GuitarToMidi VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- JUCE acquisition --------------------------------------------------------
# Two ways to get JUCE:
#   1) Point JUCE_LOCAL_PATH at an existing JUCE checkout (fastest, works offline).
#   2) Otherwise, JUCE is fetched from GitHub at configure time.
# Adjust JUCE_GIT_TAG to whatever current stable release you want to build against.
set(JUCE_LOCAL_PATH "" CACHE PATH "Path to an existing local JUCE checkout (optional)")
set(JUCE_GIT_TAG "8.0.15" CACHE STRING "JUCE git tag to fetch if JUCE_LOCAL_PATH is not set")

if (JUCE_LOCAL_PATH)
    add_subdirectory(${JUCE_LOCAL_PATH} JUCE)
else()
    include(FetchContent)
    # A plain release-tarball download is dramatically faster to fetch than a
    # git clone of the whole JUCE history (even shallow), and needs no git
    # binary at configure time.
    FetchContent_Declare(
        JUCE
        URL https://github.com/juce-framework/JUCE/archive/refs/tags/${JUCE_GIT_TAG}.tar.gz
    )
    FetchContent_MakeAvailable(JUCE)
endif()

# --- Plugin target ------------------------------------------------------------
juce_add_plugin(GuitarToMidi
    COMPANY_NAME            "YourCompany"
    PLUGIN_MANUFACTURER_CODE Ycmp
    PLUGIN_CODE             Gtmi
    FORMATS                 VST3 Standalone AU
    PRODUCT_NAME            "Guitar to MIDI"
    IS_SYNTH                FALSE
    NEEDS_MIDI_INPUT        FALSE
    NEEDS_MIDI_OUTPUT       TRUE
    IS_MIDI_EFFECT          FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES         "Fx" "Analyzer"
    AU_MAIN_TYPE            "kAudioUnitType_Effect"
)

juce_generate_juce_header(GuitarToMidi)

target_sources(GuitarToMidi PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/PluginEditor.cpp
    Source/PluginEditor.h
    Source/PluginParameters.h
    Source/dsp/MusicMath.h
    Source/dsp/YinPitchDetector.h
    Source/dsp/PolyPitchDetector.h
    Source/dsp/OnsetDetector.h
    Source/dsp/MidiNoteOutput.h
    Source/dsp/GuitarMidiEngine.h
    Source/dsp/GuitarMidiEngine.cpp
)

target_compile_definitions(GuitarToMidi PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
)

target_link_libraries(GuitarToMidi PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_dsp
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

# --- Offline evaluation / conversion tool --------------------------------------
# Synthesises guitar test material with known ground truth, runs it through the
# same engine the plugin uses, and scores the MIDI output. Can also convert a
# recorded DI .wav into a .mid file. Not needed to build the plugin.
option(GTM_BUILD_EVAL "Build the GuitarToMidiEval command-line tool" ON)
if (GTM_BUILD_EVAL)
    juce_add_console_app(GuitarToMidiEval PRODUCT_NAME "GuitarToMidiEval")
    target_sources(GuitarToMidiEval PRIVATE
        Tools/Eval/EvalMain.cpp
        Tools/Eval/GuitarSynth.h
        Source/dsp/GuitarMidiEngine.cpp
    )
    target_compile_definitions(GuitarToMidiEval PRIVATE
        GTM_ENGINE_TRACE=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
    )
    target_link_libraries(GuitarToMidiEval PRIVATE
        juce::juce_audio_formats
        juce::juce_dsp
        PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
    )
endif()
