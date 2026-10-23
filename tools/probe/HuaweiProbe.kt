import java.io.File
import java.nio.channels.FileChannel
import java.nio.file.StandardOpenOption
import java.util.zip.ZipFile

/**
 * Looks inside a Huawei/Honor UPDATE.APP without unpacking all of it: the sections, what each one holds, and the
 * build.prop of the system one. Runs on a plain JVM, so it works on a build server.
 */
fun main(args: Array<String>) {
    if (args.isEmpty()) { System.err.println("usage: HuaweiProbe <UPDATE.APP> <work dir>"); kotlin.system.exitProcess(2) }
    val app = File(args[0])
    val work = File(args.getOrElse(1) { "app-work" }).apply { mkdirs() }
    println("== ${app.name}: ${app.length() / 1048576} MB")

    val blob = if (app.name.endsWith(".zip", true)) {
        val out = File(work, "UPDATE.APP")
        if (!out.isFile) {
            ZipFile(app).use { z ->
                val e = z.entries().toList().firstOrNull { it.name.endsWith("UPDATE.APP") }
                    ?: error("no UPDATE.APP in ${app.name}: entries are ${z.entries().toList().take(10).map { it.name }}")
                println("== taking ${e.name} (${e.size / 1048576} MB)")
                z.getInputStream(e).use { i -> out.outputStream().buffered(1 shl 20).use { o -> i.copyTo(o, 1 shl 20) } }
            }
        }
        out
    } else app

    FileChannel.open(blob.toPath(), StandardOpenOption.READ).use { ch ->
        val src = ChannelSource(ch)
        println("== probe: ${HuaweiApp.probe(src)}")
        val sections = HuaweiApp.sections(src)
        println("== sections: ${sections.size}")
        for ((i, s) in sections.withIndex()) {
            println("  [%2d] hdr@%012d  size=%12d  stored=%12d  %s  %s".format(
                i, s.offset, s.size, s.compressed, s.stamp, HuaweiApp.sniff(src, s)))
        }
        println("== writing the first megabytes of each section to see what they are")
        for ((i, s) in sections.withIndex()) {
            val f = File(work, "sec$i.bin")
            runCatching { HuaweiApp.extract(src, s, f, 8L shl 20) }
                .onFailure { println("   [$i] extract failed: ${it.message}"); continue }
            val head = f.inputStream().use { it.readNBytes(16) }
            val kind = HuaweiApp.sniff(src, s)
            println("   [%2d] %-6s head=%s".format(i, kind, head.joinToString("") { "%02x".format(it) }))
            if (kind == "erofs" || kind == "ext4") {
                FileChannel.open(f.toPath(), StandardOpenOption.READ).use { fc ->
                    val msrc = ChannelSource(fc)
                    if (ErofsReader.probe(msrc)) {
                        runCatching { ErofsImage(msrc) }.onSuccess { println("      EROFS ok") }
                            .onFailure { println("      EROFS: ${it.message}") }
                    } else println("      ext4 (a partial image cannot be read, only looked at)")
                }
            }
        }
    }
    println("== done")
}