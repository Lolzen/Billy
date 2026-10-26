#include "Mpris.h"
#include "MainWindow.h"

#include <QCoreApplication>
#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDir>
#include <QFileInfo>
#include <QUrl>
#include <QVariantMap>

namespace {
const QString kObjectPath = QStringLiteral("/org/mpris/MediaPlayer2");
const QString kPlayerInterface = QStringLiteral("org.mpris.MediaPlayer2.Player");
} // namespace

class MprisRoot : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")
    Q_PROPERTY(bool CanQuit READ canQuit)
    Q_PROPERTY(bool CanRaise READ canRaise)
    Q_PROPERTY(bool HasTrackList READ hasTrackList)
    Q_PROPERTY(QString Identity READ identity)
    Q_PROPERTY(QString DesktopEntry READ desktopEntry)
    Q_PROPERTY(QStringList SupportedUriSchemes READ uriSchemes)
    Q_PROPERTY(QStringList SupportedMimeTypes READ mimeTypes)
public:
    MprisRoot(QObject *parent, MainWindow *w) : QDBusAbstractAdaptor(parent), m_w(w) {}
    bool canQuit() const { return true; }
    bool canRaise() const { return true; }
    bool hasTrackList() const { return false; }
    QString identity() const { return QStringLiteral("Billy"); }
    QString desktopEntry() const { return QStringLiteral("billy"); }
    QStringList uriSchemes() const { return {QStringLiteral("file"), QStringLiteral("http"), QStringLiteral("https")}; }
    QStringList mimeTypes() const
    {
        return {QStringLiteral("audio/mpeg"), QStringLiteral("audio/x-wav"), QStringLiteral("audio/ogg"),
                QStringLiteral("audio/flac"), QStringLiteral("audio/x-flac")};
    }

public slots:
    void Raise() { m_w->mprisRaise(); }
    void Quit() { m_w->close(); }

private:
    MainWindow *m_w;
};

class MprisPlayer : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ playbackStatus)
    Q_PROPERTY(QString LoopStatus READ loopStatus WRITE setLoopStatus)
    Q_PROPERTY(double Rate READ rate WRITE setRate)
    Q_PROPERTY(bool Shuffle READ shuffle WRITE setShuffle)
    Q_PROPERTY(QVariantMap Metadata READ metadata)
    Q_PROPERTY(double Volume READ volume WRITE setVolume)
    Q_PROPERTY(qlonglong Position READ position)
    Q_PROPERTY(double MinimumRate READ rate)
    Q_PROPERTY(double MaximumRate READ rate)
    Q_PROPERTY(bool CanGoNext READ canControl)
    Q_PROPERTY(bool CanGoPrevious READ canControl)
    Q_PROPERTY(bool CanPlay READ canControl)
    Q_PROPERTY(bool CanPause READ canControl)
    Q_PROPERTY(bool CanSeek READ canControl)
    Q_PROPERTY(bool CanControl READ canControl)
