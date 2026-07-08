# DirBridge Release Notes

本文说明 DirBridge 当前版本的发布产物、生成命令和发布前验证步骤。

## 发布版本

- 版本格式：`vX.Y.Z`
- 平台：Windows x64
- 状态：MVP 阶段预发布包

## 发布资产

本版本发布产物：

- `DirBridge-vX.Y.Z-win64.zip`：绿色压缩包，解压后运行 `DirBridge.exe`。
- `DirBridge-vX.Y.Z-win64-setup.exe`：Inno Setup 安装包。

默认输出目录：

```text
build/release/
```

## 包内内容

发布包应包含：

- `DirBridge.exe`
- Qt 运行库和平台插件
- libcurl 运行库和 CA bundle
- MinGW 运行库
- `README.md`
- `CHANGELOG.md`
- `LICENSE`
- `THIRD_PARTY_NOTICES.md`
- `licenses/` 第三方许可证文件

## 生成命令

```powershell
powershell -ExecutionPolicy Bypass -File scripts\package_release.ps1
```

脚本默认使用 `windows-mingw-release` preset 构建并生成绿色包；如果找到 `ISCC.exe`，会继续生成安装包。如果 `ISCC.exe` 不在 `PATH` 或常见安装目录中，可通过 `-InnoCompiler` 显式指定。

## 发布前验证

在发布目录内运行：

```powershell
Set-Location build\release\DirBridge-vX.Y.Z-win64
.\DirBridge.exe --check-deps
.\DirBridge.exe --smoke-test
.\DirBridge.exe --ui-remote-smoke-test
.\DirBridge.exe --ui-remote-workflow-smoke-test
```

真实 FTP/SFTP 验证需要在本机临时设置 `DIRBRIDGE_TEST_*` 环境变量后运行，不要把密码写入仓库或发布包。

版本变化、修复内容和已知问题见 [CHANGELOG.md](CHANGELOG.md)。
