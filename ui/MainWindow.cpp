#include "ui/MainWindow.h"

#include <cstdlib>

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
#include <QAction>
#include <QMenu>
#include <QPixmap>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "audio/IAudioBackend.h"
#include "app/SystemSetup.h"
#include "app/Updater.h"
#include "config/SettingsStore.h"
#include "audio/PipeWireManager.h"

#include "ui/DevicesPage.h"
#include "ui/EqualizerPage.h"
#include "ui/Notifier.h"
#include "ui/UiKit.h"
#include "ui/ProfilesPage.h"
#include "ui/SettingsPage.h"
#include "ui/MicrophonePage.h"
#include "ui/MixerPage.h"
#include "ui/TrayMixer.h"

namespace sonar::ui {

namespace {
struct PageDef {
    const char* title;
    const char* tooltip;
};

// The navigation pills, in the order buildUi() adds the pages to the stack.
constexpr PageDef kPages[] = {
    {"Dashboard", "Overview of your audio engine"},
    {"Mixer",     "Per-channel volume, mute, solo, balance and live VU meters"},
    {"Channels",  "Per-channel gain, output device and equalizer"},
    {"Microphone", "Mic input gain, level, noise suppression, gate and monitoring"},
    {"Devices",   "USB, Bluetooth, HDMI and external DAC detection & routing"},
    {"Profiles",  "Themes and how Sonero looks"},
    {"Settings",  "Application preferences, themes and performance options"},
};

// A never-null application icon: prefer themed audio icons, fall back to a
// guaranteed style icon so a tray/window icon is never blank.
QIcon appIcon() {
    for (const char* name :
         {"Sonero", "audio-headphones", "audio-card", "multimedia-volume-control"}) {
        QIcon icon = QIcon::fromTheme(QString::fromLatin1(name));
        if (!icon.isNull()) return icon;
    }
    return QApplication::style()->standardIcon(QStyle::SP_MediaVolume);
}

// Tray icon: a flat, high-contrast icon, since trays draw 16-24 px on an unknown
// background. By theme name first, then the packaged files, then the app icon.
QIcon trayIcon() {
    QIcon themed = QIcon::fromTheme(QStringLiteral("Sonero-tray"));
    if (!themed.isNull()) return themed;
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

// A card with no title, for the dashboard's tiles.
QFrame* makePlainCard() {
    auto* card = new QFrame;
    card->setObjectName(QStringLiteral("Card"));
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(22, 18, 22, 18);
    layout->setSpacing(10);
    return card;
}

QFrame* makeStatCard(const QString& caption, const QString& value, const char* valueObj) {
    auto* card = makePlainCard();
    auto* layout = static_cast<QVBoxLayout*>(card->layout());

    QLabel* cap = makeLabel(caption, "CardKey");
    QFont capFont = cap->font();
    capFont.setPointSize(9);
    capFont.setBold(true);
    capFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.0);
    cap->setFont(capFont);

    QLabel* val = makeLabel(value, valueObj);
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
    row->addWidget(makeLabel(key, "CardKey"));
    row->addStretch(1);
    row->addWidget(makeLabel(value, "CardVal"));
    return row;
}

}  // namespace

MainWindow::MainWindow(const audio::IAudioBackend& backend, audio::IMixer& mixer,
                       audio::IAppRouter* router, audio::IChannelController* controller,
                       audio::IEqualizerController* eqController,
                       audio::IDeviceFormats* deviceFormats,
                       audio::IAudioDevices* audioDevices,
                       audio::IMicrophoneController* micController, QWidget* parent)
    : QMainWindow(parent),
      backend_(backend),
      mixer_(mixer),
      router_(router),
      controller_(controller),
      eqController_(eqController),
      deviceFormats_(deviceFormats),
      audioDevices_(audioDevices),
      micController_(micController) {
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
    // Before any page exists, so a custom theme never flashes the built-in one first.
    ProfilesPage::applySavedTheme(settings_);
    notifier_ = new Notifier(settings_, this);  // desktop notifications
    // Checks GitHub for a newer release once a day (unless switched off); it
    // installs only when the user presses Update. See the Updates card in Settings.
    updater_ = new update::Updater(settings_, this);
    connect(updater_, &update::Updater::updateFound, this,
            [this](const update::Release& release) {
                if (notifier_ == nullptr) return;
                // Not clickable: the decision is made on the Settings page.
                notifier_->notify(QStringLiteral("Sonero %1 is available").arg(release.version),
                                  QStringLiteral("Open Settings → Updates to install it."),
                                  Notifier::Low);
            });
    // Clicking the "running in background" notification brings the window back.
    connect(notifier_, &Notifier::activated, this, &MainWindow::bringToFront);

    // ---- Pages, in kPages order ----
    pages_ = new QStackedWidget(this);
    pages_->addWidget(createDashboardPage());

    mixerPage_ = new MixerPage(mixer_, router_, controller_, settings_);
    pages_->addWidget(mixerPage_);

    auto* channels = new EqualizerPage(eqController_, settings_, audioDevices_);
    // Trim / auto-gain / output are edited here but owned by the mixer, whose
    // metering loop drives auto-gain. Route the intent across.
    connect(channels, &EqualizerPage::channelGainChanged, mixerPage_,
            &MixerPage::applyChannelGainDb);
    connect(channels, &EqualizerPage::channelAutoGainToggled, mixerPage_,
            &MixerPage::applyChannelAutoGain);
    connect(channels, &EqualizerPage::channelOutputChanged, mixerPage_,
            &MixerPage::applyChannelOutput);
    const auto showChannel = [this, channels](audio::ChannelId id) {
        channels->showChannelGain(mixerPage_->channelGainDb(id), mixerPage_->channelAutoGain(id));
        channels->showChannelOutput(mixerPage_->channelOutput(id));
    };
    connect(channels, &EqualizerPage::channelSelected, this, showChannel);
    // ...and back: while auto-gain runs it, not the user, owns the value.
    connect(mixerPage_, &MixerPage::channelGainDbChanged, channels,
            &EqualizerPage::showAutoGainValue);
    // Seed the controls with the channel the page opens on.
    showChannel(channels->selectedChannel());
    pages_->addWidget(channels);

    microphonePage_ = new MicrophonePage(controller_, eqController_, micController_, settings_);
    pages_->addWidget(microphonePage_);

    devicesPage_ = new DevicesPage(audioDevices_, deviceFormats_, settings_, notifier_);
    // The page already polls both battery sources (USB HID and BlueZ); the tray
    // reads its results rather than polling them a second time.
    connect(devicesPage_, &DevicesPage::batteriesChanged, this, &MainWindow::showBatteries);
    pages_->addWidget(devicesPage_);

    pages_->addWidget(new ProfilesPage(settings_));
    pages_->addWidget(new SettingsPage(notifier_, settings_, updater_));
    Q_ASSERT(pages_->count() == static_cast<int>(std::size(kPages)));

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
    QLabel* logoText = makeLabel(QStringLiteral("Sonero"), "AppLogo");
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
    navGroup_ = new QButtonGroup(this);
    navGroup_->setExclusive(true);
    for (int i = 0; i < static_cast<int>(std::size(kPages)); ++i) {
        auto* tab = new QPushButton(QString::fromUtf8(kPages[i].title), topBar);
        tab->setCheckable(true);
        tab->setCursor(Qt::PointingHandCursor);
        tab->setToolTip(QString::fromUtf8(kPages[i].tooltip));
        // Styled by the application sheet so it follows the theme; `tab` selects its tint.
        tab->setObjectName(QStringLiteral("NavPill"));
        tab->setProperty("tab", i % 7);
        tab->setFixedHeight(38);
        navGroup_->addButton(tab, i);
        topRow->addWidget(tab, 1, Qt::AlignVCenter);
    }

    connect(navGroup_, &QButtonGroup::idClicked, pages_, &QStackedWidget::setCurrentIndex);
    int startRow = 0;
    if (const char* p = std::getenv("SONAR_PAGE")) {  // dev hook for screenshots
        startRow = QString::fromLatin1(p).toInt();
    }
    if (navGroup_->button(startRow) == nullptr) {  // SONAR_PAGE out of range
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
                       "style=\"text-decoration:none\">"
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
                        .arg((available ? ui::currentTheme().ok : ui::currentTheme().danger)
                                 .name(QColor::HexRgb)));
    status->setToolTip(available ? QStringLiteral("PipeWire connected")
                                 : QStringLiteral("PipeWire offline"));

    auto* handle = new QLabel(QStringLiteral("@sonero"), footer);
    handle->setStyleSheet(
        QStringLiteral("color:%1; font-size:12px;")
            .arg(mixColors(ui::currentTheme().textDim, ui::currentTheme().bg, 0.4)
                     .name(QColor::HexRgb)));

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

    // The page polled once before the connect above existed; seed the tray from it.
    if (devicesPage_ != nullptr) {
        showBatteries(devicesPage_->batteries());
    }

    // The automatic update check (if enabled), delayed past the audio graph coming up;
    // the updater itself throttles it to once a day.
    QTimer::singleShot(8000, updater_, [this] { updater_->checkOnStartupIfDue(); });
}

void MainWindow::buildTrayIcon() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        return;  // no tray: fall back to a plain app that quits when closed
    }
    trayIcon_ = new QSystemTrayIcon(trayIcon(), this);
    trayIcon_->setToolTip(QStringLiteral("Sonero"));

    auto* menu = new QMenu(this);
    trayMenu_ = menu;  // showBatteries() inserts its entries at the top
    if (mixerPage_ != nullptr) {
        // The faders open as a window: over DBusMenu an embedded slider arrives as an empty row.
        QAction* volumeAction = menu->addAction(QStringLiteral("Volume…"));
        connect(volumeAction, &QAction::triggered, this, &MainWindow::showTrayMixer);
    }
    if (microphonePage_ != nullptr) {
        trayMicMenu_ = menu->addMenu(QStringLiteral("Microphone"));
        // Queued: picking an entry reaches this through its own triggered signal,
        // and the rebuild deletes that very action.
        connect(microphonePage_, &MicrophonePage::microphoneStateChanged, this,
                &MainWindow::rebuildTrayMicMenu, Qt::QueuedConnection);
        rebuildTrayMicMenu();
    }
    QAction* showAction = menu->addAction(QStringLiteral("Show Sonero"));
    connect(showAction, &QAction::triggered, this, &MainWindow::bringToFront);
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
                        bringToFront();
                    }
                }
            });
    trayIcon_->show();
}

