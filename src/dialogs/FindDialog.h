#pragma once

#include <QDialog>

class PlaylistModel;
class QLineEdit;
class QListWidget;

// "Find a file in current list": filters the playlist while typing.
class FindDialog : public QDialog
{
    Q_OBJECT
public:
    FindDialog(PlaylistModel *model, const QString &initialText, QWidget *parent = nullptr);
    // Text typed in the playlist while the dialog was opening.
    void appendText(const QString &text);

signals:
    void playRequested(quint64 id);
    void queueRequested(quint64 id);
    void selectRequested(quint64 id);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void refilter();
    quint64 currentId() const;
    void playCurrent();

    PlaylistModel *m_model;
    QLineEdit *m_edit;
    QListWidget *m_list;
    bool m_fullPath = false;
};
