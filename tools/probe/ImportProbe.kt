package app.aemu.importer

import java.io.File
import java.io.InputStream
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

    if (src.name.endsWith(".zip", true)) {
        val kind = try { probeZip(src, work) } catch (e: Throwable) { println("== zip layout: ${e.message}"); "payload" }
        if (kind == "newdat") return
    }
    probePayload(src, work)
}

/**
 * What is inside a vendor or AOSP archive, and whether the app's importer could take it: an A/B OTA (payload.bin),
 * a block-based OTA (system.new.dat plus its transfer.list, possibly in slices), or plain images.
 */
private fun probeZip(zipFile: File, work: File): String {
    ZipFile(zipFile).use { z ->
        val names = z.entries().toList().map { it.name.replace('\\', '/') }
        val payload = names.firstOrNull { it.endsWith("payload.bin") }
        val dat = names.filter { it.matches(Regex("(.*/)?system\\.new\\.dat(\\.br|(\\.\\d+)?)")) }
        val fw = names.filter { it.contains("firmware-update/") }
        println("== entries: ${names.size}, payload.bin: ${if (payload != null) "yes" else "no"}, " +
            "system.new.dat slices: ${dat.count { it.endsWith(".new.dat") || it.matches(Regex(".*\\.dat\\.\\d+")) }}, " +
            "firmware-update/: ${fw.size}")
        if (payload != null) return "payload"
        if (dat.isNotEmpty()) {
            probeNewDat(z, work, dat)
            return "newdat"
        }
        println("== neither a payload nor system.new.dat: this is not an OTA the importer can unpack")
        for (n in names.filter { it.endsWith(".img") || it.endsWith(".bin") }.take(15)) println("   $n")
        return "other"
    }
}

/** Rebuilds system.img the way Importer.fatEntryStream + TransferList.build do, then reads its properties */
private fun probeNewDat(z: ZipFile, work: File, datNames: List<String>) {
    val entries = z.entries().toList()
    val base = entries.firstOrNull { it.name.matches(Regex("(.*/)?system\\.new\\.dat")) }
    val numbered = entries.filter { it.name.matches(Regex("(.*/)?system\\.new\\.dat\\.\\d+")) }
        .sortedBy { it.name.substringAfterLast('.').toIntOrNull() ?: 0 }
    val listE = entries.firstOrNull { it.name.matches(Regex("(.*/)?system\\.transfer\\.list")) }
    if (base == null && numbered.isEmpty()) { println("== no system.new.dat at all"); return }
    if (listE == null) { println("== system.transfer.list is missing: TransferList cannot place the blocks"); return }
    // the base entry is a piece of the file when it carries bytes (vivo); an empty placeholder starts at ".1"
    val parts = ArrayList<java.util.zip.ZipEntry>()
    if (base != null && base.size > 0) parts.add(base)
    parts.addAll(numbered)
    println("== system: base entry ${if (base != null) "${base.size} B" else "none"}, " +
        "${numbered.size} numbered slices (${numbered.sumOf { it.size } / 1048576} MB), " +
        "${parts.size} pieces joined (${parts.sumOf { it.size } / 1048576} MB), transfer.list ${listE.size} B")
    val list = z.getInputStream(listE).use { String(it.readBytes()) }
    val ver = list.lineSequence().firstOrNull()?.trim()
    println("   transfer.list version $ver")
    val raw = File(work, "system.raw")
    val joined = File(work, "system.joined")
    val stream = if (parts.size > 1) joinedSlices(z, parts) else z.getInputStream(base ?: parts[0])
    try {
        stream.use { s -> s.copyTo(joined.outputStream().buffered(1 shl 20), 1 shl 20) }
    } catch (e: Throwable) {
        println("   joining FAILED: ${e.message ?: e}")
        return
    }
    println("   joined = ${joined.length() / 1048576} MB")
    val wholeFs = FileChannel.open(joined.toPath(), StandardOpenOption.READ).use { c ->
        val src = ChannelSource(c)
        when {
            ErofsReader.probe(src) -> "EROFS"
            Ext4Reader.probe(src) -> "ext4"
            SuperImage.probe(src) -> "super"
            SparseSource.probe(src) -> "sparse"
            else -> ""
        }
    }
    println("   joined content: ${if (wholeFs.isEmpty()) "not a file system image" else "$wholeFs image"}")
    val img: File
    if (wholeFs.isNotEmpty()) {
        println("   the data file is the whole image; the transfer.list does not index it")
        img = joined
    } else {
        img = raw
        val t0 = System.currentTimeMillis()
        try {
            joined.inputStream().buffered(1 shl 20).use { s -> TransferList.build(list, s, raw) }
        } catch (e: Throwable) {
            println("   TransferList FAILED: ${e.message ?: e}")
            return
        }
        println("   rebuilt via transfer.list = ${raw.length() / 1048576} MB in ${(System.currentTimeMillis() - t0) / 1000} s")
    }
    FileChannel.open(img.toPath(), StandardOpenOption.READ).use { ch ->
        val src = ChannelSource(ch)
        when {
            ErofsReader.probe(src) -> {
                println("   file system: EROFS")
                val fs = try { ErofsImage(src) } catch (e: Throwable) { println("   EROFS reader failed: ${e.message}"); return }
                readPropsTree(fs, "system")
            }
            Ext4Reader.probe(src) -> {
                println("   file system: ext4")
                val fs = Ext4Reader(src)
                println("   ext4 block ${fs.blockSize}, volume \"${fs.volumeName}\"")
                readProps(fs, "system")
            }
            else -> println("   file system: neither ext4 nor EROFS (the transfer.list did not build a valid image)")
        }
    }
}

private fun readPropsTree(fs: TreeSource, part: String) {
    val hits = ArrayList<Pair<String, String>>()
    try {
        fs.walk { path, isDir, link, mode, size, data ->
            val base = path.trimStart('/').substringAfterLast('/')
            if (!isDir && link == null && data != null && base in setOf("build.prop", "prop.default", "default.prop")
                && size in 1..(2L shl 20) && hits.size < 12) {
                val text = runCatching { data().use { it.readBytes().toString(Charsets.UTF_8) } }.getOrNull()
                if (text != null) hits.add(path to text)
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

/** the slices as one stream, exactly as Importer.fatEntryStream joins them */
private fun joinedSlices(z: ZipFile, slices: List<java.util.zip.ZipEntry>): InputStream = object : InputStream() {
    private var i = 0
    private var cur: InputStream? = null
    override fun read(): Int { val b = ByteArray(1); return if (read(b, 0, 1) <= 0) -1 else b[0].toInt() and 0xff }
    override fun read(b: ByteArray, off: Int, len: Int): Int {
        if (len == 0) return 0
        while (true) {
            val c = cur ?: if (i < slices.size) z.getInputStream(slices[i++]).also { cur = it } else return -1
            val n = c.read(b, off, len)
            if (n > 0) return n
            c.close(); cur = null
        }
    }
}

private fun probePayload(src: File, work: File) {
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