// Keep the SDL window's native non-client area. An owned layered window adds
// just the logo over the caption, with no work in DWM's paint or hit-test path.
#include "session_caption.h"

#include "screen.h"

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <oleacc.h>

#include "util/log.h"

#ifndef WM_DPICHANGED
# define WM_DPICHANGED 0x02E0
#endif

struct sc_session_caption {
    struct sc_screen *screen;
    HWND owner;
    HWND button;
    HWND host;
    UINT open_message;
    UINT pause_message;
    UINT pin_message;
    int width;
    int height;
    int icon_size;
    HICON dark_icon;
    HICON light_icon;
    bool hover;
    bool pressed;
};

static const WCHAR button_class[] = L"AppcpySessionCaption";

static bool
use_light_icon(HWND owner) {
    HIGHCONTRASTW contrast = { .cbSize = sizeof(contrast) };
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0)
            && (contrast.dwFlags & HCF_HIGHCONTRASTON)) {
        COLORREF text = GetSysColor(GetActiveWindow() == owner
                                   ? COLOR_CAPTIONTEXT : COLOR_INACTIVECAPTIONTEXT);
        return GetRValue(text) + GetGValue(text) + GetBValue(text) > 3 * 127;
    }

    DWORD prevalence = 0;
    DWORD size = sizeof(prevalence);
    const WCHAR dwm[] = L"Software\\Microsoft\\Windows\\DWM";
    if (GetActiveWindow() == owner
            && RegGetValueW(HKEY_CURRENT_USER, dwm, L"ColorPrevalence",
                            RRF_RT_REG_DWORD, NULL, &prevalence, &size) == ERROR_SUCCESS
            && prevalence) {
        DWORD accent;
        size = sizeof(accent);
        if (RegGetValueW(HKEY_CURRENT_USER, dwm, L"AccentColor",
                         RRF_RT_REG_DWORD, NULL, &accent, &size) == ERROR_SUCCESS) {
            return 3 * GetRValue(accent) + 6 * GetGValue(accent) + GetBValue(accent)
                   < 10 * 140;
        }
    }
    BOOL dark = FALSE;
    DwmGetWindowAttribute(owner, 20, &dark, sizeof(dark)); // immersive dark mode
    return dark;
}

static void
paint_button(struct sc_session_caption *caption) {
    int width = caption->width;
    int height = caption->height;
    if (!width || !height) {
        return;
    }

    bool light = use_light_icon(caption->owner);
    HICON icon = light ? caption->light_icon : caption->dark_icon;
    BITMAPINFO info = { .bmiHeader = {
        .biSize = sizeof(BITMAPINFOHEADER), .biWidth = width, .biHeight = -height,
        .biPlanes = 1, .biBitCount = 32, .biCompression = BI_RGB,
    }};
    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) {
        return;
    }
    DWORD *pixels;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS,
                                     (void **) &pixels, NULL, 0);
    if (bitmap) {
        HGDIOBJ previous = SelectObject(dc, bitmap);
        size_t count = (size_t) width * height;
        memset(pixels, 0, count * sizeof(*pixels));
        int size = caption->icon_size;
        DrawIconEx(dc, (width - size) / 2, (height - size) / 2, icon,
                   size, size, 0, NULL, DI_NORMAL);
        // A faint background on hover also keeps the whole button hit-testable.
        // Alpha zero would let clicks between parts of the logo reach the title.
        unsigned background = caption->hover ? 24 : 1;
        for (size_t i = 0; i < count; ++i) {
            DWORD color = pixels[i];
            unsigned extra = background * (255 - (color >> 24)) / 255;
            pixels[i] = color + (light ? extra * 0x01010101 : extra << 24);
        }
        SIZE bounds = {width, height};
        POINT source = {0, 0};
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(caption->button, NULL, NULL, &bounds, dc, &source,
                            0, &blend, ULW_ALPHA);
        SelectObject(dc, previous);
        DeleteObject(bitmap);
    }
    DeleteDC(dc);
}

