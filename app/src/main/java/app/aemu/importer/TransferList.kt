package app.aemu.importer

import java.io.File
import java.io.IOException
import java.io.InputStream
import java.io.RandomAccessFile

/**
 * Сборка system.img из блочной OTA (system.new.dat + system.transfer.list, Android 5.0+).
 * В полных OTA встречаются только команды new/zero/erase — их и поддерживаем.
 * Результат — разреженный файл (дыры не занимают место на диске).
 */
object TransferList {
    private const val BLOCK = 4096
    private const val CHUNK = BLOCK * 256

    fun build(list: String, data: InputStream, out: File) {
        val lines = list.lines().map { it.trim() }.filter { it.isNotEmpty() }
        val version = lines[0].toIntOrNull() ?: throw IOException("transfer.list: no version")
        val totalBlocks = lines[1].toLong()
        val cmdStart = if (version >= 2) 4 else 2
        RandomAccessFile(out, "rw").use { raf ->
            raf.setLength(totalBlocks * BLOCK)
            val buf = ByteArray(CHUNK)
            for (ln in lines.drop(cmdStart)) {
                val parts = ln.split(' ')
                when (parts[0]) {
                    // "new <count> <block> <block> ...": <count> blocks are taken from the data, in order, and
                    // written to the blocks listed after it. The count is a number of blocks, not the start of a
                    // range: reading it as a range turns "new 2,0,121815" into 121815 blocks and shifts everything.
                    "new" -> {
                        val v = parts[1].split(',').map { it.toLong() }
                        val count = v[0].toInt()
                        var i = 0
                        while (i < count) {
                            val blk = v.getOrNull(i + 1) ?: break
                            // blocks listed one after another go down in one seek instead of one per block
                            var run = 1
                            while (i + run < count && v.getOrNull(i + run + 1) == blk + run) run++
                            var written = 0
                            while (written < run) {
                                val n = minOf(256, run - written)
                                readFully(data, buf, n * BLOCK)
                                raf.seek((blk + written) * BLOCK)
                                raf.write(buf, 0, n * BLOCK)
                                written += n
                            }
                            i += run
                        }
                    }
                    "zero", "erase" -> {} // дыры и так нулевые
                    "move", "bsdiff", "imgdiff", "stash", "free" -> throw IOException("this is an incremental OTA, a full firmware is required")
                }
            }
        }
    }

    private fun readFully(i: InputStream, b: ByteArray, n: Int) {
        var off = 0
        while (off < n) {
            val k = i.read(b, off, n - off)
            if (k <= 0) { b.fill(0, off, n); return }
            off += k
        }
    }
}
