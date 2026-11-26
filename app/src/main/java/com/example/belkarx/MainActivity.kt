package hu.ha8mz.belkarx

import android.Manifest
import android.content.pm.PackageManager
import android.content.Context
import android.graphics.PixelFormat
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioRecord
import android.media.MediaRecorder
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.Process
import android.content.res.Configuration
import android.util.Log
import android.view.Choreographer
import android.view.GestureDetector
import android.view.Gravity
import android.view.MotionEvent
import android.view.Surface
import android.view.SurfaceHolder
import android.view.View
import android.view.ViewConfiguration
import android.view.WindowManager
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.view.ViewGroup
import android.widget.Toast
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.content.res.ColorStateList
import android.net.Uri
import androidx.activity.result.contract.ActivityResultContracts
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.app.AppCompatDelegate
import androidx.core.app.ActivityCompat
import com.google.android.material.color.MaterialColors
import com.google.android.material.slider.Slider
import hu.ha8mz.belkarx.databinding.ActivityMainBinding
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

class MainActivity : AppCompatActivity(), SurfaceHolder.Callback {

    private lateinit var binding: ActivityMainBinding
    private var audioRecord: AudioRecord? = null
    private val isRecording = AtomicBoolean(false)
    private var recordingThread: Thread? = null
    @Volatile private var surface: Surface? = null
    // The waterfall is drawn on its own thread with its own Choreographer, so UI work
    // (layout, slider drags, GC pauses on the main thread) cannot delay a frame.
    private var renderThread: HandlerThread? = null
    private var renderHandler: Handler? = null
    @Volatile private var useVsyncRenderLoop = false
    private lateinit var gestureDetector: GestureDetector
    private lateinit var prefs: android.content.SharedPreferences
    private var suppressZoomToggleCallback = false
    private val demodModeLabels = arrayOf("AM", "USB", "LSB")
    private var demodModeIndex = 0
    private val longPressHandler = Handler(Looper.getMainLooper())
    private var longPressTriggered = false
    private var longPressDownX = 0f
    private var longPressDownY = 0f
    private val markerLongPressTimeoutMs = 1000L

