// Modified by appcpy: pausing video while the client window is minimized.
package com.genymobile.scrcpy.video;

import com.genymobile.scrcpy.AndroidVersions;
import com.genymobile.scrcpy.Options;
import com.genymobile.scrcpy.control.PositionMapper;
import com.genymobile.scrcpy.display.DisplayInfo;
import com.genymobile.scrcpy.display.DisplayMonitor;
import com.genymobile.scrcpy.display.DisplayProperties;
import com.genymobile.scrcpy.display.DisplayPropertiesTracker;
import com.genymobile.scrcpy.display.DisplayResizeDebouncer;
import com.genymobile.scrcpy.model.NewDisplay;
import com.genymobile.scrcpy.model.Orientation;
import com.genymobile.scrcpy.model.Size;
import com.genymobile.scrcpy.opengl.AffineOpenGLFilter;
import com.genymobile.scrcpy.opengl.OpenGLFilter;
import com.genymobile.scrcpy.opengl.OpenGLRunner;
import com.genymobile.scrcpy.util.AffineMatrix;
import com.genymobile.scrcpy.util.Command;
import com.genymobile.scrcpy.util.Ln;
import com.genymobile.scrcpy.wrappers.ServiceManager;

import android.graphics.ImageFormat;
import android.graphics.Rect;
import android.hardware.display.VirtualDisplay;
import android.media.Image;
import android.media.ImageReader;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.view.Display;
import android.view.Surface;

import java.io.IOException;

public class NewDisplayCapture extends SurfaceCapture {

    // Internal fields copied from android.hardware.display.DisplayManager
    private static final int VIRTUAL_DISPLAY_FLAG_PUBLIC = android.hardware.display.DisplayManager.VIRTUAL_DISPLAY_FLAG_PUBLIC;
    private static final int VIRTUAL_DISPLAY_FLAG_PRESENTATION = android.hardware.display.DisplayManager.VIRTUAL_DISPLAY_FLAG_PRESENTATION;
    private static final int VIRTUAL_DISPLAY_FLAG_SECURE = 1 << 2;
    private static final int VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY = android.hardware.display.DisplayManager.VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY;
    private static final int VIRTUAL_DISPLAY_FLAG_SUPPORTS_TOUCH = 1 << 6;
    private static final int VIRTUAL_DISPLAY_FLAG_ROTATES_WITH_CONTENT = 1 << 7;
    private static final int VIRTUAL_DISPLAY_FLAG_DESTROY_CONTENT_ON_REMOVAL = 1 << 8;
    private static final int VIRTUAL_DISPLAY_FLAG_SHOULD_SHOW_SYSTEM_DECORATIONS = 1 << 9;
    private static final int VIRTUAL_DISPLAY_FLAG_TRUSTED = 1 << 10;
    private static final int VIRTUAL_DISPLAY_FLAG_OWN_DISPLAY_GROUP = 1 << 11;
    private static final int VIRTUAL_DISPLAY_FLAG_ALWAYS_UNLOCKED = 1 << 12;
    private static final int VIRTUAL_DISPLAY_FLAG_TOUCH_FEEDBACK_DISABLED = 1 << 13;
    private static final int VIRTUAL_DISPLAY_FLAG_OWN_FOCUS = 1 << 14;
    private static final int VIRTUAL_DISPLAY_FLAG_DEVICE_DISPLAY_GROUP = 1 << 15;

    private final VirtualDisplayListener vdListener;
    private final NewDisplay newDisplay;

    private final DisplayMonitor displayMonitor = new DisplayMonitor();

    private AffineMatrix displayTransform;
    private AffineMatrix eventTransform;
    private OpenGLRunner glRunner;

    private Size mainDisplaySize;
    private int mainDisplayDpi;
    private final int displayImePolicy;
    private final Rect crop;
    private final boolean captureOrientationLocked;
    private final Orientation captureOrientation;
    private final float angle;
    private final boolean vdDestroyContent;
    private final boolean vdSystemDecorations;
    private final int vdRefreshRate;
    private final boolean secureDisplay;
    private final boolean flexDisplay;
    private final int flexScaleNumerator;
    private final int flexScaleDenominator;

