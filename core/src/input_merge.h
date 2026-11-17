/*
 * input_merge -- every way of holding an Atari input is a separate SOURCE, and
 * what the machine sees is their merge. So letting go of one source (a key
 * release, the window's mouse, a gamepad that is unplugged) can never cancel
 * another's hold, and two pads on one port do not fight.
 *
 * Sources: the host keyboard, the Controllers window's pointer, and for each
 * connected pad (by its stable owner number, 0 to A800_MAX_PADS - 1) its
 * buttons and its stick / D-pad separately.
 *
 * Joystick directions are merged per port with opposed pairs cancelled (up and
 * down together are neither, as on a real CX40 whose switches cannot close
 * both); fire and the console keys are a plain OR. A paddle's position is
 * owned by whoever wrote it last; a pad writes it only when the stick moves,
 * so a still stick never overrides the window's knob.
 *
 * The functions are C so the gamepad thread (gamepad_sdl.c) can call them;
 * they live in input.cpp. This header is not input.h on purpose: atari800 has
 * an input.h of its own, and macOS file systems ignore case.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef A800_INPUT_MERGE_H
#define A800_INPUT_MERGE_H

#include "a800session.h"

#ifdef __cplusplus
extern "C" {
#endif

#define A800_MAX_PADS 8

#define A800_SRC_KEYBOARD  0
#define A800_SRC_POINTER   1
#define A800_SRC_PAD0      2                         /* + owner: its buttons */
#define A800_SRC_STICK0    (A800_SRC_PAD0 + A800_MAX_PADS)   /* + owner: stick, D-pad */
#define A800_SRC_COUNT     (A800_SRC_STICK0 + A800_MAX_PADS)
#define A800_SRC_PAD(owner)   (A800_SRC_PAD0 + (owner))
#define A800_SRC_STICK(owner) (A800_SRC_STICK0 + (owner))

/* The POT value a paddle line holds with nothing driving it (full left, as a
 * paddle that is not there reads). Centre is half of A800_POT_MAX. */
#define A800_POT_MAX    A800_PADDLE_MAX
#define A800_POT_IDLE   A800_POT_MAX
#define A800_POT_CENTRE A800_PADDLE_CENTRE

/* Hold or release a joystick (target < A800_TARGET_CONSOLE(0)) or console
 * target for a source. Anything else is ignored. */
void input_press(struct a800session *s, int src, int target, int down);

/* Let go of everything a connected pad holds (its buttons' and its stick's
 * sources), its paddle fire bits, and any paddle position it wrote (back to
 * A800_POT_IDLE). Called when a pad goes away or changes port. */
void input_pad_clear(struct a800session *s, int owner);

/* A paddle line's position (0 full right .. A800_POT_MAX full left), written
 * by a source; that source then owns the line. paddle is 0 or 1 of the port's
 * pair. */
void input_pot(struct a800session *s, int src, int port, int paddle, int value);
/* A paddle's fire button for a source. */
void input_pot_fire(struct a800session *s, int src, int port, int paddle, int down);

#ifdef __cplusplus
}
#endif

#endif /* A800_INPUT_MERGE_H */
