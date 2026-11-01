/*
 * DisplayWidget -- see DisplayWidget.h.
 *
 * Frames are pulled by serial: copy_frame does nothing when the Atari has
 * not drawn anything new (an identical frame is not published), and the
 * session's frame callback (FrameFeed) is what wakes the GUI thread, so a
 * still screen costs nothing.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DisplayWidget.h"

#include <QEvent>
#include <QFile>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QVector4D>
#include <QWindow>

/* A wrong resource path gives an empty shader, a pipeline that fails to
 * build and a silent fall back to the software display (it did, in every
 * sibling once): say so. */
static QShader loadShader(const char *path)
{
    QFile f(QString::fromLatin1(path));
    QShader s = f.open(QIODevice::ReadOnly) ? QShader::fromSerialized(f.readAll()) : QShader();
    if (!s.isValid())
        qWarning("display: shader %s is missing or empty; the GPU display will fail", path);
    return s;
}

DisplayWidget::DisplayWidget(a800session *session, QWidget *parent)
    : QRhiWidget(parent), m_session(session), m_feed(new FrameFeed(session, this)),
      m_pointer(session)
{
    m_fb.assign((size_t)A800SESSION_FB_MAX_WIDTH * A800SESSION_FB_MAX_HEIGHT, 0);
    setMinimumSize(320, 240);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    connect(m_feed, &FrameFeed::frameReady, this, &DisplayWidget::onFrame);
}

DisplayWidget::~DisplayWidget()
{
    m_feed->setActive(false);
}

void DisplayWidget::setTvAspect(bool tv) { m_tvAspect = tv; update(); }
void DisplayWidget::setSmooth(bool smooth) { m_smooth = smooth; update(); }

quint64 DisplayWidget::frames() const
{
    return m_haveFrame ? a800session_frame_count(m_session) : 0;
}

/* The packed frame as an image of its own size (sharing the buffer). */
bool DisplayWidget::savePng(const QString &path) const
{
    return QImage(reinterpret_cast<const uchar *>(m_fb.data()), m_w, m_h, m_w * 4,
                  QImage::Format_RGB32).copy().save(path, "PNG");
}

QRectF DisplayWidget::shown() const
{
    return fitRect(QSizeF(size()), pictureAspect(m_session, m_w, m_h, m_par, m_tvAspect));
}

/* ---- idle ------------------------------------------------------------------
 * A hidden or minimised window has nobody to draw for: the feed unregisters
 * from the session, so the GUI thread gets no wakeups at all until it is
 * shown again. The machine itself keeps running (netplay must not stall). */

void DisplayWidget::updateActive()
{
    QWidget *w = window();
    m_feed->setActive(isVisible() && w && !w->isMinimized());
}

void DisplayWidget::showEvent(QShowEvent *e)
{
    QRhiWidget::showEvent(e);
    if (window())
        window()->installEventFilter(this);
    updateActive();
}

void DisplayWidget::hideEvent(QHideEvent *e)
{
    QRhiWidget::hideEvent(e);
    if (window())
        window()->removeEventFilter(this);
    updateActive();
}

bool DisplayWidget::eventFilter(QObject *watched, QEvent *e)
{
    if (e->type() == QEvent::WindowStateChange || e->type() == QEvent::Show ||
        e->type() == QEvent::Hide)
        updateActive();
    return QRhiWidget::eventFilter(watched, e);
}

bool DisplayWidget::event(QEvent *e)
{
    /* The compositor can mark a window occluded without minimising it */
    if (e->type() == QEvent::ScreenChangeInternal || e->type() == QEvent::WindowActivate)
        updateActive();
    return QRhiWidget::event(e);
}

/* ---- the pointer ------------------------------------------------------------ */

static unsigned buttonsOf(Qt::MouseButtons b)
{
    return ((b & Qt::LeftButton) ? 1u : 0u) | ((b & Qt::RightButton) ? 2u : 0u);
}

void DisplayWidget::mouseMoveEvent(QMouseEvent *e)
{
    m_pointer.at(e->position(), buttonsOf(e->buttons()), shown(), m_w, m_h);
}
void DisplayWidget::mousePressEvent(QMouseEvent *e)
{
    setFocus();
    m_pointer.at(e->position(), buttonsOf(e->buttons()), shown(), m_w, m_h);
}
void DisplayWidget::mouseReleaseEvent(QMouseEvent *e)
{
    m_pointer.at(e->position(), buttonsOf(e->buttons()), shown(), m_w, m_h);
}
void DisplayWidget::leaveEvent(QEvent *) { m_pointer.leave(); }

/* ---- frames ---------------------------------------------------------------- */

