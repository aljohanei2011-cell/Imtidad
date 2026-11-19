package com.screenlink.app

import android.content.ContentValues
import android.content.Context
import android.media.MediaCodec
import android.media.MediaFormat
import android.media.MediaMuxer
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import java.nio.ByteBuffer

/**
 * Records the stream exactly as it arrives (no re-encoding) into an MP4 in Movies/Imtidad.
 * Starts at the next keyframe; picture only.
 */
class Recorder(private val context: Context, private val width: Int, private val height: Int, private val hevc: Boolean) {
    private var muxer: MediaMuxer? = null
    private var track = -1
    private var firstPts = -1L
    private var uri: Uri? = null
    private var pfd: android.os.ParcelFileDescriptor? = null
    private val info = MediaCodec.BufferInfo()
    var savedName: String = ""; private set

    private fun isKeyframe(data: ByteArray, len: Int): Boolean =
        H264.nalUnits(data, len).any { (start, _) ->
            if (hevc) ((data[start].toInt() shr 1) and 0x3F) in 16..21 else (data[start].toInt() and 0x1F) == 5
        }

    /** Returns false if recording could not continue. */
    fun write(data: ByteArray, len: Int, ptsUs: Long): Boolean {
        if (muxer == null) {
            if (!isKeyframe(data, len)) return true  // wait for a keyframe
            val csd = (if (hevc) Hevc.findParameterSets(data, len) else H264.findParameterSets(data, len)?.let { listOf(it.first, it.second) })
                ?: return true
            if (!open(csd)) return false
        }
        if (firstPts < 0) firstPts = ptsUs
        info.set(0, len, ptsUs - firstPts, if (isKeyframe(data, len)) MediaCodec.BUFFER_FLAG_KEY_FRAME else 0)
        return runCatching { muxer!!.writeSampleData(track, ByteBuffer.wrap(data, 0, len), info) }.isSuccess
    }

    private fun open(csd: List<ByteArray>): Boolean = runCatching {
        val stamp = java.text.SimpleDateFormat("yyyy-MM-dd HH.mm.ss", java.util.Locale.US).format(java.util.Date())
        savedName = "Imtidad $stamp.mp4"
        val m = if (Build.VERSION.SDK_INT >= 29) {
            val values = ContentValues().apply {
                put(MediaStore.MediaColumns.DISPLAY_NAME, savedName)
                put(MediaStore.MediaColumns.MIME_TYPE, "video/mp4")
                put(MediaStore.MediaColumns.RELATIVE_PATH, Environment.DIRECTORY_MOVIES + "/Imtidad")
                put(MediaStore.MediaColumns.IS_PENDING, 1)
            }
            uri = context.contentResolver.insert(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, values)
            pfd = context.contentResolver.openFileDescriptor(uri!!, "rw")
            MediaMuxer(pfd!!.fileDescriptor, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
        } else {
            val dir = java.io.File(context.getExternalFilesDir(Environment.DIRECTORY_MOVIES), "Imtidad").apply { mkdirs() }
            MediaMuxer(java.io.File(dir, savedName).absolutePath, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
        }
        val format = MediaFormat.createVideoFormat(if (hevc) MediaFormat.MIMETYPE_VIDEO_HEVC else MediaFormat.MIMETYPE_VIDEO_AVC, width, height)
        if (hevc) {
            val all = java.io.ByteArrayOutputStream()
            csd.forEach { all.write(it) }
            format.setByteBuffer("csd-0", ByteBuffer.wrap(all.toByteArray()))
        } else {
            format.setByteBuffer("csd-0", ByteBuffer.wrap(csd[0]))
            format.setByteBuffer("csd-1", ByteBuffer.wrap(csd[1]))
        }
        track = m.addTrack(format)
        m.start()
        muxer = m
        true
    }.getOrDefault(false)

    /** Finishes the file. Returns its name, or null if nothing was recorded. */
    fun stop(): String? {
        val m = muxer ?: run { cleanup(false); return null }
        val ok = runCatching { m.stop(); m.release() }.isSuccess
        muxer = null
        cleanup(ok)
        return if (ok) savedName else null
    }

    private fun cleanup(keep: Boolean) {
        runCatching { pfd?.close() }
        val u = uri ?: return
        if (Build.VERSION.SDK_INT >= 29) {
            if (keep) context.contentResolver.update(u, ContentValues().apply { put(MediaStore.MediaColumns.IS_PENDING, 0) }, null, null)
            else context.contentResolver.delete(u, null, null)
        }
    }
}
