/*
 * MainWindow -- see MainWindow.h.
 *
 * Plain Qt6 Widgets, deliberately not KDE Frameworks: it picks up Breeze
 * through the platform theme anyway, and staying framework-free keeps this
 * frontend usable outside a KDE session.
 *
 * Keys go to the Atari pressed and released (KeyForward). The Atari's
 * CONTROL+letter keys are its graphics characters, so the app's shortcuts
 * are CTRL+SHIFT ones, plus F9, F11, F12 and Shift+F5 -- application-wide,
 * so they work from the Controllers panel and the debugger too.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "MainWindow.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QStatusBar>
#include <QUrl>
#include <QVBoxLayout>

#include "DisplayWidget.h"
#include "FujiNetWindows.h"
#include "KeyForward.h"
#include "SettingsDialog.h"
#include "controllers/ControllersWindow.h"
#include "debugger/DebuggerWindow.h"

static const char *const kAnyFilter =
    "Atari disks, programs, cartridges, tapes (*.atr *.xfd *.atx *.pro *.dcm *.xex *.com *.exe "
    "*.bas *.lst *.car *.rom *.bin *.cas *.a8s *.zip *.gz *.atz);;All files (*)";
static const char *const kDiskFilter =
    "Atari disk images (*.atr *.xfd *.atx *.pro *.dcm *.zip *.gz *.atz);;All files (*)";
static const char *const kCartFilter =
    "Cartridge images (*.car *.rom *.bin *.zip);;All files (*)";
static const char *const kTapeFilter = "Cassette images (*.cas *.zip);;All files (*)";
static const char *const kStateFilter = "Save states (*.a8s);;All files (*)";

static QString local(const char *s) { return QString::fromUtf8(s ? s : ""); }

MainWindow::MainWindow(a800session *session, QWidget *parent)
    : QMainWindow(parent), m_session(session)
{
    setWindowTitle(QStringLiteral("FujiNet Go Atari 800"));
    /* 336 x 240 at three times, a TV's shape */
    resize(864, 720 + 60);
    setAcceptDrops(true);

    m_display = new DisplayWidget(session, this);
    setCentralWidget(m_display);
    m_display->setFocus();

    m_dot = new QLabel(QStringLiteral("●"));
    m_dot->setToolTip(QStringLiteral("FujiNet link"));
    m_status = new QLabel(QStringLiteral("Starting..."));
    statusBar()->addWidget(m_dot);
    statusBar()->addWidget(m_status, 1);

    buildMenus();
    applyView();

    connect(&m_statusTimer, &QTimer::timeout, this, &MainWindow::updateStatus);
    m_statusTimer.start(1000);
    connect(&m_fastTimer, &QTimer::timeout, this, &MainWindow::fastTick);
    m_fastTimer.start(100);
    /* the current pads and FujiNet state are the baseline: no messages */
    m_padGeneration = a800session_gamepad_generation(session);
    m_fujinetGeneration = a800session_fujinet_generation(session);
    updateStatus();

    /* A800_OPEN_CONTROLLERS / _DEBUGGER / _SETTINGS open those windows at
     * launch; A800_SCREENSHOT=file.png saves the picture after
     * A800_SCREENSHOT_FRAMES frames (300); A800_AUTOQUIT_FRAMES quits. */
    if (qEnvironmentVariableIsSet("A800_OPEN_CONTROLLERS"))
        QTimer::singleShot(0, this, [this] { a800ControllersToggle(this, m_session); });
    if (qEnvironmentVariableIsSet("A800_OPEN_DEBUGGER"))
        QTimer::singleShot(0, this, [this] { a800DebuggerShow(this, m_session); });
    if (qEnvironmentVariableIsSet("A800_OPEN_SETTINGS"))
        QTimer::singleShot(0, this, &MainWindow::showSettings);
    m_shotPath = qEnvironmentVariable("A800_SCREENSHOT");
    m_shotFrames = qEnvironmentVariableIsSet("A800_SCREENSHOT_FRAMES")
                       ? qEnvironmentVariable("A800_SCREENSHOT_FRAMES").toULongLong() : 300;
    m_quitFrames = qEnvironmentVariable("A800_AUTOQUIT_FRAMES").toULongLong();
}

void MainWindow::toast(const QString &text, int ms)
{
    statusBar()->showMessage(text, ms);
}