    private VideoConstraints videoConstraints;

    private VirtualDisplay virtualDisplay;

    private boolean pausePowerOverride;

    // While capture is paused, the fallback sink drops frames as soon as they arrive.
    private ImageReader pauseSink;
    private HandlerThread pauseSinkThread;
    private Size videoSize;
    private Size displaySize; // the logical size of the display (including rotation)
    private Size physicalSize; // the physical size of the display (without rotation)

    private DisplayPropertiesTracker tracker;
    private DisplayResizeDebouncer debouncer;

    private int dpi;

    public NewDisplayCapture(VirtualDisplayListener vdListener, Options options) {
        this.vdListener = vdListener;
        this.newDisplay = options.getNewDisplay();
        assert newDisplay != null;
        this.displayImePolicy = options.getDisplayImePolicy();
        this.crop = options.getCrop();
        assert options.getCaptureOrientationLock() != null;
        this.captureOrientationLocked = options.getCaptureOrientationLock() != Orientation.Lock.Unlocked;
        this.captureOrientation = options.getCaptureOrientation();
        assert captureOrientation != null;
        this.angle = options.getAngle();
        this.vdDestroyContent = options.getVDDestroyContent();
        this.vdSystemDecorations = options.getVDSystemDecorations();
        this.vdRefreshRate = options.getVdRefreshRate();
        this.secureDisplay = options.getSecureDisplay();
        this.flexDisplay = options.getFlexDisplay();
        int scale = options.getFlexDisplayScale();
        boolean exactThird = scale == 33 && options.getFlexDisplayIntegerScaling();
        this.flexScaleNumerator = exactThird ? 1 : scale;
        this.flexScaleDenominator = exactThird ? 3 : 100;
    }

    @Override
    protected void init(VideoConstraints videoConstraints) {
        setVideoConstraints(videoConstraints); // synchronized

        displaySize = newDisplay.getSize();
        dpi = newDisplay.getDpi();
        if (flexDisplay) {
            if (crop != null) {
                throw new IllegalArgumentException("Flex display does not support cropping");
            }

            tracker = new DisplayPropertiesTracker();
            debouncer = new DisplayResizeDebouncer(this::triggerResize);
            debouncer.start();

            // Hardcode default values if not defined
            if (displaySize == null) {
                displaySize = new Size(1280, 960);
            }
            if (flexScaleNumerator != flexScaleDenominator) {
                displaySize = new Size(scaleInitialDimension(displaySize.getWidth()), scaleInitialDimension(displaySize.getHeight()));
            }
            if (dpi == 0) {
                dpi = 160;
            }
        } else if (displaySize == null || dpi == 0) {
            DisplayInfo displayInfo = ServiceManager.getDisplayManager().getDisplayInfo(0);
            if (displayInfo != null) {
                mainDisplaySize = displayInfo.getSize();
                if ((displayInfo.getRotation() % 2) != 0) {
                    mainDisplaySize = mainDisplaySize.rotate(); // Use the natural device orientation (at rotation 0), not the current one
                }
                mainDisplayDpi = displayInfo.getDpi();
            } else {
                Ln.w("Main display not found, fallback to 1920x1080 240dpi");
                mainDisplaySize = new Size(1920, 1080);
                mainDisplayDpi = 240;
            }
        }
    }

    private int scaleInitialDimension(int dimension) {
        // Match the client's calculation for window resize requests.
        return (int) Math.max(16, (long) dimension * flexScaleNumerator / flexScaleDenominator);
    }

