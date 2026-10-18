/*
 * keymap.c -- host keys to Atari keys (atari800 AKEY codes).
 *
 * Characters mode types the character the host keyboard produced, whatever
 * the host layout; positions mode maps by key position (US layout), with
 * the host's Shift and Ctrl passed through. The editing keys follow
 * atari800's own SDL port in both modes.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "config.h"
#include "akey.h"

#include "keymap.h"

/* ASCII -> AKEY, the shift the character needs included. */
static int akey_for_char(uint32_t c)
{
    static const struct { char c; int k; } t[] = {
        {' ', AKEY_SPACE}, {'!', AKEY_EXCLAMATION}, {'"', AKEY_DBLQUOTE},
        {'#', AKEY_HASH}, {'$', AKEY_DOLLAR}, {'%', AKEY_PERCENT},
        {'&', AKEY_AMPERSAND}, {'\'', AKEY_QUOTE}, {'(', AKEY_PARENLEFT},
        {')', AKEY_PARENRIGHT}, {'*', AKEY_ASTERISK}, {'+', AKEY_PLUS},
        {',', AKEY_COMMA}, {'-', AKEY_MINUS}, {'.', AKEY_FULLSTOP},
        {'/', AKEY_SLASH}, {'0', AKEY_0}, {'1', AKEY_1}, {'2', AKEY_2},
        {'3', AKEY_3}, {'4', AKEY_4}, {'5', AKEY_5}, {'6', AKEY_6},
        {'7', AKEY_7}, {'8', AKEY_8}, {'9', AKEY_9}, {':', AKEY_COLON},
        {';', AKEY_SEMICOLON}, {'<', AKEY_LESS}, {'=', AKEY_EQUAL},
        {'>', AKEY_GREATER}, {'?', AKEY_QUESTION}, {'@', AKEY_AT},
        {'[', AKEY_BRACKETLEFT}, {'\\', AKEY_BACKSLASH}, {']', AKEY_BRACKETRIGHT},
        {'^', AKEY_CIRCUMFLEX}, {'_', AKEY_UNDERSCORE}, {'|', AKEY_BAR},
        {'a', AKEY_a}, {'b', AKEY_b}, {'c', AKEY_c}, {'d', AKEY_d}, {'e', AKEY_e},
        {'f', AKEY_f}, {'g', AKEY_g}, {'h', AKEY_h}, {'i', AKEY_i}, {'j', AKEY_j},
        {'k', AKEY_k}, {'l', AKEY_l}, {'m', AKEY_m}, {'n', AKEY_n}, {'o', AKEY_o},
        {'p', AKEY_p}, {'q', AKEY_q}, {'r', AKEY_r}, {'s', AKEY_s}, {'t', AKEY_t},
        {'u', AKEY_u}, {'v', AKEY_v}, {'w', AKEY_w}, {'x', AKEY_x}, {'y', AKEY_y},
        {'z', AKEY_z},
    };
    unsigned i;
    if (c >= 'A' && c <= 'Z')
        return akey_for_char(c - 'A' + 'a') | AKEY_SHFT;
    for (i = 0; i < sizeof t / sizeof t[0]; i++)
        if ((uint32_t)(unsigned char)t[i].c == c)
            return t[i].k;
    return AKEY_NONE;
}

int keymap_char(uint32_t c)
{
    return akey_for_char(c);
}

/* US key positions -> the Atari key in the same place (unshifted). */
static int akey_for_position(uint32_t ks)
{
    switch (ks) {
    case '-': return AKEY_LESS;
    case '=': return AKEY_GREATER;
    case '[': return AKEY_MINUS;
    case ']': return AKEY_EQUAL;
    case '\'': return AKEY_PLUS;
    case '\\': return AKEY_ASTERISK;
    case ';': return AKEY_SEMICOLON;
    case ',': return AKEY_COMMA;
    case '.': return AKEY_FULLSTOP;
    case '/': return AKEY_SLASH;
    case ' ': return AKEY_SPACE;
    default:
        if ((ks >= 'a' && ks <= 'z') || (ks >= '0' && ks <= '9'))
            return akey_for_char(ks);
        return AKEY_NONE;
    }
}

int keymap_translate(uint32_t keysym, uint32_t unicode, int shift, int ctrl,
                     int positional, int fkeys_1200, int *akey)
{
    int sc = (shift ? AKEY_SHFT : 0) | (ctrl ? AKEY_CTRL : 0);
    int k = AKEY_NONE;

    switch (keysym) {
    case A800_KEYSYM_RETURN:
    case A800_KEYSYM_KP_ENTER: k = AKEY_RETURN ^ sc; break;
    case A800_KEYSYM_BACKSPACE: k = AKEY_BACKSPACE | sc; break;
    case A800_KEYSYM_DELETE: k = shift ? AKEY_DELETE_LINE : AKEY_DELETE_CHAR; break;
    case A800_KEYSYM_INSERT: k = shift ? AKEY_INSERT_LINE : AKEY_INSERT_CHAR; break;
    case A800_KEYSYM_HOME: k = ctrl ? (AKEY_LESS | sc) : AKEY_CLEAR; break;
    case A800_KEYSYM_END:
    case A800_KEYSYM_PAGE_DOWN: k = AKEY_F2 | AKEY_SHFT; break;
    case A800_KEYSYM_PAGE_UP: k = AKEY_F1 | AKEY_SHFT; break;
    case A800_KEYSYM_TAB: k = AKEY_TAB ^ sc; break;
    case A800_KEYSYM_ESCAPE: k = AKEY_ESCAPE ^ sc; break;
    case A800_KEYSYM_CAPS_LOCK: k = AKEY_CAPSTOGGLE | sc; break;
    case '`':
    case A800_KEYSYM_LSUPER: k = AKEY_ATARI ^ sc; break;
    case A800_KEYSYM_UP: k = (fkeys_1200 ? AKEY_F1 : shift ? AKEY_MINUS : AKEY_UP) ^ sc; break;
    case A800_KEYSYM_DOWN: k = (fkeys_1200 ? AKEY_F2 : shift ? AKEY_EQUAL : AKEY_DOWN) ^ sc; break;
    case A800_KEYSYM_LEFT: k = (fkeys_1200 ? AKEY_F3 : shift ? AKEY_PLUS : AKEY_LEFT) ^ sc; break;
    case A800_KEYSYM_RIGHT: k = (fkeys_1200 ? AKEY_F4 : shift ? AKEY_ASTERISK : AKEY_RIGHT) ^ sc; break;
    case A800_KEYSYM_BREAK:
    case A800_KEYSYM_PAUSE: k = AKEY_BREAK; break;
    default:
        break;
    }
    if (k == AKEY_NONE && keysym < 0x80) {
        if (positional) {
            k = akey_for_position(keysym);
            if (k != AKEY_NONE)
                k |= sc;
        }
        else if (ctrl && keysym >= 0x20) {
            /* Ctrl+key: the key's own code, as the Atari's Ctrl does */
            k = akey_for_position(keysym);
            if (k == AKEY_NONE)
                k = akey_for_char(keysym);
            if (k != AKEY_NONE)
                k = (k & ~AKEY_SHFTCTRL) | sc;
        }
        else {
            uint32_t c = unicode;
            if (!c)
                c = shift && keysym >= 'a' && keysym <= 'z' ? keysym - 32 : keysym;
            k = akey_for_char(c);
            if (k == AKEY_NONE)
                k = akey_for_char(keysym);
        }
    }
    *akey = k;
    return k != AKEY_NONE;
}
