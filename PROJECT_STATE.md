# MirageSystem Project State
# Updated: 2026-02-24 Session 4
# Read at session start, updated at session end.

## Current Phase: GUI Refactoring + Test Coverage

## Active Blockers / Known Issues
- AOA full-path verification: requires physical USB connection (currently WiFi-only)
- WiFi ADB screenshot latency: A9 devices can still spike >8s in some conditions

## Completed 2026-02-24 Session 4 (This Session)
- gui_init.cpp: 4 static helpers extracted from initializeGUI():
  - onDeviceSelected() (commit dca432a)
  - onStartMirroring() + registerEventBusSubscriptions() (commit 77f7d19)
  - startRouteEvalThread() from initializeRouting() (commit a019037)
- Dead code cleanup: removed 8 scrcpy scripts + 34 _fix_*.py temp patches (commit e9718dc)
- gui_threads.cpp: 3 frame-update helpers extracted from deviceUpdateThread() [in progress]
- Tests: 31/31 pass throughout

## Completed 2026-02-24 Sessions 1-3
- MCP build detection fix: mirage_gui.exe -> mirage_vulkan_debug_dev.exe (server.py)
- TemplateStore CRUD 12 cases, ActionMapper 14 cases, WinUsbChecker 14 cases
- WinUSB installer: wdi-simple.exe (3.9MB) bundled at driver_installer/tools/wdi/
- Migration Phase 1 verified complete (android/, scripts/, docs/ all present)
- GUI refactoring started: onDeviceSelected() extracted from lambda

## Completed 2026-02-23
- Pipeline quality gates: 66/66 tests PASS
- Video pipeline total audit: HybridReceiver USB recovery FIXED, scrcpy dead code removed
- ScreenCaptureService startup deduplicated -> autoStartCaptureService()

## Architecture (Video Pipeline)
- Engine: FULLY CUSTOM. No scrcpy.
- Android TX: MediaProjection -> H264Encoder -> AnnexBSplitter -> RtpH264Packetizer
              -> UsbVideoSender (VID0 batch) / TcpVideoSender / UdpVideoSender
- PC RX (USB): HybridCommandSender.video_callback -> VID0 parse -> g_usb_decoders[device_id]
- PC RX (TCP): adb forward tcp:50100 -> MirrorReceiver.start_tcp_vid0 -> VID0 -> RTP -> decode
- Routing: RouteController (tcp_only_mode=true), FPS via ADB broadcast to MirageCapture APK
- Note: g_hybrid_receiver always nullptr at runtime; TCP+USB decoder path active

## Next Priorities (Ordered)
1. [IN PROGRESS] gui_threads.cpp refactor: frame update helper extraction
2. gui_device_control.cpp: renderDeviceControlPanel() may have extractable sub-panels
3. AOA full-path verification [BLOCKED: physical USB]
4. Multi-device video pipeline stress test [BLOCKED: physical USB]
5. Migration Phase 2: driver_installer/ integration, bt_auto_pair, adb_video_capture

## GUI File Line Counts (after current refactoring session)
- gui_ai_panel.cpp:       659 (already well-organized)
- gui_init.cpp:           734 (many named functions, all manageable)
- gui_threads.cpp:        591 -> ~410 after current task
- gui_device_control.cpp: 539 (named functions, good structure)
- gui_command.cpp:        356 (clean)
- gui_window.cpp:         237 (clean)

## Key Decisions Log
- 2026-02-24: Dead scrcpy scripts and _fix_*.py temp patches removed from scripts/
- 2026-02-24: GUI refactoring: extract blocks as named static functions for readability
- 2026-02-24: ActionMapper default: "tap:<id>" for unregistered keys
- 2026-02-24: wdi-simple.exe force-tracked in git (required for WinUSB installer)
- 2026-02-24: Migration Phase 1 complete; MirageVulkan is superset of MirageComplete
- 2026-02-23: Video engine fully custom, no scrcpy
- 2026-02-16: MirageVulkan designated as main repo (MirageComplete = legacy)

## Test Coverage (31/31 pass)
- TemplateStoreTest 12, ActionMapperTest 14, WinUsbCheckerTest 14
- TemplateManifestTest, BandwidthMonitorTest, HybridSenderTest, MirrorReceiverTest
- VID0ParserTest, ConfigLoaderTest, VisionDecisionEngineTest (40 cases), etc.

## Device Status
- WiFi: 192.168.0.3:5555 (X1), 192.168.0.6:5555 (A9#956), 192.168.0.8:5555 (A9#479)
- USB: 3 devices expected (WinUSB driver required for AOA)

## MCP Server Health
- Version: v5.0.0 | Status: Stable | Transports: SSE + Streamable HTTP
