// Modified by appcpy: pausing video while the client window is minimized.
package com.genymobile.scrcpy.video;

import com.genymobile.scrcpy.util.Ln;

import android.media.MediaCodec;
import android.os.Bundle;
import android.os.SystemClock;

public class CaptureControl {

    // Allow the encoder to drain in-flight output and to produce a requested key frame.
    private static final long CODEC_TRANSITION_TIMEOUT_MS = 250;

    public static final int RESET_REASON_TERMINATED = 1;
    public static final int RESET_REASON_DISPLAY_PROPERTIES_CHANGED = 1 << 1;
    public static final int RESET_REASON_CLIENT_RESET = 1 << 2;
    public static final int RESET_REASON_CLIENT_RESIZED = 1 << 3;
    // appcpy: interrupt the running encoder to pause the video
    public static final int RESET_REASON_PAUSED = 1 << 4;

    private final boolean canSuspendInput;

    private int reset = 0;
    private boolean paused;
    private boolean freezeWhilePaused;
    private long pausedAt;

    public enum PauseMode {
        RUNNING,
        VIDEO_PAUSED,
        DISPLAY_FROZEN,
    }

    // Current instance of MediaCodec to "interrupt" on reset
    private MediaCodec runningMediaCodec;
    private boolean inputSuspended;
    private boolean waitForKeyFrame;
    private long keyFrameDeadline;

    public CaptureControl(boolean canSuspendInput) {
        this.canSuspendInput = canSuspendInput;
    }

    public synchronized boolean isResetRequested() {
        return reset != 0;
    }

    public synchronized int consumeReset() {
        int value = reset;
        reset = 0;
        return value;
    }

    public synchronized void reset(int reason) {
        assert reason != 0;
        reset |= reason;
        notifyAll(); // wake awaitPauseModeChange() (for example on termination)
        if (runningMediaCodec != null) {
            if (inputSuspended) {
                // Let the codec drain EOS. The next session applies the pending pause.
                setInputSuspended(false, false);
                inputSuspended = false;
            }
            try {
                runningMediaCodec.signalEndOfInputStream();
            } catch (IllegalStateException e) {
                // ignore
            }
        }
    }

    public synchronized void setPaused(boolean paused, boolean freeze) {
        if (this.paused == paused && (!paused || freezeWhilePaused == freeze)) {
            return;
        }
        boolean enteringPause = paused && !this.paused;
        this.paused = paused;
        freezeWhilePaused = freeze;
        if (paused) {
            pausedAt = SystemClock.uptimeMillis();
            // A normal pause can drop frames at the encoder's input while leaving the display
            // connected. Switching its surface can leave the app several frames behind on resume.
            if (enteringPause && !freeze && canSuspendInput && runningMediaCodec != null && reset == 0
                    && setInputSuspended(true, false)) {
                inputSuspended = true;
                Ln.i("Video paused, display active");
                return;
            }
            if (enteringPause || inputSuspended) {
                reset(RESET_REASON_PAUSED);
                return;
            }
        } else if (inputSuspended) {
            if (setInputSuspended(false, waitForKeyFrame)) {
                inputSuspended = false;
                keyFrameDeadline = SystemClock.uptimeMillis() + CODEC_TRANSITION_TIMEOUT_MS;
                Ln.i("Video resumed");
            } else {
                reset(RESET_REASON_PAUSED);
            }
            return;
        }
        notifyAll();
    }

    public synchronized void requestKeyFrame() {
        if (runningMediaCodec == null || paused || reset != 0) {
            return;
        }
        Bundle params = new Bundle();
        params.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0);
        try {
            runningMediaCodec.setParameters(params);
        } catch (RuntimeException e) {
            Ln.w("Could not request a video keyframe: " + e.getMessage());
        }
    }

    private boolean setInputSuspended(boolean suspended, boolean requestKeyFrame) {
        try {
            Bundle params = new Bundle();
            params.putInt(MediaCodec.PARAMETER_KEY_SUSPEND, suspended ? 1 : 0);
            if (requestKeyFrame) {
                params.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0);
            }
            runningMediaCodec.setParameters(params);
            return true;
        } catch (RuntimeException e) {
            Ln.w("Could not " + (suspended ? "suspend" : "resume") + " video input: " + e.getMessage());
            return false;
        }
    }

    public synchronized boolean shouldSendFrame(boolean keyFrame) {
        if (paused && (inputSuspended || waitForKeyFrame)) {
            if (inputSuspended) {
                waitForKeyFrame = true;
                // If the codec keeps encoding, use the stopped-encoder pause path.
                if (SystemClock.uptimeMillis() - pausedAt >= CODEC_TRANSITION_TIMEOUT_MS) {
                    Ln.w("Video input kept producing frames while suspended; stopping the encoder");
                    reset(RESET_REASON_PAUSED);
                }
            }
            return false;
        }
        if (waitForKeyFrame && !keyFrame) {
            if (SystemClock.uptimeMillis() >= keyFrameDeadline && reset == 0) {
                Ln.w("No key frame after resuming video input; restarting the encoder");
                reset(RESET_REASON_CLIENT_RESET);
            }
            return false;
        }
        waitForKeyFrame = false;
        return true;
    }

    public synchronized boolean isPaused() {
        return paused;
    }

    /**
     * Wait for the next surface mode. The encoder thread owns surface changes and
     * wakes promptly when the window is restored or the session ends.
     */
    public synchronized PauseMode awaitPauseModeChange(PauseMode applied, long intervalMs, long durationMs) throws InterruptedException {
        while (paused && (reset & RESET_REASON_TERMINATED) == 0) {
            PauseMode mode = freezeWhilePaused ? PauseMode.DISPLAY_FROZEN : PauseMode.VIDEO_PAUSED;
            long waitMs = 0; // wait indefinitely for an ordinary pause
            if (freezeWhilePaused && intervalMs > 0) {
                long elapsed = SystemClock.uptimeMillis() - pausedAt;
                long phase = elapsed % intervalMs;
                if (elapsed >= intervalMs && phase < durationMs) {
                    mode = PauseMode.VIDEO_PAUSED;
                    waitMs = durationMs - phase;
                } else {
                    waitMs = intervalMs - phase;
                }
            }
            if (mode != applied) {
                return mode;
            }
            wait(waitMs);
        }
        return PauseMode.RUNNING;
    }

    public synchronized void setRunningMediaCodec(MediaCodec runningMediaCodec) {
        this.runningMediaCodec = runningMediaCodec;
        inputSuspended = false;
        waitForKeyFrame = false;
    }
}
