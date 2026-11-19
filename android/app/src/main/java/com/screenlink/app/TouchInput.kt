package com.screenlink.app

import android.os.Handler
import android.os.Looper
import android.view.MotionEvent
import android.view.View
import org.json.JSONArray
import org.json.JSONObject
import kotlin.math.abs
import kotlin.math.hypot

/** JSON input events understood by windows/src/input.hpp. */
object InputMessages {
    fun pointer(action: String, x: Float, y: Float): String =
        JSONObject().put("t", "p").put("a", action).put("x", x.toDouble()).put("y", y.toDouble()).toString()

    fun scroll(x: Float, y: Float, dy: Int): String =
        JSONObject().put("t", "s").put("x", x.toDouble()).put("y", y.toDouble()).put("dy", dy).toString()

    fun text(text: String): String = JSONObject().put("t", "k").put("text", text).toString()

    fun key(vk: Int): String = JSONObject().put("t", "v").put("vk", vk).toString()

    fun combo(vararg vks: Int): String = JSONObject().put("t", "c").put("keys", JSONArray(vks.toList())).toString()

    fun rel(dx: Int, dy: Int): String = JSONObject().put("t", "rel").put("dx", dx).put("dy", dy).toString()

    fun button(b: String, action: String): String = JSONObject().put("t", "btn").put("b", b).put("a", action).toString()

    fun keyDown(vk: Int): String = JSONObject().put("t", "kd").put("vk", vk).toString()

    fun keyUp(vk: Int): String = JSONObject().put("t", "ku").put("vk", vk).toString()

    fun zoom(x: Float, y: Float, dy: Int): String =
        JSONObject().put("t", "zoom").put("x", x.toDouble()).put("y", y.toDouble()).put("dy", dy).toString()

    fun pen(action: String, x: Float, y: Float, pressure: Float, tiltX: Int, tiltY: Int, eraser: Boolean, barrel: Boolean): String =
        JSONObject().put("t", "pen").put("a", action).put("x", x.toDouble()).put("y", y.toDouble())
            .put("p", pressure.toDouble()).put("tx", tiltX).put("ty", tiltY)
            .put("e", if (eraser) 1 else 0).put("b", if (barrel) 1 else 0).toString()
}

/** Common Windows virtual-key codes. */
object VK {
    const val BACK = 0x08; const val TAB = 0x09; const val ENTER = 0x0D; const val SHIFT = 0x10
    const val CTRL = 0x11; const val ALT = 0x12; const val ESC = 0x1B; const val LEFT = 0x25
    const val UP = 0x26; const val RIGHT = 0x27; const val DOWN = 0x28; const val DELETE = 0x2E
    const val WIN = 0x5B; const val SPACE = 0x20
    const val VOL_MUTE = 0xAD; const val VOL_DOWN = 0xAE; const val VOL_UP = 0xAF
    const val NEXT = 0xB0; const val PREV = 0xB1; const val PLAY_PAUSE = 0xB3
    fun letter(c: Char) = c.uppercaseChar().code
}

/**
 * Fingers and pen on the stream.
 *
 * Direct mode (default): the finger is the mouse pointer.
 * - tap: left click · touch and move: drag · touch and hold (500 ms): right click
 * - two fingers up/down: mouse wheel · two fingers apart/together: zoom (Ctrl + wheel)
 * - three-finger tap: right click
 *
 * Touchpad mode: the finger moves the pointer like a laptop touchpad.
 * - move: pointer moves · tap: left click · two-finger tap: right click
 * - two fingers up/down: wheel · touch and hold, then move: drag
 *
 * penOnly: fingers are ignored (draw with the pen, rest your hand anywhere).
 */
class TouchInput(private val send: (String) -> Unit, density: Float) : View.OnTouchListener, View.OnGenericMotionListener {
    @Volatile var touchpad = false
    @Volatile var penOnly = false
    /** Pen pressure feel: < 1 soft (light touch draws dark), 1 normal, > 1 firm. */
    @Volatile var pressureGamma = 1f
    private fun curve(p: Float): Float = if (pressureGamma == 1f) p else Math.pow(p.toDouble(), pressureGamma.toDouble()).toFloat()
    /** Magnifier: two fingers zoom and move the picture on this device (the PC is not touched). */
    @Volatile var lens = false
        set(value) { field = value; if (!value) view?.let { resetLens(it) } }
    private var view: View? = null

