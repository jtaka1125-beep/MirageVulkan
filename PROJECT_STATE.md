# MirageSystem Project State
# Updated: 2026-02-24 Session 8
# Read at session start, updated at session end.

## Current Phase: Migration Phase 3 COMPLETE + GUI Refactoring COMPLETE

## Active Blockers / Known Issues
- AOA full-path verification: requires physical USB connection (currently WiFi-only)
- WiFi ADB screenshot latency: A9 devices can still spike >8s in some conditions

## Completed 2026-02-24 Session 8 (This Session)
### Migration Phase 3 COMPLETE
- Untracked files: add_multi_touch.py, src/adb_device_manager_patch.py → .gitignore (patch applied, disposable)
- .gitignore: added `add_*.py`, `src/*_patch.py` patterns for one-off scripts
- tools/: 9 debug PS scripts moved to tools/archive/ (freeze_probe, launch_gui_with_probe, remote_cycle, restore_windows, run_dev_logged, run_dev_logged2, show_inputhash, start_mirage_detached)
- tools/scrcpy-server-v3.3.4: already excluded by `tools/scrcpy-server-*` pattern (not tracked)

## Completed 2026-02-24 Session 7
### GUI Refactoring Final (gui_ai_panel.cpp 659→663 lines)
- Extracted renderLearningCapture() from renderLearningMode() (commit a2320a9)
  - renderLearningMode(): Start/Stop UI + early return (~43 lines)
  - renderLearningCapture(): slot, name, ROI, capture button, save logic (~92 lines)
### Migration Phase 2 Status
- scripts/: All tools migrated (bt_auto_pair.py, adb_video_capture.py, usb_lan_controller.py, etc.)
- driver_installer/: Already in sync
- macro_editor/: Already migrated with mirage_client.py patches
- Phase 2 essentially COMPLETE (no missing files found in MirageComplete vs MirageVulkan)

## Completed 2026-02-24 Session 6
### GUI Refactoring continued (gui_init.cpp 711→724 lines with new helpers)
- Extracted registerDevicesForRouteController() from initializeRouting() (commit 350d9fa)
  - Unified USB-AOA and TCP-only registration branches into single named static function
  - Added std::any_of for X1 detection (replaces raw loop)
  - initializeRouting() now reads as 5 linear setup calls
- Removed 2 stale comments from initializeRouting() (commit ca15423)
### Fixes
- fix: add -p com.mirage.capture to FPS broadcast in onDeviceSelected() (commit 881719c)
  - Was broadcasting to ALL apps instead of only MirageCapture
- complete_auto_aoa.py: added approve_aoa_dialog() function (commit 5352d15)
  - uiautomator dump -> find AOA permission dialog -> check 'always use' -> tap OK
  - --dialog-only flag for standalone approval; --dialog-timeout parameter
  - Fixed package refs: com.mirage.accessory + com.mirage.capture (2-APK split)

## Completed 2026-02-24 Session 5
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
2. Multi-device video pipeline stress test [BLOCKED: physical USB]
3. deploy_apk.py live test on all 3 devices (WiFi ADB)

## GUI File Line Counts (Updated 2026-02-28)
- gui_ai_panel.cpp:       663
- gui_init.cpp:           801
- gui_threads.cpp:        606
- gui_device_control.cpp: 539
- gui_command.cpp:        356
- gui_window.cpp:         237
- gui_main.cpp:           279
- gui_state.cpp:           11
- mirage_context.cpp:       0
- TOTAL:                 3492 lines

## Key Decisions Log
- 2026-02-24 Sess8: Migration Phase 3 COMPLETE. tools/ archive finalized, .gitignore patterns added, untracked patch scripts excluded.
- 2026-02-24 Sess7: GUI refactoring declared COMPLETE. All functions <100 lines (except renderLearningCapture 92, renderVisionStates 88). Migration Phase 2 verified complete.
- 2026-02-24 Sess6: registerDevicesForRouteController() extracted; onDeviceSelected FPS broadcast fixed (-p flag missing)
- 2026-02-24 Sess6: approve_aoa_dialog() added to complete_auto_aoa.py (uiautomator approach)
- 2026-02-24: deploy_apk.py updated: app module removed, accessory pkg fixed
- 2026-02-24: usb_device_discovery: allow_wait=false prevents main-thread blocking at startup
- 2026-02-24: Dead scrcpy scripts and _fix_*.py temp patches removed from scripts/
- 2026-02-24: GUI refactoring: extract blocks as named static functions for readability
- 2026-02-24: wdi-simple.exe force-tracked in git (required for WinUSB installer)
- 2026-02-24: Migration Phase 1 complete; MirageVulkan is superset of MirageComplete
- 2026-02-23: Video engine fully custom, no scrcpy
- 2026-02-16: MirageVulkan designated as main repo (MirageComplete = legacy)

## Test Coverage (33 tests registered in CMakeLists.txt)
- TemplateStoreTest 12, ActionMapperTest 14, WinUsbCheckerTest 14
- TemplateManifestTest, BandwidthMonitorTest, HybridSenderTest, MirrorReceiverTest
- VID0ParserTest, ConfigLoaderTest, VisionDecisionEngineTest (40 cases), etc.

## Device Status
- WiFi: 192.168.0.3:5555 (X1), 192.168.0.6:5555 (A9#956), 192.168.0.8:5555 (A9#479)
- USB: 3 devices expected (WinUSB driver required for AOA)

## MCP Server Health
- Version: v5.0.0 | Status: Stable | Transports: SSE + Streamable HTTP
