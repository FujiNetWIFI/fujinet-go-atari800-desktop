/*
 * FujiNetWindows -- see FujiNetWindows.h.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "FujiNetWindows.h"

#include <QColor>
#include <QDesktopServices>
#include <QFont>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScrollBar>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

QColor a800AccentColor()
{
    return QColor((A800SESSION_ACCENT_RGB >> 16) & 0xff,
                  (A800SESSION_ACCENT_RGB >> 8) & 0xff,
                  A800SESSION_ACCENT_RGB & 0xff);
}

QColor a800AccentTextColor(const QWidget *w)
{
    /* on a dark palette the rust is lightened to #C8A27A (7:1) */
    const QColor bg = w ? w->palette().color(QPalette::Window) : QColor(Qt::white);
    return bg.lightness() < 128 ? QColor(0xc8, 0xa2, 0x7a) : a800AccentColor();
}

void fujinet_log_show(QWidget *parent, a800session *session)
{
    static QPointer<QWidget> win;
    if (win) {
        win->raise();
        win->activateWindow();
        return;
    }
    win = new QWidget(parent, Qt::Window);
    win->setAttribute(Qt::WA_DeleteOnClose);
    win->setWindowTitle(QStringLiteral("FujiNet Console Log"));
    win->resize(820, 560);

    auto *view = new QPlainTextEdit(win);
    view->setReadOnly(true);
    QFont mono = view->font();
    mono.setFamily(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    view->setFont(mono);

    auto *layout = new QVBoxLayout(win);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(view);

    auto *timer = new QTimer(win);
    auto refresh = [view, session]() {
        static char buf[128 * 1024];
        const int n = a800session_fujinet_copy_log(session, buf, sizeof(buf));
        const bool atEnd = view->verticalScrollBar()->value() ==
                           view->verticalScrollBar()->maximum();
        view->setPlainText(n > 0 ? QString::fromUtf8(buf)
                                 : QStringLiteral("(no FujiNet output yet)"));
        if (atEnd)
            view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum());
    };
    QObject::connect(timer, &QTimer::timeout, view, refresh);
    timer->start(1000);
    refresh();
    win->show();
}

void fujinet_config_show(QWidget *parent, a800session *session)
{
    (void)parent;
    QDesktopServices::openUrl(QUrl(QString::fromUtf8(a800session_fujinet_webui_url(session))));
}
