/*
 * controllers_test -- the KDE Controllers window, offscreen, against a running
 * machine (the built-in OS, no ROMs, no FujiNet) and an SDL virtual gamepad:
 * hidden at start, fixed size, the mouse holding the joystick's fire button and
 * dragging the stick (and letting go when it drags off, and when the window
 * hides), the console keys, the paddles' knobs turned by a drag and the wheel,
 * a turn arrow, the CX85, a mouse's mat and the Koala Pad's surface, keys typed
 * into the machine, Map taking a key and a gamepad button (and refusing F9, F11
 * and Ctrl chords), Clear, Defaults and the target list, the keyboard's own
 * check box and a key held with the mouse, F9 hiding it, and a pad's row with its
 * port picker. Waits poll with a generous ceiling, so a slow runner is merely
 * slow. SKIPs (77) where SDL has no virtual joysticks.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QWheelEvent>
#include <QWidget>

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "a800session.h"
#include "controllers/ControllersWindow.h"

namespace {

using a800ctl::kMargin;
using a800ctl::kUnit;

int failures = 0;

void check(bool pass, const char *what)
{
    std::printf("%s: %s\n", pass ? "ok" : "FAIL", what);
    if (!pass) failures++;
}

constexpr unsigned bit(int act) { return 1u << act; }

/* Waits until port's held mask has (or has not) every bit of `mask`; the window's own timers, the
 * pad thread and the machine all need the event loop. */
void expectHeld(a800session *sess, int port, unsigned mask, bool want, const char *what)
{
    for (int tries = 0; tries < 750; tries++) {   // 15 s: slow runners
        if (((a800session_buttons_held(sess, port) & mask) == mask) == want) {
            std::printf("ok: %s\n", what);
            return;
        }
        QTest::qWait(20);
    }
    std::printf("FAIL: %s\n", what);
    failures++;
}

/* Waits for a condition with the same ceiling. */
template <class F> bool waitFor(F cond)
{
    for (int tries = 0; tries < 750; tries++) {
        if (cond()) return true;
        QTest::qWait(20);
    }
    return cond();
}

QWidget *findWindow()
{
    for (QWidget *top : QApplication::topLevelWidgets())
        if (top->objectName() == QLatin1String("controllers")) return top;
    return nullptr;
}

QWidget *child(QWidget *win, const char *name)
{
    return win->findChild<QWidget *>(QString::fromLatin1(name), Qt::FindChildrenRecursively);
}

QPushButton *findButton(QWidget *win, const char *text)
{
    for (QPushButton *button : win->findChildren<QPushButton *>())
        if (button->text() == QLatin1String(text)) return button;
    return nullptr;
}

/* the layout the window draws for what is plugged in now */
int layoutOf(a800session *sess, a800_control *out)
{
    const int machine = a800session_get_int(sess, "machine", A800_M_130XE);
    const int nports = a800session_active_ports(sess);
    int devs[A800_PORTS];
    for (int p = 0; p < A800_PORTS; ++p) devs[p] = p < nports ? a800session_port_device(sess, p) : -1;
    return a800session_controls_layout(devs, nports, a800_machine_is_xl(machine), out, A800_CONTROLS_MAX, nullptr, nullptr);
}

/* the centre, in stage pixels, of the part of this kind and label (port -1: any) */
QPoint centreOf(a800session *sess, int kind, const char *label, int port, int nth = 0)
{
    a800_control lay[A800_CONTROLS_MAX];
    const int n = layoutOf(sess, lay);
    for (int idx = 0; idx < n; idx++) {
        if (lay[idx].kind != kind) continue;
        if (label && (!lay[idx].label || std::strcmp(lay[idx].label, label) != 0)) continue;
        if (port >= 0 && lay[idx].port != port) continue;
        if (nth-- > 0) continue;
        return QPoint(int(kMargin + (lay[idx].x + lay[idx].w / 2) * kUnit),
                      int(kMargin + (lay[idx].y + lay[idx].h / 2) * kUnit));
    }
    return QPoint(-1, -1);
}

