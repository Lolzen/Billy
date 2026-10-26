#pragma once

#include <QDialog>
#include <QTimer>

class QCheckBox;
class QLabel;
class QPushButton;
class QSpinBox;

// Sleep timer (Alt+T): quits Billy (and optionally powers off) after N minutes.
class SleepTimerDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SleepTimerDialog(QWidget *parent = nullptr);
    bool isRunning() const { return m_timer.isActive(); }

signals:
    void expired(bool shutdown);
    void statusChanged(const QString &text); // empty when stopped

private:
    void toggle();
    void tick();

    QSpinBox *m_minutes;
    QCheckBox *m_shutdown;
    QPushButton *m_start;
    QLabel *m_left;
    QTimer m_timer;
    qint64 m_deadline = 0;
};