static HICON
load_icon(const WCHAR *name, int size) {
    WCHAR path[32768];
    DWORD length = GetEnvironmentVariableW(L"APPCPY_CAPTION_ASSETS", path,
                                           ARRAY_LEN(path));
    if (!length || length >= ARRAY_LEN(path)
            || length + wcslen(name) + 2 > ARRAY_LEN(path)) {
        return NULL;
    }
    path[length++] = L'\\';
    wcscpy(path + length, name);
    return LoadImageW(NULL, path, IMAGE_ICON, size, size, LR_LOADFROMFILE);
}

static void
position_button(struct sc_session_caption *caption, bool refresh) {
    HWND owner = caption->owner;
    LONG_PTR style = GetWindowLongPtrW(owner, GWL_STYLE);
    if (!IsWindowVisible(owner) || IsIconic(owner)
            || (style & WS_CAPTION) != WS_CAPTION
            || !(style & WS_SYSMENU)) {
        ShowWindow(caption->button, SW_HIDE);
        return;
    }

    // Indices 2..5 are minimize, maximize, help and close. Windows supplies
    // physical screen coordinates, including when snapped or maximized.
    TITLEBARINFOEX title = { .cbSize = sizeof(title) };
    SendMessageW(owner, WM_GETTITLEBARINFOEX, 0, (LPARAM) &title);
    RECT bounds = {0};
    for (unsigned i = 2; i <= 5; ++i) {
        if (!(title.rgstate[i] & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN))
                && !IsRectEmpty(&title.rgrect[i])) {
            bounds = title.rgrect[i];
            break;
        }
    }
    if (IsRectEmpty(&bounds)) {
        ShowWindow(caption->button, SW_HIDE);
        return;
    }

    int width = (bounds.right - bounds.left) * 2 / 3;
    int height = bounds.bottom - bounds.top;
    int icon_size = MulDiv(height, 16, 30);
    if (icon_size != caption->icon_size) {
        if (caption->dark_icon) DestroyIcon(caption->dark_icon);
        if (caption->light_icon) DestroyIcon(caption->light_icon);
        caption->dark_icon = load_icon(L"appcpy-mono-dark.ico", icon_size);
        caption->light_icon = load_icon(L"appcpy-mono-light.ico", icon_size);
        caption->icon_size = icon_size;
        refresh = true;
    }
    if (!caption->dark_icon || !caption->light_icon) {
        ShowWindow(caption->button, SW_HIDE);
        return;
    }
    if (refresh || width != caption->width || height != caption->height) {
        caption->width = width;
        caption->height = height;
        paint_button(caption);
    }
    SetWindowPos(caption->button, NULL, bounds.left - width, bounds.top,
                 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static LRESULT CALLBACK
button_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        CREATESTRUCTW *create = (CREATESTRUCTW *) lparam;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR) create->lpCreateParams);
    }
    struct sc_session_caption *caption =
        (void *) GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!caption) {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    switch (message) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_MOUSEMOVE:
            if (!caption->hover) {
                TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&track);
                caption->hover = true;
                paint_button(caption);
            }
            return 0;
        case WM_MOUSELEAVE:
            caption->hover = false;
            paint_button(caption);
            return 0;
        case WM_LBUTTONDOWN:
            caption->pressed = true;
            SetCapture(hwnd);
            return 0;
        case WM_CAPTURECHANGED:
            caption->pressed = false;
            return 0;
        case WM_LBUTTONUP: {
            bool pressed = caption->pressed;
            caption->pressed = false;
            if (GetCapture() == hwnd) ReleaseCapture();
            POINT point;
            RECT bounds;
            if (!pressed || caption->screen->disconnected
                    || !GetCursorPos(&point) || !GetWindowRect(hwnd, &bounds)
                    || !PtInRect(&bounds, point)) {
                return 0;
            }
            DWORD host_pid = 0;
            DWORD foreground_pid = 0;
            GetWindowThreadProcessId(caption->host, &host_pid);
            GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);
            if (host_pid && foreground_pid != host_pid) {
                SetForegroundWindow(caption->owner);
                AllowSetForegroundWindow(host_pid);
            }
            LPARAM state = (caption->screen->controller ? 1 : 0)
                         | (caption->screen->video_paused_manually ? 2 : 0)
                         | ((SDL_GetWindowFlags(caption->screen->window)
                             & SDL_WINDOW_ALWAYS_ON_TOP) ? 4 : 0);
            PostMessageW(caption->host, caption->open_message, (WPARAM) hwnd,
                         state);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

