// =============================================================================
// MirageSystem v2 GUI - Command Functions Header
// =============================================================================
#pragma once

#include <string>

namespace mirage::gui::command {

// EventBus購読の初期化・解放
void init();
void shutdown();

// Tap commands
void sendTapCommandToAll(int x, int y);
void sendTapCommand(const std::string& device_id, int x, int y);

// Swipe commands
void sendSwipeCommandToAll(int x1, int y1, int x2, int y2, int duration_ms);
void sendSwipeCommand(const std::string& device_id, int x1, int y1, int x2, int y2, int duration_ms);

// Key commands (Back=4, Home=3, Recents=187)
void sendKeyCommand(const std::string& device_id, int keycode);

// Helper functions
std::string getDeviceIdFromSlot(int slot);

} // namespace mirage::gui::command
