cmake_minimum_required(VERSION 3.24)

project(XFolder
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(XFOLDER_CURL_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/curl/curl-8.20.0_5-win64-mingw"
    CACHE PATH
    "Root directory of the local libcurl package"
)

if(EXISTS "${XFOLDER_CURL_ROOT}/include/curl/curl.h")
    set(CURL_INCLUDE_DIR "${XFOLDER_CURL_ROOT}/include" CACHE PATH "libcurl include directory")
endif()

if(WIN32 AND EXISTS "${XFOLDER_CURL_ROOT}/lib/libcurl.dll.a")
    set(CURL_LIBRARY "${XFOLDER_CURL_ROOT}/lib/libcurl.dll.a" CACHE FILEPATH "libcurl import library")
endif()

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)
find_package(CURL REQUIRED)

add_executable(XFolder
    src/app/main.cpp
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/FilePanel.cpp
    src/ui/FilePanel.h
    src/core/CurlProtocolCheck.cpp
    src/core/CurlProtocolCheck.h
)

target_link_libraries(XFolder
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Widgets
        CURL::libcurl
)

target_compile_definitions(XFolder
    PRIVATE
        QT_NO_KEYWORDS
)

target_include_directories(XFolder
    PRIVATE
        src
)

if(WIN32 AND EXISTS "${XFOLDER_CURL_ROOT}/bin/libcurl-x64.dll")
    add_custom_command(TARGET XFolder POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${XFOLDER_CURL_ROOT}/bin/libcurl-x64.dll"
            "$<TARGET_FILE_DIR:XFolder>/libcurl-x64.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${XFOLDER_CURL_ROOT}/bin/curl-ca-bundle.crt"
            "$<TARGET_FILE_DIR:XFolder>/curl-ca-bundle.crt"
        COMMENT "Copying libcurl runtime files"
    )
endif()