/* the part driving this target */
QPoint centreOfTarget(a800session *sess, int target)
{
    a800_control lay[A800_CONTROLS_MAX];
    const int n = layoutOf(sess, lay);
    for (int idx = 0; idx < n; idx++)
        if (lay[idx].target == target)
            return QPoint(int(kMargin + (lay[idx].x + lay[idx].w / 2) * kUnit),
                          int(kMargin + (lay[idx].y + lay[idx].h / 2) * kUnit));
    return QPoint(-1, -1);
}

/* a port's device changed: applied live, and noticed by the window's own timer */
void plug(a800session *sess, int port, int device)
{
    char key[24];
    std::snprintf(key, sizeof key, "port%d_device", port + 1);
    a800session_set_int(sess, key, device);
    a800session_apply_settings(sess);
    if (!waitFor([&] { return a800session_port_device(sess, port) == device; }))
        std::printf("FAIL: port %d did not take device %d\n", port + 1, device), failures++;
    QTest::qWait(120);
}

QComboBox *waitForPadRow(QWidget *win)
{
    QComboBox *found = nullptr;
    waitFor([&] {
        for (QComboBox *box : win->findChildren<QComboBox *>())
            if (box->count() == 5 && box->itemText(0) == QLatin1String("Automatic") &&
                box->itemText(1) == QLatin1String("Port 1")) {
                found = box;
                return true;
            }
        return false;
    });
    return found;
}

constexpr int P0 = 0, P1 = 1;
constexpr int FIRE0 = A800_TARGET_PORT(P0, A800_ACT_FIRE);

void joystickTests(a800session *sess, QWidget *win)
{
    QWidget *stage = child(win, "stage");
    check(stage != nullptr, "the stage widget exists");
    if (!stage) return;
    const QSize fixed = win->size();

    const QPoint fire = centreOf(sess, A800_CTL_ROUND_BUTTON, "FIRE", P0);
    QTest::mousePress(stage, Qt::LeftButton, Qt::NoModifier, fire);
    expectHeld(sess, P0, bit(A800_ACT_FIRE), true, "pressing the fire button holds it");
    QTest::mouseMove(stage, QPoint(2, 2));
    expectHeld(sess, P0, bit(A800_ACT_FIRE), false, "dragging off lets it go");
    QTest::mouseMove(stage, fire);
    expectHeld(sess, P0, bit(A800_ACT_FIRE), true, "and back on holds it again");
    QTest::mouseRelease(stage, Qt::LeftButton, Qt::NoModifier, fire);
    expectHeld(sess, P0, bit(A800_ACT_FIRE), false, "releasing lets go");

    /* the stick follows the pointer: eight sectors of the ring */
    const QPoint stick = centreOf(sess, A800_CTL_STICK, "STICK", P0);
    const int reach = int(1.6 * kUnit);
    QTest::mousePress(stage, Qt::LeftButton, Qt::NoModifier, stick + QPoint(reach, 0));
    expectHeld(sess, P0, bit(A800_ACT_RIGHT), true, "a press right of the shaft tilts the stick right");
    QTest::mouseMove(stage, stick + QPoint(-reach, 0));
    expectHeld(sess, P0, bit(A800_ACT_LEFT), true, "dragging to the left tilts it left");
    expectHeld(sess, P0, bit(A800_ACT_RIGHT), false, "and lets go of right");
    QTest::mouseMove(stage, stick + QPoint(int(reach * 0.75), -int(reach * 0.75)));
    expectHeld(sess, P0, bit(A800_ACT_UP) | bit(A800_ACT_RIGHT), true, "the upper right is a diagonal");
    QTest::mouseMove(stage, stick + QPoint(3, 3));
    expectHeld(sess, P0, bit(A800_ACT_UP) | bit(A800_ACT_RIGHT) | bit(A800_ACT_LEFT) | bit(A800_ACT_DOWN), false,
               "a quarter of the reach is dead centre: nothing");
    QTest::mouseMove(stage, stick + QPoint(0, -reach));
    expectHeld(sess, P0, bit(A800_ACT_UP), true, "and up is up");
    win->hide();
    expectHeld(sess, P0, bit(A800_ACT_UP), false, "hiding the window lets go of what the mouse held");
    QTest::mouseRelease(stage, Qt::LeftButton, Qt::NoModifier, stick);
    win->show();
    check(win->size() == fixed, "the window is the same size after all that");
}

