#ifndef SC_WINDOW_PLACEMENT_H
#define SC_WINDOW_PLACEMENT_H

#include <SDL3/SDL.h>

#include "coords.h"

struct sc_window_placement {
    long left, top, right, bottom;
    bool maximized;
};

bool
sc_window_placement_read(struct sc_window_placement *placement);

bool
sc_window_placement_apply(SDL_Window *window,
                          const struct sc_window_placement *saved,
                          bool fullscreen, struct sc_size *size);

void
sc_window_placement_show(SDL_Window *window);

#endif
