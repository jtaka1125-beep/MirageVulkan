package com.mirage.capture.core

object Config {
    const val DEFAULT_SLOT: Int = 0
    const val TAG_VIDMETA: String  = "VIDMETA"
    const val TAG_UITREE: String   = "UI_TREE"
    const val TAG_LOG: String      = "LOG"
    const val TAG_TAP_EXEC: String = "TAP_EXEC"
    const val TAG_BACK_EXEC: String = "BACK_EXEC"
    // UDP_HOST / UDP_PORT は CaptureActivity の UI 入力値から取得するため不要
}