void consoleTests(a800session *sess, QWidget *win)
{
    QWidget *stage = child(win, "stage");
    if (!stage) return;
    const QPoint start = centreOfTarget(sess, A800_TARGET_CONSOLE(A800_CON_START));
    QTest::mousePress(stage, Qt::LeftButton, Qt::NoModifier, start);
    check(waitFor([&] { return (a800session_console_held(sess) & bit(A800_CON_START)) != 0; }), "START is held while the mouse is down");
    QTest::mouseRelease(stage, Qt::LeftButton, Qt::NoModifier, start);
    check(waitFor([&] { return (a800session_console_held(sess) & bit(A800_CON_START)) == 0; }), "and let go on the release");
    const QPoint option = centreOfTarget(sess, A800_TARGET_CONSOLE(A800_CON_OPTION));
    QTest::mousePress(stage, Qt::LeftButton, Qt::NoModifier, option);
    QTest::mouseMove(stage, QPoint(2, 2));
    check(waitFor([&] { return (a800session_console_held(sess) & bit(A800_CON_OPTION)) == 0; }), "dragging OPTION off lets it go");
    QTest::mouseRelease(stage, Qt::LeftButton, Qt::NoModifier, QPoint(2, 2));
    check(centreOfTarget(sess, A800_TARGET_CONSOLE(A800_CON_HELP)).x() < 0, "an 800 has no HELP key on the strip");
}

