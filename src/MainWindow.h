#pragma once

#include "Settings.h"
#include "audio/AudioEngine.h"

#include <QMainWindow>
#include <QPointer>

#include <atomic>
#include <functional>
#include <memory>

class FindDialog;
class MeterScale;
class Mpris;
class PlaylistModel;
class PlaylistView;
class QActionGroup;
class QLabel;
class QMenu;
class QSystemTrayIcon;
class QToolButton;
class SheepProgress;
class SleepTimerDialog;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;

    // Files or folders given on the command line or by a second instance.
    void openPaths(const QStringList &paths, bool external);
    void restoreSession();

    // Playback control (also used by MPRIS).
    void playId(quint64 id, double startSeconds = 0);
    void playSelected();
    void togglePause();
    void stopPlayback();
    void nextTrack();
    void previousTrack();
    void seekTo(double seconds);
    void seekBy(double seconds);
    void setVolume(int percent);
    int volume() const { return m_settings.volume; }
    void setPlayMode(PlayMode mode);
    PlayMode playMode() const { return m_settings.playMode; }

    AudioEngine::State mprisState() const;
    QString mprisPath() const;
    QString mprisTitle() const;
    quint64 mprisTrackId() const;
    double mprisLength() const;
    double mprisPosition() const;
    void mprisPlay();
    void mprisPause();
    void mprisRaise();

protected:
    void closeEvent(QCloseEvent *e) override;
    void changeEvent(QEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // Setup
    void createWidgets();
    void createActions();
    void createTray();
    void applySettings();
    void layoutTopPanel();
    QAction *addAct(const QString &text, const QList<QKeySequence> &keys, const std::function<void()> &slot);

    // Engine events
    void onDecodingStarted(quint64 id);
    void onTrackStarted(quint64 id);
    void onFinished();
    void onEngineError(quint64 id, const QString &message);
    void onStateChanged();
    void refreshNext();
    void updateTick();
    void updateTitle();
    void updateStatusBar();

    // Playlist commands
    void openFolderDialog(bool add);
    void startScan(const QStringList &dirs, bool recursive, const QDateTime &newerThan, bool replace, int insertRow,
                   std::function<void()> done = {});
    void cancelScan();
    void addFiles();
    void openPlaylist();
    void loadPlaylistFile(const QString &file, bool play = false);
    void savePlaylist();
    void refreshList();
    void fileInfo();
    void editEntry();
    void removeSelected();
    void trashSelected();
    void cropList();
    void exploreSelected();
    void copyEntries(bool cut);
    void pasteEntries();
    void moveSelection(int delta);
    void queueSelected();
    void jumpToPlaying();
    void find(const QString &text = QString());
    void totalTime();
    void copyToDevice();
    void loveMode();
    void addToFavorites();
    void organizeFavorites();
    void rebuildFavoritesMenu();
    void openSettings();
    void setViewFormat(ViewFormat f);
    void setVuStyle(VuStyle style);
    void softmute();
    void minimizeToTray();
    void showStatus(const QString &text, int ms = 6000);
    void handleUrlsDropped(const QList<QUrl> &urls, int row);
    bool handleKeypad(QKeyEvent *ke);
    QString displayName(quint64 id) const;

    Settings m_settings;
    Scheme m_scheme;
    AudioEngine *m_engine;
    PlaylistModel *m_model;
    PlaylistView *m_view;
    Mpris *m_mpris = nullptr;

    // Top panel
    QWidget *m_topPanel;
    SheepProgress *m_progress;
    SheepProgress *m_meter6;
    SheepProgress *m_meter14;
    MeterScale *m_scale14;
    QLabel *m_timeLabel;
    QLabel *m_volumeLabel;
    QWidget *m_playBar;
    QList<QToolButton *> m_playButtons;

    // Status bar
    QLabel *m_statusVolume;
    QLabel *m_statusFiles;
    QLabel *m_statusMode;
    QLabel *m_statusText;
    QString m_sleepText;
    QTimer *m_statusClear;

    QSystemTrayIcon *m_tray = nullptr;
    QMenu *m_favoritesMenu = nullptr;
    QList<QAction *> m_favoriteActions;
    QActionGroup *m_modeGroup = nullptr;
    QActionGroup *m_viewGroup = nullptr;
    QActionGroup *m_vuGroup = nullptr;
    QAction *m_playBarAction = nullptr;
    QAction *m_progressAction = nullptr;
    QPointer<SleepTimerDialog> m_sleepTimer;
    QPointer<FindDialog> m_findDialog;

    QTimer *m_tick;
    quint64 m_decodingId = 0;
    int m_errorsInRow = 0;
    bool m_seekDragging = false;
    double m_dbLeft = -100, m_dbRight = -100;
    qint64 m_lastTick = 0;
    int m_volumeBeforeSoftmute = -1;
    // Dragging the volume label.
    bool m_volumeDragging = false;
    int m_volumeDragStartY = 0;
    int m_volumeDragStartValue = 0;

    std::shared_ptr<std::atomic_bool> m_scanCancel;
    std::shared_ptr<std::atomic_bool> m_timeCancel;
    QStringList m_clipboard;
    bool m_quitting = false;
};
