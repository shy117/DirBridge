set(DIRBRIDGE_THIRD_PARTY_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/installed"
    CACHE PATH
    "Root directory of installed third-party dependencies"
)

set(DIRBRIDGE_CURL_ROOT
    "${DIRBRIDGE_THIRD_PARTY_ROOT}/curl"
    CACHE PATH
    "Root directory of the installed libcurl package"
)

set(DIRBRIDGE_JSON_ROOT
    "${DIRBRIDGE_THIRD_PARTY_ROOT}/nlohmann_json"
    CACHE PATH
    "Root directory of the installed nlohmann/json package"
)

set(DIRBRIDGE_SPDLOG_ROOT
    "${DIRBRIDGE_THIRD_PARTY_ROOT}/spdlog"
    CACHE PATH
    "Root directory of the installed spdlog package"
)

if(EXISTS "${DIRBRIDGE_CURL_ROOT}/include/curl/curl.h")
    set(CURL_INCLUDE_DIR "${DIRBRIDGE_CURL_ROOT}/include" CACHE PATH "libcurl include directory" FORCE)
endif()

if(WIN32 AND EXISTS "${DIRBRIDGE_CURL_ROOT}/lib/libcurl.dll.a")
    set(CURL_LIBRARY "${DIRBRIDGE_CURL_ROOT}/lib/libcurl.dll.a" CACHE FILEPATH "libcurl import library" FORCE)
endif()

find_package(CURL REQUIRED)

if(NOT EXISTS "${DIRBRIDGE_JSON_ROOT}/include/nlohmann/json.hpp")
    message(FATAL_ERROR "nlohmann/json was not found at ${DIRBRIDGE_JSON_ROOT}. Run scripts/setup_third_party.ps1.")
endif()

if(NOT TARGET nlohmann_json::nlohmann_json)
    add_library(nlohmann_json INTERFACE)
    add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
    target_include_directories(nlohmann_json INTERFACE "${DIRBRIDGE_JSON_ROOT}/include")
endif()

if(NOT EXISTS "${DIRBRIDGE_SPDLOG_ROOT}/include/spdlog/spdlog.h")
    message(FATAL_ERROR "spdlog was not found at ${DIRBRIDGE_SPDLOG_ROOT}. Run scripts/setup_third_party.ps1.")
endif()

if(NOT TARGET spdlog::spdlog_header_only)
    add_library(spdlog_header_only INTERFACE)
    add_library(spdlog::spdlog_header_only ALIAS spdlog_header_only)
    target_include_directories(spdlog_header_only INTERFACE "${DIRBRIDGE_SPDLOG_ROOT}/include")
endif()
