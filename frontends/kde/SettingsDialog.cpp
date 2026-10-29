/*
 * SettingsDialog -- see SettingsDialog.h.
 *
 * A tab per page of a800session_pref, a group box per group, a form row per
 * preference, so the four frontends show the same settings. Rows that apply
 * LIVE (the picture, the sound, the ports) take effect at once; the machine
 * -- model, memory, ROMs, peripherals -- waits and is applied together when
 * the dialog closes (one power cycle), as are the host options (FujiNet,
 * sound, gamepads: a session restart). Rows follow each other as they
 * change: memory sizes by model, ROM revisions by what has been imported.
 *
 * A ROMs tab lists every slot with its own Import: peripheral ROMs have no
 * published checksums, so they are put in their slot by hand.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <vector>

#include "controllers/ControllersWindow.h"

namespace {

struct Row {
    int pref = -1;
    QWidget *widget = nullptr;      /* the editor */
    QWidget *label = nullptr;       /* its form label */
    QString labels;                 /* a choice row's labels, as shown */
    QLabel *path = nullptr;         /* a file row's file name */
};

struct State {
    a800session *session;
    QWidget *dialog;
    std::function<void()> onLive;
    std::vector<Row> rows;
    std::vector<QLabel *> romRows;
    bool syncing = false;
    int pending = A800_APPLY_LIVE;
};

void refresh(State &st);

void changed(State &st, int i)
{
    const a800_pref *p = a800session_pref(i);
    if (p->applies > st.pending)
        st.pending = p->applies;
    if (st.pending == A800_APPLY_LIVE) {
        a800session_apply_settings(st.session);
        if (st.onLive)
            st.onLive();
    }
    const QString key = QString::fromUtf8(p->key);
    if (key == QLatin1String("machine") || key.endsWith(QLatin1String("_device")))
        a800ControllersRefresh();
    refresh(st);
}

QString choiceLabels(State &st, int i)
{
    QString all;
    const char *l;
    for (int c = 0; (l = a800session_pref_choice(st.session, i, c)) != nullptr; c++) {
        all += QString::fromUtf8(l);
        if (!a800session_pref_choice_enabled(st.session, i, c))
            all += QStringLiteral(" (not available)");
        all += QLatin1Char('\n');
    }
    return all;
}

