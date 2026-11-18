// Modified by appcpy: pausing video while the client window is minimized.
package com.genymobile.scrcpy.video;

import android.media.MediaCodec;
import android.os.SystemClock;

public class CaptureControl {

    public static final int RESET_REASON_TERMINATED = 1;
    public static final int RESET_REASON_DISPLAY_PROPERTIES_CHANGED = 1 << 1;
    public static final int RESET_REASON_CLIENT_RESET = 1 << 2;
    public static final int RESET_REASON_CLIENT_RESIZED = 1 << 3;
    // appcpy: interrupt the running encoder to pause the video
    public static final int RESET_REASON_PAUSED = 1 << 4;

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
            try {
                runningMediaCodec.signalEndOfInputStream();
            } catch (IllegalStateException e) {
                // ignore
            }
        }
    }

    // appcpy: while paused, the encoder stays stopped
    public synchronized void setPaused(boolean paused, boolean freeze) {
        if (this.paused == paused && (!paused || freezeWhilePaused == freeze)) {
            return;
        }
        boolean enteringPause = paused && !this.paused;
        this.paused = paused;
        freezeWhilePaused = freeze;
        if (paused) {
            pausedAt = SystemClock.uptimeMillis();
        }
        if (enteringPause) {
            reset(RESET_REASON_PAUSED);
        } else {
            notifyAll();
        }
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
    }
}
