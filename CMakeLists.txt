cmake_minimum_required(VERSION 3.22)

project(GrainReverb2 VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Point this at your local JUCE checkout (JUCE itself is a CMake project;
# add_subdirectory() pulls in its module targets, e.g. juce::juce_core).
# Override on the command line on another machine with:
#   cmake -B build -DJUCE_PATH=/path/to/JUCE
set(JUCE_PATH "/Users/jazer/JUCE" CACHE PATH "Path to local JUCE checkout")
add_subdirectory(${JUCE_PATH} JUCE)

# juce_add_plugin declares one CMake target per requested format (here:
# GrainReverb2_VST3, GrainReverb2_Standalone) plus a shared "GrainReverb2"
# target that holds the actual source/link config both formats build from.
# Standalone is included so you can run/debug the plugin as a normal .app
# without opening a DAW -- useful while we're building the visualizer/editor.
juce_add_plugin(GrainReverb2
    COMPANY_NAME "JazerGiles"
    PLUGIN_MANUFACTURER_CODE Jazr
    PLUGIN_CODE Gvb2
    FORMATS VST3 Standalone
    PRODUCT_NAME "GrainReverb2"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    # Without this, macOS's TCC privacy layer silently refuses microphone
    # access to the Standalone app (no prompt, no error -- input is just
    # zero-filled) because Info.plist lacks NSMicrophoneUsageDescription.
    # This generates that key so the OS actually asks for permission.
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "GrainReverb2 needs microphone input to process live audio through the granular reverb."
    # Copies the built VST3 into ~/Library/Audio/Plug-Ins/VST3 after every
    # build (Debug or Release) -- no more manual cp step before rescanning
    # in Ableton.
    COPY_PLUGIN_AFTER_BUILD TRUE)

# juce_generate_juce_header() would create a monolithic JuceHeader.h (the old
# Projucer style). We skip it and #include the specific juce_* module headers
# each file actually needs -- keeps compile-time dependencies explicit, which
# matters for a tutorial where we want it obvious which module a class needs.

target_sources(GrainReverb2 PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/GrainReverbSharedState.cpp
    Source/GrainVoiceEngine.cpp
    Source/CircularBufferVisualizer.cpp
    Source/BreakpointEditor.cpp
    Source/ParamDialsPanel.cpp)

target_compile_definitions(GrainReverb2 PUBLIC
    JUCE_WEB_BROWSER=0          # we don't use WebBrowserComponent
    JUCE_USE_CURL=0             # no networking
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

# ---- Module list -----------------------------------------------------
# Every juce_* module we link is a deliberate choice; see the explanation
# below the code block.
target_link_libraries(GrainReverb2
    PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
