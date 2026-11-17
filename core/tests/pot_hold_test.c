/*
 * pot_hold_test -- paddle and pad positions reach the machine's POT inputs and
 * HOLD there without being re-sent. (The Atari 2600 desktop had a trap here: Stella
 * only acts on an analog value that changed, so a steady position showed for one
 * frame. atari800 copies the host's value into POKEY every frame, so it does not
 * need re-sending: this test pins that, from the window's knob, the legacy
 * a800session_paddle call, the turn arrows, and an absolute pad's normalised
 * pointer.)
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "a800debug.h"
#include "test_util.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static int read_pot(a800session *s, int line)
{
    a800debug_pokey po;
    a800debug_pokey_get(a800debug_get(s), 0, &po);
    return po.pot[line];
}

/* Waits for the POT input to equal want; the machine must have run a frame. */
static int wait_pot(a800session *s, int line, int want)
{
    int t;
    for (t = 0; t < TEST_WAIT_MS; t += 20) {
        if (read_pot(s, line) == want)
            return 1;
        test_sleep_ms(20);
    }
    return 0;
}

static int stays(a800session *s, int line, int want, int ms)
{
    int t;
    for (t = 0; t < ms; t += 20) {
        if (read_pot(s, line) != want)
            return 0;
        test_sleep_ms(20);
    }
    return 1;
}

int main(void)
{
    a800session *s = test_session("pot");
    int before, after, i;

    test_set_pref(s, "machine", A800_M_800XL);
    test_set_pref(s, "port1_device", A800_DEV_PADDLES);
    test_start(s, 0);
    check(a800session_port_device(s, 0) == A800_DEV_PADDLES, "port 1 has paddles");
    check(wait_pot(s, 0, A800_PADDLE_MAX) && wait_pot(s, 1, A800_PADDLE_MAX), "idle: both paddles read full left");

    /* the window's knob: set once, held with nothing re-sent */
    a800session_paddle_pointer(s, 0, 0, 60, 1);
    check(wait_pot(s, 0, 60), "a knob drag reaches POT0");
    check(stays(s, 0, 60, 600), "and holds 600 ms later with nothing re-sent");
    a800session_paddle_pointer(s, 0, 0, 0, 0);
    check(stays(s, 0, 60, 300), "ending the drag keeps the position");
    a800session_paddle_pointer(s, 0, 0, 200, 1);
    a800session_paddle_pointer(s, 0, 0, 0, 0);
    check(wait_pot(s, 0, 200) && stays(s, 0, 200, 300), "another position");
    check(read_pot(s, 1) == A800_PADDLE_MAX, "the other paddle of the pair was not moved");

    /* the legacy call (the pointer source too) */
    a800session_paddle(s, 0, 1, 33, 0);
    check(wait_pot(s, 1, 33) && stays(s, 1, 33, 300), "a800session_paddle sets POT1 and it holds");
    check(a800session_paddle_value(s, 0, 1) == 33, "and a800session_paddle_value says so");

    /* the turn arrows: held = keeps turning (right is a lower value) */
    a800session_paddle_pointer(s, 0, 0, A800_PADDLE_CENTRE, 1);
    check(wait_pot(s, 0, A800_PADDLE_CENTRE), "centred");
    a800session_paddle_turn(s, 0, 0, 1);
    for (i = 0; i < 400 && read_pot(s, 0) > A800_PADDLE_CENTRE - 30; i++)
        test_sleep_ms(20);
    a800session_paddle_turn(s, 0, 0, 0);
    before = read_pot(s, 0);
    check(before < A800_PADDLE_CENTRE - 20, "a held right arrow turned it right");
    test_sleep_ms(300);
    after = read_pot(s, 0);
    check(after == before && a800session_paddle_value(s, 0, 0) == after, "and letting go stops it");
    a800session_paddle_turn(s, 0, 0, -1);
    for (i = 0; i < 400 && read_pot(s, 0) < before + 20; i++)
        test_sleep_ms(20);
    a800session_paddle_turn(s, 0, 0, 0);
    check(read_pot(s, 0) >= before + 20, "the left arrow turns it back");
    {
        int pinned;
        a800session_paddle_turn(s, 0, 0, -1);
        for (i = 0; i < 600 && read_pot(s, 0) < A800_PADDLE_MAX; i++)
            test_sleep_ms(20);
        a800session_paddle_turn(s, 0, 0, 0);
        pinned = read_pot(s, 0);
        check(pinned == A800_PADDLE_MAX, "it stops at its end (full left)");
    }

    /* focus loss lets go of a held arrow and keeps the position */
    a800session_paddle_turn(s, 0, 0, 1);
    test_sleep_ms(100);
    a800session_release_all(s);
    before = read_pot(s, 0);
    test_sleep_ms(300);
    check(read_pot(s, 0) == before && before < A800_PADDLE_MAX, "release_all stops a turning arrow where it is");

    a800session_stop(s);
    a800session_free(s);

    /* an absolute device's pad drawn in the window */
    s = test_session("pot-koala");
    test_set_pref(s, "machine", A800_M_800XL);
    test_set_pref(s, "port1_device", A800_DEV_KOALA);
    test_start(s, 0);
    check(wait_pot(s, 0, A800_PADDLE_MAX), "a Koala Pad with nobody touching it: 228");
    a800session_pointer_norm(s, 0.5f, 0.25f, 1, 0);
    check(wait_pot(s, 0, A800_PADDLE_CENTRE) && wait_pot(s, 1, A800_PADDLE_MAX / 4), "the pad's centre-top is (114, 57)");
    check(stays(s, 0, A800_PADDLE_CENTRE, 400), "and holds");
    a800session_pointer_norm(s, 1.0f, 1.0f, 1, 1);
    check(wait_pot(s, 0, A800_PADDLE_MAX) && wait_pot(s, 1, A800_PADDLE_MAX), "its far corner is the largest value");
    a800session_pointer_norm(s, 0.0f, 0.0f, 1, 1);
    check(wait_pot(s, 0, 1) && wait_pot(s, 1, 1), "and its near corner is clamped to 1");
    a800session_pointer_norm(s, 0.5f, 0.5f, 0, 0);
    check(wait_pot(s, 0, A800_PADDLE_MAX), "lifting off lets the POT go idle");
    a800session_stop(s);
    a800session_free(s);

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
