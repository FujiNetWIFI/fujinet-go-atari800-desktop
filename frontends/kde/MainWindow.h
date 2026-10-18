/*
 * The main window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QLabel>
#include <QMainWindow>
#include <QTimer>

#include "a800session.h"

class DisplayWidget;
class QAction;
class QMenu;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(a800session *session, QWidget *parent = nullptr);
    /* Starts the machine (FujiNet first). */
    void start();
    /* A dropped or command-line file: Open's routing (a disk boots, a program
     * runs, a cartridge goes in, a ROM is imported ...). */
    void loadMedia(const QString &path);

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void changeEvent(QEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void closeEvent(QCloseEvent *e) override;

private:
    void buildMenus();
    void fillDrivesMenu();
    void fillCartMenu();
    void fillTapeMenu();
    void updateStatus();
    void fastTick();
    void applyView();
    void showSettings();
    void powerCycle();
    void runSysaction(int sa);
    void importRoms();
    void askCartType(const QString &path, bool piggyback);
    void insertInto(int drive, const QString &path);
    void newBlankDisk(int drive);
    void showNotice(const QString &text);
    void showAbout();
    void toast(const QString &text, int ms = 4000);
    QString mediaFolder() const;
    void rememberFolder(const QString &path);

    a800session *m_session;
    DisplayWidget *m_display = nullptr;
    QMenu *m_drives = nullptr;
    QMenu *m_cart = nullptr;
    QMenu *m_tape = nullptr;
    QMenu *m_machine = nullptr;
    QLabel *m_dot = nullptr;
    QLabel *m_status = nullptr;
    QTimer m_statusTimer;
    QTimer m_fastTimer;
    QAction *m_tvAction = nullptr;
    QAction *m_smoothAction = nullptr;
    QAction *m_col80Action = nullptr;
    QAction *m_warpAction = nullptr;
    QAction *m_rebootAction = nullptr;
    QAction *m_stateSave = nullptr;
    QAction *m_stateLoad = nullptr;
    unsigned m_padGeneration = 0;
    unsigned m_fujinetGeneration = 0;
    bool m_noticeOpen = false;
    QString m_shotPath;
    quint64 m_shotFrames = 0, m_quitFrames = 0;
};