void MainWindow::rebuildTrayMicMenu() {
    if (trayMicMenu_ == nullptr) return;
    trayMicMenu_->clear();

    QAction* mute = trayMicMenu_->addAction(QStringLiteral("Mute"));
    mute->setCheckable(true);
    mute->setChecked(microphonePage_->isMuted());
    connect(mute, &QAction::triggered, microphonePage_, &MicrophonePage::setMuted);
    trayMicMenu_->addSeparator();

    const auto addDevice = [this](const QString& text, const QString& name) {
        QAction* entry = trayMicMenu_->addAction(text);
        entry->setCheckable(true);
        entry->setChecked(microphonePage_->selectedDevice() == name);
        connect(entry, &QAction::triggered, microphonePage_,
                [this, name] { microphonePage_->selectDevice(name); });
    };
    addDevice(QStringLiteral("System default"), QString());
    for (const audio::MicInputDevice& device : microphonePage_->inputDevices()) {
        addDevice(QString::fromStdString(device.description), QString::fromStdString(device.name));
    }
}

void MainWindow::showTrayMixer() {
    if (mixerPage_ == nullptr) return;
    if (trayMixer_ != nullptr && trayMixer_->isVisible()) {
        trayMixer_->close();  // the menu entry toggles: a second click puts it away
        return;
    }
    if (trayMixer_ == nullptr) {
        trayMixer_ = std::make_unique<TrayMixer>(mixer_);
        // Both directions: the window asks the mixer page to change a volume, and
        // follows the page when a fader there (or the master rescale) moves one.
        connect(trayMixer_.get(), &TrayMixer::volumeChanged, mixerPage_,
                &MixerPage::applyChannelVolume);
        connect(mixerPage_, &MixerPage::channelVolumeChanged, trayMixer_.get(),
                &TrayMixer::showVolume);
    }
    // Off the menu-activation stack: gnome-shell still holds an input grab while it
    // runs the action, and showing a window inside it froze the desktop.
    QTimer::singleShot(0, this, [this] { trayMixer_->popUp(); });
}

