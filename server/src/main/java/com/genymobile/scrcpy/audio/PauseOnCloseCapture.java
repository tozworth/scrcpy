// Modified by appcpy: pause media before releasing the phone's audio redirection.
package com.genymobile.scrcpy.audio;

import com.genymobile.scrcpy.control.PlaybackPause;

import android.media.MediaCodec;

import java.nio.ByteBuffer;

public final class PauseOnCloseCapture implements AudioCapture {
    private final AudioCapture capture;
    private final PlaybackPause playbackPause;
    private boolean started;

    public PauseOnCloseCapture(AudioCapture capture, PlaybackPause playbackPause) {
        this.capture = capture;
        this.playbackPause = playbackPause;
    }

    @Override
    public void checkCompatibility() throws AudioCaptureException {
        capture.checkCompatibility();
    }

    @Override
    public void start() throws AudioCaptureException {
        capture.start();
        started = true;
    }

    @Override
    public void stop() {
        try {
            if (started) {
                playbackPause.pause();
            }
        } finally {
            capture.stop();
        }
    }

    @Override
    public int read(ByteBuffer buffer, MediaCodec.BufferInfo info) {
        return capture.read(buffer, info);
    }
}
