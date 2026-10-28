#pragma once

#include "Settings.h"

#include <QListView>
#include <QStyledItemDelegate>

#include <functional>

class PlaylistDelegate : public QStyledItemDelegate
{
public:
    explicit PlaylistDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}
    void setScheme(const Scheme &scheme) { m_scheme = scheme; }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    Scheme m_scheme;
};

class PlaylistView : public QListView
{
    Q_OBJECT
public:
    explicit PlaylistView(QWidget *parent = nullptr);

    void setScheme(const Scheme &scheme);
    QList<int> selectedRows() const;
    QStyledItemDelegate *classicDelegate() const { return m_delegate; }
    // Painted on top of the rows (the skin's album covers).
    void setOverlay(std::function<void(QPainter &)> overlay) { m_overlay = std::move(overlay); }
    void selectRows(const QList<int> &rows, int current = -1);

signals:
    void typeAhead(const QString &text);            // letters typed: open the finder
    void seekRequested(int direction, Qt::KeyboardModifiers modifiers);
    void moveRequested(int delta);                  // Ctrl+Up/Down

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void paintEvent(QPaintEvent *e) override;

private:
    PlaylistDelegate *m_delegate;
    std::function<void(QPainter &)> m_overlay;
};
