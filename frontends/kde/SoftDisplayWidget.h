/*
 * The software display: the fallback for a machine with no usable GPU
 * device. A QWidget that scales with QPainter, fed by FrameFeed like the GPU
 * one.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QWidget>
#include <cstdint>
#include <vector>

#include "DisplayGeometry.h"
#include "DisplayView.h"
#include "FrameFeed.h"
#include "a800session.h"

class SoftDisplayWidget : public QWidget, public DisplayView {
    Q_OBJECT
public:
    explicit SoftDisplayWidget(a800session *session, QWidget *parent = nullptr);
    ~SoftDisplayWidget() override;

    QWidget *widget() override { return this; }
    void setTvAspect(bool tv) override;
    void setSmooth(bool smooth) override;
    bool savePng(const QString &path) const override;
    quint64 frames() const override;

protected:
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    bool eventFilter(QObject *watched, QEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;
    bool focusNextPrevChild(bool) override { return false; }

private:
    void onFrame();
    void updateActive();
    QRectF shown() const;

    a800session *m_session;
    FrameFeed *m_feed;
    PointerMapper m_pointer;
    std::vector<uint32_t> m_fb;   /* the session's buffer, packed at the frame's width */
    int m_w = 336, m_h = 240, m_par = 1000;
    uint64_t m_serial = 0;
    bool m_haveFrame = false;
    bool m_tvAspect = true;
    bool m_smooth = false;
};
