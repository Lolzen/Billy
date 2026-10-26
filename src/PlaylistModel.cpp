#include "PlaylistModel.h"
#include "audio/Decoder.h"

#include <QCollator>
#include <QDataStream>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMimeData>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringDecoder>
#include <QTextStream>

#include <algorithm>

namespace {
const QString kRowsMime = QStringLiteral("application/x-billy-playlist-rows");
}

PlaylistModel::PlaylistModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int PlaylistModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_entries.size();
}

QVariant PlaylistModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_entries.size())
        return {};
    const Entry &e = m_entries.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
        return displayText(index.row());
    case Qt::ToolTipRole:
    case PathRole:
        return e.path;
    case IdRole:
        return e.id;
    case QueuePosRole:
        return m_queue.indexOf(e.id) + 1;
    case ShuffledRole:
        return m_mode == PlayMode::Shuffle && m_shuffled.contains(e.id);
    case PlayingRole:
        return e.id == m_currentId;
    default:
        return {};
    }
}

Qt::ItemFlags PlaylistModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::ItemIsDropEnabled;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled | Qt::ItemNeverHasChildren;
}

Qt::DropActions PlaylistModel::supportedDropActions() const { return Qt::MoveAction | Qt::CopyAction; }
Qt::DropActions PlaylistModel::supportedDragActions() const { return Qt::MoveAction | Qt::CopyAction; }

QStringList PlaylistModel::mimeTypes() const
{
    return {kRowsMime, QStringLiteral("text/uri-list")};
}

QMimeData *PlaylistModel::mimeData(const QModelIndexList &indexes) const
{
    auto *mime = new QMimeData;
    QByteArray rows;
    QDataStream stream(&rows, QIODevice::WriteOnly);
    QList<QUrl> urls;
    QList<int> list;
    for (const QModelIndex &i : indexes)
        list << i.row();
    list = normalized(list);
    for (int r : list) {
        stream << r;
        urls << QUrl::fromLocalFile(m_entries.at(r).path);
    }
    mime->setData(kRowsMime, rows);
    mime->setUrls(urls); // allows dragging into a file manager
    return mime;
}

bool PlaylistModel::canDropMimeData(const QMimeData *data, Qt::DropAction, int, int, const QModelIndex &) const
{
    return data->hasFormat(kRowsMime) || data->hasUrls();
}

bool PlaylistModel::dropMimeData(const QMimeData *data, Qt::DropAction, int row, int, const QModelIndex &parent)
{
    if (row < 0)
        row = parent.isValid() ? parent.row() : m_entries.size();
    if (data->hasFormat(kRowsMime)) {
        QByteArray raw = data->data(kRowsMime);
        QDataStream stream(&raw, QIODevice::ReadOnly);
        QList<int> rows;
        while (!stream.atEnd()) {
            int r;
            stream >> r;
            rows << r;
        }
        moveRowsTo(rows, row);
        // Returning false keeps the view from removing the dragged rows itself.
        return false;
    }
    if (data->hasUrls()) {
        emit urlsDropped(data->urls(), row);
        return false;
    }
    return false;
}

void PlaylistModel::reindex() const
{
    if (!m_indexDirty)
        return;
    m_index.clear();
    m_index.reserve(m_entries.size());
    for (int i = 0; i < m_entries.size(); ++i)
        m_index.insert(m_entries.at(i).id, i);
    m_indexDirty = false;
}

quint64 PlaylistModel::idAt(int row) const
{
    return row >= 0 && row < m_entries.size() ? m_entries.at(row).id : 0;
}

QString PlaylistModel::pathAt(int row) const
{
    return row >= 0 && row < m_entries.size() ? m_entries.at(row).path : QString();
}

int PlaylistModel::rowOf(quint64 id) const
{
    if (!id)
        return -1;
    reindex();
    return m_index.value(id, -1);
}

QString PlaylistModel::pathOf(quint64 id) const
{
    return pathAt(rowOf(id));
}

QStringList PlaylistModel::paths() const
{
    QStringList list;
    list.reserve(m_entries.size());
    for (const Entry &e : m_entries)
        list << e.path;
    return list;
}

QStringList PlaylistModel::paths(const QList<int> &rows) const
{
    QStringList list;
    for (int r : normalized(rows))
        list << m_entries.at(r).path;
    return list;
}

