# DirBridge Agent Guide

This file defines how Codex and other coding agents should work in this repository.

## Project Areas

### Project Truth: `docs/`

`E:\Workspace\Project\DirBridge\docs` is the authoritative project documentation area.

Treat these files as the source of truth for product direction, architecture, build rules, dependency policy, and MVP scope:

- `docs/README.md`
- `docs/00-总体技术方案.md`
- `docs/01-第一阶段MVP.md`
- `docs/02-架构与分层.md`
- `docs/CMake依赖方案.md`
- `docs/三方库管理.md`
- `docs/运行环境说明.md`

Before implementing a new feature, read these three core documents first:

- `docs/00-总体技术方案.md`
- `docs/01-第一阶段MVP.md`
- `docs/02-架构与分层.md`

If code and `docs/` disagree, update `docs/` first or in the same change, and record why the implementation differs from the previous document state.

Major architecture changes must update the relevant `docs/` files. Do not leave architecture decisions only in chat, commit messages, or source comments.

### Project Memory: Obsidian

`E:\知识库\04-项目\DirBridge` is the project memory area for long-running thinking and reusable knowledge.

Use Obsidian for:

- technical research notes
- design drafts
- AI discussion records
- bug retrospectives
- development logs
- tradeoff analysis
- learning notes

Do not write build artifacts, logs, binaries, temporary files, generated dependency folders, or runtime output into the knowledge base.

All knowledge-base documents should be Markdown. Important design notes should use Obsidian-style backlinks such as `[[DirBridge]]`, `[[远程文件系统]]`, or `[[传输队列]]` where useful.

### Project Execution: Source Code

`E:\Workspace\Project\DirBridge` is the source repository and execution workspace.

Source code and build configuration live here:

- `src/`
- `cmake/`
- `scripts/`
- `CMakeLists.txt`
- `CMakePresets.json`
- `deps.lock.json`

Generated or local-only directories must not be committed:

- `build/`
- `third_party/`
- `config/`
- `logs/`

## Implementation Rules

- Keep the main stack aligned with the official docs: Qt Widgets, C++17, CMake, libcurl, nlohmann/json, and spdlog.
- Keep Core logic as pure C++ where practical. Qt UI should adapt and display state, not own protocol or transfer logic.
- UI should not include libcurl details directly. Remote behavior should go through protocol/backend abstractions.
- For MVP work, prefer small verifiable slices: implement one workflow, update docs, run checks, then commit.
- Use Doxygen-style comments for new or modified functions. Public interfaces should document intent, parameters, return values, and important side effects; non-trivial internal helpers should also be documented.
- Do not introduce SQLite, plugin systems, built-in editors, SSH terminal features, or secure password storage unless the MVP docs are explicitly updated first.

## Documentation Rules

- All project documentation is Markdown.
- Keep `docs/` focused on stable project truth: decisions, architecture, build/dependency rules, and current MVP scope.
- Put exploratory notes and ongoing thinking in Obsidian, not in `docs/`.
- Keep historical drafts under `docs/archive/` only when they explain why the current direction exists.
- When moving material from drafts into official docs, rewrite it into the current DirBridge direction instead of copying old XFolder wording blindly.

## Before Editing Code

1. Check `git status --short`.
2. Read the relevant official docs.
3. Inspect the current implementation before assuming older notes are still accurate.
4. Keep changes scoped to the current MVP step.
5. After changes, run the smallest useful validation, usually:

```powershell
cmake --build build\codex-verify
.\build\codex-verify\DirBridge.exe --check-deps
.\build\codex-verify\DirBridge.exe --smoke-test
git diff --check
```

If using `build/run`, follow `docs/运行环境说明.md`.

## Current MVP Direction

The project is currently in the first-stage MVP.

Completed direction:

- project renamed to DirBridge
- Qt Widgets shell
- local file panel
- JSON site configuration
- UI and file logging
- base file model
- remote filesystem abstraction
- libcurl remote directory listing entrypoint

Still in scope for MVP:

- real FTP/SFTP server validation
- remote refresh/new directory/delete/rename
- transfer task model and queue
- file upload/download
- drag-and-drop transfer

Deferred:

- remote editing
- permission management
- folder comparison and sync
- SSH terminal
- plugin system
- SQLite
- built-in editor
- secure password storage