    @Override
    public void prepare() {
        int displayRotation;
        if (virtualDisplay == null) {
            if (flexDisplay) {
                assert displaySize != null;
                displaySize = displaySize.constrain(videoConstraints, false);
            } else {
                if (displaySize == null) {
                    assert !flexDisplay;
                    displaySize = mainDisplaySize;
                }

                // Align the physical display size to avoid unnecessary mismatches with the output size
                displaySize = displaySize.align(videoConstraints.getAlignment());
            }

            if (dpi == 0) {
                assert !flexDisplay;
                dpi = scaleDpi(mainDisplaySize, mainDisplayDpi, displaySize);
            }

            displayRotation = 0;
            // Set the current display properties to avoid an unnecessary capture reset
            displayMonitor.setSessionDisplayProperties(new DisplayProperties(displaySize, displayRotation));
        } else {
            DisplayInfo displayInfo = ServiceManager.getDisplayManager().getDisplayInfo(virtualDisplay.getDisplay().getDisplayId());
            dpi = displayInfo.getDpi();
            displayRotation = displayInfo.getRotation();
            displaySize = displayInfo.getSize();
            if (flexDisplay) {
                displaySize = displaySize.constrain(videoConstraints, false);
            } else {
                // Align the physical display size to avoid unnecessary mismatches with the output size
                displaySize = displaySize.align(videoConstraints.getAlignment());
            }
        }

        VideoFilter filter = new VideoFilter(displaySize);

        if (crop != null) {
            boolean transposed = (displayRotation % 2) != 0;
            filter.addCrop(crop, transposed);
        }

        filter.addOrientation(displayRotation, captureOrientationLocked, captureOrientation);
        filter.addAngle(angle);

        if (!flexDisplay) {
            Size outputSize = filter.getOutputSize();
            Size filteredSize = outputSize.constrain(videoConstraints);
            if (!filteredSize.equals(outputSize)) {
                filter.addResize(filteredSize);
            }
        }

        eventTransform = filter.getInverseTransform();

        // DisplayInfo gives the oriented size (so videoSize includes the display rotation)
        videoSize = filter.getOutputSize();

        // However, the virtual display video always remains in its original orientation, so it must be rotated manually.
        // This additional display rotation must not be included in the input events transform (the expected coordinates are already in the
        // physical display size)
        if ((displayRotation % 2) == 0) {
            physicalSize = displaySize;
        } else {
            physicalSize = displaySize.rotate();
        }
        VideoFilter displayFilter = new VideoFilter(physicalSize);
        displayFilter.addRotation(displayRotation);
        AffineMatrix displayRotationMatrix = displayFilter.getInverseTransform();

        // Take care of multiplication order:
        //   displayTransform = (FILTER_MATRIX * DISPLAY_FILTER_MATRIX)⁻¹
        //                    = DISPLAY_FILTER_MATRIX⁻¹ * FILTER_MATRIX⁻¹
        //                    = displayRotationMatrix * eventTransform
        displayTransform = AffineMatrix.multiplyAll(displayRotationMatrix, eventTransform);
    }