QList<int> PlaylistModel::normalized(QList<int> rows) const
{
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    rows.erase(std::remove_if(rows.begin(), rows.end(), [this](int r) { return r < 0 || r >= m_entries.size(); }),
               rows.end());
    return rows;
}

void PlaylistModel::clear()
{
    beginResetModel();
    m_entries.clear();
    m_indexDirty = true;
    m_queue.clear();
    m_shuffled.clear();
    m_history.clear();
    m_shuffleAfter = m_shuffleNext = 0;
    m_currentRowHint = -1;
    endResetModel();
    emit queueChanged();
}

int PlaylistModel::addPaths(const QStringList &paths, int row)
{
    if (paths.isEmpty())
        return -1;
    if (row < 0 || row > m_entries.size())
        row = m_entries.size();
    beginInsertRows(QModelIndex(), row, row + paths.size() - 1);
    QVector<Entry> added;
    added.reserve(paths.size());
    for (const QString &p : paths)
        added.push_back({++m_lastId, p});
    m_entries.insert(row, paths.size(), Entry());
    std::copy(added.begin(), added.end(), m_entries.begin() + row);
    m_indexDirty = true;
    endInsertRows();
    return row;
}

void PlaylistModel::removeRowList(QList<int> rows)
{
    rows = normalized(rows);
    if (rows.isEmpty())
        return;
    const int currentRow = rowOf(m_currentId);
    bool queueTouched = false;
    // Remove from the back, in contiguous ranges.
    for (int i = rows.size() - 1; i >= 0;) {
        int last = rows.at(i), first = last;
        while (i > 0 && rows.at(i - 1) == first - 1)
            first = rows.at(--i);
        --i;
        beginRemoveRows(QModelIndex(), first, last);
        for (int r = first; r <= last; ++r) {
            const quint64 id = m_entries.at(r).id;
            queueTouched |= m_queue.removeAll(id) > 0;
            m_shuffled.remove(id);
            if (id == m_shuffleNext)
                m_shuffleAfter = m_shuffleNext = 0;
        }
        m_entries.remove(first, last - first + 1);
        m_indexDirty = true;
        endRemoveRows();
    }
    if (currentRow >= 0 && rowOf(m_currentId) < 0) {
        int removedBefore = 0;
        for (int r : rows)
            if (r < currentRow)
                ++removedBefore;
        m_currentRowHint = currentRow - removedBefore;
    }
    if (queueTouched)
        emit queueChanged();
}

QList<int> PlaylistModel::moveRowList(QList<int> rows, int delta)
{
    rows = normalized(rows);
    if (rows.isEmpty() || delta == 0)
        return rows;
    if (delta < 0 && rows.first() + delta < 0)
        return rows;
    if (delta > 0 && rows.last() + delta >= m_entries.size())
        return rows;
    return moveRowsTo(rows, delta < 0 ? rows.first() + delta : rows.last() + 1 + delta);
}

QList<int> PlaylistModel::moveRowsTo(QList<int> rows, int destination)
{
    rows = normalized(rows);
    if (rows.isEmpty())
        return rows;
    destination = qBound(0, destination, int(m_entries.size()));
    // 'destination' is a row index in the list as it was before the move.
    QVector<Entry> moved;
    int before = 0;
    for (int r : rows) {
        moved.push_back(m_entries.at(r));
        if (r < destination)
            ++before;
    }
    emit layoutAboutToBeChanged();
    const QModelIndexList oldPersistent = persistentIndexList();
    QVector<quint64> persistentIds;
    for (const QModelIndex &i : oldPersistent)
        persistentIds << idAt(i.row());

    for (int i = rows.size() - 1; i >= 0; --i)
        m_entries.remove(rows.at(i));
    int insertAt = destination - before;
    insertAt = qBound(0, insertAt, int(m_entries.size()));
    m_entries.insert(insertAt, moved.size(), Entry());
    std::copy(moved.begin(), moved.end(), m_entries.begin() + insertAt);
    m_indexDirty = true;

    QModelIndexList newPersistent;
    for (quint64 id : persistentIds)
        newPersistent << index(rowOf(id), 0);
    changePersistentIndexList(oldPersistent, newPersistent);
    emit layoutChanged();

    QList<int> result;
    for (int i = 0; i < moved.size(); ++i)
        result << insertAt + i;
    return result;
}

