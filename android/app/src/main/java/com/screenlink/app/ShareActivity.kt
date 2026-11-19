package com.screenlink.app

import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.widget.Toast
import androidx.activity.ComponentActivity

/** "Share > Send to PC (Imtidad)" from any app: sends the shared files to the PC's Downloads\Imtidad. */
class ShareActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val uris = sharedUris(intent)
        if (uris.isEmpty()) { finish(); return }
        Link.init(this)
        if (Link.currentHost.isBlank()) Link.connect(Prefs.load(this).host)
        Toast.makeText(this, "جارٍ الإرسال إلى الكمبيوتر…", Toast.LENGTH_SHORT).show()
        val handler = Handler(Looper.getMainLooper())
        val deadline = System.currentTimeMillis() + 8000
        fun attempt() {
            when {
                Link.connected -> FileTransfer.send(this, Link.currentHost, uris) { finish() }
                System.currentTimeMillis() > deadline -> {
                    Toast.makeText(this, "الكمبيوتر غير متصل", Toast.LENGTH_LONG).show()
                    finish()
                }
                else -> handler.postDelayed({ attempt() }, 300)
            }
        }
        attempt()
    }

    @Suppress("DEPRECATION")
    private fun sharedUris(intent: Intent?): List<Uri> = when (intent?.action) {
        Intent.ACTION_SEND -> listOfNotNull(
            if (Build.VERSION.SDK_INT >= 33) intent.getParcelableExtra(Intent.EXTRA_STREAM, Uri::class.java)
            else intent.getParcelableExtra(Intent.EXTRA_STREAM)
        )
        Intent.ACTION_SEND_MULTIPLE ->
            (if (Build.VERSION.SDK_INT >= 33) intent.getParcelableArrayListExtra(Intent.EXTRA_STREAM, Uri::class.java)
             else intent.getParcelableArrayListExtra(Intent.EXTRA_STREAM)) ?: emptyList()
        else -> emptyList()
    }
}
