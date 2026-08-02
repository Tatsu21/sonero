#include "app/SingleInstance.h"

#include <QLocalServer>
#include <QLocalSocket>

#include "core/Log.h"

namespace sonar {

namespace {
constexpr int kTimeoutMs = 300;
const char kActivatePayload[] = "activate";
}  // namespace

SingleInstance::SingleInstance(QString key, QObject* parent)
    : QObject(parent), key_(std::move(key)) {}

bool SingleInstance::pingPrimary() {
    QLocalSocket sock;
    sock.connectToServer(key_);
    if (!sock.waitForConnected(kTimeoutMs)) {
        return false;  // nobody is listening — we are (or will become) the primary
    }
    sock.write(kActivatePayload);
    sock.flush();
    sock.waitForBytesWritten(kTimeoutMs);
    sock.disconnectFromServer();
    log::info("Another LinuxSonar instance is running — asked it to come forward");
    return true;
}

bool SingleInstance::listen() {
    server_ = new QLocalServer(this);
    // A crashed previous run can leave a stale socket file that blocks listen();
    // clearing it is safe because pingPrimary() already proved nobody answers.
    QLocalServer::removeServer(key_);
    if (!server_->listen(key_)) {
        log::warn("Single-instance server failed to listen on '{}': {}", key_.toStdString(),
                  server_->errorString().toStdString());
        return false;
    }
    connect(server_, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket* conn = server_->nextPendingConnection()) {
            connect(conn, &QLocalSocket::disconnected, conn, &QLocalSocket::deleteLater);
            // The payload is irrelevant today; any connection means "activate me".
            emit activationRequested();
        }
    });
    return true;
}

}  // namespace sonar
