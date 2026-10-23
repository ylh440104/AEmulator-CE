package app.aemu.importer

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