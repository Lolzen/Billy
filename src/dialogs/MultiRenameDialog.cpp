#include "dialogs/MultiRenameDialog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

const QString kCase = QStringLiteral("[case]");
const QString kUpper = QStringLiteral("[upper]");
const QString kLower = QStringLiteral("[lower]");

QString titleCase(const QString &s)
{
    QString out = s.toLower();
    bool start = true;
    for (QChar &c : out) {
        if (start && c.isLetter())
            c = c.toUpper();
        start = !c.isLetterOrNumber() && c != QLatin1Char('\'');
    }
    return out;
}

} // namespace

MultiRenameDialog::MultiRenameDialog(const QStringList &paths, QWidget *parent)
    : QDialog(parent), m_paths(paths)
{
    setWindowTitle(tr("Rename multiple files"));
    resize(423, 433);

    auto *intro = new QLabel(tr("If you want to rename an entire album, this tool can be useful.\n"
                                "Look for similarities in the filenames and change them all at the\n"
                                "same time by using the find and replace fields."),
                             this);
    m_find = new QLineEdit(this);
    m_replace = new QLineEdit(this);
    m_replace->setPlaceholderText(tr("[case], [upper] and [lower] are special codes"));
    auto *form = new QFormLayout;
    form->addRow(tr("Find text:"), m_find);
    form->addRow(tr("Replace text:"), m_replace);

    auto *line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);

    m_preview = new QPlainTextEdit(this);
    m_preview->setReadOnly(true);
    m_preview->setLineWrapMode(QPlainTextEdit::NoWrap);

    auto *tip = new QPushButton(tr("&Tip"), this);
    auto *replaceAll = new QPushButton(tr("&Replace all"), this);
    auto *close = new QPushButton(tr("&Close"), this);
    close->setDefault(true);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(tip);
    buttons->addStretch(1);
    buttons->addWidget(replaceAll);
    buttons->addWidget(close);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addLayout(form);
    layout->addWidget(line);
    layout->addWidget(new QLabel(tr("Preview:"), this));
    layout->addWidget(m_preview, 1);
    layout->addLayout(buttons);

    connect(m_find, &QLineEdit::textChanged, this, &MultiRenameDialog::updatePreview);
    connect(m_replace, &QLineEdit::textChanged, this, &MultiRenameDialog::updatePreview);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(replaceAll, &QPushButton::clicked, this, &MultiRenameDialog::renameAll);
    connect(tip, &QPushButton::clicked, this, [this] {
        QMessageBox::information(
            this, tr("Tip"),
            tr("Make a copy of a certain music folder, to try before damaging your filenames.\n\n"
               "This can be a dangerous tool, think what will happen when searching for a character \"a\" "
               "and replace it with \"\" (nothing) on all your files. There's no way to undo that operation, "
               "so beware.\n\n"
               "It all depends on how you're organizing your albums. But here are two examples:\n"
               "1. If you want to remove a certain text part from a range of songs, search for the text and "
               "replace it with nothing.\n"
               "2. To add a certain text (prefix) to a range of songs, search for nothing and replace it with "
               "some text.\n\n"
               "Special codes (for replace field):\n"
               "[case] : This Makes Every Title Like This.\n"
               "[upper] : MAKES EVERYTHING UPPER CASE.\n"
               "[lower] : makes everything lower case."));
    });
    m_find->setFocus();
    updatePreview();
}

QString MultiRenameDialog::transform(const QString &baseName, const QString &find, const QString &replace)
{
    QString repl = replace;
    const bool doCase = repl.contains(kCase, Qt::CaseInsensitive);
    const bool doUpper = repl.contains(kUpper, Qt::CaseInsensitive);
    const bool doLower = repl.contains(kLower, Qt::CaseInsensitive);
    repl.remove(kCase, Qt::CaseInsensitive);
    repl.remove(kUpper, Qt::CaseInsensitive);
    repl.remove(kLower, Qt::CaseInsensitive);

    QString name = baseName;
    if (find.isEmpty()) {
        name = repl + name; // nothing to find: add a prefix
    } else {
        name.replace(find, repl, Qt::CaseInsensitive);
    }
    if (doCase)
        name = titleCase(name);
    if (doUpper)
        name = name.toUpper();
    if (doLower)
        name = name.toLower();
    return name;
}

void MultiRenameDialog::updatePreview()
{
    QStringList lines;
    for (const QString &p : m_paths) {
        const QFileInfo fi(p);
        const QString suffix = fi.suffix().isEmpty() ? QString() : QStringLiteral(".") + fi.suffix();
        lines << transform(fi.completeBaseName(), m_find->text(), m_replace->text()) + suffix;
    }
    m_preview->setPlainText(lines.join(QLatin1Char('\n')));
}

void MultiRenameDialog::renameAll()
{
    if (m_paths.size() > 100
        && QMessageBox::question(this, windowTitle(), tr("You are about to rename > 100 files!\nContinue?"))
               != QMessageBox::Yes)
        return;
    if (QMessageBox::question(this, windowTitle(),
                              tr("Rename all selected files to the previewed filenames?\nThere's no undo!\n\nContinue?"))
        != QMessageBox::Yes)
        return;

    QStringList failed;
    int count = 0;
    for (QString &p : m_paths) {
        const QFileInfo fi(p);
        const QString suffix = fi.suffix().isEmpty() ? QString() : QStringLiteral(".") + fi.suffix();
        const QString base = transform(fi.completeBaseName(), m_find->text(), m_replace->text()).trimmed();
        const QString target = fi.absoluteDir().filePath(base + suffix);
        if (target == p)
            continue;
        if (base.isEmpty() || base.contains(QLatin1Char('/')) || QFileInfo::exists(target) || !QFile::rename(p, target)) {
            failed << fi.fileName();
            continue;
        }
        m_renamed << qMakePair(p, target);
        p = target;
        ++count;
    }
    QString msg = tr("%n file(s) have been renamed.", nullptr, count);
    if (!failed.isEmpty())
        msg += QStringLiteral("\n\n") + tr("Could not rename:") + QStringLiteral("\n") + failed.mid(0, 20).join(QLatin1Char('\n'));
    QMessageBox::information(this, windowTitle(), msg);
    m_find->clear();
    m_replace->clear();
    updatePreview();
}
