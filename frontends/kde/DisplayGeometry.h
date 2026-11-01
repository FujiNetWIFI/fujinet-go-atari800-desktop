/*
 * Where the picture goes inside the display, shared by the GPU and the
 * software display, and the pointer mapping back from it.
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
#pragma once

#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QtGlobal>

#include "a800session.h"

/* The picture's width over its height, as shown. */
inline double pictureAspect(a800session *session, int w, int h, int par, bool tv)
{
    const double pw = w * par / 1000.0;
    if (!tv)
        return pw / h;
    if (w >= 560 && par == 500)
        return 4.0 / 3.0;   /* an 80-column card's monitor */
    /* an Atari pixel on an NTSC TV is 6/7 as wide as tall; on PAL, 1.03 */
    return pw * (a800session_refresh_rate(session) > 55000 ? 6.0 / 7.0 : 1.03) / h;
}

/* The largest rectangle of the given aspect that fits, centred. */
inline QRectF fitRect(QSizeF avail, double want)
{
    const double w = avail.width(), h = avail.height();
    double sw, sh;
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }
    return QRectF((w - sw) / 2.0, (h - sh) / 2.0, sw, sh);
}

/* The pointer: mice, Koala Pad, light pen and gun. */
class PointerMapper {
public:
    explicit PointerMapper(a800session *session) : m_session(session) {}

    /* p in widget coordinates; shown is where the w x h frame is drawn. */
    void at(const QPointF &p, unsigned buttons, const QRectF &shown, int w, int h)
    {
        if (!a800session_pointer_device_active(m_session) || shown.width() <= 0)
            return;
        const int fx = (int)((p.x() - shown.x()) * w / shown.width());
        const int fy = (int)((p.y() - shown.y()) * h / shown.height());
        const int inside = fx >= 0 && fy >= 0 && fx < w && fy < h;
        a800session_pointer(m_session, qBound(0, fx, w - 1), qBound(0, fy, h - 1), inside, buttons);
        QPointF kept = p;
        if (m_have) {
            /* mice count movement: in frame pixels, as the picture is drawn */
            const int dx = (int)((p.x() - m_last.x()) * w / shown.width());
            const int dy = (int)((p.y() - m_last.y()) * h / shown.height());
            if (dx || dy)
                a800session_pointer_motion(m_session, dx, dy, buttons);
            if (!dx) kept.setX(m_last.x());
            if (!dy) kept.setY(m_last.y());
        }
        m_last = kept;
        m_have = true;
    }

    void leave()
    {
        m_have = false;
        if (a800session_pointer_device_active(m_session))
            a800session_pointer(m_session, 0, 0, 0, 0);
    }

private:
    a800session *m_session;
    QPointF m_last;
    bool m_have = false;
};
