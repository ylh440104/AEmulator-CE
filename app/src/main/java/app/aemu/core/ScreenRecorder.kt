package app.aemu.core

import android.graphics.Bitmap
import android.graphics.Rect
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.MediaMuxer
import android.view.Surface
import java.io.File
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

class ScreenRecorder(
    private val out: File,
    width: Int,
    height: Int,
    private val frame: (Bitmap) -> Boolean,
    private val log: (String) -> Unit,
    private val audio: Boolean = false,
) {
    private val w = width and 1.inv()
    private val h = height and 1.inv()
    @Volatile private var live = false
    private var worker: Thread? = null
    private val pcm = LinkedBlockingQueue<ByteArray>(256)
    @Volatile private var silent = 0L
    private val origin = System.nanoTime()

    val running: Boolean get() = live
    val file: File get() = out

    fun writeAudio(buf: ByteArray, off: Int, len: Int) {
        if (!live || !audio) return
        val n = len - len % FRAME_BYTES
        if (n <= 0) return
        if (!pcm.offer(buf.copyOfRange(off, off + n))) silent += n
    }

    fun start(): Boolean {
        if (live) return true
        return runCatching {
            out.parentFile?.mkdirs()
            val fmt = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, w, h).apply {
                setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
                setInteger(MediaFormat.KEY_BIT_RATE, (w * h * 6).coerceIn(1_000_000, 12_000_000))
                setInteger(MediaFormat.KEY_FRAME_RATE, FPS)
                setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
            }
            val video = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
            video.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            val surface = video.createInputSurface()
            video.start()
            val sound = if (audio) openAudio() else null
            val muxer = MediaMuxer(out.absolutePath, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
            live = true
            worker = Thread({ run(video, surface, muxer, sound) }, "screen-rec").apply { start() }
            log("recording: started, ${w}x$h" + if (sound != null) ", with sound" else "")
        }.onFailure { live = false; log("recording: cannot start: ${it.message}") }.isSuccess
    }

    fun stop() {
        if (!live) return
        live = false
        pcm.offer(EOS)
        runCatching { worker?.join(8000) }
        worker = null
    }

    private fun openAudio(): MediaCodec? = runCatching {
        val fmt = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_AAC, RATE, CHANNELS).apply {
            setInteger(MediaFormat.KEY_AAC_PROFILE, MediaCodecInfo.CodecProfileLevel.AACObjectLC)
            setInteger(MediaFormat.KEY_BIT_RATE, 128_000)
            setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 16_384)
        }
        MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_AAC).also {
            it.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            it.start()
        }
    }.onFailure { log("recording: sound not available, video only: ${it.message}") }.getOrNull()

    private fun run(video: MediaCodec, surface: Surface, muxer: MediaMuxer, sound: MediaCodec?) {
        val bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        val dst = Rect(0, 0, w, h)
        val vInfo = MediaCodec.BufferInfo()
        val aInfo = MediaCodec.BufferInfo()
        var vTrack = -1
        var aTrack = -1
        var muxing = false
        var wantAudio = sound != null
        var frames = 0
        var samples = 0L
        val started = System.nanoTime()

        fun startMuxing() {
            if (muxing || vTrack < 0) return
            if (wantAudio && aTrack < 0) return
            runCatching { muxer.start(); muxing = true }
        }

        fun drain(codec: MediaCodec, info: MediaCodec.BufferInfo, isVideo: Boolean, end: Boolean) {
            var idle = 0
            while (true) {
                val i = codec.dequeueOutputBuffer(info, if (end) 10_000 else 0)
                when {
                    i == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        val t = runCatching { muxer.addTrack(codec.outputFormat) }.getOrDefault(-1)
                        if (isVideo) vTrack = t else aTrack = t
                        startMuxing()
                    }
                    i >= 0 -> {
                        val buf = codec.getOutputBuffer(i)
                        val target = if (isVideo) vTrack else aTrack
                        if (buf != null && muxing && target >= 0 && info.size > 0 &&
                            info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG == 0)
                            runCatching { muxer.writeSampleData(target, buf, info) }
                        codec.releaseOutputBuffer(i, false)
                        if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) return
                    }
                    else -> if (!end || ++idle > 300) return
                }
            }
        }

        fun push(codec: MediaCodec, data: ByteArray, from: Int, count: Int): Int {
            val i = codec.dequeueInputBuffer(10_000)
            if (i < 0) return 0
            val ib = codec.getInputBuffer(i) ?: return 0
            ib.clear()
            val n = minOf(ib.remaining(), count)
            if (n <= 0) return 0
            ib.put(data, from, n)
            codec.queueInputBuffer(i, 0, n, pts(samples), 0)
            samples += n / FRAME_BYTES
            return n
        }

        fun feedAudio(end: Boolean) {
            val codec = sound ?: return
            if (!wantAudio) { pcm.clear(); return }
            var idle = 0
            while (true) {
                val chunk = pcm.poll(if (end) 20 else 1, TimeUnit.MILLISECONDS)
                if (chunk == null) {
                    if (end) { if (++idle > 400) return; continue }
                    // the encoder has to report its format before the muxer can start, and it does that only
                    // once it has seen samples: with the guest still quiet, the gap is filled with silence
                    val target = (System.nanoTime() - origin) * RATE / 1_000_000_000L
                    var guard = 0
                    while (samples < target && guard++ < 16) if (push(codec, SILENCE, 0, SILENCE.size) <= 0) break
                    return
                }
                if (chunk === EOS) {
                    val i = codec.dequeueInputBuffer(10_000)
                    if (i >= 0) codec.queueInputBuffer(i, 0, 0, pts(samples), MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                    return
                }
                var off = 0
                while (off < chunk.size) {
                    val n = push(codec, chunk, off, chunk.size - off)
                    if (n <= 0) break
                    off += n
                }
            }
        }

        try {
            val step = 1_000_000_000L / FPS
            var next = System.nanoTime()
            while (live) {
                if (frame(bmp)) {
                    val c = surface.lockHardwareCanvas()
                    try { c.drawBitmap(bmp, null, dst, null) } finally { surface.unlockCanvasAndPost(c) }
                    frames++
                }
                drain(video, vInfo, true, false)
                if (sound != null) { feedAudio(false); drain(sound, aInfo, false, false) }
                // an encoder that never reports its format (no AAC on this phone) would hold the whole file back
                if (wantAudio && !muxing && aTrack < 0 && System.nanoTime() - started > 3_000_000_000L) {
                    wantAudio = false
                    log("recording: the sound encoder stayed silent, the file goes on without it")
                    startMuxing()
                }
                next += step
                val wait = (next - System.nanoTime()) / 1_000_000
                if (wait > 0) Thread.sleep(wait) else next = System.nanoTime()
            }
            if (sound != null) feedAudio(true)
            video.signalEndOfInputStream()
            drain(video, vInfo, true, true)
            if (sound != null && wantAudio) drain(sound, aInfo, false, true)
        } catch (t: Throwable) {
            log("recording: ${t.message ?: t}")
        } finally {
            live = false
            pcm.clear()
            runCatching { video.stop() }; runCatching { video.release() }
            sound?.let { runCatching { it.stop() }; runCatching { it.release() } }
            runCatching { if (muxing) muxer.stop() }; runCatching { muxer.release() }
            runCatching { surface.release() }
            if (audio && silent > 0) log("recording: dropped ${silent / (RATE * FRAME_BYTES)} s of sound (the encoder fell behind)")
            log("recording: finished, $frames frames, ${out.length() / 1024} KB")
        }
    }

    private fun pts(samples: Long) = origin + samples * 1_000_000_000L / RATE

    companion object {
        const val FPS = 30
        private const val RATE = 48000
        private const val CHANNELS = 2
        private const val FRAME_BYTES = 4
        private val EOS = ByteArray(0)
        private val SILENCE = ByteArray(4096)
    }
}