void refresh(State &st)
{
    st.syncing = true;
    for (Row &r : st.rows) {
        const a800_pref *p = a800session_pref(r.pref);
        const bool on = a800session_pref_enabled(st.session, r.pref);
        r.widget->setEnabled(on);
        if (r.label)
            r.label->setEnabled(on);
        switch (p->type) {
        case A800_PREF_BOOL:
            static_cast<QCheckBox *>(r.widget)->setChecked(
                a800session_get_int(st.session, p->key, p->def) != 0);
            break;
        case A800_PREF_INT:
            static_cast<QSpinBox *>(r.widget)->setValue(a800session_get_int(st.session, p->key, p->def));
            break;
        case A800_PREF_CHOICE: {
            auto *combo = static_cast<QComboBox *>(r.widget);
            const QString labels = choiceLabels(st, r.pref);
            if (labels != r.labels) {
                combo->clear();
                for (const QString &l : labels.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
                    combo->addItem(l);
                r.labels = labels;
            }
            combo->setCurrentIndex(qMax(0, a800session_pref_get_choice(st.session, r.pref)));
            break;
        }
        case A800_PREF_TEXT: {
            /* the user's own text: filled in once, never rewritten mid-edit */
            auto *edit = static_cast<QLineEdit *>(r.widget);
            if (!edit->property("a800-text-set").toBool()) {
                edit->setText(QString::fromUtf8(a800session_pref_get_text(st.session, r.pref)));
                edit->setProperty("a800-text-set", true);
            }
            break;
        }
        default: {
            const QString v = QString::fromUtf8(a800session_get_str(st.session, p->key, ""));
            r.path->setText(v.isEmpty() ? QStringLiteral("None") : QFileInfo(v).fileName());
            r.path->setToolTip(v);
            break;
        }
        }
    }
    for (int s = 0; s < (int)st.romRows.size(); s++)
        st.romRows[s]->setText(a800session_rom_available(st.session, s)
                                   ? QStringLiteral("Imported") : QStringLiteral("Not imported"));
    st.syncing = false;
}

QWidget *makeEditor(State &st, int i, QLabel **pathOut)
{
    const a800_pref *p = a800session_pref(i);
    switch (p->type) {
    case A800_PREF_BOOL: {
        auto *cb = new QCheckBox;
        QObject::connect(cb, &QCheckBox::toggled, cb, [&st, i](bool on) {
            if (st.syncing)
                return;
            a800session_set_int(st.session, a800session_pref(i)->key, on);
            changed(st, i);
        });
        return cb;
    }
    case A800_PREF_INT: {
        auto *sb = new QSpinBox;
        sb->setRange(p->min, p->max);
        sb->setSingleStep(p->step > 0 ? p->step : 1);
        QObject::connect(sb, &QSpinBox::valueChanged, sb, [&st, i](int v) {
            if (st.syncing)
                return;
            a800session_set_int(st.session, a800session_pref(i)->key, v);
            changed(st, i);
        });
        return sb;
    }
    case A800_PREF_CHOICE: {
        auto *combo = new QComboBox;
        QObject::connect(combo, &QComboBox::currentIndexChanged, combo, [&st, i, combo](int c) {
            if (st.syncing || c < 0)
                return;
            if (!a800session_pref_choice_enabled(st.session, i, c)) {
                /* not imported, or not on this machine: put it back */
                QMessageBox::information(st.dialog, QStringLiteral("Not Available"),
                    QStringLiteral("Import its ROM first (the ROMs tab), or choose a machine that "
                                   "takes it."));
                refresh(st);
                return;
            }
            a800session_pref_set_choice(st.session, i, c);
            changed(st, i);
            (void)combo;
        });
        return combo;
    }
    case A800_PREF_TEXT: {
        /* valid text is stored; anything else is flagged and not stored, so the last
         * good text stays in force */
        auto *edit = new QLineEdit;
        const char *hold = a800session_pref_placeholder(i);
        if (hold)
            edit->setPlaceholderText(QString::fromUtf8(hold));
        QObject::connect(edit, &QLineEdit::textChanged, edit, [&st, i, edit](const QString &text) {
            if (st.syncing)
                return;
            char err[200];
            if (a800session_pref_set_text(st.session, i, text.trimmed().toUtf8().constData(), err,
                                          sizeof err) != 0) {
                edit->setStyleSheet(QStringLiteral("QLineEdit { border: 1px solid #c01c28; }"));
                edit->setToolTip(QString::fromUtf8(err));
                return;
            }
            edit->setStyleSheet(QString());
            edit->setToolTip(QString::fromUtf8(a800session_pref(i)->help));
            changed(st, i);
        });
        return edit;
    }
    default: {
        auto *box = new QWidget;
        auto *h = new QHBoxLayout(box);
        h->setContentsMargins(0, 0, 0, 0);
        auto *name = new QLabel;
        name->setMinimumWidth(180);
        auto *choose = new QPushButton(QStringLiteral("Choose..."));
        auto *clear = new QToolButton;
        clear->setText(QStringLiteral("Clear"));
        h->addWidget(name, 1);
        h->addWidget(choose);
        h->addWidget(clear);
        QObject::connect(choose, &QPushButton::clicked, box, [&st, i] {
            const a800_pref *pp = a800session_pref(i);
            const QString cur = QString::fromUtf8(a800session_get_str(st.session, pp->key, ""));
            const QString f = pp->type == A800_PREF_DIR
                ? QFileDialog::getExistingDirectory(st.dialog, QString::fromUtf8(pp->label), cur)
                : QFileDialog::getOpenFileName(st.dialog, QString::fromUtf8(pp->label), cur);
            if (f.isEmpty())
                return;
            a800session_set_str(st.session, pp->key, f.toLocal8Bit().constData());
            changed(st, i);
        });
        QObject::connect(clear, &QToolButton::clicked, box, [&st, i] {
            a800session_set_str(st.session, a800session_pref(i)->key, "");
            changed(st, i);
        });
        *pathOut = name;
        return box;
    }
    }
}

QWidget *romsPage(State &st)
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    auto *note = new QLabel(QStringLiteral(
        "The built-in AltirraOS and Altirra BASIC run everything without these. Machine ▸ Import "
        "System ROMs recognises Atari's own ROMs and MAME's sets; a peripheral's ROM goes in its "
        "slot here. Kept in %1.").arg(QString::fromUtf8(a800session_roms_path(st.session))));
    note->setWordWrap(true);
    v->addWidget(note);
    QGroupBox *group = nullptr;
    QFormLayout *form = nullptr;
    QString last;
    for (int s = 0; s < a800session_rom_slot_count(); s++) {
        const a800_rom_slot *slot = a800session_rom_slot(s);
        const QString g = QString::fromUtf8(slot->group);
        if (!group || g != last) {
            group = new QGroupBox(g);
            form = new QFormLayout(group);
            v->addWidget(group);
            last = g;
        }
        auto *row = new QWidget;
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        auto *state = new QLabel;
        auto *imp = new QPushButton(QStringLiteral("Import..."));
        imp->setToolTip(QStringLiteral("%1 bytes").arg(slot->size));
        h->addWidget(state, 1);
        h->addWidget(imp);
        QObject::connect(imp, &QPushButton::clicked, row, [&st, s] {
            const QString f = QFileDialog::getOpenFileName(
                st.dialog, QStringLiteral("Import %1").arg(QString::fromUtf8(a800session_rom_slot(s)->name)));
            if (f.isEmpty())
                return;
            char msg[512];
            if (a800session_rom_import(st.session, f.toLocal8Bit().constData(), s, msg, sizeof msg) > 0) {
                /* revisions that need it become choosable */
                st.pending = qMax(st.pending, (int)A800_APPLY_POWER);
            } else {
                QMessageBox::warning(st.dialog, QStringLiteral("Nothing Imported"), QString::fromUtf8(msg));
            }
            refresh(st);
        });
        form->addRow(QString::fromUtf8(slot->name), row);
        st.romRows.push_back(state);
    }
    v->addStretch(1);
    auto *scroll = new QScrollArea;
    scroll->setWidget(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    return scroll;
}

} // namespace

