/*
 * SoftDisplayWidget -- see SoftDisplayWidget.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SoftDisplayWidget.h"

#include <QEvent>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>

SoftDisplayWidget::SoftDisplayWidget(a800session *session, QWidget *parent)
    : QWidget(parent), m_session(session), m_feed(new FrameFeed(session, this)),
      m_pointer(session)
{
    m_fb.assign((size_t)A800SESSION_FB_MAX_WIDTH * A800SESSION_FB_MAX_HEIGHT, 0);
    setMinimumSize(320, 240);
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(false);
    setMouseTracking(true);
    connect(m_feed, &FrameFeed::frameReady, this, &SoftDisplayWidget::onFrame);
}

SoftDisplayWidget::~SoftDisplayWidget()
{
    m_feed->setActive(false);
}

void SoftDisplayWidget::setTvAspect(bool tv) { m_tvAspect = tv; update(); }
void SoftDisplayWidget::setSmooth(bool smooth) { m_smooth = smooth; update(); }

quint64 SoftDisplayWidget::frames() const
{
    return m_haveFrame ? a800session_frame_count(m_session) : 0;
}

/* The packed frame as an image of its own size (sharing the buffer). */
static QImage frameView(const std::vector<uint32_t> &buf, int w, int h)
{
    return QImage(reinterpret_cast<const uchar *>(buf.data()), w, h, w * 4, QImage::Format_RGB32);
}

bool SoftDisplayWidget::savePng(const QString &path) const
{
    return frameView(m_fb, m_w, m_h).copy().save(path, "PNG");
}

QRectF SoftDisplayWidget::shown() const
{
    return fitRect(QSizeF(size()), pictureAspect(m_session, m_w, m_h, m_par, m_tvAspect));
}

void SoftDisplayWidget::updateActive()
{
    m_feed->setActive(isVisible() && window() && !window()->isMinimized());
}

void SoftDisplayWidget::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    if (window())
        window()->installEventFilter(this);
    updateActive();
}

void SoftDisplayWidget::hideEvent(QHideEvent *e)
{
    QWidget::hideEvent(e);
    if (window())
        window()->removeEventFilter(this);
    updateActive();
}

bool SoftDisplayWidget::eventFilter(QObject *watched, QEvent *e)
{
    if (e->type() == QEvent::WindowStateChange || e->type() == QEvent::Show ||
        e->type() == QEvent::Hide)
        updateActive();
    return QWidget::eventFilter(watched, e);
}

void SoftDisplayWidget::onFrame()
{
    int w, h, par;
    if (!a800session_copy_frame(m_session, m_fb.data(), &w, &h, &par, &m_serial) || w <= 0 || h <= 0)
        return;
    m_w = w;
    m_h = h;
    m_par = par > 0 ? par : 1000;
    m_haveFrame = true;
    update();
}

void SoftDisplayWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    p.setRenderHint(QPainter::SmoothPixmapTransform, m_smooth);
    p.drawImage(shown(), frameView(m_fb, m_w, m_h));
}

static unsigned buttonsOf(Qt::MouseButtons b)
{
    return ((b & Qt::LeftButton) ? 1u : 0u) | ((b & Qt::RightButton) ? 2u : 0u);
}

void SoftDisplayWidget::mouseMoveEvent(QMouseEvent *e)
{
    m_pointer.at(e->position(), buttonsOf(e->buttons()), shown(), m_w, m_h);
}
void SoftDisplayWidget::mousePressEvent(QMouseEvent *e)
{
    setFocus();
    m_pointer.at(e->position(), buttonsOf(e->buttons()), shown(), m_w, m_h);
}
void SoftDisplayWidget::mouseReleaseEvent(QMouseEvent *e)
{
    m_pointer.at(e->position(), buttonsOf(e->buttons()), shown(), m_w, m_h);
}
void SoftDisplayWidget::leaveEvent(QEvent *) { m_pointer.leave(); }
