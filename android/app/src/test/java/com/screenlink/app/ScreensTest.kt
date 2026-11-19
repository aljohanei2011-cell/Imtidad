package com.screenlink.app

import android.content.Context
import android.os.Looper
import androidx.test.core.app.ActivityScenario
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.github.takahirom.roborazzi.captureRoboImage
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode

/** Pictures of the home screen in the new design: tablet and phone, light and dark, saved under build/screens. */
@RunWith(AndroidJUnit4::class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class ScreensTest {
    private fun shoot(name: String, theme: Int) {
        val context = ApplicationProvider.getApplicationContext<Context>()
        Prefs.load(context).copy(onboarded = true, theme = theme, mode = "MIRROR").save(context)
        ThemeChoice.loaded = true
        ThemeChoice.mode = theme
        ThemeChoice.forceDark = false
        ActivityScenario.launch(MainActivity::class.java).use { scenario ->
            repeat(5) { shadowOf(Looper.getMainLooper()).idle() }
            scenario.onActivity { it.window.decorView.rootView.captureRoboImage("build/screens/$name.png") }
        }
    }

    @Test @Config(sdk = [34], qualifiers = "w1280dp-h800dp-land-mdpi")
    fun tabletLight() = shoot("tablet-light", 1)

    @Test @Config(sdk = [34], qualifiers = "w1280dp-h800dp-land-mdpi")
    fun tabletDark() = shoot("tablet-dark", 2)

    @Test @Config(sdk = [34], qualifiers = "w390dp-h844dp-port-xhdpi")
    fun phoneLight() = shoot("phone-light", 1)

    @Test @Config(sdk = [34], qualifiers = "w390dp-h844dp-port-xhdpi")
    fun phoneDark() = shoot("phone-dark", 2)
}
