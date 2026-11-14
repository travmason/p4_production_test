/* Host test for the lamp-input debounce. Run with `ctest --test-dir sim/build`. */

#include <stdio.h>

#include "dash_debounce.h"

static int s_failed;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            s_failed++;                                                 \
        }                                                               \
    } while (0)

/* Feed a string of '0'/'1' samples and return the outputs the same way. */
static const char *run(dash_debounce_t *d, const char *in, char *out)
{
    int i = 0;
    for (; in[i] != '\0'; i++) {
        out[i] = dash_debounce_step(d, in[i] == '1', 3) ? '1' : '0';
    }
    out[i] = '\0';
    return out;
}

#include <string.h>

int main(void)
{
    char out[64];
    dash_debounce_t d = {0};

    /* Turns on at the third agreeing sample, and off the same way. */
    CHECK(strcmp(run(&d, "0011111000", out), "0000111110") == 0);

    /* Spikes of one or two samples never get through, either way. */
    d = (dash_debounce_t) {0};
    CHECK(strcmp(run(&d, "0100110010", out), "0000000000") == 0);
    d = (dash_debounce_t) { .state = true };
    CHECK(strcmp(run(&d, "1011001101", out), "1111111111") == 0);

    /* An interrupted run starts counting again. */
    d = (dash_debounce_t) {0};
    CHECK(strcmp(run(&d, "1101101110", out), "0000000011") == 0);

    /* A 1.5 Hz flasher sampled at 10 ms (33 on, 33 off) comes through whole,
       two samples late. */
    d = (dash_debounce_t) {0};
    int on = 0, edges = 0;
    bool prev = false;
    for (int t = 0; t < 660; t++) {
        const bool v = dash_debounce_step(&d, (t / 33) % 2 == 1, 3);
        on += v;
        edges += v != prev;
        prev = v;
    }
    CHECK(edges == 19);
    CHECK(on >= 325 && on <= 330);

    if (s_failed == 0) {
        printf("debounce: all checks passed\n");
        return 0;
    }
    printf("debounce: %d check(s) FAILED\n", s_failed);
    return 1;
}
