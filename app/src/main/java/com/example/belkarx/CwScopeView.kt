package hu.ha8mz.belkarx

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.DashPathEffect
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.util.AttributeSet
import android.view.Choreographer
import android.view.MotionEvent
import android.view.View
import androidx.core.view.ViewCompat
import java.util.Locale
import kotlin.math.floor

/**
 * CW level graph: the received tone level over the last few seconds, in dB above the noise
 * floor (green line on black, time runs left to right, newest at the right edge).
 *
 * - Grey line at 0 dB: the noise floor.
 * - Yellow line: the squelch threshold. The tone has to reach it to be decoded.
 *   Drag up or down anywhere on the graph to move it (this switches Auto off).
 * - AUTO button (bottom right): the decoder sets the squelch itself, a little above the noise
 *   peaks it measures in the pauses; the yellow line then follows that value.
 * - Dashed green line: where the decoder currently cuts the keying (halfway between noise and
 *   tone), shown when it is above the yellow line.
 *
 * While [running], the view pulls new points from [source] on every display frame and scrolls
 * at the steady point rate with a small look-ahead, like the waterfall, so the line glides
 * instead of jumping whenever a burst of audio arrives. The pacing uses the vsync time of each
 * frame (Choreographer), not the time onDraw happens to run, which varies with UI work.
 */
class CwScopeView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : View(context, attrs) {

    /**
     * Fills `levels` with the newest points (oldest first) and `total[0]` with the number of
     * points produced so far; returns how many points were written.
     */
    var source: ((levels: FloatArray, total: LongArray) -> Int)? = null

    /** Called while the threshold is dragged (`finished` = finger lifted). */
    var onThresholdChanged: ((db: Float, finished: Boolean) -> Unit)? = null

    /** Called when Auto is switched on or off from the view (button or dragging the line). */
    var onAutoChanged: ((enabled: Boolean) -> Unit)? = null

    /** Automatic squelch: the line shows [autoThresholdDb] and the AUTO button is lit. */
    var autoMode = true
        set(value) {
            field = value
            updateDescription()
            invalidate()
        }

    /** The squelch the decoder chose in Auto mode, dB over noise. */
    var autoThresholdDb = DEFAULT_THRESHOLD_DB
        set(value) {
            field = value
            if (autoMode) invalidate()
        }

    /** The squelch currently drawn: the automatic or the manual value. */
    private val shownThresholdDb: Float
        get() = if (autoMode) autoThresholdDb else thresholdDb

    var running = false
        set(value) {
            field = value
            started = false
            if (value) scheduleFrame() else cancelFrame()
        }

    var thresholdDb = DEFAULT_THRESHOLD_DB
        set(value) {
            field = value.coerceIn(MIN_THRESHOLD_DB, MAX_THRESHOLD_DB)
            updateDescription()
            invalidate()
        }

    /** Level where the decoder cuts the keying, dB over noise (NaN: unknown). */
    var cutDb = Float.NaN

    private val levels = FloatArray(BUFFER_POINTS)
    private val totalHolder = LongArray(1)
    private var levelCount = 0
    private var newestIndex = -1L     // index of levels[levelCount - 1]

    // Scroll pacing (in points).
    private var started = false
    private var position = 0.0        // point index drawn at the right edge (fractional)
    private var lastFrameNs = 0L
    private var lastNewDataNs = 0L
    private var lastPullNs = 0L
    private var frameScheduled = false
    private val frameCallback = Choreographer.FrameCallback { frameTimeNanos ->
        frameScheduled = false
        if (!running) return@FrameCallback
        pullAndPace(frameTimeNanos)
        invalidate()
        scheduleFrame()
    }