void PlaylistModel::setPathAt(int row, const QString &path)
{
    if (row < 0 || row >= m_entries.size())
        return;
    m_entries[row].path = path;
    const QModelIndex i = index(row);
    emit dataChanged(i, i);
}

void PlaylistModel::crop(const QList<int> &rows)
{
    QSet<int> keep;
    for (int r : rows)
        keep.insert(r);
    QList<int> remove;
    for (int r = 0; r < m_entries.size(); ++r)
        if (!keep.contains(r))
            remove << r;
    removeRowList(remove);
}

int PlaylistModel::removeMissing()
{
    QList<int> remove;
    for (int r = 0; r < m_entries.size(); ++r)
        if (!QFileInfo::exists(m_entries.at(r).path))
            remove << r;
    removeRowList(remove);
    return remove.size();
}

int PlaylistModel::removeDuplicates()
{
    QSet<QString> seen;
    QList<int> remove;
    for (int r = 0; r < m_entries.size(); ++r) {
        const QString key = QFileInfo(m_entries.at(r).path).absoluteFilePath();
        if (seen.contains(key))
            remove << r;
        else
            seen.insert(key);
    }
    removeRowList(remove);
    return remove.size();
}

void PlaylistModel::randomize()
{
    emit layoutAboutToBeChanged();
    const QModelIndexList oldPersistent = persistentIndexList();
    QVector<quint64> persistentIds;
    for (const QModelIndex &i : oldPersistent)
        persistentIds << idAt(i.row());
    std::shuffle(m_entries.begin(), m_entries.end(), *QRandomGenerator::global());
    m_indexDirty = true;
    QModelIndexList newPersistent;
    for (quint64 id : persistentIds)
        newPersistent << index(rowOf(id), 0);
    changePersistentIndexList(oldPersistent, newPersistent);
    emit layoutChanged();
}

void PlaylistModel::setViewFormat(ViewFormat format, const QStringList &musicFolders, const QString &delimiter)
{
    m_viewFormat = format;
    m_musicFolders.clear();
    for (QString f : musicFolders) {
        f = QDir::cleanPath(f);
        if (!f.endsWith(QLatin1Char('/')))
            f += QLatin1Char('/');
        m_musicFolders << f;
    }
    // Longest first, so nested music folders strip as much as possible.
    std::sort(m_musicFolders.begin(), m_musicFolders.end(),
              [](const QString &a, const QString &b) { return a.size() > b.size(); });
    m_delimiter = delimiter.isEmpty() ? QStringLiteral(" / ") : delimiter;
    if (!m_entries.isEmpty())
        emit dataChanged(index(0), index(m_entries.size() - 1), {Qt::DisplayRole});
}

QString PlaylistModel::displayText(int row) const
{
    return displayTextForPath(pathAt(row));
}

QString PlaylistModel::displayTextForPath(const QString &path) const
{
    switch (m_viewFormat) {
    case ViewFormat::FullPath:
        return path;
    case ViewFormat::RelativePath: {
        for (const QString &root : m_musicFolders) {
            if (path.startsWith(root)) {
                QString rel = path.mid(root.size());
                const int dot = rel.lastIndexOf(QLatin1Char('.'));
                if (dot > rel.lastIndexOf(QLatin1Char('/')))
                    rel.truncate(dot);
                return rel.replace(QLatin1Char('/'), m_delimiter);
            }
        }
        // Not inside a music folder: show the parent folder and the file name.
        const QFileInfo fi(path);
        return fi.dir().dirName() + m_delimiter + fi.completeBaseName();
    }
    case ViewFormat::FileName:
    default:
        return QFileInfo(path).completeBaseName();
    }
}

void PlaylistModel::emitRowChanged(quint64 id)
{
    const int r = rowOf(id);
    if (r >= 0) {
        const QModelIndex i = index(r);
        emit dataChanged(i, i);
    }
}

void PlaylistModel::setPlayMode(PlayMode mode)
{
    m_mode = mode;
    clearHistory();
}

void PlaylistModel::clearHistory()
{
    m_queue.clear();
    m_shuffled.clear();
    m_history.clear();
    m_shuffleAfter = m_shuffleNext = 0;
    if (m_currentId && m_mode == PlayMode::Shuffle)
        m_shuffled.insert(m_currentId);
    if (!m_entries.isEmpty())
        emit dataChanged(index(0), index(m_entries.size() - 1));
    emit queueChanged();
}

