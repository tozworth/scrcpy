#ifndef SC_SESSION_CAPTION_H
#define SC_SESSION_CAPTION_H

struct sc_screen;
struct sc_session_caption;

struct sc_session_caption *
sc_session_caption_create(struct sc_screen *screen);

void
sc_session_caption_destroy(struct sc_session_caption *caption);

#endif
