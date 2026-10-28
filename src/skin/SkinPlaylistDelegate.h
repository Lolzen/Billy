#pragma once

#include <QHash>
#include <QImage>
#include <QStyledItemDelegate>

class PlaylistModel;
class QListView;

// Playlist rows for the skinned look: a header for every album (folder),
// a number column, a note for the playing item, bold titles, the remaining
// time of the playing item, and the album cover next to each group.
// Albums are taken from the folder names, so no tags have to be read.
class SkinPlaylistDelegate : public QStyledItemDelegate
{
public:
    SkinPlaylistDelegate(PlaylistModel *model, QObject *parent = nullptr);

    void setRemaining(const QString &text) { m_remaining = text; }
    void setPaused(bool paused) { m_paused = paused; }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

    // Draws the covers of the visible groups on top of the rows.
    void paintCovers(QPainter &p, QListView *view);

private:
    QString groupOf(int row) const;
    bool isGroupStart(int row) const;
    int indexInGroup(int row) const;
    QImage coverFor(int row);

    PlaylistModel *m_model;
    QString m_remaining;
    bool m_paused = false;
    QHash<QString, QImage> m_covers; // folder -> scaled cover (null: none)
};
