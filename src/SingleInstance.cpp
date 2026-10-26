#include "SingleInstance.h"

#include <QDataStream>
#include <QLocalServer>
#include <QLocalSocket>

SingleInstance::SingleInstance(QObject *parent) : QObject(parent) {}

SingleInstance::~SingleInstance() = default;

QString SingleInstance::serverName()
{
    return QStringLiteral("billy-player-") + qEnvironmentVariable("USER", QStringLiteral("user"));
}

bool SingleInstance::sendToRunning(const QStringList &args)
{
    QLocalSocket socket;
    socket.connectToServer(serverName());
    if (!socket.waitForConnected(500))
        return false;
    QByteArray data;
    QDataStream out(&data, QIODevice::WriteOnly);
    out << args;
    socket.write(data);
    socket.waitForBytesWritten(1000);
    socket.disconnectFromServer();
    return true;
}

void SingleInstance::listen()
{
    m_server = new QLocalServer(this);
    QLocalServer::removeServer(serverName()); // stale socket after a crash
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server->listen(serverName()))
        return;
    connect(m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *socket = m_server->nextPendingConnection()) {
            connect(socket, &QLocalSocket::disconnected, socket, [this, socket] {
                QByteArray data = socket->readAll();
                QDataStream in(&data, QIODevice::ReadOnly);
                QStringList args;
                in >> args;
                socket->deleteLater();
                emit messageReceived(args);
            });
        }
    });
}
