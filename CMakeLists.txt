cmake_minimum_required(VERSION 3.16)

if(DEFINED ENV{FIRMWARE_VERSION})
    set(PROJECT_VER "$ENV{FIRMWARE_VERSION}")
else()
    set(PROJECT_VER "0.1.0-dev")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(smart_fan)

idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)
