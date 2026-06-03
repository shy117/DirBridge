# CMake依赖方案

关联：[[XFolder]]、[[技术选型-远程传输库]]

## 目标

XFolder 使用 CMake 作为唯一构建系统，不再使用 qmake。

第一阶段依赖：

- Qt Widgets
- libcurl

后续可能增加：

- Windows Credential Manager
- SQLite
- Qt Concurrent
- 单元测试框架

## 推荐目录

```text
XFolder
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

## libcurl

CMake 集成推荐：

```cmake
find_package(CURL REQUIRED COMPONENTS FTP SFTP)
target_link_libraries(XFolder PRIVATE CURL::libcurl)
```

注意事项：

- `FindCURL` 对协议和特性组件的支持依赖 CMake 版本。
- 如果本机 CMake 版本较旧，可能只能检查 `CURL_FOUND` 和链接目标，协议检测需要运行期或自定义检测。
- Windows 上不同 libcurl 包可能启用不同协议。必须确认当前构建支持 FTP 和 SFTP。

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

- XFolder.exe
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
- 目标平台是 64 位。

如果 FTP 或 SFTP 不可用，配置应失败，并提示用户更换 libcurl 构建。

## 后续建议

先实现最小 CMake 验证项目：

- 能启动一个空 Qt Widgets 主窗口。
- 能打印 libcurl 版本。
- 能检查 libcurl protocol list 中包含 `ftp` 和 `sftp`。

通过后再进入 FilePanel 和 TransferManager 实现。