/* ---- menus ---------------------------------------------------------------------- */

static QAction *appShortcut(QWidget *owner, QAction *a, const QKeySequence &k)
{
    /* application-wide: the panels have the keyboard too */
    a->setShortcut(k);
    a->setShortcutContext(Qt::ApplicationShortcut);
    owner->addAction(a);
    return a;
}

void MainWindow::buildMenus()
{
    m_machine = menuBar()->addMenu(QStringLiteral("&Machine"));
    m_machine->addAction(QStringLiteral("&Open..."), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O), this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, QStringLiteral("Open"), mediaFolder(),
                                                       QString::fromUtf8(kAnyFilter));
        if (!f.isEmpty())
            loadMedia(f);
    });
    m_machine->addSeparator();
    appShortcut(this, m_machine->addAction(QStringLiteral("&Power Cycle"), this, &MainWindow::powerCycle),
                QKeySequence(Qt::SHIFT | Qt::Key_F5));
    /* F5 itself is the Atari's RESET key through the bindings */
    m_machine->addAction(QStringLiteral("&Reset (F5)"), this, [this] { a800session_reset(m_session); });
    m_rebootAction = m_machine->addAction(QStringLiteral("Reboot to &CONFIG"), this, [this] {
        if (a800session_reboot_to_config(m_session) == 0)
            toast(QStringLiteral("Back to FujiNet's CONFIG"));
    });
    m_warpAction = m_machine->addAction(QStringLiteral("&Warp Speed"));
    m_warpAction->setCheckable(true);
    m_warpAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_W));
    connect(m_warpAction, &QAction::triggered, this, [this](bool on) {
        a800session_set_warp(m_session, on);
        updateStatus();
    });
    m_machine->addSeparator();
    m_stateSave = m_machine->addAction(QStringLiteral("&Save State..."), this, [this] {
        const QString dir = local(a800session_data_path(m_session)) + QStringLiteral("/states");
        QDir().mkpath(dir);
        const QString f = QFileDialog::getSaveFileName(this, QStringLiteral("Save State"),
                                                       dir + QStringLiteral("/atari.a8s"),
                                                       QString::fromUtf8(kStateFilter));
        if (f.isEmpty())
            return;
        if (a800session_state_save(m_session, f.toLocal8Bit().constData()) != 0)
            QMessageBox::warning(this, QStringLiteral("Cannot Save the State"), local(a800session_last_error(m_session)));
        else
            toast(QStringLiteral("State saved"));
    });
    m_stateLoad = m_machine->addAction(QStringLiteral("&Load State..."), this, [this] {
        const QString dir = local(a800session_data_path(m_session)) + QStringLiteral("/states");
        const QString f = QFileDialog::getOpenFileName(this, QStringLiteral("Load State"), dir,
                                                       QString::fromUtf8(kStateFilter));
        if (f.isEmpty())
            return;
        if (a800session_state_load(m_session, f.toLocal8Bit().constData()) != 0)
            QMessageBox::warning(this, QStringLiteral("Cannot Load the State"), local(a800session_last_error(m_session)));
        else
            toast(QStringLiteral("State loaded"));
    });
    m_machine->addAction(QStringLiteral("Import System &ROMs..."), this, &MainWindow::importRoms);
    m_machine->addSeparator();
    m_machine->addAction(QStringLiteral("P&references..."), QKeySequence(Qt::CTRL | Qt::Key_Comma), this,
                         &MainWindow::showSettings);
    m_machine->addSeparator();
    m_machine->addAction(QStringLiteral("&Quit"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Q), this,
                         [this] { close(); });
    connect(m_machine, &QMenu::aboutToShow, this, [this] {
        const bool fuji = a800session_fujinet_running(m_session);
        m_rebootAction->setVisible(fuji);
        m_stateSave->setEnabled(a800session_states_available(m_session));
        m_stateLoad->setEnabled(a800session_states_available(m_session));
        m_warpAction->setChecked(a800session_warp(m_session));
    });

    m_drives = menuBar()->addMenu(QStringLiteral("&Drives"));
    connect(m_drives, &QMenu::aboutToShow, this, &MainWindow::fillDrivesMenu);
    fillDrivesMenu();
    m_cart = menuBar()->addMenu(QStringLiteral("&Cartridge"));
    connect(m_cart, &QMenu::aboutToShow, this, &MainWindow::fillCartMenu);
    fillCartMenu();
    m_tape = menuBar()->addMenu(QStringLiteral("&Program Recorder"));
    connect(m_tape, &QMenu::aboutToShow, this, &MainWindow::fillTapeMenu);
    fillTapeMenu();

    QMenu *console = menuBar()->addMenu(QStringLiteral("C&onsole"));
    struct { const char *label; int key; } keys[] = {
        { "&Option (F2)", A800_CON_OPTION }, { "&Select (F3)", A800_CON_SELECT },
        { "S&tart (F4)", A800_CON_START }, { "&Help (F6)", A800_CON_HELP },
        { "&Break (F7, Pause)", A800_CON_BREAK },
    };
    for (const auto &k : keys) {
        const int key = k.key;
        /* held for a tenth of a second: long enough for the OS's look */
        console->addAction(QString::fromUtf8(k.label), this, [this, key] {
            a800session_console(m_session, key, 1);
            QTimer::singleShot(120, this, [this, key] { a800session_console(m_session, key, 0); });
        });
    }

    QMenu *edit = menuBar()->addMenu(QStringLiteral("&Edit"));
    edit->addAction(QStringLiteral("&Copy Screen Text"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C), this, [this] {
        static char buf[8192];
        if (a800session_copy_screen_text(m_session, buf, sizeof buf) <= 0) {
            toast(QStringLiteral("No text screen to copy"));
            return;
        }
        QApplication::clipboard()->setText(QString::fromUtf8(buf));
        toast(QStringLiteral("The screen's text is on the clipboard"), 3000);
    });
    edit->addAction(QStringLiteral("&Paste"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V), this, [this] {
        const QString t = QApplication::clipboard()->text();
        if (!t.isEmpty())
            a800session_paste(m_session, t.toUtf8().constData());
    });

    QMenu *view = menuBar()->addMenu(QStringLiteral("&View"));
    appShortcut(this, view->addAction(QStringLiteral("C&ontrollers"), this,
                                      [this] { a800ControllersToggle(this, m_session); }),
                QKeySequence(Qt::Key_F9));
    appShortcut(this, view->addAction(QStringLiteral("&Debugger"), this,
                                      [this] { a800DebuggerToggle(this, m_session); }),
                QKeySequence(Qt::Key_F12));
    view->addSeparator();
    m_col80Action = view->addAction(QStringLiteral("&80-Column Display"));
    m_col80Action->setCheckable(true);
    connect(m_col80Action, &QAction::triggered, this,
            [this](bool on) { a800session_set_80col_display(m_session, on); });
    m_tvAction = view->addAction(QStringLiteral("&TV Aspect"));
    m_tvAction->setCheckable(true);
    m_tvAction->setToolTip(QStringLiteral("A TV's shape; off shows square pixels"));
    connect(m_tvAction, &QAction::triggered, this, [this](bool on) {
        const int i = a800session_pref_find("aspect");
        if (i >= 0)
            a800session_pref_set_choice(m_session, i, on ? 0 : 1);
        applyView();
    });
    m_smoothAction = view->addAction(QStringLiteral("&Smooth Scaling"));
    m_smoothAction->setCheckable(true);
    connect(m_smoothAction, &QAction::triggered, this, [this](bool on) {
        a800session_set_int(m_session, "smooth", on);
        applyView();
    });
    appShortcut(this, view->addAction(QStringLiteral("&Fullscreen"), this, [this] {
        if (isFullScreen()) showNormal(); else showFullScreen();
    }), QKeySequence(Qt::Key_F11));
    connect(view, &QMenu::aboutToShow, this, [this] {
        m_col80Action->setVisible(a800session_80col_fitted(m_session));
        m_col80Action->setChecked(a800session_80col_display(m_session));
    });

    QMenu *fuji = menuBar()->addMenu(QStringLiteral("&FujiNet"));
    fuji->addAction(QStringLiteral("&Web UI"), this, [this] {
        if (!a800session_fujinet_running(m_session)) {
            toast(QStringLiteral("FujiNet is off (Preferences ▸ FujiNet)"));
            return;
        }
        fujinet_config_show(this, m_session);
    });
    fuji->addAction(QStringLiteral("Console &Log"), this, [this] { fujinet_log_show(this, m_session); });
    fuji->addAction(QStringLiteral("&Restart FujiNet"), this, [this] {
        if (a800session_fujinet_restart(m_session) != 0)
            QMessageBox::warning(this, QStringLiteral("Cannot Restart FujiNet"),
                                 local(a800session_last_error(m_session)));
        else
            toast(QStringLiteral("FujiNet restarted"), 3000);
    });

    QMenu *help = menuBar()->addMenu(QStringLiteral("&Help"));
    help->addAction(QStringLiteral("&About FujiNet Go Atari 800"), this, &MainWindow::showAbout);
}

