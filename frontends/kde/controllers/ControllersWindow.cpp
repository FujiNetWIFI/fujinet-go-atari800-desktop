/*
 * ControllersWindow -- see ControllersWindow.h.
 *
 * Two painted widgets over the session's own tables, so every frontend shows
 * the same thing: the STAGE (the controller ports with what is plugged into each,
 * over the console keys; a800session_controls_layout says what and where, in
 * layout units, and a800session_controls_hit what a click landed on) and the
 * KEYBOARD (a800session_keyboard_layout, drawn as the machine's own: the 400's
 * membrane, the 800's brown keys, the XL/XE's charcoal ones, the 1200XL's with F1
 * to F4). The CX40 joystick and the CX30 paddles are the Atari 2600 desktop's
 * drawing, from DESIGN-atari.md, written in the design's own device-local units.
 *
 * Everything acts on mouse press AND release, not "clicked": the Atari scans its
 * keyboard and samples the sticks once per frame, so a press present only for the
 * instant of a click falls between frames. A key or a button is HELD while the
 * mouse is down on it (dragging off lets go); SHIFT and CTRL latch for the next
 * key instead, since a mouse cannot hold two keys. Losing the focus, hiding and
 * rebuilding let go of what the mouse holds here (only that: the main window
 * releases its own keys).
 *
 * What the Atari sees held -- host keyboard, gamepads and this window -- is lit in
 * orange, so the window doubles as an input tester for a new pad. Keys typed with
 * the window active go to the Atari as in the main window.
 *
 * FIXED SIZE: the stage is as wide as the layout (30.6 units) whatever is
 * plugged in; only the number of ports (two on an XL/XE, four on a 400/800) changes
 * its height, and with them the window. A singleton, hidden rather than destroyed,
 * so a half-done remap survives closing it.
 *
 * MAP MODE: Map arms a two-step rebind -- click a joystick direction (a side of the
 * stick's ring), a fire button or a console key (on the stage or the keyboard), or
 * choose any of the 30 controls from the list, then press a key or a gamepad
 * button. Rebinding steals the key from whatever held it; Clear takes a control's
 * key and button away. Paddles, mice and the rest are not remappable: a paddle
 * follows the gamepad's sticks.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ControllersWindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QRadialGradient>
#include <QScreen>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>

#include "../Accent.h"
#include "../FujiNetWindows.h"
#include "../KeyForward.h"
#include "../help/HelpWindow.h"

using a800ctl::kCaseMargin;
using a800ctl::kKeyUnit;
using a800ctl::kMargin;
using a800ctl::kStageW;
using a800ctl::kUnit;

namespace {

constexpr int kMaxPadRows = 3;
constexpr qreal kDimAlpha = 0.4;        /* an unpicked part while Map waits for a key */
constexpr int kKnobNudge = 8;           /* one mouse-wheel notch, of 0..228 */

/* ---- the colours (DESIGN-atari.md) ------------------------------------------------------ */

const QColor kPlastic(0x1C, 0x1C, 0x1C);
const QColor kPlasticHi(0x2A, 0x2A, 0x2A);
const QColor kPlasticDeep(0x10, 0x10, 0x10);
const QColor kFire(0xE2, 0x3B, 0x1F);
const QColor kFireHi(0xF0, 0x5A, 0x38);
const QColor kFireDark(0xB0, 0x2A, 0x14);
const QColor kPaddleBody(0x1F, 0x1F, 0x1F);
const QColor kPaddleHi(0x2E, 0x2E, 0x31);
const QColor kPaddleButton(0xD9, 0x3A, 0x22);
const QColor kPaddleButtonHi(0xE8, 0x49, 0x2C);
const QColor kSilver(0xC9, 0xC9, 0xC9);
const QColor kSilverShade(0x8E, 0x8E, 0x8E);
const QColor kLabelOrange(0xE0, 0x8A, 0x1F);
const QColor kMatTop(0x5A, 0x5B, 0x61);
const QColor kMatBottom(0x3E, 0x3F, 0x44);
const QColor kMatEdge(0x1A, 0x1A, 0x1D);
const QColor kMatInk(0xD9, 0xD6, 0xCB);
const QColor kPanel(0x15, 0x15, 0x15);

QString local(const char *text) { return QString::fromUtf8(text ? text : ""); }

QString orNone(const QString &text) { return text.isEmpty() ? QStringLiteral("none") : text; }

/* ---- the machine's keyboard and console keys: one skin per family ---------------------- */

struct Skin {
    QColor caseTop, caseBottom, caseEdge;   /* the case */
    QColor cap, capHi, capEdge;             /* a key cap */
    QColor legend, shifted;                 /* the legends */
    QColor fnCap;                           /* the 1200XL's F1-F4 */
    bool membrane = false;                  /* the 400: flat keys on a panel */
};

Skin skinFor(int family)
{
    Skin s;
    switch (family) {
    case A800_KBD_400:
        s.caseTop = QColor(0x24, 0x24, 0x26);
        s.caseBottom = QColor(0x15, 0x15, 0x17);
        s.caseEdge = QColor(0x08, 0x08, 0x08);
        s.cap = QColor(0x2C, 0x2C, 0x2F);
        s.capHi = QColor(0x3A, 0x3A, 0x3E);
        s.capEdge = QColor(0x0B, 0x0B, 0x0C);
        s.legend = QColor(0xEE, 0xE8, 0xD8);
        s.shifted = QColor(0xE0, 0x8A, 0x1F);
        s.fnCap = s.cap;
        s.membrane = true;
        break;
    case A800_KBD_XL:
    case A800_KBD_1200XL:
        s.caseTop = QColor(0xEC, 0xE9, 0xE0);
        s.caseBottom = QColor(0xD8, 0xD5, 0xCB);
        s.caseEdge = QColor(0x9A, 0x97, 0x8C);
        s.cap = QColor(0x2E, 0x2E, 0x31);
        s.capHi = QColor(0x44, 0x44, 0x49);
        s.capEdge = QColor(0x14, 0x14, 0x16);
        s.legend = QColor(0xF2, 0xF2, 0xF2);
        s.shifted = QColor(0xC9, 0xC9, 0xCC);
        s.fnCap = QColor(0x4A, 0x4A, 0x50);
        break;
    default:   /* the 800 */
        s.caseTop = QColor(0xDA, 0xD3, 0xBF);
        s.caseBottom = QColor(0xC4, 0xBC, 0xA6);
        s.caseEdge = QColor(0x8C, 0x85, 0x72);
        s.cap = QColor(0x3B, 0x35, 0x2B);
        s.capHi = QColor(0x4F, 0x48, 0x3C);
        s.capEdge = QColor(0x1C, 0x18, 0x12);
        s.legend = QColor(0xE9, 0xE4, 0xD4);
        s.shifted = QColor(0xBF, 0xB8, 0xA4);
        s.fnCap = s.cap;
        break;
    }
    return s;
}

/* The console keys: orange to yellow on the 400 and the 800, charcoal on the XL/XE (HELP, a
 * round grey key there). */
QColor consoleFill(int family, int con)
{
    if (family == A800_KBD_XL || family == A800_KBD_1200XL)
        return con == A800_CON_HELP ? QColor(0xA9, 0xA7, 0x9E) : QColor(0x2E, 0x2E, 0x31);
    switch (con) {
    case A800_CON_RESET: return QColor(0xD8, 0x92, 0x1C);
    case A800_CON_OPTION: return QColor(0xE6, 0x9C, 0x1E);
    case A800_CON_SELECT: return QColor(0xEE, 0xAC, 0x1D);
    case A800_CON_START: return QColor(0xF6, 0xBE, 0x13);
    default: return QColor(0xE6, 0x9C, 0x1E);
    }
}

QColor consoleInk(int family, int con)
{
    if (family == A800_KBD_XL || family == A800_KBD_1200XL)
        return con == A800_CON_HELP ? QColor(0x2A, 0x2A, 0x2C) : QColor(0xF2, 0xF2, 0xF2);
    return QColor(0x3A, 0x2A, 0x10);
}

/* ---- small drawing helpers ------------------------------------------------------------ */

/* A device's pixel origin and the pixels per layout unit: a part's art is written in the
 * design's own local units and mapped here. */
struct Dev {
    QPointF o;
    double u = kUnit;
    QPointF p(double x, double y) const { return o + QPointF(x * u, y * u); }
    QRectF r(double x, double y, double w, double h) const { return QRectF(p(x, y), QSizeF(w * u, h * u)); }
    double s(double v) const { return v * u; }
};

QFont pixelFont(QFont font, int pixels, bool bold, bool italic = false)
{
    font.setPixelSize(pixels);
    font.setBold(bold);
    font.setItalic(italic);
    return font;
}

/* Lines centred in rect, the font shrunk until they fit. */
void drawFitted(QPainter &pnt, const QRectF &rect, const QString &text, int pixels, bool bold, const QColor &color,
                bool italic = false, int align = Qt::AlignCenter)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    QFont font = pixelFont(pnt.font(), pixels, bold, italic);
    for (;;) {
        const QFontMetricsF metrics(font);
        qreal wide = 0;
        for (const QString &line : lines) wide = qMax(wide, metrics.horizontalAdvance(line));
        const int size = font.pixelSize() > 0 ? font.pixelSize() : 10;
        if ((wide <= rect.width() + 1 && metrics.lineSpacing() * lines.size() <= rect.height() + 3) || size <= 5)
            break;
        font.setPixelSize(size - 1);
    }
    pnt.setFont(font);
    pnt.setPen(color);
    pnt.drawText(rect, align, text);
}

/* A polygon with its corners rounded by r. */
QPainterPath roundedPolygon(const QVector<QPointF> &pts, double radius)
{
    QPainterPath path;
    const int n = pts.size();
    for (int i = 0; i < n; ++i) {
        const QPointF prev = pts[(i + n - 1) % n], cur = pts[i], next = pts[(i + 1) % n];
        auto toward = [&](const QPointF &from, const QPointF &to) {
            const QPointF delta = to - from;
            const double len = std::hypot(delta.x(), delta.y());
            const double take = std::min(radius, len / 2);
            return from + delta * (take / len);
        };
        const QPointF a = toward(cur, prev), b = toward(cur, next);
        if (i == 0) path.moveTo(a); else path.lineTo(a);
        path.quadTo(cur, b);
    }
    path.closeSubpath();
    return path;
}

/* A binding's key, shortened to fit a small part (tooltips keep the full name). */
QString legendName(QString key)
{
    if (key.startsWith(QLatin1String("Keypad "))) return QStringLiteral("KP ") + key.mid(7);
    if (key.startsWith(QLatin1String("Left "))) return QStringLiteral("L ") + key.mid(5);
    if (key.startsWith(QLatin1String("Right "))) return QStringLiteral("R ") + key.mid(6);
    if (key == QLatin1String("Backspace")) return QStringLiteral("BkSp");
    if (key == QLatin1String("Escape")) return QStringLiteral("Esc");
    if (key == QLatin1String("Return")) return QStringLiteral("Ret");
    if (key == QLatin1String("Space")) return QStringLiteral("Spc");
    if (key == QLatin1String("Up")) return QStringLiteral("↑");
    if (key == QLatin1String("Down")) return QStringLiteral("↓");
    return key;
}

QString keyName(a800session *sess, int target)
{
    char name[48] = "";
    const a800_binding bind = a800session_binding_get(sess, target);
    if (bind.keysym) a800session_keysym_name(bind.keysym, name, sizeof name);
    return local(name);
}

QString padName(a800session *sess, int target)
{
    const a800_binding bind = a800session_binding_get(sess, target);
    return bind.button == A800_PAD_BTN_NONE ? QString() : local(a800_pad_button_name(bind.button));
}

/* Map mode on a control: the key over the gamepad button. */
QString bindingText(a800session *sess, int target)
{
    const QString key = keyName(sess, target), pad = padName(sess, target);
    QString text = key.isEmpty() ? QStringLiteral("—") : key;
    if (!pad.isEmpty()) text += QLatin1Char('\n') + pad;
    return text;
}

QString bindingTip(a800session *sess, int target)
{
    const QString key = keyName(sess, target), pad = padName(sess, target);
    QString tip = QStringLiteral("%1: %2").arg(local(a800_target_name(target)),
                                               key.isEmpty() ? QStringLiteral("no key") : key);
    if (!pad.isEmpty()) tip += QStringLiteral(", ") + pad;
    return tip;
}

/* The target's number within its port (an a800_action), or -1 for the console's. */
int actionOf(int target, int port)
{
    if (port < 0 || port >= A800_PORTS) return -1;
    const int act = target - port * A800_ACT_PER_PORT;
    return act >= 0 && act < A800_ACT_PER_PORT ? act : -1;
}

