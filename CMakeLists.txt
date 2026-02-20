# =============================================================================
# MirageVulkan - Vulkan Video Unified Pipeline
# =============================================================================
# Features:
#   - Vulkan Video H.264 decode (zero-copy GPU pipeline)
#   - FFmpeg fallback (D3D11VA HW accel)
#   - YUV→RGBA compute shader conversion
#   - ImGui + Vulkan rendering
#   - Multi-device Android mirroring
# =============================================================================

cmake_minimum_required(VERSION 3.20)
project(mirage_vulkan LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# =============================================================================
# Options
# =============================================================================
option(USE_VULKAN_VIDEO "Enable Vulkan Video H.264 decode" ON)
option(USE_FFMPEG "Enable FFmpeg fallback decoder" ON)
option(USE_LIBUSB "Enable USB AOA video receiver" ON)
option(BUILD_TESTS "Build test executables" ON)
option(USE_OCR "Enable Tesseract OCR frame analysis" OFF)
option(MIRAGE_OCR_ENABLED "Enable OCR integration (alias for USE_OCR)" OFF)
option(USE_AI "Enable AI engine (template matching + OCR integration)" OFF)

# =============================================================================
# Find Vulkan
# =============================================================================
find_package(Vulkan REQUIRED)

if(Vulkan_FOUND)
    message(STATUS "Vulkan SDK: ${Vulkan_INCLUDE_DIR}")
    message(STATUS "Vulkan Library: ${Vulkan_LIBRARY}")
endif()

# =============================================================================
# Third party
# =============================================================================
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)

if(NOT EXISTS "${IMGUI_DIR}/imgui.h")
    message(FATAL_ERROR "ImGui not found at ${IMGUI_DIR}")
endif()

# =============================================================================
# Source files
# =============================================================================

# Core infrastructure
set(CORE_SOURCES
    src/mirage_log.hpp
    src/mirage_protocol.hpp
    src/config_loader.hpp
    src/event_bus.hpp
    src/frame_dispatcher.hpp
    src/stb_image_impl.cpp
    src/ai_engine.cpp
    src/ai_engine.hpp
)

# AI / Learning / Template Management
set(AI_SOURCES
    src/ai/learning_mode.cpp
    src/ai/learning_mode.hpp
    src/ai/template_manifest.cpp
    src/ai/template_manifest.hpp
    src/ai/template_store.cpp
    src/ai/template_store.hpp
    src/ai/template_capture.cpp
    src/ai/template_capture.hpp
    src/ai/template_writer.cpp
    src/ai/template_writer.hpp
    src/ai/template_autoscan.cpp
    src/ai/template_autoscan.hpp
    src/ai/template_hot_reload.cpp
    src/ai/template_hot_reload.hpp
    src/ai/vision_decision_engine.cpp
    src/ai/vision_decision_engine.hpp
)

# Video pipeline (Vulkan Video + FFmpeg fallback)
set(VIDEO_SOURCES
    src/video/vulkan_video_decoder.cpp
    src/video/vulkan_video_session.cpp
    src/video/vulkan_video_dpb.cpp
    src/video/vulkan_video_decode_impl.cpp
    src/video/vulkan_video_decoder.hpp
    src/video/h264_parser.cpp
    src/video/h264_parser.hpp
    src/video/yuv_converter.cpp
    src/video/yuv_converter.hpp
    src/video/unified_decoder.cpp
    src/video/unified_decoder.hpp
)

# FFmpeg fallback decoder
if(USE_FFMPEG)
    list(APPEND VIDEO_SOURCES
        src/video/h264_decoder.cpp
        src/video/h264_decoder.hpp
    )
endif()

# Vulkan infrastructure
set(VULKAN_SOURCES
    src/vulkan/vulkan_context.cpp
    src/vulkan/vulkan_context.hpp
    src/vulkan/vulkan_swapchain.cpp
    src/vulkan/vulkan_swapchain.hpp
    src/vulkan/vulkan_texture.cpp
    src/vulkan/vulkan_texture.hpp
    src/vulkan/vulkan_compute.cpp
    src/vulkan/vulkan_compute.hpp
    src/vulkan/vulkan_image.cpp
    src/vulkan/vulkan_image.hpp
    src/vulkan_compute_processor.cpp
    src/vulkan_compute_processor.hpp
    src/vulkan_template_matcher.cpp
    src/vulkan_template_matcher.hpp
)

