#include "window_placement.h"

#include <limits.h>
#include <stdlib.h>
#include <windows.h>
#include <dwmapi.h>

#include "util/env.h"
#include "util/log.h"
#include "util/sdl.h"
#include "util/str.h"

bool
sc_window_placement_read(struct sc_window_placement *placement) {
    char *value = sc_get_env("APPCPY_WINDOW_PLACEMENT");
    if (!value) {
        return false;
    }

    long fields[5];
    size_t count = sc_str_parse_integers(value, ',', ARRAY_LEN(fields), fields);
    free(value);
    if (count != ARRAY_LEN(fields) || fields[4] < 0 || fields[4] > 1) {
        return false;
    }

    int64_t width = (int64_t) fields[2] - fields[0];
    int64_t height = (int64_t) fields[3] - fields[1];
    if (width <= 0 || height <= 0 || width > INT_MAX || height > INT_MAX) {
        return false;
    }

    *placement = (struct sc_window_placement) {
        .left = fields[0], .top = fields[1],
        .right = fields[2], .bottom = fields[3],
        .maximized = fields[4],
    };
    return true;
}

static HWND
get_hwnd(SDL_Window *window) {
    return SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                  SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
}

bool
sc_window_placement_apply(SDL_Window *window,
                          const struct sc_window_placement *saved,
                          bool fullscreen, struct sc_size *size) {
    HWND hwnd = get_hwnd(window);
    WINDOWPLACEMENT placement = {
        .length = sizeof(placement),
        .showCmd = SW_HIDE,
        .ptMinPosition = {-1, -1},
        .ptMaxPosition = {-1, -1},
        .rcNormalPosition = {saved->left, saved->top, saved->right, saved->bottom},
    };
    if (!hwnd) {
        return false;
    }

    HMONITOR original_monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!SetWindowPlacement(hwnd, &placement)) {
        return false;
    }
    // SDL may preserve the old client size during a monitor DPI transition.
    // Reapply the outer bounds once the window is on the destination monitor.
    if (MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) != original_monitor
            && !SetWindowPlacement(hwnd, &placement)) {
        return false;
    }

    if (saved->maximized || fullscreen) {
        // Windows computes these client bounds when showing the window.
        // Keep it cloaked until the stream is ready.
        BOOL cloak = TRUE;
        if (FAILED(DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak)))) {
            return false;
        }
        if (saved->maximized && !SDL_MaximizeWindow(window)) {
            return false;
        }
        if (fullscreen && !SDL_SetWindowFullscreen(window, true)) {
            return false;
        }
        if (!SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOACTIVATE
                         | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER)) {
            return false;
        }
    }

    int width, height;
    if (!SDL_GetWindowSize(window, &width, &height) || width <= 0 || height <= 0
            || width > UINT16_MAX || height > UINT16_MAX) {
        return false;
    }
    *size = (struct sc_size) {width, height};
    return true;
}

void
sc_window_placement_show(SDL_Window *window) {
    if (SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN) {
        sc_sdl_show_window(window);
    } else {
        BOOL cloak = FALSE;
        HWND hwnd = get_hwnd(window);
        if (FAILED(DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak)))) {
            LOGW("Could not uncloak the restored window");
        }
        SDL_RaiseWindow(window);
    }
}
