cmake_minimum_required(VERSION 3.16)

# Development builds use the milestone version below. Tagged release CI sets
# GATEWAY_PROJECT_VER from the tag and verifies the embedded esp_app_desc_t.
if(DEFINED ENV{GATEWAY_PROJECT_VER} AND NOT "$ENV{GATEWAY_PROJECT_VER}" STREQUAL "")
    set(PROJECT_VER "$ENV{GATEWAY_PROJECT_VER}")
else()
    set(PROJECT_VER "0.7.0-dev")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_sms_gateway)
