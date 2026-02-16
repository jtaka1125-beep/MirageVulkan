#include "mirage_log.hpp"
// =============================================================================
// MirageSystem v2 GUI - Window Procedure
// =============================================================================
#include "gui_window.hpp"
#include "gui_state.hpp"

#include <windowsx.h>
#include "imgui.h"
#include "imgui_impl_win32.h"

// Forward declaration for ImGui
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace mirage::gui::window {

using namespace mirage::gui::state;

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
        return true;
    }

    switch (msg) {
        // Prevent GDI from erasing background - Vulkan handles all rendering
        case WM_ERASEBKGND:
            return 1;  // Tell Windows we handled it (prevents white flash)

        case WM_PAINT: {
            // Minimal paint handler - Vulkan renders via swapchain, not GDI
            PAINTSTRUCT ps;
            BeginPaint(hWnd, &ps);
            EndPaint(hWnd, &ps);
            return 0;
        }

        case WM_SIZING: {
            // Enforce aspect ratio during resize
            RECT* rect = reinterpret_cast<RECT*>(lParam);
            int width = rect->right - rect->left;
            int height = rect->bottom - rect->top;

            // Get window border sizes
            RECT windowRect = {0, 0, 100, 100};
            AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);
            int borderW = (windowRect.right - windowRect.left) - 100;
            int borderH = (windowRect.bottom - windowRect.top) - 100;

            int clientW = width - borderW;
            int clientH = height - borderH;

            // Adjust based on which edge is being dragged
            switch (wParam) {
                case WMSZ_LEFT:
                case WMSZ_RIGHT:
                case WMSZ_BOTTOMLEFT:
                case WMSZ_BOTTOMRIGHT:
                    // Width changed, adjust height
                    clientH = static_cast<int>(clientW / ASPECT_RATIO);
                    break;
                case WMSZ_TOP:
                case WMSZ_BOTTOM:
                    // Height changed, adjust width
                    clientW = static_cast<int>(clientH * ASPECT_RATIO);
                    break;
                case WMSZ_TOPLEFT:
                case WMSZ_TOPRIGHT:
                    // Top edge changed, adjust width
                    clientW = static_cast<int>(clientH * ASPECT_RATIO);
                    break;
            }

            // Apply new size
            width = clientW + borderW;
            height = clientH + borderH;

            // For top/bottom only drags, expand width from center
            if (wParam == WMSZ_TOP || wParam == WMSZ_BOTTOM) {
                int oldWidth = rect->right - rect->left;
                int widthDiff = width - oldWidth;
                rect->left -= widthDiff / 2;
                rect->right = rect->left + width;
            } else {
                switch (wParam) {
                    case WMSZ_LEFT:
                    case WMSZ_TOPLEFT:
                    case WMSZ_BOTTOMLEFT:
                        rect->left = rect->right - width;
                        break;
                    default:
                        rect->right = rect->left + width;
                        break;
                }
            }

            switch (wParam) {
                case WMSZ_TOP:
                case WMSZ_TOPLEFT:
                case WMSZ_TOPRIGHT:
                    rect->top = rect->bottom - height;
                    break;
                default:
                    rect->bottom = rect->top + height;
                    break;
            }
            return TRUE;
        }

        case WM_GETMINMAXINFO: {
            // Set minimum/maximum window size with aspect ratio
            MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);

            // Calculate border sizes
            RECT borderRect = {0, 0, 100, 100};
            AdjustWindowRect(&borderRect, WS_OVERLAPPEDWINDOW, FALSE);
            int borderW = (borderRect.right - borderRect.left) - 100;
            int borderH = (borderRect.bottom - borderRect.top) - 100;

            // Get screen size
            int screenW = GetSystemMetrics(SM_CXSCREEN);
            int screenH = GetSystemMetrics(SM_CYSCREEN);

            // Calculate max client size that fits screen while maintaining 16:9
            int maxClientW = screenW - borderW;
            int maxClientH = screenH - borderH - 40; // 40 for taskbar

            // Apply aspect ratio constraint to max size
            int maxW_fromH = static_cast<int>(maxClientH * ASPECT_RATIO);
            int maxH_fromW = static_cast<int>(maxClientW / ASPECT_RATIO);

            if (maxW_fromH <= maxClientW) {
                // Height is the limiting factor
                maxClientW = maxW_fromH;
            } else {
                // Width is the limiting factor
                maxClientH = maxH_fromW;
            }

            // Minimum client size: 960x540 (half of 1920x1080)
            mmi->ptMinTrackSize.x = 320 + borderW;
            mmi->ptMinTrackSize.y = 180 + borderH;

            // Maximum client size with aspect ratio
            mmi->ptMaxTrackSize.x = maxClientW + borderW;
            mmi->ptMaxTrackSize.y = maxClientH + borderH;
            return 0;
        }

        case WM_SIZE: {
            auto gui = g_gui;
            if (gui && wParam != SIZE_MINIMIZED) {
                gui->onResize(LOWORD(lParam), HIWORD(lParam));
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            auto gui = g_gui;
            if (gui) {
                gui->onMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            auto gui = g_gui;
            if (gui) {
                gui->onMouseDown(0, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            auto gui = g_gui;
            if (gui) {
                gui->onMouseUp(0, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            }
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            auto gui = g_gui;
            if (gui) {
                gui->onMouseDoubleClick(0, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            }
            return 0;
        }

        case WM_RBUTTONDOWN: {
            auto gui = g_gui;
            if (gui) {
                gui->onMouseDown(1, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            }
            return 0;
        }

        case WM_RBUTTONUP: {
            auto gui = g_gui;
            if (gui) {
                gui->onMouseUp(1, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            }
            return 0;
        }

        case WM_KEYDOWN: {
            auto gui = g_gui;
            if (gui) {
                gui->onKeyDown(static_cast<int>(wParam));
            }
            return 0;
        }

        case WM_KEYUP: {
            auto gui = g_gui;
            if (gui) {
                gui->onKeyUp(static_cast<int>(wParam));
            }
            return 0;
        }
            
        case WM_DESTROY:
            MLOG_INFO("wndproc", "WM_DESTROY received");
            g_running = false;
            PostQuitMessage(0);
            return 0;

        case WM_CLOSE:
            MLOG_INFO("wndproc", "WM_CLOSE received");
            g_running = false;
            DestroyWindow(hWnd);
            return 0;
    }
    
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

} // namespace mirage::gui::window
