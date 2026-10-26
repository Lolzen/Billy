#pragma once

#include <QObject>
#include <QStringList>

class QLocalServer;

// Forwards the command line of a second Billy to the running one.
class SingleInstance : public QObject
{
    Q_OBJECT
public:
    explicit SingleInstance(QObject *parent = nullptr);
    ~SingleInstance() override;

    // Returns true when another instance accepted 'args' (this one should exit).
    bool sendToRunning(const QStringList &args);
    void listen();

signals:
    void messageReceived(const QStringList &args);

private:
    static QString serverName();
    QLocalServer *m_server = nullptr;
};
