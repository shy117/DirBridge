# DirBridge

DirBridge 是一个面向 Windows 桌面场景的远程文件管理工具，目标是提供类似 Xftp 的本地与远程双栏文件管理体验。

当前项目仍处于 MVP 开发阶段，版本号格式为 `vX.Y.Z`。具体版本变化见 [CHANGELOG.md](CHANGELOG.md)。

## 当前能力

- 本地文件浏览。
- FTP / SFTP 远程目录浏览。
- 站点配置保存和加载。
- 多远程会话 Tab。
- 远程新建、删除、重命名等基础文件操作。
- 单文件上传和下载。
- 拖拽上传、下载和远程面板内移动。
- 全局传输队列。
- 目录传输聚合进度。
- 应用日志和基础配置。

## 技术栈

- C++17
- Qt Widgets
- CMake
- libcurl
- nlohmann/json
- spdlog

## 构建环境要求

DirBridge 使用 C++17，并依赖标准库 `<filesystem>`。不建议为了兼容旧编译器降低 C++ 标准。更完整的构建兼容性、ABI 差异和依赖准备方式见 [docs/CMake依赖方案.md](docs/CMake依赖方案.md)。

最低要求：

- 64 位 Windows。
- CMake 3.24 或更新版本。
- Qt 5 或 Qt 6，需包含 Widgets 和 Svg 模块。
- 支持 C++17 `<filesystem>` 的编译器和标准库。

当前建议的编译器基线：

- MinGW GCC 9 或更新版本。
- MSVC 19.29 或更新版本。

不支持：

- GCC 7.3 / 旧版 MinGW。该环境缺少可用的 `<filesystem>`，会导致构建失败。

已验证环境：

- Windows + Qt 6 + MinGW：主要开发和发布验证环境。
- Windows + Visual Studio 2022 + Qt 6 MSVC + vcpkg `curl[ssh]`：已验证可编译，Core 检查和依赖检查通过。
- Windows + Visual Studio 2019 + Qt 5 MSVC + MSVC 19.29：已验证项目可编译，Core 检查通过。

计划保持兼容但仍需补齐完整验证：

- Qt 5 + MinGW。

## 快速开始

准备第三方依赖：

    powershell -ExecutionPolicy Bypass -File scripts\setup_third_party.ps1

使用 CMake preset 配置和构建。公开仓库中的 `CMakePresets.json` 不包含本机 Qt / MinGW 绝对路径；如果 Qt 或 MinGW 不在默认搜索路径中，请在本地创建不提交的 `CMakeUserPresets.json` 覆盖路径配置。

    cmake --preset windows-mingw-debug
    cmake --build --preset windows-mingw-debug

项目当前主要验证环境是 Windows + MinGW + Qt 6。

MSVC 构建推荐在 Visual Studio Developer PowerShell 中使用 vcpkg preset：

    cmake --preset windows-msvc-vcpkg-debug
    cmake --build --preset windows-msvc-vcpkg-debug

MSVC 下的 libcurl ABI、vcpkg 依赖准备、运行时 PATH 和 `CMakeUserPresets.json` 细节见 [docs/CMake依赖方案.md](docs/CMake依赖方案.md)。

## 运行检查

构建完成后，建议先运行依赖检查：

    .\build\windows-mingw-debug\DirBridge.exe --check-deps

开发验证时还可以运行：

    .\build\windows-mingw-debug\DirBridge.exe --smoke-test
    .\build\windows-mingw-debug\DirBridge.exe --ui-remote-smoke-test
    .\build\windows-mingw-debug\DirBridge.exe --ui-remote-workflow-smoke-test

如果使用单独的验证构建目录，请以实际构建输出路径为准。

## 文档

项目文档入口：

- [docs/README.md](docs/README.md)

重点文档：

- [总体技术方案](docs/00-总体技术方案.md)
- [第一阶段 MVP](docs/01-第一阶段MVP.md)
- [架构与分层](docs/02-架构与分层.md)
- [CMake 依赖方案](docs/CMake依赖方案.md)
- [三方库管理](docs/三方库管理.md)
- [运行环境说明](docs/运行环境说明.md)
- [版本记录](CHANGELOG.md)

## 参与贡献

欢迎围绕构建兼容性、远程文件操作、传输队列和 UI 体验提交改进。提交前建议至少运行：

    cmake --build .\build\verify
    .\build\verify\DirBridgeCoreChecks.exe
    .\build\verify\DirBridge.exe --check-deps
    .\build\verify\DirBridge.exe --smoke-test

如果修改 UI 或远程会话流程，请同时运行：

    .\build\verify\DirBridge.exe --ui-remote-smoke-test
    .\build\verify\DirBridge.exe --ui-remote-workflow-smoke-test

## 许可证

DirBridge 源码使用 Apache License 2.0，详见 [LICENSE](LICENSE)。

第三方库和图标资源遵循各自许可证，详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。Windows 发布包应随附项目许可证和第三方许可证说明。