# GUI
set(GUI_SOURCES
    src/gui/gui_main.cpp
    src/gui/gui_init.cpp
    src/gui/gui_command.cpp
    src/gui/gui_window.cpp
    src/gui/gui_threads.cpp
    src/gui/gui_device_control.cpp
    src/gui/gui_ai_panel.cpp
    src/gui/gui_state.cpp
    src/gui/mirage_context.cpp
    src/gui_application.cpp
    src/gui_render.cpp
    src/gui_render_left_panel.cpp
    src/gui_render_main_view.cpp
    src/gui_render_dialogs.cpp
    src/gui_input.cpp
    src/device_transform.cpp
)

# Communication
set(COMM_SOURCES
    src/usb_video_receiver.cpp
    src/usb_command_sender.cpp
    src/multi_usb_command_sender.cpp
    src/hybrid_command_sender.cpp
    src/wifi_command_sender.cpp
    src/mirror_receiver.cpp
    src/hybrid_receiver.cpp
    src/multi_device_receiver.cpp
    src/tcp_video_receiver.cpp
    src/macro_api_server.cpp
    src/adb_device_manager.cpp
    src/device_registry.cpp
    src/aoa_protocol.cpp
    src/aoa_hid_touch.cpp
    src/adb_touch_fallback.cpp
    src/usb_device_discovery.cpp
    src/usb_command_api.cpp
    src/bandwidth_monitor.cpp
    src/route_controller.cpp
    src/winusb_checker.cpp
    src/ipc_client.cpp
    src/ai/ui_finder.cpp
    src/ai/ui_finder.hpp
    src/ui_finder.hpp
)

# ImGui
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
)

# =============================================================================
# Object Library (shared between Release and Debug)
# =============================================================================
# This avoids compiling the same sources twice for Release/Debug builds

add_library(mirage_core OBJECT
    ${CORE_SOURCES}
    ${VIDEO_SOURCES}
    ${VULKAN_SOURCES}
    ${GUI_SOURCES}
    ${COMM_SOURCES}
    ${IMGUI_SOURCES}
)

target_include_directories(mirage_core PUBLIC
    src
    src/ai
    src/gui
    src/video
    src/vulkan
    ${CMAKE_SOURCE_DIR}/third_party
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

# =============================================================================
# Main executable (Release)
# =============================================================================
add_executable(mirage_vulkan WIN32 $<TARGET_OBJECTS:mirage_core>)

target_include_directories(mirage_vulkan PRIVATE
    src
    src/ai
    src/gui
    src/video
    src/vulkan
    ${CMAKE_SOURCE_DIR}/third_party
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

# =============================================================================
# Libraries
# =============================================================================
set(MIRAGE_LIBS
    Vulkan::Vulkan
    dwmapi
    ws2_32
)

# FFmpeg
if(USE_FFMPEG)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED libavcodec libavutil libswscale)

    target_include_directories(mirage_core PUBLIC ${FFMPEG_INCLUDE_DIRS})
    target_compile_definitions(mirage_core PUBLIC USE_FFMPEG)
    target_link_directories(mirage_core PUBLIC ${FFMPEG_LIBRARY_DIRS})
    list(APPEND MIRAGE_LIBS ${FFMPEG_LIBRARIES})

    message(STATUS "FFmpeg enabled: ${FFMPEG_LIBRARIES}")
else()
    message(STATUS "FFmpeg disabled - Vulkan Video only")
endif()

# libusb
if(USE_LIBUSB)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBUSB REQUIRED libusb-1.0)

    target_include_directories(mirage_core PUBLIC ${LIBUSB_INCLUDE_DIRS})
    target_compile_definitions(mirage_core PUBLIC USE_LIBUSB)
    target_link_directories(mirage_core PUBLIC ${LIBUSB_LIBRARY_DIRS})
    list(APPEND MIRAGE_LIBS ${LIBUSB_LIBRARIES})

    message(STATUS "USB AOA enabled: libusb-1.0")
