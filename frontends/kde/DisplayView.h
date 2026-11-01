/*
 * What the main window needs from its display, so the GPU display and the
 * software fallback are interchangeable.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QString>
#include <QtGlobal>

class QWidget;

class DisplayView {
public:
    virtual ~DisplayView() = default;
    virtual QWidget *widget() = 0;
    /* The TV's shape (the frame's pixel aspect applied), or square pixels. */
    virtual void setTvAspect(bool tv) = 0;
    virtual void setSmooth(bool smooth) = 0;
    /* The last frame as a PNG. */
    virtual bool savePng(const QString &path) const = 0;
    /* Frames the machine has run, once a picture has reached the display
     * (a still screen publishes nothing new, but the count still runs). */
    virtual quint64 frames() const = 0;
};
