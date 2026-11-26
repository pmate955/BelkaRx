package hu.ha8mz.belkarx

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.util.AttributeSet
import android.view.Choreographer
import android.view.View
import kotlin.math.min

/**
 * RTTY tuning scope, the classic "crossed ellipses" display of RTTY terminal units: the mark
 * filter output drives the horizontal axis, the space filter output the vertical one.
 *
 * - A correctly tuned signal draws a "+": a horizontal line (mark) crossed by a vertical one
 *   (space), both about as long as the dashed ring.
 * - Only one line, or two lines of very different length: the marker is off center, or the
 *   shift is wrong (one tone falls outside its filter).
 * - A diagonal or a round blob: noise, or the tones sit between the filters.
 *
 * The decoder normalizes the size, so the picture stays the same size from weak to strong signals.
 * The trace is bright while the squelch is open and dim otherwise. Older points fade out, like
 * the afterglow of a scope tube.
 */
class RttyScopeView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : View(context, attrs) {

    /** Fills `xy` with the newest points (x, y interleaved, oldest first); returns the point count. */
    var source: ((xy: FloatArray) -> Int)? = null

    /** Squelch open: the trace is drawn bright. */
    var signal = false
        set(value) {
            if (field == value) return
            field = value
            invalidate()
        }

    var running = false
        set(value) {
            field = value
            if (value) scheduleFrame() else cancelFrame()
        }

    private val points = FloatArray(2 * VISIBLE_POINTS)
    private var pointCount = 0
    private val segments = FloatArray(4 * VISIBLE_POINTS)
    private var lastNewDataNs = 0L
    private var lastPullNs = 0L
    private var frameScheduled = false
    private val frameCallback = Choreographer.FrameCallback { frameTimeNanos ->
        frameScheduled = false
        if (!running) return@FrameCallback
        pull(frameTimeNanos)
        scheduleFrame()
    }

    private val density = resources.displayMetrics.density
    private val tracePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 1.5f * density
        strokeCap = Paint.Cap.ROUND
    }
    private val gridPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.rgb(0x1E, 0x2A, 0x20)
        style = Paint.Style.STROKE
        strokeWidth = 1f
    }
    private val ringPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.rgb(0x2C, 0x3C, 0x30)
        style = Paint.Style.STROKE
        strokeWidth = 1f * density
        pathEffect = android.graphics.DashPathEffect(floatArrayOf(4f * density, 4f * density), 0f)
    }
    private val markLabelPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = MARK_COLOR
        textSize = 10f * resources.displayMetrics.scaledDensity
        textAlign = Paint.Align.RIGHT
    }
    private val spaceLabelPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = SPACE_COLOR
        textSize = 10f * resources.displayMetrics.scaledDensity
    }

    init {
        setBackgroundColor(Color.BLACK)
        contentDescription = "RTTY tuning scope. Mark is horizontal, space is vertical; " +
            "a centered signal draws a cross."
    }

    /** Always square: as wide as it is high. */
    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val height = MeasureSpec.getSize(heightMeasureSpec)
        val width = if (MeasureSpec.getMode(widthMeasureSpec) == MeasureSpec.UNSPECIFIED) {
            height
        } else {
            min(height, MeasureSpec.getSize(widthMeasureSpec))
        }
        setMeasuredDimension(width, height)
    }

    private fun scheduleFrame() {
        if (frameScheduled || !running || !isAttachedToWindow) return
        frameScheduled = true
        val choreographer = Choreographer.getInstance()
        // Every frame while data flows; a slow check when capture is stopped.
        if (lastPullNs - lastNewDataNs < IDLE_AFTER_NS) {
            choreographer.postFrameCallback(frameCallback)
        } else {
            choreographer.postFrameCallbackDelayed(frameCallback, IDLE_POLL_MS)
        }
    }

    private fun cancelFrame() {
        Choreographer.getInstance().removeFrameCallback(frameCallback)
        frameScheduled = false
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        if (running) scheduleFrame()
    }

    override fun onDetachedFromWindow() {
        cancelFrame()
        super.onDetachedFromWindow()
    }

    private fun pull(now: Long) {
        val pullPoints = source ?: return
        lastPullNs = now
        val previousCount = pointCount
        val previousX = if (pointCount > 0) points[2 * pointCount - 2] else Float.NaN
        val previousY = if (pointCount > 0) points[2 * pointCount - 1] else Float.NaN
        pointCount = pullPoints(points)
        val changed = pointCount != previousCount ||
            (pointCount > 0 && (points[2 * pointCount - 2] != previousX || points[2 * pointCount - 1] != previousY))
        if (changed) {
            lastNewDataNs = now
            invalidate()
        }
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val size = min(width, height).toFloat()
        val cx = width / 2f
        val cy = height / 2f
        val radius = size / 2f * 0.92f

        // Axes and the ring where a normal-strength tone ends (the decoder scales to ~0.8).
        canvas.drawLine(cx - radius, cy, cx + radius, cy, gridPaint)
        canvas.drawLine(cx, cy - radius, cx, cy + radius, gridPaint)
        canvas.drawCircle(cx, cy, radius * RING_LEVEL, ringPaint)
        canvas.drawText("M", width - 3f * density, cy - 3f * density, markLabelPaint)
        canvas.drawText("S", cx + 3f * density, markLabelPaint.textSize + 1f * density, spaceLabelPaint)

        // Trace: in AGE_GROUPS parts, the older ones fainter.
        val count = pointCount
        if (count < 2) return
        tracePaint.color = if (signal) TRACE_COLOR else TRACE_COLOR_IDLE
        val groupSize = (count - 1 + AGE_GROUPS - 1) / AGE_GROUPS
        var start = 1
        for (group in 0 until AGE_GROUPS) {
            val end = min(count, start + groupSize)
            var n = 0
            for (i in start until end) {
                segments[n++] = cx + points[2 * i - 2] * radius
                segments[n++] = cy - points[2 * i - 1] * radius
                segments[n++] = cx + points[2 * i] * radius
                segments[n++] = cy - points[2 * i + 1] * radius
            }
            if (n > 0) {
                tracePaint.alpha = (255 * (group + 1) / AGE_GROUPS)
                canvas.drawLines(segments, 0, n, tracePaint)
            }
            start = end
        }
    }

    companion object {
        const val VISIBLE_POINTS = 360       // ~0.18 s at the decoder's ~2 kHz point rate
        private const val AGE_GROUPS = 4
        private const val RING_LEVEL = 0.8f
        private const val IDLE_AFTER_NS = 500_000_000L
        private const val IDLE_POLL_MS = 100L
        val MARK_COLOR = Color.rgb(0x4C, 0xE0, 0x6A)
        val SPACE_COLOR = Color.rgb(0xFF, 0x90, 0x00)
        private val TRACE_COLOR = Color.rgb(0x4C, 0xE0, 0x6A)
        private val TRACE_COLOR_IDLE = Color.rgb(0x5A, 0x6E, 0x5E)
    }
}
