cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

execute_process(
    COMMAND git describe --tags --match "v[0-9]*" --abbrev=0
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GIT_TAG_RESULT
)
if(GIT_TAG_RESULT EQUAL 0 AND GIT_TAG MATCHES "^v")
    string(SUBSTRING ${GIT_TAG} 1 -1 PROJECT_VER)
else()
    set(PROJECT_VER "0.1.0")
endif()
message(STATUS "Firmware version: ${PROJECT_VER}")

project(esp32_qwen_audio_gw)