    private val handler = Handler(Looper.getMainLooper())
    private val slop = 10f * density
    private val scrollStep = 30f * density
    private val zoomStep = 40f * density
    private val speed = 1.6f
    private var downX = 0f
    private var downY = 0f
    private var downNx = 0f
    private var downNy = 0f
    private var downTime = 0L
    private var pending = false
    private var dragging = false
    private var scrolling = false
    private var lastScrollY = 0f
    private var scrollAcc = 0f
    private var startSpan = 0f
    private var lastSpan = 0f
    private var zoomAcc = 0f
    private var gesture = Gesture.None
    private var maxPointers = 0
    private var moved = false
    private var lastX = 0f
    private var lastY = 0f
    private var remX = 0f
    private var remY = 0f

    private enum class Gesture { None, Scroll, Zoom }

    private val longPress = Runnable {
        if (pending && !scrolling) {
            pending = false
            if (touchpad) {
                dragging = true
                send(InputMessages.button("l", "d"))  // hold, then move to drag
            } else {
                send(InputMessages.pointer("r", downNx, downNy))
            }
        }
    }
    // ---------------------------------------------------------------- pen
    private var penNearUntil = 0L   // palm rejection: ignore fingers while the pen is near

    private fun isPen(e: MotionEvent): Boolean {
        val tool = e.getToolType(0)
        return tool == MotionEvent.TOOL_TYPE_STYLUS || tool == MotionEvent.TOOL_TYPE_ERASER
    }

    private fun sendPen(v: View, e: MotionEvent, action: String) {
        val nx = (e.x / v.width.coerceAtLeast(1)).coerceIn(0f, 1f)
        val ny = (e.y / v.height.coerceAtLeast(1)).coerceIn(0f, 1f)
        // Android: tilt = angle from vertical, orientation = direction. Windows wants tilt along X and Y.
        val tilt = e.getAxisValue(MotionEvent.AXIS_TILT).toDouble()
        val orientation = e.getAxisValue(MotionEvent.AXIS_ORIENTATION).toDouble()
        val deg = Math.toDegrees(tilt)
        val tx = (deg * kotlin.math.sin(orientation)).toInt().coerceIn(-90, 90)
        val ty = (-deg * kotlin.math.cos(orientation)).toInt().coerceIn(-90, 90)
        val eraser = e.getToolType(0) == MotionEvent.TOOL_TYPE_ERASER
        val barrel = (e.buttonState and MotionEvent.BUTTON_STYLUS_PRIMARY) != 0
        send(InputMessages.pen(action, nx, ny, curve(e.pressure.coerceIn(0f, 1f)), tx, ty, eraser, barrel))
        penNearUntil = System.currentTimeMillis() + 600
    }

    /** Pen hovering above the screen (moves the pointer without clicking). */
    override fun onGenericMotion(v: View, e: MotionEvent): Boolean {
        if (!isPen(e)) return false
        when (e.actionMasked) {
            MotionEvent.ACTION_HOVER_ENTER, MotionEvent.ACTION_HOVER_MOVE -> sendPen(v, e, "h")
            MotionEvent.ACTION_HOVER_EXIT -> sendPen(v, e, "x")
            else -> return false
        }
        return true
    }

    private fun onPenTouch(v: View, e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> sendPen(v, e, "d")
            MotionEvent.ACTION_MOVE -> {
                // Send the samples batched since the last event too, for smooth lines.
                val eraser = e.getToolType(0) == MotionEvent.TOOL_TYPE_ERASER
                for (i in 0 until e.historySize) {
                    val nx = (e.getHistoricalX(i) / v.width.coerceAtLeast(1)).coerceIn(0f, 1f)
                    val ny = (e.getHistoricalY(i) / v.height.coerceAtLeast(1)).coerceIn(0f, 1f)
                    send(InputMessages.pen("m", nx, ny, curve(e.getHistoricalPressure(i).coerceIn(0f, 1f)), 0, 0, eraser, false))
                }
                sendPen(v, e, "m")
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> sendPen(v, e, "u")
        }
        return true
    }

