// Modified by appcpy: pausing video while the client window is minimized.
package com.genymobile.scrcpy.video;

import android.media.MediaCodec;

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
        notifyAll(); // wake waitWhilePaused() (for example on termination)
        if (runningMediaCodec != null) {
            try {
                runningMediaCodec.signalEndOfInputStream();
            } catch (IllegalStateException e) {
                // ignore
            }
        }
    }

    // appcpy: while paused, the encoder stays stopped and the capture surface is detached
    public synchronized void setPaused(boolean paused, boolean freeze) {
        if (paused) {
            // Takes effect when the encoder pauses (immediately unless already paused)
            freezeWhilePaused = freeze;
        }
        if (this.paused == paused) {
            return;
        }
        this.paused = paused;
        if (paused) {
            reset(RESET_REASON_PAUSED);
        } else {
            notifyAll();
        }
    }

    public synchronized boolean isPaused() {
        return paused;
    }

    public synchronized boolean isFreezeWhilePaused() {
        return freezeWhilePaused;
    }

    /**
     * Block until the video is resumed or termination is requested.
     */
    public synchronized void waitWhilePaused() throws InterruptedException {
        while (paused && (reset & RESET_REASON_TERMINATED) == 0) {
            wait();
        }
    }

    public synchronized void setRunningMediaCodec(MediaCodec runningMediaCodec) {
        this.runningMediaCodec = runningMediaCodec;
    }
}
