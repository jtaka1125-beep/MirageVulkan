# MirageSystem Project State
# Updated: 2026-02-18
# Read at session start, updated at session end.

## Current Phase: MCP Server v5.0 Stabilization + Multi-Agent Infrastructure

## Active Blockers
- WiFi ADB screenshot latency: A9 devices sometimes >8s (warmup+retry mitigates)
- Claude.ai does not support Streamable HTTP connector yet (SSE works)
- MirageComplete->MirageVulkan migration Phase 1 not yet started

## Completed This Week (2026-02-18)
- MCP Server v5.0.0: Streamable HTTP transport (hybrid SSE+HTTP)
- Screenshot 4-layer optimization: ADB lock consolidation, async fallback, WiFi keepalive
- Startup parallelization: 100s -> 4s (ADB init in background thread)
- USB Hub cp932 encoding fix (mojibake resolved)
- build_mcp_response deduplication (single code path for SSE+HTTP)
- 42 patch files archived to patches_archive/
- Claude Code v2.1.45 verified as latest
- CLAUDE.md + PROJECT_STATE.md memory foundation created for MirageVulkan

## Completed Earlier
- scrcpy-server raw H.264 streaming on all 3 test devices
- TCP-to-UDP bridge threads
- AOA Bridge v3.0.5: 42K -> 9.2K lines, 100% deployment success
- MirageView overlay UI (Direct2D, 20-device/256-slot)
- Bluetooth auto-pairing via WinRT APIs
- Watchdog v3.3 with ADB discovery and cloudflared tunnel
- MirageVulkan: Vulkan Video H.264 decoder, 7 GPU shaders, 16 test suites
- Android APK 3-module split: MirageAndroid + MirageAccessory + MirageCapture

## Next Priorities (Ordered)
1. Pipeline quality gates (build gate + AI reviewer in task_queue.py)
2. Sonnet/Opus model split for token budget optimization
3. MirageComplete -> MirageVulkan migration Phase 1 (android/, core scripts)
4. scrcpy-server integration resume (Android 15 MediaProjection)
5. Multi-device video pipeline test (2x USB + 1x WiFi)
6. GUI refactoring (gui_render.cpp 938 lines -> split)

## Key Decisions Log
- 2026-02-18: Multi-agent: Director(Opus) + Worker(Sonnet) + Reviewer(Sonnet)
- 2026-02-18: File-based memory (CLAUDE.md + PROJECT_STATE.md), not database
- 2026-02-16: MirageVulkan designated as main repo (MirageComplete = legacy)
- 2026-02-11: Android APK 2-split: MirageCapture + MirageAccessory
- 2026-02-11: AOA permission dialog 3-tier auto-approval
- 2026-02-08: SSE + Streamable HTTP hybrid deployment

## Device Status
- USB: Npad X1 (93020523431940) on hub port 2-2
- WiFi: 192.168.0.3 (Npad X1), 192.168.0.6 (A9), 192.168.0.8 (A9)
- All 4 devices connected and operational

## MCP Server Health
- Version: v5.0.0
- Uptime: Stable after optimizations
- Memory: ~34 MB, Errors: 0
- Transports: SSE (active), Streamable HTTP (ready)

## Token Budget Notes
- Plan: Max 20x (00/mo, ~900 msgs/5h, ~220K tokens/window)
- Workers: Use Sonnet (3x cheaper than Opus)
- CLAUDE.md: Keep under 2K tokens
- MAX_TURNS: 8 complex, 5 simple edits
