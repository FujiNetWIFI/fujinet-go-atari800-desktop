/*
 * gamepad_map_test -- pad-to-port assignment on an SDL virtual joystick, on a
 * machine with four ports: a pad assigned to Port 4 drives only Port 4; the
 * assignment is remembered (the "pad_assign" setting) across an unplug, a
 * restart of the session and a whole new session; two pads are separate
 * sources, so one unplugging while holding lets go of its own hold and leaves
 * the other's; a second pad assigned to a taken port plays nothing; a trigger
 * past half travel does not hang the gamepad thread; a raw adapter (no SDL
 * mapping) drives through its raw buttons and hat; and a pad's stick turns a
 * paddle without overriding the window's knob. SKIPs (77) where SDL has no
 * virtual joysticks or real pads are in the way.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <SDL3/SDL.h>

#include "a800debug.h"
#include "test_util.h"

#define NAME_A "a800map pad A"
#define NAME_B "a800map pad B"
#define NAME_RAW "a800map raw adapter"
#define B(a) (1u << (a))

static int failures;
static char g_base[512], g_cfg[600], g_data[600];

static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static int wait_held(a800session *s, int port, unsigned want, unsigned console)
{
    int t;
    for (t = 0; t < TEST_WAIT_MS; t += 20) {
        if (a800session_buttons_held(s, port) == want && a800session_console_held(s) == console)
            return 1;
        if (t == 10000)   /* a slow runner may still get there: say what is going on */
            fprintf(stderr, "still waiting: port %d held %02X (want %02X), console %02X (want %02X)\n",
                    port, a800session_buttons_held(s, port), want, a800session_console_held(s), console);
        test_sleep_ms(20);
    }
    return 0;
}

static int stays_held(a800session *s, int port, unsigned want, int ms)
{
    int t;
    for (t = 0; t < ms; t += 20) {
        if (a800session_buttons_held(s, port) != want)
            return 0;
        test_sleep_ms(20);
    }
    return 1;
}

static int find_pad(a800session *s, const char *name)
{
    int i;
    for (i = 0; i < a800session_gamepad_count(s); i++) {
        char buf[64];
        a800session_gamepad_name(s, i, buf, sizeof buf);
        if (!strcmp(buf, name))
            return i;
    }
    return -1;
}

static int wait_pad(a800session *s, const char *name)
{
    int t, idx;
    for (t = 0; t < TEST_WAIT_MS; t += 20) {
        if ((idx = find_pad(s, name)) >= 0)
            return idx;
        test_sleep_ms(20);
    }
    return -1;
}

static int wait_gone(a800session *s, const char *name)
{
    int t;
    for (t = 0; t < TEST_WAIT_MS; t += 20) {
        if (find_pad(s, name) < 0)
            return 1;
        test_sleep_ms(20);
    }
    return 0;
}

static int wait_port(a800session *s, const char *name, int want)
{
    int t, idx;
    for (t = 0; t < TEST_WAIT_MS; t += 20) {
        idx = find_pad(s, name);
        if (idx >= 0 && a800session_gamepad_effective_port(s, idx) == want)
            return 1;
        test_sleep_ms(20);
    }
    return 0;
}

typedef struct { SDL_JoystickID id; SDL_Joystick *joy; } vpad;

static vpad plug(const char *name, int product, int raw)
{
    SDL_VirtualJoystickDesc desc;
    vpad v = { 0, NULL };
    SDL_INIT_INTERFACE(&desc);
    desc.type = raw ? SDL_JOYSTICK_TYPE_UNKNOWN : SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = 0x1209;
    desc.product_id = (Uint16)product;
    desc.naxes = raw ? 4 : SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = raw ? 6 : SDL_GAMEPAD_BUTTON_COUNT;
    desc.nhats = raw ? 1 : 0;
    desc.name = name;
    v.id = SDL_AttachVirtualJoystick(&desc);
    if (v.id)
        v.joy = SDL_OpenJoystick(v.id);
    return v;
}

