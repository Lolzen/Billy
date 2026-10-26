#include "dialogs/RenameDialog.h"
#include "audio/Decoder.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>

RenameDialog::RenameDialog(const QString &path, QWidget *parent)
    : QDialog(parent), m_path(path)
{
    setWindowTitle(tr("Rename single file"));
    resize(440, 471);

    const QFileInfo fi(path);
    m_edit = new QLineEdit(fi.fileName(), this);
    auto *clean = new QPushButton(tr("&R"), this);
    clean->setToolTip(tr("Removes unnecessary spaces, %20 or _ from filename."));
    clean->setFixedWidth(clean->fontMetrics().horizontalAdvance(QStringLiteral("WW")) + 12);
    connect(clean, &QPushButton::clicked, this, [this] { m_edit->setText(cleanName(m_edit->text())); });

    auto *editRow = new QHBoxLayout;
    editRow->addWidget(m_edit, 1);
    editRow->addWidget(clean);

    auto *line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);

    m_info = new QPlainTextEdit(this);
    m_info->setReadOnly(true);
    m_info->setFrameShape(QFrame::NoFrame);
    m_info->setLineWrapMode(QPlainTextEdit::NoWrap);
    QPalette pal = m_info->palette();
    pal.setColor(QPalette::Base, palette().color(QPalette::Window));
    m_info->setPalette(pal);

    QStringList lines;
    lines << tr("Path: %1").arg(QDir::toNativeSeparators(fi.absolutePath()));
    lines << tr("Size: %1").arg(QLocale().formattedDataSize(fi.size()));
    lines << tr("Modified: %1").arg(QLocale().toString(fi.lastModified(), QLocale::ShortFormat));
    const AudioInfo info = Audio::probe(path, true);
    if (info.ok) {
        QString fmt = tr("Format: %1").arg(info.format);
        lines << fmt;
        QString details = QStringLiteral("  %1 Hz %2").arg(info.sampleRate).arg(info.channels == 1 ? tr("Mono") : info.channels == 2 ? tr("Stereo") : tr("%1 Channels").arg(info.channels));
        if (info.bitrate > 0)
            details += QStringLiteral(", %1 kbps").arg(info.bitrate);
        lines << details;
        if (info.seconds >= 0)
            lines << tr("Length: %1").arg(Audio::formatTime(info.seconds));
        if (!info.tags.isEmpty())
            lines << QString() << info.tags;
    } else {
        lines << tr("Format: %1").arg(info.error.isEmpty() ? tr("Unknown format") : info.error);
    }
    m_info->setPlainText(lines.join(QLatin1Char('\n')));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &RenameDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(tr("Rename file:"), this));
    layout->addLayout(editRow);
    layout->addWidget(line);
    layout->addWidget(m_info, 1);
    layout->addWidget(buttons);

    m_edit->setFocus();
    // Select the name without the extension, ready to type over.
    m_edit->setSelection(0, fi.completeBaseName().size());
}

QString RenameDialog::cleanName(QString name)
{
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    QString ext = dot > 0 ? name.mid(dot) : QString();
    QString base = dot > 0 ? name.left(dot) : name;
    base.replace(QLatin1String("%20"), QLatin1String(" "));
    base.replace(QLatin1Char('_'), QLatin1Char(' '));
    base = base.simplified();
    base.replace(QRegularExpression(QStringLiteral("\\s*-\\s*")), QStringLiteral(" - "));
    return base + ext.trimmed();
}

void RenameDialog::accept()
{
    const QString name = m_edit->text().trimmed();
    const QFileInfo fi(m_path);
    if (name == fi.fileName()) {
        QDialog::reject();
        return;
    }
    if (name.isEmpty() || name.contains(QLatin1Char('/'))) {
        QMessageBox::warning(this, windowTitle(), tr("Can't rename file."));
        return;
    }
    if (QFileInfo(name).suffix().isEmpty()) {
        QMessageBox::warning(this, windowTitle(),
                             tr("Please end your new filename with an extension (ie \".mp3\" or \".wav\")"));
        return;
    }
    const QString target = fi.absoluteDir().filePath(name);
    const bool caseOnly = target.compare(m_path, Qt::CaseInsensitive) == 0;
    if (!caseOnly && QFileInfo::exists(target)) {
        QMessageBox::warning(this, windowTitle(), tr("A file with this name already exists."));
        return;
    }
    if (!QFile::rename(m_path, target)) {
        QMessageBox::warning(this, windowTitle(), tr("Can't rename file."));
        return;
    }
    m_newPath = target;
    QDialog::accept();
}
