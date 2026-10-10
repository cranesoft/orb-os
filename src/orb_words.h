#pragma once
// The words the UI uses for its one input, said the way this board's input works.
//
// Every hint on the Orb is written for the knob: "turn to choose, push to select". The Big
// Orb has no knob; its glass stands in for one (knob_touch.cpp), and a hint that says
// "push" on a screen with nothing to push is a hint that is wrong. orb_ui() rewrites a
// knob sentence into a touch one on a board with ORB_HAS_TOUCH_KNOB, and returns it
// untouched everywhere else, so the Orb's text is byte for byte what it was.
//
//   turn -> circle     push -> tap     the knob -> the dial
//
// The result lives in a small ring of static buffers, which is fine for its one use:
// lv_label_set_text() copies the string before the next call can reuse the slot.
#include "config.h"

#if ORB_HAS_TOUCH_KNOB
#include <string.h>

inline const char *orb_ui(const char *s) {
    static char ring[4][160];
    static unsigned slot = 0;
    char *out = ring[slot++ & 3];
    static const struct { const char *from, *to; } W[] = {
        { "the knob", "the dial" }, { "The knob", "The dial" },
        { "turn",     "circle"   }, { "Turn",     "Circle"   },
        { "push",     "tap"      }, { "Push",     "Tap"      },
    };
    size_t o = 0;
    while (*s && o < sizeof(ring[0]) - 1) {
        bool hit = false;
        // Whole words only, so "return" never becomes "recircle".
        const bool startOfWord = (o == 0) || !((out[o - 1] >= 'a' && out[o - 1] <= 'z') ||
                                               (out[o - 1] >= 'A' && out[o - 1] <= 'Z'));
        if (startOfWord) {
            for (const auto &w : W) {
                const size_t n = strlen(w.from);
                const char after = s[n];
                if (strncmp(s, w.from, n) == 0 && !((after >= 'a' && after <= 'z') || (after >= 'A' && after <= 'Z'))) {
                    for (const char *t = w.to; *t && o < sizeof(ring[0]) - 1; ++t) out[o++] = *t;
                    s += n;
                    hit = true;
                    break;
                }
            }
        }
        if (!hit) out[o++] = *s++;
    }
    out[o] = 0;
    return out;
}
#else
inline const char *orb_ui(const char *s) { return s; }
#endif
