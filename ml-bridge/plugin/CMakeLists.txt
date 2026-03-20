# acestep-vst — AU + VST3 (macOS) / VST3 (Linux, Windows)
cmake_minimum_required(VERSION 3.22)

# Fetch JUCE
include(FetchContent)
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG        8.0.12
  GIT_SHALLOW    TRUE
)
set(JUCE_ENABLE_GPL_MODE ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(JUCE)

# AU is macOS-only; all other platforms get VST3 only
if(APPLE)
  set(PLUGIN_FORMATS AU VST3)
else()
  set(PLUGIN_FORMATS VST3)
endif()

juce_add_plugin(AcestepVST
  VERSION 0.1.0
  COMPANY_NAME "AudioHacking"
  IS_SYNTH FALSE
  NEEDS_MIDI_INPUT FALSE
  NEEDS_MIDI_OUTPUT FALSE
  IS_MIDI_EFFECT FALSE
  EDITOR_WANTS_KEYBOARD_FOCUS FALSE
  PLUGIN_MANUFACTURER_CODE AuHk
  PLUGIN_CODE AcSt
  FORMATS ${PLUGIN_FORMATS}
  PRODUCT_NAME "acestep-vst"
  COPY_PLUGIN_AFTER_BUILD FALSE
)

target_sources(AcestepVST
  PRIVATE
    PluginProcessor.cpp
    PluginEditor.cpp
    PluginPreview.cpp
)

target_compile_definitions(AcestepVST
  PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_MODAL_LOOPS_PERMITTED=1
)

target_link_libraries(AcestepVST
  PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_formats
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)