/* ---- ElidedLabel: never widens the window --------------------------------------------- */

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
    QString text() const { return m_text; }
    QSize sizeHint() const override { return QSize(0, fontMetrics().lineSpacing() * m_lines); }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter pnt(this);
        pnt.setPen(m_dim ? QColor(Qt::gray) : palette().color(QPalette::WindowText));
        if (m_lines > 1)
            pnt.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, m_text);
        else
            pnt.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter,
                         fontMetrics().elidedText(m_text, Qt::ElideRight, width()));
    }

private:
    QString m_text;
    bool m_dim;
    int m_lines;
};

/* ---- KeyboardWidget: the machine's own keyboard --------------------------------------- */

/* The Atari keyboard, painted from a800session_keyboard_layout in key units over a case
 * of the machine's colour. It reports presses by table position; the window holds the key. */
class KeyboardWidget : public QWidget {
public:
    KeyboardWidget(a800session *session, std::function<void(int, bool)> onKey)
        : m_session(session), m_onKey(std::move(onKey))
    {
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(false);
        setObjectName(QStringLiteral("keyboard"));
    }

    /* The machine changed: its keys. Copied, since the session's table is one static buffer.
     * True when anything changed. */
    bool load()
    {
        const int family = a800session_keyboard_family(m_session);
        const a800_kbd_key *keys = nullptr;
        const int n = a800session_keyboard_layout(m_session, &keys);
        if (family == m_family && n == (int)m_keys.size() && n > 0) {
            bool same = true;
            for (int i = 0; i < n && same; ++i) {
                const a800_kbd_key &a = keys[i], &b = m_keys[(size_t)i];
                same = a.label == b.label && a.shifted == b.shifted && a.index == b.index && a.x == b.x && a.y == b.y &&
                       a.w == b.w && a.h == b.h && a.kind == b.kind && a.console == b.console;
            }
            if (same) return false;
        }
        m_family = family;
        m_skin = skinFor(family);
        m_keys.assign(keys, keys + qMax(0, n));
        m_targets.clear();
        float cols = 0, rows = 0;
        for (const a800_kbd_key &k : m_keys) {
            m_targets.push_back(k.console >= 0 ? A800_TARGET_CONSOLE(k.console) : -1);
            cols = qMax(cols, k.x + k.w);
            rows = qMax(rows, k.y + k.h);
        }
        m_held.assign(m_keys.size(), 0);
        m_pressed = -1;
        setFixedSize(qCeil(cols * kKeyUnit) + 2 * kCaseMargin, qCeil(rows * kKeyUnit) + 2 * kCaseMargin);
        update();
        return true;
    }
    int count() const { return (int)m_keys.size(); }
    const a800_kbd_key &key(int i) const { return m_keys[(size_t)i]; }
    int target(int i) const { return m_targets[(size_t)i]; }
    QRectF keyRect(int i) const
    {
        const a800_kbd_key &k = m_keys[(size_t)i];
        return QRectF(kCaseMargin + k.x * kKeyUnit + 1.5, kCaseMargin + k.y * kKeyUnit + 1.5, k.w * kKeyUnit - 3,
                      k.h * kKeyUnit - 3);
    }
    void setMapState(int state)
    {
        m_mapState = state;
        update();
    }
    /* What the Atari sees held (and the key the mouse holds); repaints only on a change. */
    void pollHeld(int mouseIndex)
    {
        bool changed = false;
        for (size_t i = 0; i < m_keys.size(); ++i) {
            const char on = m_keys[i].index == mouseIndex || a800session_keyboard_held(m_session, m_keys[i].index);
            if (on != m_held[i]) {
                m_held[i] = on;
                changed = true;
            }
        }
        if (changed) update();
    }
    /* Lets go of the key the mouse is on. */
    void releaseMouse()
    {
        if (m_pressed >= 0) release();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter pnt(this);
        pnt.setRenderHint(QPainter::Antialiasing);
        pnt.setRenderHint(QPainter::TextAntialiasing);
        /* the case */
        const QRectF caseRect = QRectF(rect()).adjusted(1, 1, -1, -1);
        QLinearGradient grad(caseRect.topLeft(), caseRect.bottomLeft());
        grad.setColorAt(0, m_skin.caseTop);
        grad.setColorAt(1, m_skin.caseBottom);
        pnt.setPen(QPen(m_skin.caseEdge, 2));
        pnt.setBrush(grad);
        pnt.drawRoundedRect(caseRect, 10, 10);
        if (m_skin.membrane) {
            /* the 400's panel: an orange pinstripe round the keys */
            pnt.setPen(QPen(QColor(0xE0, 0x8A, 0x1F), 1));
            pnt.setBrush(Qt::NoBrush);
            pnt.drawRoundedRect(caseRect.adjusted(5, 5, -5, -5), 7, 7);
        }
        for (int i = 0; i < (int)m_keys.size(); ++i) paintKey(pnt, i);
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
                     && (m_keys[(size_t)i].kind == A800_KEY_SHIFT || m_keys[(size_t)i].kind == A800_KEY_CTRL))
                tip = QStringLiteral("Latches for the next key");
            if (tip.isEmpty()) QToolTip::hideText();
            else QToolTip::showText(he->globalPos(), tip, this, keyRect(i).toRect());
            return true;
        }
        return QWidget::event(e);
    }

private:
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

    void paintKey(QPainter &pnt, int i)
    {
        const a800_kbd_key &k = m_keys[(size_t)i];
        const QRectF r = keyRect(i);
        const int t = m_targets[(size_t)i];
        const bool mapping = m_mapState != -2;
        const bool picked = mapping && t >= 0 && t == m_mapState;
        const bool grey = mapping && t < 0;
        const bool lit = !mapping && m_held[(size_t)i] != 0;
        const bool consoleKey = k.kind == A800_KEY_CONSOLE;
        const bool xl = m_family == A800_KBD_XL || m_family == A800_KBD_1200XL;
        const bool round = consoleKey && k.console == A800_CON_HELP && xl;

        QColor face = m_skin.cap;
        QColor ink = m_skin.legend;
        if (consoleKey) {
            face = consoleFill(m_family, k.console);
            ink = consoleInk(m_family, k.console);
        } else if (k.kind == A800_KEY_FUNCTION) {
            face = m_skin.fnCap;
        }
        if (lit || picked) {
            face = a800CtlHeld();
            ink = a800CtlHeldInk();
        }
        pnt.save();
        if (grey) pnt.setOpacity(0.45);

        QRectF cap = r;
        if (round) {
            const double d = std::min(r.width(), r.height());
            cap = QRectF(r.center().x() - d / 2, r.center().y() - d / 2, d, d);
        }
        /* the key: an edge, a face with a lit top */
        pnt.setPen(QPen(m_skin.capEdge, 1.2));
        pnt.setBrush(m_skin.capEdge);
        if (round) pnt.drawEllipse(cap); else pnt.drawRoundedRect(cap, 5, 5);
        const QRectF top = m_skin.membrane ? cap.adjusted(1, 1, -1, -1) : cap.adjusted(1.5, 1.5, -1.5, -3.5);
        QLinearGradient shade(top.topLeft(), top.bottomLeft());
        shade.setColorAt(0, face.lighter(m_skin.membrane ? 110 : 125));
        shade.setColorAt(1, face);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(shade);
        if (round) pnt.drawEllipse(top); else pnt.drawRoundedRect(top, 4, 4);
        if (picked || (mapping && t >= 0)) {
            pnt.setPen(QPen(picked ? a800AccentColor() : QColor(255, 255, 255, 90), picked ? 2.5 : 1.2));
            pnt.setBrush(Qt::NoBrush);
            pnt.drawRoundedRect(cap.adjusted(-1, -1, 1, 1), 5, 5);
        }

        /* the legend */
        const QRectF in = top.adjusted(2, 1, -2, -1);
        if (mapping && t >= 0) {
            drawFitted(pnt, in, bindingText(m_session, t), 10, true, ink);
        } else if (k.shifted) {
            const QRectF upper(in.x(), in.y(), in.width(), in.height() * 0.42);
            const QRectF lower(in.x(), in.y() + in.height() * 0.42, in.width(), in.height() * 0.58);
            drawFitted(pnt, upper, local(k.shifted), 9, false, lit || picked ? ink : m_skin.shifted);
            drawFitted(pnt, lower, local(k.label), 14, true, ink);
        } else if (k.label && k.label[0]) {
            const QString label = local(k.label);
            const bool boxed = k.kind == A800_KEY_SHIFT || k.kind == A800_KEY_CTRL;
            if (boxed) {
                /* CTRL and SHIFT carry boxed legends */
                QFont f = pixelFont(pnt.font(), 11, true);
                const QFontMetricsF fm(f);
                const qreal wd = std::min(in.width() - 4, fm.horizontalAdvance(label) + 10);
                const QRectF box(in.center().x() - wd / 2, in.center().y() - 9, wd, 18);
                pnt.setPen(QPen(ink, 1.2));
                pnt.setBrush(Qt::NoBrush);
                pnt.drawRoundedRect(box, 3, 3);
            }
            drawFitted(pnt, in, label, consoleKey ? 11 : 13, true, ink);
        }
        pnt.restore();
    }

    a800session *m_session;
    std::function<void(int, bool)> m_onKey;
    std::vector<a800_kbd_key> m_keys;
    std::vector<int> m_targets;     /* each key's console target (Map), or -1 */
    std::vector<char> m_held;
    int m_family = -1;
    Skin m_skin = skinFor(A800_KBD_800);
    int m_mapState = -2;
    int m_pressed = -1;             /* table position the mouse is on, or -1 */
};

/* ---- StageWidget: the ports and the console ---------------------------------------------- */

class StageWidget : public QWidget {
public:
    /* onPick: a control was chosen in Map mode (its target); onHint: a message for the hint line */
    StageWidget(a800session *session, std::function<void(int)> onPick, std::function<void(const QString &)> onHint)
        : m_session(session), m_onPick(std::move(onPick)), m_onHint(std::move(onHint))
    {
        setObjectName(QStringLiteral("stage"));
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
        std::memset(m_dev, 0, sizeof m_dev);
        syncLayout();
        pollHeld();
    }

    /* Lets go of everything the mouse holds (also on focus loss and rebuild). */
    void releaseAll()
    {
        switch (m_hold) {
        case Stick: a800session_pointer_set(m_session, m_holdPort, 0); break;
        case Knob: a800session_paddle_pointer(m_session, m_holdPort, m_holdPaddle, 0, 0); break;
        case Part:
            if (m_partDown) applyPart(m_holdIdx, false);
            break;
        case PadSurface: endPad(); break;
        default: break;
        }
        m_hold = None;
        m_holdIdx = -1;
        m_partDown = false;
        m_stickVec = QPointF();
        update();
    }

    /* -2 idle, -1 armed, >= 0 waiting for a key or pad button for that target */
    void setMap(int state)
    {
        m_map = state;
        update();
    }
    void setShowBindings(bool visible)
    {
        m_showBindings = visible;
        update();
    }

    /* The layout for what is plugged in; true when the ports, a device or the machine changed. */
    bool syncLayout()
    {
        const int machine = a800session_get_int(m_session, "machine", A800_M_130XE);
        const int xl = a800_machine_is_xl(machine) ? 1 : 0;
        const int nports = qBound(1, a800session_active_ports(m_session), (int)A800_PORTS);
        int devs[A800_PORTS];
        for (int p = 0; p < A800_PORTS; ++p) devs[p] = p < nports ? a800session_port_device(m_session, p) : -1;
        const int family = a800session_keyboard_family(m_session);
        if (m_n > 0 && nports == m_nports && xl == m_xl && family == m_family &&
            std::memcmp(devs, m_dev, sizeof devs) == 0)
            return false;
        if (m_hold != None) releaseAll();
        m_nports = nports;
        m_xl = xl;
        m_family = family;
        std::memcpy(m_dev, devs, sizeof devs);
        m_n = a800session_controls_layout(devs, nports, xl, m_lay, A800_CONTROLS_MAX, &m_w, &m_h);
        setFixedSize(kStageW, qCeil(m_h * kUnit) + 2 * kMargin);
        update();
        return true;
    }

