# Runs Assistant - root build.
# See specification.md (normative) + plan.md (build order, P0).
# Requires CMake 3.27+. Dependency pins are recorded as git submodules:
#   libs/JUCE                       tag 8.0.15
#   libs/clap-juce-extensions       main (pinned at scaffold time)
# Fresh clone: git submodule update --init --recursive --depth 1
cmake_minimum_required(VERSION 3.27)
project(RunsAssistant VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(RUNS_ENABLE_CLAP "Build the CLAP wrapper (needs libs/clap-juce-extensions)" ON)
option(RUNS_BUILD_TESTS "Build the headless test tools (needs a compiler only, no DAW)" ON)

if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/JUCE/CMakeLists.txt")
    message(FATAL_ERROR
        "JUCE not found at libs/JUCE. Run from the repo root:\n"
        "  git submodule update --init --recursive --depth 1")
endif ()

add_subdirectory(libs/JUCE)

if (RUNS_ENABLE_CLAP)
    if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/clap-juce-extensions/CMakeLists.txt")
        message(FATAL_ERROR
            "clap-juce-extensions not found at libs/clap-juce-extensions. Run:\n"
            "  git submodule update --init --recursive --depth 1")
    endif ()
    add_subdirectory(libs/clap-juce-extensions EXCLUDE_FROM_ALL)
endif ()

# ---------------------------------------------------------------- main target
# VST3 ships as Instrument|Synth with one silent stereo bus (hosts need an
# audio-bus category for MIDI routing; S2). CLAP ships as note-effect.
# AU personalities (aumf/aumi) + Standalone: declared per platform - AU is
# macOS only; the MFX personality builds in P0's CMake but AU validation is
# deferred (plan P0 item 3, D3 post-v1).
set(RUNS_SOURCES
    Source/processor/PluginProcessor.h
    Source/processor/PluginProcessor.cpp
    Source/processor/PlayheadAdapter.h
    Source/processor/PlayheadAdapter.cpp
    Source/editor/PluginEditor.h
    Source/editor/PluginEditor.cpp)

# JUCE-free core engine (plain C++17; see plan.md P1). Enforced JUCE-free by
# the include-check in the headless-tests block below.
set(RUNS_CORE_SOURCES
    Source/core/ScaleModel.h
    Source/core/ScaleModel.cpp
    Source/core/PairTracker.h
    Source/core/PairTracker.cpp
    Source/core/Walk.h
    Source/core/Walk.cpp
    Source/core/CurveMap.h
    Source/core/CurveMap.cpp
    Source/core/VelocityModel.h
    Source/core/VelocityModel.cpp
    Source/core/RunEngine.h
    Source/core/RunEngine.cpp)

juce_add_plugin(RunsAssistant
    COMPANY_NAME "Runs Community"
    BUNDLE_ID dev.runsassistant.native
    PLUGIN_MANUFACTURER_CODE Runs
    PLUGIN_CODE Run1
    PRODUCT_NAME "Runs Assistant"
    VERSION ${PROJECT_VERSION}
    FORMATS Standalone VST3 AU
    DESCRIPTION "MIDI run generator (pair-triggered scale runs)"
    PLUGIN_NAME "Runs Assistant"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    IS_MIDI_EFFECT FALSE
    VST3_CATEGORIES Instrument Synth
    AU_MAIN_TYPE "kAudioUnitType_MusicEffect"
    AU_EXPORT_PREFIX RunsAssistantAU)

target_sources(RunsAssistant PRIVATE ${RUNS_SOURCES})
target_sources(RunsAssistant PRIVATE ${RUNS_CORE_SOURCES})
target_include_directories(RunsAssistant PRIVATE Source)
target_link_libraries(RunsAssistant PRIVATE juce::juce_audio_utils)
target_compile_definitions(RunsAssistant PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

if (RUNS_ENABLE_CLAP)
    clap_juce_extensions_plugin(
        TARGET RunsAssistant
        CLAP_ID "dev.runsassistant.native"
        CLAP_FEATURES note-effect utility)
endif ()

# ------------------------------------------------------- Logic MIDI-FX target
# Second AU personality: aumi MIDI effect for Logic MIDI FX slots (macOS only).
if (APPLE)
    juce_add_plugin(RunsAssistantMFX
        COMPANY_NAME "Runs Community"
        BUNDLE_ID dev.runsassistant.native.mfx
        PLUGIN_MANUFACTURER_CODE Runs
        PLUGIN_CODE Run2
        PRODUCT_NAME "RunsAssistantMFX"
        VERSION ${PROJECT_VERSION}
        FORMATS AU Standalone
        DESCRIPTION "Runs assistant (Logic MIDI FX personality)"
        PLUGIN_NAME "RunsAssistantMFX"
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT TRUE
        IS_MIDI_EFFECT TRUE)

    target_sources(RunsAssistantMFX PRIVATE ${RUNS_SOURCES})
    target_sources(RunsAssistantMFX PRIVATE ${RUNS_CORE_SOURCES})
    target_include_directories(RunsAssistantMFX PRIVATE Source)
    target_link_libraries(RunsAssistantMFX PRIVATE juce::juce_audio_utils)
    target_compile_definitions(RunsAssistantMFX PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)

    if (RUNS_ENABLE_CLAP)
        clap_juce_extensions_plugin(
            TARGET RunsAssistantMFX
            CLAP_ID "dev.runsassistant.native.mfx"
            CLAP_FEATURES note-effect utility)
    endif ()
endif ()

# ---------------------------------------------------------------- headless tests
# Console tools only: no audio hardware, no GUI, no DAW. Runs under CTest.
if (RUNS_BUILD_TESTS)
    add_executable(P0StateRoundTrip tests/P0StateRoundTrip.cpp
        ${RUNS_SOURCES}
        ${RUNS_CORE_SOURCES})
    target_include_directories(P0StateRoundTrip PRIVATE Source)
    target_link_libraries(P0StateRoundTrip PRIVATE juce::juce_audio_utils)
    target_compile_definitions(P0StateRoundTrip PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)

    include(CTest)
    add_test(NAME P0StateRoundTrip COMMAND P0StateRoundTrip)

    # ------------------------------------------------ ProcessorTests (P2)
    # Full-processBlock conformance: pair capture matrix, epsilon alignment,
    # cut matrix, CC mapping/absorption, binary chunk round-trip.
    add_executable(ProcessorTests
        tests/ProcessorTests.cpp
        ${RUNS_SOURCES}
        ${RUNS_CORE_SOURCES})
    target_include_directories(ProcessorTests PRIVATE Source tests)
    target_link_libraries(ProcessorTests PRIVATE juce::juce_audio_utils)
    target_compile_definitions(ProcessorTests PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)
    add_test(NAME ProcessorTests COMMAND ProcessorTests)

    # --------------------------------------------------- CoreTests (P1, JUCE-free)
    add_executable(CoreTests
        tests/TestMain.cpp
        tests/CoreTests.cpp
        tests/EngineTests.cpp
        tests/SweepTests.cpp
        ${RUNS_CORE_SOURCES})
    target_include_directories(CoreTests PRIVATE tests Source)
    add_test(NAME CoreTests COMMAND CoreTests)

    # Source/core must stay JUCE-free (plain C++17, testable standalone;
    # failed configure otherwise, same house rule as Eloquent).
    file(GLOB_RECURSE RUNS_CORE_FILES CONFIGURE_DEPENDS
         "${CMAKE_CURRENT_SOURCE_DIR}/Source/core/*")
    set(RUNS_CORE_CHECKED OFF)
    foreach (corefile IN LISTS RUNS_CORE_FILES)
        file(READ "${corefile}" corecontent)
        if (corecontent MATCHES "#[ \t]*include[ \t]*[<\"]juce")
            message(FATAL_ERROR "Source/core must not include JUCE: ${corefile}")
        endif ()
        set(RUNS_CORE_CHECKED ON)
    endforeach ()
    if (NOT RUNS_CORE_CHECKED)
        message(FATAL_ERROR "Source/core has no files; include-check skipped")
    endif ()
endif ()
