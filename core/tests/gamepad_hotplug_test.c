/* Gamepads come and go: an SDL virtual pad is picked up as joystick 1 when
   plugged in, drives the joystick and the console keys, drives paddles when
   the port has them, is released when unplugged while held, and gets its
   chosen port back when plugged in again. SKIPs (77) where SDL has no
   virtual joysticks or real pads are in the way. */
#include <SDL3/SDL.h>

#include "test_util.h"

#define PAD_NAME "a800test virtual pad"
#define B(a) (1u << (a))

static int failures;

static void expect(a800session *s, int port, unsigned want, unsigned console, const char *what)
{
    unsigned held = 0, con = 0;
    int i;
    for (i = 0; i < 100; i++) {
        held = a800session_buttons_held(s, port);
        con = a800session_console_held(s);
        if (held == want && con == console) {
            printf("ok: %s\n", what);
            return;
        }
        test_sleep_ms(20);
    }
    printf("FAIL: %s (held %02X want %02X, console %02X want %02X)\n", what, held, want, con, console);
    failures++;
}

static SDL_Joystick *plug(SDL_JoystickID *vid)
{
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = PAD_NAME;
    *vid = SDL_AttachVirtualJoystick(&desc);
    return *vid ? SDL_OpenJoystick(*vid) : NULL;
}

static int find_pad(a800session *s)
{
    int i;
    for (i = 0; i < a800session_gamepad_count(s); i++) {
        char name[64];
        a800session_gamepad_name(s, i, name, sizeof name);
        if (!strcmp(name, PAD_NAME))
            return i;
    }
    return -1;
}

int main(void)
{
    a800session *s = test_session("pads");
    a800session_start_opts o = {0, 0, 1};
    SDL_JoystickID vid;
    SDL_Joystick *vj;
    char ev[160];
    int i, idx, baseline;
    unsigned gen;

    CHECK(a800session_start(s, &o) == 0, "start: %s", a800session_last_error(s));
    test_sleep_ms(700);
    baseline = a800session_gamepad_count(s);
    if (baseline > 0) {
        printf("SKIP: real pads attached\n");
        a800session_free(s);
        return 77;
    }
    gen = a800session_gamepad_generation(s);
    vj = plug(&vid);
    if (!vj) {
        printf("SKIP: no virtual joysticks: %s\n", SDL_GetError());
        a800session_free(s);
        return 77;
    }
    for (i = 0; i < 100 && (idx = find_pad(s)) < 0; i++)
        test_sleep_ms(20);
    CHECK(idx >= 0 && a800session_gamepad_effective_port(s, idx) == 0, "plugged in: joystick 1");
    CHECK(a800session_gamepad_generation(s) != gen, "generation moved");
    a800session_gamepad_last_event(s, ev, sizeof ev);
    CHECK(strstr(ev, "Connected"), "event: %s", ev);

    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, true);
    expect(s, 0, B(A800_ACT_FIRE), 0, "South fires");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, false);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
    expect(s, 0, B(A800_ACT_UP), 0, "D-pad up");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, false);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTX, 20000);
    expect(s, 0, B(A800_ACT_RIGHT), 0, "the stick right");
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTX, 0);
    expect(s, 0, 0, 0, "centred");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_START, true);
    expect(s, 0, 0, B(A800_CON_START), "Start is START");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_START, false);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_BACK, true);
    expect(s, 0, 0, B(A800_CON_SELECT), "Back is SELECT");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_BACK, false);
    expect(s, 0, 0, 0, "released");

    /* chosen port 2, unplugged while holding a direction */
    a800session_gamepad_assign(s, idx, 1);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true);
    expect(s, 1, B(A800_ACT_RIGHT), 0, "assigned to joystick 2");
    gen = a800session_gamepad_generation(s);
    SDL_CloseJoystick(vj);
    SDL_DetachVirtualJoystick(vid);
    expect(s, 1, 0, 0, "unplugged while held: released");
    for (i = 0; i < 100 && a800session_gamepad_generation(s) == gen; i++)
        test_sleep_ms(20);
    a800session_gamepad_last_event(s, ev, sizeof ev);
    CHECK(strstr(ev, "Disconnected") && find_pad(s) < 0, "unplug event: %s", ev);

    /* plugged in again: joystick 2 again */
    vj = plug(&vid);
    for (i = 0; i < 100 && (idx = find_pad(s)) < 0; i++)
        test_sleep_ms(20);
    CHECK(idx >= 0 && a800session_gamepad_effective_port(s, idx) == 1, "replugged: joystick 2 again");

    SDL_CloseJoystick(vj);
    SDL_DetachVirtualJoystick(vid);
    a800session_stop(s);
    a800session_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
