/*
 * ControllersWindow -- see ControllersWindow.h.
 *
 * The on-screen Atari keyboard (painted from the session's table, so every
 * frontend shows the same keys for the machine: HELP on the XL/XE, F1-F4 on
 * the 1200XL), the controller ports with what is plugged into each (a stick
 * and fire for a joystick, two dials for paddles), the gamepads and their
 * ports, and Map mode.
 *
 * Keys and controls act on mouse press AND release, not "clicked": the
 * Atari scans its keyboard and samples the sticks once per frame, so a press
 * present only for the instant of a click falls between frames. A key is
 * HELD while the mouse is down on it; SHIFT and CTRL latch for the next key
 * instead (a mouse cannot hold two keys). Dragging off a key releases it;
 * losing the focus, hiding and rebuilding let go of what the mouse holds
 * here (only that: the main window releases its own keys).
 *
 * What the Atari sees held -- host keyboard, gamepads and this panel -- is
 * lit in the accent, so the panel doubles as an input tester for a new pad.
 * Keys typed with the panel active go to the Atari as in the main window.
 *
 * FIXED SIZE: kKeyUnit pixels per key unit, fixed-size controls whose Map
 * labels elide, and elided gamepad names, so neither Map mode nor a long
 * name resizes the window; only a machine or port device change does (the
 * keyboard and ports are rebuilt). A singleton, hidden rather than
 * destroyed, so a half-done remap survives closing it.
 *
 * MAP MODE: Map arms a two-step rebind -- click a joystick control or a
 * console key (RESET, OPTION, SELECT, START, HELP, BREAK), then press a key
 * or a gamepad button. Rebinding steals the key from whatever held it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ControllersWindow.h"

#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QToolTip>
#include <QtMath>
#include <QVBoxLayout>

#include <cstring>
#include <functional>
#include <utility>
#include <vector>

#include "../FujiNetWindows.h"
#include "../KeyForward.h"

namespace {

constexpr int kKeyUnit = 34;    /* pixels per key unit */
constexpr int kKeyGap = 3;      /* between key caps */
/* A joystick control: room for a two-line Map label ("Keypad Enter" over
 * "Right Shoulder") in the Map font. */
constexpr int kCellW = 84;
constexpr int kCellH = 42;
constexpr int kCellGap = 4;
constexpr int kPortW = 3 * kCellW + 2 * kCellGap;
constexpr int kPaddleMax = 228;
constexpr int kMaxPads = 8;

/* Kept across rebuilds; 228 is where the session starts them. */
int g_paddleValue[A800_PORTS][2] = { { 228, 228 }, { 228, 228 }, { 228, 228 }, { 228, 228 } };
bool g_paddleFire[A800_PORTS][2];

const QString kGray = QStringLiteral("color: gray;");

QString accentStyle()
{
    return QStringLiteral("background: %1; color: white;").arg(a800AccentColor().name());
}

/* The console keys as far as Map is concerned. The layout table carries
 * labels only, so they are matched by label. */
int consoleTarget(const char *label)
{
    static const struct { const char *label; int con; } con[] = {
        { "RESET", A800_CON_RESET }, { "OPTION", A800_CON_OPTION },
        { "SELECT", A800_CON_SELECT }, { "START", A800_CON_START },
        { "HELP", A800_CON_HELP }, { "BREAK", A800_CON_BREAK },
    };
    for (const auto &c : con)
        if (!std::strcmp(label, c.label)) return A800_TARGET_CONSOLE(c.con);
    return -1;
}

/* Map mode on a control: the key over the gamepad button. */
QString bindingText(a800session *s, int target)
{
    const a800_binding b = a800session_binding_get(s, target);
    char key[48];
    a800session_keysym_name(b.keysym, key, sizeof key);
    QString text = key[0] ? QString::fromUtf8(key) : QStringLiteral("—");
    if (b.button != A800_PAD_BTN_NONE)
        text += QLatin1Char('\n') + QString::fromUtf8(a800_pad_button_name(b.button));
    return text;
}

QString bindingTip(a800session *s, int target)
{
    const a800_binding b = a800session_binding_get(s, target);
    char key[48];
    a800session_keysym_name(b.keysym, key, sizeof key);
    QString tip = QStringLiteral("%1: %2").arg(QString::fromUtf8(a800_target_name(target)),
                                               key[0] ? QString::fromUtf8(key) : QStringLiteral("no key"));
    if (b.button != A800_PAD_BTN_NONE)
        tip += QStringLiteral(", ") + QString::fromUtf8(a800_pad_button_name(b.button));
    return tip;
}

