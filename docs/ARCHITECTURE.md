# MirageSystem Architecture

## Overview

MirageSystem is a Windows-based Android device mirroring and control system.
Core principle: **No USB debugging required** — AOA (Android Open Accessory) is the
primary communication pathway. ADB is fallback/debug only.

```
┌─────────────────────────────────────────────────────────────────┐
│                     MirageGUI (Windows)                         │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────────┐   │
│  │  ImGui   │  │  Vulkan  │  │  H.264   │  │   Template   │   │
│  │  UI      │  │  Render  │  │  Decode  │  │   Matcher    │   │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └──────┬───────┘   │
│       │              │              │               │           │
│  ┌────┴──────────────┴──────────────┴───────────────┴───────┐  │
│  │              GuiApplication (main loop)                   │  │
│  └─────────────────────────┬─────────────────────────────────┘  │
│                            │                                    │
│  ┌─────────────────────────┴─────────────────────────────────┐  │
│  │            HybridCommandSender (3-tier fallback)          │  │
│  │  ┌──────────┐  ┌──────────────┐  ┌──────────────────┐    │  │
│  │  │ AOA HID  │  │  MIRA USB    │  │  ADB Fallback    │    │  │
│  │  │ (1-8ms)  │  │  (bulk xfer) │  │  (150-300ms)     │    │  │
│  │  └──────────┘  └──────────────┘  └──────────────────┘    │  │
│  └───────────────────────────────────────────────────────────┘  │
│                                                                 │
│  ┌──────────────────────────────────┐  ┌────────────────────┐  │
│  │  MultiUsbCommandSender          │  │  TCP Video Recv    │  │
│  │  (libusb, WinUSB, AOA protocol) │  │  (WiFi H.264)      │  │
│  └──────────────┬───────────────────┘  └────────┬───────────┘  │
└─────────────────┼───────────────────────────────┼───────────────┘
                  │ USB                           │ WiFi
┌─────────────────┼───────────────────────────────┼───────────────┐
│                 │        Android Device          │               │
│  ┌──────────────┴───────────┐  ┌────────────────┴────────────┐  │
│  │  MirageService (AOA)     │  │  CaptureActivity            │  │
│  │  - Command receiver      │  │  - MediaProjection          │  │
│  │  - HID touch input       │  │  - H.264 encode             │  │
│  │  - Accessory mode        │  │  - TCP stream               │  │
│  └──────────────────────────┘  └─────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

## Key Design Principles

1. **Native resolution preservation** — No scaling anywhere. Bitrate/FPS for bandwidth.
2. **AOA-first, ADB-fallback** — Security: no USB debugging on production devices.
3. **Dual-pipeline communication** — Operations on USB, video on WiFi. Failover both ways.
4. **3-tier touch input** — AOA HID → MIRA USB bulk → ADB shell input.

## Directory Structure

```
src/
├── gui/                    # ImGui window, command handling, state
│   ├── gui_main.cpp        # Entry point, window creation
│   ├── gui_command.cpp      # Command dispatch to devices
│   ├── gui_window.cpp       # Win32 window management
│   └── gui_state.cpp        # Global state (devices, config)
├── gui_application.cpp      # Main loop, frame orchestration
├── gui_input.cpp            # Mouse/keyboard → device touch/key
├── gui_render*.cpp          # ImGui rendering (panels, dialogs)
│
├── multi_usb_command_sender # USB device management (libusb)
├── aoa_protocol.cpp         # AOA mode switch, HID registration
├── aoa_hid_touch.*          # AOA v2 HID multitouch controller
├── adb_touch_fallback.*     # ADB shell input fallback
├── hybrid_command_sender.*  # 3-tier fallback orchestrator
│
├── mirror_receiver.cpp      # TCP video stream receiver
├── h264_decoder.cpp         # FFmpeg H.264 decoding
├── video_texture.cpp        # Decoded frame → Vulkan texture
│
├── vulkan/                  # Vulkan rendering pipeline
│   ├── vulkan_context.cpp   # Instance, device, queues
│   ├── vulkan_swapchain.cpp # Swapchain management
│   └── vulkan_texture.cpp   # Texture upload/management
│
├── vulkan_template_matcher  # GPU template matching (SAT/NCC)
├── ai/                      # Template capture, learning mode
│
├── auto_setup.cpp           # MirageAutoSetup integration
├── adb_device_manager.cpp   # ADB device discovery
└── bandwidth_monitor.cpp    # Network bandwidth tracking

tests/
├── test_aoa_hid_touch.cpp   # HID report, coords, state (8 tests)
├── test_vid0_parser.cpp     # Video packet parsing
├── test_event_bus.cpp       # Event system
├── test_config_loader.cpp   # Config file loading
├── test_bandwidth_monitor   # Bandwidth calculation
├── test_route_controller    # Network route switching
├── test_vulkan_compute.cpp  # Vulkan compute pipeline
└── test_winusb_checker.cpp  # WinUSB driver detection

android/
├── app/                     # MirageService (AOA accessory)
└── capture/                 # CaptureActivity (MediaProjection)
```

## Communication Flow

```
Touch Input:  GUI click → HybridCommandSender → AOA HID (USB ctrl)
                                                → MIRA bulk (USB)
                                                → ADB shell (WiFi)

Video Stream: Android MediaProjection → H.264 encode
              → TCP socket (WiFi) → MirrorReceiver
              → H.264 decode → VulkanTexture → ImGui display
              
              (or USB bulk transfer if WiFi unavailable)
```

## Device-Specific Resolutions

| Device   | Resolution | Notes                    |
|----------|-----------|--------------------------|
| A9       | 800×1268  | RebotAi, MediaTek        |
| Npad X1  | 1080×1920 | Standard                 |

## Build

```
mkdir build && cd build
cmake .. -G "MinGW Makefiles"
mingw32-make -j4
```

Requires: MinGW-w64, Vulkan SDK, libusb-1.0, FFmpeg (avcodec/avformat)

## GPU

AMD Radeon (id:0x1638, integrated), Vulkan 1.3.260, AMD driver 26.1.1
