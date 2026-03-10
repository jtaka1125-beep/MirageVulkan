package com.mirage.capture.util

import android.content.Context
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

object RouteTrace {
    private const val PREF = "route_trace"
    private const val KEY = "lines"
    private val fmt = SimpleDateFormat("HH:mm:ss.SSS", Locale.US)

    fun append(context: Context, line: String) {
        try {
            val sp = context.getSharedPreferences(PREF, Context.MODE_PRIVATE)
            val prev = sp.getString(KEY, "") ?: ""
            val next = prev + fmt.format(Date()) + " " + line + "\n"
            sp.edit().putString(KEY, next.takeLast(16000)).commit()
        } catch (_: Exception) {
        }
    }

    fun clear(context: Context) {
        try {
            context.getSharedPreferences(PREF, Context.MODE_PRIVATE).edit().putString(KEY, "").commit()
        } catch (_: Exception) {
        }
    }
}