    private fun resetLens(v: View) {
        v.scaleX = 1f; v.scaleY = 1f; v.translationX = 0f; v.translationY = 0f
    }

    private var lensSpan = 0f
    private var lensX = 0f
    private var lensY = 0f

    /** Pinch and pan the picture itself. Touch coordinates stay correct: Android maps them through the scale. */
    private fun lensMove(v: View, e: MotionEvent) {
        val sp = span(e)
        // Raw (screen) positions: the view moves under the fingers.
        if (android.os.Build.VERSION.SDK_INT < 29) return  // needs per-finger screen positions
        val cx = (e.getRawX(0) + e.getRawX(1)) / 2
        val cy = (e.getRawY(0) + e.getRawY(1)) / 2
        if (lensSpan > 0f) {
            v.pivotX = 0f; v.pivotY = 0f
            val old = v.scaleX
            val scale = (old * sp / lensSpan).coerceIn(1f, 5f)
            // Keep the point between the fingers in place while zooming, then follow the pan.
            var tx = cx - (cx - v.translationX - v.left) * (scale / old) - v.left + (cx - lensX)
            var ty = cy - (cy - v.translationY - v.top) * (scale / old) - v.top + (cy - lensY)
            tx = tx.coerceIn(v.width * (1 - scale), 0f)
            ty = ty.coerceIn(v.height * (1 - scale), 0f)
            v.scaleX = scale; v.scaleY = scale; v.translationX = tx; v.translationY = ty
        }
        lensSpan = sp; lensX = cx; lensY = cy
    }

