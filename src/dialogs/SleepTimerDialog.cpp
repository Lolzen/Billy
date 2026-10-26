#include "dialogs/SleepTimerDialog.h"

#include <QCheckBox>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

SleepTimerDialog::SleepTimerDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Sleep timer"));

    m_minutes = new QSpinBox(this);
    m_minutes->setRange(1, 24 * 60);
    m_minutes->setValue(30);
    m_shutdown = new QCheckBox(tr("&Shutdown computer when done"), this);
    m_start = new QPushButton(tr("&Start"), this);
    m_start->setDefault(true);
    auto *close = new QPushButton(tr("&Close"), this);
    m_left = new QLabel(this);

    auto *line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);

    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Minutes left before quitting Billy:"), this));
    row->addWidget(m_minutes);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_left, 1);
    buttons->addWidget(m_start);
    buttons->addWidget(close);
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(row);
    layout->addWidget(m_shutdown);
    layout->addWidget(line);
    layout->addLayout(buttons);

    m_timer.setInterval(1000);
    connect(&m_timer, &QTimer::timeout, this, &SleepTimerDialog::tick);
    connect(m_start, &QPushButton::clicked, this, &SleepTimerDialog::toggle);
    connect(close, &QPushButton::clicked, this, &QDialog::hide);
}

void SleepTimerDialog::toggle()
{
    if (m_timer.isActive()) {
        m_timer.stop();
        m_start->setText(tr("&Start"));
        m_left->clear();
        m_minutes->setEnabled(true);
        emit statusChanged(QString());
        emit statusChanged(tr("Sleep timer aborted."));
        return;
    }
    m_deadline = QDateTime::currentMSecsSinceEpoch() + qint64(m_minutes->value()) * 60000;
    m_timer.start();
    m_start->setText(tr("&Abort"));
    m_minutes->setEnabled(false);
    tick();
    hide();
}

void SleepTimerDialog::tick()
{
    const qint64 left = (m_deadline - QDateTime::currentMSecsSinceEpoch()) / 1000;
    if (left <= 0) {
        m_timer.stop();
        emit statusChanged(QString());
        emit expired(m_shutdown->isChecked());
        return;
    }
    const QString text = tr("Sleep: %1:%2").arg(left / 60).arg(left % 60, 2, 10, QLatin1Char('0'));
    m_left->setText(text);
    emit statusChanged(text);
}
