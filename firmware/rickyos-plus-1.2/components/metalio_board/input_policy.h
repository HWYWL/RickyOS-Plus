#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct { bool armed, tracking, fired; uint64_t since; } PowerHold;
// Require release after startup or an invalid read; never infer a held key from stale I2C.
static inline bool power_hold_sample(PowerHold *s, bool valid, bool pressed, uint64_t ms) {
    if (!valid) { *s = (PowerHold){0}; return false; }
    if (!pressed) { *s = (PowerHold){.armed=true}; return false; }
    if (!s->armed || s->fired) return false;
    if (!s->tracking) { s->tracking=true; s->since=ms; }
    if (ms-s->since < 3000) return false;
    s->fired=true;
    return true;
}
static inline void touch_fixed_map(bool landscape, int rx, int ry, int *x, int *y) {
    *x=landscape ? 683-ry : rx; *y=landscape ? rx : ry;
    if (*x<0) *x=0;
    if (*x>683) *x=683;
    if (*y<0) *y=0;
    if (*y>1215) *y=1215;
}
