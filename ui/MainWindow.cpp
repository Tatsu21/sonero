#include "ui/MainWindow.h"

#include <cstdlib>
#include <string>
#include <string_view>

#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPixmap>
#include <QPushButton>
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
    bool ready;  // false -> placeholder page, and a nav pill that cannot be clicked
};

// Order here defines both the sidebar order and the stacked-page order.
constexpr PageDef kPages[] = {
    {"Dashboard", "Overview of your audio engine", true},
    {"Mixer",     "Per-channel volume, mute, solo, balance and live VU meters", true},
    {"Channels",  "Per-channel gain, output device and equalizer", true},
    {"Microphone", "Mic input gain, level, noise suppression, gate and monitoring", true},
    {"Devices",   "USB, Bluetooth, HDMI and external DAC detection & routing", true},
    {"Profiles",  "Save and load complete audio configurations", false},
    {"Settings",  "Application preferences, themes and performance options", true},
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
    resize(1440, 900);
    setMinimumSize(1040, 660);

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
            mixerPage_ = new MixerPage(mixer_, router_, controller_, audioDevices_, settings_);
            pages_->addWidget(mixerPage_);
        } else if (titleView == "Channels") {
            auto* channels = new EqualizerPage(eqController_, settings_, audioDevices_);
            // Trim / auto-gain / output are edited here but owned by the mixer,
            // whose metering loop drives auto-gain. Route the intent across.
            if (mixerPage_ != nullptr) {
                connect(channels, &EqualizerPage::channelGainChanged, mixerPage_,
                        &MixerPage::applyChannelGainDb);
                connect(channels, &EqualizerPage::channelAutoGainToggled, mixerPage_,
                        &MixerPage::applyChannelAutoGain);
                connect(channels, &EqualizerPage::channelOutputChanged, mixerPage_,
                        &MixerPage::applyChannelOutput);
                connect(channels, &EqualizerPage::channelSelected, this,
                        [this, channels](audio::ChannelId id) {
                            channels->showChannelGain(mixerPage_->channelGainDb(id),
                                                      mixerPage_->channelAutoGain(id));
                        });
                // ...and back: while auto-gain runs it, not the user, owns the value.
                connect(mixerPage_, &MixerPage::channelGainDbChanged, channels,
                        &EqualizerPage::showAutoGainValue);
                // Nothing has been clicked yet, so seed the controls with the
                // channel the page opens on — otherwise a restored gain or an
                // enabled Auto only appears after switching channels once.
                const audio::ChannelId first = channels->selectedChannel();
                channels->showChannelGain(mixerPage_->channelGainDb(first),
                                          mixerPage_->channelAutoGain(first));
            }
            pages_->addWidget(channels);
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
    // ---- Top bar: logo, then one pill per page ----
    auto* topBar = new QWidget(this);
    auto* topRow = new QHBoxLayout(topBar);
    topBar->setFixedHeight(60);
    // Same 16px gutter as the content panel below, so the mark, the panel edge
    // and the footer all share one left margin.
    topRow->setContentsMargins(16, 0, 16, 0);
    topRow->setSpacing(10);

    auto* logoMark = new QLabel(topBar);
    {
        // The project's own mark, not a themed stand-in.
        const QString iconPath = setup::resourcePath(QStringLiteral("icons/sonero-64.png"));
        logoMark->setPixmap(iconPath.isEmpty()
                                ? appIcon().pixmap(28, 28)
                                : QPixmap(iconPath).scaled(28, 28, Qt::KeepAspectRatio,
                                                           Qt::SmoothTransformation));
    }
    auto* logoText = new QLabel(QStringLiteral("Sonero"), topBar);
    logoText->setObjectName(QStringLiteral("AppLogo"));
    // Mark and wordmark share one vertical centre line; without the explicit
    // alignment the label sits on its own baseline and drifts off the icon.
    logoMark->setFixedSize(28, 28);
    logoMark->setAlignment(Qt::AlignCenter);
    logoText->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    topRow->addWidget(logoMark, 0, Qt::AlignVCenter);
    topRow->addSpacing(9);
    topRow->addWidget(logoText, 0, Qt::AlignVCenter);
    topRow->addSpacing(24);

    // Each tab carries a faint tint of its own, so the eye can find a page by
    // colour before reading the word.
    static const char* const kTabTints[] = {
        "#2a2b33", "#1e2d24", "#242036", "#2f2334", "#332b1e", "#1e2f30", "#232833",
    };
    navGroup_ = new QButtonGroup(this);
    navGroup_->setExclusive(true);
    for (int i = 0; i < static_cast<int>(std::size(kPages)); ++i) {
        auto* tab = new QPushButton(QString::fromUtf8(kPages[i].title), topBar);
        tab->setCheckable(true);
        tab->setCursor(kPages[i].ready ? Qt::PointingHandCursor : Qt::ArrowCursor);
        tab->setToolTip(QString::fromUtf8(kPages[i].subtitle));
        // The pill carries its own sheet, so the theme's disabled rule does not
        // reach it — the grey for a page that has nothing behind it is here.
        tab->setStyleSheet(
            QStringLiteral("QPushButton { background:%1; border:none; border-radius:9px;"
                           " padding:0 18px; color:#c8ccd8; font-size:14px; }"
                           "QPushButton:hover { color:#ffffff; }"
                           "QPushButton:checked { color:#ffffff; font-weight:700; }"
                           "QPushButton:disabled { background:#15161c; color:#474b5c; }")
                .arg(QString::fromUtf8(kTabTints[i % 7])));
        tab->setFixedHeight(38);
        // Opening a placeholder only promises a module that is not there.
        tab->setEnabled(kPages[i].ready);
        navGroup_->addButton(tab, i);
        topRow->addWidget(tab, 1, Qt::AlignVCenter);
    }

    connect(navGroup_, &QButtonGroup::idClicked, pages_, &QStackedWidget::setCurrentIndex);
    int startRow = 0;
    if (const char* p = std::getenv("SONAR_PAGE")) {  // dev hook for screenshots
        startRow = QString::fromLatin1(p).toInt();
    }
    const QAbstractButton* startTab = navGroup_->button(startRow);
    if (startTab == nullptr || !startTab->isEnabled()) {  // SONAR_PAGE may name a placeholder
        startRow = 0;
    }
    if (QAbstractButton* first = navGroup_->button(startRow)) {
        first->setChecked(true);
    }
    pages_->setCurrentIndex(startRow);

    // ---- Footer: where the project lives, and the backend's state ----
    auto* footer = new QWidget(this);
    auto* footRow = new QHBoxLayout(footer);
    footRow->setContentsMargins(16, 6, 16, 12);
    footRow->setSpacing(8);

    auto* repo = new QLabel(
        QStringLiteral("<a href=\"https://github.com/Tatsu21/sonero\" "
                       "style=\"color:#6b7183; text-decoration:none\">"
                       "https://github.com/Tatsu21/sonero</a>"),
        footer);
    repo->setOpenExternalLinks(true);
    repo->setTextFormat(Qt::RichText);

    auto* ghMark = new QLabel(footer);
    {
        const QString ghPath = setup::resourcePath(QStringLiteral("icons/github.png"));
        if (!ghPath.isEmpty()) {
            ghMark->setPixmap(QPixmap(ghPath).scaled(15, 15, Qt::KeepAspectRatio,
                                                     Qt::SmoothTransformation));
        }
        ghMark->setFixedSize(15, 15);
        ghMark->setAlignment(Qt::AlignCenter);
    }

    // The backend's state used to live in the sidebar; keep it, quietly, next to
    // the handle rather than losing it in the redesign.
    const bool available = backend_.isAvailable();
    auto* status = new QLabel(footer);
    status->setTextFormat(Qt::RichText);
    status->setText(QStringLiteral("<span style='color:%1'>&#9679;</span>")
                        .arg(available ? QStringLiteral("#9ece6a") : QStringLiteral("#f7768e")));
    status->setToolTip(available ? QStringLiteral("PipeWire connected")
                                 : QStringLiteral("PipeWire offline"));

    auto* handle = new QLabel(QStringLiteral("@sonero"), footer);
    handle->setStyleSheet(QStringLiteral("color:#5a5f70; font-size:12px;"));

    footRow->addWidget(ghMark);
    footRow->addWidget(repo);
    footRow->addStretch(1);
    footRow->addWidget(status);
    footRow->addWidget(handle);

    // ---- Assemble ----
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(topBar);

    // The pages sit on a rounded panel inset from the window edge.
    auto* panel = new QFrame(central);
    panel->setObjectName(QStringLiteral("ContentPanel"));
    panel->setStyleSheet(
        QStringLiteral("#ContentPanel { background:#313238; border-radius:16px; }"));
    pages_->setStyleSheet(QStringLiteral("QStackedWidget { background:transparent; }"));
    auto* panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->addWidget(pages_);
    auto* panelWrap = new QHBoxLayout;
    panelWrap->setContentsMargins(16, 0, 16, 0);
    panelWrap->addWidget(panel);
    layout->addLayout(panelWrap, 1);
    layout->addWidget(footer);
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
