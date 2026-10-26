#pragma once

#include "Settings.h"

#include <QDialog>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QFontComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTreeWidget;

// Settings (F6). Every change is applied immediately through changed().
class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    SettingsDialog(const Settings &settings, const Scheme &scheme, const QString &outputInfo, QWidget *parent = nullptr);

    const Settings &settings() const { return m_settings; }
    const Scheme &scheme() const { return m_scheme; }
    static QStringList schemeFiles();

signals:
    void changed();

private:
    QWidget *createMediaPage();
    QWidget *createDisplayPage();
    QWidget *createAppearancePage();
    QWidget *createDevicePage();
    QWidget *createSetupPage();
    void fillSchemes();
    void updateSchemeControls();
    void emitChanged();

    Settings m_settings;
    Scheme m_scheme;
    QString m_outputInfo;

    QTreeWidget *m_tree;
    QStackedWidget *m_stack;
    QListWidget *m_musicFolders;
    QComboBox *m_schemes;
    QComboBox *m_schemeItem;
    QPushButton *m_colorButton;
    QCheckBox *m_bold;
    QFontComboBox *m_fontName;
    QComboBox *m_fontSize;
    QPushButton *m_deleteScheme;
    bool m_updating = false;
};
