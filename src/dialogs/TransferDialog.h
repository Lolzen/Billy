#pragma once

#include <QDialog>

#include <atomic>

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QThread;

// "Copy selected files to external device" (Ctrl+M).
class TransferDialog : public QDialog
{
    Q_OBJECT
public:
    TransferDialog(const QStringList &files, const QString &target, QWidget *parent = nullptr);
    ~TransferDialog() override;
    QString target() const;

protected:
    void reject() override;

private:
    void updateInfo();
    void start();

    QStringList m_files;
    qint64 m_totalBytes = 0;
    QLineEdit *m_target;
    QLabel *m_selected;
    QLabel *m_free;
    QLabel *m_inUse;
    QLabel *m_status;
    QProgressBar *m_progress;
    QPushButton *m_start;
    QThread *m_worker = nullptr;
    std::atomic_bool m_cancel{false};
};