/* Rebuilt each time it opens: with FujiNet on the drives are FujiNet's. */
void MainWindow::fillDrivesMenu()
{
    const bool fuji = a800session_fujinet_running(m_session);
    m_drives->clear();
    m_drives->addSection(fuji ? QStringLiteral("FujiNet's drives") : QStringLiteral("atari800's drives"));
    for (int d = 0; d < A800_DRIVES; ++d) {
        a800_drive_info di;
        if (a800session_drive_info(m_session, d, &di) != 0)
            memset(&di, 0, sizeof di);
        const QString file = di.path[0] ? QFileInfo(local(di.path)).fileName() : QStringLiteral("empty");
        QMenu *sub = m_drives->addMenu(QStringLiteral("D%1: %2%3").arg(d + 1).arg(file,
            di.path[0] && di.read_only ? QStringLiteral(" (protected)") : QString()));
        sub->addAction(fuji ? QStringLiteral("&Mount from the SD Card...") : QStringLiteral("&Insert..."), this,
                       [this, d, fuji] {
            const QString dir = fuji ? local(a800session_sd_path(m_session)) : mediaFolder();
            const QString f = QFileDialog::getOpenFileName(this, QStringLiteral("D%1:").arg(d + 1), dir,
                                                           QString::fromUtf8(kDiskFilter));
            if (!f.isEmpty())
                insertInto(d, f);
        });
        sub->addAction(QStringLiteral("&New Blank Disk..."), this, [this, d] { newBlankDisk(d); });
        if (di.path[0]) {
            sub->addAction(QStringLiteral("&Eject"), this, [this, d] { a800session_drive_eject(m_session, d); });
            if (!fuji) {
                QAction *wp = sub->addAction(QStringLiteral("&Write Protect"));
                wp->setCheckable(true);
                wp->setChecked(di.read_only);
                connect(wp, &QAction::triggered, this,
                        [this, d](bool on) { a800session_drive_set_read_only(m_session, d, on); });
            }
        }
    }
    m_drives->addSeparator();
    if (fuji) {
        m_drives->addAction(QStringLiteral("&Import to SD Card..."), this, [this] {
            const QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("Import to FujiNet's SD Card"),
                                                                    mediaFolder(), QString::fromUtf8(kAnyFilter));
            int ok = 0, failed = 0;
            for (const QString &f : files) {
                char dest[1024];
                if (a800session_import_to_sd(m_session, f.toLocal8Bit().constData(), dest, sizeof dest) == 0)
                    ok++;
                else
                    failed++;
            }
            if (!files.isEmpty())
                toast(failed ? QStringLiteral("%1 copied to FujiNet's SD card, %2 could not be").arg(ok).arg(failed)
                             : QStringLiteral("%1 copied to FujiNet's SD card: mount from CONFIG or Drives").arg(ok));
        });
        m_drives->addAction(QStringLiteral("Open &SD Folder"), this, [this] {
            const QString dir = local(a800session_sd_path(m_session));
            QDir().mkpath(dir);
            QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
        });
    } else {
        m_drives->addAction(QStringLiteral("Open &Media Folder"), this, [this] {
            const QString dir = local(a800session_media_path(m_session));
            QDir().mkpath(dir);
            QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
        });
    }
}