else()
    message(STATUS "USB AOA disabled")
endif()

# WASAPI audio
list(APPEND MIRAGE_LIBS ole32 uuid)

# Bluetooth
list(APPEND MIRAGE_LIBS bthprops)

# Tesseract OCR
if(USE_OCR)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(TESSERACT REQUIRED tesseract)
    pkg_check_modules(LEPTONICA REQUIRED lept)

    target_sources(mirage_core PRIVATE src/frame_analyzer.cpp)
    target_include_directories(mirage_core PRIVATE ${TESSERACT_INCLUDE_DIRS} ${LEPTONICA_INCLUDE_DIRS})
    target_link_directories(mirage_core PUBLIC ${TESSERACT_LIBRARY_DIRS} ${LEPTONICA_LIBRARY_DIRS})
    target_compile_definitions(mirage_core PRIVATE MIRAGE_OCR_ENABLED=1)
    list(APPEND MIRAGE_LIBS ${TESSERACT_LIBRARIES} ${LEPTONICA_LIBRARIES})

    message(STATUS "Tesseract OCR enabled: ${TESSERACT_LIBRARIES}")
else()
    message(STATUS "Tesseract OCR disabled")
endif()

# MIRAGE_OCR_ENABLED alias → USE_OCRと同期
if(MIRAGE_OCR_ENABLED AND NOT USE_OCR)
    message(STATUS "MIRAGE_OCR_ENABLED=ON → USE_OCR also enabled")
    set(USE_OCR ON CACHE BOOL "" FORCE)
endif()

# AI Engine
if(USE_AI)
    target_sources(mirage_core PRIVATE ${AI_SOURCES})
    target_compile_definitions(mirage_core PRIVATE USE_AI)
    message(STATUS "AI Engine enabled")
else()
    message(STATUS "AI Engine disabled (stub mode)")
endif()

# OBJECT ライブラリは target_link_directories の PUBLIC 伝播をサポートしないため
# 消費側ターゲットに直接設定する必要がある
if(USE_FFMPEG)
    target_link_directories(mirage_vulkan PRIVATE ${FFMPEG_LIBRARY_DIRS})
endif()
if(USE_LIBUSB)
    target_link_directories(mirage_vulkan PRIVATE ${LIBUSB_LIBRARY_DIRS})
endif()
if(USE_OCR)
    target_link_directories(mirage_vulkan PRIVATE ${TESSERACT_LIBRARY_DIRS} ${LEPTONICA_LIBRARY_DIRS})
endif()

target_link_libraries(mirage_vulkan PRIVATE ${MIRAGE_LIBS})

# =============================================================================
# Compiler settings (apply to object library)
# =============================================================================
target_compile_definitions(mirage_core PUBLIC
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    VK_USE_PLATFORM_WIN32_KHR
)

if(USE_VULKAN_VIDEO)
    target_compile_definitions(mirage_core PUBLIC USE_VULKAN_VIDEO)
endif()

target_compile_options(mirage_core PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /W4>
    $<$<CXX_COMPILER_ID:GNU>:-finput-charset=UTF-8 -Wall -Wextra>
)

target_link_libraries(mirage_core PUBLIC Vulkan::Vulkan)

# =============================================================================
# Debug version (reuses object library)
# =============================================================================
add_executable(mirage_vulkan_debug $<TARGET_OBJECTS:mirage_core>)