    /* What the Atari sees held (keyboard, gamepads, the mouse), where the paddles stand and the
     * console keys; repaints only on a change. */
    void pollHeld()
    {
        bool changed = syncLayout();
        for (int port = 0; port < A800_PORTS; ++port) {
            const unsigned held = a800session_buttons_held(m_session, port);
            if (held != m_held[port]) {
                m_held[port] = held;
                changed = true;
            }
            for (int pad = 0; pad < 2; ++pad) {
                const int pos = a800session_paddle_value(m_session, port, pad);
                if (pos != m_pos[port][pad]) {
                    m_pos[port][pad] = pos;
                    changed = true;
                }
            }
        }
        const unsigned con = a800session_console_held(m_session);
        if (con != m_con) {
            m_con = con;
            changed = true;
        }
        if (changed) update();
    }

    int partCount() const { return m_n; }
    const a800_control &part(int idx) const { return m_lay[idx]; }

    /* a part as a pixel rect in the stage */
    QRectF partRect(int idx) const
    {
        const a800_control &ctl = m_lay[idx];
        return QRectF(origin() + QPointF(ctl.x * kUnit, ctl.y * kUnit), QSizeF(ctl.w * kUnit, ctl.h * kUnit));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter pnt(this);
        pnt.setRenderHint(QPainter::Antialiasing);
        pnt.setRenderHint(QPainter::TextAntialiasing);
        paintMat(pnt);
        syncLayout();
        for (int idx = 0; idx < m_n; ++idx) paintPart(pnt, idx);
    }

    void mousePressEvent(QMouseEvent *evt) override
    {
        if (evt->button() != Qt::LeftButton) return;
        const QPointF pos = evt->position();
        const int hit = hitAt(pos);
        if (m_map != -2) {
            if (hit >= 0) pick(hit, pos);
            return;
        }
        if (m_hold != None || hit < 0) return;     /* one thing at a time */
        const a800_control &ctl = m_lay[hit];
        switch (ctl.kind) {
        case A800_CTL_STICK:
            m_hold = Stick;
            m_holdIdx = hit;
            m_holdPort = ctl.port;
            dragStick(ctl, pos);
            break;
        case A800_CTL_KNOB:
            m_hold = Knob;
            m_holdIdx = hit;
            m_holdPort = ctl.port;
            m_holdPaddle = ctl.arg;
            dragKnob(ctl, pos);
            break;
        case A800_CTL_MAT:
        case A800_CTL_BALL:
            m_hold = Mat;
            m_holdIdx = hit;
            m_holdPort = ctl.port;
            m_last = pos;
            break;
        case A800_CTL_PAD:
            m_hold = PadSurface;
            m_holdIdx = hit;
            m_holdPort = ctl.port;
            dragPad(ctl, pos);
            break;
        case A800_CTL_ROUND_BUTTON:
        case A800_CTL_PADDLE_FIRE:
        case A800_CTL_TURN:
        case A800_CTL_CONSOLE_KEY:
        case A800_CTL_POINTER_BUTTON:
        case A800_CTL_CX85_KEY:
            m_hold = Part;
            m_holdIdx = hit;
            m_holdPort = ctl.port;
            m_partDown = true;
            applyPart(hit, true);
            break;
        default: return;
        }
        pollHeld();
        update();
    }

    /* Dragging off a button lets it go (and back on, holds it again); the stick follows the
     * pointer; the knob turns to it; a mat or the ball moves what it drives. */
    void mouseMoveEvent(QMouseEvent *evt) override
    {
        const QPointF pos = evt->position();
        const int hover = m_hold == None ? hitAt(pos) : -1;
        if (hover != m_hover) {
            m_hover = hover;
            update();
        }
        if (m_hold == Stick) {
            dragStick(m_lay[m_holdIdx], pos);
            update();
        } else if (m_hold == Knob) {
            dragKnob(m_lay[m_holdIdx], pos);
        } else if (m_hold == Part) {
            const bool over = hitAt(pos) == m_holdIdx;
            if (over != m_partDown) {
                m_partDown = over;
                applyPart(m_holdIdx, over);
                pollHeld();
                update();
            }
        } else if (m_hold == Mat) {
            const QPointF d = pos - m_last;
            m_last = pos;
            const int dx = int(std::lround(d.x())), dy = int(std::lround(d.y()));
            if (dx || dy) a800session_pointer_motion(m_session, dx, dy, m_ptrMask[m_holdPort]);
        } else if (m_hold == PadSurface) {
            dragPad(m_lay[m_holdIdx], pos);
        }
    }

    void mouseReleaseEvent(QMouseEvent *evt) override
    {
        if (evt->button() != Qt::LeftButton) return;
        if (m_hold != None) releaseAll();
        m_hover = hitAt(evt->position());
        pollHeld();
        update();
    }

    void wheelEvent(QWheelEvent *evt) override
    {
        if (m_map != -2 || m_hold != None) return;
        const int idx = hitAt(evt->position());
        if (idx < 0) return;
        const a800_control &ctl = m_lay[idx];
        const int steps = evt->angleDelta().y() / 120;
        if (!steps || ctl.kind != A800_CTL_KNOB) return;
        /* clockwise (a scroll up) is to the right, a lower value */
        const int value = qBound(0, a800session_paddle_value(m_session, ctl.port, ctl.arg) - steps * kKnobNudge,
                                 (int)A800_PADDLE_MAX);
        a800session_paddle_pointer(m_session, ctl.port, ctl.arg, value, 1);
        a800session_paddle_pointer(m_session, ctl.port, ctl.arg, value, 0);
        pollHeld();
        update();
        evt->accept();
    }

    void leaveEvent(QEvent *evt) override
    {
        if (m_hover != -1) {
            m_hover = -1;
            update();
        }
        QWidget::leaveEvent(evt);
    }

    bool event(QEvent *evt) override
    {
        if (evt->type() == QEvent::ToolTip) {
            auto *help = static_cast<QHelpEvent *>(evt);
            const QString tip = tipAt(help->pos());
            if (tip.isEmpty())
                QToolTip::hideText();
            else
                QToolTip::showText(help->globalPos(), tip, this);
            return true;
        }
        return QWidget::event(evt);
    }

private:
    enum Hold { None, Stick, Knob, Part, Mat, PadSurface };

    static QPointF origin() { return a800ctl::origin(); }

    int hitAt(const QPointF &pos) const
    {
        return a800session_controls_hit(m_lay, m_n, float((pos.x() - kMargin) / kUnit), float((pos.y() - kMargin) / kUnit));
    }

    /* a part's centre in layout units */
    static QPointF centreOf(const a800_control &ctl) { return QPointF(ctl.x + ctl.w / 2, ctl.y + ctl.h / 2); }

    /* the pointer, in layout units from a part's centre */
    QPointF fromCentre(const a800_control &ctl, const QPointF &pos) const
    {
        const QPointF centre = centreOf(ctl);
        return QPointF((pos.x() - kMargin) / kUnit - centre.x(), (pos.y() - kMargin) / kUnit - centre.y());
    }

    /* A part pressed (down) or let go: the call it drives. */
    void applyPart(int idx, bool down)
    {
        if (idx < 0 || idx >= m_n) return;
        const a800_control &ctl = m_lay[idx];
        switch (ctl.kind) {
        case A800_CTL_ROUND_BUTTON:
        case A800_CTL_CONSOLE_KEY:
            if (ctl.target >= 0) a800session_press(m_session, ctl.target, down ? 1 : 0);
            break;
        case A800_CTL_PADDLE_FIRE:
            m_paddleFire[ctl.port][ctl.arg & 1] = down;
            a800session_paddle_fire(m_session, ctl.port, ctl.arg, down ? 1 : 0);
            break;
        case A800_CTL_TURN:
            m_turnHeld[ctl.port][ctl.arg & 1] = down ? ctl.dir : 0;
            a800session_paddle_turn(m_session, ctl.port, ctl.arg, down ? ctl.dir : 0);
            break;
        case A800_CTL_POINTER_BUTTON:
            if (down) m_ptrMask[ctl.port] |= (unsigned)ctl.arg; else m_ptrMask[ctl.port] &= ~(unsigned)ctl.arg;
            a800session_pointer_motion(m_session, 0, 0, m_ptrMask[ctl.port]);
            break;
        case A800_CTL_CX85_KEY:
            a800session_cx85_key(m_session, ctl.arg, down ? 1 : 0);
            break;
        default: break;
        }
    }

    /* The stick follows the pointer wherever it is dragged to: the directions it resolves to are
     * the pointer's hold on the port. */
    void dragStick(const a800_control &ctl, const QPointF &pos)
    {
        const QPointF d = fromCentre(ctl, pos);
        const float radius = ctl.w / 2;
        a800session_pointer_set(m_session, ctl.port, a800session_stick_from_vector(float(d.x()), float(d.y()), radius));
        const double len = std::hypot(d.x(), d.y());
        m_stickVec = len > radius ? QPointF(d.x() / len, d.y() / len) : QPointF(d.x() / radius, d.y() / radius);
        pollHeld();
    }

    /* The knob to the angle of the pointer (nothing in the dead centre). */
    void dragKnob(const a800_control &ctl, const QPointF &pos)
    {
        const QPointF d = fromCentre(ctl, pos);
        if (std::hypot(d.x(), d.y()) < ctl.w / 2 * 0.2) return;
        a800session_paddle_pointer(m_session, ctl.port, ctl.arg, a800session_knob_from_vector(float(d.x()), float(d.y())), 1);
        pollHeld();
    }

    /* A point of an absolute device's pad: u and v, 0..1 from its top-left (held inside the pad). */
    void dragPad(const a800_control &ctl, const QPointF &pos)
    {
        const QRectF box = partRect(m_holdIdx);
        const double u = qBound(0.0, (pos.x() - box.left()) / box.width(), 1.0);
        const double v = qBound(0.0, (pos.y() - box.top()) / box.height(), 1.0);
        m_padPoint = QPointF(u, v);
        m_padActive = true;
        /* the tablet's touch is its own button (bit 1); the Koala's buttons are separate parts */
        const unsigned mask = ctl.arg == 1 ? m_ptrMask[ctl.port] | 1u : m_ptrMask[ctl.port];
        a800session_pointer_norm(m_session, float(u), float(v), 1, mask);
        update();
    }

    void endPad()
    {
        if (m_holdPort >= 0 && m_holdPort < A800_PORTS)
            a800session_pointer_norm(m_session, float(m_padPoint.x()), float(m_padPoint.y()), 0, m_ptrMask[m_holdPort]);
        m_padActive = false;
    }

    /* Map mode: a click on a control picks its target. The stick has four (the side of the ring that
     * was clicked); a fire button or a console key its own. A paddle, a mouse and the rest are not
     * remappable. */
    void pick(int idx, const QPointF &pos)
    {
        const a800_control &ctl = m_lay[idx];
        const QPointF d = fromCentre(ctl, pos);
        if (ctl.kind == A800_CTL_STICK) {
            if (std::hypot(d.x(), d.y()) < ctl.w / 2 * 0.3) {
                m_onHint(QStringLiteral("Click toward a side of the ring: up, down, left or right."));
                return;
            }
            const int act = std::abs(d.x()) > std::abs(d.y()) ? (d.x() < 0 ? A800_ACT_LEFT : A800_ACT_RIGHT)
                                                              : (d.y() < 0 ? A800_ACT_UP : A800_ACT_DOWN);
            m_onPick(A800_TARGET_PORT(ctl.port, act));
        } else if ((ctl.kind == A800_CTL_ROUND_BUTTON || ctl.kind == A800_CTL_CONSOLE_KEY) && ctl.target >= 0) {
            m_onPick(ctl.target);
        } else {
            m_onHint(QStringLiteral("That cannot be remapped: paddles and the pointer devices follow the gamepad "
                                    "and the mouse."));
        }
    }

    /* the targets a part drives, for Map's dimming and highlight */
    std::vector<int> targetsOf(const a800_control &ctl) const
    {
        switch (ctl.kind) {
        case A800_CTL_STICK:
            return { A800_TARGET_PORT(ctl.port, A800_ACT_UP), A800_TARGET_PORT(ctl.port, A800_ACT_DOWN),
                     A800_TARGET_PORT(ctl.port, A800_ACT_LEFT), A800_TARGET_PORT(ctl.port, A800_ACT_RIGHT) };
        case A800_CTL_ROUND_BUTTON:
        case A800_CTL_CONSOLE_KEY:
            if (ctl.target >= 0) return { ctl.target };
            return {};
        default:
            return {};
        }
    }

    bool mapping() const { return m_map != -2; }
    bool showBind() const { return mapping() || m_showBindings; }

