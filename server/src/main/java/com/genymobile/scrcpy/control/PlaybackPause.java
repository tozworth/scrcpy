// Modified by appcpy: package-scoped media pause before releasing audio capture.
package com.genymobile.scrcpy.control;

import com.genymobile.scrcpy.FakeContext;
import com.genymobile.scrcpy.util.Ln;

import android.content.Context;
import android.media.session.MediaController;
import android.media.session.MediaSessionManager;
import android.media.session.PlaybackState;
import android.os.SystemClock;

import java.util.ArrayList;
import java.util.List;

public final class PlaybackPause {
    private String packageName;
    private boolean attempted;

    // app_process does not run ZygoteInit's media-service initialization on Android 12+.
    // Initialize only if absent, leaving older Android versions and normal app contexts alone.
    public static synchronized void prepare() throws ReflectiveOperationException {
        Class<?> initializer;
        try {
            initializer = Class.forName("android.media.MediaFrameworkPlatformInitializer");
        } catch (ClassNotFoundException e) {
            return;
        }
        if (initializer.getMethod("getMediaServiceManager").invoke(null) == null) {
            Class<?> manager = Class.forName("android.media.MediaServiceManager");
            initializer.getMethod("setMediaServiceManager", manager).invoke(null, manager.getConstructor().newInstance());
        }
    }

    public synchronized void setPackageName(String packageName) {
        this.packageName = packageName;
    }

    // Recorder shutdown and server shutdown may race. Both must wait for this
    // operation before releasing capture; merely using an atomic flag is insufficient.
    public synchronized void pause() {
        if (attempted || packageName == null) {
            return;
        }
        attempted = true;
        try {
            prepare();
            Context context = FakeContext.get();
            MediaSessionManager manager = (MediaSessionManager) context.getSystemService(Context.MEDIA_SESSION_SERVICE);
            List<MediaController> pending = new ArrayList<>();
            for (MediaController session : manager.getActiveSessions(null)) {
                if (!packageName.equals(session.getPackageName())) {
                    continue;
                }
                // Use shell attribution even if the manager came from the system context.
                MediaController controller = new MediaController(context, session.getSessionToken());
                PlaybackState state = controller.getPlaybackState();
                if (state == null || isPlaying(state)) {
                    controller.getTransportControls().pause(); // Never a play/pause toggle.
                    pending.add(controller);
                }
            }
            // Pause is asynchronous. Allow its callback to stop playback before
            // REMOTE_SUBMIX is released, but never keep a closing window stuck.
            long deadline = SystemClock.uptimeMillis() + 400;
            while (!pending.isEmpty()) {
                for (int i = pending.size() - 1; i >= 0; --i) {
                    PlaybackState state = pending.get(i).getPlaybackState();
                    if (state != null && !isPlaying(state)) {
                        pending.remove(i);
                    }
                }
                if (pending.isEmpty()) {
                    break;
                }
                if (SystemClock.uptimeMillis() >= deadline) {
                    Ln.w("Timed out waiting for media to pause: " + packageName);
                    break;
                }
                SystemClock.sleep(20);
            }
            Ln.i("Requested media pause on close for " + packageName);
        } catch (Exception e) {
            // Shell normally has MEDIA_CONTENT_CONTROL. Never fall back to a
            // global media key or a force-stop if an OEM denies session access.
            Ln.w("Could not pause media for " + packageName, e);
        }
    }

    private static boolean isPlaying(PlaybackState state) {
        int value = state.getState();
        return value == PlaybackState.STATE_PLAYING || value == PlaybackState.STATE_BUFFERING
                || value == PlaybackState.STATE_CONNECTING || value == PlaybackState.STATE_FAST_FORWARDING
                || value == PlaybackState.STATE_REWINDING || value == PlaybackState.STATE_SKIPPING_TO_NEXT
                || value == PlaybackState.STATE_SKIPPING_TO_PREVIOUS || value == PlaybackState.STATE_SKIPPING_TO_QUEUE_ITEM;
    }
}