    // Decoded text of the CW and RTTY decoders (log for saving, tail shown in the strip).
    private lateinit var cwOutput: DecoderText
    private lateinit var rttyOutput: DecoderText
    private var pendingSaveOutput: DecoderText? = null
    private val cwStatus = FloatArray(6)
    private val rttyStatus = FloatArray(3)
    private var rttyBaudIndex = DEFAULT_RTTY_BAUD_INDEX
    private var rttyShiftIndex = DEFAULT_RTTY_SHIFT_INDEX
    private val rttyBaudSegments = mutableListOf<TextView>()
    private val rttyShiftSegments = mutableListOf<TextView>()
    private var colorScaleIndex = 0
    private val colorScaleSwatches = mutableListOf<View>()
    private val cwPollHandler = Handler(Looper.getMainLooper())
    private val cwPollRunnable = object : Runnable {
        override fun run() {
            pollCwDecoder()
            cwPollHandler.postDelayed(this, CW_POLL_INTERVAL_MS)
        }
    }
    private val rttyPollRunnable = object : Runnable {
        override fun run() {
            pollRttyDecoder()
            cwPollHandler.postDelayed(this, CW_POLL_INTERVAL_MS)
        }
    }
    private val saveLogLauncher =
        registerForActivityResult(ActivityResultContracts.CreateDocument("text/plain")) { uri ->
            val output = pendingSaveOutput
            pendingSaveOutput = null
            if (uri != null && output != null) writeLog(uri, output)
        }
    private val markerLongPressMoveToleranceMultiplier = 3f
    private val markerLongPressRunnable = Runnable {
        // In CW / RTTY mode the decoder needs the marker: a long press (easy to do by accident
        // while tuning) must not turn it off, which would also stop the decoder. The press simply
        // keeps acting as a marker drag, and the marker still snaps to the signal on release.
        if (decoderOn()) {
            Log.d("BelkaRx", "Waterfall long-press ignored in decoder mode")
            return@Runnable
        }
        longPressTriggered = true
        val newState = !binding.markerToggle.isChecked
        binding.markerToggle.isChecked = newState
        if (newState) {
            setAdjustableMarkerTouchX(longPressDownX)
        }
        Log.d("BelkaRx", "Waterfall long-press: marker toggled to $newState at x=$longPressDownX")
    }
    private val renderFrameCallback = object : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            // Runs on the render thread; Choreographer.getInstance() is that thread's instance.
            if (!useVsyncRenderLoop) return
            if (isRecording.get() && surface != null) {
                renderFrame(frameTimeNanos)
            }
            Choreographer.getInstance().postFrameCallback(this)
        }
    }

    private fun startVsyncRenderLoop() {
        if (useVsyncRenderLoop) return
        useVsyncRenderLoop = true
        val handler = renderHandler ?: HandlerThread("BelkaRx-Render", Process.THREAD_PRIORITY_DISPLAY).let { thread ->
            thread.start()
            renderThread = thread
            Handler(thread.looper).also { renderHandler = it }
        }
        handler.post {
            Choreographer.getInstance().removeFrameCallback(renderFrameCallback)
            Choreographer.getInstance().postFrameCallback(renderFrameCallback)
        }
    }

    private fun stopVsyncRenderLoop() {
        if (!useVsyncRenderLoop) return
        useVsyncRenderLoop = false
        renderHandler?.post { Choreographer.getInstance().removeFrameCallback(renderFrameCallback) }
    }

    override fun onDestroy() {
        cwPollHandler.removeCallbacks(cwPollRunnable)
        cwPollHandler.removeCallbacks(rttyPollRunnable)
        stopVsyncRenderLoop()
        renderHandler?.post { releaseRenderer() }
        renderThread?.quitSafely()
        renderThread = null
        renderHandler = null
        super.onDestroy()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        AppCompatDelegate.setDefaultNightMode(AppCompatDelegate.MODE_NIGHT_YES)
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        
        // Initialize SharedPreferences
        prefs = getSharedPreferences("BelkaRxSettings", Context.MODE_PRIVATE)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)
        cwOutput = DecoderText("CW", binding.cwText, binding.cwTextScroll) {
            if (cwStatus[0] > 0f) String.format(Locale.US, "Last speed: %.0f WPM\n", cwStatus[0]) else ""
        }
        rttyOutput = DecoderText("RTTY", binding.rttyText, binding.rttyTextScroll) {
            String.format(
                Locale.US, "Settings: %s Bd, %d Hz shift%s\n",
                RTTY_BAUD_LABELS[rttyBaudIndex], RTTY_SHIFTS[rttyShiftIndex],
                if (binding.rttyInvertToggle.isChecked) ", inverted" else "",
            )
        }
        
        // Hide the action bar to save space
        supportActionBar?.hide()

        // Setup gesture detector for surface double-tap
        gestureDetector = GestureDetector(this, GestureListener())

        // Set initial fullscreen mode based on orientation
        updateFullscreenMode(resources.configuration.orientation)

        binding.waterfallSurface.holder.addCallback(this)
        binding.waterfallSurface.setOnTouchListener { _, event ->
            gestureDetector.onTouchEvent(event)

            val touchSlop = ViewConfiguration.get(this).scaledTouchSlop.toFloat()
            val longPressMoveTolerance = touchSlop * markerLongPressMoveToleranceMultiplier
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    longPressDownX = event.x
                    longPressDownY = event.y
                    longPressTriggered = false
                    longPressHandler.removeCallbacks(markerLongPressRunnable)
                    longPressHandler.postDelayed(markerLongPressRunnable, markerLongPressTimeoutMs)
                }
                MotionEvent.ACTION_MOVE -> {
                    if (kotlin.math.abs(event.x - longPressDownX) > longPressMoveTolerance ||
                        kotlin.math.abs(event.y - longPressDownY) > longPressMoveTolerance) {
                        longPressHandler.removeCallbacks(markerLongPressRunnable)
                    }
                }
                MotionEvent.ACTION_UP,
                MotionEvent.ACTION_CANCEL -> {
                    longPressHandler.removeCallbacks(markerLongPressRunnable)
                    // With a decoder on, releasing the marker snaps it onto the nearest signal
                    // (in RTTY mode: the center of the nearest mark/space pair).
                    if (event.actionMasked == MotionEvent.ACTION_UP && !longPressTriggered &&
                        binding.markerToggle.isChecked && decoderOn()) {
                        snapMarkerToPeak()
                    }
                }
            }

            if (binding.markerToggle.isChecked) {
                when (event.actionMasked) {
                    MotionEvent.ACTION_DOWN,
                    MotionEvent.ACTION_MOVE -> {
                        if (!longPressTriggered) {
                            setAdjustableMarkerTouchX(event.x)
                        }
                    }
                }
            }
            true
        }
        
        setupColorScaleSwatches()

        binding.toggleControlsButton.setOnClickListener {
            binding.toggleControlsButton.isChecked = false // Soha ne maradjon benyomva
            val controlsContainer = binding.controlsContainer
            if (controlsContainer.visibility == View.VISIBLE) {
                controlsContainer.visibility = View.GONE
                binding.toggleControlsButton.textOn = "▼"
                binding.toggleControlsButton.textOff = "▼"
                binding.toggleControlsButton.isChecked = false // Update UI refresh
            } else {
                controlsContainer.visibility = View.VISIBLE
                binding.toggleControlsButton.textOn = "▲"
                binding.toggleControlsButton.textOff = "▲"
                binding.toggleControlsButton.isChecked = false // Update UI refresh
            }
        }

        binding.startStopButton.setOnClickListener {
            if (isRecording.get()) {
                stopRecording()
            } else {
                if (checkPermission()) {
                    startRecording()
                } else {
                    requestPermission()
                }
            }
        }

        binding.sensitivitySeekBar.addOnChangeListener { _: Slider, value: Float, fromUser: Boolean ->
            setSensitivity(value.toInt())
            if (fromUser) saveSettings()
        }

        binding.contrastSeekBar.addOnChangeListener { _: Slider, value: Float, fromUser: Boolean ->
            setContrast(value.toInt())
            if (fromUser) saveSettings()
        }

        binding.swapIQToggle.setOnCheckedChangeListener { _, isChecked ->
            setSwapIQ(isChecked)
            saveSettings()
            Log.d("BelkaRx", "Swap I/Q checkbox changed: $isChecked")
        }

        binding.zoomToggle.setOnCheckedChangeListener { _, isChecked ->
            if (suppressZoomToggleCallback) {
                return@setOnCheckedChangeListener
            }
            setZoom(isChecked)
            saveSettings()
            Log.d("BelkaRx", "Zoom checkbox changed: $isChecked")
        }

        binding.showSpectrumToggle.setOnCheckedChangeListener { _, isChecked ->
            setShowSpectrum(isChecked)
            updateSpectrumOptionControlsEnabled(isChecked)
            saveSettings()
            Log.d("BelkaRx", "Show spectrum checkbox changed: $isChecked")
        }

        binding.markerToggle.setOnCheckedChangeListener { _, isChecked ->
            setAdjustableMarkerEnabled(isChecked)
            // The decoders listen at the marker, so they cannot run without one.
            if (!isChecked && binding.cwToggle.isChecked) {
                binding.cwToggle.isChecked = false
            }
            if (!isChecked && binding.rttyToggle.isChecked) {
                binding.rttyToggle.isChecked = false
            }
            saveSettings()
            Log.d("BelkaRx", "Marker checkbox changed: $isChecked")
        }

        binding.cwToggle.setOnCheckedChangeListener { _, isChecked ->
            applyCwEnabled(isChecked)
            saveSettings()
            Log.d("BelkaRx", "CW decoder changed: $isChecked")
        }

        binding.rttyToggle.setOnCheckedChangeListener { _, isChecked ->
            applyRttyEnabled(isChecked)
            saveSettings()
            Log.d("BelkaRx", "RTTY decoder changed: $isChecked")
        }

        binding.cwPanel.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ -> updateOverlayInset() }
        binding.rttyPanel.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ -> updateOverlayInset() }

        binding.cwScope.source = { levels, total -> cwGetScope(levels, total) }
        binding.cwScope.onAutoChanged = { enabled ->
            setCwAutoThreshold(enabled)
            // Switching Auto off keeps the line where it was: that becomes the manual value.
            if (!enabled) setCwThresholdDb(binding.cwScope.thresholdDb)
            prefs.edit()
                .putBoolean("cwThresholdAuto", enabled)
                .putFloat("cwThresholdDb", binding.cwScope.thresholdDb)
                .apply()
        }
        binding.cwScope.onThresholdChanged = { db, finished ->
            setCwThresholdDb(db)
            if (finished) prefs.edit().putFloat("cwThresholdDb", db).apply()
        }

        binding.cwClearButton.setOnClickListener {
            cwPollText()  // drop anything not shown yet
            cwOutput.clear()
        }
        binding.cwSaveButton.setOnClickListener { saveLog(cwOutput) }

        setupRttyControls()

        binding.demodModeButton.setOnClickListener {
            demodModeIndex = (demodModeIndex + 1) % demodModeLabels.size
            updateDemodModeButtonText()
            setDemodMode(demodModeIndex)
            saveSettings()
            Log.d("BelkaRx", "Demod mode changed: ${demodModeLabels[demodModeIndex]}")
        }

        binding.spectrumFilledToggle.setOnCheckedChangeListener { _, isChecked ->
            setSpectrumFilled(isChecked)
            saveSettings()
            Log.d("BelkaRx", "Spectrum filled checkbox changed: $isChecked")
        }

        binding.spectrumConstantColorToggle.setOnCheckedChangeListener { _, isChecked ->
            setSpectrumConstantColor(isChecked)
            saveSettings()
            Log.d("BelkaRx", "Spectrum constant color checkbox changed: $isChecked")
        }


        // Load saved settings
        loadSettings()

        setSensitivity(binding.sensitivitySeekBar.value.toInt())
        setContrast(binding.contrastSeekBar.value.toInt())
        setSwapIQ(binding.swapIQToggle.isChecked)
        setZoom(binding.zoomToggle.isChecked)
        setFixedWindowEnabled(true)
        setShowSpectrum(binding.showSpectrumToggle.isChecked)
        setAdjustableMarkerEnabled(binding.markerToggle.isChecked)
        setDemodMode(demodModeIndex)
        setSpectrumFilled(binding.spectrumFilledToggle.isChecked)
        setSpectrumConstantColor(binding.spectrumConstantColorToggle.isChecked)
        setColorScale(colorScaleIndex)
        updateSpectrumOptionControlsEnabled(binding.showSpectrumToggle.isChecked)
        applyCwEnabled(binding.cwToggle.isChecked)
        applyRttyEnabled(binding.rttyToggle.isChecked)
        Log.d("BelkaRx", "Initial UI setup: Swap I/Q=${binding.swapIQToggle.isChecked}")


    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        updateFullscreenMode(newConfig.orientation)
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) {
            applyImmersiveFullscreen()
        }
    }

    private fun decoderOn() = binding.cwToggle.isChecked || binding.rttyToggle.isChecked

    /** The waterfall overlay keeps the marker above the visible decoder strip. */
    private fun updateOverlayInset() {
        val panel = listOf(binding.cwPanel, binding.rttyPanel).firstOrNull { it.visibility == View.VISIBLE }
        setOverlayBottomInset(panel?.height ?: 0)  // a panel just shown updates again after layout
    }

    private fun applyCwEnabled(enabled: Boolean) {
        // One decoder at a time: they share the marker and the strip at the bottom.
        if (enabled && binding.rttyToggle.isChecked) {
            binding.rttyToggle.isChecked = false
        }
        // The decoder listens at the marker, so turning CW on also turns the marker on.
        if (enabled && !binding.markerToggle.isChecked) {
            binding.markerToggle.isChecked = true
        }
        setCwEnabled(enabled)
        binding.cwPanel.visibility = if (enabled) View.VISIBLE else View.GONE
        binding.cwScope.running = enabled
        updateOverlayInset()
        cwPollHandler.removeCallbacks(cwPollRunnable)
        if (enabled) {
            cwPollHandler.post(cwPollRunnable)
        } else {
            binding.cwKeyIndicator.backgroundTintList = ColorStateList.valueOf(CW_COLOR_IDLE)
        }
    }

    private fun pollCwDecoder() {
        val text = cwPollText()
        if (text.isNotEmpty()) cwOutput.append(text)

        cwGetStatus(cwStatus)
        binding.cwScope.cutDb = cwStatus[4]
        binding.cwScope.autoThresholdDb = cwStatus[5]
        val wpm = cwStatus[0]
        binding.cwWpmText.text = if (wpm > 0f) String.format(Locale.US, "%.0f WPM", wpm) else "-- WPM"
        val color = when {
            cwStatus[2] > 0.5f -> CW_COLOR_KEY_DOWN
            cwStatus[3] > 0.5f -> CW_COLOR_SIGNAL
            else -> CW_COLOR_IDLE
        }
        binding.cwKeyIndicator.backgroundTintList = ColorStateList.valueOf(color)
    }

    private fun applyRttyEnabled(enabled: Boolean) {
        // One decoder at a time: they share the marker and the strip at the bottom.
        if (enabled && binding.cwToggle.isChecked) {
            binding.cwToggle.isChecked = false
        }
        // The decoder is centered on the marker, so turning RTTY on also turns the marker on.
        if (enabled && !binding.markerToggle.isChecked) {
            binding.markerToggle.isChecked = true
        }
        setRttyEnabled(enabled)
        binding.rttyPanel.visibility = if (enabled) View.VISIBLE else View.GONE
        binding.rttyScope.running = enabled
        updateOverlayInset()
        cwPollHandler.removeCallbacks(rttyPollRunnable)
        if (enabled) {
            cwPollHandler.post(rttyPollRunnable)
        } else {
            binding.rttySignalIndicator.backgroundTintList = ColorStateList.valueOf(CW_COLOR_IDLE)
        }
    }

    /** Baud rate and shift choices, Invert, the tuning scope source, Clear and Save. */
    private fun setupRttyControls() {
        buildSegments(binding.rttyBaudRow, RTTY_BAUD_LABELS, RTTY_BAUD_LABELS.map { "$it baud" }, rttyBaudSegments) {
            rttyBaudIndex = it
            applyRttyParams()
            saveSettings()
        }
        buildSegments(
            binding.rttyShiftRow, RTTY_SHIFTS.map { it.toString() }.toTypedArray(),
            RTTY_SHIFTS.map { "$it hertz shift" }, rttyShiftSegments,
        ) {
            rttyShiftIndex = it
            applyRttyParams()
            saveSettings()
        }
        binding.rttyInvertToggle.setOnCheckedChangeListener { _, _ ->
            applyRttyParams()
            saveSettings()
        }
        binding.rttyScope.source = { xy -> rttyGetScope(xy) }
        binding.rttyClearButton.setOnClickListener {
            rttyPollText()  // drop anything not shown yet
            rttyOutput.clear()
        }
        binding.rttySaveButton.setOnClickListener { saveLog(rttyOutput) }
        applyRttyParams()
    }

    private fun applyRttyParams() {
        showSelectedSegment(rttyBaudSegments, rttyBaudIndex)
        showSelectedSegment(rttyShiftSegments, rttyShiftIndex)
        setRttyParams(RTTY_BAUDS[rttyBaudIndex], RTTY_SHIFTS[rttyShiftIndex].toFloat(), binding.rttyInvertToggle.isChecked)
    }

    private fun pollRttyDecoder() {
        val text = rttyPollText()
        if (text.isNotEmpty()) rttyOutput.append(text)

        rttyGetStatus(rttyStatus)
        val signal = rttyStatus[1] > 0.5f
        binding.rttyScope.signal = signal
        binding.rttySnrText.text =
            if (isRecording.get()) String.format(Locale.US, "%.0f dB", rttyStatus[0]) else "-- dB"
        // Squelch open: the dot shows the current bit (green mark, orange space).
        val color = when {
            !signal -> CW_COLOR_IDLE
            rttyStatus[2] > 0.5f -> RttyScopeView.MARK_COLOR
            else -> RttyScopeView.SPACE_COLOR
        }
        binding.rttySignalIndicator.backgroundTintList = ColorStateList.valueOf(color)
    }

    /**
     * A row of pill-shaped choices (one selected), like a segmented button. [onSelect] gets the
     * index of the tapped one.
     */
    private fun buildSegments(
        row: LinearLayout,
        labels: Array<String>,
        descriptions: List<String>,
        out: MutableList<TextView>,
        onSelect: (Int) -> Unit,
    ) {
        row.removeAllViews()
        out.clear()
        labels.forEachIndexed { index, label ->
            val segment = TextView(this).apply {
                text = label
                gravity = Gravity.CENTER
                textSize = 13f
                typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
                maxLines = 1
                contentDescription = descriptions[index]
                isClickable = true
                isFocusable = true
                setOnClickListener { onSelect(index) }
            }
            row.addView(segment, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f).apply {
                marginStart = if (index == 0) 0 else dp(4)
            })
            out.add(segment)
        }
    }

    private fun showSelectedSegment(segments: List<TextView>, selected: Int) {
        val primary = MaterialColors.getColor(binding.root, com.google.android.material.R.attr.colorPrimary)
        val onPrimary = MaterialColors.getColor(binding.root, com.google.android.material.R.attr.colorOnPrimary)
        segments.forEachIndexed { index, segment ->
            val isSelected = index == selected
            segment.isSelected = isSelected  // TalkBack reads "selected"
            segment.background = GradientDrawable().apply {
                cornerRadius = dp(100).toFloat()
                if (isSelected) {
                    setColor(primary)
                } else {
                    setColor(0x14FFFFFF)
                    setStroke(dp(1), 0x40FFFFFF)
                }
            }
            segment.setTextColor(if (isSelected) onPrimary else Color.WHITE)
        }
    }

    private fun saveLog(output: DecoderText) {
        if (output.isEmpty()) {
            Toast.makeText(this, "Nothing decoded yet", Toast.LENGTH_SHORT).show()
            return
        }
        pendingSaveOutput = output
        val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
        saveLogLauncher.launch("BelkaRx-${output.mode}-$stamp.txt")
    }

    private fun writeLog(uri: Uri, output: DecoderText) {
        val content = buildString {
            append("BelkaRx ${output.mode} log\n")
            append("Saved: ").append(SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(Date())).append('\n')
            append(output.details())
            append('\n')
            append(output.log)
            append('\n')
        }
        try {
            contentResolver.openOutputStream(uri)?.use { it.write(content.toByteArray(Charsets.UTF_8)) }
            Toast.makeText(this, "${output.mode} log saved", Toast.LENGTH_SHORT).show()
        } catch (e: Exception) {
            Log.e("BelkaRx", "Saving ${output.mode} log failed: ${e.message}")
            Toast.makeText(this, "Could not save the ${output.mode} log", Toast.LENGTH_LONG).show()
        }
    }

    /**
     * Decoded text of one decoder: the full log for saving (each transmission starts on a new
     * line with a timestamp, after a pause of at least [DECODER_NEW_LINE_PAUSE_MS]) and the tail
     * shown in the strip, which can be scrolled back. The strip follows the newest text while
     * scrolled to the bottom; scrolled up, it stays put so the text can be read.
     */
    private inner class DecoderText(
        val mode: String,
        private val view: TextView,
        private val scroll: ScrollView,
        val details: () -> String,
    ) {
        val log = StringBuilder()
        private val display = StringBuilder()
        private var lastTextMs = 0L
        private var following = true

        init {
            // Scroll in the layout pass (before the frame is drawn), so new lines never jump.
            val keepAtBottom = View.OnLayoutChangeListener { _, _, _, _, _, _, _, _, _ ->
                if (following) scroll.scrollTo(0, maxOf(0, view.height - scroll.height))
            }
            view.addOnLayoutChangeListener(keepAtBottom)
            scroll.addOnLayoutChangeListener(keepAtBottom)
            scroll.setOnScrollChangeListener { _, _, scrollY, _, _ ->
                following = scrollY >= view.height - scroll.height - dp(8)
            }
        }

        fun isEmpty() = log.isEmpty()

        fun append(text: String) {
            val now = System.currentTimeMillis()
            if (log.isEmpty() || now - lastTextMs > DECODER_NEW_LINE_PAUSE_MS) {
                if (log.isNotEmpty() && log.last() != '\n') log.append('\n')
                log.append('[')
                    .append(SimpleDateFormat("HH:mm:ss", Locale.US).format(Date(now)))
                    .append("] ")
                if (display.isNotEmpty() && display.last() != ' ' && display.last() != '\n') display.append(' ')
            }
            log.append(text)
            display.append(text)
            if (display.length > DECODER_DISPLAY_CHARS) {
                // Drop a big chunk at once, so the wrapped lines reflow only rarely.
                display.delete(0, display.length - DECODER_DISPLAY_CHARS / 2)
            }
            lastTextMs = now
            show()
        }

        fun clear() {
            log.setLength(0)
            display.setLength(0)
            following = true
            show()
        }

        // The text view wraps the text; the scroll view shows the visible window.
        fun show() {
            view.text = display
        }
    }

    private fun updateSpectrumOptionControlsEnabled(isSpectrumEnabled: Boolean) {
        binding.spectrumFilledToggle.isEnabled = isSpectrumEnabled
        binding.spectrumConstantColorToggle.isEnabled = isSpectrumEnabled
    }

    private fun applyImmersiveFullscreen() {
        window.decorView.systemUiVisibility = (
            View.SYSTEM_UI_FLAG_FULLSCREEN or
            View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
        )
        supportActionBar?.hide()
    }

    private fun updateFullscreenMode(orientation: Int) {
        applyImmersiveFullscreen()
        updateLayoutForOrientation(orientation)
    }

    private fun updateLayoutForOrientation(orientation: Int) {
        // Decoder text strips: 3 lines in portrait, 2 in landscape where height is short.
        val landscape = orientation == Configuration.ORIENTATION_LANDSCAPE
        // CW text scrolls: 3 visible lines in portrait, 2 in landscape.
        binding.cwTextScroll.layoutParams = binding.cwTextScroll.layoutParams.apply {
            height = binding.cwText.lineHeight * (if (landscape) 2 else 3)
        }
        binding.cwScope.layoutParams = binding.cwScope.layoutParams.apply {
            height = dp(if (landscape) 88 else 140)
        }
        // RTTY text scrolls: 6 visible lines in portrait, 2 in landscape.
        binding.rttyTextScroll.layoutParams = binding.rttyTextScroll.layoutParams.apply {
            height = binding.rttyText.lineHeight * (if (landscape) 2 else 6)
        }
        binding.rttyScope.layoutParams = binding.rttyScope.layoutParams.apply {
            height = dp(if (landscape) 88 else 140)
        }
        // RTTY settings: captions above the choices in portrait, beside them in landscape.
        for (group in listOf(binding.rttyBaudGroup, binding.rttyShiftGroup)) {
            group.orientation = if (landscape) LinearLayout.HORIZONTAL else LinearLayout.VERTICAL
            val caption = group.getChildAt(0)
            caption.layoutParams = LinearLayout.LayoutParams(
                if (landscape) dp(72) else ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
            ).apply { bottomMargin = if (landscape) 0 else dp(2) }
            val choices = group.getChildAt(1)
            choices.layoutParams = LinearLayout.LayoutParams(
                if (landscape) 0 else ViewGroup.LayoutParams.MATCH_PARENT,
                dp(32),
                if (landscape) 1f else 0f,
            )
        }

        val topContainer = binding.topBarCenterContainer
        val dropdownContainer = binding.dropdownDynamicArea
        val controlsContainer = binding.controlsContainer
        val mainToggleContainer = binding.mainToggleContainer
        
        // Remove views from their current parents safely
        val viewsToMove = listOf(
            binding.spinnerContainer,
            binding.sensitivityView,
            binding.contrastView,
            binding.demodModeButton,
            binding.showSpectrumToggle,
            binding.spectrumOptionsContainer
        )
        for (v in viewsToMove) {
            (v.parent as? ViewGroup)?.removeView(v)
        }
        
        // Helper function for layout params
        // Spinners and sliders sit 8dp in, so with their own 8dp padding they line up with the 16dp button rows.
        fun getDropParams() = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            setMargins(dp(8), dp(8), dp(8), 0)
        }
        
        fun getTopParams(marginStart: Int = 0) = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply {
            setMargins(marginStart, 0, 0, 0)
        }

        fun getSpectrumOptionsParams() = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply {
            setMargins(dp(16), dp(8), dp(16), 0)
        }

        fun getSpectrumOptionsInlineParams() = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        )

        fun getTopModeButtonParams() = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply {
            setMargins(dp(8), 0, dp(8), 0)
        }

        if (orientation == Configuration.ORIENTATION_LANDSCAPE) {
            // In Landscape: Sensitivity & Contrast on top, Color Scale in dropdown,
            // spectrum option toggles continue inline on the main toggle row.
            binding.sensitivityView.layoutParams = getTopParams()
            binding.demodModeButton.layoutParams = getTopModeButtonParams()
            binding.contrastView.layoutParams = getTopParams()
            topContainer.addView(binding.sensitivityView)
            topContainer.addView(binding.demodModeButton)
            topContainer.addView(binding.contrastView)
            
            binding.spinnerContainer.layoutParams = getDropParams()
            dropdownContainer.addView(binding.spinnerContainer)

            binding.showSpectrumToggle.layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply {
                setMargins(4, 0, 0, 0)
            }
            mainToggleContainer.addView(binding.showSpectrumToggle)

            binding.spectrumOptionsContainer.layoutParams = getSpectrumOptionsInlineParams()
            mainToggleContainer.addView(binding.spectrumOptionsContainer)
        } else {
            // In Portrait: place Spectrum + Fill + Mono on the same row.
            binding.sensitivityView.layoutParams = getTopParams()
            topContainer.addView(binding.sensitivityView)
            
            binding.spinnerContainer.layoutParams = getDropParams()
            binding.contrastView.layoutParams = getDropParams()
            dropdownContainer.addView(binding.spinnerContainer)
            dropdownContainer.addView(binding.contrastView)

            binding.demodModeButton.layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply {
                setMargins(4, 0, 0, 0)
            }
            val markerIndex = mainToggleContainer.indexOfChild(binding.markerToggle)
            if (markerIndex >= 0) {
                mainToggleContainer.addView(binding.demodModeButton, markerIndex + 1)
            } else {
                mainToggleContainer.addView(binding.demodModeButton)
            }

            binding.showSpectrumToggle.layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
            binding.spectrumOptionsContainer.addView(binding.showSpectrumToggle, 0)

            binding.spectrumOptionsContainer.layoutParams = getSpectrumOptionsParams()
            controlsContainer.addView(binding.spectrumOptionsContainer)
        }

        distributeEvenly(mainToggleContainer)
        distributeEvenly(binding.spectrumOptionsContainer)
    }

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).toInt()

    /**
     * Spreads the buttons of a row across its full width: each keeps its natural width and
     * gets an equal share of the leftover space, with the same gap between neighbours.
     * A nested row (the inline spectrum options in landscape) counts as one button per child.
     */
    private fun distributeEvenly(row: LinearLayout) {
        for (i in 0 until row.childCount) {
            val child = row.getChildAt(i)
            val params = child.layoutParams as LinearLayout.LayoutParams
            params.width = ViewGroup.LayoutParams.WRAP_CONTENT
            params.weight = if (child is LinearLayout) child.childCount.toFloat() else 1f
            params.setMargins(if (i == 0) 0 else dp(6), 0, 0, 0)
            child.layoutParams = params
        }
    }

    /**
     * Waterfall color scale picker: one small swatch per scale showing its gradient (sampled
     * from the native color scale, weak signals on the left). The selected one has a white
     * frame; long-press shows the name.
     */
    private fun setupColorScaleSwatches() {
        val row = binding.colorScaleRow
        row.removeAllViews()
        colorScaleSwatches.clear()
        val preview = IntArray(16)
        COLOR_SCALE_NAMES.forEachIndexed { index, name ->
            getColorScalePreview(index, preview)
            val swatch = View(this).apply {
                background = GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT, preview.copyOf()).apply {
                    cornerRadius = dp(6).toFloat()
                }
                contentDescription = "$name color scale"
                tooltipText = name
                isClickable = true
                isFocusable = true
                setOnClickListener { selectColorScale(index, fromUser = true) }
            }
            row.addView(swatch, LinearLayout.LayoutParams(0, dp(26), 1f).apply {
                marginStart = if (index == 0) 0 else dp(6)
            })
            colorScaleSwatches.add(swatch)
        }
        selectColorScale(colorScaleIndex, fromUser = false)
    }

    private fun selectColorScale(index: Int, fromUser: Boolean) {
        colorScaleIndex = index.coerceIn(0, COLOR_SCALE_NAMES.lastIndex)
        colorScaleSwatches.forEachIndexed { i, swatch ->
            val selected = i == colorScaleIndex
            swatch.isSelected = selected  // TalkBack reads "selected"
            (swatch.background as? GradientDrawable)?.setStroke(
                dp(if (selected) 2 else 1),
                if (selected) Color.WHITE else 0x66FFFFFF,
            )
        }
        setColorScale(colorScaleIndex)
        if (fromUser) {
            saveSettings()
            Log.d("BelkaRx", "Color scale selected: ${COLOR_SCALE_NAMES[colorScaleIndex]}")
        }
    }

    private fun checkPermission(): Boolean {
        return ActivityCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED
    }

    private fun requestPermission() {
        ActivityCompat.requestPermissions(this, arrayOf(Manifest.permission.RECORD_AUDIO), 1001)
    }

    private fun startRecording() {
        Log.i("BelkaRx", "startRecording() called")
        // Capture always uses the input Android routes to: a connected USB sound card, otherwise
        // the built-in microphone. (There used to be a device list, but it never changed the route.)
        if (!checkPermission()) return

        binding.statusText.text = "Status: Starting AudioRecord UNPROCESSED capture..."
        
        try {
            if (!tryAudioRecordUnprocessed()) {
                binding.statusText.text = "Status: Error - Failed to start AudioRecord UNPROCESSED"
                Log.e("BelkaRx", "Failed to start AudioRecord UNPROCESSED")
                return
            }
        } catch (e: Exception) {
            binding.statusText.text = "Status: Crash - ${e.message}"
            Log.e("BelkaRx", "startRecording crash: ${e.message}", e)
        }
    }

    /**
     * Try to capture stereo audio using AudioRecord with UNPROCESSED source (API 24+)
     */
    private fun tryAudioRecordUnprocessed(): Boolean {
        return try {
            val sampleRate = 192000
            val channelConfig = AudioFormat.CHANNEL_IN_STEREO
            val audioFormat = AudioFormat.ENCODING_PCM_16BIT
            
            // Calculate buffer size
            val bufferSize = AudioRecord.getMinBufferSize(sampleRate, channelConfig, audioFormat)
            if (bufferSize <= 0) {
                Log.w("BelkaRx", "Invalid buffer size: $bufferSize")
                return false
            }

            // Room for 4 FFT windows (~85 ms at 192 kHz), so a late read never overruns
            // the recorder and drops samples.
            val fftWindowBytes = FFT_SIZE * 2 /* channels */ * 2 /* bytes */
            val recordBufferBytes = maxOf(bufferSize * 2, fftWindowBytes * 4)
            Log.i("BelkaRx", "Attempting AudioRecord with UNPROCESSED source: minBufferSize=$bufferSize, using $recordBufferBytes")

            // Try to create AudioRecord with UNPROCESSED source (API 24+)
            audioRecord = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
                // Use AudioRecordingConfiguration for unprocessed audio (if available)
                try {
                    val audioRecordBuilder = AudioRecord.Builder()
                        .setAudioSource(MediaRecorder.AudioSource.UNPROCESSED)
                        .setAudioFormat(
                            AudioFormat.Builder()
                                .setEncoding(audioFormat)
                                .setSampleRate(sampleRate)
                                .setChannelMask(channelConfig)
                                .build()
                        )
                        .setBufferSizeInBytes(recordBufferBytes)

                    // Note: setAudioDevice is not available in AudioRecord.Builder
                    // The UNPROCESSED source should handle device selection

                    audioRecordBuilder.build()
                } catch (e: Exception) {
                    Log.w("BelkaRx", "AudioRecord.Builder failed: ${e.message}, using fallback")
                    AudioRecord(
                        MediaRecorder.AudioSource.UNPROCESSED,
                        sampleRate,
                        channelConfig,
                        audioFormat,
                        recordBufferBytes
                    )
                }
            } else {
                AudioRecord(
                    MediaRecorder.AudioSource.UNPROCESSED,
                    sampleRate,
                    channelConfig,
                    audioFormat,
                    recordBufferBytes
                )
            }

            if (audioRecord?.state != AudioRecord.STATE_INITIALIZED) {
                Log.w("BelkaRx", "AudioRecord failed to initialize with UNPROCESSED")
                audioRecord?.release()
                audioRecord = null
                return false
            }

            setNativeSampleRate(sampleRate)
            audioRecord?.startRecording()
            
            isRecording.set(true)
            binding.startStopButton.isChecked = true

            // Wait a bit for surface to be initialized
            var surfaceReady = false
            for (i in 0..49) {
                if (surface != null) {
                    surfaceReady = true
                    break
                }
                try {
                    Thread.sleep(20)
                } catch (e: Exception) {
                    // Ignore
                }
            }
            
            if (!surfaceReady) {
                Log.w("BelkaRx", "Surface not ready after 1 second wait")
            }

            startVsyncRenderLoop()
            recordingThread = Thread {
                processAudioViaAudioRecord(audioRecord!!, bufferSize)
            }
            recordingThread?.start()

            // Without UNPROCESSED support Android may hand over a processed (AGC-ed) signal,
            // which makes the noise floor rise whenever a strong signal stops.
            val audioManager = getSystemService(Context.AUDIO_SERVICE) as AudioManager
            val rawInputSupported =
                audioManager.getProperty(AudioManager.PROPERTY_SUPPORT_AUDIO_SOURCE_UNPROCESSED) == "true"
            Log.i("BelkaRx", "UNPROCESSED audio source supported: $rawInputSupported")
            binding.statusText.text = if (rawInputSupported) {
                "Status: AudioRecord Stereo UNPROCESSED @ ${sampleRate/1000}k"
            } else {
                "Status: AudioRecord Stereo @ ${sampleRate/1000}k (raw input not supported, AGC may apply)"
            }
            Log.i("BelkaRx", "AudioRecord started successfully with UNPROCESSED source at $sampleRate Hz")
            true
        } catch (e: Exception) {
            Log.e("BelkaRx", "AudioRecord UNPROCESSED failed: ${e.message}")
            audioRecord?.release()
            audioRecord = null
            false
        }
    }

    /**
     * Process audio data from AudioRecord using the optimized pattern from 
     * https://github.com/hardcodedjoy/android-lib-audioinput
     * Aggressively reads and processes stereo audio with maximum buffer size
     */
    private fun processAudioViaAudioRecord(recorder: AudioRecord, minBufferSize: Int) {
        Log.i("BelkaRx", "AudioRecord processing thread started (minBufferSize=$minBufferSize bytes)")
        
        // Small reads (1024 stereo frames, ~5 ms at 192 kHz) keep data flowing steadily instead of
        // blocking for a whole FFT window at a time.
        val readChunkShorts = READ_CHUNK_FRAMES * 2
        val shortBuf = ShortArray(readChunkShorts)
        Log.i("BelkaRx", "AudioRecord read chunk: $readChunkShorts shorts (${readChunkShorts * 2} bytes)")

        // Sliding FFT window with 50% overlap: a line is processed every HOP_SIZE frames.
        // Must match kHopSize in native-lib.cpp.
        val windowShorts = FFT_SIZE * 2
        val keepShorts = (FFT_SIZE - HOP_SIZE) * 2
        val outputBuf = ShortArray(windowShorts)
        var outputIdx = 0
        var read: Int
        var processCount = 0
        var readCount = 0
        
        // Statistics for detecting mono vs stereo
        var monoCount = 0
        var stereoCount = 0
        var lastStereoCheck = 0

        while (isRecording.get()) {
            try {
                read = recorder.read(shortBuf, 0, shortBuf.size)
                readCount++
                
                if (read == 0) {
                    // No data available yet - brief sleep
                    if (readCount % 100 == 0) {
                        Log.d("BelkaRx", "AudioRecord: no data (${readCount} reads)")
                    }
                    try {
                        Thread.sleep(0, 250)  // 0.25 ms sleep
                    } catch (e: Exception) {
                        // Ignore
                    }
                    continue
                }

                if (read < 0) {
                    Log.e("BelkaRx", "AudioRecord read error: $read")
                    break
                }

                if (readCount == 1) {
                    Log.i("BelkaRx", "First read from AudioRecord: $read shorts")
                }

                if (surface == null) {
                    Log.w("BelkaRx", "Surface is null, skipping processing")
                    continue
                }

                // Detect if we're getting stereo or mono data
                if (readCount - lastStereoCheck > 50) {
                    lastStereoCheck = readCount
                    // Check first 4 samples - if L != R consistently, it's stereo
                    var isDifferent = false
                    if (read >= 4) {
                        // Compare L and R channels
                        if (shortBuf[0] != shortBuf[1] || shortBuf[2] != shortBuf[3]) {
                            isDifferent = true
                            stereoCount++
                        } else {
                            monoCount++
                        }
                    }
                    if (readCount == 51) {
                        Log.i("BelkaRx", "Audio type check: isDifferent=$isDifferent, stereo=$stereoCount, mono=$monoCount")
                    }
                }

                // Copy data into output buffer for processing
                try {
                    var srcIdx = 0
                    while (srcIdx < read) {
                        val n = minOf(read - srcIdx, windowShorts - outputIdx)
                        System.arraycopy(shortBuf, srcIdx, outputBuf, outputIdx, n)
                        srcIdx += n
                        outputIdx += n

                        // When the window is full, process it and keep its second half for the next one.
                        if (outputIdx >= windowShorts) {
                            processCount++
                            if (processCount % 100 == 0) {
                                Log.d("BelkaRx", "Calling processAudioData (count=$processCount, stereo=$stereoCount, mono=$monoCount)")
                            }

                            processAudioData(outputBuf, outputBuf.size)
                            System.arraycopy(outputBuf, windowShorts - keepShorts, outputBuf, 0, keepShorts)
                            outputIdx = keepShorts
                        }
                    }
                } catch (e: Exception) {
                    Log.e("BelkaRx", "Audio processing error: ${e.message}")
                    e.printStackTrace()
                    break
                }
            } catch (e: Exception) {
                Log.e("BelkaRx", "AudioRecord read exception: ${e.message}")
                e.printStackTrace()
                break
            }
        }

        try {
            recorder.stop()
        } catch (e: Exception) {
            Log.w("BelkaRx", "Error stopping recorder: ${e.message}")
        }
        
        Log.i("BelkaRx", "AudioRecord processing thread stopped (reads=$readCount, processes=$processCount, stereo=$stereoCount, mono=$monoCount)")
    }



    override fun onPause() {
        super.onPause()
        saveSettings()
    }

    private fun updateDemodModeButtonText() {
        binding.demodModeButton.text = demodModeLabels[demodModeIndex]
    }

    private fun saveSettings() {
        val editor = prefs.edit()
        editor.putInt("sensitivity", binding.sensitivitySeekBar.value.toInt())
        editor.putInt("contrast", binding.contrastSeekBar.value.toInt())
        editor.putBoolean("swapIQ", binding.swapIQToggle.isChecked)
        editor.putBoolean("zoom", binding.zoomToggle.isChecked)
        editor.putBoolean("showSpectrum", binding.showSpectrumToggle.isChecked)
        editor.putBoolean("marker", binding.markerToggle.isChecked)
        editor.putBoolean("cw", binding.cwToggle.isChecked)
        editor.putBoolean("rtty", binding.rttyToggle.isChecked)
        editor.putInt("rttyBaud", rttyBaudIndex)
        editor.putInt("rttyShift", rttyShiftIndex)
        editor.putBoolean("rttyInvert", binding.rttyInvertToggle.isChecked)
        editor.putInt("demodMode", demodModeIndex)
        editor.putBoolean("spectrumFilled", binding.spectrumFilledToggle.isChecked)
        editor.putBoolean("spectrumConstantColor", binding.spectrumConstantColorToggle.isChecked)
        editor.putInt("colorScale", colorScaleIndex)
        editor.remove("deviceSelection")  // left over from the removed device list
        editor.apply()
        Log.d("BelkaRx", "Settings saved")
    }

    private fun loadSettings() {
        val sensitivity = prefs.getInt("sensitivity", 100)
        val contrast = prefs.getInt("contrast", 100)
        val swapIQ = prefs.getBoolean("swapIQ", false)
        val zoom = prefs.getBoolean("zoom", false)
        val showSpectrum = prefs.getBoolean("showSpectrum", false)
        val marker = prefs.getBoolean("marker", false)
        val cw = prefs.getBoolean("cw", false)
        // Read before any toggle below is set: their listeners save the settings.
        val rtty = prefs.getBoolean("rtty", false)
        val rttyBaud = prefs.getInt("rttyBaud", DEFAULT_RTTY_BAUD_INDEX)
        val rttyShift = prefs.getInt("rttyShift", DEFAULT_RTTY_SHIFT_INDEX)
        val rttyInvert = prefs.getBoolean("rttyInvert", false)
        val demodMode = prefs.getInt("demodMode", 0)
        val spectrumFilled = prefs.getBoolean("spectrumFilled", false)
        val spectrumConstantColor = prefs.getBoolean("spectrumConstantColor", false)
        val colorScale = prefs.getInt("colorScale", 0)
        
        binding.sensitivitySeekBar.value = sensitivity.toFloat()
        binding.contrastSeekBar.value = contrast.toFloat()
        binding.swapIQToggle.isChecked = swapIQ
        binding.zoomToggle.isChecked = zoom
        binding.showSpectrumToggle.isChecked = showSpectrum
        binding.markerToggle.isChecked = marker
        binding.cwToggle.isChecked = cw && marker
        rttyBaudIndex = rttyBaud.coerceIn(0, RTTY_BAUDS.lastIndex)
        rttyShiftIndex = rttyShift.coerceIn(0, RTTY_SHIFTS.lastIndex)
        binding.rttyInvertToggle.isChecked = rttyInvert
        applyRttyParams()
        binding.rttyToggle.isChecked = rtty && marker && !cw
        binding.cwScope.thresholdDb = prefs.getFloat("cwThresholdDb", CwScopeView.DEFAULT_THRESHOLD_DB)
        setCwThresholdDb(binding.cwScope.thresholdDb)
        binding.cwScope.autoMode = prefs.getBoolean("cwThresholdAuto", true)
        setCwAutoThreshold(binding.cwScope.autoMode)
        demodModeIndex = demodMode.coerceIn(0, demodModeLabels.lastIndex)
        updateDemodModeButtonText()
        binding.spectrumFilledToggle.isChecked = spectrumFilled
        binding.spectrumConstantColorToggle.isChecked = spectrumConstantColor
        selectColorScale(colorScale, fromUser = false)


        Log.d("BelkaRx", "Settings loaded: sensitivity=$sensitivity, contrast=$contrast, swapIQ=$swapIQ, zoom=$zoom, colorScale=$colorScale")
    }

    private fun stopRecording() {
        isRecording.set(false)
        stopVsyncRenderLoop()
        recordingThread?.join()
        try {
            audioRecord?.stop()
        } catch (e: Exception) {}
        audioRecord?.release()
        audioRecord = null
        // stopService(Intent(this, SdrService::class.java))
        binding.startStopButton.isChecked = false
        binding.statusText.text = "Status: Idle"
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        holder.setFormat(PixelFormat.RGBA_8888)
        surface = holder.surface
        // Pass surface to native renderer.
        val s = surface
        if (s != null) {
            setNativeSurface(s)
            if (isRecording.get() && audioRecord != null) {
                startVsyncRenderLoop()
            }
        }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        surface = holder.surface
        val s = surface
        if (s != null) {
            setNativeSurface(s)
            if (isRecording.get() && audioRecord != null) {
                startVsyncRenderLoop()
            }
        }
        setSurfaceSize(width, height)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        surface = null
        stopVsyncRenderLoop()
        // The EGL surface lives on the render thread; it must be gone before this returns.
        runOnRenderThreadAndWait { releaseRenderSurface() }
        setNativeSurface(null)
    }

    private fun runOnRenderThreadAndWait(block: () -> Unit) {
        val handler = renderHandler ?: return
        val done = CountDownLatch(1)
        if (handler.post { try { block() } finally { done.countDown() } }) {
            done.await(500, TimeUnit.MILLISECONDS)
        }
    }

    private external fun processAudioData(data: ShortArray, size: Int)
    private external fun renderFrame(frameTimeNanos: Long)
    private external fun releaseRenderSurface()
    private external fun releaseRenderer()
    private external fun setCwEnabled(enabled: Boolean)
    private external fun setOverlayBottomInset(pixels: Int)
    private external fun snapMarkerToPeak(): Boolean
    private external fun cwPollText(): String
    private external fun cwGetStatus(out: FloatArray)
    private external fun setCwThresholdDb(db: Float)
    private external fun setCwAutoThreshold(enabled: Boolean)
    private external fun getColorScalePreview(scale: Int, out: IntArray)
    private external fun cwGetScope(out: FloatArray, total: LongArray): Int
    private external fun setRttyEnabled(enabled: Boolean)
    private external fun setRttyParams(baud: Float, shiftHz: Float, invert: Boolean)
    private external fun rttyPollText(): String
    private external fun rttyGetStatus(out: FloatArray)
    private external fun rttyGetScope(out: FloatArray): Int
    private external fun setSurfaceSize(width: Int, height: Int)
    private external fun setSensitivity(value: Int)
    private external fun setContrast(value: Int)
    private external fun setNativeSampleRate(rate: Int)
    private external fun setSwapIQ(swap: Boolean)
    private external fun setZoom(enabled: Boolean)
    private external fun setZoomFromTouch(enabled: Boolean, touchX: Float)
    private external fun setFixedWindowEnabled(enabled: Boolean)
    private external fun setShowSpectrum(enabled: Boolean)
    private external fun setAdjustableMarkerEnabled(enabled: Boolean)
    private external fun setAdjustableMarkerTouchX(touchX: Float)
    private external fun setDemodMode(mode: Int)
    private external fun setColorScale(scale: Int)
    private external fun setSpectrumFilled(filled: Boolean)
    private external fun setSpectrumConstantColor(constant: Boolean)
    private external fun setNativeSurface(surface: Surface?)

    private inner class GestureListener : GestureDetector.SimpleOnGestureListener() {
        override fun onDoubleTap(e: MotionEvent): Boolean {
            val newZoomState = !binding.zoomToggle.isChecked
            setZoomFromTouch(newZoomState, e.x)

            suppressZoomToggleCallback = true
            binding.zoomToggle.isChecked = newZoomState
            suppressZoomToggleCallback = false

            saveSettings()
            Log.d("BelkaRx", "Surface double-tap: toggled Zoom to $newZoomState at x=${e.x}")
            return true
        }
    }

    companion object {
        private const val FFT_SIZE = 4096          // stereo frames per FFT window
        private const val HOP_SIZE = 2048          // 50% overlap; must match kHopSize in native-lib.cpp
        private const val READ_CHUNK_FRAMES = 1024 // stereo frames per AudioRecord.read()

        private val COLOR_SCALE_NAMES = arrayOf(
            "Classic Rainbow", "Light Blue", "Grayscale", "Cool-Hot", "Green Phosphor", "LCD",
        )
        private const val CW_POLL_INTERVAL_MS = 100L  // text and status; the graph pulls its own data
        private const val DECODER_NEW_LINE_PAUSE_MS = 10_000L  // a pause this long starts a new log line
        private const val DECODER_DISPLAY_CHARS = 10_000  // kept in the strip to scroll back through
        private const val CW_COLOR_KEY_DOWN = 0xFF4CE06A.toInt()
        private const val CW_COLOR_SIGNAL = 0xFF2E6B3A.toInt()
        private const val CW_COLOR_IDLE = 0xFF555555.toInt()

        // RTTY: 45.45 Bd (amateur), 50 Bd (DWD weather and most utility stations), 75 Bd.
        private val RTTY_BAUDS = floatArrayOf(45.45f, 50f, 75f)
        private val RTTY_BAUD_LABELS = arrayOf("45", "50", "75")
        private const val DEFAULT_RTTY_BAUD_INDEX = 1
        // Shifts in Hz: 85 (narrow utility), 170 (amateur), 425 / 450 (DWD), 850 (wide utility).
        private val RTTY_SHIFTS = intArrayOf(85, 170, 425, 450, 850)
        private const val DEFAULT_RTTY_SHIFT_INDEX = 3

        init {
            System.loadLibrary("belkarx")
        }
    }
}
