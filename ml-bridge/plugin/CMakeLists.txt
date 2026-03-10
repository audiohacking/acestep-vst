# AceForge Bridge plugin — AU + VST3 (macOS target)
cmake_minimum_required(VERSION 3.22)

if(NOT APPLE)
  message(STATUS "AceForge Bridge plugin: skipping (target is macOS only for now)")
  return()
endif()

# ObjectiveCXX is now enabled at root level

# Fetch JUCE
include(FetchContent)
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG        8.0.12
  GIT_SHALLOW    TRUE
)
# Required for building without a JUCE commercial license (GPL/AGPL)
set(JUCE_ENABLE_GPL_MODE ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(JUCE)

# Plugin target: generator (output-only, no MIDI), not instrument/synth
juce_add_plugin(AceForgeBridge
  VERSION 0.1.0
  COMPANY_NAME "AudioHacking"
  IS_SYNTH FALSE
  NEEDS_MIDI_INPUT FALSE
  NEEDS_MIDI_OUTPUT FALSE
  IS_MIDI_EFFECT FALSE
  EDITOR_WANTS_KEYBOARD_FOCUS FALSE
  PLUGIN_MANUFACTURER_CODE AuHk
  PLUGIN_CODE AfBr
  FORMATS AU VST3
  PRODUCT_NAME "AceForge-Bridge"
  COPY_PLUGIN_AFTER_BUILD FALSE
)

target_sources(AceForgeBridge
  PRIVATE
  PluginProcessor.cpp
  PluginEditor.cpp
)

target_compile_definitions(AceForgeBridge
  PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0
  JUCE_USE_MP3AUDIOFORMAT=1
)

target_link_libraries(AceForgeBridge
  PRIVATE
  juce::juce_audio_utils
  juce::juce_audio_formats
  PUBLIC
  juce::juce_recommended_config_flags
  juce::juce_recommended_lto_flags
  juce::juce_recommended_warning_flags
)
