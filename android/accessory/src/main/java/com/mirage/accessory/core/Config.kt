package com.mirage.accessory.core

object Config {
    const val DEFAULT_SLOT: Int = 0
    // UdpSender のデフォルト宛先 (PC側 MirageVulkan のUDP受信ポート)
    // TODO: AccessoryActivity UI から動的設定に移行する
    const val UDP_HOST: String = "192.168.0.2"  // FIX-9: 172.20.10.2 (hotspot) → 192.168.0.2 (LAN)
    const val UDP_PORT: Int = 60000
    const val TAG_VIDMETA: String       = "VIDMETA"
    const val TAG_UITREE: String        = "UI_TREE"
    const val TAG_LOG: String           = "LOG"
    const val TAG_TAP_EXEC: String      = "TAP_EXEC"
    const val TAG_BACK_EXEC: String     = "BACK_EXEC"
    const val TAG_SWIPE_EXEC: String    = "SWIPE_EXEC"
    const val TAG_PINCH_EXEC: String    = "PINCH_EXEC"
    const val TAG_LONGPRESS_EXEC: String = "LONGPRESS_EXEC"
}