public:
    MprisPlayer(QObject *parent, MainWindow *w) : QDBusAbstractAdaptor(parent), m_w(w) {}

    QString playbackStatus() const
    {
        switch (m_w->mprisState()) {
        case AudioEngine::Playing: return QStringLiteral("Playing");
        case AudioEngine::Paused: return QStringLiteral("Paused");
        default: return QStringLiteral("Stopped");
        }
    }
    QString loopStatus() const
    {
        switch (m_w->playMode()) {
        case PlayMode::RepeatFile: return QStringLiteral("Track");
        case PlayMode::RepeatList:
        case PlayMode::Shuffle: return QStringLiteral("Playlist");
        default: return QStringLiteral("None");
        }
    }
    void setLoopStatus(const QString &s)
    {
        if (s == QLatin1String("Track"))
            m_w->setPlayMode(PlayMode::RepeatFile);
        else if (s == QLatin1String("Playlist"))
            m_w->setPlayMode(PlayMode::RepeatList);
        else
            m_w->setPlayMode(PlayMode::PlayList);
    }
    double rate() const { return 1.0; }
    void setRate(double) {}
    bool shuffle() const { return m_w->playMode() == PlayMode::Shuffle; }
    void setShuffle(bool on) { m_w->setPlayMode(on ? PlayMode::Shuffle : PlayMode::RepeatList); }
    QVariantMap metadata() const
    {
        QVariantMap map;
        const QString path = m_w->mprisPath();
        if (path.isEmpty()) {
            map.insert(QStringLiteral("mpris:trackid"), QVariant::fromValue(QDBusObjectPath("/org/mpris/MediaPlayer2/TrackList/NoTrack")));
            return map;
        }
        map.insert(QStringLiteral("mpris:trackid"),
                   QVariant::fromValue(QDBusObjectPath(QStringLiteral("/org/billy/track/%1").arg(m_w->mprisTrackId()))));
        const double len = m_w->mprisLength();
        if (len > 0)
            map.insert(QStringLiteral("mpris:length"), qlonglong(len * 1e6));
        map.insert(QStringLiteral("xesam:title"), m_w->mprisTitle());
        if (path.contains(QLatin1String("://"))) {
            map.insert(QStringLiteral("xesam:url"), path);
        } else {
            map.insert(QStringLiteral("xesam:url"), QUrl::fromLocalFile(path).toString());
            map.insert(QStringLiteral("xesam:album"), QFileInfo(path).dir().dirName());
        }
        return map;
    }
    double volume() const { return m_w->volume() / 100.0; }
    void setVolume(double v) { m_w->setVolume(int(qBound(0.0, v, 1.0) * 100 + 0.5)); }
    qlonglong position() const { return qlonglong(m_w->mprisPosition() * 1e6); }
    bool canControl() const { return true; }

public slots:
    void Next() { m_w->nextTrack(); }
    void Previous() { m_w->previousTrack(); }
    void Pause() { m_w->mprisPause(); }
    void PlayPause() { m_w->togglePause(); }
    void Stop() { m_w->stopPlayback(); }
    void Play() { m_w->mprisPlay(); }
    void Seek(qlonglong offset) { m_w->seekTo(m_w->mprisPosition() + offset / 1e6); }
    void SetPosition(const QDBusObjectPath &, qlonglong pos) { m_w->seekTo(pos / 1e6); }
    void OpenUri(const QString &uri)
    {
        const QUrl url(uri);
        m_w->openPaths({url.isLocalFile() ? url.toLocalFile() : uri}, true);
    }

signals:
    void Seeked(qlonglong position);

private:
    MainWindow *m_w;
};

Mpris::Mpris(MainWindow *window) : QObject(window), m_window(window)
{
    new MprisRoot(this, window);
    new MprisPlayer(this, window);
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return;
    QString service = QStringLiteral("org.mpris.MediaPlayer2.billy");
    if (!bus.registerService(service)) {
        // Multiple instances: make the name unique.
        service += QStringLiteral(".instance%1").arg(QCoreApplication::applicationPid());
        if (!bus.registerService(service))
            return;
    }
    m_registered = bus.registerObject(kObjectPath, this, QDBusConnection::ExportAdaptors);
}

void Mpris::notifyChanged()
{
    if (!m_registered)
        return;
    auto *player = findChild<MprisPlayer *>();
    if (!player)
        return;
    QVariantMap changed;
    changed.insert(QStringLiteral("PlaybackStatus"), player->playbackStatus());
    changed.insert(QStringLiteral("LoopStatus"), player->loopStatus());
    changed.insert(QStringLiteral("Shuffle"), player->shuffle());
    changed.insert(QStringLiteral("Metadata"), player->metadata());
    changed.insert(QStringLiteral("Volume"), player->volume());
    QDBusMessage msg = QDBusMessage::createSignal(kObjectPath, QStringLiteral("org.freedesktop.DBus.Properties"),
                                                  QStringLiteral("PropertiesChanged"));
    msg << kPlayerInterface << changed << QStringList();
    QDBusConnection::sessionBus().send(msg);
}

#include "Mpris.moc"
