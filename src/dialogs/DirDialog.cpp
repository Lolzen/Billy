#include "dialogs/DirDialog.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

DirDialog::DirDialog(const QString &title, const QStringList &musicFolders, const QString &root,
                     const QString &current, bool includeSubfolders, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(title);
    setWindowFlag(Qt::WindowMaximizeButtonHint);
    resize(329, 424);

    m_roots = new QComboBox(this);
    m_roots->setToolTip(tr("Root folder. Add your music folders in Settings > Media for quick access."));
    for (const QString &f : musicFolders)
        m_roots->addItem(QDir::toNativeSeparators(f), f);
    m_roots->addItem(tr("Home folder"), QDir::homePath());
    m_roots->addItem(tr("All drives"), QStringLiteral("/"));
    m_roots->addItem(tr("Select root folder..."), QString());

    m_fs = new QFileSystemModel(this);
    m_fs->setFilter(QDir::AllDirs | QDir::NoDotAndDotDot);
    m_fs->setReadOnly(true);
    m_fs->setRootPath(QStringLiteral("/"));

    m_tree = new QTreeView(this);
    m_tree->setModel(m_fs);
    m_tree->setHeaderHidden(true);
    for (int c = 1; c < m_fs->columnCount(); ++c)
        m_tree->hideColumn(c);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setContextMenuPolicy(Qt::ActionsContextMenu);

    m_subdirs = new QCheckBox(tr("Include &subfolders"), this);
    m_subdirs->setChecked(includeSubfolders);
    m_filter = new QComboBox(this);
    m_filter->setToolTip(tr("Filters new files based on file date."));
    m_filter->addItems({tr("No filter"), tr("Today"), tr("Last week"), tr("Last month")});

    auto *open = new QAction(tr("&Open"), this);
    connect(open, &QAction::triggered, this, &QDialog::accept);
    auto *quick = new QAction(tr("&Add to playlist"), this);
    quick->setShortcut(Qt::Key_Insert);
    connect(quick, &QAction::triggered, this, [this] {
        for (const QString &d : selectedDirs())
            emit quickAddRequested(d, m_subdirs->isChecked());
    });
    auto *explore = new QAction(tr("&Explore..."), this);
    explore->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    connect(explore, &QAction::triggered, this, [this] {
        const QStringList dirs = selectedDirs();
        if (!dirs.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(dirs.first()));
    });
    auto *maximize = new QAction(this);
    maximize->setShortcut(Qt::Key_F12);
    connect(maximize, &QAction::triggered, this, [this] { isMaximized() ? showNormal() : showMaximized(); });
    m_tree->addActions({open, quick, explore});
    addAction(quick);
    addAction(explore);
    addAction(maximize);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *options = new QHBoxLayout;
    options->addWidget(m_subdirs);
    options->addStretch(1);
    options->addWidget(m_filter);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_roots);
    layout->addWidget(m_tree, 1);
    layout->addLayout(options);
    layout->addWidget(buttons);

    connect(m_roots, &QComboBox::activated, this, [this](int index) {
        QString dir = m_roots->itemData(index).toString();
        if (dir.isEmpty()) {
            dir = QFileDialog::getExistingDirectory(this, tr("Select root folder"), QDir::homePath());
            if (dir.isEmpty())
                return;
            m_roots->insertItem(0, QDir::toNativeSeparators(dir), dir);
            m_roots->setCurrentIndex(0);
        }
        setRoot(dir);
    });
    connect(m_tree, &QTreeView::doubleClicked, this, [this](const QModelIndex &i) {
        // Double click on a leaf folder opens it; otherwise it expands.
        if (!m_fs->hasChildren(i))
            accept();
    });

    QString start = root;
    if (start.isEmpty())
        start = musicFolders.isEmpty() ? QDir::homePath() : musicFolders.first();
    int idx = m_roots->findData(start);
    if (idx < 0) {
        m_roots->insertItem(0, QDir::toNativeSeparators(start), start);
        idx = 0;
    }
    m_roots->setCurrentIndex(idx);
    setRoot(start);
    if (!current.isEmpty() && current.startsWith(start)) {
        const QModelIndex i = m_fs->index(current);
        if (i.isValid()) {
            m_tree->setCurrentIndex(i);
            m_tree->scrollTo(i);
        }
    }
    m_tree->setFocus();
}

void DirDialog::setRoot(const QString &root)
{
    const QModelIndex r = m_fs->index(root);
    m_tree->setRootIndex(r);
    m_tree->setCurrentIndex(r);
}

QString DirDialog::root() const
{
    return m_fs->filePath(m_tree->rootIndex());
}

QStringList DirDialog::selectedDirs() const
{
    QStringList dirs;
    for (const QModelIndex &i : m_tree->selectionModel()->selectedRows())
        dirs << m_fs->filePath(i);
    if (dirs.isEmpty()) {
        const QModelIndex cur = m_tree->currentIndex().isValid() ? m_tree->currentIndex() : m_tree->rootIndex();
        dirs << m_fs->filePath(cur);
    }
    return dirs;
}

bool DirDialog::includeSubfolders() const
{
    return m_subdirs->isChecked();
}

QDateTime DirDialog::newerThan() const
{
    const QDateTime today(QDate::currentDate(), QTime(0, 0));
    switch (m_filter->currentIndex()) {
    case 1: return today;
    case 2: return today.addDays(-7);
    case 3: return today.addMonths(-1);
    default: return {};
    }
}
