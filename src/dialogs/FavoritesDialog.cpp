#include "dialogs/FavoritesDialog.h"
#include "Settings.h"

#include <QAction>
#include <QCollator>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QUrl>

#include <algorithm>

FavoritesDialog::FavoritesDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Organize favorites"));
    resize(347, 339);

    m_list = new QListWidget(this);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setContextMenuPolicy(Qt::ActionsContextMenu);

    auto *rename = new QAction(tr("&Rename"), this);
    rename->setShortcut(Qt::Key_F2);
    auto *remove = new QAction(tr("&Delete"), this);
    remove->setShortcut(Qt::Key_Delete);
    auto *explore = new QAction(tr("&Explore..."), this);
    explore->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    auto *refreshAc = new QAction(tr("&Refresh"), this);
    refreshAc->setShortcut(Qt::Key_F5);
    auto *sep = new QAction(this);
    sep->setSeparator(true);
    m_list->addActions({rename, remove, sep, explore, refreshAc});
    addActions({rename, remove, explore, refreshAc});

    connect(rename, &QAction::triggered, this, [this] {
        QListWidgetItem *item = m_list->currentItem();
        if (!item)
            return;
        const QString file = item->data(Qt::UserRole).toString();
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Rename"), tr("New name:"), QLineEdit::Normal,
                                                   item->text(), &ok).trimmed();
        if (!ok || name.isEmpty() || name == item->text())
            return;
        if (!isValidName(name)) {
            QMessageBox::warning(this, windowTitle(),
                                 tr("The title may not have any of the following characters:\n\\ / : ? \" < > |"));
            return;
        }
        const QString target = QFileInfo(file).absoluteDir().filePath(name + QStringLiteral(".txt"));
        if (QFileInfo::exists(target) || !QFile::rename(file, target))
            QMessageBox::warning(this, windowTitle(), tr("Can't rename file."));
        refresh();
    });
    connect(remove, &QAction::triggered, this, [this] {
        const auto items = m_list->selectedItems();
        if (items.isEmpty())
            return;
        if (QMessageBox::question(this, windowTitle(), tr("Are you sure you want to delete the selected item(s)?"))
            != QMessageBox::Yes)
            return;
        for (QListWidgetItem *item : items)
            QFile::remove(item->data(Qt::UserRole).toString());
        refresh();
    });
    connect(explore, &QAction::triggered, this,
            [] { QDesktopServices::openUrl(QUrl::fromLocalFile(Settings::favoritesDir())); });
    connect(refreshAc, &QAction::triggered, this, &FavoritesDialog::refresh);

    auto *grid = new QGridLayout(this);
    auto *b1 = new QPushButton(rename->text(), this);
    auto *b2 = new QPushButton(remove->text(), this);
    auto *b3 = new QPushButton(explore->text(), this);
    auto *b4 = new QPushButton(refreshAc->text(), this);
    auto *close = new QPushButton(tr("&Close"), this);
    close->setDefault(true);
    connect(b1, &QPushButton::clicked, rename, &QAction::trigger);
    connect(b2, &QPushButton::clicked, remove, &QAction::trigger);
    connect(b3, &QPushButton::clicked, explore, &QAction::trigger);
    connect(b4, &QPushButton::clicked, refreshAc, &QAction::trigger);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    grid->addWidget(m_list, 0, 0, 6, 1);
    grid->addWidget(b1, 0, 1);
    grid->addWidget(b2, 1, 1);
    grid->addWidget(b3, 2, 1);
    grid->addWidget(b4, 3, 1);
    grid->setRowStretch(4, 1);
    grid->addWidget(close, 5, 1);
    auto *tip = new QLabel(tr("Tip: Add a number before the titles, to sort manually."), this);
    tip->setWordWrap(true);
    grid->addWidget(tip, 6, 0, 1, 2);
    refresh();
}

QStringList FavoritesDialog::favorites()
{
    QDir dir(Settings::favoritesDir());
    QStringList files = dir.entryList({QStringLiteral("*.txt")}, QDir::Files);
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(files.begin(), files.end(), [&](const QString &a, const QString &b) { return collator.compare(a, b) < 0; });
    for (QString &f : files)
        f = dir.filePath(f);
    return files;
}

bool FavoritesDialog::isValidName(const QString &name)
{
    static const QString forbidden = QStringLiteral("\\/:?\"<>|*");
    for (const QChar c : name)
        if (forbidden.contains(c))
            return false;
    return !name.trimmed().isEmpty();
}

void FavoritesDialog::refresh()
{
    m_list->clear();
    for (const QString &f : favorites()) {
        auto *item = new QListWidgetItem(QFileInfo(f).completeBaseName(), m_list);
        item->setData(Qt::UserRole, f);
    }
}
