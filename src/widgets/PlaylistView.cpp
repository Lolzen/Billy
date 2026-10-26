#include "widgets/PlaylistView.h"
#include "PlaylistModel.h"

#include <QKeyEvent>
#include <QPainter>

#include <algorithm>

void PlaylistDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const bool selected = option.state & QStyle::State_Selected;
    const bool playing = index.data(PlaylistModel::PlayingRole).toBool();
    const int queuePos = index.data(PlaylistModel::QueuePosRole).toInt();
    const bool shuffled = index.data(PlaylistModel::ShuffledRole).toBool();

    QColor bg = m_scheme.background;
    QColor fg = m_scheme.font;
    if (selected) {
        bg = playing ? m_scheme.selectionPlaying : m_scheme.selection;
        fg = playing ? m_scheme.playingSelected : m_scheme.fontSelected;
    } else if (playing) {
        fg = m_scheme.playing;
    } else if (queuePos > 0) {
        fg = m_scheme.queued;
    } else if (shuffled) {
        fg = m_scheme.shuffled;
    }

    painter->save();
    painter->fillRect(option.rect, bg);
    QFont f = option.font;
    if (playing && m_scheme.playingBold)
        f.setBold(true);
    painter->setFont(f);
    painter->setPen(fg);

    QString text = index.data(Qt::DisplayRole).toString();
    if (queuePos > 0)
        text = QStringLiteral("[%1] ").arg(queuePos) + text;
    const QRect r = option.rect.adjusted(3, 0, -3, 0);
    const QString elided = QFontMetrics(f).elidedText(text, Qt::ElideMiddle, r.width());
    painter->drawText(r, Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine, elided);

    if (option.state & QStyle::State_HasFocus && selected) {
        QPen pen(fg, 1, Qt::DotLine);
        painter->setPen(pen);
        painter->drawRect(option.rect.adjusted(0, 0, -1, -1));
    }
    painter->restore();
}

QSize PlaylistDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const
{
    QFont f = option.font;
    f.setBold(true);
    return QSize(100, QFontMetrics(f).height() + 3);
}

PlaylistView::PlaylistView(QWidget *parent)
    : QListView(parent), m_delegate(new PlaylistDelegate(this))
{
    setItemDelegate(m_delegate);
    setUniformItemSizes(true);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::MoveAction);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFrameShape(QFrame::StyledPanel);
    setTabKeyNavigation(false);
}

void PlaylistView::setScheme(const Scheme &scheme)
{
    m_delegate->setScheme(scheme);
    QPalette pal = palette();
    pal.setColor(QPalette::Base, scheme.background);
    pal.setColor(QPalette::Text, scheme.font);
    setPalette(pal);
    setFont(scheme.listFont());
    viewport()->update();
}

QList<int> PlaylistView::selectedRows() const
{
    QList<int> rows;
    if (!selectionModel())
        return rows;
    for (const QModelIndex &i : selectionModel()->selectedIndexes())
        rows << i.row();
    std::sort(rows.begin(), rows.end());
    return rows;
}

void PlaylistView::selectRows(const QList<int> &rows, int current)
{
    if (!selectionModel() || !model())
        return;
    QItemSelection sel;
    for (int r : rows) {
        const QModelIndex i = model()->index(r, 0);
        sel.select(i, i);
    }
    selectionModel()->select(sel, QItemSelectionModel::ClearAndSelect);
    if (current < 0 && !rows.isEmpty())
        current = rows.first();
    if (current >= 0) {
        const QModelIndex i = model()->index(current, 0);
        selectionModel()->setCurrentIndex(i, QItemSelectionModel::NoUpdate);
        scrollTo(i);
    }
}

void PlaylistView::keyPressEvent(QKeyEvent *e)
{
    const Qt::KeyboardModifiers mods = e->modifiers() & ~Qt::KeypadModifier;
    switch (e->key()) {
    case Qt::Key_Left:
    case Qt::Key_Right:
        if (!(e->modifiers() & Qt::KeypadModifier) || mods != Qt::NoModifier) {
            emit seekRequested(e->key() == Qt::Key_Left ? -1 : 1, mods);
            return;
        }
        break;
    case Qt::Key_Up:
    case Qt::Key_Down:
        if (mods == Qt::ControlModifier) {
            emit moveRequested(e->key() == Qt::Key_Up ? -1 : 1);
            return;
        }
        break;
    default:
        break;
    }

    // Typing a name opens the finder.
    const QString text = e->text();
    if (!text.isEmpty() && (mods & ~Qt::ShiftModifier) == Qt::NoModifier && !(e->modifiers() & Qt::KeypadModifier)
        && text.at(0).isLetterOrNumber()) {
        emit typeAhead(text);
        return;
    }
    QListView::keyPressEvent(e);
}
