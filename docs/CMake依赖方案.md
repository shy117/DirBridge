# CMake依赖方案

关联：[总体技术方案](00-总体技术方案.md)、[三方库管理](三方库管理.md)

## 目标

DirBridge 使用 CMake 作为唯一构建系统，不再使用 qmake。

第一阶段依赖：

- Qt Widgets
- Qt Svg
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
│   ├── config
│   ├── core
│   ├── logging
│   ├── protocol
│   └── ui
└── tests
```

## 编译器基线

项目使用 C++17，并依赖标准库 `<filesystem>`。最低编译器要求以“可正常编译并链接 `<filesystem>`”为准：

- MinGW GCC 9 或更新版本。
- MSVC 19.29 或更新版本。
- 64 位 Windows 构建。

GCC 7.3 / 旧版 MinGW 不在支持范围内，因为该环境缺少可用的 `<filesystem>`。CMake 配置阶段应给出明确错误，而不是让用户在源码编译阶段遇到难以判断的头文件错误。

## Qt

CMake 中同时支持 Qt 5 和 Qt 6，不默认限定 Qt 6。

推荐策略：

```cmake
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Svg)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Svg)
```

第一版使用 Qt Widgets，不引入 QML。应用图标和 Fluent SVG 图标通过 Qt 资源系统加载，因此需要显式接入 Qt Svg；Qt Svg 属于 Qt 自身模块，不是额外第三方依赖。

Qt5/Qt6 的局部 API 差异优先在调用点使用条件编译处理。例如拖拽坐标在 Qt 6 使用 `QDropEvent::position()`，Qt 5 使用 `QDropEvent::pos()`。只有兼容判断扩散到多处重复时，再考虑建立统一兼容层。

## 图标与资源接入

当前仓库的图标与资源接入使用两套入口：

- `resources/resources.qrc`：负责 Qt 运行时资源，包含应用图标和已接入的 Fluent SVG 图标。
- `resources/windows/dirbridge.rc`：负责 Windows 可执行文件图标。

当前资源目录中的稳定结构包括：

- `resources/icons/app/`：应用图标、Logo 和多尺寸位图资源。
- `resources/icons/fluent/`：已接入的 Fluent UI System Icons SVG 资源。
- `resources/licenses/`：第三方图标许可证文件。

新增图标资源时，应同步检查：

- `resources.qrc` 是否已加入资源清单。
- Windows 下是否需要更新 `.rc` 入口。
- 发布包和许可证说明是否需要同步补充。

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
- MSVC 构建推荐使用 `curl[ssh]:x64-windows`，让 vcpkg 从源码生成匹配 MSVC ABI 的 `libcurl.lib` 及其依赖。

### 本地预编译包

优点：

- 初期配置快。
- 可直接放入固定目录测试。

缺点：

- DLL、include、lib 路径容易漂移。
- Debug/Release、x86/x64、MSVC/MinGW 混用风险高。
- 不利于长期复现。

当前 `scripts/setup_third_party.ps1` 准备的是 curl.se 的 Windows MinGW 预编译包，并复制 `libcurl.dll.a`。这适合 MinGW 构建，不适合作为 MSVC 的依赖来源。MSVC 构建应使用匹配 MSVC ABI 的 libcurl，推荐通过仓库 `vcpkg.json` 声明的 `curl[ssh]` 从源码生成。

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

MSVC 构建有两类 preset：

- `windows-msvc-debug` / `windows-msvc-release`：使用手工准备的 MSVC ABI libcurl，默认查找 `third_party/installed/curl-msvc`。
- `windows-msvc-vcpkg-debug` / `windows-msvc-vcpkg-release`：使用 `VCPKG_ROOT` 指向的 vcpkg，并通过 `vcpkg.json` 构建 `curl[ssh]`。

MSVC + vcpkg 验证命令示例：

```powershell
$env:VCPKG_ROOT='<vcpkg 根目录>'
cmake --preset windows-msvc-vcpkg-debug -DCMAKE_PREFIX_PATH=<Qt MSVC Kit 路径>
cmake --build --preset windows-msvc-vcpkg-debug
```

运行 MSVC 构建产物前，需要把 vcpkg 和 Qt 的运行库目录加入 `PATH`：

```powershell
$env:PATH="build\windows-msvc-vcpkg-debug\vcpkg_installed\x64-windows\bin;<Qt MSVC Kit 路径>\bin;$env:PATH"
.\build\windows-msvc-vcpkg-debug\DirBridgeCoreChecks.exe
.\build\windows-msvc-vcpkg-debug\DirBridge.exe --check-deps
```

当前 MSVC 验证使用 Visual Studio 2022、Qt 6 MSVC Kit 和 vcpkg `curl[ssh]:x64-windows`。`--check-deps` 已确认 vcpkg 构建的 libcurl 支持 `ftp` 和 `sftp`。

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