void MainWindow::fillCartMenu()
{
    char info[256];
    m_cart->clear();
    if (a800session_cart_info(m_session, info, sizeof info) > 0)
        m_cart->addAction(QStringLiteral("In it: %1").arg(local(info)))->setEnabled(false);
    auto pick = [this](bool piggyback) {
        const QString f = QFileDialog::getOpenFileName(this, piggyback ? QStringLiteral("Insert a Piggyback Cartridge")
                                                                       : QStringLiteral("Insert a Cartridge"),
                                                       mediaFolder(), QString::fromUtf8(kCartFilter));
        if (f.isEmpty())
            return;
        char msg[512];
        const QByteArray p = f.toLocal8Bit();
        const int rc = piggyback ? a800session_cart_insert_piggyback(m_session, p.constData(), 0, msg, sizeof msg)
                                 : a800session_cart_insert(m_session, p.constData(), 0, msg, sizeof msg);
        rememberFolder(f);
        if (rc > 0)
            askCartType(f, piggyback);
        else if (rc < 0)
            QMessageBox::warning(this, QStringLiteral("Cannot Use the Cartridge"), local(msg));
        else
            toast(msg[0] ? local(msg) : QStringLiteral("Cartridge inserted"));
    };
    m_cart->addAction(QStringLiteral("&Insert Cartridge..."), this, [pick] { pick(false); });
    m_cart->addAction(QStringLiteral("Insert &Piggyback..."), this, [pick] { pick(true); });
    m_cart->addAction(QStringLiteral("&Remove Cartridge"), this, [this] { a800session_cart_remove(m_session); });
    m_cart->addAction(QStringLiteral("Remove Pi&ggyback"), this,
                      [this] { a800session_cart_remove_piggyback(m_session); });
}