    /* While Map waits for a key, every control but the picked one steps back. */
    bool dimmed(const a800_control &ctl) const
    {
        if (m_map < 0) return false;
        if (ctl.kind == A800_CTL_BODY || ctl.kind == A800_CTL_CAPTION || ctl.kind == A800_CTL_NOTE) return false;
        const std::vector<int> targets = targetsOf(ctl);
        for (int target : targets) if (target == m_map) return false;
        return true;
    }
    bool picked(const a800_control &ctl) const
    {
        if (m_map < 0) return false;
        for (int target : targetsOf(ctl)) if (target == m_map) return true;
        return false;
    }

    /* Is the part lit: held by anyone (keyboard, a pad, this window)? */
    bool heldPart(int idx) const
    {
        const a800_control &ctl = m_lay[idx];
        switch (ctl.kind) {
        case A800_CTL_ROUND_BUTTON: {
            const int act = actionOf(ctl.target, ctl.port);
            return act >= 0 && (m_held[ctl.port] & (1u << act)) != 0;
        }
        case A800_CTL_CONSOLE_KEY:
            return ctl.target >= 0 && (m_con & (1u << (ctl.target - A800_TARGET_CONSOLE(0)))) != 0;
        case A800_CTL_PADDLE_FIRE:
            return m_paddleFire[ctl.port][ctl.arg & 1];
        case A800_CTL_TURN:
            return m_turnHeld[ctl.port][ctl.arg & 1] == ctl.dir;
        case A800_CTL_POINTER_BUTTON:
            return (m_ptrMask[ctl.port] & (unsigned)ctl.arg) != 0;
        case A800_CTL_CX85_KEY:
            return m_hold == Part && m_partDown && m_holdIdx == idx;
        default: return false;
        }
    }

    QString tipAt(const QPoint &pos) const
    {
        const int idx = hitAt(pos);
        if (idx < 0) return QString();
        const a800_control &ctl = m_lay[idx];
        if (ctl.kind == A800_CTL_STICK) {
            auto key = [&](int act) { return orNone(keyName(m_session, A800_TARGET_PORT(ctl.port, act))); };
            return QStringLiteral("Drag to move the stick\nUp: %1  Down: %2  Left: %3  Right: %4")
                .arg(key(A800_ACT_UP), key(A800_ACT_DOWN), key(A800_ACT_LEFT), key(A800_ACT_RIGHT));
        }
        if (ctl.kind == A800_CTL_KNOB)
            return QStringLiteral("Paddle %1: drag round to turn it, scroll to nudge it").arg(ctl.port * 2 + ctl.arg);
        if ((ctl.kind == A800_CTL_ROUND_BUTTON || ctl.kind == A800_CTL_CONSOLE_KEY) && ctl.target >= 0) {
            return QStringLiteral("%1\nKey: %2    Gamepad: %3")
                .arg(local(a800_target_name(ctl.target)), orNone(keyName(m_session, ctl.target)),
                     orNone(padName(m_session, ctl.target)));
        }
        if (ctl.kind == A800_CTL_MAT || ctl.kind == A800_CTL_BALL || ctl.kind == A800_CTL_PAD)
            return local(ctl.legend);
        return QString();
    }

    /* ---- painting ------------------------------------------------------------------ */

    void paintMat(QPainter &pnt)
    {
        const QRectF mat(1, 1, width() - 2, height() - 2);
        QLinearGradient grad(mat.topLeft(), mat.bottomLeft());
        grad.setColorAt(0, kMatTop);
        grad.setColorAt(1, kMatBottom);
        pnt.setPen(QPen(kMatEdge, 2));
        pnt.setBrush(grad);
        pnt.drawRoundedRect(mat, 14, 14);
    }

    QColor heldFill() const { return a800CtlHeld(); }

    bool roundKind(int kind) const
    {
        return kind == A800_CTL_STICK || kind == A800_CTL_KNOB || kind == A800_CTL_BALL || kind == A800_CTL_ROUND_BUTTON;
    }

    void paintPart(QPainter &pnt, int idx)
    {
        const a800_control &ctl = m_lay[idx];
        const bool dim = dimmed(ctl);
        const bool lit = heldPart(idx);
        if (dim) pnt.setOpacity(kDimAlpha);
        pnt.save();
        switch (ctl.kind) {
        case A800_CTL_CAPTION: paintCaption(pnt, idx); break;
        case A800_CTL_BODY: paintBody(pnt, idx); break;
        case A800_CTL_STICK: paintStick(pnt, idx); break;
        case A800_CTL_KNOB: paintKnob(pnt, idx); break;
        case A800_CTL_ROUND_BUTTON: paintRoundButton(pnt, idx, lit); break;
        case A800_CTL_PADDLE_FIRE: paintPaddleFire(pnt, idx, lit); break;
        case A800_CTL_TURN: paintTurn(pnt, idx, lit); break;
        case A800_CTL_CONSOLE_KEY: paintConsoleKey(pnt, idx, lit); break;
        case A800_CTL_POINTER_BUTTON: paintPointerButton(pnt, idx, lit); break;
        case A800_CTL_MAT: paintMouseMat(pnt, idx); break;
        case A800_CTL_BALL: paintBall(pnt, idx); break;
        case A800_CTL_PAD: paintPad(pnt, idx); break;
        case A800_CTL_CX85_KEY: paintCx85Key(pnt, idx, lit); break;
        case A800_CTL_NOTE: paintNote(pnt, idx); break;
        default: break;
        }
        pnt.restore();
        pnt.setOpacity(1);
        if (picked(ctl)) {
            pnt.setPen(QPen(a800AccentColor().lighter(150), 2.5));
            pnt.setBrush(Qt::NoBrush);
            const QRectF box = partRect(idx).adjusted(-2, -2, 2, 2);
            if (roundKind(ctl.kind)) pnt.drawEllipse(box); else pnt.drawRoundedRect(box, 4, 4);
        } else if (m_hover == idx && m_hold == None && m_map == -2) {
            pnt.setPen(QPen(QColor(255, 255, 255, 70), 1.5));
            pnt.setBrush(Qt::NoBrush);
            const QRectF box = partRect(idx).adjusted(-1, -1, 1, 1);
            if (roundKind(ctl.kind)) pnt.drawEllipse(box); else pnt.drawRoundedRect(box, 3, 3);
        }
        if (showBind() && !dim) paintBindingTags(pnt, idx);
    }

    void paintCaption(QPainter &pnt, int idx)
    {
        const a800_control &ctl = m_lay[idx];
        const QString text = QStringLiteral("%1 — %2").arg(local(ctl.label), local(ctl.legend));
        drawFitted(pnt, partRect(idx), text, 13, true, kMatInk);
    }

    /* The device a body belongs to: its top-left in pixels. */
    Dev devOf(const a800_control &body) const
    {
        Dev dev;
        dev.o = origin() + QPointF(body.x * kUnit, body.y * kUnit);
        return dev;
    }

    void paintBody(QPainter &pnt, int idx)
    {
        const a800_control &body = m_lay[idx];
        const QString label = local(body.label);
        const Dev dev = devOf(body);
        m_dev0 = dev;
        m_devBody = QPointF(body.x, body.y);
        m_devLabel = label;
        if (label == QLatin1String("Joystick")) paintJoystickBody(pnt, dev, body.port);
        else if (label == QLatin1String("Paddle A") || label == QLatin1String("Paddle B")) paintPaddleBody(pnt, dev);
        else if (label == QLatin1String("Atari ST mouse")) paintMouseBody(pnt, dev, false);
        else if (label == QLatin1String("Amiga mouse")) paintMouseBody(pnt, dev, true);
        else if (label == QLatin1String("Trak-Ball")) paintTrakBallBody(pnt, dev);
        else if (label == QLatin1String("Koala Pad")) paintKoalaBody(pnt, dev);
        else if (label == QLatin1String("Touch tablet")) paintTabletBody(pnt, dev);
        else if (label == QLatin1String("Light pen")) paintLightPenBody(pnt, dev);
        else if (label == QLatin1String("Light gun")) paintLightGunBody(pnt, dev);
        else if (label == QLatin1String("CX85")) paintCx85Body(pnt, dev);
        else if (label == QLatin1String("Console")) paintConsoleBody(pnt, dev, body);
        else paintNothing(pnt, dev, body);
    }

    /* ---- the CX40 joystick (DESIGN-atari.md section 2) ---- */

    /* the stick's tilt: -1..1 each way, from the drag or from what is held */
    QPointF tiltOf(int port) const
    {
        if (m_hold == Stick && m_holdPort == port) return m_stickVec;
        const unsigned held = m_held[port];
        double dx = 0, dy = 0;
        if (held & (1u << A800_ACT_LEFT)) dx -= 1;
        if (held & (1u << A800_ACT_RIGHT)) dx += 1;
        if (held & (1u << A800_ACT_UP)) dy -= 1;
        if (held & (1u << A800_ACT_DOWN)) dy += 1;
        if (dx && dy) { dx *= 0.7071; dy *= 0.7071; }
        return QPointF(dx, dy);
    }

    void paintJoystickBody(QPainter &pnt, const Dev &dev, int port)
    {
        /* the case: textured black, a bevel round its top face */
        const QRectF box = dev.r(0, 0, 8, 8);
        QLinearGradient face(box.topLeft(), box.bottomRight());
        face.setColorAt(0, QColor(0x30, 0x30, 0x30));
        face.setColorAt(1, QColor(0x17, 0x17, 0x17));
        pnt.setPen(QPen(kPlasticDeep, dev.s(0.14)));
        pnt.setBrush(face);
        pnt.drawRoundedRect(box, dev.s(0.9), dev.s(0.9));
        pnt.setPen(QPen(kPlasticHi, dev.s(0.1)));
        pnt.setBrush(Qt::NoBrush);
        pnt.drawRoundedRect(dev.r(0.25, 0.25, 7.5, 7.5), dev.s(0.7), dev.s(0.7));

        /* the fire button's recess: a teardrop, a round pit joined to the ring */
        const QPointF fireC(1.7, 1.7), ringC(4.5, 4.5);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(kPlasticDeep);
        pnt.drawEllipse(dev.p(fireC.x(), fireC.y()), dev.s(1.1), dev.s(1.1));
        {
            const QPointF dir(0.7071, 0.7071), perp(-0.7071, 0.7071);
            const double half = 0.45, reach = 2.2;
            QPolygonF neck;
            neck << dev.p(fireC.x() + perp.x() * half, fireC.y() + perp.y() * half)
                 << dev.p(fireC.x() + dir.x() * reach + perp.x() * half, fireC.y() + dir.y() * reach + perp.y() * half)
                 << dev.p(fireC.x() + dir.x() * reach - perp.x() * half, fireC.y() + dir.y() * reach - perp.y() * half)
                 << dev.p(fireC.x() - perp.x() * half, fireC.y() - perp.y() * half);
            pnt.drawPolygon(neck);
        }

        /* the ring and its ticks */
        const QPointF centre = dev.p(ringC.x(), ringC.y());
        pnt.setBrush(kPlasticDeep);
        pnt.drawEllipse(centre, dev.s(3.0), dev.s(3.0));

        /* a held direction lights its sector of the ring (two held: the diagonal's) */
        const QPointF tilt = tiltOf(port);
        const unsigned held = m_held[port];
        const bool heldDir = held & ((1u << A800_ACT_UP) | (1u << A800_ACT_DOWN) | (1u << A800_ACT_LEFT) |
                                     (1u << A800_ACT_RIGHT));
        if (heldDir || (m_hold == Stick && m_holdPort == port && std::hypot(tilt.x(), tilt.y()) > 0.3)) {
            const double qtAngle = qRadiansToDegrees(std::atan2(-tilt.y(), tilt.x()));
            QPainterPath wedge;
            wedge.moveTo(centre);
            wedge.arcTo(QRectF(centre.x() - dev.s(3.0), centre.y() - dev.s(3.0), dev.s(6.0), dev.s(6.0)), qtAngle - 22.5, 45);
            wedge.closeSubpath();
            QColor tint = heldFill();
            tint.setAlpha(110);
            pnt.setBrush(tint);
            pnt.drawPath(wedge);
        }
        QColor tick(0x3A, 0x3A, 0x3A);
        pnt.setPen(QPen(tick, dev.s(0.1), Qt::SolidLine, Qt::RoundCap));
        for (int i = 0; i < 36; ++i) {
            const double a = qDegreesToRadians(i * 10.0);
            const double r0 = 2.55, r1 = 2.85;
            pnt.drawLine(dev.p(ringC.x() + std::cos(a) * r0, ringC.y() + std::sin(a) * r0),
                         dev.p(ringC.x() + std::cos(a) * r1, ringC.y() + std::sin(a) * r1));
        }
        /* "TOP", embossed near the top of the ring */
        drawFitted(pnt, dev.r(3.7, 1.52, 1.6, 0.5), QStringLiteral("TOP"), 9, true, kPlasticHi);
    }