QFont scaledFont(QFont f, qreal scale, bool bold = false)
{
    f.setPointSizeF((f.pointSizeF() > 0 ? f.pointSizeF() : 10) * scale);
    f.setBold(bold);
    return f;
}

/* Lines centred in r, the font shrunk until they fit. */
void drawFitted(QPainter &p, const QRectF &r, const QString &text, QFont f)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (;;) {
        const QFontMetricsF fm(f);
        qreal w = 0;
        for (const QString &l : lines) w = qMax(w, fm.horizontalAdvance(l));
        if ((w <= r.width() && fm.lineSpacing() * lines.size() <= r.height() + 2)
            || f.pointSizeF() <= 5.5)
            break;
        f.setPointSizeF(f.pointSizeF() - 0.5);
    }
    p.setFont(f);
    p.drawText(r, Qt::AlignCenter, text);
}

/* ---- ElidedLabel ------------------------------------------------------------ */

/* A label that never widens the window: one line elides, more wrap into a
 * fixed height. */
class ElidedLabel : public QWidget {
public:
    explicit ElidedLabel(bool dim, int lines = 1) : m_dim(dim), m_lines(lines)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }
    void setText(const QString &text)
    {
        if (text == m_text) return;
        m_text = text;
        update();
    }
    QSize sizeHint() const override { return QSize(0, fontMetrics().lineSpacing() * m_lines); }
    QSize minimumSizeHint() const override { return sizeHint(); }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setPen(m_dim ? QColor(Qt::gray) : palette().color(QPalette::WindowText));
        if (m_lines > 1)
            p.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, m_text);
        else
            p.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter,
                       fontMetrics().elidedText(m_text, Qt::ElideRight, width()));
    }
private:
    QString m_text;
    bool m_dim;
    int m_lines;
};

/* ---- PadButton -------------------------------------------------------------- */

/* A fixed-size button that reports press and release, not "clicked": a
 * stick direction is HELD, since the Atari samples it once per frame. */
class PadButton : public QPushButton {
public:
    PadButton(const QString &face, int target, std::function<void(PadButton *, bool)> onPress)
        : QPushButton(face), m_face(face), m_target(target), m_onPress(std::move(onPress))
    {
        setFocusPolicy(Qt::NoFocus);
        setFixedSize(kCellW, kCellH);
        m_baseFont = font();
    }
    int target() const { return m_target; }
    void setHeld(bool held)
    {
        if (held == m_held) return;
        m_held = held;
        setStyleSheet(held ? accentStyle() : QString());
    }
    /* Map mode: the binding, each line elided to the button. */
    void showBinding(a800session *s, bool isTarget)
    {
        setFont(scaledFont(m_baseFont, 0.85));
        QStringList lines = bindingText(s, m_target).split(QLatin1Char('\n'));
        for (QString &l : lines) l = fontMetrics().elidedText(l, Qt::ElideRight, width() - 8);
        setText(lines.join(QLatin1Char('\n')));
        setToolTip(bindingTip(s, m_target));
        setHeld(isTarget);
    }
    void showFace()
    {
        setFont(m_baseFont);
        setText(m_face);
        setToolTip(QString());
        setHeld(false);
    }
protected:
    /* Not QPushButton's own: its pressed look would hide the held fill. */
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && !m_down) {
            m_down = true;
            m_onPress(this, true);
        }
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && m_down) {
            m_down = false;
            m_onPress(this, false);
        }
    }
    /* Dragging off a button must release it. */
    void leaveEvent(QEvent *e) override
    {
        if (m_down) {
            m_down = false;
            m_onPress(this, false);
        }
        QPushButton::leaveEvent(e);
    }
private:
    QString m_face;
    int m_target;
    std::function<void(PadButton *, bool)> m_onPress;
    QFont m_baseFont;
    bool m_down = false;
    bool m_held = false;
};

/* ---- KeyboardWidget --------------------------------------------------------- */

/* The Atari keyboard, painted from a800session_keyboard_layout in key
 * units. It reports presses by table position; the window holds the key. */
class KeyboardWidget : public QWidget {
public:
    KeyboardWidget(a800session *session, std::function<void(int, bool)> onKey)
        : m_session(session), m_onKey(std::move(onKey))
    {
        setFocusPolicy(Qt::NoFocus);
    }

