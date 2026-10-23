package app.aemu.core

import android.content.Context
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * What the guest said around a crash, and every log the image holds gathered in one place.
 *
 * A process that dies is reported by its exit code, and the code alone says very little: 137 is a SIGKILL, which
 * may come from the low memory killer, from system_server deciding the process is stuck, or from a signal the system
 * sent itself. The reason is written in the guest log; this reads it out and phrases it, so the failure message the
 * user sees is the real one instead of a guess.
 */
object CrashDigest {
    /** lines worth quoting when something died: signals, aborts, link and dex failures, the killer's own records */
    private val SIGNIFICANT = listOf(
        "Fatal signal", "signal 11", "signal 6", "signal 9", "abort message", "Abort message",
        "backtrace:", "CANNOT LINK EXECUTABLE", "UnsatisfiedLinkError", "dlopen failed",
        "No original dex files found", "Unable to open or create cache", "dex2oat", "Out of memory",
        "lowmemorykiller", "Killing", "Kill '", "cannot allocate", "failed to map",
        "FATAL EXCEPTION", "ANR in ", "Watchdog", "system_server", "Zygote", "preloaded-classes",
        "INVALID_ARGUMENT", "The device is shutting down", "SELinux", "avc: denied",
    )

    /** the last few lines of the guest log that explain a death, or an empty list */
    fun reasons(root: File, limit: Int = 8): List<String> {
        val recs = runCatching { GuestLog.tail(root, 512 * 1024) }.getOrDefault(emptyList())
        if (recs.isEmpty()) return emptyList()
        val hits = ArrayList<String>()
        for (r in recs) {
            if (SIGNIFICANT.any { r.msg.contains(it) || r.tag.contains(it) }) {
                val line = "${r.prio}/${r.tag}: ${r.msg}".trim()
                if (hits.none { it == line }) hits.add(line)
            }
        }
        return hits.takeLast(limit)
    }

    /** a phrase for the exit code, backed by what the log actually shows */
    fun explain(root: File, exitCode: Int, reason: List<String>): String {
        if (reason.isEmpty()) return byCode(exitCode)
        val text = reason.joinToString(" | ")
        val killed = text.contains("lowmemorykiller", true) || text.contains("Killing", true) || text.contains("Out of memory", true)
        val aborted = text.contains("abort message", true) || text.contains("Fatal signal", true) || text.contains("signal 6", true)
        val link = text.contains("CANNOT LINK", true) || text.contains("UnsatisfiedLinkError", true) || text.contains("dlopen failed", true)
        val dex = text.contains("No original dex files", true) || text.contains("dex2oat", true)
        return when {
            link -> "a library the system needs cannot be loaded: $text"
            dex -> "the precompiled framework code is missing or unusable: $text"
            killed -> "the system ran out of memory and killed it: $text"
            aborted -> "it aborted on its own: $text"
            else -> "it died (code $exitCode): $text"
        }
    }

    /** what the exit code alone means, when the log says nothing */
    fun byCode(code: Int): String = when (code) {
        137 -> "killed with SIGKILL (code 137): the system ran out of memory, or system_server died and took it down"
        134 -> "aborted (code 134): the process called abort()"
        139 -> "crashed with SIGSEGV (code 139): an invalid memory access"
        132 -> "crashed with SIGILL (code 132): an instruction the CPU cannot run, or a broken library"
        6 -> "aborted (code 6): the process called abort()"
        11 -> "crashed with SIGSEGV (code 11): an invalid memory access"
        1 -> "exited with an error (code 1): it refused to start, the log holds the reason"
        else -> "exited (code $code)"
    }

    /**
     * Copies every log the image keeps (the host side one and each guest buffer, decoded to text) into a folder the
     * user can reach, next to a short summary of the device and the last failure. Returns the folder.
     */
    fun export(ctx: Context, paths: VmPaths, img: GuestImage, failure: String?): File {
        val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
        val home = File(android.os.Environment.getExternalStorageDirectory(), FirmwareFolder.HOME)
        val out = File(home, "logs/${img.name.replace(Regex("[^\\p{L}\\p{N} ._-]"), "_").take(40)}-$stamp")
        out.mkdirs()

        runCatching {
            File(out, "summary.txt").writeText(buildString {
                appendLine("image:    ${img.name}")
                appendLine("id:       ${img.id}")
                appendLine("version:  ${img.displayVersion}")
                appendLine("api:      ${img.api}")
                appendLine("skin:     ${img.skin}")
                appendLine("engine:   ${img.effective().engine}")
                appendLine("exported: $stamp")
                appendLine("device:   ${android.os.Build.MANUFACTURER} ${android.os.Build.MODEL}, Android ${android.os.Build.VERSION.RELEASE} (API ${android.os.Build.VERSION.SDK_INT})")
                appendLine("abi:      ${android.os.Build.SUPPORTED_ABIS.joinToString()}")
                appendLine()
                if (failure != null) {
                    appendLine("last failure:")
                    appendLine(failure)
                    appendLine()
                }
                val why = reasons(paths.root)
                if (why.isNotEmpty()) {
                    appendLine("what the guest log says about the last death:")
                    why.forEach { appendLine("  $it") }
                }
            })
        }

        // the host side log, as it stands
        runCatching { File(paths.bin, "aemu.log").takeIf { it.isFile }?.copyTo(File(out, "host.log"), true) }
        // the service logs the engine writes beside it
        paths.bin.listFiles()?.forEach { f ->
            if (f.isFile && f.name.endsWith(".log") && f.name != "aemu.log") {
                runCatching { f.copyTo(File(out, "host-${f.name}"), true) }
            }
        }
        // the guest's own buffers: binary records, decoded here so they can be read without the app
        for (buf in listOf("main", "system", "radio", "events", "crash")) {
            val recs = runCatching { GuestLog.tail(paths.root, 2 * 1024 * 1024, buf) }.getOrDefault(emptyList())
            if (recs.isEmpty()) continue
            runCatching {
                File(out, "guest-$buf.log").bufferedWriter().use { w ->
                    for (r in recs) w.write("${r.prio}/${r.tag}: ${r.msg}\n")
                }
            }
        }
        // anything else the engine dropped in run/
        paths.bin.listFiles()?.forEach { f ->
            if (!f.isFile || f.name.endsWith(".log")) return@forEach
            if (f.length() > (4L shl 20)) return@forEach
            runCatching { f.copyTo(File(out, "run-${f.name}"), true) }
        }
        return out
    }
}