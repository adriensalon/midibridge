cmake_minimum_required(VERSION 3.25.0)
project(midibridge DESCRIPTION "midibridge")

set(BUILD_SHARED_LIBS OFF)
set(RTMIDI_BUILD_TESTING OFF)

# midibridge
add_subdirectory(external/imgui)
add_subdirectory(external/rtmidi)
add_subdirectory(external/vtmidi)
file(GLOB_RECURSE midibridge_source "source/*.cpp")
add_executable(midibridge ${midibridge_source})
set_target_properties(midibridge PROPERTIES CXX_STANDARD 17)
target_link_libraries(midibridge PRIVATE imgui rtmidi vtmidi)
vtmidi_copy_dll(midibridge)