    /* The machine changed: its keys. Copied, since the session's table is
     * one static buffer. */
    void load()
    {
        const a800_kbd_key *keys = nullptr;
        const int n = a800session_keyboard_layout(m_session, &keys);
        m_keys.assign(keys, keys + qMax(0, n));
        m_targets.clear();
        float cols = 0, rows = 0;
        for (const a800_kbd_key &k : m_keys) {
            m_targets.push_back(consoleTarget(k.label));
            cols = qMax(cols, k.x + k.w);
            rows = qMax(rows, k.y + k.h);
        }
        m_held.assign(m_keys.size(), 0);
        m_pressed = -1;
        setFixedSize(qCeil(cols * kKeyUnit), qCeil(rows * kKeyUnit));
        update();
    }
    const a800_kbd_key &key(int i) const { return m_keys[(size_t)i]; }
    int target(int i) const { return m_targets[(size_t)i]; }
    void setMapState(int state)
    {
        m_mapState = state;
        update();
    }
    /* What the Atari sees held (and the key the mouse holds); repaints only
     * on a change. */
    void pollHeld(int mouseIndex)
    {
        bool changed = false;
        for (size_t i = 0; i < m_keys.size(); ++i) {
            const char on = m_keys[i].index == mouseIndex
                            || a800session_keyboard_held(m_session, m_keys[i].index);
            if (on != m_held[i]) {
                m_held[i] = on;
                changed = true;
            }
        }
        if (changed) update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QPalette pal = palette();
        const QColor accent = a800AccentColor();
        const QFont keyFont = scaledFont(font(), 0.75, true);
        const QFont smallFont = scaledFont(font(), 0.65);
        const bool mapping = m_mapState != -2;
        for (int i = 0; i < (int)m_keys.size(); ++i) {
            const a800_kbd_key &k = m_keys[(size_t)i];
            const QRectF r = keyRect(i);
            const int t = m_targets[(size_t)i];
            /* Map mode: the target filled, the keys it cannot remap greyed */
            const bool fill = mapping ? t >= 0 && t == m_mapState : m_held[(size_t)i] != 0;
            const bool grey = mapping && t < 0;
            p.setPen(pal.color(QPalette::Mid));
            p.setBrush(fill ? accent : pal.color(grey ? QPalette::Window : QPalette::Button));
            p.drawRoundedRect(r, 4, 4);
            p.setPen(fill ? QColor(Qt::white)
                          : pal.color(grey ? QPalette::Disabled : QPalette::Active, QPalette::ButtonText));
            const QRectF in = r.adjusted(2, 1, -2, -1);
            if (mapping && t >= 0) {
                drawFitted(p, in, bindingText(m_session, t), smallFont);
            } else if (k.shifted) {
                const QRectF top(in.x(), in.y(), in.width(), in.height() / 2);
                const QRectF bottom(in.x(), in.y() + in.height() / 2, in.width(), in.height() / 2);
                drawFitted(p, top, QString::fromUtf8(k.shifted), smallFont);
                drawFitted(p, bottom, QString::fromUtf8(k.label), keyFont);
            } else if (k.label[0]) {
                drawFitted(p, in, QString::fromUtf8(k.label), keyFont);
            }
        }
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) return;
        const int i = keyAt(e->position());
        if (i < 0) return;
        m_pressed = i;
        m_onKey(i, true);
    }

    /* Dragging off a key must release it. */
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (m_pressed >= 0 && !keyRect(m_pressed).contains(e->position())) release();
    }

    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton) release();
    }

    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::ToolTip) {
            auto *he = static_cast<QHelpEvent *>(e);
            const int i = keyAt(he->pos());
            QString tip;
            if (i >= 0 && m_mapState != -2 && m_targets[(size_t)i] >= 0)
                tip = bindingTip(m_session, m_targets[(size_t)i]);
            else if (i >= 0 && m_mapState == -2
                     && (!std::strcmp(m_keys[(size_t)i].label, "SHIFT")
                         || !std::strcmp(m_keys[(size_t)i].label, "CTRL")))
                tip = QStringLiteral("Latches for the next key");
            if (tip.isEmpty()) QToolTip::hideText();
            else QToolTip::showText(he->globalPos(), tip, this, keyRect(i).toRect());
            return true;
        }
        return QWidget::event(e);
    }

