#include "dash_debounce.h"

bool dash_debounce_step(dash_debounce_t *d, bool raw, uint8_t need)
{
    if (raw == d->state) {
        d->run = 0;
    } else if (++d->run >= need) {
        d->state = raw;
        d->run = 0;
    }
    return d->state;
}
