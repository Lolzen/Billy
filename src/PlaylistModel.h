#pragma once

#include "Settings.h"

#include <QAbstractListModel>
#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QUrl>
#include <QVector>

#include <atomic>

// The playlist: entries, queue, shuffle history and the play mode logic.
class PlaylistModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        PathRole,
        QueuePosRole,  // 1-based position in the queue, 0 if not queued
        ShuffledRole,  // already played in the current shuffle round
        PlayingRole,
    };

    struct Entry
    {
        quint64 id = 0;
        QString path;
    };

    explicit PlaylistModel(QObject *parent = nullptr);

    // QAbstractListModel
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    Qt::DropActions supportedDropActions() const override;
    Qt::DropActions supportedDragActions() const override;
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;
    bool canDropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                         const QModelIndex &parent) const override;
    bool dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                      const QModelIndex &parent) override;

    // Content
    int count() const { return m_entries.size(); }
    quint64 idAt(int row) const;
    QString pathAt(int row) const;
    int rowOf(quint64 id) const;
    QString pathOf(quint64 id) const;
    QStringList paths() const;
    QStringList paths(const QList<int> &rows) const;

    void clear();
    int addPaths(const QStringList &paths, int row = -1); // returns the first new row
    void removeRowList(QList<int> rows);
    QList<int> moveRowList(QList<int> rows, int delta);   // Ctrl+Up/Down
    QList<int> moveRowsTo(QList<int> rows, int destination);
    void setPathAt(int row, const QString &path);
    void crop(const QList<int> &rows);
    int removeMissing();
    int removeDuplicates();
    void randomize();

    // Display
    void setViewFormat(ViewFormat format, const QStringList &musicFolders, const QString &delimiter);
    QString displayText(int row) const;
    QString displayTextForPath(const QString &path) const;

    // Playback logic
    void setPlayMode(PlayMode mode);
    PlayMode playMode() const { return m_mode; }
    quint64 currentId() const { return m_currentId; }
    int currentRow() const { return rowOf(m_currentId); }
    void setCurrentId(quint64 id);
    // The track that follows 'afterId'. 'manual' is true for the "next" command.
    quint64 nextId(quint64 afterId, bool manual);
    quint64 previousId(quint64 id);
    void toggleQueue(const QList<int> &rows);
    void enqueue(quint64 id);
    QList<quint64> queue() const { return m_queue; }
    void clearHistory(); // queue and shuffle history

    // Files
    static QStringList readPlaylistFile(const QString &file);
    static bool writePlaylistFile(const QString &file, const QStringList &paths);
    static QStringList scanFolder(const QString &dir, bool recursive, const std::atomic_bool *cancel = nullptr,
                                  const QDateTime &newerThan = QDateTime());
    static void sortPaths(QStringList &paths, SortWay way);

signals:
    void urlsDropped(const QList<QUrl> &urls, int row);
    void queueChanged();

private:
    void reindex() const;
    void emitRowChanged(quint64 id);
    QList<int> normalized(QList<int> rows) const;

    QVector<Entry> m_entries;
    mutable QHash<quint64, int> m_index;
    mutable bool m_indexDirty = true;
    quint64 m_lastId = 0;

    PlayMode m_mode = PlayMode::RepeatList;
    quint64 m_currentId = 0;
    int m_currentRowHint = -1;     // where the current item was, if it got removed
    QList<quint64> m_queue;
    QSet<quint64> m_shuffled;      // played in this shuffle round
    QList<quint64> m_history;      // play order, for "previous" in shuffle mode
    quint64 m_shuffleAfter = 0;    // cached shuffle decision: m_shuffleAfter -> m_shuffleNext
    quint64 m_shuffleNext = 0;

    ViewFormat m_viewFormat = ViewFormat::FileName;
    QStringList m_musicFolders;
    QString m_delimiter = QStringLiteral(" / ");
};
