#include "ui/SettingsPage.h"

#include "core/Version.h"

#include <QApplication>
#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "app/AutoStart.h"
#include "app/SystemSetup.h"
#include "config/SettingsStore.h"
#include "ui/Notifier.h"

namespace sonar::ui {

namespace {
// A titled card; returns the body layout to fill.
QVBoxLayout* makeCard(QVBoxLayout* root, const QString& title, const QString& subtitle) {
    auto* frame = new QFrame;
    frame->setObjectName(QStringLiteral("Card"));
    auto* body = new QVBoxLayout(frame);
    body->setContentsMargins(22, 18, 22, 20);
    body->setSpacing(8);

    auto* t = new QLabel(title);
    t->setObjectName(QStringLiteral("SectionTitle"));
    body->addWidget(t);
    if (!subtitle.isEmpty()) {
        auto* s = new QLabel(subtitle);
        s->setObjectName(QStringLiteral("Hint"));
        s->setWordWrap(true);
        body->addWidget(s);
    }
    body->addSpacing(4);
    root->addWidget(frame);
    return body;
}
}  // namespace

SettingsPage::SettingsPage(Notifier* notifier, config::SettingsStore* settings, QWidget* parent)
    : QWidget(parent), notifier_(notifier), settings_(settings) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    auto* page = new QWidget;
    scroll->setWidget(page);
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(40, 34, 40, 34);
    root->setSpacing(16);

    auto* title = new QLabel(QStringLiteral("Settings"));
    title->setObjectName(QStringLiteral("PageTitle"));
    auto* subtitle = new QLabel(QStringLiteral("Application preferences"));
    subtitle->setObjectName(QStringLiteral("PageSubtitle"));
    auto* head = new QVBoxLayout;
    head->setSpacing(4);
    head->addWidget(title);
    head->addWidget(subtitle);
    root->addLayout(head);

    // --- Notifications ---
    QVBoxLayout* notif = makeCard(
        root, QStringLiteral("Notifications"),
        QStringLiteral("Desktop notifications for low battery and devices connecting."));

    auto* enable = new QCheckBox(QStringLiteral("Show desktop notifications"));
    if (notifier_ != nullptr) {
        enable->setChecked(notifier_->enabled());
        connect(enable, &QCheckBox::toggled, this,
                [this](bool on) { notifier_->setEnabled(on); });
    } else {
        enable->setEnabled(false);
    }
    notif->addWidget(enable);

    auto* testRow = new QHBoxLayout;
    auto* test = new QPushButton(QStringLiteral("Send test notification"));
    test->setCursor(Qt::PointingHandCursor);
    connect(test, &QPushButton::clicked, this, [this] {
        if (notifier_ != nullptr) {
            notifier_->notify(QStringLiteral("Sonero"),
                              QStringLiteral("Notifications are working \xF0\x9F\x8E\xA7"),
                              Notifier::Normal);
        }
    });
    testRow->addWidget(test);
    testRow->addStretch(1);
    notif->addLayout(testRow);

    // --- Background ---
    QVBoxLayout* bg = makeCard(
        root, QStringLiteral("Background"),
        QStringLiteral("Keep Sonero running when the window is closed so its audio "
                       "routing and controls stay active. Quit from the tray icon menu."));

    // Read/modify/write a single key of the "general" settings section.
    const auto putGeneral = [this](const QString& key, bool value) {
        if (settings_ == nullptr) {
            return;
        }
        QJsonObject g = settings_->section(QStringLiteral("general"));
        g[key] = value;
        settings_->putSection(QStringLiteral("general"), g);
    };
    const QJsonObject general =
        settings_ != nullptr ? settings_->section(QStringLiteral("general")) : QJsonObject();

    auto* runInBg = new QCheckBox(QStringLiteral("Run in background when the window is closed"));
    runInBg->setChecked(general.value(QStringLiteral("runInBackground")).toBool(true));
    runInBg->setEnabled(settings_ != nullptr);
    connect(runInBg, &QCheckBox::toggled, this,
            [putGeneral](bool on) { putGeneral(QStringLiteral("runInBackground"), on); });
    bg->addWidget(runInBg);

