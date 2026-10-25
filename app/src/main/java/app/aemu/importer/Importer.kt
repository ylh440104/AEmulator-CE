package app.aemu.importer

import android.content.Context
import android.net.Uri
import android.system.Os
import app.aemu.core.GuestImage
import app.aemu.core.ImageStore
import app.aemu.core.TreeFixer
import app.aemu.core.VmPaths
import app.aemu.core.RecoveryImage
import org.apache.commons.compress.archivers.tar.TarArchiveEntry
import org.apache.commons.compress.archivers.tar.TarArchiveInputStream
import org.apache.commons.compress.archivers.sevenz.SevenZFile
import org.apache.commons.compress.archivers.zip.ZipArchiveEntry
import org.apache.commons.compress.archivers.zip.ZipFile
import org.apache.commons.compress.compressors.bzip2.BZip2CompressorInputStream
import org.brotli.dec.BrotliInputStream
import org.tukaani.xz.XZInputStream
import java.io.BufferedInputStream
import java.io.File
import java.io.FileInputStream
import java.io.IOException
import java.io.InputStream
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.channels.FileChannel
import java.nio.file.StandardOpenOption
import java.util.zip.GZIPInputStream

/**
 * Импорт прошивки в дерево гостя. Понимает (в том числе вложенные друг в друга):
 *  - ZIP для CWM/TWRP (MIUI, CyanogenMod, большинство прошивок 2.x–4.x): system/… + boot.img + updater-script
 *  - ZIP/TGZ factory-образов Google, прошивки с system.img внутри
 *  - TAR / TAR.MD5 (Samsung Odin, TouchWiz), TWRP-бэкапы (.win), tar.gz/xz/bz2
 *  - system.img: ext2/3/4, в том числе sparse (и system.img_sparsechunk.* Motorola)
 *  - system.new.dat(.br) + system.transfer.list (OTA 5.x–6.x)
 *  - уже готовое дерево rootfs в tar.gz (например, из стендов HTC)
 */