void MainWindow::fillTapeMenu()
{
    int block = 0, blocks = 0;
    m_tape->clear();
    if (a800session_tape_position(m_session, &block, &blocks) == 0 && blocks > 0)
        m_tape->addAction(QStringLiteral("Block %1 of %2").arg(block).arg(blocks))->setEnabled(false);
    m_tape->addAction(QStringLiteral("&Load Tape..."), this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, QStringLiteral("Load a Tape"), mediaFolder(),
                                                       QString::fromUtf8(kTapeFilter));
        if (f.isEmpty())
            return;
        char msg[512];
        if (a800session_tape_insert(m_session, f.toLocal8Bit().constData(), msg, sizeof msg) != 0) {
            QMessageBox::warning(this, QStringLiteral("Cannot Load the Tape"), local(msg));
            return;
        }
        rememberFolder(f);
        toast(msg[0] ? local(msg) : QStringLiteral("Tape loaded: Boot Tape, or CLOAD / RUN \"C:\""));
    });
    m_tape->addAction(QStringLiteral("&Record New Tape..."), this, [this] {
        const QString f = QFileDialog::getSaveFileName(this, QStringLiteral("Record a New Tape"),
                                                       mediaFolder() + QStringLiteral("/tape.cas"),
                                                       QString::fromUtf8(kTapeFilter));
        if (f.isEmpty())
            return;
        if (a800session_tape_record_new(m_session, f.toLocal8Bit().constData()) != 0)
            QMessageBox::warning(this, QStringLiteral("Cannot Record"), local(a800session_last_error(m_session)));
        else
            toast(QStringLiteral("Recording: CSAVE or LIST \"C:\" writes to the tape"));
    });
    m_tape->addAction(QStringLiteral("Re&wind"), this, [this] { a800session_tape_rewind(m_session); });
    m_tape->addAction(QStringLiteral("&Boot Tape (Start held at power-on)"), this,
                      [this] { a800session_tape_boot(m_session); });
    m_tape->addAction(QStringLiteral("&Eject"), this, [this] { a800session_tape_eject(m_session); });
}

/* ---- media --------------------------------------------------------------------- */

QString MainWindow::mediaFolder() const
{
    const QString last = local(a800session_get_str(m_session, "last_dir", ""));
    if (!last.isEmpty() && QFileInfo(last).isDir())
        return last;
    return local(a800session_media_path(m_session));
}

void MainWindow::rememberFolder(const QString &path)
{
    a800session_set_str(m_session, "last_dir", QFileInfo(path).absolutePath().toLocal8Bit().constData());
}

void MainWindow::insertInto(int drive, const QString &path)
{
    char msg[512];
    if (a800session_drive_insert(m_session, drive, path.toLocal8Bit().constData(), 0, msg, sizeof msg) != 0) {
        QMessageBox::warning(this, QStringLiteral("Cannot Insert the Disk"), local(msg));
        return;
    }
    rememberFolder(path);
    toast(msg[0] ? local(msg) : QStringLiteral("%1 is in D%2:").arg(QFileInfo(path).fileName()).arg(drive + 1));
}