    public void startNew(Surface surface) {
        try {
            int flags = VIRTUAL_DISPLAY_FLAG_PUBLIC
                    | VIRTUAL_DISPLAY_FLAG_PRESENTATION
                    | VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY
                    | VIRTUAL_DISPLAY_FLAG_SUPPORTS_TOUCH
                    | VIRTUAL_DISPLAY_FLAG_ROTATES_WITH_CONTENT;
            if (secureDisplay) {
                flags |= VIRTUAL_DISPLAY_FLAG_SECURE;
            }
            if (vdDestroyContent) {
                flags |= VIRTUAL_DISPLAY_FLAG_DESTROY_CONTENT_ON_REMOVAL;
            }
            if (vdSystemDecorations) {
                flags |= VIRTUAL_DISPLAY_FLAG_SHOULD_SHOW_SYSTEM_DECORATIONS;
            }
            if (Build.VERSION.SDK_INT >= AndroidVersions.API_33_ANDROID_13) {
                flags |= VIRTUAL_DISPLAY_FLAG_TRUSTED
                        | VIRTUAL_DISPLAY_FLAG_OWN_DISPLAY_GROUP
                        | VIRTUAL_DISPLAY_FLAG_ALWAYS_UNLOCKED
                        | VIRTUAL_DISPLAY_FLAG_TOUCH_FEEDBACK_DISABLED;
                if (Build.VERSION.SDK_INT >= AndroidVersions.API_34_ANDROID_14) {
                    flags |= VIRTUAL_DISPLAY_FLAG_OWN_FOCUS
                            | VIRTUAL_DISPLAY_FLAG_DEVICE_DISPLAY_GROUP;
                }
            }
            VirtualDisplay vd = ServiceManager.getDisplayManager()
                    .createNewVirtualDisplay("scrcpy", displaySize.getWidth(), displaySize.getHeight(), dpi, surface, flags, vdRefreshRate);
            setCurrentVirtualDisplay(vd); // used for client resize
            int virtualDisplayId = vd.getDisplay().getDisplayId();
            Ln.i("New display: " + displaySize.getWidth() + "x" + displaySize.getHeight() + "/" + dpi + " (id=" + virtualDisplayId + ")");

            if (displayImePolicy != -1) {
                ServiceManager.getWindowManager().setDisplayImePolicy(virtualDisplayId, displayImePolicy);
            }

            displayMonitor.start(virtualDisplayId, (props) -> {
                int reason;
                if (flexDisplay) {
                    boolean isClientResize = tracker.onChanged(props);
                    if (isClientResize) {
                        reason = CaptureControl.RESET_REASON_CLIENT_RESIZED;
                    } else {
                        reason = CaptureControl.RESET_REASON_DISPLAY_PROPERTIES_CHANGED;
                        // Display properties have changed, cancel pending client resize requests
                        debouncer.cancelResize();
                    }
                } else {
                    reason = CaptureControl.RESET_REASON_DISPLAY_PROPERTIES_CHANGED;
                }
                getCaptureControl().reset(reason);
            });
        } catch (Exception e) {
            Ln.e("Could not create display", e);
            throw new AssertionError("Could not create display");
        }
    }

    @Override
    public void start(Surface surface) throws IOException {
        if (displayTransform != null) {
            assert glRunner == null;
            OpenGLFilter glFilter = new AffineOpenGLFilter(displayTransform);
            glRunner = new OpenGLRunner(glFilter);
            surface = glRunner.start(physicalSize, videoSize, surface);
        }

        if (virtualDisplay == null) {
            startNew(surface);
        } else {
            virtualDisplay.setSurface(surface);
        }
        releasePauseSink();

        if (vdListener != null) {
            PositionMapper positionMapper = PositionMapper.create(videoSize, eventTransform, displaySize);
            vdListener.onNewVirtualDisplay(virtualDisplay.getDisplay().getDisplayId(), positionMapper);
        }
    }

    @Override
    public void onEncoderStarted() throws IOException {
        if (pausePowerOverride && !setPausePower(false)) {
            throw new IOException("Could not restore virtual display power");
        }
    }

    @Override
    public void stop() {
        if (glRunner != null) {
            glRunner.stopAndRelease();
            glRunner = null;
        }
    }

    @Override
    public void pause(boolean freeze, Surface persistentSurface) {
        // Freeze stops display composition. Keep the persistent encoder surface connected
        // when Android lets us power off this display independently.
        // A non-frozen pause must keep consuming buffers so apps can continue drawing.
        synchronized (this) {
            if (virtualDisplay == null) {
                return;
            }
            if (freeze) {
                if (persistentSurface == null || virtualDisplay.getSurface() != persistentSurface || !setPausePower(true)) {
                    virtualDisplay.setSurface(null);
                }
                releasePauseSink();
                Ln.i("Video paused, display frozen");
                return;
            }
            if (pausePowerOverride) {
                Ln.i(setPausePower(false) ? "Video paused, display active" : "Video paused, display frozen");
                return;
            }
            releasePauseSink();
            try {
                pauseSinkThread = new HandlerThread("pause-sink");
                pauseSinkThread.start();
                pauseSink = ImageReader.newInstance(physicalSize.getWidth(), physicalSize.getHeight(), ImageFormat.PRIVATE, 2);
                pauseSink.setOnImageAvailableListener(reader -> {
                    try {
                        Image image = reader.acquireLatestImage();
                        if (image != null) {
                            image.close();
                        }
                    } catch (RuntimeException e) {
                        // The reader may be closed concurrently on resume
                    }
                }, new Handler(pauseSinkThread.getLooper()));
                virtualDisplay.setSurface(pauseSink.getSurface());
            } catch (RuntimeException e) {
                Ln.w("Could not create the paused video sink, detaching the display surface: " + e.getMessage());
                releasePauseSink();
                virtualDisplay.setSurface(null);
                Ln.i("Video paused, display frozen");
                return;
            }
            Ln.i("Video paused, display active");
        }
    }