target_include_directories(mirage_vulkan_debug PRIVATE
    src
    src/ai
    src/gui
    src/video
    src/vulkan
    ${CMAKE_SOURCE_DIR}/third_party
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

# OBJECT ライブラリは target_link_directories の PUBLIC 伝播をサポートしないため
# 消費側ターゲットに直接設定する必要がある
if(USE_FFMPEG)
    target_link_directories(mirage_vulkan_debug PRIVATE ${FFMPEG_LIBRARY_DIRS})
endif()
if(USE_LIBUSB)
    target_link_directories(mirage_vulkan_debug PRIVATE ${LIBUSB_LIBRARY_DIRS})
endif()
if(USE_OCR)
    target_link_directories(mirage_vulkan_debug PRIVATE ${TESSERACT_LIBRARY_DIRS} ${LEPTONICA_LIBRARY_DIRS})
endif()

target_link_libraries(mirage_vulkan_debug PRIVATE ${MIRAGE_LIBS})

# Debug-specific definitions only
target_compile_definitions(mirage_vulkan_debug PRIVATE _DEBUG)

# =============================================================================
# Tests
# =============================================================================
if(BUILD_TESTS)
    set(GTEST_DIR ${CMAKE_SOURCE_DIR}/third_party/googletest/googletest-1.14.0)

    if(EXISTS "${GTEST_DIR}/googletest/src/gtest-all.cc")
        add_library(gtest STATIC
            ${GTEST_DIR}/googletest/src/gtest-all.cc
            ${GTEST_DIR}/googletest/src/gtest_main.cc
        )
        target_include_directories(gtest PUBLIC
            ${GTEST_DIR}/googletest/include
            ${GTEST_DIR}/googletest
        )

        # Vulkan Video decoder test
        add_executable(test_vulkan_video
            tests/test_vulkan_video.cpp
            src/video/vulkan_video_decoder.cpp
            src/video/vulkan_video_session.cpp
            src/video/vulkan_video_dpb.cpp
            src/video/vulkan_video_decode_impl.cpp
            src/video/h264_parser.cpp
            src/video/yuv_converter.cpp
            src/vulkan/vulkan_context.cpp
            src/vulkan/vulkan_compute.cpp
            src/vulkan/vulkan_image.cpp
        )
        target_include_directories(test_vulkan_video PRIVATE
            src src/video src/vulkan
        )
        target_link_libraries(test_vulkan_video PRIVATE gtest Vulkan::Vulkan)
        target_compile_definitions(test_vulkan_video PRIVATE
            WIN32_LEAN_AND_MEAN NOMINMAX VK_USE_PLATFORM_WIN32_KHR
        )

        # H264 parser test
        add_executable(test_h264_parser
            tests/test_h264_parser.cpp
            src/video/h264_parser.cpp
        )
        target_include_directories(test_h264_parser PRIVATE src src/video)
        target_link_libraries(test_h264_parser PRIVATE gtest)
        target_compile_definitions(test_h264_parser PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

        # E2E decode test
        add_executable(test_e2e_decode
            tests/test_e2e_decode.cpp
            src/video/vulkan_video_decoder.cpp
            src/video/vulkan_video_session.cpp
            src/video/vulkan_video_dpb.cpp
            src/video/vulkan_video_decode_impl.cpp
            src/video/h264_parser.cpp
            src/video/yuv_converter.cpp
            src/vulkan/vulkan_context.cpp
            src/vulkan/vulkan_compute.cpp
            src/vulkan/vulkan_image.cpp
        )
        target_include_directories(test_e2e_decode PRIVATE src src/video src/vulkan)
        target_link_libraries(test_e2e_decode PRIVATE gtest Vulkan::Vulkan)
        target_compile_definitions(test_e2e_decode PRIVATE
            WIN32_LEAN_AND_MEAN NOMINMAX VK_USE_PLATFORM_WIN32_KHR
        )

        # EventBus test
        add_executable(test_event_bus
            tests/test_event_bus.cpp
        )
        target_include_directories(test_event_bus PRIVATE src)
        target_link_libraries(test_event_bus PRIVATE gtest)
        target_compile_definitions(test_event_bus PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

        # VID0 parser test
        add_executable(test_vid0_parser
            tests/test_vid0_parser.cpp
        )
        target_include_directories(test_vid0_parser PRIVATE src)
        target_link_libraries(test_vid0_parser PRIVATE gtest)
        target_compile_definitions(test_vid0_parser PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

        # ADB security test
        add_executable(test_adb_security
            tests/test_adb_security.cpp
        )
        target_include_directories(test_adb_security PRIVATE src)
        target_link_libraries(test_adb_security PRIVATE gtest)
        target_compile_definitions(test_adb_security PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

        # AOA HID touch test (no libusb required)
        add_executable(test_aoa_hid
            tests/test_aoa_hid.cpp
            src/aoa_hid_touch.cpp
        )
        target_include_directories(test_aoa_hid PRIVATE src)
        target_link_libraries(test_aoa_hid PRIVATE gtest)
        target_compile_definitions(test_aoa_hid PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

        # MIRA protocol test (header-only)
        add_executable(test_mira_protocol
            tests/test_mira_protocol.cpp
        )
        target_include_directories(test_mira_protocol PRIVATE src)
        target_link_libraries(test_mira_protocol PRIVATE gtest)
        target_compile_definitions(test_mira_protocol PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

        # FrameDispatcher test (header-only)
        add_executable(test_frame_dispatcher
            tests/test_frame_dispatcher.cpp
        )
        target_include_directories(test_frame_dispatcher PRIVATE src)
        target_link_libraries(test_frame_dispatcher PRIVATE gtest)
        target_compile_definitions(test_frame_dispatcher PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

        # RTT/Bandwidth monitor test (requires bandwidth_monitor.cpp)
        add_executable(test_rtt_bandwidth
            tests/test_rtt_bandwidth.cpp
            src/bandwidth_monitor.cpp
        )
        target_include_directories(test_rtt_bandwidth PRIVATE src)
        target_link_libraries(test_rtt_bandwidth PRIVATE gtest)
        target_compile_definitions(test_rtt_bandwidth PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

        # Mirror receiver RTP parsing test (header-only)
        add_executable(test_mirror_receiver
            tests/test_mirror_receiver.cpp
        )
        target_include_directories(test_mirror_receiver PRIVATE src)
        target_link_libraries(test_mirror_receiver PRIVATE gtest)
        target_compile_definitions(test_mirror_receiver PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

        # Hybrid command sender test (header-only, uses mirage_protocol.hpp)
        add_executable(test_hybrid_sender
            tests/test_hybrid_sender.cpp
        )
        target_include_directories(test_hybrid_sender PRIVATE src)
        target_link_libraries(test_hybrid_sender PRIVATE gtest)
        target_compile_definitions(test_hybrid_sender PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

        # GUI logic test (headless, no Vulkan dependencies)
        add_executable(test_gui_logic
            tests/test_gui_logic.cpp
        )
        target_include_directories(test_gui_logic PRIVATE src)
        target_link_libraries(test_gui_logic PRIVATE gtest)
        target_compile_definitions(test_gui_logic PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

        # Result type test (header-only)
        add_executable(test_result
            tests/test_result.cpp
        )
        target_include_directories(test_result PRIVATE src)
        target_link_libraries(test_result PRIVATE gtest)
        target_compile_definitions(test_result PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

        # Route controller test (transport layer)
        add_executable(test_route_controller
            tests/test_route_controller.cpp
            src/route_controller.cpp
            src/bandwidth_monitor.cpp
        )
        target_include_directories(test_route_controller PRIVATE src)
        target_link_libraries(test_route_controller PRIVATE gtest)
        target_compile_definitions(test_route_controller PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

        # ADB device manager logic test (no actual ADB execution)
        add_executable(test_adb_device_manager
            tests/test_adb_device_manager.cpp
        )
        target_include_directories(test_adb_device_manager PRIVATE src)
        target_link_libraries(test_adb_device_manager PRIVATE gtest)
        target_compile_definitions(test_adb_device_manager PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

        # USB command packet building test
        add_executable(test_usb_command
            tests/test_usb_command.cpp
        )
        target_include_directories(test_usb_command PRIVATE src)
        target_link_libraries(test_usb_command PRIVATE gtest)
        target_compile_definitions(test_usb_command PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

        # TemplateManifest test (CPU only, no GPU)
        add_executable(test_template_manifest
            tests/test_template_manifest.cpp
            src/ai/template_manifest.cpp
        )
        target_include_directories(test_template_manifest PRIVATE src)
        target_link_libraries(test_template_manifest PRIVATE gtest)
        target_compile_definitions(test_template_manifest PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

        # TemplateCapture test (CPU only, RGBA→Gray8 conversion)
        add_executable(test_template_capture
            tests/test_template_capture.cpp
            src/ai/template_capture.cpp
        )
        target_include_directories(test_template_capture PRIVATE src)
        target_link_libraries(test_template_capture PRIVATE gtest)
        target_compile_definitions(test_template_capture PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

        # UiFinder XML parse test (CPU only, mock ADB)
        add_executable(test_ui_finder_parse
            tests/test_ui_finder_parse.cpp
            src/ai/ui_finder.cpp
        )
        target_include_directories(test_ui_finder_parse PRIVATE src)
        target_link_libraries(test_ui_finder_parse PRIVATE gtest)
        target_compile_definitions(test_ui_finder_parse PRIVATE
            WIN32_LEAN_AND_MEAN NOMINMAX VK_USE_PLATFORM_WIN32_KHR
        )

        # ActionMapper test (CPU only, header-only)
        add_executable(test_action_mapper
            tests/test_action_mapper.cpp
        )
        target_include_directories(test_action_mapper PRIVATE src)
        target_link_libraries(test_action_mapper PRIVATE gtest)
        target_compile_definitions(test_action_mapper PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

        # VisionDecisionEngine test (CPU only, 状態遷移ロジック)
        add_executable(test_vision_decision_engine
            tests/test_vision_decision_engine.cpp
            src/ai/vision_decision_engine.cpp
        )
        target_include_directories(test_vision_decision_engine PRIVATE src src/ai)
        target_link_libraries(test_vision_decision_engine PRIVATE gtest)
        target_compile_definitions(test_vision_decision_engine PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

        enable_testing()
        add_test(NAME VulkanVideoTest COMMAND test_vulkan_video)
        add_test(NAME H264ParserTest COMMAND test_h264_parser)
        add_test(NAME E2EDecodeTest COMMAND test_e2e_decode)
        add_test(NAME EventBusTest COMMAND test_event_bus)
        add_test(NAME Vid0ParserTest COMMAND test_vid0_parser)
        add_test(NAME AdbSecurityTest COMMAND test_adb_security)
        add_test(NAME AoaHidTest COMMAND test_aoa_hid)
        add_test(NAME MiraProtocolTest COMMAND test_mira_protocol)
        add_test(NAME FrameDispatcherTest COMMAND test_frame_dispatcher)
        add_test(NAME RttBandwidthTest COMMAND test_rtt_bandwidth)
        add_test(NAME MirrorReceiverTest COMMAND test_mirror_receiver)
        add_test(NAME HybridSenderTest COMMAND test_hybrid_sender)
        add_test(NAME GuiLogicTest COMMAND test_gui_logic)
        add_test(NAME ResultTest COMMAND test_result)
        add_test(NAME RouteControllerTest COMMAND test_route_controller)
        add_test(NAME AdbDeviceManagerTest COMMAND test_adb_device_manager)
        add_test(NAME UsbCommandTest COMMAND test_usb_command)
        add_test(NAME TemplateManifestTest COMMAND test_template_manifest)
        add_test(NAME TemplateCaptureTest COMMAND test_template_capture)
        add_test(NAME UiFinderParseTest COMMAND test_ui_finder_parse)
        add_test(NAME ActionMapperTest COMMAND test_action_mapper)
        add_test(NAME VisionDecisionEngineTest COMMAND test_vision_decision_engine)

        # FrameAnalyzer CPUロジック ユニットテスト (Tesseract不要)
        add_executable(test_frame_analyzer_unit
            tests/test_frame_analyzer_unit.cpp
        )
        target_include_directories(test_frame_analyzer_unit PRIVATE src)
        target_link_libraries(test_frame_analyzer_unit PRIVATE gtest)
        target_compile_definitions(test_frame_analyzer_unit PRIVATE
            WIN32_LEAN_AND_MEAN NOMINMAX MIRAGE_OCR_ENABLED=1
        )
        add_test(NAME FrameAnalyzerUnitTest COMMAND test_frame_analyzer_unit)

        # AI E2E統合テスト (GPU不要、モック使用)
        add_executable(test_ai_e2e
            tests/test_ai_e2e.cpp
            src/ai/template_capture.cpp
            src/ai/template_store.cpp
            src/stb_image_impl.cpp
        )
        target_include_directories(test_ai_e2e PRIVATE src)
        target_link_libraries(test_ai_e2e PRIVATE gtest)
        target_compile_definitions(test_ai_e2e PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        add_test(NAME AIE2ETest COMMAND test_ai_e2e)

        # Frame Analyzer OCR test
        if(USE_OCR)
            add_executable(test_frame_analyzer
                tests/test_frame_analyzer.cpp
                src/frame_analyzer.cpp
            )
            target_include_directories(test_frame_analyzer PRIVATE src)
            target_link_libraries(test_frame_analyzer PRIVATE
                gtest ${TESSERACT_LIBRARIES} ${LEPTONICA_LIBRARIES}
            )
            target_link_directories(test_frame_analyzer PRIVATE
                ${TESSERACT_LIBRARY_DIRS} ${LEPTONICA_LIBRARY_DIRS}
            )
            target_compile_definitions(test_frame_analyzer PRIVATE
                WIN32_LEAN_AND_MEAN NOMINMAX MIRAGE_OCR_ENABLED=1
            )
            target_include_directories(test_frame_analyzer PRIVATE
                ${TESSERACT_INCLUDE_DIRS} ${LEPTONICA_INCLUDE_DIRS}
            )
            add_test(NAME FrameAnalyzerTest COMMAND test_frame_analyzer)
        endif()

        message(STATUS "Tests enabled")
    else()
        message(WARNING "GoogleTest not found, tests disabled")
    endif()
endif()

# =============================================================================
# Shader compilation (requires glslc from Vulkan SDK)
# =============================================================================
find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin")

if(GLSLC)
    # List all compute shaders
    set(SHADER_SOURCES
        yuv_to_rgba.comp
        rgba_to_gray.comp
        prefix_sum_horizontal.comp
        prefix_sum_vertical.comp
        pyramid_downsample.comp
        template_match_ncc.comp
        template_match_sat.comp
    )

    set(SHADER_OUTPUTS)

    foreach(SHADER ${SHADER_SOURCES})
        set(SHADER_SOURCE ${CMAKE_SOURCE_DIR}/shaders/${SHADER})
        set(SHADER_OUTPUT ${CMAKE_BINARY_DIR}/shaders/${SHADER}.spv)

        add_custom_command(
            OUTPUT ${SHADER_OUTPUT}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
            COMMAND ${GLSLC} -O ${SHADER_SOURCE} -o ${SHADER_OUTPUT}
            DEPENDS ${SHADER_SOURCE}
            COMMENT "Compiling shader: ${SHADER}"
        )

        list(APPEND SHADER_OUTPUTS ${SHADER_OUTPUT})
    endforeach()

    add_custom_target(shaders DEPENDS ${SHADER_OUTPUTS})
    add_dependencies(mirage_vulkan shaders)

    message(STATUS "Shader compiler: ${GLSLC}")
    message(STATUS "Shaders to compile: ${SHADER_SOURCES}")
else()
    message(WARNING "glslc not found - shaders must be compiled manually")
endif()

# =============================================================================
# Install
# =============================================================================
install(TARGETS mirage_vulkan DESTINATION bin)
install(DIRECTORY ${CMAKE_BINARY_DIR}/shaders/ DESTINATION bin/shaders OPTIONAL FILES_MATCHING PATTERN "*.spv")

message(STATUS "")
message(STATUS "=== MirageVulkan Configuration ===")
message(STATUS "  Vulkan Video: ${USE_VULKAN_VIDEO}")
message(STATUS "  FFmpeg:       ${USE_FFMPEG}")
message(STATUS "  USB AOA:      ${USE_LIBUSB}")
message(STATUS "  Tests:        ${BUILD_TESTS}")
message(STATUS "  OCR:          ${USE_OCR}")
message(STATUS "")
