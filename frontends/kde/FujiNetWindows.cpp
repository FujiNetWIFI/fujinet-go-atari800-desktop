/*
 * FujiNetWindows -- see FujiNetWindows.h.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "FujiNetWindows.h"
#include "help/HelpWindow.h"

#include <QColor>
#include <QDesktopServices>
#include <QFont>
#include <QPalette>
#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QShortcut>
#include <QPointer>
#include <QScrollBar>
#include <QTextCursor>
#include <QTextDocument>
#include <memory>
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

    /* Help, and F1 for it */
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Help);
    buttons->setContentsMargins(8, 0, 8, 8);
    QWidget *w = win;
    QObject::connect(buttons, &QDialogButtonBox::helpRequested, w, [w] { a800HelpFor(w); });
    new QShortcut(QKeySequence(Qt::Key_F1), w, [w] { a800HelpFor(w); });
    win->setProperty("a800HelpContext", QStringLiteral("fujilog"));

    auto *layout = new QVBoxLayout(win);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(view);
    layout->addWidget(buttons);

    auto *timer = new QTimer(win);
    /* Append what the log gained since the last tick; the view is never
     * rebuilt, so the reader's scroll position and selection stay put and
     * only a reader at the bottom follows the new text. A FujiNet restart
     * (a new log) clears it. */
    view->setPlainText(QStringLiteral("(no FujiNet output yet)"));
    auto cursor = std::make_shared<a800_log_cursor>();
    auto placeholder = std::make_shared<bool>(true);
    constexpr int kKeepChars = 256 * 1024;
    auto refresh = [view, session, cursor, placeholder]() {
        static char buf[64 * 1024];
        for (int round = 0; round < 16; ++round) {
            int reset = 0;
            const int n = a800session_fujinet_read_log(session, cursor.get(), buf, sizeof(buf), &reset);
            if (n <= 0 && !reset)
                return;
            QScrollBar *bar = view->verticalScrollBar();
            const int value = bar->value();
            bool atEnd = value >= bar->maximum() - 2;
            if (reset || *placeholder) {
                view->clear();
                *placeholder = false;
                atEnd = true;
            }
            QTextCursor end(view->document());  // not the view's own: its selection stays
            end.movePosition(QTextCursor::End);
            end.insertText(QString::fromUtf8(buf));
            if (view->document()->characterCount() > kKeepChars) {  // whole lines
                QTextCursor cut(view->document());
                cut.setPosition(view->document()->characterCount() - kKeepChars * 3 / 4);
                cut.movePosition(QTextCursor::NextBlock);
                QTextCursor head(view->document());
                head.setPosition(0);
                head.setPosition(cut.position(), QTextCursor::KeepAnchor);
                head.removeSelectedText();
            }
            bar->setValue(atEnd ? bar->maximum() : value);
            if (n < static_cast<int>(sizeof(buf)) - 1)
                return;
        }
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