    void paintStick(QPainter &pnt, int idx)
    {
        const a800_control &ctl = m_lay[idx];
        const Dev dev = m_dev0;
        const double cx = ctl.x - m_devBody.x() + ctl.w / 2, cy = ctl.y - m_devBody.y() + ctl.h / 2;
        const QPointF tilt = tiltOf(ctl.port);

        /* the boot: concentric ribbed rings, squashed a little toward the tilt */
        static const struct { double r; QColor col; } rings[] = {
            { 2.1, QColor(0x15, 0x15, 0x15) }, { 1.7, QColor(0x26, 0x26, 0x26) }, { 1.3, QColor(0x15, 0x15, 0x15) },
        };
        pnt.setPen(Qt::NoPen);
        for (const auto &ring : rings) {
            pnt.setBrush(ring.col);
            pnt.drawEllipse(dev.p(cx + tilt.x() * 0.3 * (ring.r / 2.1), cy + tilt.y() * 0.3 * (ring.r / 2.1)),
                            dev.s(ring.r), dev.s(ring.r * (1 - 0.08 * std::hypot(tilt.x(), tilt.y()))));
        }
        /* the shaft cap: an octagon that slides toward where it is pushed, up to 1.0 u */
        const QPointF cap = dev.p(cx + tilt.x() * 1.0, cy + tilt.y() * 1.0);
        QPolygonF octagon;
        for (int i = 0; i < 8; ++i) {
            const double a = qDegreesToRadians(22.5 + i * 45.0);
            octagon << cap + QPointF(std::cos(a) * dev.s(0.85), std::sin(a) * dev.s(0.85));
        }
        pnt.setPen(QPen(kPlasticHi, dev.s(0.1)));
        pnt.setBrush(QColor(0x0C, 0x0C, 0x0C));
        pnt.drawPolygon(octagon);
        /* a highlight on the cap's upper-left, so it reads as a top */
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(QColor(255, 255, 255, 24));
        pnt.drawEllipse(cap + QPointF(-dev.s(0.25), -dev.s(0.28)), dev.s(0.3), dev.s(0.22));
    }

    void paintRoundButton(QPainter &pnt, int idx, bool lit)
    {
        const QPointF centre = partRect(idx).center();
        /* the joystick's fire button: red, glossy, in its recess */
        const double r = kUnit * 0.8;
        pnt.setPen(QPen(lit ? heldFill() : kFireDark, lit ? kUnit * 0.15 : kUnit * 0.06));
        pnt.setBrush(lit ? kFireDark : kFire);
        pnt.drawEllipse(centre, r, r);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(lit ? QColor(255, 255, 255, 30) : kFireHi);
        pnt.drawEllipse(centre + QPointF(-r * 0.25, -r * 0.3), r * 0.45, r * 0.3);
    }

    /* ---- the CX30 paddle (section 3) ---- */

    void paintPaddleBody(QPainter &pnt, const Dev &dev)
    {
        const QVector<QPointF> pts = { dev.p(1.0, 0.3), dev.p(5.6, 0.3), dev.p(7.0, 5.0), dev.p(6.2, 5.9), dev.p(0.8, 5.9),
                                       dev.p(0.0, 4.6) };
        const QPainterPath shape = roundedPolygon(pts, dev.s(0.7));
        QLinearGradient grad(dev.p(0, 0), dev.p(7, 6));
        grad.setColorAt(0, kPaddleHi);
        grad.setColorAt(1, QColor(0x16, 0x16, 0x16));
        pnt.setPen(QPen(QColor(0x0A, 0x0A, 0x0A), dev.s(0.1)));
        pnt.setBrush(grad);
        pnt.drawPath(shape);
        pnt.setPen(QPen(kPaddleHi, dev.s(0.08)));
        pnt.setBrush(Qt::NoBrush);
        pnt.drawPath(roundedPolygon({ dev.p(1.15, 0.5), dev.p(5.45, 0.5), dev.p(6.75, 4.95), dev.p(6.05, 5.7),
                                      dev.p(0.95, 5.7), dev.p(0.25, 4.55) }, dev.s(0.55)));

        /* the label plate, bottom right */
        const QRectF plate = dev.r(3.85, 4.7, 2.75, 1.05);
        pnt.setPen(QPen(QColor(0x8A, 0x8A, 0x8A), dev.s(0.05)));
        pnt.setBrush(kPlasticDeep);
        pnt.drawRoundedRect(plate, dev.s(0.22), dev.s(0.22));
        drawFitted(pnt, dev.r(3.95, 4.78, 1.55, 0.9), QStringLiteral("paddle"), 12, true, QColor(0xE6, 0xE6, 0xE6), true);
        /* two crossed rackets and a ball, in the plate's gold */
        const QColor gold(0xDC, 0xCB, 0x78);
        pnt.setPen(QPen(gold, dev.s(0.06)));
        pnt.setBrush(Qt::NoBrush);
        pnt.drawEllipse(dev.p(5.7, 5.02), dev.s(0.2), dev.s(0.24));
        pnt.drawEllipse(dev.p(6.08, 4.95), dev.s(0.2), dev.s(0.24));
        pnt.setPen(QPen(gold, dev.s(0.08), Qt::SolidLine, Qt::RoundCap));
        pnt.drawLine(dev.p(5.75, 5.26), dev.p(6.0, 5.55));
        pnt.drawLine(dev.p(6.1, 5.2), dev.p(5.85, 5.55));
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(gold);
        pnt.drawEllipse(dev.p(5.5, 5.5), dev.s(0.07), dev.s(0.07));
    }

    /* the knob: ribbed, turning to where the paddle stands */
    void paintKnob(QPainter &pnt, int idx)
    {
        const a800_control &ctl = m_lay[idx];
        const Dev dev = m_dev0;
        const double cx = ctl.x - m_devBody.x() + ctl.w / 2, cy = ctl.y - m_devBody.y() + ctl.h / 2;
        const QPointF centre = dev.p(cx, cy);
        const double angle = a800session_knob_angle(m_pos[ctl.port][ctl.arg & 1]);

        pnt.setPen(QPen(QColor(0x05, 0x05, 0x05), dev.s(0.08)));
        pnt.setBrush(QColor(0x0E, 0x0E, 0x0E));
        pnt.drawEllipse(centre, dev.s(2.05), dev.s(2.05));
        /* the knurled rim: sixty ribs that turn with the knob */
        pnt.setPen(QPen(QColor(0x2A, 0x2A, 0x2A), dev.s(0.09), Qt::SolidLine, Qt::RoundCap));
        for (int i = 0; i < 60; ++i) {
            const double a = qDegreesToRadians(angle + i * 6.0);
            pnt.drawLine(centre + QPointF(std::sin(a) * dev.s(1.88), -std::cos(a) * dev.s(1.88)),
                         centre + QPointF(std::sin(a) * dev.s(2.02), -std::cos(a) * dev.s(2.02)));
        }
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(QColor(0x1A, 0x1A, 0x1A));
        pnt.drawEllipse(centre, dev.s(1.65), dev.s(1.65));
        QRadialGradient cap(centre - QPointF(dev.s(0.4), dev.s(0.5)), dev.s(1.8));
        cap.setColorAt(0, QColor(0x2E, 0x2E, 0x2E));
        cap.setColorAt(1, QColor(0x1E, 0x1E, 0x1E));
        pnt.setBrush(cap);
        pnt.drawEllipse(centre, dev.s(1.55), dev.s(1.55));
        /* the position mark (added: the real knob has none, but a turn must be visible) */
        const double rad = qDegreesToRadians(angle);
        const bool active = m_hold == Knob && m_holdPort == ctl.port && m_holdPaddle == ctl.arg;
        pnt.setPen(QPen(active ? heldFill() : QColor(0xD8, 0xD8, 0xD8), dev.s(0.13), Qt::SolidLine, Qt::RoundCap));
        pnt.drawLine(centre + QPointF(std::sin(rad) * dev.s(0.85), -std::cos(rad) * dev.s(0.85)),
                     centre + QPointF(std::sin(rad) * dev.s(1.45), -std::cos(rad) * dev.s(1.45)));
    }

    void paintPaddleFire(QPainter &pnt, int idx, bool lit)
    {
        /* a paddle's red button, on the left edge */
        const QRectF box = partRect(idx);
        QLinearGradient grad(box.topLeft(), box.bottomLeft());
        grad.setColorAt(0, lit ? kFireDark : kPaddleButtonHi);
        grad.setColorAt(1, lit ? kFireDark.darker(120) : kPaddleButton);
        pnt.setPen(QPen(lit ? heldFill() : QColor(0x7A, 0x1A, 0x0C), lit ? kUnit * 0.1 : kUnit * 0.05));
        pnt.setBrush(grad);
        pnt.drawRoundedRect(box, kUnit * 0.22, kUnit * 0.22);
    }

    void paintTurn(QPainter &pnt, int idx, bool lit)
    {
        const a800_control &ctl = m_lay[idx];
        const QRectF box = partRect(idx);
        const bool left = ctl.dir < 0;
        QPolygonF tri;
        const QRectF in = box.adjusted(box.width() * 0.12, box.height() * 0.12, -box.width() * 0.12, -box.height() * 0.12);
        if (left)
            tri << QPointF(in.left(), in.center().y()) << QPointF(in.right(), in.top()) << QPointF(in.right(), in.bottom());
        else
            tri << QPointF(in.right(), in.center().y()) << QPointF(in.left(), in.top()) << QPointF(in.left(), in.bottom());
        pnt.setPen(QPen(lit ? heldFill() : QColor(0x05, 0x05, 0x05), kUnit * 0.05));
        pnt.setBrush(lit ? heldFill() : QColor(0x6E, 0x6E, 0x72));
        pnt.drawPolygon(tri);
    }

    /* ---- the mice, the Trak-Ball, the Koala Pad, the touch tablet (section 8) ---- */

    void paintMouseBody(QPainter &pnt, const Dev &dev, bool amiga)
    {
        const double w = 4.0, h = amiga ? 6.4 : 6.0;
        const QColor base = amiga ? QColor(0xCF, 0xC8, 0xB6) : QColor(0xC4, 0xBE, 0xAF);
        QLinearGradient grad(dev.p(0, 0), dev.p(w, h));
        grad.setColorAt(0, base.lighter(112));
        grad.setColorAt(1, base.darker(112));
        pnt.setPen(QPen(base.darker(150), dev.s(0.08)));
        pnt.setBrush(grad);
        if (amiga) {
            /* a wedge nose: the front (top) a little narrower */
            pnt.drawPath(roundedPolygon({ dev.p(0.5, 0), dev.p(3.5, 0), dev.p(4.0, 1.0), dev.p(4.0, 6.4), dev.p(0, 6.4),
                                          dev.p(0, 1.0) }, dev.s(0.9)));
        } else {
            pnt.drawRoundedRect(dev.r(0, 0, w, h), dev.s(1.2), dev.s(1.2));
        }
        /* the line between the buttons, and the fuji embossed low on the back */
        pnt.setPen(QPen(base.darker(135), dev.s(0.07)));
        pnt.drawLine(dev.p(w / 2, 0.15), dev.p(w / 2, h * 0.4));
        drawFitted(pnt, dev.r(0.8, h - 1.7, w - 1.6, 1.0), amiga ? QStringLiteral("amiga") : QStringLiteral("ATARI"), 11,
                   true, base.darker(122), true);
    }

    void paintTrakBallBody(QPainter &pnt, const Dev &dev)
    {
        const QVector<QPointF> pts = { dev.p(0.6, 0.0), dev.p(8.4, 0.0), dev.p(9.0, 0.8), dev.p(9.0, 4.4), dev.p(8.4, 5.0),
                                       dev.p(0.6, 5.0), dev.p(0.0, 4.4), dev.p(0.0, 0.8) };
        QLinearGradient grad(dev.p(0, 0), dev.p(0, 5));
        grad.setColorAt(0, QColor(0x33, 0x33, 0x35));
        grad.setColorAt(1, QColor(0x1A, 0x1A, 0x1C));
        pnt.setPen(QPen(QColor(0x08, 0x08, 0x08), dev.s(0.1)));
        pnt.setBrush(grad);
        pnt.drawPath(roundedPolygon(pts, dev.s(0.35)));
        /* the ring the ball sits in */
        pnt.setPen(QPen(QColor(0x0A, 0x0A, 0x0A), dev.s(0.1)));
        pnt.setBrush(QColor(0x0F, 0x0F, 0x10));
        pnt.drawEllipse(dev.p(4.5, 2.6), dev.s(1.9), dev.s(1.9));
        /* a silver strip: ATARI left, TRAK-BALL right */
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(QColor(0xC9, 0xC9, 0xC9));
        pnt.drawRoundedRect(dev.r(0.3, 4.2, 8.4, 0.55), dev.s(0.12), dev.s(0.12));
        drawFitted(pnt, dev.r(0.5, 4.2, 2.2, 0.55), QStringLiteral("ATARI"), 9, true, QColor(0x22, 0x22, 0x22),
                   false, Qt::AlignLeft | Qt::AlignVCenter);
        drawFitted(pnt, dev.r(5.8, 4.2, 2.8, 0.55), QStringLiteral("TRAK-BALL"), 9, true, QColor(0x22, 0x22, 0x22),
                   false, Qt::AlignRight | Qt::AlignVCenter);
    }

