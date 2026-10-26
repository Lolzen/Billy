#pragma once

#include <QDateTime>
#include <QDialog>

class QCheckBox;
class QComboBox;
class QFileSystemModel;
class QTreeView;

// Folder picker rooted at the music folders ("Open folder" F4 / "Add folder" F3).
class DirDialog : public QDialog
{
    Q_OBJECT
public:
    DirDialog(const QString &title, const QStringList &musicFolders, const QString &root, const QString &current,
              bool includeSubfolders, QWidget *parent = nullptr);

    QStringList selectedDirs() const;
    bool includeSubfolders() const;
    QDateTime newerThan() const; // invalid: no filter
    QString root() const;

signals:
    void quickAddRequested(const QString &dir, bool recursive);

private:
    void setRoot(const QString &root);

    QComboBox *m_roots;
    QTreeView *m_tree;
    QFileSystemModel *m_fs;
    QCheckBox *m_subdirs;
    QComboBox *m_filter;
};