private:
    QRectF keyRect(int i) const
    {
        const a800_kbd_key &k = m_keys[(size_t)i];
        return QRectF(k.x * kKeyUnit + kKeyGap / 2.0, k.y * kKeyUnit + kKeyGap / 2.0,
                      k.w * kKeyUnit - kKeyGap, k.h * kKeyUnit - kKeyGap);
    }
    int keyAt(const QPointF &pos) const
    {
        for (int i = 0; i < (int)m_keys.size(); ++i)
            if (keyRect(i).contains(pos)) return i;
        return -1;
    }
    void release()
    {
        if (m_pressed < 0) return;
        const int i = m_pressed;
        m_pressed = -1;
        m_onKey(i, false);
    }

    a800session *m_session;
    std::function<void(int, bool)> m_onKey;
    std::vector<a800_kbd_key> m_keys;
    std::vector<int> m_targets;     /* each key's console target (Map), or -1 */
    std::vector<char> m_held;
    int m_mapState = -2;
    int m_pressed = -1;             /* table position the mouse is on, or -1 */
};

/* ---- ControllersWindow ------------------------------------------------------ */

class ControllersWindow : public QWidget {
public:
    ControllersWindow(a800session *session, QWidget *parent);
    void present();
    void refresh();

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    void changeEvent(QEvent *e) override;
    /* Tab is the Atari's TAB, not focus traversal. */
    bool focusNextPrevChild(bool) override { return false; }

private:
    int machineNow() const;
    int portsNow(int machine) const;
    bool layoutChanged() const;
    void rebuild();
    QWidget *buildPort(int port);
    QWidget *buildJoystick(int port);
    QWidget *buildPaddles(int port);
    PadButton *control(const QString &face, int target);
    void onControl(int target, bool down);
    void onKey(int pos, bool down);
    void onPaddleFire(int port, int p, PadButton *b, bool down);
    void paddleSend(int port, int p);
    void letGo();
    void setMapState(int state);
    void refreshLabels();
    void pollCapture();
    void pollHeld();
    void pollSlow();
    void rebuildPads();
    void refreshPadPorts();

    a800session *m_session;
    QVBoxLayout *m_root = nullptr;
    KeyboardWidget *m_keyboard = nullptr;
    QWidget *m_ports = nullptr;
    std::vector<PadButton *> m_controls;            /* the joystick controls */
    QSlider *m_paddleDial[A800_PORTS][2] = {};
    PadButton *m_paddleFireButton[A800_PORTS][2] = {};
    QVBoxLayout *m_padBox = nullptr;
    std::vector<QWidget *> m_padRows;
    std::vector<QLabel *> m_padEff;                 /* each pad's "drives port N" */
    unsigned m_padGen = ~0u;
    QPushButton *m_mapButton = nullptr;
    ElidedLabel *m_hint = nullptr;
    QTimer m_captureTimer;
    QTimer m_heldTimer;
    QTimer m_slowTimer;
    /* what the keyboard and the ports were built for */
    int m_builtMachine = -1;
    int m_builtPorts = 0;
    int m_builtDev[A800_PORTS] = {};
    /* what the mouse holds here */
    int m_mouseKey = -1;        /* on-screen keyboard index */
    int m_mouseTarget = -1;     /* joystick target */
    uint32_t m_swallow = 0;     /* the key Map took: its repeats and release too */
    /* -2 idle, -1 armed and waiting for a target, >=0 waiting for a key or
     * gamepad button. */
    int m_mapState = -2;
};

ControllersWindow::ControllersWindow(a800session *session, QWidget *parent)
    /* A dialog-type window, so a tiling compositor floats it: it is a
     * panel of fixed-size keys and buttons. */
    : QWidget(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint
                      | Qt::CustomizeWindowHint), m_session(session)
{
    setObjectName(QStringLiteral("controllers"));
    setWindowTitle(QStringLiteral("Controllers"));
    setFocusPolicy(Qt::StrongFocus);

    m_root = new QVBoxLayout(this);
    m_root->setSizeConstraint(QLayout::SetFixedSize);
    m_keyboard = new KeyboardWidget(m_session, [this](int pos, bool down) { onKey(pos, down); });
    m_root->addWidget(m_keyboard, 0, Qt::AlignHCenter);
    /* the ports go in at 1 (rebuild) */

    /* Gamepads: one row per pad, rebuilt as they come and go */
    auto *pads = new QGroupBox(QStringLiteral("Gamepads"));
    m_padBox = new QVBoxLayout(pads);
    m_root->addWidget(pads);

    auto *maprow = new QHBoxLayout;
    m_mapButton = new QPushButton(QStringLiteral("Map"));
    m_mapButton->setFocusPolicy(Qt::NoFocus);
    m_mapButton->setFixedWidth(qMax(m_mapButton->sizeHint().width(),
                                    m_mapButton->fontMetrics().horizontalAdvance(QStringLiteral("Done")) + 32));
    connect(m_mapButton, &QPushButton::clicked, this, [this] { setMapState(m_mapState == -2 ? -1 : -2); });
    auto *defaults = new QPushButton(QStringLiteral("Defaults"));
    defaults->setFocusPolicy(Qt::NoFocus);
    connect(defaults, &QPushButton::clicked, this, [this] {
        a800session_bindings_reset(m_session);
        refreshLabels();
    });
    m_hint = new ElidedLabel(true, 2);
    maprow->addWidget(m_mapButton);
    maprow->addWidget(defaults);
    maprow->addWidget(m_hint, 1);
    m_root->addLayout(maprow);

    connect(&m_captureTimer, &QTimer::timeout, this, [this] { pollCapture(); });
    connect(&m_heldTimer, &QTimer::timeout, this, [this] { pollHeld(); });
    connect(&m_slowTimer, &QTimer::timeout, this, [this] { pollSlow(); });
    setMapState(-2);
    rebuild();
}