class Importer(
    private val ctx: Context,
    private val onProgress: (String, Float) -> Unit,
    private val log: (String) -> Unit,
) {
    private lateinit var paths: VmPaths
    private lateinit var root: File
    private val tmp = File(ctx.cacheDir, "import").apply { mkdirs() }
    private var ramdisk: List<BootImage.CpioEntry>? = null
    private var recovery: ByteArray? = null
    private val symlinks = ArrayList<Pair<String, String>>() // (цель, путь ссылки в госте)
    private val perms = ArrayList<Triple<String, Int, Boolean>>() // (путь, режим, рекурсивно-файлы)
    private var files = 0
    private var bytes = 0L
    private var gotSystem = false
    @Volatile var cancelled = false

    fun import(uri: Uri, name: String): GuestImage {
        val id = ImageStore.newId(ctx)
        paths = VmPaths(ctx, id)
        root = paths.root
        root.mkdirs()
        try {
            onProgress("Opening $name", 0f)
            val pfd = ctx.contentResolver.openFileDescriptor(uri, "r") ?: throw IOException("file failed to open")
            val srcSize = runCatching { pfd.statSize }.getOrDefault(-1L)
            // a firmware unpacks to roughly twice its compressed size (system.img, the ramdisk, the recovery):
            // with less room than that the import would stop halfway, leaving a broken tree behind
            if (srcSize > 0) {
                val need = srcSize + (srcSize / 2) + (256L shl 20)
                val free = ImageStore.imagesDir(ctx).usableSpace
                if (free in 1 until need) throw IOException("not enough space: need ${need shr 20} MB, free ${free shr 20} MB")
            }
            pfd.use {
                val ch = FileInputStream(pfd.fileDescriptor).channel
                val seekable = runCatching { ch.position(0); ch.size() > 0 }.getOrDefault(false)
                if (seekable) handle(ChannelSource(ch), ch, name, 0)
                else {
                    val t = spill(FileInputStream(pfd.fileDescriptor), "whole")
                    FileChannel.open(t.toPath(), StandardOpenOption.READ).use { c -> handle(ChannelSource(c), c, name, 0) }
                    t.delete()
                }
            }
            // build.prop is optional: some dumps/ports lack it, Analyzer infers the version from the tree
            if (!File(root, "system/framework").isDirectory && !File(root, "system/build.prop").isFile)
                throw IOException("no Android system partition found in file")
            finishTree()
            recovery?.let { runCatching { RecoveryImage.install(paths, it, log) } }
            onProgress("Analyzing firmware", 0.97f)
            val img = Analyzer(ctx, paths, ramdisk).analyze(id, name)
            TreeFixer(ctx, paths, img, log).sanitize()
            ImageStore.save(ctx, img.copy(sizeBytes = ImageStore.du(paths.dir)))
            onProgress("Done", 1f)
            log("import: $files files, ${bytes shr 20} MB")
            return ImageStore.get(ctx, id)!!
        } catch (t: Throwable) {
            closeHeld()
            ImageStore.delete(ctx, id)
            throw t
        } finally {
            closeHeld()
            tmp.listFiles()?.forEach { it.delete() }
        }
    }

    // ------------------------------------------------------------------ разбор контейнеров

    private fun handle(src: RandomSource, ch: FileChannel?, name: String, depth: Int) {
        if (depth > 4) return
        val head = ByteBuffer.allocate(1100)
        src.read(0, head); head.flip()
        val h = ByteArray(head.remaining()).also { head.get(it) }
        when {
            h.size > 4 && h[0] == 'P'.code.toByte() && h[1] == 'K'.code.toByte() && h[2].toInt() == 3 && h[3].toInt() == 4 -> {
                if (ch != null) importZip(ch, name, depth) else throw IOException("zip without random access")
            }
            h.size > 6 && h[0] == '7'.code.toByte() && h[1] == 'z'.code.toByte() && h[2] == 0xBC.toByte() && h[3] == 0xAF.toByte() -> {
                if (ch != null) importSevenZ(ch, name, depth) else throw IOException("7z without random access")
            }
            h.size > 7 && String(h, 0, 6, Charsets.ISO_8859_1) == "Rar!\u001a\u0007" -> importRar(src, name, depth)
            PacFile.probe(h) -> if (ch != null) importPac(src, ch) else throw IOException("pac without random access")
            KdzImage.probeKdz2(src) -> importKdz2(src)
            KdzImage.probeDz2(src) -> importDz2(src)
            KdzImage.probe(src) -> importKdz(src)
            PayloadReader.probe(h) -> importPayload(src)
            SinImage.probe(h) || SinImage.probeOld(h) && name.endsWith(".sin", true) -> importSin(streamOf(src), name.substringAfterLast('/'))
            SparseSource.probe(src) || Ext4Reader.probe(src) || SuperImage.probe(src) || ErofsReader.probe(src) -> importImage(src, "system")
            // (the content first: forum rootfs ".tar" files are gzip-compressed)
            isTar(h) || !isCompressed(h) && name.endsWith(".tar", true) || name.endsWith(".md5", true) || name.endsWith(".win", true) ->
                importTarStream(streamOf(src), name, depth, src)
            h[0] == 0x1f.toByte() && h[1] == 0x8b.toByte() -> importCompressed(GZIPInputStream(streamOf(src), 1 shl 16), name, depth)
            h[0] == 0xfd.toByte() && h[1] == '7'.code.toByte() -> importCompressed(XZInputStream(streamOf(src)), name, depth)
            h[0] == 'B'.code.toByte() && h[1] == 'Z'.code.toByte() && h[2] == 'h'.code.toByte() -> importCompressed(BZip2CompressorInputStream(streamOf(src)), name, depth)
            h[0] == 0x04.toByte() && h[1] == 0x22.toByte() && h[2] == 0x4d.toByte() && h[3] == 0x18.toByte() ->
                importCompressed(org.apache.commons.compress.compressors.lz4.FramedLZ4CompressorInputStream(streamOf(src)), name, depth)
            String(h, 0, 8, Charsets.ISO_8859_1) == "ANDROID!" -> takeBoot(readAllFrom(src))
            // raw images without a recognisable header at 0 (YAFFS2 dumps, Samsung RFS)
            Yaffs2Image.probe(src) || FatImage.probe(src) -> importImage(src, "system")
            else -> throw IOException("unknown file format \"$name\"")
        }
    }

    /**
     * gzip/xz/bzip2/lz4 around something: a tar is unpacked as a stream; anything else (GSI system.img.xz, a zip,
     * a sparse image) is written out and recognised again.
     */
    private fun importCompressed(s: InputStream, name: String, depth: Int) {
        val b = java.io.BufferedInputStream(s, 1 shl 16)
        b.mark(1024)
        val head = ByteArray(512)
        var n = 0
        while (n < head.size) { val k = b.read(head, n, head.size - n); if (k < 0) break; n += k }
        b.reset()
        if (n >= 262 && isTar(head)) { importTarStream(b, name, depth); return }
        val inner = name.replace(Regex("(?i)\\.(gz|tgz|xz|bz2|lz4)$"), "")
        val t = spill(b, inner.substringAfterLast('/'))
        try { FileChannel.open(t.toPath(), StandardOpenOption.READ).use { c -> handle(ChannelSource(c), c, inner, depth + 1) } }
        finally { t.delete() }
    }

    /** RAR 4 (junrar): entries handled like those of a zip, via temporary files. */
    private fun importRar(src: RandomSource, name: String, depth: Int) {
        val whole = File(tmp, "${System.nanoTime()}-whole.rar")
        streamOf(src).use { i -> whole.outputStream().use { o -> i.copyTo(o, 1 shl 20) } }
        try {
            com.github.junrar.Archive(whole).use { a ->
                val nested = ArrayList<Pair<File, String>>()
                for (fh in a.fileHeaders) {
                    if (cancelled) throw IOException("cancelled")
                    if (fh.isDirectory) continue
                    val n = fh.fileName.replace('\\', '/')
                    val base = n.substringAfterLast('/')
                    when {
                        n.startsWith("system/") -> { a.getInputStream(fh).use { writeFile(n, it, null) }; gotSystem = true }
                        base.equals("boot.img", true) -> a.getInputStream(fh).use { takeBoot(it.readBytes()) }
                        base.equals("recovery.img", true) && fh.fullUnpackSize < 512_000_000 -> a.getInputStream(fh).use { recovery = it.readBytes() }
                        base.matches(SYSTEM_IMAGE) || base.matches(Regex("(?i).*\\.(zip|7z|tar|md5|tgz|gz|xz|pac|ftf|sin|kdz|dz|new\\.dat|transfer\\.list)")) && fh.fullUnpackSize > 20_000_000 ->
                            nested.add(a.getInputStream(fh).use { spill(it, base) } to base)
                    }
                    onProgress("Extracting: $base", -1f)
                }
                for ((f, base) in nested) {
                    try { FileChannel.open(f.toPath(), StandardOpenOption.READ).use { c -> handle(ChannelSource(c), c, base, depth + 1) } }
                    finally { f.delete() }
                }
            }
        } finally { whole.delete() }
        if (!gotSystem) log("no system found in archive $name")
    }

    private fun isTar(h: ByteArray) = h.size > 262 && String(h, 257, 5, Charsets.ISO_8859_1) == "ustar"

    /**
     * The entry that starts a partition's data: "<part>.new.dat", or its first slice when the vendor split the file
     * (a 4 GB limit on the archive's file system: vivo cuts both system and vendor into 16 slices of 475 MB).
     */
    private fun entryNamed(entries: List<ZipArchiveEntry>, part: String): ZipArchiveEntry? =
        entries.firstOrNull { it.name.matches(Regex("(.*/)?$part\\.new\\.dat")) }
            ?: entries.firstOrNull { it.name.matches(Regex("(.*/)?$part\\.new\\.dat\\.1")) }
            ?: entries.filter { it.name.matches(Regex("(.*/)?$part\\.new\\.dat\\.\\d+")) }
                .minByOrNull { it.name.substringAfterLast('.').toIntOrNull() ?: Int.MAX_VALUE }

    /**
     * The bytes of an entry as one stream, the slices of a split data file joined in order.
     *
     * A vendor that had to stay under its archive's file size limit cuts both "system.new.dat" and the remaining
     * slices: vivo's package carries 16 pieces of 475 MB, the first of them under the plain name. So the base entry
     * counts as the first slice whenever it has bytes of its own; a package that only uses it as a placeholder
     * (0 bytes) starts at ".1". The base name has to match exactly, so "vendor.new.dat.3" never ends up in system.
     */
    private fun fatEntryStream(zip: ZipFile, entry: ZipArchiveEntry): InputStream {
        val m = Regex("^(.*)\\.new\\.dat(?:\\.(\\d+))?$").find(entry.name.replace('\\', '/'))
        // (a Brotli-compressed data file is never split: it has to be inflated before anything can be read)
        if (m == null || entry.name.endsWith(".br", true)) return zip.getInputStream(entry)
        val dir = m.groupValues[1]
        val numbered = zip.entries.toList()
            .mapNotNull { e -> Regex("^${Regex.escape(dir)}\\.new\\.dat\\.(\\d+)$").find(e.name.replace('\\', '/'))
                ?.let { mm -> (mm.groupValues[1].toIntOrNull() ?: return@mapNotNull null) to e } }
            .sortedBy { it.first }
        val base = Regex("^${Regex.escape(dir)}\\.new\\.dat$").let { re ->
            zip.entries.toList().firstOrNull { re.matches(it.name.replace('\\', '/')) }
        }
        val parts = ArrayList<ZipArchiveEntry>()
        // the base entry is a piece of the file only when it carries bytes: a package that uses it as an empty
        // placeholder (0 bytes) starts at ".1", and one that put real data in it (vivo) starts at the base
        if (base != null && base.size > 0) parts.add(base)
        parts.addAll(numbered.map { it.second })
        if (parts.size <= 1) return zip.getInputStream(entry)
        log("${dir.substringAfterLast('/')}: ${parts.size} pieces joined (${parts.sumOf { it.size } shr 20} MB)")
        return object : InputStream() {
            private var i = 0
            private var cur: InputStream? = null
            override fun read(): Int { val b = ByteArray(1); return if (read(b, 0, 1) <= 0) -1 else b[0].toInt() and 0xff }
            override fun read(b: ByteArray, off: Int, len: Int): Int {
                if (len == 0) return 0
                while (true) {
                    val c = cur ?: if (i < parts.size) zip.getInputStream(parts[i++]).also { cur = it } else return -1
                    val n = c.read(b, off, len)
                    if (n > 0) return n
                    runCatching { c.close() }
                    cur = null
                }
            }
            override fun close() { runCatching { cur?.close() }; cur = null }
        }
    }

    /** gzip, xz, bzip2 */
    private fun isCompressed(h: ByteArray) = h.size > 2 && (h[0] == 0x1f.toByte() && h[1] == 0x8b.toByte() ||
        h[0] == 0xfd.toByte() && h[1] == '7'.code.toByte() || h[0] == 'B'.code.toByte() && h[1] == 'Z'.code.toByte() && h[2] == 'h'.code.toByte())

    private fun streamOf(src: RandomSource): InputStream = object : InputStream() {
        var pos = 0L
        override fun read(): Int { val b = ByteArray(1); return if (read(b, 0, 1) <= 0) -1 else b[0].toInt() and 0xff }
        override fun read(b: ByteArray, off: Int, len: Int): Int {
            if (pos >= src.size) return -1
            val k = minOf(len.toLong(), src.size - pos).toInt()
            src.read(pos, ByteBuffer.wrap(b, off, k))
            pos += k
            return k
        }
    }.let { BufferedInputStream(it, 1 shl 20) }

    private fun readAllFrom(src: RandomSource): ByteArray {
        val b = ByteBuffer.allocate(src.size.toInt()); src.read(0, b); return b.array()
    }

    private var zipChannel: FileChannel? = null
    /** partition images spilled out of a nested zip, imported only after that zip's temp file is gone (disk peak) */
    private var deferred: MutableList<Pair<File, String>>? = null

    private fun importZip(ch: FileChannel, name: String, depth: Int) {
        val zip = ZipFile.builder().setSeekableByteChannel(ch).get()
        val prevCh = zipChannel
        zipChannel = ch
        try { importZipEntries(zip, name, depth) } finally { zipChannel = prevCh }
    }

    private fun importZipEntries(zip: ZipFile, name: String, depth: Int) {
        val entries = zip.entries.toList()
        val total = entries.sumOf { maxOf(0L, it.size) }.coerceAtLeast(1)
        var done = 0L
        // сначала — сценарий установки (ссылки и права)
        entries.firstOrNull { it.name.endsWith("META-INF/com/google/android/updater-script") }?.let { e ->
            zip.getInputStream(e).use { parseUpdaterScript(String(it.readBytes())) }
        }
        // прошивка, упакованная вместе с папкой (Имя/META-INF/…, Имя/system/…): папку-обёртку снимаем
        val wrap = entries.firstOrNull { it.name.endsWith("META-INF/com/google/android/updater-script") }
            ?.name?.substringBefore("META-INF/")?.takeIf { it.isNotEmpty() && it.count { c -> c == '/' } == 1 } ?: ""
        if (wrap.isNotEmpty()) log("archive with wrapper folder \"${wrap.trimEnd('/')}\"")
        val names = entries.map { it.name.removePrefix(wrap) }.toSet()
        val datBr = entryNamed(entries, "system")
        val sparseChunks = entries.filter { it.name.matches(Regex("(?i)(.*/)?system\\.img(_sparsechunk)?[._]\\d+")) }
            .sortedBy { it.name.substringAfterLast('.').let { n -> n.filter { c -> c.isDigit() }.toIntOrNull() ?: 0 } }
        // vendor firmware keeps the images a recovery would flash under firmware-update/ (vivo, OPPO, Xiaomi's
        // super images), out of reach of the rules below, which look at the entry's own name
        val fwDir = entries.firstOrNull { e -> !e.isDirectory && e.name.contains("firmware-update/") }
            ?.name?.substringBefore("firmware-update/")?.plus("firmware-update/")
        for (e in entries) {
            if (cancelled) throw IOException("cancelled")
            val n = e.name.replace('\\', '/').removePrefix(wrap)
            val base = n.substringAfterLast('/')
            // (a boot image of a vendor package: the ramdisk, and the recovery next to it)
            val inFw = fwDir != null && n.startsWith(fwDir) && n.count { it == '/' } == fwDir.count { it == '/' }
            when {
                e.isDirectory -> {}
                n.contains("__MACOSX/") || base.startsWith("._") -> {}
                n.startsWith("system/") -> {
                    zip.getInputStream(e).use { writeFile(n, it, e.unixMode.takeIf { m -> m != 0 }) }
                    gotSystem = true
                }
                base.equals("boot.img", true) || inFw && base.matches(Regex("(?i)boot(_a)?\\.img")) ->
                    zip.getInputStream(e).use { takeBoot(it.readBytes()) }
                base.equals("recovery.img", true) || inFw && base.matches(Regex("(?i)recovery(_a)?\\.img")) ->
                    if (e.size < 512_000_000) zip.getInputStream(e).use { recovery = it.readBytes() }
                inFw && base.matches(Regex("(?i)super(\\.sparse)?\\.img")) -> withEntrySource(zip, e) { importImage(it, "system") }
                inFw && base.matches(Regex("(?i)(system|vendor|product|system_ext|odm)(\\.sparse)?\\.img")) -> {
                    val part = base.substringBefore('.').lowercase()
                    withEntrySource(zip, e) {
                        runCatching { importImage(it, if (part == "vendor" || part == "odm") part else "system/$part") }
                            .onFailure { err -> log("$part.img: ${err.message}") }
                    }
                }
                base.equals("payload.bin", true) -> withEntrySource(zip, e) { importPayload(it) }
                // Sony .ftf: system.sin is the system partition, kernel.sin the kernel with the ramdisk
                base.matches(Regex("(?i)(system|kernel|boot)\\.sin")) && !(gotSystem && base.startsWith("system", true)) ->
                    zip.getInputStream(e).use { importSin(it, base) }
                base.matches(Regex("(?i)vendor(\\.ext4)?\\.img")) ->
                    withEntrySource(zip, e) { runCatching { importImage(if (SparseSource.probe(it)) SparseSource(listOf(it)) else it, "vendor") } }
                base.lowercase().endsWith(".zip") && (base.startsWith("image-") || depth == 0 && e.size > 50_000_000) -> {
                    handleNestedZip(spillEntry(zip, e), base, depth)
                }
                base.matches(Regex("(?i).*\\.(tar|tar\\.md5|md5|tgz|tar\\.gz)")) 
                    // Odin: AP/PDA/CODE — система, BL/KERNEL/HOME — ядро с рамдиском; модем и CSC не нужны
                    && !base.startsWith("MODEM") && !base.startsWith("CP_") && !base.contains("CSC") -> {
                    zip.getInputStream(e).use { s ->
                        val st = if (base.endsWith("gz")) GZIPInputStream(s, 1 shl 16) else s
                        importTarStream(st, base, depth + 1)
                    }
                }
            }
            done += maxOf(0L, e.size)
            onProgress("Extracting: $base", 0.9f * done / total)
        }
        if (datBr != null && !gotSystem) {
            val listName = datBr.name.substringBeforeLast("system.new.dat") + "system.transfer.list"
            val listE = entries.firstOrNull { it.name == listName }
            onProgress("Building system.img from OTA", 0.5f)
            // A block OTA stores a stream that its transfer.list places into the image. A few vendors instead put the
            // finished file system into the data file and cut it into slices (vivo: 16 pieces of 475 MB, an EROFS
            // system in them, and a 423 byte list that indexes a different partition). Joining the pieces is then the
            // whole job, and running the list over it would write 34 blocks into an 8 GB image and lose the rest.
            val raw = File(tmp, "system.raw")
            fatEntryStream(zip, datBr).use { s ->
                s.copyTo(raw.outputStream().buffered(1 shl 20), 1 shl 20)
            }
            val isWholeImage = FileChannel.open(raw.toPath(), StandardOpenOption.READ).use { c ->
                val src = ChannelSource(c)
                ErofsReader.probe(src) || Ext4Reader.probe(src) || SuperImage.probe(src) || SparseSource.probe(src)
            }
            if (isWholeImage) {
                log("${datBr.name.substringAfterLast('/')}: the data file is a whole file system, the transfer.list is not used")
                FileChannel.open(raw.toPath(), StandardOpenOption.READ).use { c -> importImage(ChannelSource(c), "system") }
                raw.delete()
            } else {
                if (listE == null) { raw.delete(); throw IOException("system.transfer.list missing") }
                val list = zip.getInputStream(listE).use { String(it.readBytes()) }
                val img = File(tmp, "system.img")
                raw.inputStream().buffered(1 shl 20).use { s -> TransferList.build(list, s, img) }
                raw.delete()
                FileChannel.open(img.toPath(), StandardOpenOption.READ).use { c -> importImage(ChannelSource(c), "system") }
                img.delete()
            }
            // a Treble OTA (8.0+) carries the vendor partition the same way; the firmware's apps and overlays live there.
            // With dynamic partitions (10+) product, system_ext and odm come as their own data files too.
            for (p in listOf("vendor", "product", "system_ext", "odm")) {
                val pDat = entryNamed(entries, p) ?: continue
                val pList = entries.firstOrNull { it.name == listName.replace("system.transfer.list", "$p.transfer.list") }
                runCatching {
                    onProgress("Building $p.img from OTA", 0.8f)
                    val praw = File(tmp, "$p.raw")
                    fatEntryStream(zip, pDat).use { s ->
                        val data = if (pDat.name.endsWith(".br")) BrotliInputStream(BufferedInputStream(s, 1 shl 20)) else s
                        data.copyTo(praw.outputStream().buffered(1 shl 20), 1 shl 20)
                    }
                    val whole = FileChannel.open(praw.toPath(), StandardOpenOption.READ).use { c ->
                        val src = ChannelSource(c)
                        ErofsReader.probe(src) || Ext4Reader.probe(src) || SuperImage.probe(src) || SparseSource.probe(src)
                    }
                    if (whole) {
                        log("${pDat.name.substringAfterLast('/')}: a whole file system")
                        FileChannel.open(praw.toPath(), StandardOpenOption.READ).use { c ->
                            importImage(ChannelSource(c), if (p == "vendor" || p == "odm") p else "system/$p")
                        }
                    } else {
                        if (pList == null) { log("$p: transfer.list missing, skipped"); praw.delete(); return@runCatching }
                        val pimg = File(tmp, "$p.img")
                        praw.inputStream().buffered(1 shl 20).use { s ->
                            TransferList.build(zip.getInputStream(pList).use { String(it.readBytes()) }, s, pimg)
                        }
                        praw.delete()
                        FileChannel.open(pimg.toPath(), StandardOpenOption.READ).use { c ->
                            importImage(ChannelSource(c), if (p == "vendor" || p == "odm") p else "system/$p")
                        }
                        pimg.delete()
                    }
                }.onFailure { log("$p partition: ${it.message}") }
            }
        }
        if (sparseChunks.isNotEmpty() && !gotSystem) {
            val parts = sparseChunks.map { spillEntry(zip, it) }
            val chans = parts.map { FileChannel.open(it.toPath(), StandardOpenOption.READ) }
            try { importImage(SparseSource(chans.map { ChannelSource(it) }), "system") } finally { chans.forEach { it.close() }; parts.forEach { it.delete() } }
        }
        if (!names.any { it.startsWith("system/") } && !gotSystem) log("no system found in archive $name")
    }

    /** 7z: entries are read in order (solid archives cannot seek); interesting ones are unpacked like their zip twins */
    private fun importSevenZ(ch: FileChannel, name: String, depth: Int) {
        SevenZFile.builder().setSeekableByteChannel(ch).get().use { z ->
            val nested = ArrayList<Pair<File, String>>()
            // (a block OTA keeps its data and its transfer.list as separate entries: both have to be in the same
            // directory for the reader to pair them, so everything that is not a whole image goes to one place)
            while (true) {
                if (cancelled) throw IOException("cancelled")
                val e = z.nextEntry ?: break
                if (e.isDirectory) continue
                val n = e.name.replace('\\', '/')
                val base = n.substringAfterLast('/')
                val stream = z.getInputStream(e)
                when {
                    n.contains("__MACOSX/") || base.startsWith("._") -> {}
                    n.startsWith("system/") -> { writeFile(n, stream, null); gotSystem = true }
                    base.equals("boot.img", true) -> takeBoot(stream.readBytes())
                    base.equals("recovery.img", true) && e.size < 512_000_000 -> recovery = stream.readBytes()
                    base.matches(SYSTEM_IMAGE) -> nested.add(spill(stream, base) to "system")
                    base.matches(Regex("(?i)(system|kernel|boot)\\.sin")) -> importSin(stream, base)
                    base.matches(Regex("(?i).*\\.(zip|7z|rar|tar|tar\\.md5|md5|tgz|tar\\.gz|pac|ftf|kdz|dz|new\\.dat|transfer\\.list)")) && e.size > 20_000_000 &&
                        !base.startsWith("MODEM") && !base.startsWith("CP_") && !base.contains("CSC") ->
                        nested.add(spill(stream, base) to "archive")
                }
                onProgress("Extracting: $base", 0.5f)
            }
            for ((f, kind) in nested) {
                try {
                    FileChannel.open(f.toPath(), StandardOpenOption.READ).use { c ->
                        val src = ChannelSource(c)
                        if (kind == "system") importImage(src, "system")
                        else handle(src, c, f.name, depth + 1)
                    }
                } finally { f.delete() }
            }
            if (!gotSystem && nested.isEmpty()) log("no system found in archive $name")
        }
    }

    private fun withEntrySource(zip: ZipFile, e: ZipArchiveEntry, block: (RandomSource) -> Unit) {
        // несжатую запись читаем прямо из архива по смещению, сжатую — через временный файл
        val ch = zipChannel
        if (e.method == ZipArchiveEntry.STORED && ch != null) {
            runCatching { zip.getRawInputStream(e).close() } // вычисляет dataOffset
            if (e.dataOffset > 0) { block(ChannelSource(ch, e.dataOffset, e.size)); return }
        }
        val t = spillEntry(zip, e)
        deferred?.let { d -> d.add(t to e.name); return }
        // (kept until the whole package is done: an EROFS may continue into the image of another partition)
        partitionFiles.add(t)
        FileChannel.open(t.toPath(), StandardOpenOption.READ).use { c -> block(ChannelSource(c)) }
    }

    /** a nested zip: its images are copied out first, the zip deleted, then the images unpacked */
    private fun handleNestedZip(t: File, base: String, depth: Int) {
        val outer = deferred
        val mine = ArrayList<Pair<File, String>>()
        deferred = mine
        try {
            FileChannel.open(t.toPath(), StandardOpenOption.READ).use { c -> handle(ChannelSource(c), c, base, depth + 1) }
        } finally { deferred = outer; t.delete() }
        try {
            for ((f, entry) in mine) {
                val part = if (entry.substringAfterLast('/').lowercase().startsWith("vendor")) "vendor" else "system"
                FileChannel.open(f.toPath(), StandardOpenOption.READ).use { c ->
                    val src = ChannelSource(c)
                    val img = if (SparseSource.probe(src)) SparseSource(listOf(src)) else src
                    if (part == "vendor") runCatching { importImage(img, part) } else importImage(img, part)
                }
                f.delete()
            }
        } finally { mine.forEach { it.first.delete() } }
    }

    private fun spillEntry(zip: ZipFile, e: ZipArchiveEntry): File = zip.getInputStream(e).use { spill(it, e.name.substringAfterLast('/')) }

    private fun spill(i: InputStream, name: String): File {
        val t = File(tmp, "${System.nanoTime()}-$name")
        t.outputStream().use { o -> i.copyTo(o, 1 shl 20) }
        return t
    }

    /** [under]: the tar itself as a random-access source, when [s] reads it from its first byte (a tar that is the
     *  file being imported, not one inside a compressed stream or another archive) */
    private fun importTarStream(s: InputStream, name: String, depth: Int, under: RandomSource? = null) {
        val tar = TarArchiveInputStream(s, "UTF-8")
        var fullRoot: Boolean? = null
        while (true) {
            if (cancelled) throw IOException("cancelled")
            val e: TarArchiveEntry = tar.nextEntry ?: break
            var n = e.name.removePrefix("./").trimStart('/')
            if (n.isEmpty()) continue
            val base = n.substringAfterLast('/')
            // дерево rootfs целиком (system/, data/, dev/, …)
            if (fullRoot == null && (n == "system" || n.startsWith("system/") || n.startsWith("init.rc") || n.startsWith("default.prop"))) fullRoot = true
            when {
                (base.matches(SYSTEM_IMAGE) || base.matches(Regex("(?i)(system\\.img(\\.ext4)?|factoryfs\\.(img|rfs)|super\\.img)\\.lz4"))) && e.isFile -> {
                    if (under != null && !base.endsWith(".lz4", true)) {
                        // an Odin tar holds the image as it is: read in place, the phone needs no second copy of it
                        importImage(ExtentSource(under, listOf(tar.bytesRead to e.size)), "system")
                    } else {
                        val t = spill(maybeLz4(tar, base), base)
                        FileChannel.open(t.toPath(), StandardOpenOption.READ).use { c -> importImage(ChannelSource(c), "system") }
                        t.delete()
                    }
                }
                // other partitions as separate images (Samsung CSC: prism, optics; pre-super AP: vendor, product, odm)
                base.matches(PART_IMAGE) && e.isFile && e.size > 4096 -> {
                    val p = base.lowercase().substringBefore('.')
                    val mount = when (p) { "product", "system_ext" -> "system/$p"; else -> p }
                    runCatching {
                        val t = spill(maybeLz4(tar, base), base)
                        try { FileChannel.open(t.toPath(), StandardOpenOption.READ).use { c -> importImage(ChannelSource(c), mount) } } finally { t.delete() }
                    }.onFailure { log("$base skipped: ${it.message}") }
                }
                base.matches(Regex("(?i)(system|kernel|boot)\\.sin")) && e.isFile -> importSin(tar.nonClosing(), base)
                base.matches(Regex("(?i)(boot\\.img|zImage|kernel)(\\.lz4)?")) && e.isFile -> takeBoot(maybeLz4(tar, base).readBytes())
                base.matches(Regex("(?i)recovery\\.img(\\.lz4)?")) && e.isFile && e.size < 64_000_000 -> recovery = maybeLz4(tar, base).readBytes()
                base.matches(Regex("(?i).*\\.(tar|tar\\.md5)")) && e.isFile && e.size > 20_000_000 -> importTarStream(tar.nonClosing(), base, depth + 1)
                // factory-образы Google: tgz → image-*.zip → system.img/boot.img; Samsung: zip внутри tar
                base.lowercase().endsWith(".zip") && e.isFile && e.size > 20_000_000 -> {
                    handleNestedZip(spill(tar.nonClosing(), base), base, depth)
                }
                n.matches(Regex("^(system|data|dev|sbin|vendor|etc)(/.*)?$")) || n.matches(Regex("^[^/]+\\.rc$")) || n == "default.prop" || n.startsWith("dhd.") -> {
                    // файлы корня (рамдиск, dhd.*) берём, только если архив — целое дерево rootfs
                    if (fullRoot != true && !n.startsWith("system") && !(n.startsWith("dhd.") || n.endsWith(".rc") || n == "default.prop" || n.startsWith("sbin"))) continue
                    when {
                        e.isDirectory -> File(root, n).mkdirs()
                        e.isSymbolicLink -> symlinks.add(e.linkName to "/$n")
                        e.isLink -> hardlink(n, e.linkName)
                        e.isFile -> {
                            writeFile(n, tar.nonClosing(), e.mode)
                            if (n.startsWith("system/")) gotSystem = true
                        }
                    }
                }
                // TWRP system.ext4.win: пути без префикса system/
                name.contains("system", true) && name.endsWith(".win", true) -> {
                    val p = "system/$n"
                    when {
                        e.isDirectory -> File(root, p).mkdirs()
                        e.isSymbolicLink -> symlinks.add(e.linkName to "/$p")
                        e.isFile -> { writeFile(p, tar.nonClosing(), e.mode); gotSystem = true }
                    }
                }
            }
            if (files % 200 == 0) onProgress("Extracting: $base", -1f)
        }
    }

    private fun InputStream.nonClosing(): InputStream = object : java.io.FilterInputStream(this) { override fun close() {} }

    private fun maybeLz4(s: InputStream, name: String): InputStream =
        if (name.endsWith(".lz4", true)) org.apache.commons.compress.compressors.lz4.FramedLZ4CompressorInputStream(s.nonClosing()) else s.nonClosing()

    private fun hardlink(n: String, target: String) {
        val src = File(root, target.removePrefix("./").trimStart('/'))
        val dst = File(root, n)
        if (src.isFile) { dst.parentFile?.mkdirs(); src.copyTo(dst, overwrite = true) }
    }

    // ------------------------------------------------------------------ образы ФС

    /** A partition image of any supported file system: sparse → ext4, super.img (dynamic partitions), YAFFS2, FAT/RFS. */
    private fun importImage(src0: RandomSource, mount: String) {
        val src = if (SparseSource.probe(src0)) SparseSource(listOf(src0)) else src0
        when {
            Ext4Reader.probe(src) -> importExt4(src, mount)
            SuperImage.probe(src) -> {
                val parts = SuperImage.partitions(src)
                log("super.img: ${parts.keys.joinToString()}")
                fun pick(n: String) = parts[n]?.takeIf { it.isNotEmpty() } ?: parts["${n}_a"]?.takeIf { it.isNotEmpty() }
                importImage(ExtentSource(src, pick("system") ?: throw IOException("super.img without a system partition")), "system")
                for (p in listOf("vendor", "product", "system_ext", "odm")) pick(p)?.let { e ->
                    runCatching { importImage(ExtentSource(src, e), if (p == "vendor" || p == "odm") p else "system/$p") }
                        .onFailure { log("super.img: $p skipped: ${it.message}") }
                }
                for (p in parts.keys.map { it.removeSuffix("_a") }.filter { it.matches(SKIN_PARTITION) }.distinct()) pick(p)?.let { e ->
                    runCatching { importImage(ExtentSource(src, e), p) }.onFailure { log("super.img: $p skipped: ${it.message}") }
                }
            }
            Yaffs2Image.probe(src) -> importTree(Yaffs2Image(src), mount, "YAFFS2")
            FatImage.probe(src) -> importTree(FatImage(src), mount, "FAT/RFS")
            ErofsReader.probe(src) -> importTree(ErofsImage(src, extraDeviceSources()), mount, "EROFS")
            else -> throw IOException("unknown file system in the $mount image")
        }
    }

    /**
     * The other images of the same package, for an EROFS whose device list points outside itself (a system that was
     * built together with system_ext and keeps the tail of its file system there). The images already unpacked into
     * the temp directory are offered in the order they were written; one that does not continue the file system is
     * never read from.
     */
    private fun extraDeviceSources(): List<RandomSource> {
        val out = ArrayList<RandomSource>()
        for (f in partitionFiles) {
            if (!f.isFile || f.length() < 4096) continue
            runCatching {
                val ch = FileChannel.open(f.toPath(), StandardOpenOption.READ)
                heldChannels.add(ch)
                out.add(ChannelSource(ch))
            }
        }
        return out
    }

    /** image files of this package kept around while a multi device file system is read */
    private val partitionFiles = ArrayList<File>()
    private val heldChannels = ArrayList<FileChannel>()

    private fun closeHeld() {
        heldChannels.forEach { runCatching { it.close() } }
        heldChannels.clear()
    }

    private fun importKdz(src: RandomSource) {
        val layout = KdzImage.layout(src)
        log("kdz: ${layout.parts.size} partitions (${layout.extraChunks} extra chunks)")
        for (p in layout.parts) {
            val base = p.name.substringAfterLast('/').substringBefore('.').lowercase()
            val mount = when {
                base.startsWith("system") -> "system"
                base.startsWith("vendor") -> "vendor"
                else -> continue
            }
            onProgress("Unpacking ${p.name}", -1f)
            val out = File(tmp, "${System.nanoTime()}-$base.img")
            runCatching {
                KdzImage.extract(src, p, out)
                FileChannel.open(out.toPath(), StandardOpenOption.READ).use { c -> importImage(ChannelSource(c), mount) }
            }.onFailure { log("kdz ${p.name}: ${it.message}") }
            out.delete()
        }
    }
    /** the newer LG kdz (v1/v2/v3): the container points at a dz, the dz holds the partitions in chunks */
    private fun importKdz2(src: RandomSource) {
        val (dz, dzName) = KdzImage.dzOfKdz2(src) ?: throw IOException("kdz: no dz record in the container")
        log("kdz: $dzName")
        importDz2(dz)
    }
    /** the newer LG dz: chunk headers with sectors, the data zlib compressed */
    private fun importDz2(src: RandomSource) {
        val chunks = KdzImage.dz2Chunks(src)
        val parts = chunks.map { it.part }.distinct()
        log("dz: ${chunks.size} chunks over ${parts.size} partitions: ${parts.joinToString().take(200)}")
        val files = KdzImage.extractDz2(src, chunks, setOf("system", "vendor"), tmp) { p -> onProgress("Unpacking $p", -1f) }
        for (f in files) {
            val base = f.name.substringBefore('.').lowercase()
            val mount = when {
                base.startsWith("system") -> "system"
                base.startsWith("vendor") -> "vendor"
                else -> continue
            }
            onProgress("Importing ${f.name}", -1f)
            runCatching {
                FileChannel.open(f.toPath(), StandardOpenOption.READ).use { c -> importImage(ChannelSource(c), mount) }
            }.onFailure { log("dz ${f.name}: ${it.message}") }
            f.delete()
        }
    }

    private fun importTree(tree: TreeSource, mount: String, kind: String) {
        onProgress("Reading image $mount ($kind)", -1f)
        var n = 0
        tree.walk { path, isDir, link, mode, size, data ->
            if (cancelled) throw IOException("cancelled")
            val rel = "$mount/$path"
            val f = File(root, rel)
            if (!f.canonicalPath.startsWith(root.canonicalPath)) return@walk
            when {
                isDir -> f.mkdirs()
                link != null -> symlinks.add(link to "/$rel")
                data != null -> {
                    f.parentFile?.mkdirs()
                    data().use { i -> f.outputStream().buffered(1 shl 20).use { o -> i.copyTo(o, 1 shl 16) } }
                    applyMode(f, mode and 0xfff)
                    files++; bytes += size
                }
            }
            if (++n % 150 == 0) onProgress("$mount: ${path.substringAfterLast('/')}", -1f)
        }
        if (mount == "system") { gotSystem = true; hoistSystemAsRoot() }
        log("image $mount ($kind): $n objects")
    }

    /** Sony SIN v3: unpacked into a sparse temporary image, then read like any partition image. */
    private fun importSin(s: InputStream, base: String) {
        val raw = File(tmp, "${System.nanoTime()}-$base.img")
        try {
            onProgress("Unpacking $base", -1f)
            // the system partition is read out of the SIN itself (a copy of the SIN takes far less room than the
            // unpacked partition); the layouts that cannot be read in place are unpacked as before
            if (base.startsWith("system", true)) {
                s.use { i -> raw.outputStream().use { o -> i.copyTo(o, 1 shl 20) } }
                if (cancelled) throw IOException("cancelled")
                val inPlace = FileChannel.open(raw.toPath(), StandardOpenOption.READ).use { c ->
                    SinImage.source(c)?.also { importImage(it, "system") } != null
                }
                if (inPlace) return
                val sin = File(tmp, "${System.nanoTime()}-$base")
                if (!raw.renameTo(sin)) throw IOException("cannot move $raw")
                try { sin.inputStream().use { SinImage.unpack(it, raw) { if (cancelled) throw IOException("cancelled") } } } finally { sin.delete() }
            } else
            SinImage.unpack(s, raw) { if (cancelled) throw IOException("cancelled") }
            if (base.startsWith("kernel", true) || base.startsWith("boot", true)) { takeBoot(raw.readBytes()); return }
            if (base.startsWith("recovery", true) || base.startsWith("fotakernel", true)) return
            FileChannel.open(raw.toPath(), StandardOpenOption.READ).use { c -> importImage(ChannelSource(c), base.substringBefore('.').lowercase()) }
        } finally { raw.delete() }
    }

    /** Spreadtrum .pac: partition files are slices of the package; system, boot and recovery are taken from it. */
    private fun importPac(src: RandomSource, ch: FileChannel) {
        val list = PacFile.entries(src)
        log("PAC: ${list.joinToString { "${it.id}=${it.name}" }}")
        fun slice(e: PacFile.Entry) = ChannelSource(ch, e.offset, e.size)
        fun isPart(e: PacFile.Entry, p: String) = e.id.equals(p, true) || e.name.substringBefore('.').equals(p, true)
        list.firstOrNull { isPart(it, "boot") }?.let { e ->
            val b = ByteBuffer.allocate(e.size.toInt()); slice(e).read(0, b); takeBoot(b.array())
        }
        list.firstOrNull { isPart(it, "recovery") }?.takeIf { it.size < 64_000_000 }?.let { e ->
            val b = ByteBuffer.allocate(e.size.toInt()); slice(e).read(0, b); recovery = b.array()
        }
        val sys = list.firstOrNull { isPart(it, "system") } ?: list.firstOrNull { isPart(it, "super") }
            ?: throw IOException("PAC without a system image")
        importImage(slice(sys), "system")
        list.firstOrNull { isPart(it, "vendor") }?.let { e -> runCatching { importImage(slice(e), "vendor") } }
    }

    /** A full A/B OTA: every partition with files of the firmware is taken out of payload.bin in turn and read. */
    private fun importPayload(src: RandomSource) {
        log("OTA payload.bin")
        // (the vendor partitions of a skin are part of the system tree: my_stock of OPPO is 3.6 GB of it, so they
        // are taken beside system, before the small fixed ones)
        val skin = PayloadReader.partitionNames(src).map { it.removeSuffix("_a") }
            .filter { it.matches(SKIN_PARTITION) }.distinct()
            .sortedByDescending { n ->
                // largest first: an aborted import then fails late, not after the important partitions
                runCatching { PayloadReader.partitionSize(src, n) }.getOrDefault(0L)
            }
        for ((part, mount) in listOf("system" to "system") +
            skin.map { it to it } +
            listOf("vendor" to "vendor", "odm" to "odm", "product" to "system/product", "system_ext" to "system/system_ext")) {
            val img = File(tmp, "${System.nanoTime()}-$part.img")
            try {
                val ok = PayloadReader.extractPartition(src, part, img) { p -> onProgress("Extracting $part from payload.bin", p) }
                if (!ok) { if (part == "system") throw IOException("payload.bin without a system partition"); continue }
                FileChannel.open(img.toPath(), StandardOpenOption.READ).use { c ->
                    if (part == "system") importImage(ChannelSource(c), mount)
                    else runCatching { importImage(ChannelSource(c), mount) }.onFailure { log("payload.bin: $part skipped: ${it.message}") }
                }
            } finally {
                img.delete()
            }
        }
    }

    private fun importExt4(src: RandomSource, mount: String) {
        val fs = Ext4Reader(src)
        onProgress("Reading image $mount (ext4, block ${fs.blockSize})", -1f)
        var n = 0
        fs.walk { path, node ->
            if (cancelled) throw IOException("cancelled")
            val rel = "$mount/$path"
            val f = File(root, rel)
            when {
                node.isDir -> f.mkdirs()
                node.isLink -> symlinks.add(fs.linkTarget(node) to "/$rel")
                node.isFile -> {
                    f.parentFile?.mkdirs()
                    f.outputStream().buffered(1 shl 20).use { fs.copy(node, it) }
                    applyMode(f, node.perm)
                    files++; bytes += node.size
                }
            }
            if (++n % 150 == 0) onProgress("$mount: ${path.substringAfterLast('/')}", -1f)
        }
        if (mount == "system") { gotSystem = true; hoistSystemAsRoot() }
        log("image $mount: $n objects")
    }

    /**
     * A "system-as-root" image (every A/B device, and all of them from Android 10) holds the whole root directory, with
     * the system partition's files in its /system. It was read under <root>/system like a plain system image, so it is
     * moved one level up: <root>/system/system becomes <root>/system, and the links it brought (/bin, /etc, /product,
     * /system_ext, /init...) land in the root where they belong.
     */
    private fun hoistSystemAsRoot() {
        val outer = File(root, "system")
        val inner = File(outer, "system")
        if (!File(inner, "framework").isDirectory && !File(inner, "build.prop").isFile) return
        val aside = File(root, ".system-as-root")
        if (!outer.renameTo(aside)) { log("system-as-root: cannot move the tree"); return }
        aside.listFiles()?.forEach { f ->
            val dst = File(root, f.name)
            if (dst.exists() && dst.isDirectory && f.isDirectory) { f.listFiles()?.forEach { c -> c.renameTo(File(dst, c.name)) }; f.delete() }
            else f.renameTo(dst)
        }
        aside.delete()
        for (i in symlinks.indices) {
            val (target, link) = symlinks[i]
            if (link.startsWith("/system/")) symlinks[i] = target to link.removePrefix("/system")
        }
        log("system-as-root image: the tree is taken as the root directory")
    }

    private fun takeBoot(data: ByteArray) {
        if (ramdisk != null) return
        val rd = runCatching { BootImage.ramdisk(data) }.getOrNull()
        if (rd.isNullOrEmpty()) { log("boot: ramdisk not recognized"); return }
        ramdisk = rd
        log("boot: ramdisk, ${rd.size} files")
    }

    // ------------------------------------------------------------------ файлы, ссылки, права

    private fun writeFile(rel: String, i: InputStream, mode: Int?) {
        val f = File(root, rel)
        if (!f.canonicalPath.startsWith(root.canonicalPath)) return // защита от ../ в архиве
        f.parentFile?.mkdirs()
        if (runCatching { android.system.OsConstants.S_ISLNK(Os.lstat(f.path).st_mode) }.getOrDefault(false)) f.delete()
        f.outputStream().buffered(1 shl 20).use { o -> bytes += i.copyTo(o, 1 shl 16) }
        if (mode != null) applyMode(f, mode and 0xfff)
        files++
    }

    private fun applyMode(f: File, perm: Int) {
        if (perm and 0x49 != 0) f.setExecutable(true, false)
        f.setReadable(true, false)
    }

    private fun parseUpdaterScript(s: String) {
        val text = s.replace(Regex("#[^\n]*"), "")
        for (m in Regex("symlink\\s*\\(([^;]*?)\\)\\s*;", RegexOption.DOT_MATCHES_ALL).findAll(text)) {
            val args = Regex("\"([^\"]*)\"").findAll(m.groupValues[1]).map { it.groupValues[1] }.toList()
            if (args.size >= 2) for (link in args.drop(1)) symlinks.add(args[0] to link)
        }
        for (m in Regex("set_perm_recursive\\s*\\(([^;]*?)\\)\\s*;", RegexOption.DOT_MATCHES_ALL).findAll(text)) {
            val a = m.groupValues[1].split(',').map { it.trim().trim('"') }
            if (a.size >= 5) a[3].toIntOrNull(8)?.let { perms.add(Triple(a[4], it, true)) }
        }
        for (m in Regex("set_perm\\s*\\(([^;]*?)\\)\\s*;", RegexOption.DOT_MATCHES_ALL).findAll(text)) {
            val a = m.groupValues[1].split(',').map { it.trim().trim('"') }
            if (a.size >= 4) a[2].toIntOrNull(8)?.let { perms.add(Triple(a[3], it, false)) }
        }
        for (m in Regex("set_metadata(_recursive)?\\s*\\(([^;]*?)\\)\\s*;", RegexOption.DOT_MATCHES_ALL).findAll(text)) {
            val a = m.groupValues[2].split(',').map { it.trim().trim('"') }
            if (a.isEmpty()) continue
            val key = if (m.groupValues[1].isNotEmpty()) "fmode" else "mode"
            val i = a.indexOf(key)
            if (i > 0 && i + 1 < a.size) a[i + 1].toIntOrNull(8)?.let { perms.add(Triple(a[0], it, m.groupValues[1].isNotEmpty())) }
        }
        log("updater-script: ${symlinks.size} symlinks, ${perms.size} perms")
    }

    /** Ссылки (с переводом абсолютных целей в относительные), права, служебные ссылки корня. */
    private fun finishTree() {
        // рамдиск: init*.rc, default.prop, sbin/, file_contexts…
        ramdisk?.forEach { e ->
            val type = e.mode and 0xF000
            val n = e.name
            if (n.isEmpty() || n == "." || n.startsWith("system/") || n.startsWith("data/") || n.startsWith("dev/") || n.startsWith("proc") || n.startsWith("sys/")) return@forEach
            val f = File(root, n)
            when (type) {
                0x4000 -> f.mkdirs()
                0xA000 -> symlinks.add(String(e.data) to "/$n")
                0x8000 -> {
                    if (n == "init" || n.startsWith("sbin/ueventd") || n.startsWith("sbin/adbd") || n == "charger") return@forEach
                    f.parentFile?.mkdirs(); f.writeBytes(e.data)
                    applyMode(f, e.mode and 0xfff)
                }
            }
        }
        var made = 0
        for ((target, link) in symlinks) {
            val rel = link.trimStart('/')
            val f = File(root, rel)
            if (!f.canonicalPath.startsWith(root.canonicalPath)) continue
            f.parentFile?.mkdirs()
            val t = relTarget(link, target)
            runCatching {
                if (f.exists() || isLink(f)) { if (f.isDirectory && !isLink(f)) return@runCatching; f.delete() }
                Os.symlink(t, f.absolutePath); made++
            }
        }
        for ((p, mode, rec) in perms) {
            val f = File(root, p.trimStart('/'))
            if (rec) f.walkTopDown().filter { it.isFile }.forEach { applyMode(it, mode) } else if (f.isFile) applyMode(f, mode)
        }
        // исполняемые — всё в bin/xbin/sbin
        for (d in listOf("system/bin", "system/xbin", "sbin", "vendor/bin", "system/vendor/bin")) {
            File(root, d).listFiles()?.forEach { if (it.isFile) it.setExecutable(true, false) }
        }
        standardLinks()
        fun rootLink(name: String, target: String) {
            val f = File(root, name)
            if (!f.exists() && !isLink(f) && File(root, target).exists()) runCatching { Os.symlink(target, f.absolutePath) }
        }
        rootLink("vendor", "system/vendor")
        rootLink("etc", "system/etc")
        rootLink("bin", "system/bin")
        runCatching { File(root, "system/etc/mtab").let { if (!it.exists() && !isLink(it)) Os.symlink("../../proc/mounts", it.absolutePath) } }
        log("symlinks created: $made")
    }

    private fun isLink(f: File) = runCatching { android.system.OsConstants.S_ISLNK(Os.lstat(f.absolutePath).st_mode) }.getOrDefault(false)

    /** Абсолютную цель ссылки гостя делаем относительной — иначе ядро телефона уйдёт за пределы дерева. */
    /**
     * Архивы без updater-script и без ссылок в самом архиве (multirom, выгрузки system/ из TWRP) не
     * содержат sh → mksh и ссылок на апплеты toolbox — без них не стартует ни одна служба init.
     * Создаём их сами, если их нет: апплет берём, только если его имя есть в самом toolbox.
     */
    private fun standardLinks() {
        val bin = File(root, "system/bin")
        fun missing(n: String) = File(bin, n).let { !it.exists() && !isLink(it) }
        var made = 0
        if (missing("sh")) {
            val sh = listOf("mksh", "ash").firstOrNull { File(bin, it).isFile }
            if (sh != null && runCatching { Os.symlink(sh, File(bin, "sh").absolutePath) }.isSuccess) made++
        }
        val toolbox = File(bin, "toolbox")
        if (toolbox.isFile) {
            val text = runCatching { String(toolbox.readBytes(), Charsets.ISO_8859_1) }.getOrDefault("")
            for (a in TOOLBOX_APPLETS) {
                if (!missing(a)) continue
                if (!text.contains("\u0000$a\u0000")) continue
                if (runCatching { Os.symlink("toolbox", File(bin, a).absolutePath) }.isSuccess) made++
            }
        }
        if (made > 0) log("sh/toolbox symlinks created: $made (missing from archive)")
    }

    private fun relTarget(link: String, target: String): String {
        if (!target.startsWith("/")) return target
        val from = link.trimStart('/').split('/').dropLast(1)
        val to = target.trimStart('/').split('/').filter { it.isNotEmpty() }
        var i = 0
        while (i < from.size && i < to.size && from[i] == to[i]) i++
        val ups = List(from.size - i) { ".." }
        return (ups + to.drop(i)).joinToString("/").ifEmpty { "." }
    }

    companion object {
        /** partition images other than system inside a tar (taken after system, which SYSTEM_IMAGE catches first) */
        private val PART_IMAGE = Regex("(?i)(vendor|product|odm|prism|optics)\\.img(\\.ext4)?(\\.lz4)?")
        /** system partition images by name (ext4/sparse, Samsung RFS, YAFFS2 dumps, super.img of 10+, erofs, f2fs) */
        private val SYSTEM_IMAGE = Regex("(?i)system(_[a-z0-9]+)?(\\.ext4|\\.erofs|\\.f2fs)?\\.img(\\.ext4)?|" +
            "system_image\\.img|system\\.raw\\.img|factoryfs\\.(img|rfs)|" +
            "system\\.(rfs|yaffs2?|ext4|erofs|f2fs)|super(\\.raw)?\\.img|system_[ab]\\.img")
        /** partitions a vendor's skin adds beside the AOSP ones, mounted at the root under their own name: Xiaomi
         *  mi_ext (HyperOS apps and ro.mi.os.*), OPPO/realme/OnePlus my_*, vivo vgc */
        private val SKIN_PARTITION = Regex("mi_ext|my_[a-z]+|vgc|prism|optics")
        /** Апплеты toolbox Android 2.3–6.0 (ссылка создаётся, только если апплет есть в бинарнике). */
        private val TOOLBOX_APPLETS = listOf(
            "cat", "chcon", "chmod", "chown", "clear", "cmp", "cp", "date", "dd", "df", "dmesg", "du", "getenforce",
            "getevent", "getprop", "getsebool", "grep", "hd", "id", "ifconfig", "iftop", "insmod", "ioctl", "ionice",
            "kill", "ln", "load_policy", "log", "ls", "lsmod", "lsof", "md5", "mkdir", "mkswap", "mount", "mv",
            "nandread", "netstat", "newfs_msdos", "nohup", "notify", "printenv", "ps", "readlink", "renice",
            "restorecon", "rm", "rmdir", "rmmod", "route", "runcon", "schedtop", "sendevent", "setenforce",
            "setprop", "setsebool", "sleep", "smd", "start", "stop", "swapoff", "swapon", "sync", "top", "touch",
            "umount", "uptime", "vmstat", "watchprops", "wipe", "chroot", "sh",
        ).filter { it != "sh" }
    }
}
