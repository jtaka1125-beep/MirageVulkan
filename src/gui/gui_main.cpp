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
    // WSACleanupのみ: macro_api_serverはWSA_FLAG_NO_HANDLE_INHERITで対策済み
    WSACleanup();
    mirage::log::closeLogFile();
    return EXCEPTION_CONTINUE_SEARCH;  // OSのクラッシュレポーターに渡す
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/,
                   LPSTR /*lpCmdLine*/, int nCmdShow) {

    // === SESSION1 DIAG: very first line of WinMain ===
    { FILE* _cf = fopen("C:\\MirageWork\\s1_diag2.log", "a");
      if (_cf) { fprintf(_cf, "[STEP1] WinMain entered\n"); fclose(_cf); } }

    // Windows timer resolution 1ms: prevents sleep_for(13ms) from sleeping 31ms

    // Windows timer resolution 1ms: prevents sleep_for(13ms) from sleeping 31ms

    // Windows timer: 1ms precision (default 15.6ms causes sleep_for(13ms) to sleep 31ms -> 22fps)
    { HMODULE h=LoadLibraryA("winmm.dll"); if(h){ typedef UINT(__stdcall*FP)(UINT); FP f=(FP)GetProcAddress(h,"timeBeginPeriod"); if(f)f(1); } }

    // Initialize Winsock
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
    { FILE* _cf = fopen("C:\\MirageWork\\s1_diag2.log", "a");
      if (_cf) { fprintf(_cf, "[STEP2] WSAStartup OK\n"); fclose(_cf); } }

    // Initialize configuration and open log file
    auto& sys_config = mirage::config::getSystemConfig();
    { FILE* _cf = fopen("C:\\MirageWork\\s1_diag2.log", "a");
      if (_cf) { fprintf(_cf, "[STEP3] getSystemConfig OK\n"); fclose(_cf); } }
    mirage::config::applyEnvironmentOverrides(sys_config);
    std::string log_path = sys_config.log_directory + "\\" + sys_config.log_filename;
    { FILE* _cf = fopen("C:\\MirageWork\\s1_diag2.log", "a");
      if (_cf) { fprintf(_cf, "[STEP4] logpath=%s\n", log_path.c_str()); fclose(_cf); } }
    mirage::log::startAsyncLogger();  // Start async log writer before any MLOG calls
    { FILE* _cf = fopen("C:\\MirageWork\\s1_diag2.log", "a");
      if (_cf) { fprintf(_cf, "[STEP5] startAsyncLogger OK\n"); fclose(_cf); } }
    mirage::log::openLogFile(log_path.c_str());
    { FILE* _cf = fopen("C:\\MirageWork\\s1_diag2.log", "a");
      if (_cf) { fprintf(_cf, "[STEP6] openLogFile OK\n"); fclose(_cf); } }

    try {

    // Initialize state
    // Debug: log startup progress to crash file (helps diagnose Session 1 failures)
    if (FILE* cf = fopen("C:\\MirageWork\\s1_crash.log", "a")) {
        fprintf(cf, "WinMain started, about to initializeState\n"); fclose(cf);
    }
    initializeState();
    if (FILE* cf = fopen("C:\\MirageWork\\s1_crash.log", "a")) {
        fprintf(cf, "initializeState OK\n"); fclose(cf);
    }

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
    // Force window to foreground and process messages so DWM registers it
    SetForegroundWindow(hwnd);
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    { MSG msg; for(int i=0;i<20;i++){ while(PeekMessage(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessage(&msg);} Sleep(50); } }
    MLOG_INFO("gui", "Window visible, DWM notified, proceeding to Vulkan init");

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

#ifdef MIRAGE_OCR_ENABLED
    initializeOCR();
#endif

#if defined(USE_AI) && defined(MIRAGE_OCR_ENABLED)
    // FrameAnalyzer -> AIEngine 接続 (OCRフォールバック有効化)
    if (g_ai_engine) {
        g_ai_engine->setFrameAnalyzer(&mirage::analyzer());
        MLOG_INFO("gui", "FrameAnalyzer -> AIEngine connected (OCR fallback enabled)");
        if (g_gui) g_gui->logInfo(u8"OCR -> AIEngine connected (OCR fallback enabled)");
    }
#endif

    // Start device update thread
    std::thread updateThread(deviceUpdateThread);
    UpdateWindow(hwnd);
    
    // Main loop - capped at TARGET_FPS to prevent CPU spinning
    static constexpr int TARGET_FPS = 60;  // Cap render loop at 60 FPS
    static constexpr auto FRAME_BUDGET = std::chrono::microseconds(1000000 / TARGET_FPS);

    MSG msg = {};
    while (g_running && g_gui->isRunning()) {
        auto frame_start = std::chrono::steady_clock::now();

        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) {
                g_running = false;
            }
        }

        if (!g_running) break;

        auto t0 = std::chrono::steady_clock::now();
        g_gui->processPendingFrames();
        auto t1 = std::chrono::steady_clock::now();
        g_gui->beginFrame();
        auto t2 = std::chrono::steady_clock::now();
        
        // Render device control panel (NEW - AOA/ADB buttons)
        renderDeviceControlPanel();

#ifdef USE_AI
        renderAIPanel();
#endif

        g_gui->render();
        auto t3 = std::chrono::steady_clock::now();
        g_gui->endFrame();
        auto t4 = std::chrono::steady_clock::now();

        // Frame cap: sleep remaining budget to limit CPU usage
        auto elapsed = std::chrono::steady_clock::now() - frame_start;
        if (elapsed < FRAME_BUDGET) {
            std::this_thread::sleep_for(FRAME_BUDGET - elapsed);
        }
        auto t5 = std::chrono::steady_clock::now();
        static std::atomic<int> ml_cnt{0};
        int ml = ml_cnt.fetch_add(1);
        if (ml < 20 || ml % 300 == 0) {
            auto us = [](auto a, auto b){ return (long long)std::chrono::duration_cast<std::chrono::microseconds>(b-a).count(); };
            MLOG_INFO("mainloop", "#%d ppf=%lld beginF=%lld render=%lld endF=%lld sleep=%lld total=%lld us",
                ml, us(t0,t1), us(t1,t2), us(t2,t3), us(t3,t4), us(t4,t5), us(frame_start,t5));
        }
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
        // Write to crash log (absolute path, works even if log system failed to init)
        if (FILE* cf = fopen("C:\\MirageWork\\s1_crash.log", "a")) {
            fprintf(cf, "EXCEPTION: %s\n", e.what()); fclose(cf);
        }
        MessageBoxA(nullptr, e.what(), "MirageSystem Fatal Error", MB_OK | MB_ICONERROR);
    } catch (...) {
        MLOG_ERROR("gui", "FATAL unknown exception");
        if (FILE* cf = fopen("C:\\MirageWork\\s1_crash.log", "a")) {
            fprintf(cf, "UNKNOWN_EXCEPTION\n"); fclose(cf);
        }
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
