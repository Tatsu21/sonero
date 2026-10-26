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

    // Identity before the application object, not after. These are static and
    // documented as settable beforehand, and on Wayland it matters: the platform
    // plugin registers the process with the desktop portal while QApplication is
    // being constructed, taking the desktop file name as the app id. Setting the
    // name afterwards made Qt ask the portal a second time on the same bus
    // connection, which it refuses — "Connection already associated with an
    // application ID" — leaving the registration that stuck as the one made
    // before the name was known.
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
    if (instance.pingPrimary()) {
        return 0;
    }
    instance.listen();

    sonar::ui::applyTheme(qtApp);

    int code = 0;
    {
        // Dependency injection: concrete implementations are chosen here and
        // passed in as their abstractions. Scoped so the whole object graph
        // (including PipeWireManager, which logs while shutting down) is
        // destroyed before the logger is shut down below.
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

    // An installed update restarts into the new build only now: the audio graph
    // is torn down, and once the socket is closed the new process becomes the
    // primary instead of waking this one on its way out.
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