void DisplayWidget::onFrame()
{
    int w, h, par;
    /* 0x00RRGGBB is BGRA8 as it is; the frame is packed at its own width
     * from the start of the buffer */
    if (!a800session_copy_frame(m_session, m_fb.data(), &w, &h, &par, &m_serial) || w <= 0 || h <= 0)
        return;
    m_w = w;
    m_h = h;
    m_par = par > 0 ? par : 1000;
    m_haveFrame = true;
    m_dirty = true;
    update();
}

/* ---- GPU ------------------------------------------------------------------- */

void DisplayWidget::releaseResources()
{
    m_pipeline.reset();
    m_srb.reset();
    m_linear.reset();
    m_nearest.reset();
    m_tex.reset();
    m_ubuf.reset();
}

/* One texture exactly the frame's size: made again when the size changes. */
void DisplayWidget::makeTexture()
{
    m_texSize = QSize(m_w, m_h);
    m_tex.reset(m_rhi->newTexture(QRhiTexture::BGRA8, m_texSize));
    m_tex->create();
    /* a new texture is empty, and a still picture publishes nothing more */
    if (m_haveFrame)
        m_dirty = true;
}

void DisplayWidget::makeBindings()
{
    m_smoothApplied = m_smooth;
    m_srb.reset(m_rhi->newShaderResourceBindings());
    m_srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, m_ubuf.get()),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                  m_tex.get(), m_smooth ? m_linear.get() : m_nearest.get()),
    });
    m_srb->create();
}

void DisplayWidget::initialize(QRhiCommandBuffer *)
{
    if (m_rhi != rhi()) {
        releaseResources();
        m_rhi = rhi();
    }
    if (m_pipeline)
        return;

    m_ubuf.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 16));
    m_ubuf->create();
    m_nearest.reset(m_rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest,
                                      QRhiSampler::None, QRhiSampler::ClampToEdge,
                                      QRhiSampler::ClampToEdge));
    m_nearest->create();
    m_linear.reset(m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                                     QRhiSampler::None, QRhiSampler::ClampToEdge,
                                     QRhiSampler::ClampToEdge));
    m_linear->create();
    makeTexture();
    makeBindings();

    m_pipeline.reset(m_rhi->newGraphicsPipeline());
    m_pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    m_pipeline->setShaderStages({
        { QRhiShaderStage::Vertex, loadShader(":/shaders/display.vert.qsb") },
        { QRhiShaderStage::Fragment, loadShader(":/shaders/display.frag.qsb") },
    });
    m_pipeline->setVertexInputLayout({});
    m_pipeline->setShaderResourceBindings(m_srb.get());
    m_pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    if (!m_pipeline->create()) {
        m_pipeline.reset();
        emit renderFailed();
    }
}

void DisplayWidget::render(QRhiCommandBuffer *cb)
{
    if (!m_pipeline)
        return;
    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();

    bool rebind = m_smooth != m_smoothApplied;
    if (m_texSize != QSize(m_w, m_h)) {
        makeTexture();
        rebind = true;
    }
    if (rebind)
        makeBindings();

    if (m_dirty) {
        QRhiTextureSubresourceUploadDescription sub;
        const qsizetype bytes = (qsizetype)m_w * m_h * 4;
        sub.setData(QByteArray::fromRawData(reinterpret_cast<const char *>(m_fb.data()), bytes));
        sub.setDataStride(m_w * 4);
        sub.setSourceSize(QSize(m_w, m_h));
        u->uploadTexture(m_tex.get(), QRhiTextureUploadDescription({ QRhiTextureUploadEntry(0, 0, sub) }));
        m_dirty = false;
    }

    const QSize px = renderTarget()->pixelSize();
    const QRectF r = fitRect(QSizeF(px), pictureAspect(m_session, m_w, m_h, m_par, m_tvAspect));

    /* pixel rect -> clip space (y up), then through the backend's correction
     * so Vulkan's flipped y comes out the right way up */
    auto clip = [&](double x, double y) {
        return m_rhi->clipSpaceCorrMatrix() *
               QVector4D(float(x / px.width() * 2.0 - 1.0), float(1.0 - y / px.height() * 2.0), 0.f, 1.f);
    };
    const QVector4D tl = clip(r.left(), r.top());
    const QVector4D br = clip(r.right(), r.bottom());
    const float rect[4] = { tl.x(), tl.y(), br.x(), br.y() };
    u->updateDynamicBuffer(m_ubuf.get(), 0, sizeof rect, rect);

    cb->beginPass(renderTarget(), Qt::black, { 1.0f, 0 }, u);
    cb->setGraphicsPipeline(m_pipeline.get());
    cb->setViewport(QRhiViewport(0, 0, px.width(), px.height()));
    cb->setShaderResources(m_srb.get());
    cb->draw(4);
    cb->endPass();
}