/* ---- the keyboard and the ports ------------------------------------------------ */

int ControllersWindow::machineNow() const
{
    const int m = a800session_get_int(m_session, "machine", A800_M_130XE);
    return m >= 0 && m < A800_M_COUNT ? m : A800_M_130XE;
}

/* The 400 and 800 have four ports, the XL/XE two (four with a MultiJoy). */
int ControllersWindow::portsNow(int machine) const
{
    return a800_machine_is_xl(machine) && !a800session_get_int(m_session, "multijoy", 0)
           ? 2 : A800_PORTS;
}

bool ControllersWindow::layoutChanged() const
{
    const int m = machineNow(), n = portsNow(m);
    if (m != m_builtMachine || n != m_builtPorts) return true;
    for (int port = 0; port < n; ++port)
        if (a800session_port_device(m_session, port) != m_builtDev[port]) return true;
    return false;
}

void ControllersWindow::rebuild()
{
    letGo();
    m_controls.clear();
    for (int port = 0; port < A800_PORTS; ++port)
        for (int p = 0; p < 2; ++p) {
            m_paddleDial[port][p] = nullptr;
            m_paddleFireButton[port][p] = nullptr;
        }
    delete m_ports;

    m_builtMachine = machineNow();
    m_builtPorts = portsNow(m_builtMachine);
    m_keyboard->load();
    /* two ports to a row: four side by side would be twice the keyboard */
    m_ports = new QWidget;
    auto *grid = new QGridLayout(m_ports);
    grid->setContentsMargins(0, 0, 0, 0);
    for (int port = 0; port < m_builtPorts; ++port) {
        m_builtDev[port] = a800session_port_device(m_session, port);
        grid->addWidget(buildPort(port), port / 2, port % 2);
    }
    m_root->insertWidget(1, m_ports, 0, Qt::AlignHCenter);
    refreshLabels();
}

QWidget *ControllersWindow::buildPort(int port)
{
    const int dev = a800session_port_device(m_session, port);
    auto *box = new QGroupBox(QStringLiteral("Port %1").arg(port + 1));
    auto *v = new QVBoxLayout(box);
    auto *device = new QLabel(QString::fromUtf8(a800_device_name(dev)));
    device->setStyleSheet(kGray);
    device->setAlignment(Qt::AlignCenter);
    v->addWidget(device);
    if (dev == A800_DEV_JOYSTICK) {
        v->addWidget(buildJoystick(port), 0, Qt::AlignHCenter);
    } else if (dev == A800_DEV_PADDLES) {
        v->addWidget(buildPaddles(port), 0, Qt::AlignHCenter);
    } else {
        auto *hint = new QLabel(dev == A800_DEV_CX85 ? QStringLiteral("Type on the host's numeric keypad.")
                                : dev == A800_DEV_NONE ? QString()
                                : QStringLiteral("The pointer over the picture drives it."));
        hint->setStyleSheet(kGray);
        hint->setWordWrap(true);
        hint->setAlignment(Qt::AlignCenter);
        hint->setFixedWidth(kPortW);
        v->addWidget(hint, 1);
    }
    v->addStretch();
    return box;
}

