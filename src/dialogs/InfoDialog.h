#pragma once

#include <QDialog>

// Read-only text window, used for the shortcut list, the about box and
// the "total time" report.
class InfoDialog : public QDialog
{
    Q_OBJECT
public:
    InfoDialog(const QString &title, const QString &text, QWidget *parent = nullptr, bool html = false);

    static QString shortcutsText();
    static QString aboutHtml();
};
