package app.aemu.core

import java.io.File
import java.nio.file.Files

/**
 * Android 1.x `libui.so` (EGLDisplaySurface) cannot show anything when the framebuffer has no second page.
 *
 * The emulated fb0 accepts FBIOPUT_VSCREENINFO but keeps yres_virtual == yres, so mapFrameBuffer() logs
 * "page flipping not supported", clears PAGE_FLIP and allocates the second ("back") buffer with malloc().
 * SurfaceFlinger then renders into that malloc'd buffer (the constructor primes the window with buffer 1), and
 * swapBuffers() returns immediately when PAGE_FLIP is clear, so the buffer is never copied to the screen: fb0 stays
 * all zero, the screen stays black (SurfaceFlinger itself looks perfectly healthy in dumpsys).
 *
 * Fix: without page flipping render straight into the mapped framebuffer, i.e. use buffer 0 for buffer 1 as well.
 *
 *   mapFrameBuffer, no-flip branch (Thumb):
 *     ldr r0,[sp,#0x12c] ; bl malloc ; cmp r0,#0 ; bne L ; ... ; L: str r0,[sp,#0x114]
 *   becomes
 *     adds r0,r4,#0      ; b L ; nop   ; cmp r0,#0 ; bne L ; ... ; L: str r0,[sp,#0x114]      (r4 = the fb0 mapping)
 *
 *   ~EGLDisplaySurface frees buffer 1 when PAGE_FLIP is clear; that would now free the mmap'd framebuffer, so the
 *   `bmi skip_free` there becomes an unconditional `b skip_free`.
 *
 * Only runs on API 1 and 2 (Android 1.0 / 1.1; anything newer has a libui that does not need it).
 *
 * The sites are located by scanning, not by fixed offsets: every libui.so under the tree is searched for
 * the two instruction patterns, with the BL targets, the stack-slot offset and the branch distance treated
 * as wildcards (the slot offset must agree between the instructions that use it). A file is only touched
 * when each pattern matches exactly once, so an unknown or already patched build is left alone.
 */
internal object FbDirectPatch {
    private const val ANY = -1

    /** Android 1.0 and 1.1 only. */
    private val API_RANGE = 1..2

    // ldr r0,[sp,#S] ; bl ?? ; cmp r0,#0 ; bne +6 ; ldr r1,[sp,#S] ; adds r0,r4,#0 ; bl ?? ; movs r7,#12 ; rsbs r7,r7,#0
    // (S is any stack slot, but both loads must use the same one)
    private val MAP_SIG = intArrayOf(
        ANY, 0x98, ANY, ANY, ANY, ANY, 0x00, 0x28, 0x06, 0xd1, ANY, 0x99, 0x20, 0x1c, ANY, ANY, ANY, ANY, 0x0c, 0x27, 0x7f, 0x42,
    )
    private val MAP_NEW = intArrayOf(0x20, 0x1c, 0x09, 0xe0, 0xc0, 0x46) // adds r0,r4,#0 ; b +0x12 ; nop

    // ldr r3,[r4,#8] ; lsls r2,r3,#31 ; bmi skip ; adds r2,r6,#0 ; adds r2,#0xf0 ; ldr r0,[r2] ; bl free ; adds r0,r6,#0 ; pop {r4-r6,pc}
    private val DTOR_SIG = intArrayOf(
        0xa3, 0x68, 0xda, 0x07, ANY, 0xd4, 0x32, 0x1c, 0xf0, 0x32, 0x10, 0x68, ANY, ANY, ANY, ANY, 0x30, 0x1c, 0x70, 0xbd,
    )

    // patched mapFrameBuffer site: adds r0,r4,#0 ; b ; nop ; cmp r0,#0 ; bne +6 ; ldr r1,[sp,#S] ; adds r0,r4,#0
    private val DONE_SIG = intArrayOf(0x20, 0x1c, 0x09, 0xe0, 0xc0, 0x46, 0x00, 0x28, 0x06, 0xd1, ANY, 0x99, 0x20, 0x1c)

    // ---- layout B: the m5 / pre-1.0 libui (m5-rc15), same design, different code generation
    //   mapFrameBuffer, the no-flip branch (a jump target, not next to the flag test):
    //     ldr r0,[sp,#S] ; bl malloc ; cmp r0,#0 ; beq fail ; str r0,[sp,#T] ; b back
    //   -> `adds r0,r4,#0 ; nop ; nop` in place of `ldr ; bl`: buffer 1 is the mapped framebuffer (r4) as well
    private val MAP_B_SIG = intArrayOf(
        ANY, 0x98, ANY, 0xf7, ANY, ANY,                          // ldr r0,[sp,#S] ; bl malloc
        0x00, 0x28, ANY, 0xd0, ANY, 0x90, ANY, 0xe7,             // cmp r0,#0 ; beq ; str r0,[sp,#T] ; b
    )
    private const val MAP_B_AT = 0
    private val MAP_B_NEW = intArrayOf(0x20, 0x1c, 0xc0, 0x46, 0xc0, 0x46) // adds r0,r4,#0 ; nop ; nop
    private val MAP_B_DONE = intArrayOf(0x20, 0x1c, 0xc0, 0x46, 0xc0, 0x46, 0x00, 0x28, ANY, 0xd0, ANY, 0x90)

    //   ~EGLDisplaySurface (D1 and D2 are two copies of the same code):
    //     movs r0,#0xb4 ; ldr r3,[r4,r0] ; lsls r2,r3,#31 ; bmi skip ; movs r2,#0x9c ; ldr r0,[r4,r2] ; bl free
    private val DTOR_B_SIG = intArrayOf(
        0xb4, 0x20, 0x23, 0x58, 0xda, 0x07, ANY, 0xd4, 0x9c, 0x22, 0xa0, 0x58, ANY, 0xf7,
    )

