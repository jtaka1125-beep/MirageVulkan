package com.mirage.accessory.util

import android.content.Intent
import android.os.Build
import android.os.Parcelable

/**
 * API 33+ overload-safe getParcelableExtra().
 *
 * Android 13 (API 33) introduced type-safe overloads; the older generic form is deprecated.
 */
inline fun <reified T : Parcelable> Intent.parcelableExtra(name: String): T? {
    return if (Build.VERSION.SDK_INT >= 33) {
        getParcelableExtra(name, T::class.java)
    } else {
        @Suppress("DEPRECATION")
        getParcelableExtra(name) as? T
    }
}
