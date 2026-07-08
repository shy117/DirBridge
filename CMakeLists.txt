cmake_minimum_required(VERSION 3.24)

project(DirBridge
    VERSION 0.5.6
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

include(cmake/ThirdParty.cmake)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Svg)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Svg)

add_library(DirBridgeCore STATIC
    src/config/SiteProfile.cpp
    src/config/SiteProfile.h
    src/config/PasswordCrypto.cpp
    src/config/PasswordCrypto.h
    src/config/SettingsStore.cpp
    src/config/SettingsStore.h
    src/config/SiteStore.cpp
    src/config/SiteStore.h
    src/config/UserSettings.cpp
    src/config/UserSettings.h
    src/core/CurlProtocolCheck.cpp
    src/core/CurlProtocolCheck.h
    src/core/DependencyCheck.cpp
    src/core/DependencyCheck.h
    src/core/FakeRemoteFileSystem.cpp
    src/core/FakeRemoteFileSystem.h
    src/core/FileItem.cpp
    src/core/FileItem.h
    src/core/RemoteFileSystem.h
    src/core/TransferJob.cpp
    src/core/TransferJob.h
    src/core/TransferManager.cpp
    src/core/TransferManager.h
    src/core/TransferQueue.cpp
    src/core/TransferQueue.h
    src/logging/AppLogger.cpp
    src/logging/AppLogger.h
    src/protocol/CurlRemoteFileSystem.cpp
    src/protocol/CurlRemoteFileSystem.h
)

set_target_properties(DirBridgeCore PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_link_libraries(DirBridgeCore
    PUBLIC
        CURL::libcurl
        nlohmann_json::nlohmann_json
        spdlog::spdlog_header_only
)

if(WIN32)
    target_link_libraries(DirBridgeCore
        PUBLIC
            Crypt32
    )
endif()

target_include_directories(DirBridgeCore
    PUBLIC
        src
)

set(DIRBRIDGE_RESOURCE_FILES
    resources/resources.qrc
)

if(WIN32)
    list(APPEND DIRBRIDGE_RESOURCE_FILES
        resources/windows/dirbridge.rc
    )
endif()

add_executable(DirBridge
    src/app/main.cpp
    src/app/UiSmokeTests.cpp
    src/app/UiSmokeTests.h
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/FilePanel.cpp
    src/ui/FilePanel.h
    ${DIRBRIDGE_RESOURCE_FILES}
)

if(WIN32)
    set_target_properties(DirBridge PROPERTIES
        WIN32_EXECUTABLE TRUE
    )
endif()

target_link_libraries(DirBridge
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Widgets
        Qt${QT_VERSION_MAJOR}::Svg
        DirBridgeCore
)

target_compile_definitions(DirBridge
    PRIVATE
        QT_NO_KEYWORDS
)

target_include_directories(DirBridge
    PRIVATE
        src
)

add_executable(DirBridgeCoreChecks
    tests/core/CoreChecks.cpp
)

target_link_libraries(DirBridgeCoreChecks
    PRIVATE
        DirBridgeCore
)

target_include_directories(DirBridgeCoreChecks
    PRIVATE
        src
)

add_executable(DirBridgeRemoteCheck
    tests/integration/RemoteCheck.cpp
)

target_link_libraries(DirBridgeRemoteCheck
    PRIVATE
        DirBridgeCore
)

target_include_directories(DirBridgeRemoteCheck
    PRIVATE
        src
)

if(WIN32)
    get_filename_component(DIRBRIDGE_CXX_RUNTIME_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)

    function(dirbridge_copy_runtime_files target_name)
        if(EXISTS "${DIRBRIDGE_CURL_ROOT}/bin/libcurl-x64.dll")
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${DIRBRIDGE_CURL_ROOT}/bin/libcurl-x64.dll"
                    "$<TARGET_FILE_DIR:${target_name}>/libcurl-x64.dll"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${DIRBRIDGE_CURL_ROOT}/bin/curl-ca-bundle.crt"
                    "$<TARGET_FILE_DIR:${target_name}>/curl-ca-bundle.crt"
                COMMENT "Copying libcurl runtime files for ${target_name}"
            )
        endif()

        foreach(runtime_name IN ITEMS libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)
            if(EXISTS "${DIRBRIDGE_CXX_RUNTIME_DIR}/${runtime_name}")
                add_custom_command(TARGET ${target_name} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${DIRBRIDGE_CXX_RUNTIME_DIR}/${runtime_name}"
                        "$<TARGET_FILE_DIR:${target_name}>/${runtime_name}"
                    COMMENT "Copying MinGW runtime ${runtime_name} for ${target_name}"
                )
            endif()
        endforeach()
    endfunction()

    dirbridge_copy_runtime_files(DirBridge)
    dirbridge_copy_runtime_files(DirBridgeCoreChecks)
    dirbridge_copy_runtime_files(DirBridgeRemoteCheck)
endif()
