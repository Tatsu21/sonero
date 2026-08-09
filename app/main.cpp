#include <cstdlib>
#include <memory>

#include <QApplication>
#include <QTimer>
#include <QWidget>

#include "app/Application.h"
#include "app/SingleInstance.h"
#include "audio/Mixer.h"
#include "audio/PipeWireManager.h"
#include "core/Log.h"
#include "ui/Theme.h"

int main(int argc, char* argv[]) {
    sonar::log::init(sonar::log::Level::Debug);

    QApplication qtApp(argc, argv);
    // --background: start hidden (used by the autostart entry). Parsed by hand to
    // keep startup free of QCommandLineParser's help/version exit paths.
    bool background = false;
    for (const QString& arg : QCoreApplication::arguments().mid(1)) {
        if (arg == QLatin1String("--background") || arg == QLatin1String("-b")) {
            background = true;
        }
    }
    QApplication::setApplicationName(QStringLiteral("Sonero"));
    QApplication::setOrganizationName(QStringLiteral("Sonero"));
    QApplication::setApplicationVersion(QStringLiteral(SONAR_VERSION));
    // Ties the process to Sonero.desktop so Wayland/GNOME uses its icon for the
    // window, notifications and (with the AppIndicator extension) the tray item.
    QApplication::setDesktopFileName(QStringLiteral("Sonero"));

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

        // Optional headless screenshot for development: SONAR_SCREENSHOT=/path.png
        if (const char* shot = std::getenv("SONAR_SCREENSHOT")) {
            const QString path = QString::fromLocal8Bit(shot);
            QTimer::singleShot(1700, &qtApp, [&app, path]() {
                if (QWidget* w = app.mainWindow()) {
                    w->grab().save(path);
                }
                QCoreApplication::quit();
            });
        }

        code = qtApp.exec();
    }

    sonar::log::shutdown();
    return code;
}
