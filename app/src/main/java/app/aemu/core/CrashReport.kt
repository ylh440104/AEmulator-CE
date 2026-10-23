package app.aemu.core

import android.content.Context
import app.aemu.BuildConfig
import java.io.File
import java.io.PrintWriter
import java.io.StringWriter
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * A crash inside the app, written down before the process ends.
 *
 * Some failures are not a guest process dying but the emulator itself: the engine crashes on a firmware it cannot
 * handle, the app window closes, and the user is back on the library with nothing to report. The stack trace goes to
 * a file in the app's own folder first (that write cannot fail on a permission), and from there to the shared folder
 * so it can be attached to a bug report.
 */
object CrashReport {
    private const val NAME = "last-crash.txt"

    fun install(ctx: Context) {
        val previous = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, error ->
            runCatching { write(ctx, thread, error) }
            previous?.uncaughtException(thread, error)
        }
        // native crashes (the engine's own code) do not reach the handler above, but they leave a tombstone in the
        // app's cache directory; it is copied out on the next start
        runCatching { collectTombstones(ctx) }
    }

    private fun write(ctx: Context, thread: Thread, error: Throwable) {
        val stamp = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(Date())
        val trace = StringWriter().also { error.printStackTrace(PrintWriter(it)) }.toString()
        val text = buildString {
            appendLine("time:    $stamp")
            appendLine("thread:  ${thread.name}")
            appendLine("device:  ${android.os.Build.MANUFACTURER} ${android.os.Build.MODEL}, Android ${android.os.Build.VERSION.RELEASE} (API ${android.os.Build.VERSION.SDK_INT})")
            appendLine("abi:     ${android.os.Build.SUPPORTED_ABIS.joinToString()}")
            appendLine("version: ${BuildConfig.VERSION_NAME} (${BuildConfig.VERSION_CODE})")
            appendLine()
            appendLine(trace)
        }
        // the app's own folder always accepts this; /sdcard may not, when the storage permission was never granted
        runCatching { File(ctx.filesDir, NAME).writeText(text) }
        publish(ctx, text)
    }

    /** true when a crash was recorded and has not been exported yet */
    fun pending(ctx: Context): Boolean = File(ctx.filesDir, NAME).isFile

    fun lastText(ctx: Context): String? = runCatching {
        File(ctx.filesDir, NAME).takeIf { it.isFile }?.readText()
    }.getOrNull()

    fun clear(ctx: Context) = runCatching { File(ctx.filesDir, NAME).delete() }

    private fun publish(ctx: Context, text: String) {
        runCatching {
            val home = File(android.os.Environment.getExternalStorageDirectory(), FirmwareFolder.HOME)
            val out = File(home, "logs")
            out.mkdirs()
            val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
            File(out, "crash-$stamp.txt").writeText(text)
        }
    }

    /** tombstones left by a native crash of the engine, moved to the same place as the java ones */
    private fun collectTombstones(ctx: Context) {
        val dirs = listOf(File(ctx.cacheDir, "tombstones"), File("/data/tombstones"))
        for (d in dirs) {
            if (!d.isDirectory) continue
            d.listFiles()?.forEach { f ->
                if (!f.isFile || f.length() > (2L shl 20)) return@forEach
                val text = runCatching { f.readText(Charsets.ISO_8859_1) }.getOrNull() ?: return@forEach
                if (!text.contains("*** ***") && !text.contains("pid:")) return@forEach
                publish(ctx, "native crash: ${f.name}\n\n$text")
                runCatching { f.delete() }
            }
        }
    }
}