/* The stick's four directions around its fire button. */
QWidget *ControllersWindow::buildJoystick(int port)
{
    auto *w = new QWidget;
    auto *grid = new QGridLayout(w);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(kCellGap);
    grid->addWidget(control(QStringLiteral("▲"), A800_TARGET_PORT(port, A800_ACT_UP)), 0, 1);
    grid->addWidget(control(QStringLiteral("◀"), A800_TARGET_PORT(port, A800_ACT_LEFT)), 1, 0);
    grid->addWidget(control(QStringLiteral("Fire"), A800_TARGET_PORT(port, A800_ACT_FIRE)), 1, 1);
    grid->addWidget(control(QStringLiteral("▶"), A800_TARGET_PORT(port, A800_ACT_RIGHT)), 1, 2);
    grid->addWidget(control(QStringLiteral("▼"), A800_TARGET_PORT(port, A800_ACT_DOWN)), 2, 1);
    return w;
}

/* A pair of paddles: a dial (228 at the left .. 0 at the right, as the POT
 * counters read: clockwise lowers it) and a fire button each, numbered as
 * Atari BASIC's PADDLE() does. */
QWidget *ControllersWindow::buildPaddles(int port)
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);
    for (int p = 0; p < 2; ++p) {
        auto *name = new QLabel(QStringLiteral("Paddle %1").arg(port * 2 + p));
        name->setStyleSheet(kGray);
        auto *dial = new QSlider(Qt::Horizontal);
        dial->setRange(0, kPaddleMax);
        dial->setInvertedAppearance(true);
        dial->setValue(g_paddleValue[port][p]);
        dial->setFocusPolicy(Qt::NoFocus);
        dial->setFixedWidth(kPortW - kCellW - kCellGap);
        connect(dial, &QSlider::valueChanged, this, [this, port, p](int value) {
            g_paddleValue[port][p] = value;
            paddleSend(port, p);
        });
        auto *fire = new PadButton(QStringLiteral("Fire"), -1, [this, port, p](PadButton *b, bool down) {
            onPaddleFire(port, p, b, down);
        });
        fire->setFixedSize(kCellW, kCellH - 8);
        auto *row = new QHBoxLayout;
        row->setSpacing(kCellGap);
        row->addWidget(dial);
        row->addWidget(fire);
        v->addWidget(name);
        v->addLayout(row);
        m_paddleDial[port][p] = dial;
        m_paddleFireButton[port][p] = fire;
    }
    return w;
}

PadButton *ControllersWindow::control(const QString &face, int target)
{
    auto *b = new PadButton(face, target, [this](PadButton *pb, bool down) { onControl(pb->target(), down); });
    m_controls.push_back(b);
    return b;
}

/* ---- pressing ------------------------------------------------------------------ */

void ControllersWindow::onControl(int target, bool down)
{
    if (m_mapState != -2) {
        if (down) setMapState(target);    /* armed: picks (or changes) the target */
        return;
    }
    if (down) {
        letGo();
        m_mouseTarget = target;
        a800session_press(m_session, target, 1);
    } else if (m_mouseTarget == target) {
        m_mouseTarget = -1;
        a800session_press(m_session, target, 0);
    }
    pollHeld();
}

void ControllersWindow::onKey(int pos, bool down)
{
    const int index = m_keyboard->key(pos).index;
    if (m_mapState != -2) {
        if (down && m_keyboard->target(pos) >= 0) setMapState(m_keyboard->target(pos));
        return;
    }
    if (down) {
        letGo();
        m_mouseKey = index;
        a800session_keyboard_press(m_session, index, 1);
    } else if (m_mouseKey == index) {
        m_mouseKey = -1;
        a800session_keyboard_press(m_session, index, 0);
    }
    pollHeld();
}

void ControllersWindow::paddleSend(int port, int p)
{
    a800session_paddle(m_session, port, p, g_paddleValue[port][p], g_paddleFire[port][p]);
}

void ControllersWindow::onPaddleFire(int port, int p, PadButton *b, bool down)
{
    if (down && m_mapState != -2) return;
    if (g_paddleFire[port][p] == down) return;
    g_paddleFire[port][p] = down;
    paddleSend(port, p);
    b->setHeld(down);
}

/* Lets go of everything the mouse holds in this panel. */
void ControllersWindow::letGo()
{
    if (m_mouseKey >= 0) {
        a800session_keyboard_press(m_session, m_mouseKey, 0);
        m_mouseKey = -1;
    }
    if (m_mouseTarget >= 0) {
        a800session_press(m_session, m_mouseTarget, 0);
        m_mouseTarget = -1;
    }
    for (int port = 0; port < A800_PORTS; ++port)
        for (int p = 0; p < 2; ++p)
            if (g_paddleFire[port][p]) {
                g_paddleFire[port][p] = false;
                paddleSend(port, p);
                if (m_paddleFireButton[port][p]) m_paddleFireButton[port][p]->setHeld(false);
            }
}

