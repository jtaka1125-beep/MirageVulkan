// =============================================================================
// MirageSystem v2 GUI - Main Entry Point (Refactored)
// =============================================================================
// Modular GUI system with separated concerns:
//   - gui_state.hpp/cpp    : Global state management
//   - gui_command.hpp/cpp  : Device command functions
//   - gui_window.hpp/cpp   : Window procedure
//   - gui_threads.hpp/cpp  : Background threads
//   - gui_device_control.hpp/cpp : AOA/ADB control
//   - gui_init.hpp/cpp     : Component initialization
// =============================================================================

#pragma comment(lib, "ws2_32.lib")

#include "mirage_log.hpp"
#include "mirage_config.hpp"
#include "gui_state.hpp"
#include "gui_window.hpp"
#include "gui_threads.hpp"
#include "gui_device_control.hpp"
#include "gui_ai_panel.hpp"
#include "gui_init.hpp"
#include "gui_command.hpp"
#include "config_loader.hpp"
#include "winusb_checker.hpp"

#include <thread>

using namespace mirage::gui::state;
using namespace mirage::gui::window;
using namespace mirage::gui::threads;
using namespace mirage::gui::device_control;
using namespace mirage::gui::ai_panel;
using namespace mirage::gui::init;
namespace cmd = mirage::gui::command;


// =============================================================================
// Main Entry Point
// =============================================================================

