package app.aemu.importer
import java.io.RandomAccessFile
import java.io.File
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder
/**
 * LG KDZ, the firmware container of the LG update tool (a .kdz file, or a .dz extracted from one).
 *
 * The file starts with a 272 byte header: a magic, then a list of up to five partitions, each a name and the number
 * of 512 byte chunks it spans, plus the number of extra chunks after the partitions. The payload follows the header,
 * one chunk after another in the order the header lists them, each partition compressed on its own. The compressed
 * data is a dz (deflate) stream that ends with an MD5 of the plain data; the plain data is the partition image.
 *
 * The newer LG firmwares (2015 and later: X Cam, Magna, G5 and on) moved to a different layout that this also
 * reads. A kdz of v1/v2/v3 keeps its parts in records after the header and points at the one that holds a dz; the
 * dz itself starts with the magic 0x74189632, a 512 byte header, and a chunk header per data block (124 bytes in
 * v0, 512 in v1). Each chunk is zlib compressed and carries the sector it belongs to, so a partition is rebuilt
 * as a sparse image: the chunks are written at their sector and the gaps in between stay holes, which is exactly
 * what an image whose firmware left the empty ranges out looks like.
 */
object KdzImage {
    private const val HEADER = 272
    private const val CHUNK = 512
    private val MAGIC_KDZ = byteArrayOf(0x28, 0x05, 0x00, 0x00)
    private val MAGIC_DZ = byteArrayOf(0x32, 0x05, 0x00, 0x00)
    class Part(val name: String, var offset: Long, var chunks: Int) {
        val compressed get() = chunks.toLong() * CHUNK
    }
    /** [source] is the whole container, [parts] the partitions in the order their data appears */
    class Layout(val headerSize: Int, val parts: List<Part>, val extraChunks: Int)

    fun probe(src: RandomSource): Boolean {
        if (src.size < HEADER) return false
        val b = ByteBuffer.allocate(4)
        src.read(0, b); b.flip()
        val m = ByteArray(4).also { b.get(it) }
        return m.contentEquals(MAGIC_KDZ) || m.contentEquals(MAGIC_DZ)
    }
    /** the newer dz layout: magic 0x74189632, a 512 byte header, then a chunk header per data block */
    fun probeDz2(src: RandomSource): Boolean {
        if (src.size < 512) return false
        val b = ByteBuffer.allocate(4)
        src.read(0, b); b.flip()
        val m = ByteArray(4).also { b.get(it) }
        return m.contentEquals(byteArrayOf(0x32, 0x96, 0x18, 0x74))
    }
    /** the kdz container of v1/v2/v3: magic 0x50447932, 0x80253134 or 0x25223824, followed by file records */
    fun probeKdz2(src: RandomSource): Boolean {
        if (src.size < 512) return false
        val b = ByteBuffer.allocate(8)
        src.read(0, b); b.flip()
        val m = ByteArray(8).also { b.get(it) }
        if (!m.contentEquals(byteArrayOf(0x18, 0x79, 0x44, 0x50))) return false  // 0x50447932 little-endian
        val size = ByteBuffer.wrap(m, 4, 4).order(ByteOrder.LITTLE_ENDIAN).int
        return size == 1304 || size == 1320
    }

