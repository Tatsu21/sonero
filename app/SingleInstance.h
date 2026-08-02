#pragma once

#include <QObject>
#include <QString>

class QLocalServer;

namespace sonar {

// Enforces a single running instance and lets a second launch wake the first.
//
// A primary instance owns a named QLocalServer socket. Any later launch calls
// pingPrimary(): if the socket answers, an instance is already alive — the caller
// forwards an "activate" ping and exits *before* it ever builds the PipeWire graph,
// so we never end up with two audio backends fighting over the same sinks. The
// primary reacts to that ping by emitting activationRequested() (used to raise and
// focus the window).
class SingleInstance : public QObject {
    Q_OBJECT

public:
    explicit SingleInstance(QString key, QObject* parent = nullptr);

    // Tries to reach an already-running primary and, if found, asks it to activate.
    // Returns true when a primary answered — the caller should then quit immediately.
    bool pingPrimary();

    // Becomes the primary by listening on the named socket. Returns false if the
    // socket could not be claimed (another instance won the race).
    bool listen();

signals:
    // A secondary instance asked the primary to come to the foreground.
    void activationRequested();

private:
    QString key_;
    QLocalServer* server_ = nullptr;
};

}  // namespace sonar