static LONG WINAPI mirageUnhandledExceptionFilter(EXCEPTION_POINTERS* ep) {
    try { if (mirage::ctx().macro_api_server) mirage::ctx().macro_api_server->stop(); } catch (...) {}
    WSACleanup();
    mirage::log::closeLogFile();
    // デバッグ用: EXCEPTION_CONTINUE_SEARCHでOSにレポート生成させる
    return EXCEPTION_CONTINUE_SEARCH;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/,
                   LPSTR /*lpCmdLine*/, int nCmdShow) {

    // SEH例外フィルタ登録 (クラッシュ時にソケットをクリーンアップ)
    SetUnhandledExceptionFilter(mirageUnhandledExceptionFilter);

    // Initialize Winsock
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    // Initialize configuration and open log file
    auto& sys_config = mirage::config::getSystemConfig();
    mirage::config::applyEnvironmentOverrides(sys_config);
    std::string log_path = sys_config.log_directory + "\\" + sys_config.log_filename;
    mirage::log::openLogFile(log_path.c_str());

    try {

    // Initialize state
    initializeState();

    // Start ADB detection
    std::thread adbThread(adbDetectionThread);
    MLOG_INFO("gui", "ADB検出待機中...");
    while (!g_adb_ready.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    adbThread.join();
    MLOG_INFO("gui", "ADB検出完了");

    // WinUSB driver check - diagnose driver issues before USB init
    int winusb_needs_count = 0;
    {
        auto usb_devices = mirage::WinUsbChecker::checkDevices();
        for (const auto& dev : usb_devices) {
            if (dev.needs_winusb) winusb_needs_count++;
        }
        if (winusb_needs_count > 0) {
            MLOG_WARN("gui", "WinUSB driver missing on %d device(s)! USB AOA will not work.", winusb_needs_count);
            MLOG_WARN("gui", "Commands will use ADB fallback. Video will use WiFi.");
            auto summary = mirage::WinUsbChecker::getDiagnosticSummary();
            MLOG_WARN("gui", "Driver status: %s", summary.c_str());
        } else if (!usb_devices.empty()) {
            MLOG_INFO("gui", "WinUSB driver check: all %d device(s) OK", (int)usb_devices.size());
        }
    }


    // Start WiFi ADB watchdog (joins at shutdown via g_running=false)
    std::thread watchdogThread(wifiAdbWatchdogThread);
    // watchdog joins at cleanup (no more detach)

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;  // Removed CS_HREDRAW|CS_VREDRAW - Vulkan handles all rendering
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"MirageSystemV2";
    
    if (!RegisterClassExW(&wc)) {
        MessageBoxW(nullptr, L"Failed to register window class", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    
    // Create window - adapt to actual screen resolution
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    MLOG_INFO("main", "Screen resolution: %dx%d", screenW, screenH);

    // Target 16:9 aspect ratio, fit within screen (leave room for taskbar)
    int clientW, clientH;
    if (screenW >= 1920 && screenH >= 1080) {
        // Full HD or larger: use 1920x1080
        clientW = 1920;
        clientH = 1080;
    } else {
        // Smaller screen: fit within available space
        int availH = screenH - 40;  // Reserve for taskbar
        clientW = screenW;
        clientH = static_cast<int>(clientW / (16.0f / 9.0f));
        if (clientH > availH) {
            clientH = availH;
            clientW = static_cast<int>(clientH * (16.0f / 9.0f));
        }
    }
    MLOG_INFO("main", "Window client size: %dx%d", clientW, clientH);

    RECT rect = {0, 0, clientW, clientH};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    // Center on screen
    int winW = rect.right - rect.left;
    int winH = rect.bottom - rect.top;
    int posX = (screenW - winW) / 2;
    int posY = (screenH - winH) / 2;
    if (posX < 0) posX = 0;
    if (posY < 0) posY = 0;

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"MirageSystem v2 - Control Panel",
        WS_OVERLAPPEDWINDOW,
        posX, posY,
        winW, winH,
        nullptr, nullptr, hInstance, nullptr
    );
    
    if (!hwnd) {
        MessageBoxW(nullptr, L"Failed to create window", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Initialize components
    // IMPORTANT: AOA switch MUST happen BEFORE MirageCapture startup.
    // AOA switching causes USB re-enumeration which kills ADB transport.
    // By initializing AOA first, devices are already in AOA mode when capture starts.
    initializeHybridCommand();
    (void)initializeMultiReceiver();
    MLOG_INFO("gui", "Receivers initialized");
    initializeRouting();

    // Initialize IPC (fallback)
    g_ipc = std::make_unique<::gui::MirageIpcClient>();
    g_ipc->connect();

    // Show window BEFORE Vulkan init (AMD driver requires visible window for surface)
    // Always show normal: schtasks passes nCmdShow=SW_HIDE which hides the window
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    // Initialize GUI
    initializeGUI(hwnd);

    // EventBus→CommandSenderパイプライン開始
    cmd::init();

    // Show deferred WinUSB warning in GUI (g_gui is now initialized)
    if (winusb_needs_count > 0 && g_gui) {
        g_gui->logWarning(u8"USB\u76f4\u63a5\u5236\u5fa1: WinUSB\u30c9\u30e9\u30a4\u30d0\u304c\u672a\u30a4\u30f3\u30b9\u30c8\u30fc\u30eb ("
            + std::to_string(winusb_needs_count) + u8"\u53f0) - [\u30c9\u30e9\u30a4\u30d0\u8a2d\u5b9a]\u30dc\u30bf\u30f3\u3067\u30a4\u30f3\u30b9\u30c8\u30fc\u30eb\u3057\u3066\u304f\u3060\u3055\u3044");
        g_gui->logInfo(u8"ADB\u30d5\u30a9\u30fc\u30eb\u30d0\u30c3\u30af\u3067\u64cd\u4f5c\u4e2d (\u30bf\u30c3\u30d7/\u30b9\u30ef\u30a4\u30d7\u306fADB\u7d4c\u7531)");
    }

#ifdef USE_AI
    initializeAI();
    mirage::gui::ai_panel::init();
#endif

#ifdef USE_OCR
    initializeOCR();
#endif

    // Start device update thread
    std::thread updateThread(deviceUpdateThread);
    UpdateWindow(hwnd);
    
    // Main loop
    MSG msg = {};
    while (g_running && g_gui->isRunning()) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) {
                g_running = false;
            }
        }

        if (!g_running) break;

        g_gui->processPendingFrames();
        g_gui->beginFrame();
        
        // Render device control panel (NEW - AOA/ADB buttons)
        renderDeviceControlPanel();

#ifdef USE_AI
        renderAIPanel();
#endif

        g_gui->render();
        g_gui->endFrame();
    }
    
    // Cleanup
    g_running = false;
    if (updateThread.joinable()) {
        updateThread.join();
    }
    
    // Join watchdog thread
    if (watchdogThread.joinable()) {
        watchdogThread.join();
        MLOG_INFO("gui", "Watchdog thread joined");
    }

    // Join route evaluation thread
    g_route_eval_running.store(false);
    if (g_route_eval_thread.joinable()) {
        g_route_eval_thread.join();
        MLOG_INFO("gui", "Route eval thread joined");
    }

#ifdef USE_AI
    mirage::gui::ai_panel::shutdown();
#endif

    // EventBusコマンド購読解除
    cmd::shutdown();

    cleanupState();

    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInstance);
    

    } catch (const std::exception& e) {
        MLOG_ERROR("gui", "FATAL unhandled exception: %s", e.what());
        MessageBoxA(nullptr, e.what(), "MirageSystem Fatal Error", MB_OK | MB_ICONERROR);
    } catch (...) {
        MLOG_ERROR("gui", "FATAL unknown exception");
        MessageBoxA(nullptr, "Unknown fatal error", "MirageSystem Fatal Error", MB_OK | MB_ICONERROR);
    }

    WSACleanup();
    
    mirage::log::closeLogFile();
    return 0;
}

#ifdef _CONSOLE
int main(int argc, char* argv[]) {
    // Initialize structured logging
    mirage::log::openLogFile(mirage::config::getConfig().log.log_path);
#ifdef _DEBUG
    mirage::log::setLogLevel(mirage::log::Level::Debug);
#endif
    MLOG_INFO("main", "MirageSystem v2.2 starting...");
    return WinMain(GetModuleHandle(nullptr), nullptr, GetCommandLineA(), SW_SHOWNORMAL);
}
#endif
