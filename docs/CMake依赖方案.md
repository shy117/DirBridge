# CMake依赖方案

关联：[总体技术方案](00-总体技术方案.md)、[三方库管理](三方库管理.md)、远程传输库技术选型历史归档

## 目标

DirBridge 使用 CMake 作为唯一构建系统，不再使用 qmake。

第一阶段依赖：

- Qt Widgets
- libcurl
- nlohmann/json
- spdlog

后续可能增加：

- Windows Credential Manager
- SQLite
- Qt Concurrent
- 单元测试框架

## 推荐目录

```text
DirBridge
├── CMakeLists.txt
├── CMakePresets.json
├── docs
├── src
│   ├── app
│   ├── ui
│   ├── core
│   ├── backend
│   └── platform
└── tests
```

本次只创建 `docs`，源码骨架在文档确认后再创建。

## Qt

CMake 中优先支持 Qt 6，同时保留 Qt 5 兼容空间。

推荐策略：

```cmake
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)
```

第一版使用 Qt Widgets，不引入 QML。

## 三方依赖

三方库通过 `deps.lock.json` 锁定版本，并使用脚本准备到：

```text
third_party/installed
```

初始化命令：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\setup_third_party.ps1
```

CMake 只引用 `third_party/installed`，不直接引用 `_downloads` 或 `_source`。

当前 CMake 暴露目标：

- `CURL::libcurl`
- `nlohmann_json::nlohmann_json`
- `spdlog::spdlog_header_only`

注意事项：

- `FindCURL` 对协议和特性组件的支持依赖 CMake 版本。
- 如果本机 CMake 版本较旧，可能只能检查 `CURL_FOUND` 和链接目标，协议检测需要运行期或自定义检测。
- Windows 上不同 libcurl 包可能启用不同协议。必须确认当前构建支持 FTP 和 SFTP。
- nlohmann/json 和 spdlog 第一阶段都使用 header-only 方式。

## Windows 依赖来源

推荐两条路径：

### vcpkg

优点：

- CMake 集成顺滑。
- 依赖版本和 ABI 管理相对清晰。
- 适合 Qt/CMake 项目长期维护。

注意：

- 需要确认 libcurl 的 SSH 支持是否启用。
- 团队机器需要统一 vcpkg triplet。

### 本地预编译包

优点：

- 初期配置快。
- 可直接放入固定目录测试。

缺点：

- DLL、include、lib 路径容易漂移。
- Debug/Release、x86/x64、MSVC/MinGW 混用风险高。
- 不利于长期复现。

## 运行目录

Windows Release 包需要包含：

- DirBridge.exe
- Qt 运行库
- Qt platforms 插件
- libcurl DLL
- libcurl 所依赖的 SSL/SSH/zlib 等 DLL

后续应使用 `windeployqt` 或 CPack 生成发布目录。

## 配置阶段检查

CMake 配置阶段应检查：

- Qt Widgets 可用。
- libcurl 可链接。
- libcurl 支持 FTP。
- libcurl 支持 SFTP。
- nlohmann/json 可编译并完成序列化验证。
- spdlog 可写入日志文件。
- 目标平台是 64 位。

如果 FTP 或 SFTP 不可用，配置应失败，并提示用户更换 libcurl 构建。

## 后续建议

先实现最小 CMake 验证项目：

- 能启动一个空 Qt Widgets 主窗口。
- 能打印 libcurl 版本。
- 能检查 libcurl protocol list 中包含 `ftp` 和 `sftp`。
- 能通过 `--check-deps` 检查 libcurl、JSON 和日志。

通过后再进入 FilePanel 和 TransferManager 实现。

## 当前验证方式

仓库保留通用 `CMakePresets.json`，不写入个人机器上的 Qt / MinGW 绝对路径。

本机私有路径应放入不提交的 `CMakeUserPresets.json`，例如：

- Qt Kit 路径。
- MinGW 编译器和 make 路径。
- 当前终端运行时需要追加的 PATH。

通用依赖路径仍由仓库 preset 指向：

- libcurl：`third_party/installed/curl`
- nlohmann/json：`third_party/installed/nlohmann_json`
- spdlog：`third_party/installed/spdlog`
- CMake preset：`windows-mingw-debug`

验证命令：

```powershell
cmake --preset windows-mingw-debug
cmake --build --preset windows-mingw-debug
.\build\windows-mingw-debug\DirBridge.exe --check-curl
.\build\windows-mingw-debug\DirBridge.exe --check-deps
.\build\windows-mingw-debug\DirBridge.exe --smoke-test
```

直接运行 Debug 构建目录下的 exe 时，需要确保 Qt 和 MinGW 运行库在当前终端 `PATH` 中。可通过本机 `CMakeUserPresets.json`、终端环境变量或手动设置完成。

`--check-curl` 用于确认当前 libcurl 支持 `ftp` 和 `sftp`。`--check-deps` 用于确认 JSON 和 spdlog 可用。