    /** the newer kdz container: the record that holds the dz, or null when there is none */
    fun dzOfKdz2(src: RandomSource): Pair<RandomSource, String>? {
        val head = ByteBuffer.allocate(1320)
        src.read(0, head); head.flip()
        val b = ByteArray(head.remaining()).also { head.get(it) }
        val hdrSize = ByteBuffer.wrap(b, 0, 4).order(ByteOrder.LITTLE_ENDIAN).int
        if (hdrSize !in 1304..1320) throw IOException("kdz: header size $hdrSize")
        // the records follow the header: 264 bytes each in v1, 272 in v2/v3; the first is the dz, the rest dlls
        val recSize = if (hdrSize == 1304) 264 else 272
        val count = (hdrSize - 8) / recSize
        for (i in 0 until count) {
            val at = 8 + i * recSize
            val name = str(b, at, 256)
            if (name.isBlank()) continue
            val size: Long
            val off: Long
            if (recSize == 264) {
                size = u32(b, at + 256)
                off = u32(b, at + 260)
            } else {
                size = u64(b, at + 256)
                off = u64(b, at + 264)
            }
            if (size <= 0 || off <= 0 || off + size > src.size) continue
            if (name.endsWith(".dz", true)) return SlicedSource(src, off, size) to name
        }
        return null
    }
    /** one part of a newer dz: a chunk header that says where its data belongs */
    class Dz2Chunk(val part: String, val off: Long, val csize: Int, val usize: Int, val sector: Long, val sparse: Boolean)
    /** the chunk headers of a newer dz, in the order their data appears in the file */
    fun dz2Chunks(src: RandomSource): List<Dz2Chunk> {
        val head = ByteBuffer.allocate(512)
        src.read(0, head); head.flip()
        val b = ByteArray(head.remaining()).also { head.get(it) }
        if (!b.copyOfRange(0, 4).contentEquals(byteArrayOf(0x32, 0x96, 0x18, 0x74))) throw IOException("dz: not a v2 dz")
        // part_count sits after magic+version+reserved+model_name+sw_version+build_date = 4+4+4+4+32+128+16
        val partCount = u32(b, 192).toInt()
        if (partCount <= 0 || partCount > 200_000) throw IOException("dz: chunk count $partCount")
        // the chunk headers may be the wider v1 ones: told apart by the magic at the start of each
        val probe = ByteBuffer.allocate(4)
        src.read(512, probe); probe.flip()
        val pm = ByteArray(4).also { probe.get(it) }
        val v1 = pm.contentEquals(byteArrayOf(0x30, 0x12, 0x95, 0x78))
        var dataOff = 512L + partCount.toLong() * (if (v1) 512L else 124L)
        val out = ArrayList<Dz2Chunk>(partCount)
        var p = 512L
        for (i in 0 until partCount) {
            val h = ByteBuffer.allocate(if (v1) 512 else 124)
            src.read(p, h); h.flip()
            val hb = ByteArray(h.remaining()).also { h.get(it) }
            if (!hb.copyOfRange(0, 4).contentEquals(byteArrayOf(0x30, 0x12, 0x95, 0x78))) throw IOException("dz: chunk header magic at $p")
            // v0: magic(4) part_name(32) chunk_name(64) decompressed_size(4)@100 compressed_size(4)@104 hash(16)
            // v1 adds start_sector(4)@124 sector_count@128 hw_partition@132 crc@136 unique_part_id@140
            //     is_sparse@144 is_ubi@148 part_start_sector@152
            val part = str(hb, 4, 32)
            val usize = u32(hb, 100).toInt()
            val csize = u32(hb, 104).toInt()
            val sector = if (v1) u32(hb, 124) else 0L
            val sparse = v1 && u32(hb, 144) != 0L
            if (csize <= 0 || csize > (64L shl 20)) throw IOException("dz: chunk size $csize at $p")
            out += Dz2Chunk(part, dataOff, csize, usize, sector, sparse)
            dataOff += csize
            p += if (v1) 512L else 124L
        }
        return out
    }
    /** rebuilds the wanted partitions of a newer dz: every chunk is written at the sector its header names */
    fun extractDz2(src: RandomSource, chunks: List<Dz2Chunk>, wanted: Set<String>, outDir: File, progress: (String) -> Unit): List<File> {
        val out = ArrayList<File>()
        for ((part, list) in chunks.groupBy { it.part }) {
            if (wanted.none { part.startsWith(it, true) }) continue
            val f = File(outDir, "${part.replace(Regex("[^A-Za-z0-9._-]"), "_")}.img")
            RandomAccessFile(f, "rw").use { raf ->
                raf.setLength(0)
                for (c in list) {
                    progress(part)
                    val comp = ByteArray(c.csize)
                    src.read(c.off, ByteBuffer.wrap(comp))
                    val inf = java.util.zip.Inflater()
                    try {
                        inf.setInput(comp)
                        val plain = ByteArray(c.usize)
                        var n = 0
                        while (n < plain.size && !inf.finished()) {
                            val k = inf.inflate(plain, n, plain.size - n)
                            if (k == 0) break
                            n += k
                        }
                        // a chunk that names its sector is one piece of a sparse image: it goes there, and the
                        // ranges no chunk claims stay holes; the ones of a plain partition simply follow each other
                        val at = if (c.sector > 0) c.sector * 512 else raf.length()
                        raf.seek(at)
                        raf.write(plain, 0, n)
                    } finally { inf.end() }
                }
            }
            out += f
        }
        return out
    }
    private fun str(b: ByteArray, at: Int, len: Int): String {
        var end = at
        while (end < at + len && end < b.size && b[end] != 0.toByte()) end++
        return String(b, at, minOf(end - at, len), Charsets.ISO_8859_1)
    }
    private fun u32(b: ByteArray, at: Int) = ByteBuffer.wrap(b, at, 4).order(ByteOrder.LITTLE_ENDIAN).int.toLong() and 0xffffffffL
    private fun u64(b: ByteArray, at: Int) = ByteBuffer.wrap(b, at, 8).order(ByteOrder.LITTLE_ENDIAN).long
    /** a source that reads from [off, off+size) of another one */
    class SlicedSource(private val src: RandomSource, private val off: Long, override val size: Long) : RandomSource {
        override fun read(pos: Long, dst: ByteBuffer) {
            val lim = dst.limit()
            val can = minOf(dst.remaining().toLong(), size - pos).coerceAtLeast(0).toInt()
            if (can == 0) { while (dst.hasRemaining()) dst.put(0); return }
            dst.limit(dst.position() + can)
            src.read(off + pos, dst)
            dst.limit(lim)
        }
    }

