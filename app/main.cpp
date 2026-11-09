#include <cstdlib>
#include <memory>

#include <QApplication>
#include <QProcess>
#include <QWidget>

#include "app/Application.h"
#include "app/SingleInstance.h"
#include "app/Updater.h"
#include "audio/Mixer.h"
#include "audio/PipeWireManager.h"
#include "core/Log.h"
#include "core/Version.h"
#include "ui/Theme.h"

int main(int argc, char* argv[]) {
    sonar::log::init(sonar::log::Level::Debug);

    // Identity before QApplication: on Wayland the platform plugin registers the app id
    // with the portal during construction, and a later rename is refused.
    QApplication::setApplicationName(QStringLiteral("Sonero"));
    QApplication::setOrganizationName(QStringLiteral("Sonero"));
    QApplication::setApplicationVersion(QString::fromStdString(sonar::versionString()));
    // Ties the process to Sonero.desktop so Wayland/GNOME uses its icon for the
    // window, notifications and (with the AppIndicator extension) the tray item.
    QApplication::setDesktopFileName(QStringLiteral("Sonero"));

    QApplication qtApp(argc, argv);
    // --background: start hidden (used by the autostart entry). Parsed by hand to
    // keep startup free of QCommandLineParser's help/version exit paths.
    bool background = false;
    for (const QString& arg : QCoreApplication::arguments().mid(1)) {
        if (arg == QLatin1String("--background") || arg == QLatin1String("-b")) {
            background = true;
        }
    }

    // Single instance: if one is already running, wake it and bail out *before*
    // touching PipeWire, so a second launch never spins up a duplicate audio graph.
    sonar::SingleInstance instance(QStringLiteral("Sonero.instance"));
    if (instance.pingPrimary()) return 0;
    instance.listen();

    sonar::ui::applyTheme(qtApp);

    int code = 0;
    {
        // Concrete implementations are chosen here. Scoped so the whole graph is destroyed
        // before the logger shuts down.
        auto backend = std::make_unique<sonar::audio::PipeWireManager>();
        auto mixer = std::make_unique<sonar::audio::Mixer>();
        sonar::Application app(std::move(backend), std::move(mixer));
        app.start(background);

        // A second launch pings us: surface the (possibly hidden) window.
        if (QWidget* w = app.mainWindow()) {
            QObject::connect(&instance, &sonar::SingleInstance::activationRequested, w, [w] {
                w->showNormal();
                w->raise();
                w->activateWindow();
            });
        }

        code = qtApp.exec();
    }

    // Start an installed update only now: with the socket closed, the new process
    // becomes the primary instead of waking this one.
    const QString relaunch = sonar::update::takeScheduledRelaunch();
    if (!relaunch.isEmpty()) {
        instance.close();
        if (QProcess::startDetached(relaunch, {})) {
            sonar::log::info("Restarting into {}", relaunch.toStdString());
        } else {
            sonar::log::warn("Could not start {}", relaunch.toStdString());
        }
    }

    sonar::log::shutdown();
    return code;
}