void PlaylistModel::setCurrentId(quint64 id)
{
    const quint64 old = m_currentId;
    m_currentId = id;
    m_currentRowHint = -1;
    bool queueTouched = false;
    if (id) {
        queueTouched = m_queue.removeAll(id) > 0;
        if (m_mode == PlayMode::Shuffle)
            m_shuffled.insert(id);
        if (m_history.isEmpty() || m_history.last() != id)
            m_history << id;
        if (m_history.size() > 1000)
            m_history.removeFirst();
    }
    if (m_shuffleAfter != id)
        m_shuffleAfter = m_shuffleNext = 0;
    emitRowChanged(old);
    emitRowChanged(id);
    if (queueTouched) {
        // Queue numbers of the other items changed as well.
        if (!m_entries.isEmpty())
            emit dataChanged(index(0), index(m_entries.size() - 1), {QueuePosRole});
        emit queueChanged();
    }
}

quint64 PlaylistModel::nextId(quint64 afterId, bool manual)
{
    if (m_entries.isEmpty())
        return 0;
    for (quint64 q : m_queue)
        if (rowOf(q) >= 0)
            return q;

    int row = rowOf(afterId);
    // If the item is gone, continue where it was.
    const bool removed = row < 0 && afterId == m_currentId && m_currentRowHint >= 0;
    if (removed)
        row = m_currentRowHint - 1;
    const int n = m_entries.size();

    switch (m_mode) {
    case PlayMode::RepeatFile:
        if (!manual && row >= 0 && !removed)
            return afterId;
        return idAt((row + 1) % n);
    case PlayMode::PlayFile:
        if (!manual)
            return 0;
        return row + 1 < n ? idAt(row + 1) : 0;
    case PlayMode::PlayList:
        return row + 1 < n ? idAt(row + 1) : (manual ? idAt(0) : 0);
    case PlayMode::RepeatList:
        return idAt((row + 1 + n) % n);
    case PlayMode::Shuffle: {
        if (m_shuffleAfter == afterId && m_shuffleNext && rowOf(m_shuffleNext) >= 0)
            return m_shuffleNext;
        QVector<quint64> candidates;
        for (const Entry &e : m_entries)
            if (!m_shuffled.contains(e.id) && e.id != afterId)
                candidates << e.id;
        if (candidates.isEmpty()) {
            // Every item has been played: start a new round.
            m_shuffled.clear();
            if (afterId)
                m_shuffled.insert(afterId);
            for (const Entry &e : m_entries)
                if (e.id != afterId)
                    candidates << e.id;
            emit dataChanged(index(0), index(n - 1), {ShuffledRole});
        }
        if (candidates.isEmpty())
            return afterId; // single item list
        m_shuffleAfter = afterId;
        m_shuffleNext = candidates.at(QRandomGenerator::global()->bounded(int(candidates.size())));
        return m_shuffleNext;
    }
    }
    return 0;
}

quint64 PlaylistModel::previousId(quint64 id)
{
    if (m_entries.isEmpty())
        return 0;
    if (m_mode == PlayMode::Shuffle) {
        // Walk back through what was really played.
        while (!m_history.isEmpty() && (m_history.last() == id || rowOf(m_history.last()) < 0))
            m_history.removeLast();
        if (!m_history.isEmpty()) {
            const quint64 prev = m_history.takeLast();
            m_shuffled.remove(id);
            return prev;
        }
    }
    int row = rowOf(id);
    if (row < 0)
        row = m_currentRowHint >= 0 ? m_currentRowHint : 0;
    const int n = m_entries.size();
    return idAt((row - 1 + n) % n);
}

void PlaylistModel::toggleQueue(const QList<int> &rows)
{
    for (int r : normalized(rows)) {
        const quint64 id = m_entries.at(r).id;
        if (!m_queue.removeAll(id))
            m_queue << id;
    }
    if (!m_entries.isEmpty())
        emit dataChanged(index(0), index(m_entries.size() - 1), {QueuePosRole});
    emit queueChanged();
}

void PlaylistModel::enqueue(quint64 id)
{
    if (!id || m_queue.contains(id))
        return;
    m_queue << id;
    emitRowChanged(id);
    emit queueChanged();
}

// ------------------------------------------------------------------ files

