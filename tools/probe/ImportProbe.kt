package app.aemu.importer

import java.io.File
import java.io.OutputStream
import java.nio.channels.FileChannel
import java.nio.file.StandardOpenOption
import java.util.zip.ZipFile

fun main(args: Array<String>) {
    if (args.isEmpty()) {
        System.err.println("usage: ImportProbe <firmware.zip|payload.bin> <work dir>")
        kotlin.system.exitProcess(2)
    }
    val src = File(args[0])
    val work = File(args.getOrElse(1) { "probe-work" }).apply { mkdirs() }
    println("== source: ${src.name}, ${src.length() / 1048576} MB")

    val payload = File(work, "payload.bin")
    if (!payload.isFile) {
        if (src.name.endsWith(".bin", true)) src.copyTo(payload, true) else extractPayload(src, payload)
    }
    println("== payload: ${payload.length() / 1048576} MB")

    FileChannel.open(payload.toPath(), StandardOpenOption.READ).use { ch ->
        val rand = ChannelSource(ch)
        check(PayloadReader.probe(rand)) { "not an A/B OTA payload" }
        val names = PayloadReader.partitionNames(rand)
        println("== partitions (${names.size}): ${names.joinToString()}")

        for (p in listOf("system", "vendor", "product", "system_ext", "odm")) {
            val part = names.firstOrNull { it == p || it == "${p}_a" || it == "${p}_b" } ?: continue
            val out = File(work, "$part.img")
            println("-- $part: extracting from the payload ...")
            val bytes = try {
                PayloadReader.extractPartition(rand, part, out) { f -> if (f >= 0.999f) println("   $part 100%") }
                out.length()
            } catch (e: Throwable) {
                println("   $part FAILED: ${e.message ?: e}")
                continue
            }
            println("   $part.img = ${bytes / 1048576} MB")
            if (bytes < 4096) continue
            FileChannel.open(out.toPath(), StandardOpenOption.READ).use { ic ->
                val fs = try { Ext4Reader(ChannelSource(ic)) } catch (e: Throwable) {
                    println("   not ext4: ${e.message}")
                    continue
                }
                println("   ext4 block ${fs.blockSize}, volume \"${fs.volumeName}\"")
                readProps(fs, part)
            }
        }
    }
    println("== done")
}

private fun extractPayload(zipFile: File, out: File) {
    ZipFile(zipFile).use { z ->
        val e = z.entries().toList().firstOrNull { it.name.endsWith("payload.bin") }
            ?: error("no payload.bin in ${zipFile.name}: this archive is not an A/B OTA")
        println("== payload.bin in the zip: ${e.size / 1048576} MB, method ${e.method}")
        z.getInputStream(e).use { i -> out.outputStream().buffered(1 shl 20).use { o -> i.copyTo(o, 1 shl 20) } }
    }
}

private val WANTED = setOf(
    "ro.build.version.sdk", "ro.build.version.release", "ro.build.id", "ro.build.type", "ro.build.tags",
    "ro.product.name", "ro.product.device", "ro.product.model", "ro.product.brand",
    "ro.product.cpu.abi", "ro.product.cpu.abilist", "ro.zygote", "ro.treble.enabled",
    "ro.hardware.egl", "ro.build.version.security_patch", "ro.apex.updatable",
)

private fun readProps(fs: Ext4Reader, part: String) {
    val hits = ArrayList<Pair<String, String>>()
    try {
        fs.walk { path, node ->
            val base = path.trimStart('/').substringAfterLast('/')
            if (node.isFile && base in setOf("build.prop", "prop.default", "default.prop") && node.size in 1..(2L shl 20) && hits.size < 12) {
                val sb = StringBuilder()
                try {
                    fs.copy(node, object : OutputStream() {
                        override fun write(b: Int) { sb.append(b.toChar()) }
                        override fun write(b: ByteArray, off: Int, len: Int) { sb.append(String(b, off, len, Charsets.UTF_8)) }
                    })
                } catch (e: Throwable) { return@walk }
                hits.add(path to sb.toString())
            }
        }
    } catch (e: Throwable) {
        println("   walk of $part stopped: ${e.message}")
    }
    if (hits.isEmpty()) { println("   no build.prop found in $part"); return }
    for ((path, text) in hits) {
        println("   --- $part:/$path")
        for (line in text.lineSequence()) {
            if (line.substringBefore('=', "").trim() in WANTED) println("      ${line.trim()}")
        }
    }
}