void MainWindow::newBlankDisk(int drive)
{
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("New Blank Disk in D%1:").arg(drive + 1));
    auto *form = new QFormLayout(&dlg);
    auto *density = new QComboBox;
    density->addItems({ QStringLiteral("Single density (90K)"), QStringLiteral("Enhanced density (130K)"),
                        QStringLiteral("Double density (180K)") });
    density->setCurrentIndex(qBound(0, a800session_get_int(m_session, "blank_density", 0), 2));
    form->addRow(QStringLiteral("Density"), density);
    auto *note = new QLabel(QStringLiteral("An empty ATR: format it from DOS on the Atari."));
    form->addRow(note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);
    if (dlg.exec() != QDialog::Accepted)
        return;
    a800session_set_int(m_session, "blank_density", density->currentIndex());
    const QString f = QFileDialog::getSaveFileName(this, QStringLiteral("Save the New Disk"),
                                                   mediaFolder() + QStringLiteral("/blank.atr"),
                                                   QStringLiteral("ATR disk images (*.atr)"));
    if (f.isEmpty())
        return;
    if (a800session_new_blank_disk(f.toLocal8Bit().constData(), density->currentIndex()) != 0) {
        QMessageBox::warning(this, QStringLiteral("Cannot Create the Disk"), f);
        return;
    }
    insertInto(drive, f);
}

/* A raw image: the user says which board it is. */
void MainWindow::askCartType(const QString &path, bool piggyback)
{
    const QByteArray p = path.toLocal8Bit();
    const int kb = a800session_cart_needs_type(p.constData());
    int types[64];
    const int n = a800session_cart_types_for(kb, types, 64);
    if (n <= 0) {
        QMessageBox::warning(this, QStringLiteral("Cannot Use the Cartridge"),
                             QStringLiteral("No Atari computer cartridge is that size."));
        return;
    }
    QStringList names;
    for (int i = 0; i < n; ++i)
        names << local(a800session_cart_type_name(types[i]));
    bool ok = false;
    const QString pick = QInputDialog::getItem(
        this, QStringLiteral("Which Cartridge Is It?"),
        QStringLiteral("%1 is a raw %2 KB image: the file does not say which cartridge board it came from. "
                       "A .car file would.").arg(QFileInfo(path).fileName()).arg(kb),
        names, 0, false, &ok);
    if (!ok)
        return;
    const int type = types[names.indexOf(pick)];
    char msg[512];
    const int rc = piggyback ? a800session_cart_insert_piggyback(m_session, p.constData(), type, msg, sizeof msg)
                             : a800session_cart_insert(m_session, p.constData(), type, msg, sizeof msg);
    if (rc == 0)
        toast(msg[0] ? local(msg) : QStringLiteral("Cartridge inserted"));
    else
        QMessageBox::warning(this, QStringLiteral("Cannot Use the Cartridge"), local(msg));
}

void MainWindow::loadMedia(const QString &path)
{
    if (!a800session_is_running(m_session)) {
        toast(QStringLiteral("The Atari is off"));
        return;
    }
    char msg[1024];
    const int rc = a800session_open(m_session, path.toLocal8Bit().constData(), msg, sizeof msg);
    if (rc == 1) {
        askCartType(path, false);
        return;
    }
    if (rc != 0) {
        QMessageBox::warning(this, QStringLiteral("Cannot Open It"),
                             msg[0] ? local(msg) : local(a800session_last_error(m_session)));
        return;
    }
    rememberFolder(path);
    toast(msg[0] ? local(msg) : QFileInfo(path).fileName());
}

