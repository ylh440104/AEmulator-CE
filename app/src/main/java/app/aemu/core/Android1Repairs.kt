package app.aemu.core

import java.io.File
import java.security.MessageDigest

internal object Android1Repairs {
    private fun hash(bytes: ByteArray): String = MessageDigest.getInstance("SHA-256")
        .digest(bytes).joinToString("") { "%02x".format(it) }

    private fun restore(
        file: File,
        damaged: String,
        original: String,
        changes: List<Pair<Int, ByteArray>>,
        log: (String) -> Unit
    ) {
        if (!file.isFile) return
        val bytes = file.readBytes()
        if (hash(bytes) != damaged) return
        val repaired = bytes.copyOf()
        for ((offset, replacement) in changes) {
            require(offset >= 0 && offset + replacement.size <= repaired.size)
            replacement.copyInto(repaired, offset)
        }
        check(hash(repaired) == original)
        val backup = File(file.parentFile, "${file.name}.before-android1-repair")
        if (!backup.exists()) file.copyTo(backup)
        val temporary = File(file.parentFile, "${file.name}.repair-new")
        temporary.writeBytes(repaired)
        temporary.setReadable(true, false)
        temporary.setWritable(true, true)
        check(temporary.renameTo(file))
        log("Android 1: restored ${file.name}, backup=${backup.name}")
    }

    fun apply(root: File, log: (String) -> Unit) {
        restore(
            File(root, "system/lib/libutils.so"),
            "0104b405f141d5696eaea9659888740314b015907bd5bce81d4d2d0a5eb628ed",
            "f588d054518fc2afdbd2e46a8e02cdc285f8494ff69deb2a47a51b5aee3a5eb8",
            listOf(
                0x2d550 to byteArrayOf(0x01, 0x68),
                0x2d556 to byteArrayOf(0xf2.toByte(), 0xf7.toByte(), 0xd5.toByte(), 0xfe.toByte()),
                0x30dda to byteArrayOf(0xef.toByte(), 0xf7.toByte(), 0x93.toByte(), 0xfa.toByte())
            ),
            log
        )
        restore(
            File(root, "system/lib/liblog.so"),
            "86a1e9d0f1b1f6740736c7508abdb9bb398c65ec473172be4c34d5d5c53d55c7",
            "ad6a25ed16dcf799cda5573fb37c35a68718a824a8d30f1bf088700d83671455",
            listOf(0x32d4 to "/dev/log/main\u0000".toByteArray(Charsets.US_ASCII)),
            log
        )
    }
}