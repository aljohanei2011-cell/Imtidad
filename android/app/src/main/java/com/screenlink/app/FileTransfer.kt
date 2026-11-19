package com.screenlink.app

import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.provider.MediaStore
import android.provider.OpenableColumns
import android.widget.Toast
import androidx.compose.runtime.mutableStateListOf
import org.json.JSONObject
import java.io.File
import java.io.InputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import kotlin.concurrent.thread

/** A file that arrived from the PC (screenshot or "Send to > Imtidad"). */
data class Received(val name: String, val uri: Uri, val mime: String)

/**
 * Files between this device and the PC over TCP 48769 (see windows/src/main.cpp):
 * tablet → PC lands in the PC's Downloads\Imtidad; PC → tablet lands in Pictures/Imtidad or Download/Imtidad.
 */
object FileTransfer {
    const val PORT = 48769
    val received = mutableStateListOf<Received>()
    private val main = Handler(Looper.getMainLooper())

    private fun toast(context: Context, text: String) = main.post { Toast.makeText(context, text, Toast.LENGTH_SHORT).show() }

    private fun readLine(input: InputStream): String {
        val sb = StringBuilder()
        val bytes = java.io.ByteArrayOutputStream()
        while (bytes.size() < 8192) {
            val b = input.read()
            if (b < 0 || b == '\n'.code) break
            bytes.write(b)
        }
        sb.append(bytes.toString("UTF-8"))
        return sb.toString()
    }

