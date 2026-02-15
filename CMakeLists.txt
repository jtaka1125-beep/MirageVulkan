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
)

# Video pipeline (Vulkan Video + FFmpeg fallback)
set(VIDEO_SOURCES
    src/video/vulkan_video_decoder.cpp
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
    src/gui/gui_command.cpp
    src/gui/gui_window.cpp
    src/gui/gui_threads.cpp
    src/gui/gui_device_control.cpp
    src/gui/gui_state.cpp
    src/gui/mirage_context.cpp
    src/gui_application.cpp
    src/gui_render.cpp
    src/gui_render_left_panel.cpp
    src/gui_render_main_view.cpp
    src/gui_render_dialogs.cpp
    src/gui_input.cpp
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
    src/adb_device_manager.cpp
    src/device_registry.cpp
    src/aoa_protocol.cpp
    src/aoa_hid_touch.cpp
    src/adb_touch_fallback.cpp
    src/usb_device_discovery.cpp
    src/bandwidth_monitor.cpp
    src/route_controller.cpp
    src/winusb_checker.cpp
    src/ipc_client.cpp
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
# Main executable
# =============================================================================
add_executable(mirage_vulkan WIN32
    ${CORE_SOURCES}
    ${VIDEO_SOURCES}
    ${VULKAN_SOURCES}
    ${GUI_SOURCES}
    ${COMM_SOURCES}
    ${IMGUI_SOURCES}
)

target_include_directories(mirage_vulkan PRIVATE
    src
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

    target_include_directories(mirage_vulkan PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_compile_definitions(mirage_vulkan PRIVATE USE_FFMPEG)
    target_link_directories(mirage_vulkan PRIVATE ${FFMPEG_LIBRARY_DIRS})
    list(APPEND MIRAGE_LIBS ${FFMPEG_LIBRARIES})

    message(STATUS "FFmpeg enabled: ${FFMPEG_LIBRARIES}")
else()
    message(STATUS "FFmpeg disabled - Vulkan Video only")
endif()

# libusb
if(USE_LIBUSB)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBUSB REQUIRED libusb-1.0)

    target_include_directories(mirage_vulkan PRIVATE ${LIBUSB_INCLUDE_DIRS})
    target_compile_definitions(mirage_vulkan PRIVATE USE_LIBUSB)
    target_link_directories(mirage_vulkan PRIVATE ${LIBUSB_LIBRARY_DIRS})
    list(APPEND MIRAGE_LIBS ${LIBUSB_LIBRARIES})

    message(STATUS "USB AOA enabled: libusb-1.0")
else()
    message(STATUS "USB AOA disabled")
endif()

# WASAPI audio
list(APPEND MIRAGE_LIBS ole32 uuid)

# Bluetooth
list(APPEND MIRAGE_LIBS bthprops)

target_link_libraries(mirage_vulkan PRIVATE ${MIRAGE_LIBS})

# =============================================================================
# Compiler settings
# =============================================================================
target_compile_definitions(mirage_vulkan PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    VK_USE_PLATFORM_WIN32_KHR
)

if(USE_VULKAN_VIDEO)
    target_compile_definitions(mirage_vulkan PRIVATE USE_VULKAN_VIDEO)
endif()

target_compile_options(mirage_vulkan PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8 /W4>
    $<$<CXX_COMPILER_ID:GNU>:-finput-charset=UTF-8 -Wall -Wextra>
)

# =============================================================================
# Debug version
# =============================================================================
add_executable(mirage_vulkan_debug
    ${CORE_SOURCES}
    ${VIDEO_SOURCES}
    ${VULKAN_SOURCES}
    ${GUI_SOURCES}
    ${COMM_SOURCES}
    ${IMGUI_SOURCES}
)

target_include_directories(mirage_vulkan_debug PRIVATE
    src
    src/gui
    src/video
    src/vulkan
    ${CMAKE_SOURCE_DIR}/third_party
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

if(USE_FFMPEG)
    target_include_directories(mirage_vulkan_debug PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_link_directories(mirage_vulkan_debug PRIVATE ${FFMPEG_LIBRARY_DIRS})
endif()

if(USE_LIBUSB)
    target_include_directories(mirage_vulkan_debug PRIVATE ${LIBUSB_INCLUDE_DIRS})
    target_link_directories(mirage_vulkan_debug PRIVATE ${LIBUSB_LIBRARY_DIRS})
endif()

target_link_libraries(mirage_vulkan_debug PRIVATE ${MIRAGE_LIBS})

target_compile_definitions(mirage_vulkan_debug PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    VK_USE_PLATFORM_WIN32_KHR
    _DEBUG
    $<$<BOOL:${USE_FFMPEG}>:USE_FFMPEG>
    $<$<BOOL:${USE_LIBUSB}>:USE_LIBUSB>
    $<$<BOOL:${USE_VULKAN_VIDEO}>:USE_VULKAN_VIDEO>
)

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

        enable_testing()
        add_test(NAME VulkanVideoTest COMMAND test_vulkan_video)
        add_test(NAME H264ParserTest COMMAND test_h264_parser)
        add_test(NAME E2EDecodeTest COMMAND test_e2e_decode)

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
message(STATUS "")