    private val density = resources.displayMetrics.density
    private val tracePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.rgb(0x4C, 0xE0, 0x6A)
        style = Paint.Style.STROKE
        strokeWidth = 1.5f * density
        strokeJoin = Paint.Join.ROUND
    }
    private val gridPaint = Paint().apply {
        color = Color.rgb(0x1E, 0x2A, 0x20)
        strokeWidth = 1f
    }
    private val noisePaint = Paint().apply {
        color = Color.rgb(0x80, 0x80, 0x80)
        strokeWidth = 1f * density
    }
    private val thresholdPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.rgb(0xFF, 0xD5, 0x4F)
        strokeWidth = 2f * density
    }
    private val cutPaint = Paint().apply {
        color = Color.argb(0xB0, 0x4C, 0xE0, 0x6A)
        strokeWidth = 1f * density
        style = Paint.Style.STROKE
        pathEffect = DashPathEffect(floatArrayOf(6f * density, 4f * density), 0f)
    }
    private val labelPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(0xA0, 0xFF, 0xFF, 0xFF)
        textSize = 10f * resources.displayMetrics.scaledDensity
    }
    private val thresholdLabelPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.rgb(0xFF, 0xD5, 0x4F)
        textSize = 12f * resources.displayMetrics.scaledDensity
        textAlign = Paint.Align.RIGHT
    }
    private val autoFillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.rgb(0xFF, 0xD5, 0x4F)
    }
    private val autoStrokePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.rgb(0xFF, 0xD5, 0x4F)
        style = Paint.Style.STROKE
        strokeWidth = 1f * density
    }
    private val autoTextPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textSize = 11f * resources.displayMetrics.scaledDensity
        textAlign = Paint.Align.CENTER
        isFakeBoldText = true
    }
    private val blackPaint = Paint().apply { color = Color.BLACK }
    private val autoRect = RectF()
    private var autoPressed = false
    private val tracePath = Path()
    private val cutPath = Path()

    init {
        setBackgroundColor(Color.BLACK)
        updateDescription()
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

    private fun pullAndPace(now: Long) {
        val pull = source ?: return
        lastPullNs = now
        val count = pull(levels, totalHolder)
        levelCount = count
        if (totalHolder[0] - 1 != newestIndex) lastNewDataNs = now
        newestIndex = totalHolder[0] - 1
        if (count <= 0) return

        val newest = newestIndex.toDouble()
        if (!started || newest < position - BUFFER_POINTS) {
            started = true
            position = newest - LOOK_AHEAD_POINTS
            lastFrameNs = now
        }
        val dt = ((now - lastFrameNs) / 1e9).coerceIn(0.0, 0.1)
        lastFrameNs = now

        val lead = newest - position
        if (lead > VISIBLE_POINTS / 2) {
            position = newest - LOOK_AHEAD_POINTS  // fell far behind: jump
        } else {
            val speed = (POINTS_PER_SECOND + (lead - LOOK_AHEAD_POINTS) * CATCH_UP_PER_SECOND)
                .coerceIn(0.0, POINTS_PER_SECOND * 1.5)
            position = (position + speed * dt).coerceAtMost(newest)
        }
    }

    private fun dbToY(db: Float): Float {
        val top = paddingTop.toFloat()
        val height = (this.height - paddingTop - paddingBottom).toFloat()
        val fraction = (db.coerceIn(MIN_DB, MAX_DB) - MIN_DB) / (MAX_DB - MIN_DB)
        return top + height * (1f - fraction)
    }

    private fun yToDb(y: Float): Float {
        val height = (this.height - paddingTop - paddingBottom).toFloat().coerceAtLeast(1f)
        val fraction = 1f - (y - paddingTop) / height
        return MIN_DB + fraction * (MAX_DB - MIN_DB)
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val w = width.toFloat()

        // Grid every 10 dB with labels.
        var db = 10f
        while (db < MAX_DB) {
            val y = dbToY(db)
            canvas.drawLine(0f, y, w, y, gridPaint)
            canvas.drawText("+${db.toInt()}", 4f * density, y - 2f * density, labelPaint)
            db += 10f
        }
        val noiseY = dbToY(0f)
        canvas.drawLine(0f, noiseY, w, noiseY, noisePaint)

        // Level trace: point `index` is drawn (position - index) steps left of the right edge.
        if (levelCount > 1 && started) {
            val step = w / (VISIBLE_POINTS - 1)
            val firstIndex = newestIndex - levelCount + 1
            // Only the points from just left of the view up to just right of the edge.
            val leftIndex = maxOf(firstIndex, floor(position - VISIBLE_POINTS).toLong() - 1)
            val rightIndex = minOf(newestIndex, floor(position).toLong() + 1)
            tracePath.reset()
            var first = true
            var index = leftIndex
            while (index <= rightIndex) {
                val x = (w - (position - index) * step).toFloat()
                val y = dbToY(levels[(index - firstIndex).toInt()])
                if (first) tracePath.moveTo(x, y) else tracePath.lineTo(x, y)
                first = false
                index++
            }
            canvas.save()
            canvas.clipRect(0f, 0f, w, height.toFloat())
            canvas.drawPath(tracePath, tracePaint)
            canvas.restore()
        }

        // Keying cut level, when the decoder runs above the squelch line.
        val squelchDb = shownThresholdDb
        if (!cutDb.isNaN() && cutDb > squelchDb + 0.5f) {
            val y = dbToY(cutDb)
            cutPath.reset()
            cutPath.moveTo(0f, y)
            cutPath.lineTo(w, y)
            canvas.drawPath(cutPath, cutPaint)
        }

        // Squelch threshold with its value.
        val thresholdY = dbToY(squelchDb)
        canvas.drawLine(0f, thresholdY, w, thresholdY, thresholdPaint)
        val label = String.format(Locale.US, if (autoMode) "auto %.0f dB" else "%.0f dB", squelchDb)
        val labelY = if (thresholdY - 4f * density - thresholdLabelPaint.textSize > 0f) {
            thresholdY - 4f * density
        } else {
            thresholdY + thresholdLabelPaint.textSize + 2f * density
        }
        canvas.drawText(label, w - 4f * density, labelY, thresholdLabelPaint)

        // AUTO button, bottom right: filled when on, outlined when off.
        val buttonW = autoTextPaint.measureText(AUTO_LABEL) + 16f * density
        val buttonH = 22f * density
        val margin = 6f * density
        autoRect.set(w - margin - buttonW, height - margin - buttonH, w - margin, height - margin)
        val radius = buttonH / 2f
        if (autoMode || autoPressed) {
            canvas.drawRoundRect(autoRect, radius, radius, autoFillPaint)
            autoTextPaint.color = Color.BLACK
        } else {
            canvas.drawRoundRect(autoRect, radius, radius, blackPaint)
            canvas.drawRoundRect(autoRect, radius, radius, autoStrokePaint)
            autoTextPaint.color = autoFillPaint.color
        }
        val textY = autoRect.centerY() - (autoTextPaint.descent() + autoTextPaint.ascent()) / 2f
        canvas.drawText(AUTO_LABEL, autoRect.centerX(), textY, autoTextPaint)
    }

    private fun isOnAutoButton(x: Float, y: Float): Boolean {
        val slop = 8f * density  // a little larger than drawn, easier to hit
        return x >= autoRect.left - slop && x <= autoRect.right + slop &&
            y >= autoRect.top - slop && y <= autoRect.bottom + slop
    }

    private fun setAutoFromUser(enabled: Boolean) {
        if (autoMode == enabled) return
        if (!enabled) thresholdDb = autoThresholdDb  // keep the line where it is
        autoMode = enabled
        onAutoChanged?.invoke(enabled)
    }

    /** Moving the line by hand switches to the manual threshold. */
    private fun dragThreshold(y: Float, finished: Boolean) {
        setAutoFromUser(false)
        thresholdDb = yToDb(y)
        onThresholdChanged?.invoke(thresholdDb, finished)
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                parent?.requestDisallowInterceptTouchEvent(true)
                if (isOnAutoButton(event.x, event.y)) {
                    autoPressed = true
                    invalidate()
                } else {
                    dragThreshold(event.y, finished = false)
                }
            }
            MotionEvent.ACTION_MOVE -> {
                if (!autoPressed) dragThreshold(event.y, finished = false)
            }
            MotionEvent.ACTION_UP -> {
                if (autoPressed) {
                    autoPressed = false
                    if (isOnAutoButton(event.x, event.y)) {
                        setAutoFromUser(!autoMode)
                        performClick()
                    }
                    invalidate()
                } else {
                    dragThreshold(event.y, finished = true)
                }
            }
            MotionEvent.ACTION_CANCEL -> {
                if (autoPressed) {
                    autoPressed = false
                    invalidate()
                } else {
                    onThresholdChanged?.invoke(thresholdDb, true)
                }
            }
        }
        return true
    }

    override fun performClick(): Boolean {
        super.performClick()
        return true
    }

    private fun updateDescription() {
        contentDescription = if (autoMode) {
            "CW signal level graph. Squelch is automatic. Drag up or down to set it by hand."
        } else {
            String.format(
                Locale.US,
                "CW signal level graph. Squelch threshold %.0f dB above noise. Drag up or down to change it.",
                thresholdDb,
            )
        }
    }

    init {
        ViewCompat.addAccessibilityAction(this, "Toggle automatic squelch") { _, _ ->
            setAutoFromUser(!autoMode)
            true
        }
    }

    companion object {
        const val MIN_THRESHOLD_DB = 3f
        const val MAX_THRESHOLD_DB = 30f
        const val DEFAULT_THRESHOLD_DB = 8f
        private const val AUTO_LABEL = "AUTO"
        const val BUFFER_POINTS = 512          // matches CwDecoder::kScopePoints
        private const val VISIBLE_POINTS = 400 // 4 s at 100 points per second
        private const val POINTS_PER_SECOND = 100.0
        private const val LOOK_AHEAD_POINTS = 10.0  // 100 ms: absorbs audio bursts
        private const val CATCH_UP_PER_SECOND = 2.0
        private const val IDLE_AFTER_NS = 500_000_000L
        private const val IDLE_POLL_MS = 100L
        private const val MIN_DB = -6f
        private const val MAX_DB = 36f
    }
}
