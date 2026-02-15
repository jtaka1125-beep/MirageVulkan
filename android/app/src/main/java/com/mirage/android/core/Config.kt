package com.mirage.android.core

object Config {
    // TODO: set these from UI later
    const val DEFAULT_SLOT: Int = 0
    const val UDP_HOST: String = "172.20.10.2" // PC IP (from config.json)
    const val UDP_PORT: Int = 60000            // must match video_base_port in config.json
    const val TAG_VIDMETA: String = "VIDMETA"
    const val TAG_UITREE: String = "UI_TREE"
    const val TAG_LOG: String = "LOG"
    const val TAG_TAP_EXEC: String = "TAP_EXEC"
    const val TAG_BACK_EXEC: String = "BACK_EXEC"
}
