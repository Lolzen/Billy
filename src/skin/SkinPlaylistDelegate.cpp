#include "skin/SkinPlaylistDelegate.h"
#include "PlaylistModel.h"
#include "audio/Decoder.h"

#include <QDir>
#include <QFileInfo>
#include <QListView>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>

namespace {

constexpr int kRowH = 19;
constexpr int kHeaderH = 30;
constexpr int kCover = 70;
constexpr int kCoverRows = 4; // rows next to the cover leave room for it

const QColor kAlbum(0xff, 0xff, 0xff);
const QColor kText(240, 240, 240);
const QColor kDim(185, 185, 185);
const QColor kTime(150, 185, 205);

QFont font(int px, bool bold = false)
{
    QFont f;
    f.setFamilies({QStringLiteral("Trebuchet MS"), QStringLiteral("Fira Sans"), QStringLiteral("Ubuntu"),
                   QStringLiteral("DejaVu Sans")});
    f.setPixelSize(px);
    f.setBold(bold);
    return f;
}

// "01 - Title" -> "Title": the number column shows the position already.
QString titleOf(const QString &name)
{
    static const QRegularExpression number(QStringLiteral("^\\d{1,3}\\s*[-._)]?\\s+"));
    QString t = name;
    t.remove(number);
    return t.isEmpty() ? name : t;
}

void drawNote(QPainter &p, const QPointF &at, const QColor &c)
{
    // A small double note (16px), drawn with paths.
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(at);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawEllipse(QRectF(1, 10, 5, 4));
    p.drawEllipse(QRectF(9, 8, 5, 4));
    p.drawRect(QRectF(5, 2, 1.3, 10));
    p.drawRect(QRectF(13, 0, 1.3, 10));
    QPainterPath beam;
    beam.moveTo(5, 2);
    beam.lineTo(14.3, 0);
    beam.lineTo(14.3, 2.5);
    beam.lineTo(5, 4.5);
    beam.closeSubpath();
    p.drawPath(beam);
    p.restore();
}

} // namespace

SkinPlaylistDelegate::SkinPlaylistDelegate(PlaylistModel *model, QObject *parent)
    : QStyledItemDelegate(parent), m_model(model)
{
}

QString SkinPlaylistDelegate::groupOf(int row) const
{
    const QString path = m_model->pathAt(row);
    if (PlaylistModel::isUrl(path))
        return QStringLiteral("://");
    return path.left(path.lastIndexOf(QLatin1Char('/')));
}

bool SkinPlaylistDelegate::isGroupStart(int row) const
{
    return row == 0 || groupOf(row) != groupOf(row - 1);
}

int SkinPlaylistDelegate::indexInGroup(int row) const
{
    const QString group = groupOf(row);
    int i = 0;
    while (row - i > 0 && i < 999 && groupOf(row - i - 1) == group)
        ++i;
    return i;
}

QSize SkinPlaylistDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const
{
    return QSize(100, kRowH + (isGroupStart(index.row()) ? kHeaderH : 0));
}