void paddleTests(a800session *sess, QWidget *win)
{
    QWidget *stage = child(win, "stage");
    if (!stage) return;
    const QSize fixed = win->size();
    plug(sess, P0, A800_DEV_PADDLES);
    check(a800session_port_device(sess, P0) == A800_DEV_PADDLES, "port 1 now shows paddles");
    check(win->size() == fixed && win->minimumSize() == win->maximumSize(), "and the window did not change size");

    const QPoint dialA = centreOf(sess, A800_CTL_KNOB, "KNOB", P0, 0);
    const int up = int(1.2 * kUnit), diag = int(1.1 * kUnit);
    QTest::mousePress(stage, Qt::LeftButton, Qt::NoModifier, dialA + QPoint(0, -up));
    check(std::abs(a800session_paddle_value(sess, P0, 0) - A800_PADDLE_CENTRE) < 4, "a press straight up centres the knob");
    QTest::mouseMove(stage, dialA + QPoint(diag, diag));
    check(a800session_paddle_value(sess, P0, 0) < 12, "dragging to the lower right turns it full clockwise (value 0)");
    QTest::mouseMove(stage, dialA + QPoint(-diag, diag));
    check(a800session_paddle_value(sess, P0, 0) > A800_PADDLE_MAX - 12, "and to the lower left full counter-clockwise (228)");
    QTest::mouseRelease(stage, Qt::LeftButton, Qt::NoModifier, dialA + QPoint(-diag, diag));
    check(a800session_paddle_value(sess, P0, 0) > A800_PADDLE_MAX - 12, "a knob holds where it was left");

    /* the second paddle: the mouse wheel nudges it, clockwise (a lower value) for a scroll up */
    const QPoint dialB = centreOf(sess, A800_CTL_KNOB, "KNOB", P0, 1);
    a800session_paddle_pointer(sess, P0, 1, A800_PADDLE_CENTRE, 1);
    a800session_paddle_pointer(sess, P0, 1, A800_PADDLE_CENTRE, 0);
    QWheelEvent wheel(QPointF(dialB), QPointF(stage->mapToGlobal(dialB)), QPoint(), QPoint(0, 120), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(stage, &wheel);
    const int nudged = a800session_paddle_value(sess, P0, 1);
    check(nudged == A800_PADDLE_CENTRE - 8, "a wheel notch nudges paddle B's knob to the right");

    /* paddle A's red button: the window takes the press (the machine's trigger has no query) */
    const QPoint fireA = centreOf(sess, A800_CTL_PADDLE_FIRE, "FIRE", P0, 0);
    QTest::mousePress(stage, Qt::LeftButton, Qt::NoModifier, fireA);
    QTest::mouseRelease(stage, Qt::LeftButton, Qt::NoModifier, fireA);

    /* a held turn arrow keeps turning the knob; letting go stops it */
    a800session_paddle_pointer(sess, P0, 0, A800_PADDLE_CENTRE, 1);
    a800session_paddle_pointer(sess, P0, 0, A800_PADDLE_CENTRE, 0);
    const QPoint turnRight = centreOf(sess, A800_CTL_TURN, ">", P0, 0);
    QTest::mousePress(stage, Qt::LeftButton, Qt::NoModifier, turnRight);
    check(waitFor([&] { return a800session_paddle_value(sess, P0, 0) < A800_PADDLE_CENTRE - 10; }),
          "paddle A's right arrow, held, turns the knob right");
    QTest::mouseRelease(stage, Qt::LeftButton, Qt::NoModifier, turnRight);
    const int stopped = a800session_paddle_value(sess, P0, 0);
    QTest::qWait(300);
    check(a800session_paddle_value(sess, P0, 0) == stopped, "and letting go stops it where it is");
    plug(sess, P0, A800_DEV_JOYSTICK);
}

void pointerDeviceTests(a800session *sess, QWidget *win)
{
    QWidget *stage = child(win, "stage");
    if (!stage) return;
    const QSize fixed = win->size();

    /* the CX85: a key of the pad is held while the mouse is down (the machine takes it) */
    plug(sess, P1, A800_DEV_CX85);
    const QPoint five = centreOf(sess, A800_CTL_CX85_KEY, "5", P1);
    check(five.x() >= 0, "port 2 shows the CX85's keys");
    QTest::mousePress(stage, Qt::LeftButton, Qt::NoModifier, five);
    QTest::mouseRelease(stage, Qt::LeftButton, Qt::NoModifier, five);

    /* a mouse: its buttons and its mat */
    plug(sess, P1, A800_DEV_ST_MOUSE);
    const QPoint mat = centreOf(sess, A800_CTL_MAT, "MAT", P1);
    check(mat.x() >= 0, "an ST mouse shows a mat to drag on");
    const QPoint left = centreOf(sess, A800_CTL_POINTER_BUTTON, "LEFT", P1);
    QTest::mousePress(stage, Qt::LeftButton, Qt::NoModifier, left);
    QTest::mouseRelease(stage, Qt::LeftButton, Qt::NoModifier, left);
    QTest::mousePress(stage, Qt::LeftButton, Qt::NoModifier, mat);
    QTest::mouseMove(stage, mat + QPoint(20, 10));
    QTest::mouseRelease(stage, Qt::LeftButton, Qt::NoModifier, mat + QPoint(20, 10));
    check(a800session_pointer_device_active(sess) != 0, "a mouse-type device is fitted");

    /* the Koala Pad's surface, and the touch tablet's */
    plug(sess, P1, A800_DEV_KOALA);
    const QPoint pad = centreOf(sess, A800_CTL_PAD, "PAD", P1);
    check(pad.x() >= 0, "the Koala Pad shows its surface");
    QTest::mousePress(stage, Qt::LeftButton, Qt::NoModifier, pad);
    QTest::mouseMove(stage, pad + QPoint(15, -15));
    QTest::mouseRelease(stage, Qt::LeftButton, Qt::NoModifier, pad + QPoint(15, -15));
    plug(sess, P1, A800_DEV_LIGHTGUN);
    check(centreOf(sess, A800_CTL_NOTE, nullptr, P1).x() >= 0, "the light gun says to aim on the picture");

    check(win->size() == fixed && win->minimumSize() == win->maximumSize(), "none of that changed the window's size");
    plug(sess, P1, A800_DEV_JOYSTICK);
}

void mapTests(a800session *sess, QWidget *win, SDL_Joystick *vjoy)
{
    QPushButton *mapButton = findButton(win, "Map");
    QWidget *stage = child(win, "stage");
    check(mapButton && stage, "the Map button exists");
    if (!mapButton || !stage) return;

    mapButton->click();
    check(mapButton->text() == QLatin1String("Done"), "Map arms");
    QTest::mouseClick(stage, Qt::LeftButton, Qt::NoModifier, centreOf(sess, A800_CTL_ROUND_BUTTON, "FIRE", P0));
    QTest::keyClick(win, Qt::Key_Q);
    check(a800session_binding_get(sess, FIRE0).keysym == 'q', "the next key is bound to the picked control");
    expectHeld(sess, P0, bit(A800_ACT_FIRE), false, "a key taken by Map is not typed into the machine");

    /* a side of the stick's ring */
    const QPoint stick = centreOf(sess, A800_CTL_STICK, "STICK", P0);
    QTest::mouseClick(stage, Qt::LeftButton, Qt::NoModifier, stick + QPoint(-int(1.7 * kUnit), 0));
    QTest::keyClick(win, Qt::Key_Z);
    check(a800session_binding_get(sess, A800_TARGET_PORT(P0, A800_ACT_LEFT)).keysym == 'z',
          "clicking the left of the ring picks Left");

    /* a console key on the stage */
    QTest::mouseClick(stage, Qt::LeftButton, Qt::NoModifier, centreOfTarget(sess, A800_TARGET_CONSOLE(A800_CON_START)));
    QTest::keyClick(win, Qt::Key_J);
    check(a800session_binding_get(sess, A800_TARGET_CONSOLE(A800_CON_START)).keysym == 'j', "a console key can be picked too");

    /* F9, F11, F12 and Ctrl chords cannot be bound */
    QTest::mouseClick(stage, Qt::LeftButton, Qt::NoModifier, centreOf(sess, A800_CTL_ROUND_BUTTON, "FIRE", P0));
    QTest::keyClick(win, Qt::Key_F11);
    check(a800session_binding_get(sess, FIRE0).keysym == 'q', "F11 is refused");
    QTest::keyClick(win, Qt::Key_B, Qt::ControlModifier);
    check(a800session_binding_get(sess, FIRE0).keysym == 'q', "a Ctrl chord is refused");
    check(a800session_key_reserved(A800_KEYSYM_F9) && a800session_key_reserved(A800_KEYSYM_F12), "F9 and F12 are reserved");

    /* Clear takes the key and the pad button away */
    QPushButton *clear = findButton(win, "Clear");
    check(clear && clear->isEnabled(), "Clear is enabled with a control picked");
    if (clear) clear->click();
    const a800_binding cleared = a800session_binding_get(sess, FIRE0);
    check(cleared.keysym == 0 && cleared.button == A800_PAD_BTN_NONE, "Clear unbinds the key and the button");

    /* the list reaches a control the picture is not showing: port 2's Up */
    auto *list = win->findChild<QComboBox *>(QStringLiteral("targetList"));
    check(list && list->isEnabled() && list->count() == A800_TARGET_COUNT + 1, "the target list holds all 30 controls");
    if (list) {
        const int port2Up = A800_TARGET_PORT(P1, A800_ACT_UP);
        for (int idx = 1; idx < list->count(); idx++)
            if (list->itemData(idx).toInt() == port2Up) {
                emit list->activated(idx);
                break;
            }
        QTest::keyClick(win, Qt::Key_K);
        check(a800session_binding_get(sess, port2Up).keysym == 'k', "a control picked from the list takes the next key");
    }

    /* a gamepad button */
    QTest::mouseClick(stage, Qt::LeftButton, Qt::NoModifier, centreOf(sess, A800_CTL_ROUND_BUTTON, "FIRE", P0));
    if (vjoy) {
        SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_RIGHT_STICK, true);
        for (int tries = 0; tries < 250 && a800session_binding_get(sess, FIRE0).button != A800_PAD_BTN_RIGHT_STICK; tries++)
            QTest::qWait(20);
        SDL_SetJoystickVirtualButton(vjoy, SDL_GAMEPAD_BUTTON_RIGHT_STICK, false);
        check(a800session_binding_get(sess, FIRE0).button == A800_PAD_BTN_RIGHT_STICK,
              "the next gamepad button is bound to the picked control");
    }
    QTest::keyClick(win, Qt::Key_Escape);
    QTest::keyClick(win, Qt::Key_Escape);
    check(mapButton->text() == QLatin1String("Map"), "Escape twice leaves Map");

    /* a key bound to the fire button is typed into the machine and held there */
    char stolen[128];
    a800session_binding_set_key(sess, FIRE0, 'q', stolen, sizeof stolen);
    QTest::keyPress(win, Qt::Key_Q);
    expectHeld(sess, P0, bit(A800_ACT_FIRE), true, "a typed Q (bound to fire) holds the machine's fire");
    QTest::keyRelease(win, Qt::Key_Q);
    expectHeld(sess, P0, bit(A800_ACT_FIRE), false, "and its release lets go");

    a800session_bindings_reset(sess);
    check(a800session_binding_get(sess, FIRE0).keysym == A800_KEYSYM_KP_0, "Defaults put the key back");

    /* Show bindings: a check box, remembered */
    auto *show = win->findChild<QCheckBox *>(QStringLiteral("showBindings"));
    check(show != nullptr && !show->isChecked(), "Show bindings starts off");
    if (show) {
        show->click();
        check(a800session_get_int(sess, "controllers_show_bindings", 0) == 1, "and is remembered");
        show->click();
    }
}