    // Start at login. The source of truth is the autostart entry on disk, not a
    // setting — the user may delete it with their desktop's own tweak tool.
    // Note: only the autostart entry starts hidden (it passes --background).
    // Launching Sonero yourself always shows the window.
    auto* atLogin = new QCheckBox(QStringLiteral("Start Sonero when I log in"));
    atLogin->setChecked(autostart::isEnabled());
    auto* atLoginHint = new QLabel;
    atLoginHint->setObjectName(QStringLiteral("Hint"));
    atLoginHint->setWordWrap(true);
    const auto describeAutostart = [atLoginHint] {
        atLoginHint->setText(
            autostart::isEnabled()
                ? QStringLiteral("Starts hidden in the background · %1")
                      .arg(autostart::desktopFilePath())
                : QStringLiteral("Not enabled — Sonero only runs when you start it."));
    };
    describeAutostart();
    connect(atLogin, &QCheckBox::toggled, this, [atLogin, describeAutostart](bool on) {
        if (!autostart::setEnabled(on)) {
            const QSignalBlocker block(atLogin);  // revert without re-entering here
            atLogin->setChecked(!on);
        }
        describeAutostart();
    });
    bg->addWidget(atLogin);
    bg->addWidget(atLoginHint);

    // A guaranteed full-exit path — the tray Quit action is unavailable on desktops
    // without a system tray (e.g. stock GNOME), so keep one here too.
    auto* quitRow = new QHBoxLayout;
    auto* quit = new QPushButton(QStringLiteral("Quit Sonero"));
    quit->setCursor(Qt::PointingHandCursor);
    connect(quit, &QPushButton::clicked, this, [] { QApplication::quit(); });
    quitRow->addStretch(1);
    quitRow->addWidget(quit);
    bg->addLayout(quitRow);

    buildSystemCard(root);
    buildAboutCard(root);

    root->addStretch(1);
}

void SettingsPage::buildAboutCard(QVBoxLayout* root) {
    QVBoxLayout* body = makeCard(root, QStringLiteral("About"),
                                 QStringLiteral("Sonero %1 — free and open source, "
                                                "MIT licensed.")
                                     .arg(QString::fromStdString(versionString())));

    // Rich text so the two links are clickable; the AI note is stated plainly here
    // and not only in the README, because it is part of how the app came to be.
    auto* text = new QLabel(QStringLiteral(
        "<p style='margin:0 0 8px 0'>A personal project, built to learn and to use "
        "every day — not a commercial product. Shared in the hope that it is useful "
        "to someone else too.</p>"
        "<p style='margin:0 0 8px 0'>Source, issues and releases: "
        "<a style='color:#7c83ff; text-decoration:none' "
        "href='https://github.com/Tatsu21/sonero'>github.com/Tatsu21/sonero</a></p>"
        "<p style='margin:0 0 8px 0'>Built with AI assistance: much of the code was "
        "drafted together with Anthropic's Claude, then reviewed, built and verified "
        "against real audio hardware by a human. Every decision, and the "
        "responsibility for the result, is the author's.</p>"
        "<p style='margin:0'>Not affiliated with SteelSeries. Sonar was the "
        "inspiration, not the source.</p>"));
    text->setObjectName(QStringLiteral("Hint"));
    text->setTextFormat(Qt::RichText);
    text->setWordWrap(true);
    text->setOpenExternalLinks(true);
    body->addWidget(text);
}

void SettingsPage::buildSystemCard(QVBoxLayout* root) {
    systemBody_ = makeCard(
        root, QStringLiteral("System integration"),
        QStringLiteral("What Sonero needs from your system. Items marked with a lock "
                       "change system files and ask for your password first — you always "
                       "see the exact commands before anything runs."));
    refreshSystemChecks();
}