    /** Sends files chosen on this device to the PC. */
    fun send(context: Context, host: String, uris: List<Uri>, done: (Int) -> Unit = {}) {
        val app = context.applicationContext
        thread(name = "imtidad-send") {
            var ok = 0
            for (uri in uris) {
                runCatching {
                    var name = "file"
                    var size = -1L
                    app.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE), null, null, null)?.use { c ->
                        if (c.moveToFirst()) {
                            name = c.getString(0) ?: name
                            if (!c.isNull(1)) size = c.getLong(1)
                        }
                    }
                    if (size < 0) size = app.contentResolver.openFileDescriptor(uri, "r")?.use { it.statSize } ?: 0L
                    Socket().use { s ->
                        s.connect(InetSocketAddress(host, PORT), 4000)
                        val out = s.getOutputStream().buffered(1 shl 16)
                        out.write((JSONObject().put("op", "put").put("name", name).put("size", size).toString() + "\n").toByteArray())
                        app.contentResolver.openInputStream(uri)?.use { it.copyTo(out, 1 shl 16) }
                        out.flush()
                        val reply = JSONObject(readLine(s.getInputStream()))
                        if (reply.optBoolean("ok")) ok++
                    }
                }
            }
            toast(app, if (ok == uris.size) "أُرسل إلى الكمبيوتر ($ok) — في التنزيلات\\Imtidad" else "أُرسل $ok من ${uris.size}")
            main.post { done(ok) }
        }
    }

    /** Downloads a file the PC announced, into the gallery (images) or Downloads. */
    fun receive(context: Context, host: String, id: String) {
        val app = context.applicationContext
        thread(name = "imtidad-receive") {
            runCatching {
                Socket().use { s ->
                    s.connect(InetSocketAddress(host, PORT), 4000)
                    s.getOutputStream().apply {
                        write((JSONObject().put("op", "get").put("id", id).toString() + "\n").toByteArray()); flush()
                    }
                    val input = s.getInputStream().buffered(1 shl 16)
                    val head = JSONObject(readLine(input))
                    if (head.has("error")) return@thread
                    val name = head.optString("name", "file")
                    val size = head.optLong("size")
                    val mime = mimeOf(name)
                    val (uri, out) = createTarget(app, name, mime)
                    out.use { o ->
                        var left = size
                        val buf = ByteArray(1 shl 16)
                        while (left > 0) {
                            val n = input.read(buf, 0, minOf(buf.size.toLong(), left).toInt())
                            if (n < 0) break
                            o.write(buf, 0, n)
                            left -= n
                        }
                    }
                    finish(app, uri)
                    main.post { received.add(0, Received(name, uri, mime)) }
                    toast(app, "وصل من الكمبيوتر: $name")
                }
            }.onFailure { toast(app, "تعذّر استلام الملف") }
        }
    }

    /** Network check: downloads [bytes] from the PC and returns Mbit/s (or null). */
    fun speedTest(host: String, bytes: Int = 8 shl 20): Double? = runCatching {
        Socket().use { s ->
            s.connect(InetSocketAddress(host, PORT), 4000)
            s.soTimeout = 15000
            s.getOutputStream().apply { write((JSONObject().put("op", "speed").put("size", bytes).toString() + "\n").toByteArray()); flush() }
            val input = s.getInputStream()
            val buf = ByteArray(1 shl 16)
            var got = 0
            val t0 = System.nanoTime()
            while (got < bytes) {
                val n = input.read(buf)
                if (n < 0) break
                got += n
            }
            val secs = (System.nanoTime() - t0) / 1e9
            if (got < bytes / 2) null else got * 8 / 1e6 / secs
        }
    }.getOrNull()

    fun mimeOf(name: String): String {
        val ext = name.substringAfterLast('.', "").lowercase()
        return android.webkit.MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext) ?: "application/octet-stream"
    }

    private fun createTarget(context: Context, name: String, mime: String): Pair<Uri, OutputStream> {
        val image = mime.startsWith("image/")
        if (Build.VERSION.SDK_INT >= 29) {
            val values = ContentValues().apply {
                put(MediaStore.MediaColumns.DISPLAY_NAME, name)
                put(MediaStore.MediaColumns.MIME_TYPE, mime)
                put(MediaStore.MediaColumns.RELATIVE_PATH,
                    (if (image) Environment.DIRECTORY_PICTURES else Environment.DIRECTORY_DOWNLOADS) + "/Imtidad")
                put(MediaStore.MediaColumns.IS_PENDING, 1)
            }
            val collection = if (image) MediaStore.Images.Media.EXTERNAL_CONTENT_URI else MediaStore.Downloads.EXTERNAL_CONTENT_URI
            val uri = context.contentResolver.insert(collection, values) ?: error("no media store")
            return uri to (context.contentResolver.openOutputStream(uri) ?: error("cannot write"))
        }
        val dir = File(context.getExternalFilesDir(null), "Imtidad").apply { mkdirs() }
        val f = File(dir, name)
        return Uri.fromFile(f) to f.outputStream()
    }

    private fun finish(context: Context, uri: Uri) {
        if (Build.VERSION.SDK_INT >= 29 && uri.scheme == "content") {
            context.contentResolver.update(uri, ContentValues().apply { put(MediaStore.MediaColumns.IS_PENDING, 0) }, null, null)
        }
    }

    /** Turns a camera photo into a one-page PDF (document scan) in the app cache. */
    fun photoToPdf(context: Context, photo: Uri): Uri? = runCatching {
        val bitmap = context.contentResolver.openInputStream(photo)?.use { android.graphics.BitmapFactory.decodeStream(it) } ?: return null
        // A4 at 150 dpi, photo scaled to fit.
        val pw = 1240
        val ph = 1754
        val doc = android.graphics.pdf.PdfDocument()
        val page = doc.startPage(android.graphics.pdf.PdfDocument.PageInfo.Builder(pw, ph, 1).create())
        val scale = minOf(pw.toFloat() / bitmap.width, ph.toFloat() / bitmap.height)
        val w = bitmap.width * scale
        val h = bitmap.height * scale
        page.canvas.drawColor(android.graphics.Color.WHITE)
        page.canvas.drawBitmap(bitmap, null, android.graphics.RectF((pw - w) / 2, (ph - h) / 2, (pw + w) / 2, (ph + h) / 2),
            android.graphics.Paint(android.graphics.Paint.FILTER_BITMAP_FLAG))
        doc.finishPage(page)
        val stamp = java.text.SimpleDateFormat("yyyy-MM-dd HH.mm.ss", java.util.Locale.US).format(java.util.Date())
        val file = File(File(context.cacheDir, "scans").apply { mkdirs() }, "Scan $stamp.pdf")
        file.outputStream().use { doc.writeTo(it) }
        doc.close()
        bitmap.recycle()
        androidx.core.content.FileProvider.getUriForFile(context, context.packageName + ".files", file)
    }.getOrNull()
}