void keyboardTests(a800session *sess, QWidget *win)
{
    auto *box = win->findChild<QCheckBox *>(QStringLiteral("showKeyboard"));
    QWidget *kbd = child(win, "keyboard");
    check(box && kbd, "the Keyboard check box and the keyboard exist");
    if (!box || !kbd) return;
    check(!box->isChecked() && !kbd->isVisible(), "the keyboard starts folded away on a small screen");
    const QSize before = win->size();
    box->click();
    check(kbd->isVisible() && a800session_get_int(sess, "controllers_keyboard", 0) == 1, "the check box shows it, and is remembered");
    check(win->minimumSize() == win->maximumSize(), "the window stays fixed-size with it showing");
    check(win->height() > before.height(), "and is taller");

    /* a key held with the mouse: Q's cap, found by its label in the layout the keyboard draws */
    const a800_kbd_key *keys = nullptr;
    const int n = a800session_keyboard_layout(sess, &keys);
    int qIdx = -1, qPos = -1;
    for (int i = 0; i < n; i++)
        if (keys[i].label && std::strcmp(keys[i].label, "Q") == 0) { qIdx = keys[i].index; qPos = i; }
    check(qIdx >= 0, "Q is on the keyboard");
    if (qPos >= 0) {
        const QPoint at(a800ctl::kCaseMargin + int((keys[qPos].x + keys[qPos].w / 2) * a800ctl::kKeyUnit),
                        a800ctl::kCaseMargin + int((keys[qPos].y + keys[qPos].h / 2) * a800ctl::kKeyUnit));
        QTest::mousePress(kbd, Qt::LeftButton, Qt::NoModifier, at);
        check(waitFor([&] { return a800session_keyboard_held(sess, qIdx) != 0; }), "a key is held while the mouse is down on it");
        QTest::mouseMove(kbd, QPoint(2, 2));
        check(waitFor([&] { return a800session_keyboard_held(sess, qIdx) == 0; }), "dragging off lets it go");
        QTest::mouseRelease(kbd, Qt::LeftButton, Qt::NoModifier, QPoint(2, 2));
    }
    box->click();
    check(!kbd->isVisible(), "unchecking folds it away again");
}

