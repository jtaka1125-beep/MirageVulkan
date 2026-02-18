# MirageSystem — Claude Code Instructions

## Project Overview
MirageVulkan: Windows-based Android device automation and screen mirroring platform.
USB AOA + WiFi hybrid connectivity, up to 20 simultaneous devices, Vulkan GPU H.264 decode, AI screen analysis.
This is the main repository (successor to MirageComplete, full upper-compatible).

## Repository Map
- **MirageVulkan** (C:\MirageWork\MirageVulkan) — MAIN: C++ core, Android APKs, scripts, shaders
- **mcp-server** (C:\MirageWork\mcp-server) — MCP server for Claude.ai integration, ADB management
- **MirageComplete** (C:\MirageWork\MirageComplete) — LEGACY: being migrated into MirageVulkan

## Architecture
- **C++ GUI** (src/, ~90 files): Vulkan + ImGui, Vulkan Video H.264 decode, libusb AOA, EventBus
- **GPU Shaders** (shaders/, 7 .comp files): YUV->RGBA, NCC template matching, pyramid downsampling
- **Android APKs** (android/): MirageAndroid (video), MirageAccessory (AOA commands), MirageCapture (permissions)
- **Scripts** (scripts/, 31 files): deploy_apk.py, device_health.py, mirage_cli.py, bt_auto_pair.py
- **MCP Server** (../mcp-server/): server.py (SSE+Streamable HTTP), task_queue.py, watchdog.py

## Build
- **C++**: Visual Studio 2022, CMake 4.x, Vulkan SDK 1.4+, FFmpeg
  cmake .. -G "Visual Studio 17 2022" -A x64
  cmake --build . --config Release
- **Tests**: 16 suites, all PASS: ctest --output-on-failure
- **APK**: python scripts/deploy_apk.py

## Absolute Rules
1. ALWAYS read the full file before modifying large files (server.py=1900+ lines, gui_render.cpp=938 lines)
2. ALWAYS verify build after code changes (cmake --build . --config Release)
3. NEVER break existing tests (16 suites must all PASS)
4. Japanese comments preferred for log messages and inline comments
5. Use MLOG_* macros for logging (printf/cout/std::clog prohibited)

## Coding Conventions
- RAII throughout (mutex/lock_guard/unique_lock)
- Thread safety: g_running atomic, 153+ mutex/lock points
- Error handling: try-catch required, WinMain fully protected
- Inter-module communication via EventBus (no direct cross-module references)
- Result<T> type for error propagation

## Device Environment
- USB: Npad X1 (TAB005) on ReTRY HUB port 2-2
- WiFi ADB: 192.168.0.3:5555 (Npad X1), 192.168.0.6:5555 (A9), 192.168.0.8:5555 (A9)
- USB Hub: ReTRY HUB (retryhub_ctrl.exe, output cp932 encoding)
- Supported: Npad X1 (Android 13, 1200x2000), A9 (Android 15, 800x1340)

## Communication Design
- 2 channels (control + video) x 2 pipes (USB + WiFi)
- USB priority; bandwidth pressure -> video to WiFi -> FPS reduction fallback
- AOA requires no USB debugging (security by design)
- APK split: MirageCapture (video) + MirageAccessory (control) for fault isolation

## Key Decisions
- scrcpy-server for MediaProjection bypass (Android 15)
- SSE + Streamable HTTP hybrid (Claude.ai connector uses SSE)
- MirageComplete -> MirageVulkan migration (Phase 1-3 planned)
- Multi-agent: Director(Opus) + Worker(Sonnet) + Reviewer(Sonnet read-only)
