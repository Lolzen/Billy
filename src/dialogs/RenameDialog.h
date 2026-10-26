#pragma once

#include <QDialog>

class QLineEdit;
class QPlainTextEdit;

// "Rename single file" (F2): rename on disk and show file details.
class RenameDialog : public QDialog
{
    Q_OBJECT
public:
    RenameDialog(const QString &path, QWidget *parent = nullptr);
    QString newPath() const { return m_newPath; }

    // Removes "%20", underscores and superfluous spaces.
    static QString cleanName(QString name);

public slots:
    void accept() override;

private:
    QString m_path;
    QString m_newPath;
    QLineEdit *m_edit;
    QPlainTextEdit *m_info;
};
