#include "dialogs/FindDialog.h"
#include "PlaylistModel.h"

#include <QAction>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

FindDialog::FindDialog(PlaylistModel *model, const QString &initialText, QWidget *parent)
    : QDialog(parent), m_model(model)
{
    setWindowTitle(tr("Find a file in current list"));
    resize(424, 337);

    m_edit = new QLineEdit(this);
    m_edit->setClearButtonEnabled(true);
    m_list = new QListWidget(this);
    m_list->setUniformItemSizes(true);
    m_list->setContextMenuPolicy(Qt::ActionsContextMenu);

    auto *play = new QAction(tr("&Play item"), this);
    play->setShortcut(Qt::Key_Return);
    play->setShortcutContext(Qt::WidgetShortcut);
    connect(play, &QAction::triggered, this, &FindDialog::playCurrent);
    auto *queue = new QAction(tr("&Queue item"), this);
    queue->setShortcut(Qt::Key_Insert);
    connect(queue, &QAction::triggered, this, [this] {
        if (const quint64 id = currentId())
            emit queueRequested(id);
    });
    auto *select = new QAction(tr("Select item in list"), this);
    select->setShortcut(Qt::Key_QuoteLeft);
    connect(select, &QAction::triggered, this, [this] {
        if (const quint64 id = currentId()) {
            emit selectRequested(id);
            reject();
        }
    });
    auto *toggle = new QAction(tr("Toggle &view"), this);
    toggle->setShortcut(Qt::Key_F8);
    connect(toggle, &QAction::triggered, this, [this] {
        m_fullPath = !m_fullPath;
        refilter();
    });
    auto *sep = new QAction(this);
    sep->setSeparator(true);
    m_list->addActions({play, queue, select, sep, toggle});
    addActions({queue, select, toggle});

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &FindDialog::playCurrent);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_edit);
    layout->addWidget(m_list, 1);
    layout->addWidget(buttons);

    connect(m_edit, &QLineEdit::textChanged, this, &FindDialog::refilter);
    connect(m_list, &QListWidget::itemActivated, this, &FindDialog::playCurrent);
    m_edit->installEventFilter(this);

    m_edit->setText(initialText);
    m_edit->setFocus();
    refilter();
    // Showing the dialog may select the text; keep typing at the end instead.
    QTimer::singleShot(0, this, [this] {
        m_edit->deselect();
        m_edit->end(false);
    });
}

void FindDialog::appendText(const QString &text)
{
    m_edit->deselect();
    m_edit->end(false);
    m_edit->insert(text);
}

void FindDialog::refilter()
{
    const QStringList words = m_edit->text().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    m_list->clear();
    if (words.isEmpty())
        return;
    for (int row = 0; row < m_model->count(); ++row) {
        const QString path = m_model->pathAt(row);
        // Search "artist/album/file", not the folders above the collection.
        const QString haystack = path.section(QLatin1Char('/'), -3);
        bool match = true;
        for (const QString &w : words) {
            if (!haystack.contains(w, Qt::CaseInsensitive)) {
                match = false;
                break;
            }
        }
        if (!match)
            continue;
        auto *item = new QListWidgetItem(m_fullPath ? path : m_model->displayText(row), m_list);
        item->setData(Qt::UserRole, m_model->idAt(row));
        item->setToolTip(path);
    }
    if (m_list->count() > 0)
        m_list->setCurrentRow(0);
}

quint64 FindDialog::currentId() const
{
    const QListWidgetItem *item = m_list->currentItem();
    return item ? item->data(Qt::UserRole).toULongLong() : 0;
}

void FindDialog::playCurrent()
{
    if (const quint64 id = currentId()) {
        emit playRequested(id);
        accept();
    }
}

bool FindDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_edit && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        switch (ke->key()) {
        case Qt::Key_Down:
        case Qt::Key_Up:
        case Qt::Key_PageDown:
        case Qt::Key_PageUp:
            // Navigate the result list without leaving the edit field.
            QCoreApplication::sendEvent(m_list, event);
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            playCurrent();
            return true;
        default:
            break;
        }
    }
    return QDialog::eventFilter(watched, event);
}
