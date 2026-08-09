#include "ui/MainWindow.h"

#include <cstdlib>
#include <string>
#include <string_view>

#include <QApplication>
#include <QCloseEvent>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QStackedWidget>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QVBoxLayout>
#include <QWidget>

#include "audio/IAudioBackend.h"
#include "app/SystemSetup.h"
#include "config/SettingsStore.h"
#include "ui/DevicesPage.h"
#include "ui/EqualizerPage.h"
#include "ui/Notifier.h"
#include "ui/SettingsPage.h"
#include "ui/MicrophonePage.h"
#include "ui/MixerPage.h"

namespace sonar::ui {

namespace {

struct PageDef {
    const char* title;
    const char* subtitle;
};

// Order here defines both the sidebar order and the stacked-page order.
constexpr PageDef kPages[] = {
    {"Dashboard", "Overview of your audio engine"},
    {"Mixer",     "Per-channel volume, mute, solo, balance and live VU meters"},
    {"Equalizer", "Independent 10 / 15 / 31-band EQ with presets"},
    {"Microphone", "Mic input gain, level, noise suppression, gate and monitoring"},
    {"Devices",   "USB, Bluetooth, HDMI and external DAC detection & routing"},
    {"Profiles",  "Save and load complete audio configurations"},
    {"Settings",  "Application preferences, themes and performance options"},
};

QString toQString(std::string_view sv) {
    return QString::fromUtf8(sv.data(), static_cast<qsizetype>(sv.size()));
}

// A never-null application icon: prefer themed audio icons, fall back to a
// guaranteed style icon so a tray/window icon is never blank.
QIcon appIcon() {
    for (const char* name :
         {"Sonero", "audio-headphones", "audio-card", "multimedia-volume-control"}) {
        QIcon icon = QIcon::fromTheme(QString::fromLatin1(name));
        if (!icon.isNull()) {
            return icon;
        }
    }
    return QApplication::style()->standardIcon(QStyle::SP_MediaVolume);
}

// Icon for the system tray. Trays render at 16-24px over a panel background whose
// colour we cannot know, so we ship a separate flat, high-contrast icon rather
// than shrinking the dark app tile into an unreadable smudge.
//
// Preferred by theme name (KDE and Cinnamon pass the name over D-Bus and let the
// theme pick the size); falls back to the packaged files, then to the app icon.
QIcon trayIcon() {
    QIcon themed = QIcon::fromTheme(QStringLiteral("Sonero-tray"));
    if (!themed.isNull()) {
        return themed;
    }
    QIcon fromFiles;
    for (const int size : {16, 22, 24, 32, 48}) {
        const QString path =
            setup::resourcePath(QStringLiteral("icons/sonero-tray-%1.png").arg(size));
        if (!path.isEmpty()) {
            fromFiles.addFile(path, QSize(size, size));
        }
    }
    return fromFiles.isNull() ? appIcon() : fromFiles;
}

QFrame* makeCard() {
    auto* card = new QFrame;
    card->setObjectName(QStringLiteral("Card"));
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(22, 18, 22, 18);
    layout->setSpacing(10);
    return card;
}

QFrame* makeStatCard(const QString& caption, const QString& value, const char* valueObj) {
    auto* card = makeCard();
    auto* layout = static_cast<QVBoxLayout*>(card->layout());

    auto* cap = new QLabel(caption);
    cap->setObjectName(QStringLiteral("CardKey"));
    QFont capFont = cap->font();
    capFont.setPointSize(9);
    capFont.setBold(true);
    capFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.0);
    cap->setFont(capFont);

    auto* val = new QLabel(value);
    val->setObjectName(QString::fromLatin1(valueObj));
    QFont valFont = val->font();
    valFont.setPointSize(19);
    valFont.setBold(true);
    val->setFont(valFont);

    layout->addWidget(cap);
    layout->addWidget(val);
    return card;
}

QHBoxLayout* keyValue(const QString& key, const QString& value) {
    auto* row = new QHBoxLayout;
    auto* k = new QLabel(key);
    k->setObjectName(QStringLiteral("CardKey"));
    auto* v = new QLabel(value);
    v->setObjectName(QStringLiteral("CardVal"));
    row->addWidget(k);
    row->addStretch(1);
    row->addWidget(v);
    return row;
}

}  // namespace

