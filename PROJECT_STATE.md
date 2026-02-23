# MirageSystem Project State
# Updated: 2026-02-24 Session 5
# Read at session start, updated at session end.

## Current Phase: GUI Refactoring + Code Quality

## Active Blockers / Known Issues
- AOA full-path verification: requires physical USB connection (currently WiFi-only)
- WiFi ADB screenshot latency: A9 devices can still spike >8s in some conditions

## Completed 2026-02-24 Session 5 (This Session)
### GUI Refactoring (gui_init.cpp 734→711 lines, gui_threads.cpp 591→563 lines)
- gui_init.cpp: 6 static helpers total extracted (all initializeGUI/initializeRouting callbacks)
  - onFpsCommand() + onRouteCommand() from initializeRouting() (commit 88b3c49)
  - initializeRouting() callbacks are now ALL single-line delegates
- gui_threads.cpp: frame update helpers + dead TCP receiver code removed (commits fc34bf6, ae4f619)
- scripts/: removed 8 scrcpy scripts + 34 _fix_*.py temp patches (commits e9718dc, ed4419e)
### Fixes
- deploy_apk.py: removed legacy 'app' module; fixed grant_permissions() to use com.mirage.accessory
- usb_device_discovery.cpp: add allow_wait=false fast path on initial open (no blocking on main thread)

## Completed 2026-02-24 Sessions 1-4
- WinUSB installer: wdi-simple.exe bundled, git force-tracked
- Migration Phase 1 verified complete (android/, scripts/, docs/ all present)
- GUI refactoring started, 4 more helpers extracted: onDeviceSelected, onStartMirroring,
  registerEventBusSubscriptions, startRouteEvalThread
- Pipeline quality gates: 66/66 tests pass
- Video pipeline audit: HybridReceiver USB recovery FIXED, scrcpy dead code removed
- ScreenCaptureService startup deduplicated -> autoStartCaptureService()

## Architecture (Video Pipeline)
- Engine: FULLY CUSTOM. No scrcpy.
- Android TX: MediaProjection -> H264Encoder -> AnnexBSplitter -> RtpH264Packetizer
              -> UsbVideoSender (VID0 batch) / TcpVideoSender / UdpVideoSender
- PC RX (USB): HybridCommandSender.video_callback -> VID0 parse -> g_usb_decoders[device_id]
- PC RX (TCP): adb forward tcp:50100 -> MirrorReceiver.start_tcp_vid0 -> VID0 -> RTP -> decode
- Routing: RouteController (tcp_only_mode=true), FPS via ADB broadcast to MirageCapture APK
- Note: g_hybrid_receiver always nullptr at runtime; TCP+USB decoder path active

## Android APK Status (2 APKs, both built)
- :capture (com.mirage.capture) - Built: capture-release.apk
  - ScreenCaptureService, H264Encoder, RtpH264Packetizer, TcpVideoSender, UdpVideoSender
  - AudioCaptureService, ML: ScreenAnalyzer, ChangeDetector, OcrEngine
- :accessory (com.mirage.accessory) - Built: accessory-release.apk
  - AccessoryIoService (AOA USB I/O), MirageAccessibilityService
  - Protocol.kt (MIRA protocol, cmd 0x00-0x27)
  - directBootAware=true, USB_ACCESSORY_ATTACHED intent-filter, accessory_filter.xml
- :app (legacy monolith) - EXCLUDED from settings.gradle.kts, sources in android/app/ for reference

## Next Priorities (Ordered)
1. AOA full-path test: connect USB, run deploy_apk.py, verify tap commands arrive [BLOCKED: physical USB]
2. AOA dialog auto-approval (uiautomator approach): add to MirageAutoSetup scripts
3. gui_device_control.cpp: check if any further extraction useful (539 lines, already clean)
4. Multi-device video pipeline stress test [BLOCKED: physical USB]
5. Migration Phase 2: driver_installer/ integration, bt_auto_pair, adb_video_capture

## GUI File Line Counts (current state)
- gui_ai_panel.cpp:       659 (well-organized, many small static functions)
- gui_init.cpp:           711 (all callbacks now single-line delegates)
- gui_threads.cpp:        563 (frame helpers extracted)
- gui_device_control.cpp: 539 (already clean, small named functions)
- gui_command.cpp:        356 (clean)
- gui_window.cpp:         237 (clean)

## Key Decisions Log
- 2026-02-24: deploy_apk.py updated: app module removed, accessory pkg fixed
- 2026-02-24: usb_device_discovery: allow_wait=false prevents main-thread blocking at startup
- 2026-02-24: Dead scrcpy scripts and _fix_*.py temp patches removed from scripts/
- 2026-02-24: GUI refactoring: extract blocks as named static functions for readability
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
