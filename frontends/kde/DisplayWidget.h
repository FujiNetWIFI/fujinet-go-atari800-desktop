/*
 * The Atari's picture: a QWidget that pulls frames from the session.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QImage>
#include <QTimer>
#include <QWidget>
#include <cstdint>

#include "a800session.h"

class DisplayWidget : public QWidget {
    Q_OBJECT
public:
    explicit DisplayWidget(a800session *session, QWidget *parent = nullptr);
    /* The TV's shape (the frame's pixel aspect applied), or square pixels. */
    void setTvAspect(bool tv);
    void setSmooth(bool smooth);
    /* The last frame as a PNG. */
    bool savePng(const QString &path) const;
    quint64 frames() const { return m_frames; }

protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;
    /* Tab is the Atari's: it reaches the window's key handler instead of
     * moving the focus. */
    bool focusNextPrevChild(bool) override { return false; }

private:
    void tick();
    double pictureAspect() const;
    void pointerAt(const QPointF &p, unsigned buttons);

    a800session *m_session;
    QImage m_image;             /* the session's buffer, A800SESSION_FB_MAX_* */
    int m_w = 336, m_h = 240, m_par = 1000;
    uint64_t m_serial = 0;
    quint64 m_frames = 0;
    QTimer m_timer;
    bool m_tvAspect = true;
    bool m_smooth = false;
    QRectF m_shown;
    QPointF m_last;
    bool m_haveLast = false;
};
