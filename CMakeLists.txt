cmake_minimum_required(VERSION 3.16)
set(IDF_TARGET "esp32s3" CACHE STRING "ESP32-S3 target")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-socks5-gateway)
# v6.1 lwIP Kconfig explicitly requires a larger FD_SETSIZE above 61 sockets.
# Apply to every source component (lwIP, VFS and app) to keep fd_set ABI identical.
idf_build_set_property(COMPILE_DEFINITIONS "FD_SETSIZE=256" APPEND)

if(CONFIG_PROXY_POC_ENABLE)
    set(proxy_ip4 "${CMAKE_CURRENT_SOURCE_DIR}/components/lwip/lwip/src/core/ipv4/ip4.c")
    if(NOT EXISTS "${proxy_ip4}")
        message(FATAL_ERROR "Phase 2 requires project-local lwIP: python tools/prepare_lwip.py apply")
    endif()
    file(READ "${proxy_ip4}" proxy_ip4_source)
    string(FIND "${proxy_ip4_source}" "Project-owned IPv4 transparent TCP hooks" proxy_hook_offset)
    if(proxy_hook_offset EQUAL -1)
        message(FATAL_ERROR "Local lwIP patch restored/missing: python tools/prepare_lwip.py apply")
    endif()
endif()