void SettingsPage::refreshSystemChecks() {
    if (systemBody_ == nullptr) {
        return;
    }
    // Drop the previous rows (everything the card's header added stays put because
    // the header widgets live above; we rebuild only what we appended last time).
    while (systemBody_->count() > 0) {
        QLayoutItem* item = systemBody_->takeAt(systemBody_->count() - 1);
        if (QWidget* w = item->widget()) {
            if (w->property("sonarSystemRow").toBool()) {
                w->deleteLater();
                delete item;
                continue;
            }
            // Reached the card header — put it back and stop.
            systemBody_->addWidget(w);
            delete item;
            break;
        }
        if (QLayout* child = item->layout()) {
            systemBody_->addLayout(child);
            delete item;
            break;
        }
        delete item;
    }

    for (const setup::Check& check : setup::runChecks()) {
        if (check.status == setup::Status::NotNeeded) {
            continue;
        }
        auto* row = new QFrame;
        row->setProperty("sonarSystemRow", true);
        row->setStyleSheet(QStringLiteral("QFrame { background:#12131b; border-radius:8px; }"));
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(12, 9, 12, 9);
        h->setSpacing(10);

        const bool ok = check.status == setup::Status::Ok;
        auto* dot = new QLabel(ok ? QStringLiteral("●") : QStringLiteral("●"));
        dot->setStyleSheet(QStringLiteral("color:%1; font-size:14px;")
                               .arg(ok ? QStringLiteral("#4ade80") : QStringLiteral("#facc15")));

        auto* title = new QLabel(check.needsRoot && !ok
                                     ? QStringLiteral("🔒 %1").arg(check.title)
                                     : check.title);
        title->setStyleSheet(QStringLiteral("font-weight:700; font-size:13px;"));
        auto* detail = new QLabel(check.detail);
        detail->setObjectName(QStringLiteral("Hint"));
        detail->setWordWrap(true);
        auto* texts = new QVBoxLayout;
        texts->setSpacing(2);
        texts->addWidget(title);
        texts->addWidget(detail);

        h->addWidget(dot);
        h->addLayout(texts, 1);

        if (check.fixable) {
            auto* fix = new QPushButton(check.needsRoot ? QStringLiteral("Set up…")
                                                        : QStringLiteral("Set up"));
            fix->setCursor(Qt::PointingHandCursor);
            const setup::CheckId id = check.id;
            const bool needsRoot = check.needsRoot;
            connect(fix, &QPushButton::clicked, this, [this, id, needsRoot] {
                if (!needsRoot) {
                    setup::installDesktopIntegration();
                    refreshSystemChecks();
                    return;
                }
                // Show exactly what will run as root, then let the user decide.
                const QString script = setup::privilegedFixScript(id);
                if (script.isEmpty()) {
                    QMessageBox::warning(this, QStringLiteral("Sonero"),
                                         QStringLiteral("The required files could not be found "
                                                        "in this build."));
                    return;
                }
                QMessageBox box(this);
                box.setWindowTitle(QStringLiteral("Administrator rights needed"));
                box.setIcon(QMessageBox::Question);
                box.setText(QStringLiteral("These commands will run as administrator:"));
                box.setInformativeText(
                    QStringLiteral("Your desktop will ask for your password. Nothing runs "
                                   "if you cancel."));
                box.setDetailedText(script);
                box.setStandardButtons(QMessageBox::Cancel | QMessageBox::Ok);
                box.setDefaultButton(QMessageBox::Ok);
                if (box.exec() != QMessageBox::Ok) {
                    return;
                }
                QString error;
                if (!setup::runPrivilegedFix(id, &error)) {
                    QMessageBox::warning(this, QStringLiteral("Sonero"), error);
                }
                refreshSystemChecks();
            });
            h->addWidget(fix);
        } else if (!ok) {
            auto* note = new QLabel(QStringLiteral("action needed"));
            note->setObjectName(QStringLiteral("Hint"));
            h->addWidget(note);
        }

        systemBody_->addWidget(row);
    }
}

}  // namespace sonar::ui
