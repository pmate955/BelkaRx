package com.example.belkarx

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.PixelFormat
import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioRecord
import android.media.MediaRecorder
import android.media.audiofx.AcousticEchoCanceler
import android.media.audiofx.AutomaticGainControl
import android.media.audiofx.NoiseSuppressor
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.view.Surface
import android.view.SurfaceHolder
import android.widget.ArrayAdapter
import android.widget.SeekBar
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import com.example.belkarx.databinding.ActivityMainBinding
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.abs

class MainActivity : AppCompatActivity(), SurfaceHolder.Callback {

    private lateinit var binding: ActivityMainBinding
    private var audioRecord: AudioRecord? = null
    private val isRecording = AtomicBoolean(false)
    private var recordingThread: Thread? = null
    private var surface: Surface? = null

    // SDR preferences
    private var sampleRate = 96000
    private val channelConfig = AudioFormat.CHANNEL_IN_STEREO
    private val audioFormat = AudioFormat.ENCODING_PCM_16BIT
    private var bufferSize = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        binding.waterfallSurface.holder.addCallback(this)
        setupDeviceSpinner()

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
            }
            override fun onStartTrackingTouch(seekBar: SeekBar?) {}
            override fun onStopTrackingTouch(seekBar: SeekBar?) {}
        })

        binding.swapIQCheckBox.setOnCheckedChangeListener { _, isChecked ->
            setSwapIQ(isChecked)
            Log.d("BelkaRx", "Swap I/Q checkbox changed: $isChecked")
        }

        binding.monoModeCheckBox.setOnCheckedChangeListener { _, isChecked ->
            setMonoMode(isChecked)
            Log.d("BelkaRx", "Mono Mode checkbox changed: $isChecked")
        }

        binding.stereoAverageModeCheckBox.setOnCheckedChangeListener { _, isChecked ->
            setStereoAverageMode(isChecked)
            Log.d("BelkaRx", "Stereo Average Mode checkbox changed: $isChecked")
        }

        setSensitivity(binding.sensitivitySeekBar.progress)
        setSwapIQ(binding.swapIQCheckBox.isChecked)
        setMonoMode(binding.monoModeCheckBox.isChecked)
        setStereoAverageMode(binding.stereoAverageModeCheckBox.isChecked)
        Log.d("BelkaRx", "Initial UI setup: Swap I/Q=${binding.swapIQCheckBox.isChecked}, Mono Mode=${binding.monoModeCheckBox.isChecked}")


    }

    private fun setupDeviceSpinner() {
        val audioManager = getSystemService(Context.AUDIO_SERVICE) as AudioManager
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
        
        Log.i("BelkaRx", "Found ${devices.size} audio input devices")
        devices.forEachIndexed { index, device ->
            Log.i("BelkaRx", "Device $index: ${device.productName} (type=${device.type}, id=${device.id})")
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
        val audioManager = getSystemService(Context.AUDIO_SERVICE) as AudioManager
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

    private fun initAudioRecord(device: AudioDeviceInfo, rate: Int): Boolean {
        // DEPRECATED: This function is no longer used. The native Oboe capture is used instead.
        Log.w("BelkaRx", "initAudioRecord() called but should not be used!")
        return false
    }

    private fun disableAudioEffects(sessionId: Int) {
        try {
            if (NoiseSuppressor.isAvailable()) {
                NoiseSuppressor.create(sessionId)?.enabled = false
            }
            if (AcousticEchoCanceler.isAvailable()) {
                AcousticEchoCanceler.create(sessionId)?.enabled = false
            }
            if (AutomaticGainControl.isAvailable()) {
                AutomaticGainControl.create(sessionId)?.enabled = false
            }
        } catch (e: Exception) {}
    }

    private fun getSourceName(source: Int): String {
        return when(source) {
            MediaRecorder.AudioSource.CAMCORDER -> "CAM"
            MediaRecorder.AudioSource.VOICE_RECOGNITION -> "VR"
            MediaRecorder.AudioSource.UNPROCESSED -> "RAW"
            MediaRecorder.AudioSource.MIC -> "MIC"
            10 -> "PERF"
            else -> "S$source"
        }
    }

    /**
     * DEPRECATED: This function is no longer used. The native Oboe capture is used instead.
     */
    private fun tryAudioRecordSources(device: AudioDeviceInfo, rate: Int): Boolean {
        Log.w("BelkaRx", "tryAudioRecordSources() called but should not be used!")
        return false
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

    private fun processAudio() {
        // The native Oboe capture thread handles audio reading and calls processAndDraw directly.
        // This thread just keeps the recording thread alive and responsive.
        Log.i("BelkaRx", "processAudio() thread started - native Oboe handles reading")
        
        while (isRecording.get()) {
            try {
                Thread.sleep(100)
            } catch (e: InterruptedException) {
                break
            }
        }
        
        Log.i("BelkaRx", "processAudio() thread stopped")
    }

    /**
     * AAudio-based audio processing thread.
     * Reads stereo frames from AAudio and processes via native code.
     */
    private fun processAudioViaAAudio() {
        val fftSize = 1024
        val buffer = ShortArray(fftSize * 2)
        
        while (isRecording.get()) {
            // In a real implementation, you would call a native JNI method to read
            // from AAudio (since AAudio is C/C++ only). For now, this is a placeholder
            // that demonstrates the intended flow. The actual audio reading happens
            // in native code on a background thread spawned from JNI.
            
            // TODO: Implement native audio reading thread that calls back to processAndDraw
            // For demonstration: sleep briefly to prevent CPU spinning
            Thread.sleep(10)
        }
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
    private external fun setNativeSampleRate(rate: Int)
    private external fun setSwapIQ(swap: Boolean)
    private external fun setMonoMode(mono: Boolean)
    private external fun setStereoAverageMode(stereo: Boolean)

    // Oboe native methods
    private external fun startOboeCapture(deviceId: Int, sampleRate: Int): Boolean
    private external fun stopOboeCapture()
    private external fun isOboeRunning(): Boolean
    private external fun getOboeChannels(): Int
    private external fun getOboeSampleRate(): Int
    private external fun setOboeSurface(surface: Surface)

    companion object {
        init {
            System.loadLibrary("belkarx")
        }
    }
}