MainWindow::MainWindow(const audio::IAudioBackend& backend, audio::IMixer& mixer,
                       audio::IAppRouter* router, audio::IChannelController* controller,
                       audio::IEqualizerController* eqController,
                       audio::IDeviceFormats* deviceFormats,
                       audio::IAudioDevices* audioDevices, QWidget* parent)
    : QMainWindow(parent),
      backend_(backend),
      mixer_(mixer),
      router_(router),
      controller_(controller),
      eqController_(eqController),
      deviceFormats_(deviceFormats),
      audioDevices_(audioDevices) {
    buildUi();
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi() {
    setWindowTitle(QStringLiteral("Sonero"));
    setWindowIcon(appIcon());
    resize(1120, 700);
    setMinimumSize(880, 560);

    // ---- Settings persistence (auto-loaded here, saved on change by the pages) ----
    settings_ = new config::SettingsStore(this);
    notifier_ = new Notifier(settings_, this);  // desktop notifications
    // Clicking the "running in background" notification brings the window back.
    connect(notifier_, &Notifier::activated, this, [this] {
        showNormal();
        raise();
        activateWindow();
    });

    // ---- Pages ----
    pages_ = new QStackedWidget(this);
    for (const auto& page : kPages) {
        const std::string_view titleView(page.title);
        if (titleView == "Dashboard") {
            pages_->addWidget(createDashboardPage());
        } else if (titleView == "Mixer") {
            pages_->addWidget(
                new MixerPage(mixer_, router_, controller_, audioDevices_, settings_));
        } else if (titleView == "Equalizer") {
            pages_->addWidget(new EqualizerPage(eqController_, settings_));
        } else if (titleView == "Microphone") {
            pages_->addWidget(new MicrophonePage(controller_, settings_));
        } else if (titleView == "Devices") {
            pages_->addWidget(
                new DevicesPage(audioDevices_, deviceFormats_, settings_, notifier_));
        } else if (titleView == "Settings") {
            pages_->addWidget(new SettingsPage(notifier_, settings_));
        } else {
            pages_->addWidget(createPlaceholderPage(QString::fromUtf8(page.title),
                                                    QString::fromUtf8(page.subtitle)));
        }
    }

    // ---- Sidebar ----
    auto* sidebar = new QFrame(this);
    sidebar->setObjectName(QStringLiteral("Sidebar"));
    sidebar->setFixedWidth(214);
    auto* sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(0, 0, 0, 0);
    sideLayout->setSpacing(0);

    auto* logo = new QLabel(sidebar);
    logo->setObjectName(QStringLiteral("AppLogo"));
    logo->setTextFormat(Qt::RichText);
    logo->setText(QStringLiteral(
        "<span style='color:#7aa2f7'>&#9670;</span> Son<span style='color:#7aa2f7'>ero</span>"));
    sideLayout->addWidget(logo);

    auto* section = new QLabel(QStringLiteral("MENU"), sidebar);
    section->setObjectName(QStringLiteral("SidebarSection"));
    sideLayout->addWidget(section);

    nav_ = new QListWidget(sidebar);
    nav_->setObjectName(QStringLiteral("Nav"));
    nav_->setFrameShape(QFrame::NoFrame);
    nav_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    for (const auto& page : kPages) {
        nav_->addItem(QString::fromUtf8(page.title));
    }
    sideLayout->addWidget(nav_, 1);

    auto* statusChip = new QLabel(sidebar);
    statusChip->setObjectName(QStringLiteral("StatusChip"));
    statusChip->setTextFormat(Qt::RichText);
    const bool available = backend_.isAvailable();
    statusChip->setText(QStringLiteral("<span style='color:%1'>&#9679;</span>&nbsp; %2")
        .arg(available ? QStringLiteral("#9ece6a") : QStringLiteral("#f7768e"),
             available ? QStringLiteral("PipeWire connected")
                       : QStringLiteral("PipeWire offline")));
    sideLayout->addWidget(statusChip);

    connect(nav_, &QListWidget::currentRowChanged,
            pages_, &QStackedWidget::setCurrentIndex);
    int startRow = 0;
    if (const char* p = std::getenv("SONAR_PAGE")) {  // dev hook for screenshots
        startRow = QString::fromLatin1(p).toInt();
    }
    nav_->setCurrentRow(startRow);

    // ---- Assemble ----
    auto* central = new QWidget(this);
    auto* layout = new QHBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(sidebar);
    layout->addWidget(pages_, 1);
    setCentralWidget(central);

    buildTrayIcon();  // enables running in the background
}

void MainWindow::buildTrayIcon() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        return;  // no tray: fall back to a plain app that quits when closed
    }
    trayIcon_ = new QSystemTrayIcon(trayIcon(), this);
    trayIcon_->setToolTip(QStringLiteral("Sonero"));

    auto* menu = new QMenu(this);
    QAction* showAction = menu->addAction(QStringLiteral("Show Sonero"));
    connect(showAction, &QAction::triggered, this, [this] {
        showNormal();
        raise();
        activateWindow();
    });
    menu->addSeparator();
    QAction* quitAction = menu->addAction(QStringLiteral("Quit"));
    connect(quitAction, &QAction::triggered, this, &MainWindow::quitApplication);
    trayIcon_->setContextMenu(menu);

    // Left-click / double-click toggles the window's visibility.
    connect(trayIcon_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger ||
                    reason == QSystemTrayIcon::DoubleClick) {
                    if (isVisible() && !isMinimized()) {
                        hide();
                    } else {
                        showNormal();
                        raise();
                        activateWindow();
                    }
                }
            });
    trayIcon_->show();
}

void MainWindow::quitApplication() {
    forceQuit_ = true;  // let closeEvent through instead of hiding to the tray
    QApplication::quit();
}