void MainWindow::showBatteries(const QList<BatteryLevel>& levels) {
    if (trayMenu_ == nullptr) return;  // no system tray on this desktop
    // Rebuild rather than edit in place: devices come and go, so the count
    // changes, not just the numbers.
    for (QAction* action : batteryActions_) {
        trayMenu_->removeAction(action);
        delete action;
    }
    batteryActions_.clear();
    if (levels.isEmpty()) return;  // nothing with a battery is connected: no entries, no separator

    // Above "Show Sonero", so the reading is the first thing the menu shows.
    QAction* before = trayMenu_->actions().value(0);
    for (const BatteryLevel& level : levels) {
        const QString text = level.charging
                                 ? QStringLiteral("%1 — %2% (charging)")
                                       .arg(level.device)
                                       .arg(level.percent)
                                 : QStringLiteral("%1 — %2%").arg(level.device).arg(level.percent);
        auto* entry = new QAction(text, trayMenu_);
        entry->setEnabled(false);  // a reading, not a command
        trayMenu_->insertAction(before, entry);
        batteryActions_.append(entry);
    }
    batteryActions_.append(trayMenu_->insertSeparator(before));
}

void MainWindow::bringToFront() {
    showNormal();
    raise();
    activateWindow();
}

void MainWindow::quitApplication() {
    forceQuit_ = true;  // let closeEvent through instead of hiding to the tray
    QApplication::quit();
}

