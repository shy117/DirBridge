# DirBridge Release Notes

本文说明 DirBridge 当前版本的发布资产和验证方式。通用 Windows/Qt 打包方法论不放在本仓库；本文件只记录 DirBridge 项目发布所需事实。

## 当前版本

- 版本：v0.5.3
- 平台：Windows x64
- 状态：MVP 阶段预发布包

## 发布资产

建议同一版本同时提供：

- `DirBridge-v0.5.3-win64.zip`：绿色压缩包，解压后运行 `DirBridge.exe`。
- `DirBridge-v0.5.3-win64-setup.exe`：Inno Setup 安装包；如果本机未安装 Inno Setup 编译器，则本轮只生成绿色压缩包。

二进制产物生成在 `build/release/` 下，不提交到 Git。

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

脚本默认使用 `windows-mingw-release` preset 构建并生成绿色包；如果找到 `ISCC.exe`，会继续生成安装包。

## 发布前验证

在发布目录内运行：

```powershell
Set-Location build\release\DirBridge-v0.5.3-win64
.\DirBridge.exe --check-deps
.\DirBridge.exe --smoke-test
.\DirBridge.exe --ui-remote-smoke-test
.\DirBridge.exe --ui-remote-workflow-smoke-test
```

真实 FTP/SFTP 验证需要在本机临时设置 `DIRBRIDGE_TEST_*` 环境变量后运行，不要把密码写入仓库或发布包。

## v0.5.3 用户可见变化

- 远程首次连接改为后台执行，慢连接时主界面不再被同步阻塞。
- 远程面板会显示连接中状态。
- 连接过程中可以点击取消。
- 连接过程中会拦截重复连接，避免连续创建多个未完成远程会话。

## 已知问题

- 连接取消目前是逻辑取消，不能强制中断已进入阻塞状态的底层 libcurl 调用。
- 远程刷新和路径跳转仍是同步目录加载流程。
- 目录传输仍未提供聚合进度显示。
- MVP 阶段仍明文保存站点密码，仅用于本地开发验证。