/* ---- Map mode ------------------------------------------------------------------ */

void ControllersWindow::setMapState(int state)
{
    m_mapState = state;
    if (state == -2) {
        m_captureTimer.stop();
        a800session_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Map"));
        m_mapButton->setStyleSheet(QString());
        m_hint->setText(QStringLiteral("Click and hold to press. SHIFT and CTRL latch for the next key."));
    } else if (state == -1) {
        m_captureTimer.stop();
        a800session_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Done"));
        m_mapButton->setStyleSheet(accentStyle());
        m_hint->setText(QStringLiteral("Click a joystick control or console key to remap"));
    } else {
        letGo();
        m_hint->setText(QStringLiteral("Press a key or gamepad button for %1")
                            .arg(QString::fromUtf8(a800_target_name(state))));
        a800session_gamepad_capture_begin(m_session);
        m_captureTimer.start(50);
    }
    refreshLabels();
}

void ControllersWindow::pollCapture()
{
    int button;
    if (m_mapState < 0) { m_captureTimer.stop(); return; }
    if (!a800session_gamepad_capture_poll(m_session, &button)) return;
    char stolen[128];
    a800session_binding_set_button(m_session, m_mapState, button, stolen, sizeof stolen);
    const QString name = QString::fromUtf8(a800_pad_button_name(button));
    setMapState(-1);    /* stay armed: remapping several in a row is normal */
    m_hint->setText(stolen[0] ? QStringLiteral("Bound %1 (was %2)").arg(name, QString::fromUtf8(stolen))
                              : QStringLiteral("Bound %1").arg(name));
}

/* In Map mode each remappable control shows what is bound to it, so
 * choosing what to change does not take remembering the whole table. */
void ControllersWindow::refreshLabels()
{
    const bool mapping = m_mapState != -2;
    for (PadButton *b : m_controls) {
        if (mapping) b->showBinding(m_session, b->target() == m_mapState);
        else b->showFace();
    }
    for (int port = 0; port < A800_PORTS; ++port)
        for (int p = 0; p < 2; ++p) {
            if (m_paddleDial[port][p]) m_paddleDial[port][p]->setEnabled(!mapping);
            if (m_paddleFireButton[port][p]) m_paddleFireButton[port][p]->setEnabled(!mapping);
        }
    m_keyboard->setMapState(m_mapState);
    pollHeld();
}

/* ---- polling ------------------------------------------------------------------- */

/* What the Atari sees held, from every source, lit (~30 Hz). */
void ControllersWindow::pollHeld()
{
    if (m_mapState != -2) return;
    unsigned held[A800_PORTS];
    for (int port = 0; port < A800_PORTS; ++port)
        held[port] = a800session_buttons_held(m_session, port);
    for (PadButton *b : m_controls) {
        const int t = b->target();
        b->setHeld(t == m_mouseTarget
                   || ((held[t / A800_ACT_PER_PORT] >> (t % A800_ACT_PER_PORT)) & 1));
    }
    m_keyboard->pollHeld(m_mouseKey);
}

/* Hot-plugged pads, auto-assignment changes, and the machine or a port's
 * device changed in Preferences. */
void ControllersWindow::pollSlow()
{
    const unsigned gen = a800session_gamepad_generation(m_session);
    if (gen != m_padGen) {
        m_padGen = gen;
        rebuildPads();
    } else {
        refreshPadPorts();
    }
    if (layoutChanged()) rebuild();
}

/* ---- gamepads ------------------------------------------------------------------ */

void ControllersWindow::refreshPadPorts()
{
    for (int i = 0; i < (int)m_padEff.size(); ++i) {
        const int eff = a800session_gamepad_effective_port(m_session, i);
        const QString text = eff >= 0 ? QStringLiteral("drives port %1").arg(eff + 1)
                                      : QStringLiteral("drives nothing");
        if (m_padEff[(size_t)i]->text() != text) m_padEff[(size_t)i]->setText(text);
    }
}

