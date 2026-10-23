package app.aemu.importer

import java.io.File
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.zip.Inflater

/**
 * Huawei/Honor UPDATE.APP, the container an SD card package keeps its partitions in (update_sd_base.zip of the
 * firmware downloads of hihonor.com, MagicOS and EMUI alike).
 *
 * There is no table of contents at the front or a terminator at the end: the file is a run of sections, and a section
 * is a fixed 92 byte header followed by its data. So the sections are found by their header shape - the length field
 * is 92 and the date field holds an ASCII time stamp - and a section runs from its header to the next one. The data is
 * either deflated (zlib) or stored.
 */
object HuaweiApp {
    private const val HEADER = 0x5C
    private const val STAMP_AT = 0x14
    private const val SIZE_AT = 0x10

    /** [offset] is the header, [end] the start of the next section (or the end of the file) */
    class Section(val offset: Long, val end: Long, val size: Long, val stamp: String, val crc: Int) {
        val dataOffset get() = offset + HEADER
        val compressed get() = end - dataOffset
    }

    fun probe(src: RandomSource): Boolean {
        if (src.size < HEADER) return false
        val h = ByteBuffer.allocate(HEADER).order(ByteOrder.LITTLE_ENDIAN)
        src.read(0, h); h.flip()
        return h.getInt(4) == HEADER && looksLikeStamp(h, STAMP_AT)
    }

    /** every section header in the file, in order */
    fun sections(src: RandomSource): List<Section> {
        val found = ArrayList<Long>()
        val chunk = ByteArray(1 shl 20)
        var base = 0L
        while (base < src.size) {
            val n = minOf(chunk.size.toLong(), src.size - base).toInt()
            if (n < HEADER) break
            val bb = ByteBuffer.wrap(chunk, 0, n).order(ByteOrder.LITTLE_ENDIAN)
            src.read(base, bb)
            var i = 0
            while (i + HEADER <= n) {
                // the length field is the cheapest of the three tests, so it goes first
                if (chunk[i + 4].toInt() == HEADER && chunk[i + 5] == 0.toByte() &&
                    chunk[i + 6] == 0.toByte() && chunk[i + 7] == 0.toByte() &&
                    looksLikeStamp(chunk, i + STAMP_AT)) {
                    found.add(base + i)
                }
                i += 4
            }
            if (n < chunk.size) break
            base += n - HEADER
        }
        val out = ArrayList<Section>()
        for ((k, off) in found.withIndex()) {
            val h = ByteBuffer.allocate(HEADER).order(ByteOrder.LITTLE_ENDIAN)
            src.read(off, h); h.flip()
            val size = h.getInt(SIZE_AT).toLong() and 0xFFFFFFFFL
            val crc = h.getInt(0x0C)
            val stamp = String(h.array(), STAMP_AT, 16, Charsets.ISO_8859_1).trim()
            val end = if (k + 1 < found.size) found[k + 1] else src.size
            out.add(Section(off, end, size, stamp, crc))
        }
        return out
    }

    /** what the section holds, judged by the bytes themselves; both zlib streams and raw images are recognised */
    fun sniff(src: RandomSource, s: Section): String {
        val head = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN)
        src.read(s.dataOffset, head); head.flip()
        val a = head.get(0).toInt() and 0xff
        val b = head.get(1).toInt() and 0xff
        if (a == 0x78 && (b == 0x01 || b == 0x9C || b == 0xDA)) return "zlib"
        if (src.size >= s.dataOffset + 2048) {
            val m = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN)
            src.read(s.dataOffset + 1024, m); m.flip()
            val v = m.getInt(0)
            if (v == 0xE0F5E1E2.toInt()) return "erofs"
            if (m.get(0).toInt() and 0xff == 0x53 && m.get(1).toInt() and 0xff == 0xEF) return "ext4"
            if (v == 0x3AFF26ED.toInt()) return "sparse"
        }
        val sig = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN)
        src.read(s.dataOffset, sig); sig.flip()
        if (String(sig.array(), 0, 8, Charsets.ISO_8859_1).startsWith("ANDROID!")) return "boot"
        return "raw"
    }

    /**
     * Writes the section out as a plain file: a zlib stream is inflated until the declared size is reached, anything
     * else is copied. [limit] caps what is written, for a look at the first megabytes.
     */
    fun extract(src: RandomSource, s: Section, out: File, limit: Long = Long.MAX_VALUE) {
        val want = minOf(s.size, limit)
        val kind = sniff(src, s)
        if (kind != "zlib") {
            out.outputStream().buffered(1 shl 20).use { o ->
                var done = 0L
                val buf = ByteArray(1 shl 20)
                while (done < want) {
                    val n = minOf(buf.size.toLong(), want - done, s.compressed - done).toInt()
                    if (n <= 0) break
                    val bb = ByteBuffer.wrap(buf, 0, n)
                    src.read(s.dataOffset + done, bb)
                    o.write(buf, 0, n)
                    done += n
                }
            }
            return
        }
        val inf = Inflater()
        val inBuf = ByteArray(1 shl 20)
        val outBuf = ByteArray(1 shl 20)
        out.outputStream().buffered(1 shl 20).use { o ->
            var pos = 0L
            var written = 0L
            while (written < want && pos < s.compressed) {
                val n = minOf(inBuf.size.toLong(), s.compressed - pos).toInt()
                val bb = ByteBuffer.wrap(inBuf, 0, n)
                src.read(s.dataOffset + pos, bb)
                pos += n
                inf.setInput(inBuf, 0, n)
                while (!inf.needsInput() && written < want) {
                    val k = inf.inflate(outBuf)
                    if (k == 0) break
                    val w = minOf(k.toLong(), want - written).toInt()
                    o.write(outBuf, 0, w)
                    written += w
                }
                if (inf.finished()) break
            }
        }
        inf.end()
    }

    private fun looksLikeStamp(b: ByteBuffer, at: Int): Boolean = looksLikeStamp(b.array(), at)

    /** "Fri Mar 11 10:20:30 2016": three letters, a space, three letters, a space, digits */
    private fun looksLikeStamp(b: ByteArray, at: Int): Boolean {
        if (at + 16 > b.size) return false
        fun letters(i: Int) = (0 until 3).all { c -> b[at + i + c].toInt().toChar() in 'A'..'Z' || b[at + i + c].toInt().toChar() in 'a'..'z' }
        fun digits(i: Int, n: Int) = (0 until n).all { c -> b[at + i + c].toInt().toChar() in '0'..'9' }
        if (!letters(0)) return false
        if (b[at + 3].toInt().toChar() != ' ') return false
        if (!letters(4)) return false
        if (b[at + 7].toInt().toChar() != ' ') return false
        return digits(8, 2)
    }
}