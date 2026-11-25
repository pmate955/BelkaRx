package hu.ha8mz.belkarx

import android.Manifest
import android.content.pm.PackageManager
import android.content.Context
import android.graphics.PixelFormat
import android.media.AudioDeviceInfo
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
import android.view.MotionEvent
import android.view.Surface
import android.view.SurfaceHolder
import android.view.View
import android.view.ViewConfiguration
import android.view.WindowManager
import android.widget.LinearLayout
import android.view.ViewGroup
import android.widget.ArrayAdapter
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.app.AppCompatDelegate
import androidx.core.app.ActivityCompat
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
    private val markerLongPressMoveToleranceMultiplier = 3f
    private val markerLongPressRunnable = Runnable {
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
        
        setupDeviceSpinner()
        setupColorScaleSpinner()

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
            saveSettings()
            Log.d("BelkaRx", "Marker checkbox changed: $isChecked")
        }

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

        binding.colorScaleSpinner.onItemSelectedListener = object : android.widget.AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: android.widget.AdapterView<*>?, view: android.view.View?, position: Int, id: Long) {
                setColorScale(position)             
                saveSettings()
                Log.d("BelkaRx", "Color scale selected: $position, isSpectrumFilled=${position == 4}")
            }
            override fun onNothingSelected(parent: android.widget.AdapterView<*>?) {}
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
        setColorScale(binding.colorScaleSpinner.selectedItemPosition)
        updateSpectrumOptionControlsEnabled(binding.showSpectrumToggle.isChecked)
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

    private fun setupDeviceSpinner() {
        val audioManager = getSystemService(AUDIO_SERVICE) as AudioManager
        val devices = audioManager.getDevices(AudioManager.GET_DEVICES_INPUTS)
        
        // Show all input devices (AudioRecord UNPROCESSED path is used for capture)
        val deviceNames = devices.map { device ->
            val type = when (device.type) {
                AudioDeviceInfo.TYPE_USB_DEVICE -> "USB Device"
                AudioDeviceInfo.TYPE_USB_HEADSET -> "USB Headset"
                AudioDeviceInfo.TYPE_BUILTIN_MIC -> "Built-in Mic"
                else -> "Device (${device.type})"
            }
            "${device.productName} [$type]"
        }
        
        val adapter = ArrayAdapter(this, android.R.layout.simple_spinner_item, deviceNames)
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        binding.deviceSpinner.adapter = adapter
        
        // Set default selection to "Other (25)" device if available
        val otherIndex = devices.indexOfFirst { it.type == 25 }
        if (otherIndex >= 0) {
            binding.deviceSpinner.setSelection(otherIndex)
        }
        
        Log.i("BelkaRx", "Found ${devices.size} audio input devices")
        devices.forEachIndexed { index, device ->
            Log.i("BelkaRx", "Device $index: ${device.productName} (type=${device.type}, id=${device.id})")
        }
    }

    private fun setupColorScaleSpinner() {
        val colorScales = arrayOf(
            "Classic Rainbow",
            "Light Blue",
            "Grayscale",
            "Cool-Hot",
            "Green Phosphor",
            "LCD"
        )
        val adapter = ArrayAdapter(this, android.R.layout.simple_spinner_item, colorScales)
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        binding.colorScaleSpinner.adapter = adapter
        binding.colorScaleSpinner.setSelection(0)  // Default to Classic Rainbow
    }

    private fun checkPermission(): Boolean {
        return ActivityCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED
    }

    private fun requestPermission() {
        ActivityCompat.requestPermissions(this, arrayOf(Manifest.permission.RECORD_AUDIO), 1001)
    }

    private fun startRecording() {
        Log.i("BelkaRx", "startRecording() called")
        val audioManager = getSystemService(AUDIO_SERVICE) as AudioManager
        val devices = audioManager.getDevices(AudioManager.GET_DEVICES_INPUTS)
        
        val selectedDeviceIndex = binding.deviceSpinner.selectedItemPosition
        
        if (selectedDeviceIndex < 0 || selectedDeviceIndex >= devices.size) {
            Toast.makeText(this, "Nincs kiválasztott eszköz", Toast.LENGTH_SHORT).show()
            return
        }
        
        val selectedDevice = devices[selectedDeviceIndex]

        if (!checkPermission()) return

        binding.statusText.text = "Status: Starting AudioRecord UNPROCESSED capture..."
        
        try {
            if (!tryAudioRecordUnprocessed(selectedDevice)) {
                binding.statusText.text = "Status: Error - Failed to start AudioRecord UNPROCESSED"
                Log.e("BelkaRx", "Failed to start AudioRecord UNPROCESSED for device ${selectedDevice.id}")
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
    private fun tryAudioRecordUnprocessed(device: AudioDeviceInfo): Boolean {
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

            binding.statusText.text = "Status: AudioRecord Stereo UNPROCESSED @ ${sampleRate/1000}k"
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
        editor.putInt("demodMode", demodModeIndex)
        editor.putBoolean("spectrumFilled", binding.spectrumFilledToggle.isChecked)
        editor.putBoolean("spectrumConstantColor", binding.spectrumConstantColorToggle.isChecked)
        editor.putInt("colorScale", binding.colorScaleSpinner.selectedItemPosition)
        editor.putInt("deviceSelection", binding.deviceSpinner.selectedItemPosition)
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
        val demodMode = prefs.getInt("demodMode", 0)
        val spectrumFilled = prefs.getBoolean("spectrumFilled", false)
        val spectrumConstantColor = prefs.getBoolean("spectrumConstantColor", false)
        val colorScale = prefs.getInt("colorScale", 0)
        val deviceSelection = prefs.getInt("deviceSelection", 0)
        
        binding.sensitivitySeekBar.value = sensitivity.toFloat()
        binding.contrastSeekBar.value = contrast.toFloat()
        binding.swapIQToggle.isChecked = swapIQ
        binding.zoomToggle.isChecked = zoom
        binding.showSpectrumToggle.isChecked = showSpectrum
        binding.markerToggle.isChecked = marker
        demodModeIndex = demodMode.coerceIn(0, demodModeLabels.lastIndex)
        updateDemodModeButtonText()
        binding.spectrumFilledToggle.isChecked = spectrumFilled
        binding.spectrumConstantColorToggle.isChecked = spectrumConstantColor
        binding.colorScaleSpinner.setSelection(colorScale.coerceIn(0, binding.colorScaleSpinner.count - 1))
        
        // Set device selection if it's valid
        if (deviceSelection >= 0 && deviceSelection < binding.deviceSpinner.count) {
            binding.deviceSpinner.setSelection(deviceSelection)
        }
        
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

        init {
            System.loadLibrary("belkarx")
        }
    }
}
