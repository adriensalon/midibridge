cmake_minimum_required(VERSION 3.25.0)
project(dx7midibridge DESCRIPTION "dx7midibridge")

set(BUILD_SHARED_LIBS OFF)
set(RTMIDI_BUILD_TESTING OFF)

# dx7midibridge
add_subdirectory(external/imgui)
add_subdirectory(external/rtmidi)
add_subdirectory(external/vtmidi)
file(GLOB_RECURSE dx7midibridge_source "source/*.cpp")
add_executable(dx7midibridge ${dx7midibridge_source})
target_include_directories(dx7midibridge PRIVATE external/cereal/include)
set_target_properties(dx7midibridge PROPERTIES CXX_STANDARD 17)
target_link_libraries(dx7midibridge PRIVATE imgui rtmidi vtmidi)
vtmidi_copy_dll(dx7midibridge)
