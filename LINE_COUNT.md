# MirageVulkan ソースコード行数レポート

**計測日:** 2026-02-16

---

## サマリ

| カテゴリ | ファイル数 | 行数 |
|---|---:|---:|
| video/ (デコーダ・パーサ) | 10 | 5,036 |
| vulkan/ (Vulkan基盤) | 10 | 1,305 |
| gui/ (GUI全般) | 20 | 5,619 |
| 通信系 (USB/TCP/ADB/AOA) | 36 | 8,711 |
| コンピュート (テンプレートマッチ) | 4 | 1,497 |
| ヘッダのみ (ユーティリティ) | 10 | 1,421 |
| shaders/ (コンピュートシェーダ) | 7 | 553 |
| tests/ | 3 | 757 |
| CMakeLists.txt | 1 | 407 |
| **合計** | **101** | **25,306** |

---

## カテゴリ別詳細

### video/ — 5,036 行

| ファイル | 行数 |
|---|---:|
| vulkan_video_decoder.cpp | 1,874 |
| vulkan_video_decoder.hpp | 341 |
| h264_parser.cpp | 541 |
| h264_parser.hpp | 300 |
| yuv_converter.cpp | 731 |
| yuv_converter.hpp | 131 |
| unified_decoder.cpp | 456 |
| unified_decoder.hpp | 188 |
| h264_decoder.cpp | 364 |
| h264_decoder.hpp | 110 |

### vulkan/ — 1,305 行

| ファイル | 行数 |
|---|---:|
| vulkan_context.cpp | 222 |
| vulkan_context.hpp | 64 |
| vulkan_swapchain.cpp | 161 |
| vulkan_swapchain.hpp | 45 |
| vulkan_texture.cpp | 156 |
| vulkan_texture.hpp | 40 |
| vulkan_compute.cpp | 165 |
| vulkan_compute.hpp | 76 |
| vulkan_image.cpp | 305 |
| vulkan_image.hpp | 71 |

### gui/ — 5,619 行

| ファイル | 行数 |
|---|---:|
| gui_application.cpp | 737 |
| gui_application.hpp | 471 |
| gui_render.cpp | 119 |
| gui_render_dialogs.cpp | 166 |
| gui_render_left_panel.cpp | 442 |
| gui_render_main_view.cpp | 419 |
| gui_input.cpp | 529 |
| gui/gui_command.cpp | 265 |
| gui/gui_command.hpp | 24 |
| gui/gui_device_control.cpp | 500 |
| gui/gui_device_control.hpp | 79 |
| gui/gui_main.cpp | 761 |
| gui/gui_state.cpp | 11 |
| gui/gui_state.hpp | 76 |
| gui/gui_threads.cpp | 492 |
| gui/gui_threads.hpp | 18 |
| gui/gui_window.cpp | 238 |
| gui/gui_window.hpp | 13 |
| gui/mirage_context.cpp | 102 |
| gui/mirage_context.hpp | 157 |

### 通信系 — 8,711 行

| ファイル | 行数 |
|---|---:|
| usb_video_receiver.cpp | 333 |
| usb_video_receiver.hpp | 112 |
| mirror_receiver.cpp | 684 |
| mirror_receiver.hpp | 160 |
| hybrid_receiver.cpp | 330 |
| hybrid_receiver.hpp | 180 |
| multi_device_receiver.cpp | 220 |
| multi_device_receiver.hpp | 105 |
| tcp_video_receiver.cpp | 293 |
| tcp_video_receiver.hpp | 53 |
| usb_command_sender.cpp | 625 |
| usb_command_sender.hpp | 103 |
| hybrid_command_sender.cpp | 463 |
| hybrid_command_sender.hpp | 150 |
| multi_usb_command_sender.cpp | 917 |
| multi_usb_command_sender.hpp | 199 |
| wifi_command_sender.cpp | 399 |
| wifi_command_sender.hpp | 108 |
| adb_device_manager.cpp | 666 |
| adb_device_manager.hpp | 136 |
| adb_touch_fallback.cpp | 88 |
| adb_touch_fallback.hpp | 76 |
| device_registry.cpp | 299 |
| device_registry.hpp | 127 |
| aoa_hid_touch.cpp | 390 |
| aoa_hid_touch.hpp | 134 |
| aoa_protocol.cpp | 243 |
| usb_device_discovery.cpp | 124 |
| ipc_client.cpp | 94 |
| ipc_client.hpp | 27 |
| bandwidth_monitor.cpp | 133 |
| bandwidth_monitor.hpp | 79 |
| route_controller.cpp | 286 |
| route_controller.hpp | 112 |
| winusb_checker.cpp | 188 |
| winusb_checker.hpp | 75 |

### コンピュート — 1,497 行

| ファイル | 行数 |
|---|---:|
| vulkan_compute_processor.cpp | 237 |
| vulkan_compute_processor.hpp | 89 |
| vulkan_template_matcher.cpp | 1,024 |
| vulkan_template_matcher.hpp | 147 |

### ヘッダのみ (ユーティリティ) — 1,421 行

| ファイル | 行数 |
|---|---:|
| mirage_log.hpp | 90 |
| mirage_protocol.hpp | 191 |
| config_loader.hpp | 214 |
| event_bus.hpp | 192 |
| frame_dispatcher.hpp | 98 |
| auto_setup.hpp | 114 |
| rtt_tracker.hpp | 264 |
| adb_security.hpp | 161 |
| vid0_parser.hpp | 93 |
| stb_image_impl.cpp | 4 |

### shaders/ — 553 行

| ファイル | 行数 |
|---|---:|
| prefix_sum_horizontal.comp | 66 |
| prefix_sum_vertical.comp | 61 |
| pyramid_downsample.comp | 28 |
| rgba_to_gray.comp | 21 |
| template_match_ncc.comp | 174 |
| template_match_sat.comp | 126 |
| yuv_to_rgba.comp | 77 |

### tests/ — 757 行

| ファイル | 行数 |
|---|---:|
| test_e2e_decode.cpp | 378 |
| test_h264_parser.cpp | 228 |
| test_vulkan_video.cpp | 151 |

### CMakeLists.txt — 407 行

---

## 構成比 (C++ソースのみ: 23,589 行)

```
通信系         ████████████████████████████████████  37.0%  (8,711)
gui/           ███████████████████████▊              23.8%  (5,619)
video/         █████████████████████▍                21.3%  (5,036)
コンピュート   ██████▎                                6.3%  (1,497)
ヘッダのみ     ██████                                 6.0%  (1,421)
vulkan/        █████▌                                 5.5%  (1,305)
```
