package app.aemu.core

import android.os.Environment
import java.io.File

/**
 * Internal storage/AEmulatorCE/firmware: firmware files dropped there show up in the library for one-tap import.
 * Next to the systems' memory cards, which live in the same folder.
 */
object FirmwareFolder {
    /** the folder of the app on the shared storage: the systems' memory cards sit here, and firmware goes in a subfolder */
    const val HOME = "AEmulatorCE"
    private val EXT = Regex("""(?i).*\.(zip|7z|rar|tar|md5|tgz|gz|xz|bz2|tbz2?|lz4|br|img|win|ext4|pac|ftf|sin|bin|yaffs2|rfs|simg)$""")
    val dir: File get() = File(Environment.getExternalStorageDirectory(), "$HOME/firmware")

    /** Firmware files, newest first; empty when there is no all-files access. */
    fun list(): List<File> = runCatching {
        if (!Environment.isExternalStorageManager()) return emptyList()
        dir.mkdirs()
        dir.listFiles().orEmpty().filter { it.isFile && it.length() > 1_000_000 && EXT.matches(it.name) }
            .sortedByDescending { it.lastModified() }
    }.getOrDefault(emptyList())
}