void SkinPlaylistDelegate::paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const int row = index.row();
    const QString path = m_model->pathAt(row);
    const bool url = PlaylistModel::isUrl(path);
    const bool playing = index.data(PlaylistModel::PlayingRole).toBool();
    const bool selected = option.state & QStyle::State_Selected;
    const int queuePos = index.data(PlaylistModel::QueuePosRole).toInt();
    QRect r = option.rect;
    p->save();

    if (isGroupStart(row)) {
        // Group header: album (folder) and artist (the folder above).
        const QRect h(r.left(), r.top(), r.width(), kHeaderH);
        QLinearGradient g(h.topLeft(), h.bottomLeft());
        g.setColorAt(0, QColor(255, 255, 255, 34));
        g.setColorAt(1, QColor(255, 255, 255, 4));
        p->fillRect(h.adjusted(0, 2, 0, -1), g);
        QString album, artist;
        if (url) {
            album = QObject::tr("Internet radio");
        } else {
            const QDir dir = QFileInfo(path).dir();
            album = dir.dirName();
            QDir up = dir;
            if (up.cdUp())
                artist = up.dirName();
        }
        drawNote(*p, QPointF(h.left() + 6, h.top() + 8), QColor(0x9c, 0xd8, 0xb0));
        p->setFont(font(13, true));
        p->setPen(kAlbum);
        p->drawText(QRect(h.left() + 30, h.top() + 2, h.width() - 40, 16), Qt::AlignLeft | Qt::AlignVCenter,
                    p->fontMetrics().elidedText(album, Qt::ElideRight, h.width() - 40));
        p->setFont(font(10));
        p->setPen(kDim);
        p->drawText(QRect(h.left() + 30, h.top() + 16, h.width() - 40, 12), Qt::AlignLeft | Qt::AlignVCenter, artist);
        r.setTop(r.top() + kHeaderH);
    }

    if (selected) {
        p->fillRect(r, QColor(255, 255, 255, playing ? 55 : 40));
        p->fillRect(QRect(r.left() + 23, r.top(), 1, r.height()), QColor(255, 255, 255, 60));
    }

    // Number column, or a play/pause sign for the playing item.
    const QRect num(r.left() + 1, r.top(), 22, r.height());
    if (playing) {
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(Qt::NoPen);
        p->setBrush(kTime);
        const QPointF c = QRectF(num).center();
        if (m_paused) {
            p->drawRect(QRectF(c.x() - 3, c.y() - 4, 2, 8));
            p->drawRect(QRectF(c.x() + 1, c.y() - 4, 2, 8));
        } else {
            QPainterPath t;
            t.moveTo(c.x() - 3, c.y() - 4.5);
            t.lineTo(c.x() + 4, c.y());
            t.lineTo(c.x() - 3, c.y() + 4.5);
            t.closeSubpath();
            p->drawPath(t);
        }
        p->setBrush(Qt::NoBrush);
        p->setRenderHint(QPainter::Antialiasing, false);
    } else {
        p->setFont(font(10));
        p->setPen(kDim);
        p->drawText(num, Qt::AlignCenter, QStringLiteral("%1.").arg(indexInGroup(row) + 1, 2, 10, QLatin1Char('0')));
    }

    // Title; the first rows of a group leave room for the cover on the right.
    const int coverRoom = (!url && indexInGroup(row) < kCoverRows) ? 130 : 0;
    int right = r.right() - 8 - coverRoom;
    if (playing && !m_remaining.isEmpty()) {
        p->setFont(font(10));
        p->setPen(kTime);
        const int tw = p->fontMetrics().horizontalAdvance(m_remaining);
        p->drawText(QRect(right - tw, r.top(), tw, r.height()), Qt::AlignVCenter | Qt::AlignRight, m_remaining);
        right -= tw + 10;
    }
    QString title = url ? m_model->displayText(row) : titleOf(QFileInfo(path).completeBaseName());
    if (queuePos > 0)
        title = QStringLiteral("[%1] ").arg(queuePos) + title;
    // The playing item: white and bold.
    p->setFont(font(11, playing));
    p->setPen(playing ? kAlbum : queuePos > 0 ? QColor(0x9c, 0xff, 0x9c) : kText);
    const QRect tr(r.left() + 30, r.top(), right - r.left() - 30, r.height());
    p->drawText(tr, Qt::AlignLeft | Qt::AlignVCenter, p->fontMetrics().elidedText(title, Qt::ElideRight, tr.width()));

    if (option.state & QStyle::State_HasFocus) {
        p->setPen(QPen(QColor(255, 255, 255, 70), 1, Qt::DotLine));
        p->setBrush(Qt::NoBrush);
        p->drawRect(r.adjusted(0, 0, -1, -1));
    }
    p->restore();
}

QImage SkinPlaylistDelegate::coverFor(int row)
{
    const QString group = groupOf(row);
    const auto it = m_covers.constFind(group);
    if (it != m_covers.constEnd())
        return it.value();
    QImage cover = Audio::coverArt(m_model->pathAt(row));
    if (!cover.isNull())
        cover = cover.scaled(kCover, kCover, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation)
                    .copy(0, 0, kCover, kCover);
    m_covers.insert(group, cover);
    return cover;
}

void SkinPlaylistDelegate::paintCovers(QPainter &p, QListView *view)
{
    const int rows = m_model->count();
    if (rows == 0)
        return;
    const QRect area = view->viewport()->rect();
    int first = view->indexAt(area.topLeft()).row();
    if (first < 0)
        first = 0;
    // Step back to the start of the group that is cut off at the top.
    while (first > 0 && !isGroupStart(first))
        --first;
    for (int row = first; row < rows; ++row) {
        const QRect vr = view->visualRect(m_model->index(row));
        if (vr.top() > area.bottom())
            break;
        if (!isGroupStart(row) || PlaylistModel::isUrl(m_model->pathAt(row)))
            continue;
        const QImage cover = coverFor(row);
        if (cover.isNull())
            continue;
        const QRect c(area.right() - 120, vr.top() + kHeaderH + 6, kCover, kCover);
        p.fillRect(c.adjusted(3, 3, 5, 5), QColor(0, 0, 0, 70));
        p.fillRect(c.adjusted(1, 1, 3, 3), QColor(0, 0, 0, 90));
        p.drawImage(c.topLeft(), cover);
        p.setPen(QColor(235, 235, 235, 210));
        p.drawRect(c.adjusted(-1, -1, 0, 0));
    }
}
