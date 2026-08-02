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

set(DIRBRIDGE_GHOSTTY_VT_ROOT
    "${DIRBRIDGE_THIRD_PARTY_ROOT}/ghostty-vt"
    CACHE PATH
    "Root directory of the project-local ghostty-vt runtime package"
)

set(DIRBRIDGE_LOCAL_CURL_LIBRARY "")

if(EXISTS "${DIRBRIDGE_CURL_ROOT}/include/curl/curl.h")
    if(MSVC)
        foreach(candidate IN ITEMS
            "${DIRBRIDGE_CURL_ROOT}/lib/libcurl.lib"
            "${DIRBRIDGE_CURL_ROOT}/lib/libcurl_imp.lib"
            "${DIRBRIDGE_CURL_ROOT}/lib/release/libcurl.lib"
            "${DIRBRIDGE_CURL_ROOT}/lib/debug/libcurl_debug.lib"
        )
            if(EXISTS "${candidate}")
                set(DIRBRIDGE_LOCAL_CURL_LIBRARY "${candidate}")
                break()
            endif()
        endforeach()
    elseif(WIN32 AND EXISTS "${DIRBRIDGE_CURL_ROOT}/lib/libcurl.dll.a")
        set(DIRBRIDGE_LOCAL_CURL_LIBRARY "${DIRBRIDGE_CURL_ROOT}/lib/libcurl.dll.a")
    endif()
endif()

if(DIRBRIDGE_LOCAL_CURL_LIBRARY)
    set(CURL_INCLUDE_DIR "${DIRBRIDGE_CURL_ROOT}/include" CACHE PATH "libcurl include directory" FORCE)
    set(CURL_LIBRARY "${DIRBRIDGE_LOCAL_CURL_LIBRARY}" CACHE FILEPATH "libcurl import library" FORCE)
elseif(MSVC AND EXISTS "${DIRBRIDGE_CURL_ROOT}/include/curl/curl.h")
    message(STATUS "DIRBRIDGE_CURL_ROOT does not contain an MSVC-compatible libcurl .lib: ${DIRBRIDGE_CURL_ROOT}")
    message(STATUS "Provide an MSVC-built libcurl root, or configure with vcpkg and curl[ssh] for x64-windows.")
endif()

find_package(CURL QUIET)

if(NOT CURL_FOUND)
    if(MSVC)
        message(FATAL_ERROR "libcurl was not found for MSVC. Build libcurl from source with an MSVC ABI, for example via vcpkg curl[ssh]:x64-windows, or set DIRBRIDGE_CURL_ROOT to a package containing include/curl/curl.h and lib/libcurl.lib.")
    endif()

    message(FATAL_ERROR "libcurl was not found. Run scripts/setup_third_party.ps1 or set DIRBRIDGE_CURL_ROOT.")
endif()

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

if(WIN32)
    if(NOT EXISTS "${DIRBRIDGE_GHOSTTY_VT_ROOT}/include/ghostty/vt.h")
        message(FATAL_ERROR "ghostty-vt headers were not found at ${DIRBRIDGE_GHOSTTY_VT_ROOT}. Run scripts/setup_ghostty_vt_local.ps1.")
    endif()
    if(NOT EXISTS "${DIRBRIDGE_GHOSTTY_VT_ROOT}/bin/ghostty-vt.dll")
        message(FATAL_ERROR "ghostty-vt.dll was not found at ${DIRBRIDGE_GHOSTTY_VT_ROOT}. Run scripts/setup_ghostty_vt_local.ps1.")
    endif()
endif()
