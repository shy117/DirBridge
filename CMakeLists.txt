cmake_minimum_required(VERSION 3.24)

project(DirBridge
    VERSION 0.7.5
    LANGUAGES CXX
)

include(CTest)

option(
    DIRBRIDGE_BUILD_SSH_TERMINAL_PROTOTYPE
    "Build the Windows ConPTY lifecycle validation prototype."
    OFF
)

set(DIRBRIDGE_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
    src/app/AppVersion.h.in
    "${DIRBRIDGE_GENERATED_DIR}/AppVersion.h"
    @ONLY
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_SIZEOF_VOID_P AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "DirBridge currently supports 64-bit Windows builds only.")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    message(FATAL_ERROR "DirBridge requires C++17 std::filesystem support. GCC 7.x/8.x MinGW is not supported; use MinGW GCC 9 or newer.")
endif()

if(MSVC AND MSVC_VERSION LESS 1929)
    message(FATAL_ERROR "DirBridge requires MSVC 19.29 or newer for the supported C++17 build baseline.")
endif()

include(CheckCXXSourceCompiles)
check_cxx_source_compiles("
    #include <filesystem>
    int main()
    {
        const auto path = std::filesystem::current_path();
        return path.empty() ? 1 : 0;
    }
" DIRBRIDGE_HAS_STD_FILESYSTEM)

if(NOT DIRBRIDGE_HAS_STD_FILESYSTEM)
    message(FATAL_ERROR "DirBridge requires a compiler and standard library with working C++17 <filesystem> support.")
endif()

if(MSVC)
    add_compile_options(/utf-8)
endif()

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
    src/core/ExternalEditDocument.cpp
    src/core/ExternalEditDocument.h
    src/core/FileCache.cpp
    src/core/FileCache.h
    src/core/FileItem.cpp
    src/core/FileItem.h
    src/core/FileReplacement.cpp
    src/core/FileReplacement.h
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

if(WIN32)
    add_library(DirBridgeTerminal STATIC
        src/terminal/AskPassProtocol.h
        src/terminal/ConPtyProcess.cpp
        src/terminal/ConPtyProcess.h
        src/terminal/OpenSshLauncher.cpp
        src/terminal/OpenSshLauncher.h
        src/terminal/OpenSshLaunchSpec.h
        src/terminal/SshLaunchRequest.h
        src/terminal/StoredPasswordChannel.cpp
        src/terminal/StoredPasswordChannel.h
        src/terminal/StoredPasswordLease.cpp
        src/terminal/StoredPasswordLease.h
        src/terminal/TerminalBrokerProtocol.cpp
        src/terminal/TerminalBrokerProtocol.h
        src/terminal/TerminalBrokerClient.cpp
        src/terminal/TerminalBrokerClient.h
        src/terminal/TerminalBrokerStart.cpp
        src/terminal/TerminalBrokerStart.h
    )

    add_executable(DirBridgeSshAskPass
        src/terminal/AskPassProtocol.h
        src/terminal/SshAskPassMain.cpp
        src/terminal/StoredPasswordLease.h
    )

    add_executable(DirBridgeTerminalBroker
        src/terminal/TerminalBrokerMain.cpp
    )

    target_link_libraries(DirBridgeTerminalBroker
        PRIVATE
            DirBridgeTerminal
            Shell32
    )

    target_include_directories(DirBridgeTerminal
        PUBLIC
            src
    )

    target_link_libraries(DirBridgeTerminal
        PRIVATE
            Advapi32
            Bcrypt
    )

    set_target_properties(
        DirBridgeTerminal
        DirBridgeSshAskPass
        DirBridgeTerminalBroker
        PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )

    target_include_directories(DirBridgeSshAskPass
        PRIVATE
            src
    )

    add_library(DirBridgeTerminalQt STATIC
        src/terminal/GhosttyTerminalEngine.cpp
        src/terminal/GhosttyTerminalEngine.h
        src/terminal/ITerminalEngine.h
        src/terminal/SshTerminalManager.cpp
        src/terminal/SshTerminalManager.h
        src/terminal/TerminalTypes.h
    )

    target_link_libraries(DirBridgeTerminalQt
        PUBLIC
            Qt${QT_VERSION_MAJOR}::Core
            DirBridgeCore
        PRIVATE
            DirBridgeTerminal
    )

    target_include_directories(DirBridgeTerminalQt
        PUBLIC
            src
        PRIVATE
            "${DIRBRIDGE_GHOSTTY_VT_ROOT}/include"
    )

    target_compile_definitions(DirBridgeTerminalQt
        PRIVATE
            QT_NO_KEYWORDS
    )
endif()

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
    src/ui/TerminalWidget.cpp
    src/ui/TerminalWidget.h
    src/ui/FileChangeMonitor.cpp
    src/ui/FileChangeMonitor.h
    src/ui/ExternalEditManager.cpp
    src/ui/ExternalEditManager.h
    src/ui/TransferConflictDialog.cpp
    src/ui/TransferConflictDialog.h
    src/ui/window_layout.cpp
    src/ui/window_remote.cpp
    src/ui/window_session.cpp
    src/ui/window_terminal.cpp
    src/ui/window_transfer.cpp
    src/ui/FilePanel.cpp
    src/ui/FilePanel.h
    src/ui/panel_menu.cpp
    src/ui/panel_navigation.cpp
    src/ui/panel_shared.cpp
    src/ui/panel_shared.h
    src/ui/panel_transfer.cpp
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

if(WIN32)
    target_sources(DirBridge PRIVATE
        src/platform/windows/WindowsShellDataObject.cpp
        src/platform/windows/WindowsShellDataObject.h
    )
    target_link_libraries(DirBridge PRIVATE DirBridgeTerminal DirBridgeTerminalQt)
    target_link_libraries(DirBridge PRIVATE ole32 shell32 shlwapi)
    add_dependencies(
        DirBridge
        DirBridgeSshAskPass
        DirBridgeTerminalBroker
    )
endif()

target_compile_definitions(DirBridge
    PRIVATE
        QT_NO_KEYWORDS
)

target_include_directories(DirBridge
    PRIVATE
        src
        "${DIRBRIDGE_GENERATED_DIR}"
)

add_executable(DirBridgeCoreChecks
    tests/core/CoreChecks.cpp
)

if(BUILD_TESTING)
    add_executable(DirBridgeTransferConflictDialogPreview
        src/app/TransferConflictDialogPreview.cpp
        src/ui/TransferConflictDialog.cpp
        src/ui/TransferConflictDialog.h
        src/ui/panel_shared.cpp
        src/ui/panel_shared.h
        ${DIRBRIDGE_RESOURCE_FILES}
    )

    target_link_libraries(DirBridgeTransferConflictDialogPreview
        PRIVATE
            Qt${QT_VERSION_MAJOR}::Widgets
            Qt${QT_VERSION_MAJOR}::Svg
            DirBridgeCore
    )

    target_compile_definitions(DirBridgeTransferConflictDialogPreview
        PRIVATE
            QT_NO_KEYWORDS
    )

    target_include_directories(DirBridgeTransferConflictDialogPreview
        PRIVATE
            src
    )

    add_test(
        NAME DirBridgeTransferConflictDialogCheck
        COMMAND DirBridgeTransferConflictDialogPreview --check
    )
endif()

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

if(WIN32)
    add_executable(DirBridgeSshTerminalVerticalCheck
        tests/integration/SshTerminalVerticalCheck.cpp
    )

    add_executable(DirBridgeSshTerminalEngineVerticalCheck
        tests/integration/SshTerminalEngineVerticalCheck.cpp
    )

    target_link_libraries(DirBridgeSshTerminalVerticalCheck
        PRIVATE
            DirBridgeCore
            DirBridgeTerminal
            Shell32
    )

    target_link_libraries(DirBridgeSshTerminalEngineVerticalCheck
        PRIVATE
            DirBridgeCore
            DirBridgeTerminalQt
            Qt${QT_VERSION_MAJOR}::Core
    )

    target_include_directories(DirBridgeSshTerminalVerticalCheck
        PRIVATE
            src
    )

    target_include_directories(DirBridgeSshTerminalEngineVerticalCheck
        PRIVATE
            src
    )

    add_dependencies(
        DirBridgeSshTerminalVerticalCheck
        DirBridgeSshAskPass
        DirBridgeTerminalBroker
    )

    add_dependencies(
        DirBridgeSshTerminalEngineVerticalCheck
        DirBridgeSshAskPass
        DirBridgeTerminalBroker
    )

    set_target_properties(
        DirBridgeSshTerminalVerticalCheck
        DirBridgeSshTerminalEngineVerticalCheck
        PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
endif()

if(DIRBRIDGE_BUILD_SSH_TERMINAL_PROTOTYPE)
    if(NOT WIN32)
        message(FATAL_ERROR "DIRBRIDGE_BUILD_SSH_TERMINAL_PROTOTYPE is only supported on Windows.")
    endif()

    add_executable(DirBridgeConPtyTestChild
        tests/terminal/ConPtyTestChild.cpp
        tests/terminal/ConPtyTestProtocol.h
    )

    add_executable(DirBridgeConPtyChecks
        tests/terminal/ConPtyChecks.cpp
        tests/terminal/ConPtyBrokerProtocol.h
        tests/terminal/ConPtyTestProtocol.h
    )

    add_executable(DirBridgeConPtyBrokerPrototype
        tests/terminal/ConPtyChecks.cpp
        tests/terminal/ConPtyBrokerProtocol.h
        tests/terminal/ConPtyTestProtocol.h
    )

    target_compile_definitions(DirBridgeConPtyBrokerPrototype
        PRIVATE
            DIRBRIDGE_CONPTY_BROKER_EXECUTABLE=1
    )

    set_target_properties(
        DirBridgeConPtyTestChild
        DirBridgeConPtyChecks
        DirBridgeConPtyBrokerPrototype
        PROPERTIES
            AUTOMOC OFF
            AUTOUIC OFF
            AUTORCC OFF
    )

    target_link_libraries(DirBridgeConPtyChecks
        PRIVATE
            Bcrypt
            Shell32
    )

    target_link_libraries(DirBridgeConPtyBrokerPrototype
        PRIVATE
            Bcrypt
            Shell32
    )

    target_link_libraries(DirBridgeConPtyTestChild
        PRIVATE
            Shell32
    )

    if(BUILD_TESTING)
        add_test(
            NAME DirBridgeConPtySmoke
            COMMAND DirBridgeConPtyChecks
                --child "$<TARGET_FILE:DirBridgeConPtyTestChild>"
                --suite smoke
        )
        set_tests_properties(DirBridgeConPtySmoke PROPERTIES TIMEOUT 60)

        add_test(
            NAME DirBridgeConPtyBrokerIpcSmoke
            COMMAND DirBridgeConPtyChecks
                --child "$<TARGET_FILE:DirBridgeConPtyTestChild>"
                --broker "$<TARGET_FILE:DirBridgeConPtyBrokerPrototype>"
                --suite broker-ipc-diagnostic
        )
        set_tests_properties(
            DirBridgeConPtyBrokerIpcSmoke
            PROPERTIES TIMEOUT 120
        )
    endif()
endif()

if(WIN32 AND BUILD_TESTING)
    add_executable(DirBridgeConPtyEchoChild
        tests/terminal/ConPtyEchoChild.cpp
    )

    add_executable(DirBridgeConPtyProcessChecks
        tests/terminal/ConPtyProcessChecks.cpp
    )

    add_executable(DirBridgeTerminalBrokerProtocolChecks
        tests/terminal/TerminalBrokerProtocolChecks.cpp
    )

    add_executable(DirBridgeTerminalBrokerClientChecks
        tests/terminal/TerminalBrokerClientChecks.cpp
    )

    add_executable(DirBridgeSshTerminalManagerChecks
        tests/terminal/SshTerminalManagerChecks.cpp
    )

    add_executable(DirBridgeGhosttyTerminalEngineChecks
        tests/terminal/GhosttyTerminalEngineChecks.cpp
    )

    add_executable(DirBridgeGhosttyTerminalEngineStressChecks
        tests/terminal/GhosttyTerminalEngineStressChecks.cpp
    )

    add_executable(DirBridgeTerminalWidgetChecks
        tests/ui/TerminalWidgetChecks.cpp
        src/ui/TerminalWidget.cpp
        src/ui/TerminalWidget.h
    )

    add_executable(DirBridgeAskPassChecks
        tests/terminal/AskPassChecks.cpp
    )

    add_executable(DirBridgeOpenSshLauncherChecks
        tests/terminal/OpenSshLauncherChecks.cpp
    )

    target_link_libraries(DirBridgeAskPassChecks
        PRIVATE
            DirBridgeTerminal
            Shell32
    )

    target_link_libraries(DirBridgeConPtyProcessChecks
        PRIVATE
            DirBridgeTerminal
            Shell32
    )

    add_dependencies(DirBridgeConPtyProcessChecks DirBridgeConPtyEchoChild)

    target_link_libraries(DirBridgeTerminalBrokerProtocolChecks
        PRIVATE
            DirBridgeTerminal
    )

    target_link_libraries(DirBridgeTerminalBrokerClientChecks
        PRIVATE
            DirBridgeTerminal
            Shell32
    )

    target_link_libraries(DirBridgeSshTerminalManagerChecks
        PRIVATE
            DirBridgeTerminalQt
            Qt${QT_VERSION_MAJOR}::Core
    )

    target_link_libraries(DirBridgeGhosttyTerminalEngineChecks
        PRIVATE
            DirBridgeTerminalQt
            Qt${QT_VERSION_MAJOR}::Core
    )

    target_link_libraries(DirBridgeGhosttyTerminalEngineStressChecks
        PRIVATE
            DirBridgeTerminalQt
            Qt${QT_VERSION_MAJOR}::Core
    )

    target_link_libraries(DirBridgeTerminalWidgetChecks
        PRIVATE
            Qt${QT_VERSION_MAJOR}::Widgets
    )

    target_include_directories(DirBridgeTerminalWidgetChecks
        PRIVATE
            src
    )

    target_compile_definitions(DirBridgeTerminalWidgetChecks
        PRIVATE
            QT_NO_KEYWORDS
    )

    add_dependencies(
        DirBridgeSshTerminalManagerChecks
        DirBridgeTerminalBrokerClientChecks
    )

    add_dependencies(DirBridgeAskPassChecks DirBridgeSshAskPass)

    target_link_libraries(DirBridgeOpenSshLauncherChecks
        PRIVATE
            DirBridgeTerminal
            Shell32
    )

    set_target_properties(
        DirBridgeAskPassChecks
        DirBridgeConPtyEchoChild
        DirBridgeConPtyProcessChecks
        DirBridgeOpenSshLauncherChecks
        DirBridgeTerminalBrokerProtocolChecks
        DirBridgeTerminalBrokerClientChecks
        DirBridgeSshTerminalManagerChecks
        DirBridgeGhosttyTerminalEngineChecks
        DirBridgeGhosttyTerminalEngineStressChecks
        PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )

    set_target_properties(DirBridgeTerminalWidgetChecks PROPERTIES
        AUTOUIC OFF
        AUTORCC OFF
    )

    add_test(
        NAME DirBridgeAskPassChecks
        COMMAND DirBridgeAskPassChecks
            "$<TARGET_FILE:DirBridgeSshAskPass>"
    )
    set_tests_properties(DirBridgeAskPassChecks PROPERTIES TIMEOUT 20)

    add_test(
        NAME DirBridgeConPtyProcessChecks
        COMMAND DirBridgeConPtyProcessChecks
            "$<TARGET_FILE:DirBridgeConPtyEchoChild>"
    )
    set_tests_properties(DirBridgeConPtyProcessChecks PROPERTIES TIMEOUT 20)

    add_test(
        NAME DirBridgeTerminalBrokerProtocolChecks
        COMMAND DirBridgeTerminalBrokerProtocolChecks
    )
    set_tests_properties(
        DirBridgeTerminalBrokerProtocolChecks
        PROPERTIES TIMEOUT 15
    )

    add_test(
        NAME DirBridgeTerminalBrokerClientChecks
        COMMAND DirBridgeTerminalBrokerClientChecks
    )
    set_tests_properties(
        DirBridgeTerminalBrokerClientChecks
        PROPERTIES TIMEOUT 15
    )

    add_test(
        NAME DirBridgeSshTerminalManagerChecks
        COMMAND DirBridgeSshTerminalManagerChecks
            "$<TARGET_FILE:DirBridgeTerminalBrokerClientChecks>"
            "${DIRBRIDGE_GHOSTTY_VT_ROOT}/bin/ghostty-vt.dll"
    )
    set_tests_properties(
        DirBridgeSshTerminalManagerChecks
        PROPERTIES TIMEOUT 15
    )

    add_test(
        NAME DirBridgeGhosttyTerminalEngineChecks
        COMMAND DirBridgeGhosttyTerminalEngineChecks
            "${DIRBRIDGE_GHOSTTY_VT_ROOT}/bin/ghostty-vt.dll"
    )
    set_tests_properties(
        DirBridgeGhosttyTerminalEngineChecks
        PROPERTIES TIMEOUT 15
    )

    add_test(
        NAME DirBridgeGhosttyTerminalEngineStressChecks
        COMMAND DirBridgeGhosttyTerminalEngineStressChecks
            "${DIRBRIDGE_GHOSTTY_VT_ROOT}/bin/ghostty-vt.dll"
    )
    set_tests_properties(
        DirBridgeGhosttyTerminalEngineStressChecks
        PROPERTIES TIMEOUT 60
    )

    add_test(
        NAME DirBridgeTerminalWidgetChecks
        COMMAND DirBridgeTerminalWidgetChecks
    )
    set_tests_properties(
        DirBridgeTerminalWidgetChecks
        PROPERTIES
            TIMEOUT 15
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_test(
        NAME DirBridgeOpenSshLauncherChecks
        COMMAND DirBridgeOpenSshLauncherChecks
    )
    set_tests_properties(
        DirBridgeOpenSshLauncherChecks
        PROPERTIES TIMEOUT 15
    )
endif()

if(BUILD_TESTING)
    add_test(NAME DirBridgeCoreChecks COMMAND DirBridgeCoreChecks)
endif()

if(WIN32 AND BUILD_TESTING)
    set_tests_properties(
        DirBridgeSshTerminalManagerChecks
        DirBridgeGhosttyTerminalEngineChecks
        DirBridgeGhosttyTerminalEngineStressChecks
        DirBridgeTerminalWidgetChecks
        PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt${QT_VERSION_MAJOR}::Core>"
    )
endif()

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
        if(EXISTS "${DIRBRIDGE_GHOSTTY_VT_ROOT}/bin/ghostty-vt.dll")
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${DIRBRIDGE_GHOSTTY_VT_ROOT}/bin/ghostty-vt.dll"
                    "$<TARGET_FILE_DIR:${target_name}>/ghostty-vt.dll"
                COMMENT "Copying ghostty-vt runtime for ${target_name}"
            )
        endif()

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