QStringList PlaylistModel::readPlaylistFile(const QString &file)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QByteArray raw = f.readAll();
    auto toUtf16 = QStringDecoder(QStringDecoder::Utf8);
    QString text = toUtf16(raw);
    if (toUtf16.hasError())
        text = QString::fromLatin1(raw); // old Windows playlists
    const QDir base = QFileInfo(file).absoluteDir();
    const bool pls = file.endsWith(QLatin1String(".pls"), Qt::CaseInsensitive);

    QStringList result;
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts);
    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1Char('[')))
            continue;
        if (pls) {
            if (!line.startsWith(QLatin1String("File"), Qt::CaseInsensitive))
                continue;
            const int eq = line.indexOf(QLatin1Char('='));
            if (eq < 0)
                continue;
            line = line.mid(eq + 1).trimmed();
        }
        if (line.startsWith(QLatin1String("file://")))
            line = QUrl(line).toLocalFile();
        if (line.contains(QLatin1String("://")))
            continue; // streams are not supported
        // Playlists written on Windows use backslashes.
        if (!QFileInfo::exists(line) && line.contains(QLatin1Char('\\')))
            line.replace(QLatin1Char('\\'), QLatin1Char('/'));
        if (QDir::isRelativePath(line))
            line = base.absoluteFilePath(line);
        result << QDir::cleanPath(line);
    }
    return result;
}

bool PlaylistModel::writePlaylistFile(const QString &file, const QStringList &paths)
{
    QSaveFile f(file);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    QTextStream out(&f);
    const QString lower = file.toLower();
    if (lower.endsWith(QLatin1String(".pls"))) {
        out << "[playlist]\n";
        for (int i = 0; i < paths.size(); ++i) {
            out << "File" << (i + 1) << '=' << paths.at(i) << '\n';
            out << "Title" << (i + 1) << '=' << QFileInfo(paths.at(i)).completeBaseName() << '\n';
        }
        out << "NumberOfEntries=" << paths.size() << '\n';
        out << "Version=2\n";
    } else if (lower.endsWith(QLatin1String(".m3u")) || lower.endsWith(QLatin1String(".m3u8"))) {
        out << "#EXTM3U\n";
        for (const QString &p : paths)
            out << "#EXTINF:-1," << QFileInfo(p).completeBaseName() << '\n' << p << '\n';
    } else {
        for (const QString &p : paths)
            out << p << '\n';
    }
    out.flush();
    return f.commit();
}

QStringList PlaylistModel::scanFolder(const QString &dir, bool recursive, const std::atomic_bool *cancel,
                                      const QDateTime &newerThan)
{
    QStringList result;
    QDirIterator it(dir, QDir::Files | QDir::Readable | QDir::NoDotAndDotDot,
                    recursive ? QDirIterator::Subdirectories | QDirIterator::FollowSymlinks
                              : QDirIterator::NoIteratorFlags);
    while (it.hasNext()) {
        if (cancel && cancel->load())
            return {};
        const QString path = it.next();
        if (!Audio::isSupported(path))
            continue;
        if (newerThan.isValid() && it.fileInfo().lastModified() < newerThan)
            continue;
        result << path;
    }
    return result;
}

void PlaylistModel::sortPaths(QStringList &paths, SortWay way)
{
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    if (way == SortWay::FileName) {
        std::stable_sort(paths.begin(), paths.end(), [&](const QString &a, const QString &b) {
            return collator.compare(a.mid(a.lastIndexOf(QLatin1Char('/')) + 1),
                                    b.mid(b.lastIndexOf(QLatin1Char('/')) + 1)) < 0;
        });
    } else {
        // Compare folder by folder, so "a/b/x" sorts before "a/b c/y".
        std::stable_sort(paths.begin(), paths.end(), [&](const QString &a, const QString &b) {
            const QStringList pa = a.split(QLatin1Char('/'));
            const QStringList pb = b.split(QLatin1Char('/'));
            const int n = qMin(pa.size(), pb.size());
            for (int i = 0; i < n; ++i) {
                // A file sorts before a deeper folder at the same level.
                const bool lastA = i == pa.size() - 1, lastB = i == pb.size() - 1;
                if (lastA != lastB)
                    return lastA;
                const int c = collator.compare(pa.at(i), pb.at(i));
                if (c != 0)
                    return c < 0;
            }
            return pa.size() < pb.size();
        });
    }
}
