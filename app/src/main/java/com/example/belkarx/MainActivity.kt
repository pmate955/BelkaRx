package com.example.belkarx

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
import android.content.res.Configuration
import android.util.Log
import android.view.GestureDetector
import android.view.MotionEvent
import android.view.Surface
import android.view.SurfaceHolder
import android.view.View
import android.view.WindowManager
import android.widget.LinearLayout
import android.view.ViewGroup
import android.widget.ArrayAdapter
import android.widget.SeekBar
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import com.example.belkarx.databinding.ActivityMainBinding
import java.util.concurrent.atomic.AtomicBoolean

class MainActivity : AppCompatActivity(), SurfaceHolder.Callback {

    private lateinit var binding: ActivityMainBinding
    private var audioRecord: AudioRecord? = null
    private val isRecording = AtomicBoolean(false)
    private var recordingThread: Thread? = null
    private var surface: Surface? = null
    private lateinit var gestureDetector: GestureDetector
    private lateinit var prefs: android.content.SharedPreferences

    override fun onCreate(savedInstanceState: Bundle?) {
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
            true
        }
        
        setupDeviceSpinner()
        setupColorScaleSpinner()

        binding.toggleControlsButton.setOnClickListener {
            val controlsContainer = binding.controlsContainer
            if (controlsContainer.visibility == View.VISIBLE) {
                controlsContainer.visibility = View.GONE
                binding.toggleControlsButton.text = "▼"
            } else {
                controlsContainer.visibility = View.VISIBLE
                binding.toggleControlsButton.text = "▲"
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

        binding.sensitivitySeekBar.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                setSensitivity(progress)
                if (fromUser) saveSettings()
            }
            override fun onStartTrackingTouch(seekBar: SeekBar?) {}
            override fun onStopTrackingTouch(seekBar: SeekBar?) {}
        })

        binding.contrastSeekBar.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                setContrast(progress)
                if (fromUser) saveSettings()
            }
            override fun onStartTrackingTouch(seekBar: SeekBar?) {}
            override fun onStopTrackingTouch(seekBar: SeekBar?) {}
        })

        binding.swapIQToggle.setOnCheckedChangeListener { _, isChecked ->
            setSwapIQ(isChecked)
            saveSettings()
            Log.d("BelkaRx", "Swap I/Q checkbox changed: $isChecked")
        }

        binding.zoomToggle.setOnCheckedChangeListener { _, isChecked ->
            setZoom(isChecked)
            saveSettings()
            Log.d("BelkaRx", "Zoom checkbox changed: $isChecked")
        }

        binding.fastWaterfallToggle.setOnCheckedChangeListener { _, isChecked ->
            setFastWaterfall(isChecked)
            saveSettings()
            Log.d("BelkaRx", "Fast waterfall checkbox changed: $isChecked")
        }

        binding.showSpectrumToggle.setOnCheckedChangeListener { _, isChecked ->
            setShowSpectrum(isChecked)
            saveSettings()
            Log.d("BelkaRx", "Show spectrum checkbox changed: $isChecked")
        }

        binding.colorScaleSpinner.onItemSelectedListener = object : android.widget.AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: android.widget.AdapterView<*>?, view: android.view.View?, position: Int, id: Long) {
                setColorScale(position)
                saveSettings()
                Log.d("BelkaRx", "Color scale selected: $position")
            }
            override fun onNothingSelected(parent: android.widget.AdapterView<*>?) {}
        }

        // Load saved settings
        loadSettings()

        setSensitivity(binding.sensitivitySeekBar.progress)
        setContrast(binding.contrastSeekBar.progress)
        setSwapIQ(binding.swapIQToggle.isChecked)
        setZoom(binding.zoomToggle.isChecked)
        setFastWaterfall(binding.fastWaterfallToggle.isChecked)
        setShowSpectrum(binding.showSpectrumToggle.isChecked)
        setColorScale(binding.colorScaleSpinner.selectedItemPosition)
        Log.d("BelkaRx", "Initial UI setup: Swap I/Q=${binding.swapIQToggle.isChecked}")


    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        updateFullscreenMode(newConfig.orientation)
    }

    private fun updateFullscreenMode(orientation: Int) {
        if (orientation == Configuration.ORIENTATION_LANDSCAPE) {
            // Hide the status bar, navigation bar, and set immersive sticky mode in landscape
            window.decorView.systemUiVisibility = (
                View.SYSTEM_UI_FLAG_FULLSCREEN or
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
            )
            supportActionBar?.hide()
        } else {
            // Show system UI elements in portrait
            window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_VISIBLE
            supportActionBar?.hide() // Keep action bar hidden to save space even in portrait
        }
        
        updateLayoutForOrientation(orientation)
    }

    private fun updateLayoutForOrientation(orientation: Int) {
        val topContainer = binding.topBarCenterContainer
        val dropdownContainer = binding.dropdownDynamicArea
        
        // Remove views from their current parents safely
        val viewsToMove = listOf(binding.colorScaleSpinner, binding.sensitivityView, binding.contrastView)
        for (v in viewsToMove) {
            (v.parent as? ViewGroup)?.removeView(v)
        }
        
        // Helper function for layout params
        fun getDropParams() = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            setMargins(0, 16, 0, 0)
        }
        
        fun getTopParams(marginStart: Int = 0) = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply {
            setMargins(marginStart, 0, 0, 0)
        }
        
        if (orientation == Configuration.ORIENTATION_LANDSCAPE) {
            // In Landscape: Sensitivity & Contrast on top, Color Scale in dropdown
            binding.sensitivityView.layoutParams = getTopParams()
            binding.contrastView.layoutParams = getTopParams(16)
            topContainer.addView(binding.sensitivityView)
            topContainer.addView(binding.contrastView)
            
            binding.colorScaleSpinner.layoutParams = getDropParams()
            dropdownContainer.addView(binding.colorScaleSpinner)
        } else {
            // In Portrait: Sensitivity on top, Color Scale & Contrast in dropdown
            binding.sensitivityView.layoutParams = getTopParams()
            topContainer.addView(binding.sensitivityView)
            
            binding.colorScaleSpinner.layoutParams = getDropParams()
            binding.contrastView.layoutParams = getDropParams()
            dropdownContainer.addView(binding.colorScaleSpinner)
            dropdownContainer.addView(binding.contrastView)
        }
    }

    private fun setupDeviceSpinner() {
        val audioManager = getSystemService(AUDIO_SERVICE) as AudioManager
        val devices = audioManager.getDevices(AudioManager.GET_DEVICES_INPUTS)
        
        // Show all input devices (native Oboe/AAudio will request unprocessed audio)
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
        val colorScales = arrayOf("Classic Rainbow", "Light Blue", "Grayscale", "Cool-Hot")
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
        
        // Try AudioRecord with UNPROCESSED source first
        try {
            if (!tryAudioRecordUnprocessed(selectedDevice)) {
                binding.statusText.text = "Status: Error - Failed to start AudioRecord UNPROCESSED"
                Log.e("BelkaRx", "Failed to start AudioRecord UNPROCESSED for device ${selectedDevice.id}")
                
                // Fallback to Oboe
                Log.i("BelkaRx", "Trying Oboe as fallback")
                val sampleRate = 192000
                setNativeSampleRate(sampleRate)
                val success = startOboeCapture(selectedDevice.id, sampleRate)
                if (!success) {
                    binding.statusText.text = "Status: Error - No audio source available"
                }
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

            Log.i("BelkaRx", "Attempting AudioRecord with UNPROCESSED source: bufferSize=$bufferSize")

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
                        .setBufferSizeInBytes(bufferSize * 2)

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
                        bufferSize * 2
                    )
                }
            } else {
                AudioRecord(
                    MediaRecorder.AudioSource.UNPROCESSED,
                    sampleRate,
                    channelConfig,
                    audioFormat,
                    bufferSize * 2
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
            binding.startStopButton.text = "Stop"

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
        Log.i("BelkaRx", "AudioRecord processing thread started (minBufferSize=$minBufferSize)")
        
        // Use aggressive buffer multiplier like the reference library (256x)
        var bufferSize = minBufferSize
        var buf: ShortArray? = null
        var attempts = 0
        
        // Try to allocate the largest buffer possible (up to 256x minBufferSize)
        while (buf == null && attempts < 9) {
            try {
                buf = ShortArray(bufferSize)
                Log.i("BelkaRx", "AudioRecord buffer allocated: $bufferSize shorts (${bufferSize * 2} bytes)")
            } catch (e: Exception) {
                Log.w("BelkaRx", "Failed to allocate buffer of size $bufferSize: ${e.message}")
                bufferSize /= 2
                attempts++
            }
        }
        
        if (buf == null) {
            Log.e("BelkaRx", "Could not allocate audio buffer")
            return
        }

        val shortBuf = buf
        val fftSize = 2048
        val outputBuf = ShortArray(fftSize * 2)  // Stereo FFT buffer
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
                    for (i in 0 until read) {
                        outputBuf[outputIdx] = shortBuf[i]
                        outputIdx++
                        
                        // When we have enough samples for FFT, process them
                        if (outputIdx >= fftSize * 2) {
                            outputIdx = 0
                            processCount++
                            if (processCount % 10 == 0) {
                                Log.d("BelkaRx", "Calling processAndDraw (count=$processCount, bufSize=${fftSize*2}, stereo=$stereoCount, mono=$monoCount)")
                            }
                            processAndDraw(outputBuf, fftSize * 2, surface!!)
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

    private fun saveSettings() {
        val editor = prefs.edit()
        editor.putInt("sensitivity", binding.sensitivitySeekBar.progress)
        editor.putInt("contrast", binding.contrastSeekBar.progress)
        editor.putBoolean("swapIQ", binding.swapIQToggle.isChecked)
        editor.putBoolean("zoom", binding.zoomToggle.isChecked)
        editor.putBoolean("fastWaterfall", binding.fastWaterfallToggle.isChecked)
        editor.putBoolean("showSpectrum", binding.showSpectrumToggle.isChecked)
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
        val fastWaterfall = prefs.getBoolean("fastWaterfall", false)
        val showSpectrum = prefs.getBoolean("showSpectrum", false)
        val colorScale = prefs.getInt("colorScale", 0)
        val deviceSelection = prefs.getInt("deviceSelection", 0)
        
        binding.sensitivitySeekBar.progress = sensitivity
        binding.contrastSeekBar.progress = contrast
        binding.swapIQToggle.isChecked = swapIQ
        binding.zoomToggle.isChecked = zoom
        binding.fastWaterfallToggle.isChecked = fastWaterfall
        binding.showSpectrumToggle.isChecked = showSpectrum
        binding.colorScaleSpinner.setSelection(colorScale)
        
        // Set device selection if it's valid
        if (deviceSelection >= 0 && deviceSelection < binding.deviceSpinner.count) {
            binding.deviceSpinner.setSelection(deviceSelection)
        }
        
        Log.d("BelkaRx", "Settings loaded: sensitivity=$sensitivity, contrast=$contrast, swapIQ=$swapIQ, zoom=$zoom, colorScale=$colorScale")
    }

    private fun stopRecording() {
        isRecording.set(false)
        recordingThread?.join()
        stopOboeCapture()
        try {
            audioRecord?.stop()
        } catch (e: Exception) {}
        audioRecord?.release()
        audioRecord = null
        // stopService(Intent(this, SdrService::class.java))
        binding.startStopButton.text = "Start"
        binding.statusText.text = "Status: Idle"
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        holder.setFormat(PixelFormat.RGBA_8888)
        surface = holder.surface
        // Pass surface to native code for Oboe rendering
        val s = surface
        if (s != null) {
            setOboeSurface(s)
        }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        surface = holder.surface
        // Update surface reference in native code
        val s = surface
        if (s != null) {
            setOboeSurface(s)
        }
        setSurfaceSize(width, height)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        surface = null
    }

    private external fun processAndDraw(data: ShortArray, size: Int, surface: Surface)
    private external fun setSurfaceSize(width: Int, height: Int)
    private external fun setSensitivity(value: Int)
    private external fun setContrast(value: Int)
    private external fun setNativeSampleRate(rate: Int)
    private external fun setSwapIQ(swap: Boolean)
    private external fun setZoom(enabled: Boolean)
    private external fun setFastWaterfall(enabled: Boolean)
    private external fun setShowSpectrum(enabled: Boolean)
    private external fun setColorScale(scale: Int)

    // Oboe native methods
    private external fun startOboeCapture(deviceId: Int, sampleRate: Int): Boolean
    private external fun stopOboeCapture()
    private external fun setOboeSurface(surface: Surface)

    private inner class GestureListener : GestureDetector.SimpleOnGestureListener() {
        override fun onDoubleTap(e: MotionEvent): Boolean {
            // Toggle zoom on double-tap
            val newZoomState = !binding.zoomToggle.isChecked
            binding.zoomToggle.isChecked = newZoomState
            Log.d("BelkaRx", "Surface double-tap: toggled Zoom to $newZoomState")
            return true
        }
    }

    companion object {
        init {
            System.loadLibrary("belkarx")
        }
    }
}
