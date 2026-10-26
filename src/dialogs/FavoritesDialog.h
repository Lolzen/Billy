#pragma once

#include <QDialog>

class QListWidget;

// "Organize favorites": rename/delete the saved favorite playlists.
class FavoritesDialog : public QDialog
{
    Q_OBJECT
public:
    explicit FavoritesDialog(QWidget *parent = nullptr);

    static QStringList favorites(); // file paths, sorted by name
    static bool isValidName(const QString &name);

private:
    void refresh();

    QListWidget *m_list;
};