    fun layout(src: RandomSource): Layout = layoutFrom(bytes(src, 0, HEADER))

    /** the header of a kdz/dz, whether it came from a file or from inside an archive */
    fun layoutFrom(head: ByteArray): Layout {
        if (head.size < HEADER) throw IOException("dz: header too short")
        val kdz = head[0].toInt() == 0x28 && head[1].toInt() == 0x05
        val dz = head[0].toInt() == 0x32 && head[1].toInt() == 0x05
        if (!kdz && !dz) throw IOException("not a dz/kdz")
        // kdz keeps its own name and the name of the file it carries in front of the partition table
        var p = if (kdz) 4 else 0
        if (kdz) { p += 1 + (head[p].toInt() and 0xff) } // the kdz file name, length prefixed
        val headerSize = if (kdz) { p + 1 } else 256
        if (headerSize > head.size) throw IOException("dz: header size $headerSize past the buffer")
        var q = if (kdz) p else 0
        val parts = ArrayList<Part>()
        var off = headerSize.toLong()
        for (i in 0 until 5) {
            if (q + 1 > head.size) break
            val nameLen = head[q].toInt() and 0xff
            if (nameLen == 0 || q + 1 + nameLen + 4 > head.size) break
            val name = String(head, q + 1, nameLen, Charsets.ISO_8859_1).trim()
            val chunks = ByteBuffer.wrap(head, q + 1 + nameLen, 4).order(ByteOrder.LITTLE_ENDIAN).int
            if (chunks <= 0) break
            parts.add(Part(name, off, chunks))
            off += chunks.toLong() * CHUNK
            q += 1 + nameLen + 4
        }
        val extraAt = if (kdz) p + 1 + 200 + 4 else 256 + 4
        val extra = if (extraAt + 4 <= head.size)
            ByteBuffer.wrap(head, extraAt, 4).order(ByteOrder.LITTLE_ENDIAN).int else 0
        return Layout(headerSize, parts, extra)
    }

    /**
     * Writes a partition out as a plain image. The bytes are a deflate stream that is fed to the inflater in pieces,
     * so a partition of a few gigabytes does not have to fit in memory.
     */
    fun extract(src: RandomSource, part: Part, out: File, chunks: Int = part.chunks) {
        val total = minOf(chunks, part.chunks).toLong() * CHUNK
        val end = minOf(part.offset + total, src.size)
        val inf = java.util.zip.Inflater()
        val inBuf = ByteArray(1 shl 20)
        val outBuf = ByteArray(1 shl 20)
        out.outputStream().buffered(1 shl 20).use { o ->
            var pos = part.offset
            while (pos < end && !inf.finished()) {
                val n = minOf(inBuf.size.toLong(), end - pos).toInt()
                src.read(pos, ByteBuffer.wrap(inBuf, 0, n))
                pos += n
                inf.setInput(inBuf, 0, n)
                while (!inf.needsInput() && !inf.finished()) {
                    val k = inf.inflate(outBuf)
                    if (k == 0) break
                    o.write(outBuf, 0, k)
                }
            }
        }
        inf.end()
    }

    private fun bytes(src: RandomSource, pos: Long, n: Int): ByteArray {
        val b = ByteBuffer.allocate(n)
        src.read(pos, b)
        return b.array()
    }
}