void MainWindow::importRoms()
{
    const QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("Import System ROMs"), QString(),
        QStringLiteral("ROM images and MAME sets (*.rom *.bin *.zip);;All files (*)"));
    if (files.isEmpty())
        return;
    QStringList log;
    int imported = 0, unknown = 0;
    QString unknownPath;
    for (const QString &f : files) {
        char msg[1024];
        const int n = a800session_rom_import(m_session, f.toLocal8Bit().constData(), -1, msg, sizeof msg);
        if (n > 0) {
            imported += n;
            log << local(msg);
        } else {
            unknown++;
            unknownPath = f;
        }
    }
    if (unknown == 1 && imported == 0) {
        /* not known by its checksum: a peripheral's ROM fits slots by size */
        int romSlots[16];
        const QByteArray p = unknownPath.toLocal8Bit();
        const int n = a800session_rom_probe(m_session, p.constData(), romSlots, 16);
        if (n > 0) {
            QStringList names;
            for (int i = 0; i < n; ++i)
                names << local(a800session_rom_slot(romSlots[i])->name);
            bool ok = false;
            const QString pick = QInputDialog::getItem(
                this, QStringLiteral("Which ROM Is It?"),
                QStringLiteral("%1 is not one of the ROMs this app knows by its checksum, but it is the size "
                               "of these. Which is it?").arg(QFileInfo(unknownPath).fileName()),
                names, 0, false, &ok);
            if (!ok)
                return;
            char msg[512];
            if (a800session_rom_import(m_session, p.constData(), romSlots[names.indexOf(pick)], msg, sizeof msg) > 0)
                toast(QStringLiteral("Imported: %1").arg(local(msg)));
            else
                QMessageBox::warning(this, QStringLiteral("Nothing Imported"), local(msg));
            return;
        }
    }
    if (unknown)
        log << QStringLiteral("%1 file%2 not recognised: a peripheral's ROM goes in its slot from "
                              "Preferences ▸ ROMs.").arg(unknown).arg(unknown == 1 ? QStringLiteral(" was")
                                                                                   : QStringLiteral("s were"));
    QMessageBox::information(this, imported ? QStringLiteral("ROMs Imported") : QStringLiteral("Nothing Imported"),
                             log.join(QStringLiteral("\n")));
}

/* ---- the machine ------------------------------------------------------------------ */

void MainWindow::start()
{
    if (a800session_is_running(m_session))
        return;
    a800session_start_opts opts;
    a800session_default_opts(m_session, &opts);
    if (a800session_start(m_session, &opts) != 0)
        QMessageBox::warning(this, QStringLiteral("Cannot Start the Atari"), local(a800session_last_error(m_session)));
    updateStatus();
}

void MainWindow::powerCycle()
{
    if (!a800session_is_running(m_session)) {
        start();
        return;
    }
    if (a800session_power_cycle(m_session) != 0)
        QMessageBox::warning(this, QStringLiteral("Cannot Power Cycle"), local(a800session_last_error(m_session)));
    updateStatus();
}

void MainWindow::runSysaction(int sa)
{
    switch (sa) {
    case A800_SYSACT_PAUSE: a800DebuggerShow(this, m_session); break;
    case A800_SYSACT_POWER: powerCycle(); break;
    case A800_SYSACT_REBOOT_CONFIG: a800session_reboot_to_config(m_session); break;
    case A800_SYSACT_WARP:
        a800session_set_warp(m_session, !a800session_warp(m_session));
        updateStatus();
        break;
    default: break;
    }
}

void MainWindow::showSettings()
{
    const int did = SettingsDialog::run(this, m_session, [this] { applyView(); });
    applyView();
    a800ControllersRefresh();
    if (did == A800_APPLY_POWER)
        toast(QStringLiteral("Power cycled with the new configuration"));
    else if (did == A800_APPLY_RESTART)
        toast(QStringLiteral("Restarted with the new options"));
    updateStatus();
}

void MainWindow::showNotice(const QString &text)
{
    m_noticeOpen = true;
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("The Atari Stopped"));
    box.setText(QStringLiteral("<b>The Atari Stopped</b>"));
    box.setInformativeText(text);
    QPushButton *power = box.addButton(QStringLiteral("Power Cycle"), QMessageBox::AcceptRole);
    QPushButton *dbg = box.addButton(QStringLiteral("Open the Debugger"), QMessageBox::ActionRole);
    box.addButton(QStringLiteral("Leave It"), QMessageBox::RejectRole);
    box.setDefaultButton(power);
    box.exec();
    m_noticeOpen = false;
    if (box.clickedButton() == power)
        powerCycle();
    else if (box.clickedButton() == dbg)
        a800DebuggerShow(this, m_session);
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, QStringLiteral("About FujiNet Go Atari 800"),
        QStringLiteral("<b>FujiNet Go Atari 800</b> %1<br><br>"
                       "The Atari 400, 800, XL and XE computers with a FujiNet on the SIO port.<br>"
                       "The emulator is atari800 (GPL-2.0-or-later, the Atari800 Development Team), "
                       "with the FujiNet firmware (GPL-3.0-or-later) connected over NetSIO. "
                       "AltirraOS and Altirra BASIC (Avery Lee) are built in; Atari's own ROMs can be "
                       "imported.<br><br>"
                       "Copyright © 2026 Thomas Cherryhomes — GPL-3.0-or-later<br>"
                       "<a href=\"https://fujinet.online/\">fujinet.online</a>")
            .arg(QStringLiteral(A800_VERSION_STRING)));
}