static void unplug(vpad *v)
{
    if (v->joy) SDL_CloseJoystick(v->joy);
    if (v->id) SDL_DetachVirtualJoystick(v->id);
    v->joy = NULL;
    v->id = 0;
}

static a800session *open_session(void)
{
    a800session_paths p;
    memset(&p, 0, sizeof p);
    p.config_dir = g_cfg;
    p.data_dir = g_data;
    p.fujinet_lib = "";
    return a800session_new(&p);
}

static void start(a800session *s)
{
    a800session_start_opts o = {0, 0, 1};
    CHECK(a800session_start(s, &o) == 0, "start: %s", a800session_last_error(s));
}

/* A virtual pad's state is applied at SDL's next joystick update (the pad
 * thread's, every 16 ms): a release followed at once by a press is a state that
 * never changed, and no event is sent. Real hardware cannot do that, so the
 * test lets each change settle. */
static void dpad(vpad *v, int button, int on)
{
    if (!SDL_SetJoystickVirtualButton(v->joy, button, on ? true : false))
        fprintf(stderr, "SetJoystickVirtualButton(%d, %d) failed: %s\n", button, on, SDL_GetError());
    test_sleep_ms(100);
}

int main(void)
{
    a800session *s;
    vpad a, b, raw;
    char text[1024];
    int ia, ib, i;
    unsigned gen, gen2;

    /* the test holds SDL's joystick subsystem itself: the session's pad thread
     * quits it on every stop, which would destroy the virtual pads */
    CHECK(SDL_InitSubSystem(SDL_INIT_GAMEPAD), "SDL: %s", SDL_GetError());
    test_tmpdir(g_base, sizeof g_base, "padmap");
    snprintf(g_cfg, sizeof g_cfg, "%s/config", g_base);
    snprintf(g_data, sizeof g_data, "%s/data", g_base);
    s = open_session();
    test_set_pref(s, "machine", A800_M_800);      /* four ports */
    start(s);
    test_sleep_ms(700);
    if (a800session_gamepad_count(s) > 0) {
        printf("SKIP: real pads attached\n");
        a800session_free(s);
        return 77;
    }
    check(a800session_active_ports(s) == 4, "an 800 has four ports");

    a = plug(NAME_A, 0x0A, 0);
    if (!a.joy) {
        printf("SKIP: no virtual joysticks: %s\n", SDL_GetError());
        a800session_free(s);
        return 77;
    }
    ia = wait_pad(s, NAME_A);
    check(ia >= 0 && wait_port(s, NAME_A, 0), "plugged in: Port 1");

    /* assigned to Port 4: it drives Port 4 and nothing else */
    a800session_gamepad_assign(s, ia, 3);
    check(a800session_gamepad_assignment(s, ia) == 3 && wait_port(s, NAME_A, 3), "assigned to Port 4");
    dpad(&a, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 1);
    check(wait_held(s, 3, B(A800_ACT_RIGHT), 0), "D-pad right drives Port 4");
    check(a800session_buttons_held(s, 0) == 0 && a800session_buttons_held(s, 1) == 0 && a800session_buttons_held(s, 2) == 0,
          "and no other port");
    a800session_gamepad_last_event(s, text, sizeof text);
    check(strstr(text, "Connected") != NULL, "the toast says Connected");
    snprintf(text, sizeof text, "%s", a800session_get_str(s, "pad_assign", ""));
    check(strstr(text, NAME_A) != NULL, "the assignment is in the pad_assign setting");

    /* unplugged while holding: let go; replugged: Port 4 again */
    gen = a800session_gamepad_generation(s);
    unplug(&a);
    check(wait_held(s, 3, 0, 0), "unplugged while held: released");
    check(wait_gone(s, NAME_A) && a800session_gamepad_generation(s) != gen, "the generation moved");
    a800session_gamepad_last_event(s, text, sizeof text);
    check(strstr(text, "Disconnected") != NULL, "the toast says Disconnected");
    a = plug(NAME_A, 0x0A, 0);
    check(wait_pad(s, NAME_A) >= 0 && wait_port(s, NAME_A, 3), "replugged: Port 4 again");

    /* two pads are two sources on two ports */
    b = plug(NAME_B, 0x0B, 0);
    ib = wait_pad(s, NAME_B);
    check(ib >= 0 && wait_port(s, NAME_B, 0), "a second pad takes the first free port (Port 1)");
    dpad(&a, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 1);
    dpad(&b, SDL_GAMEPAD_BUTTON_DPAD_UP, 1);
    check(wait_held(s, 3, B(A800_ACT_RIGHT), 0) && wait_held(s, 0, B(A800_ACT_UP), 0), "both at once, each on its own port");
    unplug(&b);
    check(wait_gone(s, NAME_B) && wait_held(s, 0, 0, 0), "B unplugged while holding: its hold is released");
    check(stays_held(s, 3, B(A800_ACT_RIGHT), 300), "and A's hold on Port 4 is untouched");
    dpad(&a, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0);
    check(wait_held(s, 3, 0, 0), "A lets go");

    /* a second pad assigned to a port a first one has plays nothing */
    b = plug(NAME_B, 0x0B, 0);
    ib = wait_pad(s, NAME_B);
    a800session_gamepad_assign(s, ib, 3);
    check(wait_port(s, NAME_A, 3) && wait_port(s, NAME_B, -1), "B assigned to A's port: A keeps it, B plays nothing");
    dpad(&b, SDL_GAMEPAD_BUTTON_DPAD_UP, 1);
    dpad(&b, SDL_GAMEPAD_BUTTON_SOUTH, 1);
    check(stays_held(s, 3, 0, 400) && a800session_buttons_held(s, 0) == 0 && a800session_buttons_held(s, 1) == 0
          && a800session_buttons_held(s, 2) == 0, "so it holds nothing anywhere");
    dpad(&b, SDL_GAMEPAD_BUTTON_DPAD_UP, 0);
    dpad(&b, SDL_GAMEPAD_BUTTON_SOUTH, 0);
    a800session_gamepad_assign(s, ib, 1);
    check(wait_port(s, NAME_B, 1), "B moved to Port 2");
    dpad(&b, SDL_GAMEPAD_BUTTON_SOUTH, 1);
    check(wait_held(s, 1, B(A800_ACT_FIRE), 0), "and fires there");
    dpad(&b, SDL_GAMEPAD_BUTTON_SOUTH, 0);
    check(wait_held(s, 1, 0, 0), "released");
    /* held while moved to another port: lets go of the old one */
    dpad(&b, SDL_GAMEPAD_BUTTON_DPAD_LEFT, 1);
    check(wait_held(s, 1, B(A800_ACT_LEFT), 0), "B holds left on Port 2");
    ib = find_pad(s, NAME_B);
    a800session_gamepad_assign(s, ib, 2);
    check(wait_held(s, 1, 0, 0) && wait_held(s, 2, B(A800_ACT_LEFT), 0), "moved to Port 3 while held: Port 2 lets go, Port 3 holds");
    dpad(&b, SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0);
    check(wait_held(s, 2, 0, 0), "released");
    a800session_gamepad_assign(s, ib, 1);
    wait_port(s, NAME_B, 1);

    /* the triggers: a hang here is a ctest timeout */
    a800session_binding_set_button(s, A800_TARGET_CONSOLE(A800_CON_OPTION), A800_PAD_BTN_LEFT_TRIGGER, NULL, 0);
    SDL_SetJoystickVirtualAxis(a.joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    check(wait_held(s, 3, 0, B(A800_CON_OPTION)), "A's left trigger presses OPTION");
    (void)a800session_gamepad_generation(s);
    check(1, "the session lock is still free with a trigger held");
    SDL_SetJoystickVirtualAxis(a.joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0);
    check(wait_held(s, 3, 0, 0), "released");
    /* the binding was rebound while a press was held: the release still lets go of what it pressed */
    SDL_SetJoystickVirtualButton(a.joy, SDL_GAMEPAD_BUTTON_START, true);
    check(wait_held(s, 3, 0, B(A800_CON_START)), "Start is START");
    a800session_binding_clear(s, A800_TARGET_CONSOLE(A800_CON_START));
    SDL_SetJoystickVirtualButton(a.joy, SDL_GAMEPAD_BUTTON_START, false);
    check(wait_held(s, 3, 0, 0), "cleared while held: the release still let go of START");
    a800session_bindings_reset(s);

    /* the remembered assignment: across a restart of the session ... */
    a800session_gamepad_assign(s, find_pad(s, NAME_A), 3);
    gen = a800session_gamepad_generation(s);
    a800session_stop(s);
    start(s);
    check(wait_pad(s, NAME_A) >= 0 && wait_pad(s, NAME_B) >= 0, "after a restart both pads are found again");
    check(wait_port(s, NAME_A, 3) && wait_port(s, NAME_B, 1), "with their ports");
    gen2 = a800session_gamepad_generation(s);
    check(gen2 == gen, "and no hot-plug was announced for them");

    /* ... and across a whole new session (the next run) */
    a800session_stop(s);
    a800session_free(s);
    s = open_session();
    test_set_pref(s, "machine", A800_M_800);
    snprintf(text, sizeof text, "%s", a800session_get_str(s, "pad_assign", ""));
    check(text[0] != '\0', "pad_assign is on disk");
    start(s);
    check(wait_pad(s, NAME_A) >= 0 && wait_pad(s, NAME_B) >= 0, "a new session finds them");
    check(wait_port(s, NAME_A, 3) && wait_port(s, NAME_B, 1), "and gives them the ports they had");
    /* back to automatic forgets it */
    a800session_gamepad_assign(s, find_pad(s, NAME_B), -1);
    snprintf(text, sizeof text, "%s", a800session_get_str(s, "pad_assign", ""));
    check(strstr(text, NAME_B) == NULL && strstr(text, NAME_A) != NULL, "Automatic removes B's entry and keeps A's");
    check(wait_port(s, NAME_B, 0), "B is automatic again: Port 1");

    /* a raw adapter: no gamepad mapping, its buttons and hat */
    raw = plug(NAME_RAW, 0x0C, 1);
    check(wait_pad(s, NAME_RAW) >= 0, "a raw adapter is picked up");
    {
        int ir = find_pad(s, NAME_RAW), port;
        a800session_gamepad_assign(s, ir, 2);
        port = 2;
        check(wait_port(s, NAME_RAW, port), "assigned to Port 3");
        a800session_binding_set_button(s, A800_TARGET_PORT(port, A800_ACT_FIRE), A800_PAD_BTN_RAW_BASE, NULL, 0);
        SDL_SetJoystickVirtualButton(raw.joy, 0, true);
        check(wait_held(s, port, B(A800_ACT_FIRE), 0), "raw button 0 (band 64) is bindable and fires");
        SDL_SetJoystickVirtualButton(raw.joy, 0, false);
        check(wait_held(s, port, 0, 0), "released");
        SDL_SetJoystickVirtualHat(raw.joy, 0, SDL_HAT_UP);
        check(wait_held(s, port, B(A800_ACT_UP), 0), "hat 0 up is the stick up");
        SDL_SetJoystickVirtualHat(raw.joy, 0, SDL_HAT_CENTERED);
        check(wait_held(s, port, 0, 0), "centred");
        SDL_SetJoystickVirtualHat(raw.joy, 0, SDL_HAT_RIGHT);
        check(wait_held(s, port, B(A800_ACT_RIGHT), 0), "hat 0 right");
        unplug(&raw);
        check(wait_held(s, port, 0, 0), "unplugged while held: released");
        a800session_bindings_reset(s);
    }

    /* a pad's stick turns a paddle without overriding the window's knob */
    unplug(&b);
    wait_gone(s, NAME_B);
    a800session_gamepad_assign(s, find_pad(s, NAME_A), -1);
    a800session_stop(s);
    test_set_pref(s, "port1_device", A800_DEV_PADDLES);
    start(s);
    ia = wait_pad(s, NAME_A);
    check(ia >= 0 && wait_port(s, NAME_A, 0), "A is automatic again: Port 1, which has paddles");
    for (i = 0; i < TEST_WAIT_MS / 20 && a800session_paddle_value(s, 0, 0) != A800_PADDLE_CENTRE; i++)
        test_sleep_ms(20);
    check(a800session_paddle_value(s, 0, 0) == A800_PADDLE_CENTRE, "the pad's centred stick sets paddle A to the centre");
    a800session_paddle_pointer(s, 0, 0, 50, 1);
    a800session_paddle_pointer(s, 0, 0, 0, 0);
    test_sleep_ms(400);
    check(a800session_paddle_value(s, 0, 0) == 50, "a still stick does not override the window's knob");
    SDL_SetJoystickVirtualAxis(a.joy, SDL_GAMEPAD_AXIS_LEFTX, -32768);
    for (i = 0; i < TEST_WAIT_MS / 20 && a800session_paddle_value(s, 0, 0) != A800_PADDLE_MAX; i++)
        test_sleep_ms(20);
    check(a800session_paddle_value(s, 0, 0) == A800_PADDLE_MAX, "the stick full left is full left");
    SDL_SetJoystickVirtualAxis(a.joy, SDL_GAMEPAD_AXIS_LEFTX, 32767);
    for (i = 0; i < TEST_WAIT_MS / 20 && a800session_paddle_value(s, 0, 0) != 0; i++)
        test_sleep_ms(20);
    check(a800session_paddle_value(s, 0, 0) == 0, "and full right");
    SDL_SetJoystickVirtualAxis(a.joy, SDL_GAMEPAD_AXIS_LEFTX, 0);
    for (i = 0; i < TEST_WAIT_MS / 20 && a800session_paddle_value(s, 0, 0) != A800_PADDLE_CENTRE; i++)
        test_sleep_ms(20);
    check(a800session_paddle_value(s, 0, 0) == A800_PADDLE_CENTRE, "back to the centre");
    SDL_SetJoystickVirtualAxis(a.joy, SDL_GAMEPAD_AXIS_RIGHTX, -32768);
    for (i = 0; i < TEST_WAIT_MS / 20 && a800session_paddle_value(s, 0, 1) != A800_PADDLE_MAX; i++)
        test_sleep_ms(20);
    check(a800session_paddle_value(s, 0, 1) == A800_PADDLE_MAX, "the right stick turns paddle B");
    SDL_SetJoystickVirtualAxis(a.joy, SDL_GAMEPAD_AXIS_LEFTX, 32767);
    for (i = 0; i < TEST_WAIT_MS / 20 && a800session_paddle_value(s, 0, 0) != 0; i++)
        test_sleep_ms(20);
    check(a800session_paddle_value(s, 0, 0) == 0, "held full right");
    unplug(&a);
    for (i = 0; i < TEST_WAIT_MS / 20 && (a800session_paddle_value(s, 0, 0) != A800_PADDLE_MAX
                                           || a800session_paddle_value(s, 0, 1) != A800_PADDLE_MAX); i++)
        test_sleep_ms(20);
    check(a800session_paddle_value(s, 0, 0) == A800_PADDLE_MAX, "unplugged: its paddles go back to idle (full left)");

    a800session_stop(s);
    a800session_free(s);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
