# DirBridge 协作规则

本文件是 Codex 和其他开发代理在本仓库工作的主要规则入口。请始终使用中文回答，除非用户明确要求使用其他语言。

## 工作边界

- `E:\Workspace\Project\DirBridge` 是源码仓库和执行工作区。
- `docs/` 只放 DirBridge 项目稳定事实，例如架构、MVP 范围、依赖、运行环境和资源接入。
- `CHANGELOG.md` 记录版本变化，包含用户可见变化、修复、开发者说明和已知问题。
- 通用 Git 流程、发布打包、许可证选择等方法论放知识库，不放本仓库 `docs/`。
- 只有用户明确要求时才修改知识库；不要默认同步知识库内容。

## 分支与版本

- 项目分支只使用 `feature/*`、`fix/*`。
- 单独文档整理也使用 `feature/*`，例如 `feature/reorganize-version-docs`。
- 不使用 `codex/*` 作为项目归档分支；Codex 临时分支如需归档，应先切到符合项目约定的分支。
- `main` 是稳定主线，功能完成并验证后再快进或合并到 `main`。
- 版本归档使用 Git tag，例如 `v0.5.3`。
- 版本记录写入 `CHANGELOG.md`，不要在 `docs/` 或知识库中按版本拆散记录。

## 修改规则

- 修改前先运行 `git status --short`。
- 先阅读相关项目文档和现有实现，再判断修改方式。
- 优先做最小、清晰、可验证的修改，不默认大改结构。
- 保持现有代码风格、命名风格、目录结构和格式化习惯。
- 不删除、重命名或大规模移动文件，除非用户明确要求。
- 不引入新依赖，除非确有必要，并说明原因、替代方案和影响。
- 不硬编码密钥、密码、Token 或隐私数据。
- Windows / PowerShell 环境下读写中文文件时显式使用 UTF-8。

## 实现约束

- 主技术栈保持为 Qt Widgets、C++17、CMake、libcurl、nlohmann/json 和 spdlog。
- Core 层尽量保持纯 C++，不依赖 Qt Widgets。
- UI 层只负责界面和交互适配，不直接包含 libcurl 细节。
- 远程行为通过 Core/Protocol 抽象进入，不在 UI 中绕过后端接口。
- 新增或修改公开接口时使用 Doxygen 风格注释，说明意图、参数、返回值和重要副作用。
- 不引入 SQLite、插件系统、内置编辑器、SSH 终端或安全密码存储，除非先更新 MVP 范围并获得确认。

## 验证规则

修改后优先运行最小有用验证。常用命令：

```powershell
cmake --build .\build\codex-verify
.\build\codex-verify\DirBridgeCoreChecks.exe
.\build\codex-verify\DirBridge.exe --check-deps
.\build\codex-verify\DirBridge.exe --smoke-test
.\build\codex-verify\DirBridge.exe --ui-remote-smoke-test
.\build\codex-verify\DirBridge.exe --ui-remote-workflow-smoke-test
git -c core.quotepath=false diff --check
```

如果某条验证无法运行，说明具体原因，并给出后续可执行命令。

## 当前 MVP 边界

已完成方向：

- 项目命名迁移为 DirBridge。
- Qt Widgets 主界面。
- 本地文件面板。
- JSON 站点配置。
- UI 和文件日志。
- 远程文件系统抽象。
- libcurl 远程目录列表入口。
- 多远程会话 Tab。
- 基础传输队列和远程文件操作。

仍在 MVP 范围：

- 目录传输聚合进度。
- 发布前真实环境验证和 Windows 打包准备。

暂缓：

- 远程编辑。
- 权限管理。
- 文件夹比较和同步。
- SSH 终端。
- 插件系统。
- SQLite。
- 内置编辑器。
- 安全密码存储。