    override fun onTouch(v: View, e: MotionEvent): Boolean {
        view = v
        if (isPen(e)) return onPenTouch(v, e)
        if (lens && e.pointerCount >= 2) {
            handler.removeCallbacks(longPress)
            if (dragging) send(if (touchpad) InputMessages.button("l", "u") else InputMessages.pointer("u", downNx, downNy))
            pending = false; dragging = false; scrolling = true; gesture = Gesture.Zoom; maxPointers = 0
            if (e.actionMasked == MotionEvent.ACTION_POINTER_DOWN) lensSpan = 0f
            if (e.actionMasked == MotionEvent.ACTION_MOVE) lensMove(v, e)
            return true
        }
        if (penOnly) return true
        if (System.currentTimeMillis() < penNearUntil) return true  // palm resting while writing
        val w = v.width.coerceAtLeast(1).toFloat()
        val h = v.height.coerceAtLeast(1).toFloat()
        val nx = (e.x / w).coerceIn(0f, 1f)
        val ny = (e.y / h).coerceIn(0f, 1f)
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                downX = e.x; downY = e.y; downNx = nx; downNy = ny; downTime = System.currentTimeMillis()
                lastX = e.x; lastY = e.y; remX = 0f; remY = 0f
                pending = true; dragging = false; scrolling = false; moved = false
                gesture = Gesture.None; maxPointers = 1
                if (!touchpad) send(InputMessages.pointer("m", nx, ny))
                handler.postDelayed(longPress, LONG_PRESS_MS)
            }
            MotionEvent.ACTION_POINTER_DOWN -> {
                handler.removeCallbacks(longPress)
                if (dragging) send(if (touchpad) InputMessages.button("l", "u") else InputMessages.pointer("u", nx, ny))
                pending = false; dragging = false; scrolling = true
                maxPointers = maxOf(maxPointers, e.pointerCount)
                lastScrollY = averageY(e); scrollAcc = 0f
                startSpan = span(e); lastSpan = startSpan; zoomAcc = 0f; gesture = Gesture.None
            }
            MotionEvent.ACTION_MOVE -> {
                if (scrolling) {
                    if (e.pointerCount == 2) twoFingerMove(e, w, h)
                    else if (e.pointerCount >= 3 && hypot(e.x - downX, e.y - downY) > slop * 2) moved = true
                } else if (touchpad) {
                    if (pending && hypot(e.x - downX, e.y - downY) > slop) {
                        handler.removeCallbacks(longPress)
                        pending = false
                    }
                    if (!pending) {
                        remX += (e.x - lastX) * speed
                        remY += (e.y - lastY) * speed
                        val dx = remX.toInt()
                        val dy = remY.toInt()
                        if (dx != 0 || dy != 0) {
                            send(InputMessages.rel(dx, dy))
                            remX -= dx; remY -= dy
                        }
                    }
                    lastX = e.x; lastY = e.y
                } else {
                    if (pending && hypot(e.x - downX, e.y - downY) > slop) {
                        handler.removeCallbacks(longPress)
                        pending = false
                        dragging = true
                        send(InputMessages.pointer("d", downNx, downNy))
                    }
                    if (dragging) send(InputMessages.pointer("m", nx, ny))
                }
            }
            MotionEvent.ACTION_UP -> {
                handler.removeCallbacks(longPress)
                val quick = System.currentTimeMillis() - downTime < 300
                when {
                    maxPointers >= 3 && !moved && quick ->
                        send(if (touchpad) InputMessages.button("r", "c") else InputMessages.pointer("r", downNx, downNy))
                    maxPointers == 2 && gesture == Gesture.None && quick && touchpad -> send(InputMessages.button("r", "c"))
                    pending && touchpad -> send(InputMessages.button("l", "c"))
                    pending -> {
                        send(InputMessages.pointer("d", downNx, downNy))
                        send(InputMessages.pointer("u", downNx, downNy))
                    }
                    dragging && touchpad -> send(InputMessages.button("l", "u"))
                    dragging -> send(InputMessages.pointer("u", nx, ny))
                }
                pending = false; dragging = false; scrolling = false
            }
            MotionEvent.ACTION_CANCEL -> {
                handler.removeCallbacks(longPress)
                if (dragging) send(if (touchpad) InputMessages.button("l", "u") else InputMessages.pointer("u", nx, ny))
                pending = false; dragging = false; scrolling = false
            }
        }
        return true
    }

    /** Two fingers: decide once between scrolling and zooming, then keep to it. */
    private fun twoFingerMove(e: MotionEvent, w: Float, h: Float) {
        val y = averageY(e)
        val sp = span(e)
        if (gesture == Gesture.None) {
            when {
                abs(sp - startSpan) > slop * 3 -> gesture = Gesture.Zoom
                abs(y - lastScrollY) > slop -> gesture = Gesture.Scroll
            }
        }
        when (gesture) {
            Gesture.Scroll -> {
                scrollAcc += y - lastScrollY
                while (abs(scrollAcc) >= scrollStep) {
                    val dir = if (scrollAcc > 0) 1 else -1
                    // Fingers moving down pull the page down, like scrolling up with a wheel.
                    send(InputMessages.scroll(downNx, downNy, 120 * dir))
                    scrollAcc -= dir * scrollStep
                }
            }
            Gesture.Zoom -> {
                zoomAcc += sp - lastSpan
                val cx = ((e.getX(0) + e.getX(1)) / 2 / w).coerceIn(0f, 1f)
                val cy = ((e.getY(0) + e.getY(1)) / 2 / h).coerceIn(0f, 1f)
                while (abs(zoomAcc) >= zoomStep) {
                    val dir = if (zoomAcc > 0) 1 else -1
                    send(InputMessages.zoom(cx, cy, 120 * dir))
                    zoomAcc -= dir * zoomStep
                }
            }
            Gesture.None -> {}
        }
        lastScrollY = y
        lastSpan = sp
    }

    private fun span(e: MotionEvent): Float =
        if (e.pointerCount < 2) 0f else hypot(e.getX(0) - e.getX(1), e.getY(0) - e.getY(1))

    private fun averageY(e: MotionEvent): Float {
        var sum = 0f
        for (i in 0 until e.pointerCount) sum += e.getY(i)
        return sum / e.pointerCount
    }

    companion object {
        const val LONG_PRESS_MS = 500L
    }
}
