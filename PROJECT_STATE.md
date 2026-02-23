# MirageSystem Project State
# Updated: 2026-02-24
# Read at session start, updated at session end.

## Current Phase: Video Pipeline Audit + Dead Code Removal

## Active Blockers / Known Issues
- Build result detection in MCP: build succeeds but API reports "exe not found" (likely checks build/Release/ while outputs are in build/).
- WiFi ADB screenshot latency: A9 devices can still spike >8s in some conditions (warmup+retry mitigates; X1 observed ~3.2s via GPT).

## Completed Recently (2026-02-20 to 2026-02-23)
- Pipeline quality gates implemented and verified: 66/66 tests PASS (see Pipeline Enhancement Report).
- GUI freeze fixes shipped (async ADB/broadcast + USB recv timeout backoff + lock consolidation).
- GPT gatewayization confirmed end-to-end: MCP, WiFi ADB screenshot, Build all OK.
- MCP Server v5.0.0 confirmed stable in long uptime operation.
- Video pipeline total audit completed (2026-02-23).
- launchScrcpyServer() removed from tcp_video_receiver (dead code, scrcpy not used).
- Architecture confirmed: fully custom video engine, no scrcpy dependency.
- [2026-02-23 Session 1] scrcpy dead code removal, architecture clarification.
- [2026-02-23 Session 2] Comprehensive video pipeline audit & improvements:
  - HybridReceiver USB recovery FIXED: usb_available uses usb_last_packet_time_ (500ms window)
  - feed_usb_data() cooldown-aware: no premature WiFi→USB promotion during cooldown
  - usb_connected() legacy accessor FIXED: same time-based logic (was always false)
  - getStats().usb_connected FIXED: time-based (was always false via null usb_receiver_)
  - Removed: pkill -f scrcpy from startScreenCapture() (dead op, 500ms latency removed)
  - Refactored: ScreenCaptureService startup deduplicated ↁEautoStartCaptureService()
  - Refactored: MediaProjection tap coordinates now dynamic (parse wm size, 0.73/0.61 ratio)
  - Cleaned: All scrcpy comments removed from active code (auto_setup, adb_device_manager,
             multi_device_receiver, mirror_receiver, gui_main, gui_init, gui_threads, gui_application)
  - Documented: g_hybrid_receiver always nullptr (HybridReceiver inactive, TCP path active)
  - Deprecated: startScreenCapture/startScreenCaptureOnAll (not called, AutoSetup is noop)
  - Confirmed: BandwidthMonitor properly fed by RouteEval thread (USB via hybrid_cmd, WiFi via multi_receiver)
  - Confirmed: IDR callback debounce uses shared_ptr<atomic> (thread-safe, not thread_local)
  - Confirmed: tcp_vid0_receive_thread has robust auto-reconnect (50 retries + outer loop)

## Completed Earlier
- MCP Server v5.0.0: Streamable HTTP transport (hybrid SSE+HTTP)
- Screenshot 4-layer optimization: ADB lock consolidation, async fallback, WiFi keepalive
- Startup parallelization: 100s -> 4s (ADB init in background thread)
- USB Hub cp932 encoding fix (mojibake resolved)
- TCP-to-UDP bridge threads
- AOA Bridge v3.0.5: 42K -> 9.2K lines, 100% deployment success
- MirageView overlay UI (Direct2D, 20-device/256-slot)
- Bluetooth auto-pairing via WinRT APIs
- Watchdog v3.3 with ADB discovery and cloudflared tunnel
- MirageVulkan: Vulkan Video H.264 decoder, 7 GPU shaders
- Android APK 3-module split: MirageAndroid + MirageAccessory + MirageCapture

## Architecture (Video Pipeline)
- Engine: FULLY CUSTOM. No scrcpy. No third-party streaming libs.
- Android TX: MediaProjection -> H264Encoder (async MediaCodec) -> AnnexBSplitter
              -> RtpH264Packetizer -> UsbVideoSender (VID0 batch) / TcpVideoSender / UdpVideoSender
- PC RX (USB): HybridCommandSender.video_callback -> VID0 parse -> g_usb_decoders[device_id]
               (MirrorReceiver with init_decoder_only, RTP fed via feed_rtp_packet)
- PC RX (TCP): adb forward tcp:50100 -> MirrorReceiver.start_tcp_vid0 (auto-reconnect)
               -> VID0 parse -> RTP -> MirrorReceiver decode -> EventBus -> GUI
- Routing:     RouteController (tcp_only_mode=true) evaluates WiFi bandwidth every 1s
               -> sends FPS commands via ADB broadcast to MirageCapture APK
- Frame delivery: MultiDeviceReceiver.framePollThreadFunc (2ms poll) -> FrameCallback -> EventBus

## Next Priorities (Ordered)
1. AOA full-path verification: UsbVideoSender (Android) <-> feed_usb_data() (PC) end-to-end
2. Fix MCP build result detection -- DONE (5f1a29e)
3. Multi-device video pipeline stress test (2x USB + 1x WiFi simultaneous)
4. GUI refactoring (split oversized rendering/logic units)
5. MirageComplete -> MirageVulkan migration Phase 1

## Key Decisions Log
- 2026-02-23: scrcpy-server NOT used. Video engine is fully custom.
- 2026-02-23: HybridReceiver USB recovery was broken (usb_receiver_ always nullptr) ↁEFIXED.
- 2026-02-23: g_hybrid_receiver always nullptr at runtime ↁETCP+USB decoder path is active.
- 2026-02-23: startScreenCapture/AutoSetup = noop wrappers (deprecated, not called by GUI).
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
- Memory: ~20-30 MB class
- Transports: SSE (active), Streamable HTTP (ready)

## Token Budget Notes
- Prefer cheap workers for routine ops; reserve premium model for planning/review
- Keep CLAUDE.md under ~2K tokens
`n- [2026-02-24] ActionMapper 14 cases (AM-1..AM-14) - 30/30 total`n- [2026-02-24] WinUsbChecker 14 cases (WUC-1..WUC-14) - 31/31 total`n- [2026-02-24] Migration Phase 1: install_android_winusb.py + wdi-simple.exe copied from MirageComplete
