#pragma once

#include <QObject>
#include <QString>

class QLocalServer;

namespace sonar {

// Single running instance: the primary owns a QLocalServer socket; a later launch
// pings it and exits before building the PipeWire graph, and the primary emits
// activationRequested() to raise its window.
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

    // Stops answering, so that a process started after this call becomes the
    // primary instead of waking this one.
    void close();

signals:
    // A secondary instance asked the primary to come to the foreground.
    void activationRequested();

private:
    QString key_;
    QLocalServer* server_ = nullptr;
};

}  // namespace sonar
