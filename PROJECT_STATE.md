# MirageSystem Project State
# Updated: 2026-02-24
# Read at session start, updated at session end.

## Current Phase: Test Coverage + Migration Cleanup

## Active Blockers / Known Issues
- AOA full-path verification: requires physical USB connection (currently WiFi-only)
- WiFi ADB screenshot latency: A9 devices can still spike >8s in some conditions

## Completed 2026-02-24 (This Session)
- [Session 1] MCP build detection fix: mirage_gui.exe -> mirage_vulkan_debug_dev.exe (server.py)
- [Session 1] TemplateStore CRUD 12 cases (TS-1..TS-12) - total 30/30
- [Session 2] ActionMapper 14 cases (AM-1..AM-14) - total 30/30
  - Confirmed: getAction/getTextAction default returns "tap:<id>", not empty string
- [Session 2] WinUSB installer: wdi-simple.exe (3.9MB) bundled at driver_installer/tools/wdi/
- [Session 2] Migration Phase 1 verified complete:
  - android/ (APK, keystore) in MirageVulkan
  - scripts/ all present (mirage_cli.py, auto_setup_devices.py, etc.)
  - docs/ superset of MirageComplete; usb_driver .inf/.cat refreshed

## Completed 2026-02-23
- Pipeline quality gates: 66/66 tests PASS
- Video pipeline total audit: HybridReceiver USB recovery FIXED, scrcpy dead code removed
- usb_connected() / feed_usb_data() / getStats().usb_connected all FIXED
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
1. AOA full-path verification [BLOCKED: physical USB]
2. Multi-device video pipeline stress test [BLOCKED: physical USB]
3. GUI refactoring: extract callbacks from initializeGUI() (589L), gui_threads.cpp (591L)
4. Migration Phase 2: driver_installer/ integration, bt_auto_pair, adb_video_capture
5. VisionDecisionEngine unit tests

## Key Decisions Log
- 2026-02-24: ActionMapper default: "tap:<id>" for unregistered keys (not empty)
- 2026-02-24: wdi-simple.exe force-tracked in git (required for WinUSB installer)
- 2026-02-24: Migration Phase 1 complete; MirageVulkan is superset of MirageComplete
- 2026-02-23: Video engine fully custom, no scrcpy
- 2026-02-23: g_hybrid_receiver always nullptr; TCP+USB decoder path active
- 2026-02-16: MirageVulkan designated as main repo (MirageComplete = legacy)

## Test Coverage (30/30 pass)
- TemplateStoreTest 12, ActionMapperTest 14, WinUsbCheckerTest 14
- TemplateManifestTest 20+, BandwidthMonitorTest, HybridSenderTest, MirrorReceiverTest, VID0ParserTest

## Device Status
- WiFi: 192.168.0.3:5555 (X1), 192.168.0.6:5555 (A9#956), 192.168.0.8:5555 (A9#479)
- USB: 3 devices expected (WinUSB driver required for AOA)

## MCP Server Health
- Version: v5.0.0 | Status: Stable | Transports: SSE + Streamable HTTP
