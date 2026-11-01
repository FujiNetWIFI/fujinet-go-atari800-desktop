/*
 * The Atari's picture on the GPU: a QRhiWidget that keeps one texture the
 * size of the frame (rebuilt when the frame's size changes) and draws one
 * textured quad, only when the session has published a new frame (see
 * FrameFeed). Qt picks Vulkan, Metal, D3D or OpenGL for it; the scaling,
 * aspect and filtering all happen on the GPU.
 *
 * If no GPU device can be had it emits renderFailed() and the main window
 * swaps in the software SoftDisplayWidget.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QImage>
#include <QRhiWidget>
#include <cstdint>
#include <memory>
#include <vector>
#include <rhi/qrhi.h>

#include "DisplayGeometry.h"
#include "DisplayView.h"
#include "FrameFeed.h"
#include "a800session.h"

class DisplayWidget : public QRhiWidget, public DisplayView {
    Q_OBJECT
public:
    explicit DisplayWidget(a800session *session, QWidget *parent = nullptr);
    ~DisplayWidget() override;

    QWidget *widget() override { return this; }
    void setTvAspect(bool tv) override;
    void setSmooth(bool smooth) override;
    bool savePng(const QString &path) const override;
    quint64 frames() const override;

protected:
    void initialize(QRhiCommandBuffer *cb) override;
    void render(QRhiCommandBuffer *cb) override;
    void releaseResources() override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    bool event(QEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;
    /* Tab is the Atari's: it reaches the window's key handler instead of
     * moving the focus. */
    bool focusNextPrevChild(bool) override { return false; }

private:
    void onFrame();
    void updateActive();
    void makeTexture();
    void makeBindings();
    QRectF shown() const;

    a800session *m_session;
    FrameFeed *m_feed;
    PointerMapper m_pointer;
    std::vector<uint32_t> m_fb;   /* XRGB8888 straight from the session, packed at the frame's width */
    int m_w = 336, m_h = 240, m_par = 1000;
    uint64_t m_serial = 0;
    bool m_haveFrame = false;
    bool m_dirty = false;         /* m_fb holds a frame not yet uploaded */
    bool m_tvAspect = true;
    bool m_smooth = false;
    bool m_smoothApplied = false;

    QRhi *m_rhi = nullptr;
    QSize m_texSize;
    std::unique_ptr<QRhiBuffer> m_ubuf;
    std::unique_ptr<QRhiTexture> m_tex;
    std::unique_ptr<QRhiSampler> m_nearest, m_linear;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline> m_pipeline;
};
