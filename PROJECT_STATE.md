# MirageSystem Project State
# Updated: 2026-02-20
# Read at session start, updated at session end.

## Current Phase: GUI Freeze Fixes Completed + GPT Gateway (MCP) Operationalization

## Active Blockers / Known Issues
- Build result detection in MCP: build succeeds but API reports "exe not found" (likely checks build/Release/ while outputs are in build/).
- WiFi ADB screenshot latency: A9 devices can still spike >8s in some conditions (warmup+retry mitigates; X1 observed ~3.2s via GPT).
- PROJECT_STATE.md was stale (last updated 2026-02-18) → updated today to improve GPT/Claude response accuracy.

## Completed Recently (2026-02-18 to 2026-02-20)
- Pipeline quality gates implemented and verified: 66/66 tests PASS (see Pipeline Enhancement Report).
- GUI freeze fixes shipped (async ADB/broadcast + USB recv timeout backoff + lock consolidation).
- GPT gatewayization confirmed end-to-end:
  - MCP status OK
  - WiFi ADB screenshot OK (X1 ~3.2s)
  - Build OK (mirage_vulkan.exe generated)
- MCP Server v5.0.0 confirmed stable in long uptime operation.

## Completed Earlier
- MCP Server v5.0.0: Streamable HTTP transport (hybrid SSE+HTTP)
- Screenshot 4-layer optimization: ADB lock consolidation, async fallback, WiFi keepalive
- Startup parallelization: 100s -> 4s (ADB init in background thread)
- USB Hub cp932 encoding fix (mojibake resolved)
- scrcpy-server raw H.264 streaming on all 3 test devices
- TCP-to-UDP bridge threads
- AOA Bridge v3.0.5: 42K -> 9.2K lines, 100% deployment success
- MirageView overlay UI (Direct2D, 20-device/256-slot)
- Bluetooth auto-pairing via WinRT APIs
- Watchdog v3.3 with ADB discovery and cloudflared tunnel
- MirageVulkan: Vulkan Video H.264 decoder, 7 GPU shaders
- Android APK 3-module split: MirageAndroid + MirageAccessory + MirageCapture

## Next Priorities (Ordered)
1. Fix MCP build result detection (exe path discovery in api_gateway.py)
2. Keep PROJECT_STATE/CLAUDE.md fresh (single source of truth for assistants)
3. scrcpy-server integration resume (Android 15 MediaProjection)
4. Multi-device video pipeline stress test (2x USB + 1x WiFi)
5. GUI refactoring (e.g., split oversized rendering/logic units)
6. MirageComplete -> MirageVulkan migration Phase 1 (android/, core scripts)

## Key Decisions Log
- 2026-02-20: GPT as primary ops gateway via MCP (status/screenshot/build verified)
- 2026-02-18: Multi-agent: Director(Opus) + Worker(Sonnet) + Reviewer(Sonnet)
- 2026-02-18: File-based memory (CLAUDE.md + PROJECT_STATE.md), not database
- 2026-02-16: MirageVulkan designated as main repo (MirageComplete = legacy)

## Device Status (Expected)
- USB: Npad X1 (93020523431940), A9 #956 (A9250700956), A9 #479 (A9250700479)
- WiFi: 192.168.0.3:5555 (X1), 192.168.0.6:5555 (A9 #956), 192.168.0.8:5555 (A9 #479)
- Total: 3 devices (USB 3 + WiFi 3 logical connections)

## MCP Server Health (Observed)
- Version: v5.0.0
- Status: Stable
- Memory: ~20–30 MB class
- Transports: SSE (active), Streamable HTTP (ready)

## Token Budget Notes
- Prefer cheap workers for routine ops; reserve premium model for planning/review
- Keep CLAUDE.md under ~2K tokens
