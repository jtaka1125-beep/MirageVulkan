// =============================================================================
// MirageSystem - GUI Initialization Helpers
// =============================================================================
// Split from gui_main.cpp for maintainability
// Contains: Component initialization functions
// =============================================================================
#pragma once

#include <windows.h>

namespace mirage::gui::init {

// USB video callback setup (must be called after g_hybrid_cmd is created)
void setupUsbVideoCallback();

// Component initializers (call in order)
bool initializeMultiReceiver();
bool initializeTcpReceiver();
void initializeHybridCommand();
void initializeRouting();
void initializeGUI(HWND hwnd);

#ifdef USE_AI
void initializeAI();
#endif

#ifdef USE_OCR
void initializeOCR();
#endif

} // namespace mirage::gui::init