void padRowTests(a800session *sess, QWidget *win, const char *padName)
{
    QComboBox *box = waitForPadRow(win);
    check(box != nullptr, "a pad's row appears with Automatic and Ports 1 to 4");
    if (!box) return;
    int slot = -1;
    for (int idx = 0; idx < a800session_gamepad_count(sess); idx++) {
        char got[64];
        a800session_gamepad_name(sess, idx, got, sizeof got);
        if (std::strcmp(got, padName) == 0) slot = idx;
    }
    check(slot >= 0, "the pad is in the session");
    if (slot < 0) return;
    check(box->currentIndex() == 0, "it starts on Automatic");
    box->setCurrentIndex(3);    // Port 3
    check(a800session_gamepad_assignment(sess, slot) == 2, "choosing Port 3 assigns it");
    check(a800session_gamepad_effective_port(sess, slot) == 2, "and it drives port 3");
    box->setCurrentIndex(0);
    check(a800session_gamepad_assignment(sess, slot) == -1, "Automatic clears the assignment");
}

} // namespace

int main(int argc, char **argv)
{
    QTemporaryDir config, data;
    if (!config.isValid() || !data.isValid()) return 1;
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        std::printf("SKIP: SDL gamepad subsystem: %s\n", SDL_GetError());
        return 77;
    }
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    static const char padName[] = "a800test kde pad";
    desc.name = padName;
    const SDL_JoystickID vid = SDL_AttachVirtualJoystick(&desc);
    if (!vid) {
        std::printf("SKIP: no virtual joysticks here: %s\n", SDL_GetError());
        return 77;
    }
    SDL_Joystick *vjoy = SDL_OpenJoystick(vid);

    const QByteArray cfg = config.path().toLocal8Bit(), dat = data.path().toLocal8Bit();
    a800session_paths paths;
    std::memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg.constData();
    paths.data_dir = dat.constData();
    paths.fujinet_lib = "";      /* never the user's real FujiNet */
    a800session *sess = a800session_new(&paths);
    if (!sess) {
        std::printf("SKIP: no session\n");
        return 77;
    }
    /* an 800 (four ports, no HELP key), the built-in OS, no sound, no FujiNet */
    a800session_set_int(sess, "machine", A800_M_800);
    a800session_start_opts opts;
    a800session_default_opts(sess, &opts);
    opts.enable_fujinet = 0;
    opts.enable_audio = 0;
    opts.enable_gamepad = 1;
    if (a800session_start(sess, &opts) != 0) {
        std::printf("SKIP: the machine would not start: %s\n", a800session_last_error(sess));
        a800session_free(sess);
        return 77;
    }
    check(a800session_active_ports(sess) == 4, "an 800 has four ports");

    QWidget host;
    check(!a800ControllersVisible(), "hidden at start");
    a800ControllersToggle(&host, sess);
    check(a800ControllersVisible(), "F9 / the menu shows it");
    QWidget *win = findWindow();
    check(win != nullptr, "the window exists");
    if (win) {
        check(win->windowTitle() == QLatin1String("Controllers"), "titled Controllers");
        check(win->minimumSize() == win->maximumSize() && win->minimumSize() == win->size(),
              "fixed size: minimum == maximum == size");
        joystickTests(sess, win);
        consoleTests(sess, win);
        paddleTests(sess, win);
        pointerDeviceTests(sess, win);
        win->show();
        mapTests(sess, win, vjoy);
        keyboardTests(sess, win);
        padRowTests(sess, win, padName);
        QTest::keyClick(win, Qt::Key_F9);
        check(!win->isVisible(), "F9 hides the window");
        a800ControllersToggle(&host, sess);
        check(a800ControllersVisible(), "and the menu brings it back");
        a800ControllersToggle(&host, sess);
        check(!a800ControllersVisible(), "toggling again hides it");
    }

    a800session_stop(sess);
    if (vjoy) SDL_CloseJoystick(vjoy);
    SDL_DetachVirtualJoystick(vid);
    a800session_free(sess);
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