int SettingsDialog::run(QWidget *parent, a800session *session, const std::function<void()> &onLive)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(QStringLiteral("Preferences"));
    dlg.resize(640, 720);
    State st;
    st.session = session;
    st.dialog = &dlg;
    st.onLive = onLive;

    auto *tabs = new QTabWidget;
    QWidget *page = nullptr;
    QVBoxLayout *pageLayout = nullptr;
    QFormLayout *form = nullptr;
    QString lastPage, lastGroup;
    for (int i = 0; i < a800session_pref_count(); i++) {
        const a800_pref *p = a800session_pref(i);
        const QString pg = QString::fromUtf8(p->page), gr = QString::fromUtf8(p->group);
        if (!page || pg != lastPage) {
            if (pageLayout)
                pageLayout->addStretch(1);
            page = new QWidget;
            pageLayout = new QVBoxLayout(page);
            auto *scroll = new QScrollArea;
            scroll->setWidget(page);
            scroll->setWidgetResizable(true);
            scroll->setFrameShape(QFrame::NoFrame);
            tabs->addTab(scroll, pg);
            if (pg == QLatin1String("Peripherals"))
                tabs->addTab(romsPage(st), QStringLiteral("ROMs"));
            lastPage = pg;
            lastGroup.clear();
            if (pg == QLatin1String("Machine")) {
                auto *note = new QLabel(QStringLiteral(
                    "Machine settings apply together when Preferences closes: the Atari "
                    "power-cycles once."));
                note->setWordWrap(true);
                pageLayout->addWidget(note);
            }
        }
        if (gr != lastGroup) {
            auto *box = new QGroupBox(gr);
            form = new QFormLayout(box);
            pageLayout->addWidget(box);
            lastGroup = gr;
        }
        Row r;
        r.pref = i;
        r.widget = makeEditor(st, i, &r.path);
        auto *label = new QLabel(QString::fromUtf8(p->label));
        if (p->help)
            label->setToolTip(QString::fromUtf8(p->help));
        r.widget->setToolTip(p->help ? QString::fromUtf8(p->help) : QString());
        r.label = label;
        form->addRow(label, r.widget);
        st.rows.push_back(r);
    }
    if (pageLayout)
        pageLayout->addStretch(1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto *v = new QVBoxLayout(&dlg);
    v->addWidget(tabs);
    v->addWidget(buttons);
    refresh(st);
    dlg.exec();

    for (const Row &r : st.rows)
        if (a800session_pref(r.pref)->type == A800_PREF_TEXT &&
            static_cast<QLineEdit *>(r.widget)->styleSheet().size())
            QMessageBox::warning(parent, QStringLiteral("Not saved"),
                                 QStringLiteral("\"%1\" is not valid; the previous %2 stays.")
                                     .arg(static_cast<QLineEdit *>(r.widget)->text(),
                                          QString::fromUtf8(a800session_pref(r.pref)->label)));
    a800session_settings_flush(session);
    return st.pending > A800_APPLY_LIVE ? a800session_apply_settings(session) : 0;
}
