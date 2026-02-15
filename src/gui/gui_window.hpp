// =============================================================================
// MirageSystem v2 GUI - Window Procedure Header
// =============================================================================
#pragma once

#include <Windows.h>

namespace mirage::gui::window {

// Window procedure callback
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

} // namespace mirage::gui::window