    void paintKoalaBody(QPainter &pnt, const Dev &dev)
    {
        QLinearGradient grad(dev.p(0, 0), dev.p(0, 8.2));
        grad.setColorAt(0, QColor(0xEE, 0xE7, 0xD2));
        grad.setColorAt(1, QColor(0xD8, 0xD0, 0xB8));
        pnt.setPen(QPen(QColor(0x9C, 0x95, 0x80), dev.s(0.08)));
        pnt.setBrush(grad);
        pnt.drawRoundedRect(dev.r(0, 0, 6.4, 8.2), dev.s(0.6), dev.s(0.6));
        /* the name plate at the bottom */
        drawFitted(pnt, dev.r(0.4, 7.35, 5.6, 0.7), QStringLiteral("KOALA PAD"), 11, true, QColor(0x55, 0x4F, 0x40));
    }

    void paintTabletBody(QPainter &pnt, const Dev &dev)
    {
        pnt.setPen(QPen(QColor(0x08, 0x08, 0x08), dev.s(0.1)));
        pnt.setBrush(QColor(0x1E, 0x1E, 0x1E));
        pnt.drawRoundedRect(dev.r(0, 0, 8.6, 5.2), dev.s(0.5), dev.s(0.5));
        /* a silver label strip: ATARI left, TOUCH TABLET right */
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(QColor(0xD8, 0xD8, 0xD4));
        pnt.drawRoundedRect(dev.r(0.4, 0.25, 7.8, 0.6), dev.s(0.12), dev.s(0.12));
        drawFitted(pnt, dev.r(0.6, 0.25, 2.2, 0.6), QStringLiteral("ATARI"), 9, true, QColor(0x22, 0x22, 0x22), false,
                   Qt::AlignLeft | Qt::AlignVCenter);
        drawFitted(pnt, dev.r(4.4, 0.25, 3.6, 0.6), QStringLiteral("TOUCH TABLET"), 9, true, QColor(0x22, 0x22, 0x22),
                   false, Qt::AlignRight | Qt::AlignVCenter);
        /* the stylus, clipped on the right */
        pnt.setPen(QPen(QColor(0x8A, 0x8A, 0x8E), dev.s(0.16), Qt::SolidLine, Qt::RoundCap));
        pnt.drawLine(dev.p(8.35, 1.3), dev.p(8.35, 4.7));
    }

    void paintLightPenBody(QPainter &pnt, const Dev &dev)
    {
        /* a thin black pen with a grey clip and a tip */
        pnt.setPen(QPen(QColor(0x08, 0x08, 0x08), dev.s(0.06)));
        pnt.setBrush(QColor(0x22, 0x22, 0x22));
        pnt.drawRoundedRect(dev.r(0.15, 1.2, 0.7, 6.6), dev.s(0.3), dev.s(0.3));
        pnt.setBrush(QColor(0x9A, 0x9A, 0x9E));
        pnt.drawRoundedRect(dev.r(0.0, 0.0, 1.0, 1.4), dev.s(0.2), dev.s(0.2));       /* the grey clip */
        pnt.setBrush(QColor(0x55, 0x55, 0x58));
        pnt.drawPolygon(QPolygonF({ dev.p(0.2, 7.8), dev.p(0.8, 7.8), dev.p(0.5, 8.0) }));
    }

    void paintLightGunBody(QPainter &pnt, const Dev &dev)
    {
        const QColor grey(0xB7, 0xB2, 0xA6);
        pnt.setPen(QPen(grey.darker(150), dev.s(0.08)));
        pnt.setBrush(grey);
        /* the barrel, the frame and the ribbed grip */
        pnt.drawRoundedRect(dev.r(0.0, 0.8, 7.2, 1.6), dev.s(0.5), dev.s(0.5));
        pnt.drawRoundedRect(dev.r(2.4, 0.2, 4.2, 2.8), dev.s(0.5), dev.s(0.5));
        pnt.drawPath(roundedPolygon({ dev.p(3.8, 2.8), dev.p(6.6, 2.8), dev.p(7.3, 6.8), dev.p(4.6, 7.0) }, dev.s(0.4)));
        pnt.setPen(QPen(grey.darker(135), dev.s(0.06)));
        for (int i = 0; i < 4; ++i)
            pnt.drawLine(dev.p(4.3 + i * 0.1, 3.7 + i * 0.8), dev.p(6.7 + i * 0.1, 3.7 + i * 0.8));
        /* the front sight and the trigger */
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(QColor(0x6E, 0x6C, 0x66));
        pnt.drawRect(dev.r(0.2, 0.35, 0.5, 0.5));
        pnt.drawRoundedRect(dev.r(3.4, 2.7, 0.7, 1.3), dev.s(0.25), dev.s(0.25));
    }

    void paintCx85Body(QPainter &pnt, const Dev &dev)
    {
        pnt.setPen(QPen(QColor(0x8C, 0x85, 0x72), dev.s(0.08)));
        pnt.setBrush(QColor(0xD7, 0xCF, 0xB8));
        pnt.drawRoundedRect(dev.r(0, 0, 6.4, 5.0), dev.s(0.45), dev.s(0.45));
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(QColor(0x55, 0x56, 0x4F));
        pnt.drawRoundedRect(dev.r(0.12, 0.2, 6.16, 4.6), dev.s(0.3), dev.s(0.3));
        /* the name, in the free corner right of the point key */
        drawFitted(pnt, dev.r(5.05, 3.75, 1.2, 0.5), QStringLiteral("ATARI"), 8, true, QColor(0xF0, 0xC2, 0x1A));
        drawFitted(pnt, dev.r(5.05, 4.25, 1.2, 0.5), QStringLiteral("CX85"), 9, true, QColor(0xE8, 0xE4, 0xD6));
    }

    void paintNothing(QPainter &pnt, const Dev &dev, const a800_control &body)
    {
        pnt.setPen(QPen(QColor(255, 255, 255, 60), 1, Qt::DashLine));
        pnt.setBrush(Qt::NoBrush);
        pnt.drawRoundedRect(dev.r(0, 0, body.w, body.h), dev.s(0.3), dev.s(0.3));
        drawFitted(pnt, dev.r(0, 0, body.w, body.h), QStringLiteral("nothing"), 11, false, QColor(255, 255, 255, 120), true);
    }

    void paintPointerButton(QPainter &pnt, int idx, bool lit)
    {
        const a800_control &ctl = m_lay[idx];
        const QRectF box = partRect(idx);
        QColor base = QColor(0xB9, 0xB2, 0xA0);
        QColor edge = QColor(0x8C, 0x85, 0x72);
        if (m_devLabel == QLatin1String("Koala Pad")) { base = QColor(0x15, 0x15, 0x15); edge = QColor(0x05, 0x05, 0x05); }
        else if (m_devLabel == QLatin1String("Trak-Ball")) { base = QColor(0x4A, 0x4A, 0x4E); edge = QColor(0x08, 0x08, 0x08); }
        else if (m_devLabel == QLatin1String("Atari ST mouse")) { base = QColor(0xB0, 0xAA, 0x9B); }
        QLinearGradient grad(box.topLeft(), box.bottomLeft());
        grad.setColorAt(0, lit ? heldFill() : base.lighter(118));
        grad.setColorAt(1, lit ? heldFill().darker(120) : base);
        pnt.setPen(QPen(lit ? heldFill() : edge, lit ? kUnit * 0.1 : kUnit * 0.05));
        pnt.setBrush(grad);
        pnt.drawRoundedRect(box, kUnit * 0.25, kUnit * 0.25);
        (void)ctl;
    }

    void paintMouseMat(QPainter &pnt, int idx)
    {
        const QRectF box = partRect(idx);
        QLinearGradient grad(box.topLeft(), box.bottomLeft());
        grad.setColorAt(0, QColor(0x34, 0x36, 0x3C));
        grad.setColorAt(1, QColor(0x27, 0x28, 0x2D));
        pnt.setPen(QPen(QColor(0x14, 0x14, 0x16), 2));
        pnt.setBrush(grad);
        pnt.drawRoundedRect(box, kUnit * 0.5, kUnit * 0.5);
        drawFitted(pnt, box.adjusted(kUnit * 0.3, 0, -kUnit * 0.3, 0), local(m_lay[idx].legend), 12, false,
                   QColor(255, 255, 255, 130), true);
    }

    void paintBall(QPainter &pnt, int idx)
    {
        const QRectF box = partRect(idx);
        const QPointF c = box.center();
        QRadialGradient ball(c - QPointF(box.width() * 0.18, box.height() * 0.2), box.width() * 0.7);
        ball.setColorAt(0, QColor(0x8E, 0x8E, 0x94));
        ball.setColorAt(0.55, QColor(0x3C, 0x3C, 0x40));
        ball.setColorAt(1, QColor(0x14, 0x14, 0x16));
        pnt.setPen(QPen(QColor(0x05, 0x05, 0x05), 1.5));
        pnt.setBrush(ball);
        pnt.drawEllipse(box);
    }

    void paintPad(QPainter &pnt, int idx)
    {
        const a800_control &ctl = m_lay[idx];
        const QRectF box = partRect(idx);
        const bool tablet = ctl.arg == 1;
        QLinearGradient grad(box.topLeft(), box.bottomLeft());
        grad.setColorAt(0, tablet ? QColor(0x32, 0x35, 0x38) : QColor(0x44, 0x44, 0x47));
        grad.setColorAt(1, tablet ? QColor(0x24, 0x27, 0x2A) : QColor(0x34, 0x34, 0x36));
        pnt.setPen(QPen(tablet ? QColor(0xC0, 0x40, 0x8A) : QColor(0x1C, 0x1C, 0x1E), tablet ? 2.5 : 2));
        pnt.setBrush(grad);
        pnt.drawRoundedRect(box, kUnit * 0.2, kUnit * 0.2);
        drawFitted(pnt, box.adjusted(8, 8, -8, -8), local(ctl.legend), 12, false, QColor(255, 255, 255, 90), true);
        if (m_padActive && m_holdIdx == idx) {
            const QPointF at(box.left() + m_padPoint.x() * box.width(), box.top() + m_padPoint.y() * box.height());
            pnt.setPen(QPen(heldFill(), 1.5));
            pnt.drawLine(QPointF(at.x(), box.top() + 3), QPointF(at.x(), box.bottom() - 3));
            pnt.drawLine(QPointF(box.left() + 3, at.y()), QPointF(box.right() - 3, at.y()));
            pnt.setBrush(heldFill());
            pnt.drawEllipse(at, 5, 5);
        }
    }

    void paintCx85Key(QPainter &pnt, int idx, bool lit)
    {
        const a800_control &ctl = m_lay[idx];
        const QRectF box = partRect(idx);
        QLinearGradient grad(box.topLeft(), box.bottomLeft());
        grad.setColorAt(0, lit ? heldFill() : QColor(0x4F, 0x48, 0x3C));
        grad.setColorAt(1, lit ? heldFill().darker(115) : QColor(0x3A, 0x35, 0x2B));
        pnt.setPen(QPen(lit ? heldFill() : QColor(0x1C, 0x18, 0x12), 1.2));
        pnt.setBrush(grad);
        pnt.drawRoundedRect(box, 4, 4);
        /* "+ ENTER" is two lines in its tall key */
        QString text = local(ctl.label);
        if (ctl.arg == A800_CX85_PLUS_ENTER) text.replace(QLatin1Char(' '), QLatin1Char('\n'));
        drawFitted(pnt, box.adjusted(1, 1, -1, -1), text, 10, true, lit ? a800CtlHeldInk() : QColor(0xE8, 0xE4, 0xD6));
    }