bool MainWindow::runInBackgroundEnabled() const {
    if (settings_ == nullptr) return false;
    // Background daemon behaviour is the default.
    return settings_->value(QStringLiteral("general"), QStringLiteral("runInBackground"))
        .toBool(true);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    // Hide instead of exiting, keeping the PipeWire graph alive. Brought back from the
    // tray, or by launching Sonero again (the single-instance guard forwards that).
    if (!forceQuit_ && runInBackgroundEnabled()) {
        hide();
        event->ignore();
        showBackgroundHintOnce();
        return;
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::showBackgroundHintOnce() {
    if (settings_ == nullptr) return;
    if (settings_->value(QStringLiteral("general"), QStringLiteral("backgroundHintShown"))
            .toBool(false)) {
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
    settings_->putValue(QStringLiteral("general"), QStringLiteral("backgroundHintShown"), true);
}

QWidget* MainWindow::createDashboardPage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(40, 34, 40, 34);
    layout->setSpacing(22);

    addPageHeader(layout, QStringLiteral("Dashboard"),
                  QStringLiteral("Overview of your audio engine"));

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

    auto* card = makePlainCard();
    auto* cardLayout = static_cast<QVBoxLayout*>(card->layout());
    QLabel* connTitle = makeLabel(QStringLiteral("Connection"), "SectionTitle");
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
        QLabel* hint = makeLabel(
            QStringLiteral("Start the PipeWire service to enable audio features."), "Hint");
        cardLayout->addWidget(hint);
    }

    layout->addWidget(card);
    layout->addStretch(1);
    return page;
}

}  // namespace sonar::ui
