#pragma once

#include <QDialog>
#include <QPair>

class QLineEdit;
class QPlainTextEdit;

// "Rename multiple files": find/replace over the names of the selected files.
class MultiRenameDialog : public QDialog
{
    Q_OBJECT
public:
    MultiRenameDialog(const QStringList &paths, QWidget *parent = nullptr);
    // (old path, new path) of every renamed file.
    QList<QPair<QString, QString>> renamed() const { return m_renamed; }

    static QString transform(const QString &baseName, const QString &find, const QString &replace);

private:
    void updatePreview();
    void renameAll();

    QStringList m_paths;
    QList<QPair<QString, QString>> m_renamed;
    QLineEdit *m_find;
    QLineEdit *m_replace;
    QPlainTextEdit *m_preview;
};