    /** Every offset where [sig] matches and [accept] agrees (Thumb code is 2-byte aligned). */
    private fun findAll(d: ByteArray, sig: IntArray, accept: (Int) -> Boolean = { true }): List<Int> {
        val hits = ArrayList<Int>()
        var i = 0
        while (i <= d.size - sig.size) {
            var ok = true
            for (k in sig.indices) if (sig[k] != ANY && (d[i + k].toInt() and 0xff) != sig[k]) { ok = false; break }
            if (ok && accept(i)) hits += i
            i += 2
        }
        return hits
    }

    private fun mapSite(d: ByteArray): List<Int> =
        findAll(d, MAP_SIG) { d[it] == d[it + 10] } // same stack slot for both loads

    private fun dtorSite(d: ByteArray): List<Int> =
        findAll(d, DTOR_SIG) { (d[it + 4].toInt() and 0xff) < 0x80 } // forward bmi only

    /** Patches [d] in place. Returns true when libui was changed (false: unknown build, ambiguous, or already patched). */
    fun patch(d: ByteArray): Boolean {
        val m = mapSite(d)
        val t = dtorSite(d)
        if (m.size != 1 || t.size != 1) return patchB(d)
        for (k in MAP_NEW.indices) d[m[0] + k] = MAP_NEW[k].toByte()
        // bmi -> b, same target: Thumb B (0xE000 | imm11) has the same low byte as the bmi's imm8
        d[t[0] + 5] = 0xe0.toByte()
        return true
    }

    fun alreadyPatched(d: ByteArray): Boolean = findAll(d, DONE_SIG).isNotEmpty() || findAll(d, MAP_B_DONE).isNotEmpty()

    /** Layout B (see MAP_B_SIG): one mapFrameBuffer site and at least one destructor copy. */
    private fun siteCountsB(d: ByteArray): Pair<Int, Int> =
        findAll(d, MAP_B_SIG).size to findAll(d, DTOR_B_SIG) { (d[it + 6].toInt() and 0xff) < 0x80 }.size

    private fun patchB(d: ByteArray): Boolean {
        val m = findAll(d, MAP_B_SIG)
        val t = findAll(d, DTOR_B_SIG) { (d[it + 6].toInt() and 0xff) < 0x80 }
        if (m.size != 1 || t.isEmpty() || t.size > 2) return false
        val at = m[0] + MAP_B_AT
        for (k in MAP_B_NEW.indices) d[at + k] = MAP_B_NEW[k].toByte()
        for (x in t) d[x + 7] = 0xe0.toByte()   // bmi skip -> b skip (same target, same low byte)
        return true
    }

    /** Every libui.so that can be in the tree (regular files only, nothing outside [root]). */
    private fun candidates(root: File): List<File> {
        val out = LinkedHashSet<File>()
        for (dir in listOf("system/lib", "vendor/lib", "system/vendor/lib")) {
            val base = File(root, dir)
            if (!base.isDirectory) continue
            base.walkTopDown().maxDepth(2).filter { it.name == "libui.so" }.forEach { out += it }
        }
        return out.toList()
    }

    private fun patchFile(root: File, f: File, log: (String) -> Unit) {
        if (!f.isFile || Files.isSymbolicLink(f.toPath())) return
        if (!f.canonicalPath.startsWith(root.canonicalPath + File.separator)) return
        val d = f.readBytes()
        if (alreadyPatched(d)) { log("display: ${f.relativeTo(root).path} already carries the direct-to-fb0 patch"); return }
        val orig = d.copyOf()
        // (pre-1.0 builds such as m5-rc15 log "page flipping not supported" and stay black; with no line here the account of
        // why the patch did not apply was missing: the counts say whether the code of this build differs or is ambiguous)
        val mapHits = mapSite(d).size
        val dtorHits = dtorSite(d).size
        val (mapB, dtorB) = siteCountsB(d)
        val matchesA = mapHits == 1 && dtorHits == 1
        val matchesB = mapB == 1 && dtorB in 1..2
        if (!matchesA && !matchesB) {
            val text = String(d, Charsets.ISO_8859_1)
            val at = text.indexOf("page flipping not supported")
            log("display: ${f.relativeTo(root).path} (${d.size} bytes) NOT patched: layout A map/dtor matches=$mapHits/$dtorHits (1/1 needed), layout B map/dtor matches=$mapB/$dtorB (1/1-2 needed); " +
                "message string ${if (at >= 0) "at 0x" + at.toString(16) else "not found"}; ELF class ${d.getOrNull(4)?.toInt()}, machine 0x" +
                (if (d.size > 19) (((d[19].toInt() and 0xff) shl 8) or (d[18].toInt() and 0xff)).toString(16) else "?"))
            return
        }
        if (patch(d)) {
            val bak = File(f.parentFile, "libui.so.aemu-orig")
            if (!bak.exists()) bak.writeBytes(orig)
            f.setWritable(true)
            f.writeBytes(d)
            log("display: ${f.relativeTo(root).path} patched (Android 1.x draws straight into fb0 when the framebuffer cannot page-flip)")
        }
    }

    /** [api] is the firmware's API level; the patch is only for Android 1.0 / 1.1 (API 1 and 2). */
    fun apply(root: File, api: Int, log: (String) -> Unit) {
        if (api !in API_RANGE) return
        for (f in candidates(root)) runCatching { patchFile(root, f, log) }
            .onFailure { log("display: libui patch failed (${f.name}): ${it.message}") }
    }
}
