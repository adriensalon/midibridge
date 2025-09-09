cmake_minimum_required(VERSION 3.16)

if (CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(VTMIDI_DLL_NAME "teVirtualMIDI64.dll" CACHE STRING "Name of teVirtualMIDI DLL in this folder")
else()
  set(VTMIDI_DLL_NAME "teVirtualMIDI.dll" CACHE STRING "Name of teVirtualMIDI DLL in this folder")
endif()

set(VTMIDI_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE PATH "Include dir containing teVirtualMIDI.h")
set(VTMIDI_DLL_PATH "${CMAKE_CURRENT_LIST_DIR}/${VTMIDI_DLL_NAME}" CACHE FILEPATH "Absolute path to teVirtualMIDI DLL")

if (NOT EXISTS "${VTMIDI_DLL_PATH}")
    message(WARNING "vtmidi: DLL not found at ${VTMIDI_DLL_PATH}")
endif()

add_library(vtmidi INTERFACE)
target_include_directories(vtmidi INTERFACE "${VTMIDI_INCLUDE_DIR}")

function(vtmidi_copy_dll target_name)
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${VTMIDI_DLL_PATH}" "$<TARGET_FILE_DIR:${target_name}>"
        COMMENT "Copying ${VTMIDI_DLL_NAME} next to $<TARGET_FILE_NAME:${target_name}>"
    )
endfunction()