    void paintNote(QPainter &pnt, int idx)
    {
        drawFitted(pnt, partRect(idx), local(m_lay[idx].label), 13, true, kMatInk, true);
    }

    /* ---- the console strip ---- */

    void paintConsoleBody(QPainter &pnt, const Dev &dev, const a800_control &body)
    {
        pnt.setPen(QPen(QColor(0x05, 0x05, 0x05), dev.s(0.1)));
        pnt.setBrush(kPanel);
        pnt.drawRoundedRect(dev.r(0, 0, body.w, body.h), dev.s(0.35), dev.s(0.35));
        pnt.setPen(QPen(QColor(0xE5, 0x8A, 0x1C, 150), dev.s(0.05)));
        pnt.setBrush(Qt::NoBrush);
        pnt.drawRoundedRect(dev.r(0.1, 0.1, body.w - 0.2, body.h - 0.2), dev.s(0.28), dev.s(0.28));
    }

    void paintConsoleKey(QPainter &pnt, int idx, bool lit)
    {
        const a800_control &ctl = m_lay[idx];
        const QRectF box = partRect(idx).adjusted(1, 1, -1, -1);
        const int con = ctl.target >= 0 ? ctl.target - A800_TARGET_CONSOLE(0) : 0;
        QColor face = lit ? heldFill() : consoleFill(m_family, con);
        const QColor ink = lit ? a800CtlHeldInk() : consoleInk(m_family, con);
        const bool round = con == A800_CON_HELP && (m_family == A800_KBD_XL || m_family == A800_KBD_1200XL);
        QRectF key = box;
        if (round) {
            const double d = std::min(box.width(), box.height());
            key = QRectF(box.center().x() - d / 2, box.center().y() - d / 2, d, d);
        }
        QLinearGradient grad(key.topLeft(), key.bottomLeft());
        grad.setColorAt(0, face.lighter(125));
        grad.setColorAt(1, face);
        pnt.setPen(QPen(face.darker(190), 1.2));
        pnt.setBrush(grad);
        if (round) pnt.drawEllipse(key); else pnt.drawRoundedRect(key, 5, 5);
        if (round) {
            drawFitted(pnt, key.adjusted(1, 1, -1, -1), local(ctl.label), 8, true, ink);
        } else if (showBind() && !mapping()) {
            drawFitted(pnt, QRectF(key.left(), key.top() + 1, key.width(), key.height() * 0.5), local(ctl.label), 10, true, ink);
            drawFitted(pnt, QRectF(key.left(), key.top() + key.height() * 0.5, key.width(), key.height() * 0.46),
                       bindText(ctl.target), 8, true, ink);
        } else {
            drawFitted(pnt, key.adjusted(2, 1, -2, -1), local(ctl.label), 11, true, ink);
        }
    }

    /* ---- Show bindings / Map: the key each control has ---- */

    void paintTag(QPainter &pnt, const QRectF &box, const QString &text, bool pill, const QColor &ink = Qt::white)
    {
        if (text.isEmpty()) return;
        if (pill) {
            pnt.setPen(Qt::NoPen);
            pnt.setBrush(QColor(0, 0, 0, 170));
            pnt.drawRoundedRect(box, 3, 3);
        }
        drawFitted(pnt, box.adjusted(1, 0, -1, 0), text, 9, true, ink);
    }

    QString bindText(int target) const
    {
        const QString key = keyName(m_session, target);
        return key.isEmpty() ? QStringLiteral("—") : legendName(key);
    }

    void paintBindingTags(QPainter &pnt, int idx)
    {
        const a800_control &ctl = m_lay[idx];
        const QRectF box = partRect(idx);
        switch (ctl.kind) {
        case A800_CTL_STICK: {
            /* the four directions, round the ring */
            const double r = ctl.w / 2 * kUnit;
            const QPointF c = box.center();
            struct Side { int act; double dx, dy; };
            const Side sides[4] = { { A800_ACT_UP, 0, -1 }, { A800_ACT_DOWN, 0, 1 },
                                    { A800_ACT_LEFT, -1, 0 }, { A800_ACT_RIGHT, 1, 0 } };
            for (const Side &side : sides) {
                const QPointF at = c + QPointF(side.dx * (r + kUnit * 0.98), side.dy * (r + kUnit * 0.95));
                paintTag(pnt, QRectF(at.x() - kUnit * 0.7, at.y() - kUnit * 0.25, kUnit * 1.4, kUnit * 0.5),
                         bindText(A800_TARGET_PORT(ctl.port, side.act)), true);
            }
            break;
        }
        case A800_CTL_ROUND_BUTTON:
            if (ctl.target >= 0) paintTag(pnt, box.adjusted(-4, 8, 4, -8), bindText(ctl.target), false);
            break;
        default: break;
        }
    }

    a800session *m_session;
    std::function<void(int)> m_onPick;
    std::function<void(const QString &)> m_onHint;

    a800_control m_lay[A800_CONTROLS_MAX];
    int m_n = 0;
    int m_nports = 0;
    int m_xl = -1;
    int m_family = -1;
    int m_dev[A800_PORTS];
    float m_w = 0, m_h = 0;

    unsigned m_held[A800_PORTS] = {};
    int m_pos[A800_PORTS][2] = {};
    unsigned m_con = 0;
    unsigned m_ptrMask[A800_PORTS] = {};          /* the pointer buttons this window holds */
    bool m_paddleFire[A800_PORTS][2] = {};
    int m_turnHeld[A800_PORTS][2] = {};

    Hold m_hold = None;
    int m_holdIdx = -1;
    int m_holdPort = 0;
    int m_holdPaddle = 0;
    bool m_partDown = false;
    QPointF m_stickVec;                   /* the drag, -1..1 each way (drawing only) */
    QPointF m_last;                       /* a mat's last pointer position */
    QPointF m_padPoint;                   /* a pad's last touch, 0..1 */
    bool m_padActive = false;
    int m_hover = -1;

    int m_map = -2;
    bool m_showBindings = false;

    Dev m_dev0;                           /* the device being painted (set by its body) */
    QPointF m_devBody;
    QString m_devLabel;
};

/* ---- ControllersWindow --------------------------------------------------------------- */

class ControllersWindow : public QWidget {
public:
    ControllersWindow(a800session *session, QWidget *parent);
    void present();
    void refresh();

protected:
    void keyPressEvent(QKeyEvent *evt) override;
    void keyReleaseEvent(QKeyEvent *evt) override;
    void showEvent(QShowEvent *evt) override;
    void hideEvent(QHideEvent *evt) override;
    void changeEvent(QEvent *evt) override;
    /* Tab is the Atari's TAB, not focus traversal. */
    bool focusNextPrevChild(bool) override { return false; }

private:
    void letGo();
    void onKey(int pos, bool down);
    void setMapState(int state);
    void bound(int target, const QString &what, const QString &stolen);
    void pollCapture();
    void pollHeld();
    void pollSlow();
    void rebuildPads();
    void refreshPadPorts();
    static bool keyboardDefault();

    a800session *m_session;
    StageWidget *m_stage = nullptr;
    KeyboardWidget *m_keyboard = nullptr;
    QWidget *m_padArea = nullptr;
    QVBoxLayout *m_padBox = nullptr;
    std::vector<QWidget *> m_padRows;
    std::vector<QLabel *> m_padEff;                 /* each pad's "drives port N" */
    unsigned m_padGen = ~0u;
    QPushButton *m_mapButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    QComboBox *m_targetList = nullptr;
    ElidedLabel *m_hint = nullptr;
    QTimer m_captureTimer;
    QTimer m_heldTimer;
    QTimer m_slowTimer;
    int m_mouseKey = -1;        /* on-screen keyboard index the mouse holds */
    uint32_t m_swallow = 0;     /* the key Map took: its repeats and release too */
    /* -2 idle, -1 armed and waiting for a target, >=0 waiting for a key or gamepad button. */
    int m_mapState = -2;
};

bool ControllersWindow::keyboardDefault()
{
    /* the keyboard shows by default only where it fits under the ports */
    const QScreen *screen = QGuiApplication::primaryScreen();
    return screen && screen->availableGeometry().height() >= 1200;
}

ControllersWindow::ControllersWindow(a800session *session, QWidget *parent)
    /* A dialog-type window, so a tiling compositor floats it: it is a panel of fixed-size parts and
     * nothing here wants more room. */
    : QWidget(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::CustomizeWindowHint),
      m_session(session)
{
    setObjectName(QStringLiteral("controllers"));
    setWindowTitle(QStringLiteral("Controllers"));
    setProperty("a800HelpContext", QStringLiteral("controllers"));
    setFocusPolicy(Qt::StrongFocus);

    auto *root = new QVBoxLayout(this);
    /* Sized to its parts and kept there: a resizable panel gets tiled and its parts stretched. */
    root->setSizeConstraint(QLayout::SetFixedSize);

    m_stage = new StageWidget(
        session,
        [this](int target) { setMapState(target); },
        [this](const QString &text) { m_hint->setText(text); });
    root->addWidget(m_stage, 0, Qt::AlignHCenter);

    m_keyboard = new KeyboardWidget(m_session, [this](int pos, bool down) { onKey(pos, down); });
    m_keyboard->load();
    root->addWidget(m_keyboard, 0, Qt::AlignHCenter);

    /* Gamepads: one row per pad, rebuilt as they come and go, in room for a fixed number so the
     * window does not change size when one is plugged in */
    auto *pads = new QGroupBox(QStringLiteral("Gamepads"));
    auto *padsBox = new QVBoxLayout(pads);
    m_padArea = new QWidget;
    m_padBox = new QVBoxLayout(m_padArea);
    m_padBox->setContentsMargins(0, 0, 0, 0);
    m_padBox->setSpacing(2);
    const int rowHeight = QComboBox().sizeHint().height() + 2;
    m_padArea->setFixedHeight(rowHeight * kMaxPadRows + m_padBox->spacing() * (kMaxPadRows - 1));
    m_padBox->setAlignment(Qt::AlignTop);
    padsBox->addWidget(m_padArea);
    root->addWidget(pads);

    auto *mapRow = new QHBoxLayout;
    m_mapButton = new QPushButton(QStringLiteral("Map"));
    m_mapButton->setFocusPolicy(Qt::NoFocus);
    m_mapButton->setFixedWidth(qMax(m_mapButton->sizeHint().width(),
                                    m_mapButton->fontMetrics().horizontalAdvance(QStringLiteral("Done")) + 32));
    m_mapButton->setToolTip(QStringLiteral("Pick a control, then press a key or a gamepad button to give it"));
    connect(m_mapButton, &QPushButton::clicked, this, [this] { setMapState(m_mapState == -2 ? -1 : -2); });
    m_clearButton = new QPushButton(QStringLiteral("Clear"));
    m_clearButton->setFocusPolicy(Qt::NoFocus);
    m_clearButton->setToolTip(QStringLiteral("Take the picked control's key and gamepad button away"));
    connect(m_clearButton, &QPushButton::clicked, this, [this] {
        if (m_mapState < 0) return;
        a800session_binding_clear(m_session, m_mapState);
        m_hint->setText(QStringLiteral("Cleared. Press a key or a gamepad button to bind it again"));
        m_stage->update();
        m_keyboard->update();
    });
    auto *defaults = new QPushButton(QStringLiteral("Defaults"));
    defaults->setFocusPolicy(Qt::NoFocus);
    defaults->setToolTip(QStringLiteral("Put every key and gamepad button back to its default"));
    connect(defaults, &QPushButton::clicked, this, [this] {
        a800session_bindings_reset(m_session);
        setMapState(m_mapState == -2 ? -2 : -1);
        m_hint->setText(QStringLiteral("Every control is back to its default."));
    });
    /* all 30 controls, for the ones the picture is not showing (the other ports, the system actions) */
    m_targetList = new QComboBox;
    m_targetList->setObjectName(QStringLiteral("targetList"));
    m_targetList->setFocusPolicy(Qt::NoFocus);
    m_targetList->setFixedWidth(230);
    m_targetList->setMaxVisibleItems(24);
    m_targetList->addItem(QStringLiteral("Pick a control…"), -1);
    for (int target = 0; target < A800_TARGET_COUNT; ++target)
        m_targetList->addItem(local(a800_target_name(target)), target);
    m_targetList->setToolTip(QStringLiteral("In Map mode: choose any control, including ones not on the picture"));
    connect(m_targetList, &QComboBox::activated, this, [this](int idx) {
        const int target = m_targetList->itemData(idx).toInt();
        if (target >= 0 && m_mapState != -2) setMapState(target);
        m_targetList->setCurrentIndex(0);
    });
    mapRow->addWidget(m_mapButton);
    mapRow->addWidget(m_clearButton);
    mapRow->addWidget(defaults);
    mapRow->addWidget(m_targetList);
    mapRow->addStretch(1);
    mapRow->addWidget(a800HelpButton(QStringLiteral("controllers")));
    root->addLayout(mapRow);

    auto *optRow = new QHBoxLayout;
    auto *bindings = new QCheckBox(QStringLiteral("Show bindings"));
    bindings->setObjectName(QStringLiteral("showBindings"));
    bindings->setFocusPolicy(Qt::NoFocus);
    bindings->setToolTip(QStringLiteral("Show the key each control has (hover for its gamepad button)"));
    bindings->setChecked(a800session_get_int(session, "controllers_show_bindings", 0) > 0);
    connect(bindings, &QCheckBox::toggled, this, [this](bool checked) {
        a800session_set_int(m_session, "controllers_show_bindings", checked ? 1 : 0);
        m_stage->setShowBindings(checked);
    });
    m_stage->setShowBindings(bindings->isChecked());
    auto *keyboard = new QCheckBox(QStringLiteral("Keyboard"));
    keyboard->setObjectName(QStringLiteral("showKeyboard"));
    keyboard->setFocusPolicy(Qt::NoFocus);
    keyboard->setToolTip(QStringLiteral("The machine's own keyboard, under the ports"));
    keyboard->setChecked(a800session_get_int(session, "controllers_keyboard", keyboardDefault() ? 1 : 0) > 0);
    m_keyboard->setVisible(keyboard->isChecked());
    connect(keyboard, &QCheckBox::toggled, this, [this](bool checked) {
        a800session_set_int(m_session, "controllers_keyboard", checked ? 1 : 0);
        if (!checked) letGo();
        m_keyboard->setVisible(checked);
    });
    optRow->addWidget(bindings);
    optRow->addWidget(keyboard);
    optRow->addStretch(1);
    root->addLayout(optRow);

    m_hint = new ElidedLabel(true, 2);
    root->addWidget(m_hint);

    connect(&m_captureTimer, &QTimer::timeout, this, [this] { pollCapture(); });
    connect(&m_heldTimer, &QTimer::timeout, this, [this] { pollHeld(); });
    connect(&m_slowTimer, &QTimer::timeout, this, [this] { pollSlow(); });
    setMapState(-2);
    letGo();
    m_stage->pollHeld();
}