    private boolean setPausePower(boolean off) {
        if (Build.VERSION.SDK_INT < AndroidVersions.API_35_ANDROID_15) {
            return false;
        }
        Display display = virtualDisplay.getDisplay();
        String displayId = Integer.toString(display.getDisplayId());
        try {
            if (off) {
                Command.exec("cmd", "display", "power-off", displayId);
                pausePowerOverride = true;
                if (display.getState() == Display.STATE_OFF) {
                    return true;
                }
            }
            Command.exec("cmd", "display", "power-reset", displayId);
            pausePowerOverride = false;
            return !off;
        } catch (IOException e) {
            Ln.d("Could not " + (off ? "blank" : "restore") + " the paused virtual display: " + e.getMessage());
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        return false;
    }

    private synchronized void releasePauseSink() {
        if (pauseSink != null) {
            pauseSink.close();
            pauseSink = null;
        }
        if (pauseSinkThread != null) {
            pauseSinkThread.quitSafely();
            pauseSinkThread = null;
        }
    }

    @Override
    public void release() {
        displayMonitor.stopAndRelease();

        if (debouncer != null) {
            debouncer.stop();
        }

        if (virtualDisplay != null) {
            // synchronized with triggerResize()
            synchronized (this) {
                virtualDisplay.release();
                setCurrentVirtualDisplay(null);
            }
        }
        releasePauseSink();
    }

    @Override
    public Size getSize() {
        return videoSize;
    }

    @Override
    protected boolean applyNewVideoConstraints(VideoConstraints videoConstraints) {
        setVideoConstraints(videoConstraints); // with synchronization
        return true;
    }

    private static int scaleDpi(Size initialSize, int initialDpi, Size size) {
        int den = initialSize.getMax();
        int num = size.getMax();
        return initialDpi * num / den;
    }

    public void requestResize(int width, int height) {
        if (!flexDisplay) {
            throw new IllegalStateException("Cannot resize a non-flex display");
        }

        VideoConstraints constraints = getVideoConstraints(); // synchronized
        Size newSize = new Size(width, height).constrain(constraints, false);
        if (Ln.isEnabled(Ln.Level.VERBOSE)) {
            Ln.v(getClass().getSimpleName() + ": requestResize(" + width + ", " + height + ")");
            Ln.v(getClass().getSimpleName() + ": constrained size = " + newSize);
        }

        debouncer.requestResize(newSize);
    }

    private synchronized void setCurrentVirtualDisplay(VirtualDisplay virtualDisplay) {
        this.virtualDisplay = virtualDisplay;
    }

    private synchronized VideoConstraints getVideoConstraints() {
        return videoConstraints;
    }

    private synchronized void setVideoConstraints(VideoConstraints videoConstraints) {
        this.videoConstraints = videoConstraints;
    }

    private synchronized void triggerResize(Size size) {
        if (virtualDisplay != null) {
            size = size.constrain(videoConstraints, false); // in case the constraints have changed
            int displayId = virtualDisplay.getDisplay().getDisplayId();
            DisplayInfo displayInfo = ServiceManager.getDisplayManager().getDisplayInfo(displayId);
            @SuppressWarnings("checkstyle:HiddenField") // hides this.dpi on purpose
            int dpi = displayInfo.getDpi();
            int displayRotation = displayInfo.getRotation();
            if (captureOrientation.isSwap()) {
                size = size.rotate();
            }
            tracker.pushClientRequest(new DisplayProperties(size, displayRotation));

            // Although the display size (as detected by the DisplayMonitor) is rotated, the virtual display itself is not
            Size vdSize = (displayRotation % 2) == 0 ? size : size.rotate();
            virtualDisplay.resize(vdSize.getWidth(), vdSize.getHeight(), dpi);
        }
    }
}