/* ---- view, status --------------------------------------------------------------------- */

void MainWindow::applyView()
{
    const int i = a800session_pref_find("aspect");
    const bool tv = i < 0 || a800session_pref_get_choice(m_session, i) == 0;
    const bool smooth = a800session_get_int(m_session, "smooth", 0) != 0;
    m_display->setTvAspect(tv);
    m_display->setSmooth(smooth);
    if (m_tvAction) m_tvAction->setChecked(tv);
    if (m_smoothAction) m_smoothAction->setChecked(smooth);
}

void MainWindow::updateStatus()
{
    bool on = false;
    QString text = QStringLiteral("Off");
    if (a800session_is_running(m_session)) {
        char buf[512];
        a800session_status(m_session, buf, sizeof buf);
        text = local(buf);
        on = a800session_fujinet_link(m_session);
    }
    m_status->setText(text);
    m_dot->setStyleSheet(on ? QStringLiteral("color: %1;").arg(a800AccentColor().name())
                            : QStringLiteral("color: gray;"));
}

/* The gamepad thread cannot touch the UI; it posts system actions and this
 * timer takes them. It also says when gamepads come and go, when FujiNet
 * restarted itself, and when the Atari crashed with no debugger open. */
void MainWindow::fastTick()
{
    char text[512];
    int sa;
    while (a800session_sysaction_take(m_session, &sa))
        runSysaction(sa);
    unsigned gen = a800session_gamepad_generation(m_session);
    if (gen != m_padGeneration) {
        m_padGeneration = gen;
        if (a800session_gamepad_last_event(m_session, text, sizeof text) > 0)
            toast(local(text));
    }
    gen = a800session_fujinet_generation(m_session);
    if (gen != m_fujinetGeneration) {
        m_fujinetGeneration = gen;
        if (a800session_fujinet_last_event(m_session, text, sizeof text) > 0)
            toast(local(text));
        updateStatus();
    }
    if (!m_noticeOpen && a800session_take_notice(m_session, text, sizeof text) > 0) {
        const QString t = local(text);
        QTimer::singleShot(0, this, [this, t] { showNotice(t); });
    }
    if (!m_shotPath.isEmpty() && m_display->frames() >= m_shotFrames) {
        if (!m_display->savePng(m_shotPath))
            qWarning("A800_SCREENSHOT: could not write %s", qPrintable(m_shotPath));
        m_shotPath.clear();
    }
    if (m_quitFrames && m_display->frames() >= m_quitFrames) {
        m_quitFrames = 0;
        close();
    }
}

/* ---- events ------------------------------------------------------------------------------- */

void MainWindow::keyPressEvent(QKeyEvent *e)
{
    const int sa = a800KeySysaction(m_session, e);
    if (sa >= 0) {
        runSysaction(sa);
        return;
    }
    if (!a800ForwardKey(m_session, e, true))
        QMainWindow::keyPressEvent(e);
}

void MainWindow::keyReleaseEvent(QKeyEvent *e)
{
    if (!a800ForwardKey(m_session, e, false))
        QMainWindow::keyReleaseEvent(e);
}

/* Focus gone: nothing stays held. */
void MainWindow::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::ActivationChange && !isActiveWindow() && !a800ControllersVisible())
        a800session_release_all(m_session);
    QMainWindow::changeEvent(e);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e)
{
    const QList<QUrl> urls = e->mimeData()->urls();
    if (urls.isEmpty())
        return;
    const QString path = urls.first().toLocalFile();
    if (!path.isEmpty())
        loadMedia(path);
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    m_statusTimer.stop();
    m_fastTimer.stop();
    QMainWindow::closeEvent(e);
}
