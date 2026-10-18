/*
 * DisplayWidget -- see DisplayWidget.h.
 *
 * Frames are pulled by serial: copy_frame does nothing when the Atari has
 * not drawn anything new. The emulator paces itself; the timer only decides
 * how soon a change is seen.
 *
 * The frame's size changes with the visible area, the NTSC filter and the
 * 80-column cards; its pixel aspect (x1000) says how wide each pixel is
 * against the Atari's own. TV aspect then gives the Atari's pixels their
 * shape on a TV (NTSC 6:7, PAL a little wide) and shows an 80-column card's
 * screen 4:3, like the monitor it drove.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DisplayWidget.h"

#include <QMouseEvent>
#include <QPainter>

DisplayWidget::DisplayWidget(a800session *session, QWidget *parent)
    : QWidget(parent), m_session(session),
      m_image(A800SESSION_FB_MAX_WIDTH, A800SESSION_FB_MAX_HEIGHT, QImage::Format_RGB32)
{
    m_image.fill(Qt::black);
    setMinimumSize(320, 240);
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(false);
    setMouseTracking(true);
    connect(&m_timer, &QTimer::timeout, this, &DisplayWidget::tick);
    m_timer.start(8);
}

void DisplayWidget::tick()
{
    int w, h, par;
    /* 0x00RRGGBB is QImage::Format_RGB32 as it is; the frame is packed at
     * its own width from the start of the buffer */
    if (a800session_copy_frame(m_session, reinterpret_cast<uint32_t *>(m_image.bits()), &w, &h,
                               &par, &m_serial) && w > 0 && h > 0) {
        m_w = w;
        m_h = h;
        m_par = par > 0 ? par : 1000;
        m_frames++;
        update();
    }
}

void DisplayWidget::setTvAspect(bool tv) { m_tvAspect = tv; update(); }
void DisplayWidget::setSmooth(bool smooth) { m_smooth = smooth; update(); }

/* The packed frame as an image of its own size (sharing the buffer). */
static QImage frameView(const QImage &buf, int w, int h)
{
    return QImage(buf.constBits(), w, h, w * 4, QImage::Format_RGB32);
}

bool DisplayWidget::savePng(const QString &path) const
{
    return frameView(m_image, m_w, m_h).copy().save(path, "PNG");
}

double DisplayWidget::pictureAspect() const
{
    const double w = m_w * m_par / 1000.0, h = m_h;
    if (!m_tvAspect)
        return w / h;
    if (m_w >= 560 && m_par == 500)
        return 4.0 / 3.0;   /* an 80-column card's monitor */
    /* an Atari pixel on an NTSC TV is 6/7 as wide as tall; on PAL, 1.03 */
    return w * (a800session_refresh_rate(m_session) > 55000 ? 6.0 / 7.0 : 1.03) / h;
}

void DisplayWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    const double want = pictureAspect();
    double w = width(), h = height(), sw, sh;
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }
    m_shown = QRectF((w - sw) / 2.0, (h - sh) / 2.0, sw, sh);
    p.setRenderHint(QPainter::SmoothPixmapTransform, m_smooth);
    p.drawImage(m_shown, frameView(m_image, m_w, m_h));
}

/* ---- the pointer: mice, Koala Pad, light pen and gun ------------------------- */

static unsigned buttonsOf(Qt::MouseButtons b)
{
    return ((b & Qt::LeftButton) ? 1u : 0u) | ((b & Qt::RightButton) ? 2u : 0u);
}

void DisplayWidget::pointerAt(const QPointF &p, unsigned buttons)
{
    if (!a800session_pointer_device_active(m_session) || m_shown.width() <= 0)
        return;
    const int fx = (int)((p.x() - m_shown.x()) * m_w / m_shown.width());
    const int fy = (int)((p.y() - m_shown.y()) * m_h / m_shown.height());
    const int inside = fx >= 0 && fy >= 0 && fx < m_w && fy < m_h;
    a800session_pointer(m_session, qBound(0, fx, m_w - 1), qBound(0, fy, m_h - 1), inside, buttons);
    QPointF at = p;
    if (m_haveLast) {
        /* mice count movement: in frame pixels, as the picture is drawn */
        const int dx = (int)((p.x() - m_last.x()) * m_w / m_shown.width());
        const int dy = (int)((p.y() - m_last.y()) * m_h / m_shown.height());
        if (dx || dy)
            a800session_pointer_motion(m_session, dx, dy, buttons);
        if (!dx) at.setX(m_last.x());
        if (!dy) at.setY(m_last.y());
    }
    m_last = at;
    m_haveLast = true;
}

void DisplayWidget::mouseMoveEvent(QMouseEvent *e) { pointerAt(e->position(), buttonsOf(e->buttons())); }
void DisplayWidget::mousePressEvent(QMouseEvent *e)
{
    setFocus();
    pointerAt(e->position(), buttonsOf(e->buttons()));
}
void DisplayWidget::mouseReleaseEvent(QMouseEvent *e) { pointerAt(e->position(), buttonsOf(e->buttons())); }

void DisplayWidget::leaveEvent(QEvent *)
{
    m_haveLast = false;
    if (a800session_pointer_device_active(m_session))
        a800session_pointer(m_session, 0, 0, 0, 0);
}