bool MainWindow::runInBackgroundEnabled() const {
    if (settings_ == nullptr) {
        return false;
    }
    return settings_->section(QStringLiteral("general"))
        .value(QStringLiteral("runInBackground"))
        .toBool(true);  // background daemon behavior is the default
}

void MainWindow::closeEvent(QCloseEvent* event) {
    // Hide instead of exiting, keeping the PipeWire graph alive. This does not
    // depend on a system tray (GNOME/Wayland has none by default) — the window is
    // brought back either from the tray, or by simply launching Sonero again
    // (the single-instance guard forwards that to us as an activation request).
    if (!forceQuit_ && runInBackgroundEnabled()) {
        hide();
        event->ignore();
        showBackgroundHintOnce();
        return;
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::showBackgroundHintOnce() {
    if (settings_ == nullptr) {
        return;
    }
    QJsonObject g = settings_->section(QStringLiteral("general"));
    if (g.value(QStringLiteral("backgroundHintShown")).toBool(false)) {
        return;
    }
    const QString body =
        trayIcon_ != nullptr
            ? QStringLiteral("Still running in the background — right-click the tray icon to quit.")
            : QStringLiteral(
                  "Still running in the background. Launch Sonero again to reopen, or use "
                  "Quit in Settings to exit.");
    if (trayIcon_ != nullptr) {
        trayIcon_->showMessage(QStringLiteral("Sonero"), body, QSystemTrayIcon::Information,
                               5000);
    } else if (notifier_ != nullptr) {
        notifier_->notify(QStringLiteral("Sonero"), body, Notifier::Low, QString(),
                          /*clickable=*/true);
    }
    g[QStringLiteral("backgroundHintShown")] = true;
    settings_->putSection(QStringLiteral("general"), g);
}

QWidget* MainWindow::createDashboardPage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(40, 34, 40, 34);
    layout->setSpacing(22);

    auto* title = new QLabel(QStringLiteral("Dashboard"));
    title->setObjectName(QStringLiteral("PageTitle"));
    auto* subtitle = new QLabel(QStringLiteral("Overview of your audio engine"));
    subtitle->setObjectName(QStringLiteral("PageSubtitle"));
    auto* head = new QVBoxLayout;
    head->setSpacing(4);
    head->addWidget(title);
    head->addWidget(subtitle);
    layout->addLayout(head);

    const bool available = backend_.isAvailable();
    const auto info = backend_.serverInfo();

    auto* cards = new QHBoxLayout;
    cards->setSpacing(16);
    cards->addWidget(makeStatCard(QStringLiteral("AUDIO ENGINE"),
                                  available ? QStringLiteral("Online") : QStringLiteral("Offline"),
                                  available ? "StatusValueOk" : "StatusValueBad"));
    cards->addWidget(makeStatCard(QStringLiteral("CHANNELS"), QStringLiteral("6"), "CardVal"));
    cards->addWidget(makeStatCard(QStringLiteral("BACKEND"), QStringLiteral("PipeWire"), "CardVal"));
    layout->addLayout(cards);

    auto* card = makeCard();
    auto* cardLayout = static_cast<QVBoxLayout*>(card->layout());
    auto* connTitle = new QLabel(QStringLiteral("Connection"));
    connTitle->setObjectName(QStringLiteral("SectionTitle"));
    cardLayout->addWidget(connTitle);

    if (available) {
        cardLayout->addLayout(keyValue(QStringLiteral("Server"),
                                       QString::fromStdString(info.name)));
        cardLayout->addLayout(keyValue(QStringLiteral("Version"),
                                       QString::fromStdString(info.version)));
        cardLayout->addLayout(keyValue(
            QStringLiteral("Session"),
            QStringLiteral("%1@%2").arg(QString::fromStdString(info.userName),
                                        QString::fromStdString(info.hostName))));
    } else {
        auto* hint = new QLabel(
            QStringLiteral("Start the PipeWire service to enable audio features."));
        hint->setObjectName(QStringLiteral("Hint"));
        cardLayout->addWidget(hint);
    }

    layout->addWidget(card);
    layout->addStretch(1);
    return page;
}

QWidget* MainWindow::createPlaceholderPage(const QString& title, const QString& subtitle) {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(40, 34, 40, 34);
    layout->setSpacing(10);
    layout->setAlignment(Qt::AlignTop);

    auto* titleLabel = new QLabel(title);
    titleLabel->setObjectName(QStringLiteral("PageTitle"));

    auto* subLabel = new QLabel(subtitle);
    subLabel->setObjectName(QStringLiteral("PageSubtitle"));
    subLabel->setWordWrap(true);

    auto* card = makeCard();
    auto* cardLayout = static_cast<QVBoxLayout*>(card->layout());
    auto* badge = new QLabel(QStringLiteral("This module arrives in a later stage."));
    badge->setObjectName(QStringLiteral("Hint"));
    cardLayout->addWidget(badge);

    layout->addWidget(titleLabel);
    layout->addWidget(subLabel);
    layout->addSpacing(10);
    layout->addWidget(card);
    layout->addStretch(1);
    return page;
}

}  // namespace sonar::ui
