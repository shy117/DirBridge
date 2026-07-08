# DirBridge

DirBridge 是一个面向 Windows 桌面场景的远程文件管理工具，目标是提供类似 Xftp 的本地与远程双栏文件管理体验。

当前项目仍处于 MVP 开发阶段，当前开发版本为 v0.5.6。

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

## 快速开始

准备第三方依赖：

    powershell -ExecutionPolicy Bypass -File scripts\setup_third_party.ps1

使用 CMake preset 配置和构建。公开仓库中的 `CMakePresets.json` 不包含本机 Qt / MinGW 绝对路径；如果 Qt 或 MinGW 不在默认搜索路径中，请在本地创建不提交的 `CMakeUserPresets.json` 覆盖路径配置。

    cmake --preset windows-mingw-debug
    cmake --build --preset windows-mingw-debug

也可以按本机环境使用普通 CMake 命令构建。项目当前主要验证环境是 Windows + MinGW + Qt 6。

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

## 当前限制

- 站点密码使用 Windows 当前用户 DPAPI 加密后保存，本机当前用户可解密。
- 旧版明文站点配置可继续读取，并会在下次保存时迁移为加密字段。
- v0.5.6 聚焦站点密码加密和状态栏信息用户化。

## 许可证

DirBridge 源码使用 Apache License 2.0，详见 [LICENSE](LICENSE)。

第三方库和图标资源遵循各自许可证，详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。Windows 发布包应随附项目许可证和第三方许可证说明。
