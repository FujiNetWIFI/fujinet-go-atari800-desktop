/*
 * FujiNet Go Atari 800 -- the KDE (Qt6 Widgets) frontend.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QApplication>
#include <QIcon>
#include <csignal>

#include "MainWindow.h"
#include "a800session.h"

/* SIGTERM (a logout, a service manager) and SIGINT (Ctrl+C in a terminal)
 * quit the way closing the window does, so the session stops the machine
 * and FujiNet and flushes the settings. */
static void onQuitSignal(int)
{
    QMetaObject::invokeMethod(qApp, &QApplication::quit, Qt::QueuedConnection);
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("fujinet-go-atari800-kde"));
    app.setApplicationDisplayName(QStringLiteral("FujiNet Go Atari 800"));
    app.setDesktopFileName(QStringLiteral("online.fujinet.go.atari800.kde"));
    /* The installed icon is named after the desktop-entry id; running out
     * of the build tree, the in-tree artwork stands in. */
    {
        QIcon icon = QIcon::fromTheme(QStringLiteral("online.fujinet.go.atari800.kde"));
        if (icon.isNull()) {
            QIcon::setThemeSearchPaths(QIcon::themeSearchPaths() << QStringLiteral(A800_SOURCE_ICON_DIR));
            icon = QIcon::fromTheme(QStringLiteral("fujinet-go-atari800"));
        }
        if (!icon.isNull())
            app.setWindowIcon(icon);
    }

    a800session *session = a800session_new(nullptr);
    if (!session) {
        qCritical("Could not create the session (unusable config/data dirs?)");
        return 1;
    }
    std::signal(SIGTERM, onQuitSignal);
    std::signal(SIGINT, onQuitSignal);

    int rc;
    /* the windows go before the session does: they call into it as they
     * close */
    {
        MainWindow win(session);
        win.show();
        /* FujiNet first, then the machine (the built-in OS needs no ROMs) */
        win.start();
        /* a path on the command line goes where Open would put it */
        if (argc > 1)
            win.loadMedia(QString::fromLocal8Bit(argv[1]));
        rc = app.exec();
    }
    a800session_stop(session);
    a800session_free(session);
    return rc;
}
