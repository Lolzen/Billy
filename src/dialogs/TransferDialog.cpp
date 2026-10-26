#include "dialogs/TransferDialog.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QStorageInfo>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

TransferDialog::TransferDialog(const QStringList &files, const QString &target, QWidget *parent)
    : QDialog(parent), m_files(files)
{
    setWindowTitle(tr("Copy selected files to external device"));
    resize(420, 230);
    for (const QString &f : m_files)
        m_totalBytes += QFileInfo(f).size();

    m_target = new QLineEdit(target, this);
    m_target->setReadOnly(true);
    auto *browse = new QPushButton(QStringLiteral("..."), this);
    browse->setFixedWidth(32);
    m_selected = new QLabel(this);
    m_free = new QLabel(this);
    m_inUse = new QLabel(this);
    m_status = new QLabel(this);
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 1000);
    m_progress->setValue(0);
    m_start = new QPushButton(tr("&Start"), this);
    m_start->setDefault(true);
    auto *explore = new QPushButton(tr("&Explore..."), this);
    auto *cancel = new QPushButton(tr("&Cancel"), this);

    auto *targetRow = new QHBoxLayout;
    targetRow->addWidget(m_target, 1);
    targetRow->addWidget(browse);
    auto *form = new QFormLayout;
    form->addRow(tr("Selected files:"), m_selected);
    form->addRow(tr("Device free:"), m_free);
    form->addRow(tr("Afterwards in use:"), m_inUse);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(explore);
    buttons->addStretch(1);
    buttons->addWidget(m_start);
    buttons->addWidget(cancel);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(tr("Copy selected files to:"), this));
    layout->addLayout(targetRow);
    layout->addLayout(form);
    layout->addWidget(m_status);
    layout->addWidget(m_progress);
    layout->addLayout(buttons);

    connect(browse, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Select dir"), m_target->text());
        if (!dir.isEmpty()) {
            m_target->setText(dir);
            updateInfo();
        }
    });
    connect(explore, &QPushButton::clicked, this, [this] {
        if (!m_target->text().isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_target->text()));
    });
    connect(cancel, &QPushButton::clicked, this, &TransferDialog::reject);
    connect(m_start, &QPushButton::clicked, this, &TransferDialog::start);

    // Removable devices come and go: keep the free space up to date.
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &TransferDialog::updateInfo);
    timer->start(3000);
    updateInfo();
}

TransferDialog::~TransferDialog()
{
    if (m_worker) {
        m_cancel = true;
        m_worker->wait();
    }
}

QString TransferDialog::target() const
{
    return m_target->text();
}

void TransferDialog::updateInfo()
{
    const QLocale loc;
    m_selected->setText(tr("%1 file(s), %2").arg(m_files.size()).arg(loc.formattedDataSize(m_totalBytes)));
    const QString dir = m_target->text();
    if (dir.isEmpty() || !QFileInfo(dir).isDir()) {
        m_free->setText(tr("Not connected."));
        m_inUse->clear();
        if (!m_worker)
            m_start->setEnabled(false);
        return;
    }
    const QStorageInfo storage(dir);
    m_free->setText(loc.formattedDataSize(storage.bytesAvailable()));
    const qint64 used = storage.bytesTotal() - storage.bytesAvailable() + m_totalBytes;
    const int pct = storage.bytesTotal() > 0 ? int(100.0 * used / storage.bytesTotal()) : 0;
    m_inUse->setText(QStringLiteral("%1 %").arg(pct));
    if (!m_worker)
        m_start->setEnabled(m_totalBytes <= storage.bytesAvailable());
}

void TransferDialog::start()
{
    const QString dir = m_target->text();
    if (!QFileInfo(dir).isDir() || !QFileInfo(dir).isWritable()) {
        QMessageBox::warning(this, windowTitle(),
                             tr("The path cannot be accessed.\nIf it is an external device, please make sure it's "
                                "properly connected."));
        return;
    }
    m_start->setEnabled(false);
    m_cancel = false;
    const QStringList files = m_files;
    const qint64 total = qMax<qint64>(1, m_totalBytes);
    m_worker = QThread::create([this, files, dir, total] {
        qint64 done = 0;
        int copied = 0;
        QByteArray buf(1 << 20, Qt::Uninitialized);
        for (const QString &src : files) {
            if (m_cancel)
                break;
            const QString dst = QDir(dir).filePath(QFileInfo(src).fileName());
            QMetaObject::invokeMethod(this, [this, src] { m_status->setText(QFileInfo(src).fileName()); });
            QFile in(src), out(dst);
            if (QFileInfo::exists(dst) || !in.open(QIODevice::ReadOnly) || !out.open(QIODevice::WriteOnly)) {
                done += QFileInfo(src).size();
                continue;
            }
            bool ok = true;
            while (!in.atEnd() && !m_cancel) {
                const qint64 n = in.read(buf.data(), buf.size());
                if (n < 0 || out.write(buf.constData(), n) != n) {
                    ok = false;
                    break;
                }
                done += n;
                const int value = int(1000 * done / total);
                QMetaObject::invokeMethod(this, [this, value] { m_progress->setValue(value); });
            }
            out.close();
            if (!ok || m_cancel)
                out.remove();
            else
                ++copied;
        }
        QMetaObject::invokeMethod(this, [this, copied] {
            m_worker->deleteLater();
            m_worker = nullptr;
            m_progress->setValue(1000);
            updateInfo();
            m_status->setText(tr("%n file(s) have been copied.", nullptr, copied));
            m_start->setEnabled(true);
        });
    });
    m_worker->setParent(this);
    m_worker->start();
}

void TransferDialog::reject()
{
    if (m_worker) {
        if (QMessageBox::question(this, windowTitle(), tr("Abort operation?")) != QMessageBox::Yes)
            return;
        m_cancel = true;
        m_worker->wait();
    }
    QDialog::reject();
}