/* Lets go of everything the mouse holds in this window. */
void ControllersWindow::letGo()
{
    if (m_mouseKey >= 0) {
        a800session_keyboard_press(m_session, m_mouseKey, 0);
        m_mouseKey = -1;
    }
    m_keyboard->releaseMouse();
    m_stage->releaseAll();
}

/* A key of the on-screen keyboard pressed or let go. In Map mode a console key picks its target. */
void ControllersWindow::onKey(int pos, bool down)
{
    const int index = m_keyboard->key(pos).index;
    if (m_mapState != -2) {
        if (down && m_keyboard->target(pos) >= 0) setMapState(m_keyboard->target(pos));
        return;
    }
    if (down) {
        if (m_mouseKey >= 0) a800session_keyboard_press(m_session, m_mouseKey, 0);
        m_mouseKey = index;
        a800session_keyboard_press(m_session, index, 1);
    } else if (m_mouseKey == index) {
        m_mouseKey = -1;
        a800session_keyboard_press(m_session, index, 0);
    }
    pollHeld();
}

/* ---- Map mode -------------------------------------------------------------------------- */

void ControllersWindow::setMapState(int state)
{
    m_mapState = state;
    m_mapButton->setStyleSheet(state != -2 ? QStringLiteral("background: %1; color: white;").arg(a800AccentColor().name())
                                           : QString());
    m_clearButton->setEnabled(state >= 0);
    m_targetList->setEnabled(state != -2);
    if (state == -2) {
        m_captureTimer.stop();
        a800session_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Map"));
        m_hint->setText(QStringLiteral("Press a control with the mouse to hold it. Drag the stick, a knob or a mat; "
                                       "SHIFT and CTRL latch for the next key."));
    } else if (state == -1) {
        m_captureTimer.stop();
        a800session_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Done"));
        m_hint->setText(QStringLiteral("Click a joystick direction (a side of the ring), a fire button or a console key "
                                       "to remap it, or pick one from the list. Escape leaves Map."));
    } else {
        letGo();
        m_hint->setText(QStringLiteral("Press a key or a gamepad button for %1 (Escape cancels).")
                            .arg(local(a800_target_name(state))));
        a800session_gamepad_capture_begin(m_session);
        m_captureTimer.start(50);
    }
    m_stage->setMap(m_mapState);
    m_keyboard->setMapState(m_mapState);
    m_stage->pollHeld();
}

/* A binding was made: stay armed, since remapping several in a row is normal. */
void ControllersWindow::bound(int target, const QString &what, const QString &stolen)
{
    setMapState(-1);
    m_hint->setText(stolen.isEmpty() ? QStringLiteral("%1 is now %2").arg(local(a800_target_name(target)), what)
                                     : QStringLiteral("%1 is now %2 (taken from %3)")
                                           .arg(local(a800_target_name(target)), what, stolen));
}

void ControllersWindow::pollCapture()
{
    int button;
    if (m_mapState < 0) { m_captureTimer.stop(); return; }
    if (!a800session_gamepad_capture_poll(m_session, &button)) return;
    char stolen[128] = "";
    const int target = m_mapState;
    a800session_binding_set_button(m_session, target, button, stolen, sizeof stolen);
    bound(target, local(a800_pad_button_name(button)), local(stolen));
}

/* ---- polling ---------------------------------------------------------------------------- */

/* What the Atari sees held, from every source, lit (~30 Hz). */
void ControllersWindow::pollHeld()
{
    m_stage->pollHeld();
    if (m_keyboard->isVisible()) m_keyboard->pollHeld(m_mouseKey);
}

/* Hot-plugged pads, auto-assignment changes, and the machine or a port's device changed in Preferences. */
void ControllersWindow::pollSlow()
{
    const unsigned gen = a800session_gamepad_generation(m_session);
    if (gen != m_padGen) {
        m_padGen = gen;
        rebuildPads();
    } else {
        refreshPadPorts();
    }
    m_stage->pollHeld();
    if (m_keyboard->load()) letGo();
}

/* ---- gamepads --------------------------------------------------------------------------- */

void ControllersWindow::refreshPadPorts()
{
    for (int i = 0; i < (int)m_padEff.size(); ++i) {
        const int eff = a800session_gamepad_effective_port(m_session, i);
        const QString text = eff >= 0 ? QStringLiteral("drives port %1").arg(eff + 1) : QStringLiteral("drives nothing");
        if (m_padEff[(size_t)i]->text() != text) m_padEff[(size_t)i]->setText(text);
    }
}

/* One row per pad: its name, the port it drives and the port it is set to. */
void ControllersWindow::rebuildPads()
{
    for (QWidget *row : m_padRows) delete row;
    m_padRows.clear();
    m_padEff.clear();
    const int count = a800session_gamepad_count(m_session);
    if (count == 0) {
        auto *none = new QLabel(QStringLiteral("No gamepads connected - plug one in"));
        none->setStyleSheet(QStringLiteral("color: gray;"));
        m_padBox->addWidget(none);
        m_padRows.push_back(none);
        return;
    }
    for (int i = 0; i < count && i < kMaxPadRows; ++i) {
        char name[128];
        a800session_gamepad_name(m_session, i, name, sizeof name);
        auto *row = new QWidget;
        auto *line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        auto *label = new ElidedLabel(false);
        label->setText(local(name));
        label->setToolTip(local(name));
        auto *eff = new QLabel;
        eff->setStyleSheet(QStringLiteral("color: gray;"));
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
        line->addWidget(label, 1);
        line->addWidget(eff);
        line->addWidget(box);
        m_padBox->addWidget(row);
        m_padRows.push_back(row);
        m_padEff.push_back(eff);
    }
    if (count > kMaxPadRows) {
        /* the rest are in Preferences ▸ Input */
        m_padEff.back()->setToolTip(QStringLiteral("%1 more gamepads are listed in Preferences").arg(count - kMaxPadRows));
    }
    refreshPadPorts();
}

/* ---- the window ------------------------------------------------------------------------- */

void ControllersWindow::present()
{
    m_padGen = a800session_gamepad_generation(m_session);
    m_stage->pollHeld();
    m_keyboard->load();
    rebuildPads();
    show();
    raise();
}

void ControllersWindow::refresh()
{
    if (isVisible()) {
        letGo();
        m_stage->pollHeld();
        m_keyboard->load();
    }
}

void ControllersWindow::showEvent(QShowEvent *evt)
{
    pollHeld();
    m_heldTimer.start(30);
    m_slowTimer.start(250);
    QWidget::showEvent(evt);
}

void ControllersWindow::hideEvent(QHideEvent *evt)
{
    m_heldTimer.stop();
    m_slowTimer.stop();
    setMapState(-2);
    letGo();
    QWidget::hideEvent(evt);
}

/* Only what this window's mouse holds: the main window releases its own keys. */
void ControllersWindow::changeEvent(QEvent *evt)
{
    if (evt->type() == QEvent::ActivationChange && !isActiveWindow()) letGo();
    QWidget::changeEvent(evt);
}

void ControllersWindow::keyPressEvent(QKeyEvent *evt)
{
    /* F9 normally reaches the app's own shortcut first; with no main window it is handled here */
    if (evt->key() == Qt::Key_F9) {
        if (!evt->isAutoRepeat()) hide();
        return;
    }
    if (evt->key() == Qt::Key_F11 || evt->key() == Qt::Key_F12) return;   /* the main window's */
    const uint32_t keysym = a800KeysymFromQt(evt);
    if (m_swallow && keysym == m_swallow) return;

    if (m_mapState != -2) {
        if (evt->isAutoRepeat()) return;
        /* Esc backs out one step: a pending target, then Map itself */
        if (evt->key() == Qt::Key_Escape) {
            m_swallow = keysym;
            setMapState(m_mapState >= 0 ? -1 : -2);
            return;
        }
        if (m_mapState < 0 || !keysym) return;      /* armed, or nothing to bind: nothing is typed */
        if (evt->modifiers() & Qt::ControlModifier) {
            m_hint->setText(QStringLiteral("Ctrl chords belong to the app: they cannot be mapped."));
            return;
        }
        if (a800session_key_reserved(keysym)) {
            m_hint->setText(QStringLiteral("F9, F11 and F12 belong to the app: they cannot be mapped."));
            return;
        }
        char name[48] = "", stolen[128] = "";
        const int target = m_mapState;
        a800session_binding_set_key(m_session, target, keysym, stolen, sizeof stolen);
        if (a800session_binding_get(m_session, target).keysym != keysym) {
            m_hint->setText(QStringLiteral("That key cannot be mapped."));
            return;
        }
        a800session_keysym_name(keysym, name, sizeof name);
        m_swallow = keysym;
        bound(target, local(name), local(stolen));
        return;
    }
    /* Otherwise what the main window does with it; a system action goes to the main window's queue,
     * as a gamepad's does. */
    const int sa = a800KeySysaction(m_session, evt);
    if (sa >= 0) {
        a800session_sysaction_post(m_session, sa);
        return;
    }
    if (!a800ForwardKey(m_session, evt, true)) QWidget::keyPressEvent(evt);
}

/* A release always goes to the session, in Map mode too: it only lets go of what a press asserted,
 * so a key held while Map was armed cannot stick. */
void ControllersWindow::keyReleaseEvent(QKeyEvent *evt)
{
    if (evt->isAutoRepeat()) return;
    const uint32_t keysym = a800KeysymFromQt(evt);
    if (m_swallow && keysym == m_swallow) {
        m_swallow = 0;
        return;
    }
    if (!a800ForwardKey(m_session, evt, false)) QWidget::keyReleaseEvent(evt);
}

QPointer<ControllersWindow> g_window;

} // namespace

void a800ControllersToggle(QWidget *parent, a800session *session)
{
    if (!g_window) g_window = new ControllersWindow(session, parent);
    if (g_window->isVisible()) g_window->hide();
    else g_window->present();
}

void a800ControllersShow(QWidget *parent, a800session *session)
{
    if (!g_window) g_window = new ControllersWindow(session, parent);
    if (!g_window->isVisible()) g_window->present();
    else g_window->raise();
}

bool a800ControllersVisible()
{
    return g_window && g_window->isVisible();
}

void a800ControllersRefresh()
{
    if (g_window) g_window->refresh();
}