/* One row per pad: its name, the port it drives and the port it is set to. */
void ControllersWindow::rebuildPads()
{
    for (QWidget *w : m_padRows) delete w;
    m_padRows.clear();
    m_padEff.clear();
    const int n = a800session_gamepad_count(m_session);
    if (n == 0) {
        auto *none = new QLabel(QStringLiteral("No gamepads connected"));
        none->setStyleSheet(kGray);
        m_padBox->addWidget(none);
        m_padRows.push_back(none);
        return;
    }
    for (int i = 0; i < n && i < kMaxPads; ++i) {
        char name[128];
        a800session_gamepad_name(m_session, i, name, sizeof name);
        auto *row = new QWidget;
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        auto *label = new ElidedLabel(false);
        label->setText(QString::fromUtf8(name));
        label->setToolTip(QString::fromUtf8(name));
        auto *eff = new QLabel;
        eff->setStyleSheet(kGray);
        /* 0 = automatic (connection order), 1-4 = that port */
        auto *box = new QComboBox;
        box->setFocusPolicy(Qt::NoFocus);
        box->addItems({ QStringLiteral("Automatic"), QStringLiteral("Port 1"), QStringLiteral("Port 2"),
                        QStringLiteral("Port 3"), QStringLiteral("Port 4") });
        const int assign = a800session_gamepad_assignment(m_session, i);
        box->setCurrentIndex(assign < 0 || assign >= A800_PORTS ? 0 : assign + 1);
        connect(box, &QComboBox::currentIndexChanged, this, [this, i](int idx) {
            a800session_gamepad_assign(m_session, i, idx <= 0 ? -1 : idx - 1);
            refreshPadPorts();
        });
        h->addWidget(label, 1);
        h->addWidget(eff);
        h->addWidget(box);
        m_padBox->addWidget(row);
        m_padRows.push_back(row);
        m_padEff.push_back(eff);
    }
    refreshPadPorts();
}

/* ---- the window ----------------------------------------------------------------- */

void ControllersWindow::present()
{
    if (layoutChanged()) rebuild();
    m_padGen = a800session_gamepad_generation(m_session);
    rebuildPads();
    show();
    raise();
}

void ControllersWindow::refresh()
{
    if (isVisible()) rebuild();
    else m_builtMachine = -1;   /* rebuilt when next shown */
}

void ControllersWindow::showEvent(QShowEvent *e)
{
    pollHeld();
    m_heldTimer.start(33);
    m_slowTimer.start(250);
    QWidget::showEvent(e);
}

void ControllersWindow::hideEvent(QHideEvent *e)
{
    m_heldTimer.stop();
    m_slowTimer.stop();
    setMapState(-2);
    letGo();
    QWidget::hideEvent(e);
}

/* Only what this panel's mouse holds: the main window releases its own. */
void ControllersWindow::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::ActivationChange && !isActiveWindow()) letGo();
    QWidget::changeEvent(e);
}

void ControllersWindow::keyPressEvent(QKeyEvent *e)
{
    const uint32_t ks = a800KeysymFromQt(e);
    if (m_swallow && ks == m_swallow) return;
    if (m_mapState >= 0) {
        if (e->isAutoRepeat() || !ks) return;
        char stolen[128], name[48];
        a800session_binding_set_key(m_session, m_mapState, ks, stolen, sizeof stolen);
        a800session_keysym_name(ks, name, sizeof name);
        m_swallow = ks;
        setMapState(-1);    /* stay armed: remapping several in a row is normal */
        m_hint->setText(stolen[0] ? QStringLiteral("Bound %1 (was %2)").arg(QString::fromUtf8(name), QString::fromUtf8(stolen))
                                  : QStringLiteral("Bound %1").arg(QString::fromUtf8(name)));
        return;
    }
    /* Otherwise what the main window does with it; a system action goes
     * to the main window's queue, as a gamepad's does. */
    const int sa = a800KeySysaction(m_session, e);
    if (sa >= 0) {
        a800session_sysaction_post(m_session, sa);
        return;
    }
    if (!a800ForwardKey(m_session, e, true)) QWidget::keyPressEvent(e);
}

void ControllersWindow::keyReleaseEvent(QKeyEvent *e)
{
    const uint32_t ks = a800KeysymFromQt(e);
    if (m_swallow && ks == m_swallow) {
        if (!e->isAutoRepeat()) m_swallow = 0;
        return;
    }
    if (!a800ForwardKey(m_session, e, false)) QWidget::keyReleaseEvent(e);
}

QPointer<ControllersWindow> g_window;

} // namespace

void a800ControllersToggle(QWidget *parent, a800session *session)
{
    if (!g_window) g_window = new ControllersWindow(session, parent);
    if (g_window->isVisible()) g_window->hide();
    else g_window->present();
}

bool a800ControllersVisible()
{
    return g_window && g_window->isVisible();
}

void a800ControllersRefresh()
{
    if (g_window) g_window->refresh();
}
