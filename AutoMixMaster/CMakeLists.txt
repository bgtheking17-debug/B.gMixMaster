cmake_minimum_required(VERSION 3.22)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # no VC++ redistributable needed
project(BgPlugins VERSION 1.0.0)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(APPLE)
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()

include(FetchContent)
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 8.0.4
  GIT_SHALLOW ON)
FetchContent_MakeAvailable(JUCE)

function(add_bg_plugin target product code isMaster)
  juce_add_plugin(${target}
    COMPANY_NAME "BG Audio"
    PLUGIN_MANUFACTURER_CODE Bgau
    PLUGIN_CODE ${code}
    FORMATS VST3
    PRODUCT_NAME "${product}"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx Dynamics Mastering)
  target_sources(${target} PRIVATE
    Source/AutoProcessor.cpp Source/AutoProcessor.h
    Source/Engine.h Source/Dsp.h Source/Genres.h)
  target_compile_definitions(${target} PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    AUTO_IS_MASTER=${isMaster})
  target_link_libraries(${target}
    PRIVATE juce::juce_audio_utils
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags)
endfunction()

add_bg_plugin(BgMastering "B.g Mastering" Bgms 1)
add_bg_plugin(BgMix       "B.g Mix"       Bgmx 0)