static LRESULT CALLBACK
owner_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
           UINT_PTR id, DWORD_PTR data) {
    (void) id;
    struct sc_session_caption *caption = (void *) data;
    if (message == caption->pause_message && (HWND) lparam == caption->host) {
        return sc_screen_set_manual_video_pause(caption->screen, wparam != 0);
    }
    if (message == caption->pin_message && (HWND) lparam == caption->host) {
        // SDL reapplies its own topmost flag when resizing or leaving fullscreen.
        return !caption->screen->disconnected
            && SDL_SetWindowAlwaysOnTop(caption->screen->window, wparam != 0);
    }
    LRESULT result = DefSubclassProc(hwnd, message, wparam, lparam);
    switch (message) {
        case WM_WINDOWPOSCHANGED:
        case WM_STYLECHANGED:
        case WM_SHOWWINDOW:
            position_button(caption, false);
            break;
        case WM_DPICHANGED:
        case WM_ACTIVATE:
        case WM_NCACTIVATE:
        case WM_THEMECHANGED:
        case WM_SETTINGCHANGE:
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            position_button(caption, true);
            break;
    }
    return result;
}

struct sc_session_caption *
sc_session_caption_create(struct sc_screen *screen) {
    const char *host_value = SDL_getenv("APPCPY_CAPTION_HOST");
    if (!host_value) {
        return NULL;
    }
    char *end;
    uintptr_t host = (uintptr_t) strtoull(host_value, &end, 10);
    if (end == host_value || *end || !IsWindow((HWND) host)) {
        return NULL;
    }
    HWND owner = SDL_GetPointerProperty(SDL_GetWindowProperties(screen->window),
                                       SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW wc = {
        .lpfnWndProc = button_proc, .hInstance = instance,
        .hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512)), // IDC_ARROW
        .lpszClassName = button_class,
    };
    if (!owner || (!RegisterClassW(&wc)
                   && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)) {
        return NULL;
    }

    struct sc_session_caption *caption = calloc(1, sizeof(*caption));
    if (!caption) {
        LOG_OOM();
        return NULL;
    }
    caption->screen = screen;
    caption->owner = owner;
    caption->host = (HWND) host;
    caption->open_message = RegisterWindowMessageW(L"appcpy.OpenSessionDeck");
    caption->pause_message = RegisterWindowMessageW(L"appcpy.SetSessionPaused");
    caption->pin_message = RegisterWindowMessageW(L"appcpy.SetSessionPinned");
    caption->button = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                     button_class, L"appcpy controls", WS_POPUP,
                                     0, 0, 0, 0, owner, NULL, instance, caption);
    if (!caption->button || !caption->open_message || !caption->pause_message
            || !caption->pin_message
            || !SetWindowSubclass(owner, owner_proc, 1, (DWORD_PTR) caption)) {
        LOGW("Could not add the appcpy caption button");
        sc_session_caption_destroy(caption);
        return NULL;
    }
    return caption;
}

void
sc_session_caption_destroy(struct sc_session_caption *caption) {
    if (caption) {
        RemoveWindowSubclass(caption->owner, owner_proc, 1);
        if (caption->button) DestroyWindow(caption->button);
        if (caption->dark_icon) DestroyIcon(caption->dark_icon);
        if (caption->light_icon) DestroyIcon(caption->light_icon);
        free(caption);
    }
}
