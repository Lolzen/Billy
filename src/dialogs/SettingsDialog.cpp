#include "dialogs/SettingsDialog.h"

#include <QAudioDevice>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMediaDevices>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <cmath>

namespace {

QLabel *hint(const QString &text, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    l->setWordWrap(true);
    QPalette pal = l->palette();
    pal.setColor(QPalette::WindowText, pal.color(QPalette::Disabled, QPalette::WindowText));
    l->setPalette(pal);
    return l;
}

} // namespace

SettingsDialog::SettingsDialog(const Settings &settings, const Scheme &scheme, const QString &outputInfo, QWidget *parent)
    : QDialog(parent), m_settings(settings), m_scheme(scheme), m_outputInfo(outputInfo)
{
    setWindowTitle(tr("Settings"));
    resize(560, 400);

    m_tree = new QTreeWidget(this);
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(false);
    m_tree->setFixedWidth(130);
    m_stack = new QStackedWidget(this);

    const QList<QPair<QString, QWidget *>> pages = {
        {tr("Media"), createMediaPage()},
        {tr("Display"), createDisplayPage()},
        {tr("Appearance"), createAppearancePage()},
        {tr("Device"), createDevicePage()},
        {tr("Setup"), createSetupPage()},
    };
    for (const auto &page : pages) {
        new QTreeWidgetItem(m_tree, {page.first});
        m_stack->addWidget(page.second);
    }
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (item)
            m_stack->setCurrentIndex(m_tree->indexOfTopLevelItem(item));
    });
    m_tree->setCurrentItem(m_tree->topLevelItem(0));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);

    auto *top = new QHBoxLayout;
    top->addWidget(m_tree);
    top->addWidget(m_stack, 1);
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(top, 1);
    layout->addWidget(buttons);
}

void SettingsDialog::emitChanged()
{
    if (!m_updating)
        emit changed();
}

QWidget *SettingsDialog::createMediaPage()
{
    auto *page = new QGroupBox(tr("Media"), this);
    m_musicFolders = new QListWidget(page);
    m_musicFolders->setSelectionMode(QAbstractItemView::ExtendedSelection);
    for (const QString &f : m_settings.musicFolders)
        m_musicFolders->addItem(f);
    auto *add = new QPushButton(tr("&Add"), page);
    auto *del = new QPushButton(tr("&Delete"), page);
    connect(add, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Set your music folder"), QDir::homePath());
        if (dir.isEmpty() || m_settings.musicFolders.contains(dir))
            return;
        m_settings.musicFolders << dir;
        m_musicFolders->addItem(dir);
        emitChanged();
    });
    connect(del, &QPushButton::clicked, this, [this] {
        for (QListWidgetItem *item : m_musicFolders->selectedItems()) {
            m_settings.musicFolders.removeAll(item->text());
            delete item;
        }
        emitChanged();
    });

    auto *sortFile = new QRadioButton(tr("On &filename"), page);
    auto *sortPath = new QRadioButton(tr("On &path and filename (default)"), page);
    (m_settings.sortWay == SortWay::FileName ? sortFile : sortPath)->setChecked(true);
    connect(sortFile, &QRadioButton::toggled, this, [this](bool on) {
        m_settings.sortWay = on ? SortWay::FileName : SortWay::PathAndFileName;
        emitChanged();
    });

    auto *buttons = new QVBoxLayout;
    buttons->addWidget(add);
    buttons->addWidget(del);
    buttons->addStretch(1);
    auto *folders = new QHBoxLayout;
    folders->addWidget(m_musicFolders, 1);
    folders->addLayout(buttons);

    auto *layout = new QVBoxLayout(page);
    layout->addWidget(new QLabel(tr("Your music folder(s)"), page));
    layout->addLayout(folders, 1);
    layout->addWidget(hint(tr("Music folders are the roots of the open folder dialog, and are removed from the "
                              "names when viewing the playlist with relative paths (genre > artist > album > song)."),
                           page));
    layout->addWidget(new QLabel(tr("Sort when adding files"), page));
    layout->addWidget(sortFile);
    layout->addWidget(sortPath);
    return page;
}

QWidget *SettingsDialog::createDisplayPage()
{
    auto *page = new QGroupBox(tr("Display"), this);
    auto *hide = new QRadioButton(tr("&Hide"), page);
    auto *led6 = new QRadioButton(tr("&7 leds"), page);
    auto *led14 = new QRadioButton(tr("&14 leds"), page);
    auto *group = new QButtonGroup(page);
    group->addButton(hide, int(VuStyle::None));
    group->addButton(led6, int(VuStyle::Led6));
    group->addButton(led14, int(VuStyle::Led14));
    group->button(int(m_settings.vuStyle))->setChecked(true);
    connect(group, &QButtonGroup::idClicked, this, [this](int id) {
        m_settings.vuStyle = VuStyle(id);
        emitChanged();
    });

    auto *controls = new QCheckBox(tr("Show play controls"), page);
    controls->setChecked(m_settings.showPlayBar);
    connect(controls, &QCheckBox::toggled, this, [this](bool on) {
        m_settings.showPlayBar = on;
        emitChanged();
    });
    auto *progress = new QCheckBox(tr("Show progress panel"), page);
    progress->setChecked(m_settings.showProgressPanel);
    connect(progress, &QCheckBox::toggled, this, [this](bool on) {
        m_settings.showProgressPanel = on;
        emitChanged();
    });
    auto *delimiter = new QLineEdit(m_settings.relativeDelimiter, page);
    connect(delimiter, &QLineEdit::textChanged, this, [this](const QString &t) {
        m_settings.relativeDelimiter = t;
        emitChanged();
    });

    auto *layout = new QVBoxLayout(page);
    layout->addWidget(new QLabel(tr("Calibrated dB level meter"), page));
    auto *row6 = new QHBoxLayout;
    row6->addWidget(led6);
    row6->addWidget(hint(QStringLiteral("-60 30 15 6 3 1"), page), 1);
    auto *row14 = new QHBoxLayout;
    row14->addWidget(led14);
    row14->addWidget(hint(QStringLiteral("-oo 60 40 30 25 20 15 12 10 8 6 4 2 0"), page), 1);
    layout->addWidget(hide);
    layout->addLayout(row6);
    layout->addLayout(row14);
    layout->addSpacing(12);
    layout->addWidget(controls);
    layout->addWidget(progress);
    layout->addSpacing(12);
    auto *form = new QFormLayout;
    form->addRow(tr("Relative path delimiter:"), delimiter);
    layout->addLayout(form);
    layout->addStretch(1);
    return page;
}

QStringList SettingsDialog::schemeFiles()
{
    QDir dir(Settings::schemesDir());
    QStringList files;
    for (const QString &f : dir.entryList({QStringLiteral("*.billyscheme")}, QDir::Files, QDir::Name))
        files << dir.filePath(f);
    return files;
}

QWidget *SettingsDialog::createAppearancePage()
{
    auto *page = new QGroupBox(tr("Appearance"), this);
    m_schemes = new QComboBox(page);
    auto *save = new QPushButton(tr("&Save"), page);
    m_deleteScheme = new QPushButton(tr("&Delete"), page);
    m_schemeItem = new QComboBox(page);
    m_schemeItem->addItems(Settings::schemeItemNames());
    m_colorButton = new QPushButton(page);
    m_colorButton->setFixedWidth(60);
    m_bold = new QCheckBox(tr("Bold"), page);
    m_fontName = new QFontComboBox(page);
    m_fontSize = new QComboBox(page);
    m_fontSize->addItem(tr("Default"), 0);
    for (int s : {6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 28, 34, 40, 50, 60, 80, 100})
        m_fontSize->addItem(QString::number(s), s);
    auto *explore = new QPushButton(tr("&Explore..."), page);

    connect(m_schemes, &QComboBox::activated, this, [this](int index) {
        const QString file = m_schemes->itemData(index).toString();
        m_scheme = file.isEmpty() ? Scheme::systemScheme() : Scheme::load(file);
        m_settings.activeScheme = file.isEmpty() ? QString() : QFileInfo(file).completeBaseName();
        updateSchemeControls();
        emitChanged();
    });
    connect(m_schemeItem, &QComboBox::currentIndexChanged, this, &SettingsDialog::updateSchemeControls);
    connect(m_colorButton, &QPushButton::clicked, this, [this] {
        const int item = m_schemeItem->currentIndex();
        const QColor c = QColorDialog::getColor(m_scheme.color(item), this, m_schemeItem->currentText());
        if (!c.isValid())
            return;
        m_scheme.setColor(item, c);
        updateSchemeControls();
        emitChanged();
    });
    connect(m_bold, &QCheckBox::toggled, this, [this](bool on) {
        if (m_updating)
            return;
        m_scheme.playingBold = on;
        m_scheme.systemDefault = false;
        emitChanged();
    });
    connect(m_fontName, &QFontComboBox::currentFontChanged, this, [this](const QFont &f) {
        if (m_updating)
            return;
        m_scheme.fontName = f.family();
        emitChanged();
    });
    connect(m_fontSize, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_updating)
            return;
        m_scheme.fontSize = m_fontSize->currentData().toInt();
        emitChanged();
    });
    connect(save, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QString current = m_settings.activeScheme.isEmpty() ? tr("My scheme") : m_settings.activeScheme;
        const QString name = QInputDialog::getText(this, tr("Save scheme"), tr("Save this color scheme as:"),
                                                   QLineEdit::Normal, current, &ok).trimmed();
        if (!ok || name.isEmpty())
            return;
        const QString file = QDir(Settings::schemesDir()).filePath(name + QStringLiteral(".billyscheme"));
        if (QFileInfo::exists(file) && name != m_settings.activeScheme
            && QMessageBox::question(this, tr("Save scheme"), tr("Replace existing scheme?")) != QMessageBox::Yes)
            return;
        m_scheme.name = name;
        m_scheme.save(file);
        m_settings.activeScheme = name;
        fillSchemes();
        emitChanged();
    });
    connect(m_deleteScheme, &QPushButton::clicked, this, [this] {
        const QString file = m_schemes->currentData().toString();
        if (file.isEmpty())
            return;
        if (QMessageBox::question(this, tr("Delete scheme"), tr("Are you sure you want to delete this scheme?"))
            != QMessageBox::Yes)
            return;
        QFile::remove(file);
        m_settings.activeScheme.clear();
        m_scheme = Scheme::systemScheme();
        fillSchemes();
        updateSchemeControls();
        emitChanged();
    });
    connect(explore, &QPushButton::clicked, this,
            [] { QDesktopServices::openUrl(QUrl::fromLocalFile(Settings::schemesDir())); });

    auto *schemeRow = new QHBoxLayout;
    schemeRow->addWidget(m_schemes, 1);
    schemeRow->addWidget(save);
    schemeRow->addWidget(m_deleteScheme);
    auto *itemRow = new QHBoxLayout;
    itemRow->addWidget(m_schemeItem, 1);
    itemRow->addWidget(m_colorButton);
    itemRow->addWidget(m_bold);
    auto *fontRow = new QHBoxLayout;
    fontRow->addWidget(m_fontName, 1);
    fontRow->addWidget(m_fontSize);

    auto *layout = new QVBoxLayout(page);
    layout->addWidget(new QLabel(tr("Schemes"), page));
    layout->addLayout(schemeRow);
    layout->addWidget(new QLabel(tr("Items"), page));
    layout->addLayout(itemRow);
    layout->addWidget(new QLabel(tr("Playlist font"), page));
    layout->addLayout(fontRow);
    layout->addStretch(1);
    auto *bottom = new QHBoxLayout;
    bottom->addStretch(1);
    bottom->addWidget(explore);
    layout->addLayout(bottom);

    fillSchemes();
    updateSchemeControls();
    return page;
}

void SettingsDialog::fillSchemes()
{
    m_updating = true;
    m_schemes->clear();
    m_schemes->addItem(tr("System default"), QString());
    for (const QString &f : schemeFiles())
        m_schemes->addItem(QFileInfo(f).completeBaseName(), f);
    const int idx = m_settings.activeScheme.isEmpty() ? 0 : m_schemes->findText(m_settings.activeScheme);
    m_schemes->setCurrentIndex(qMax(0, idx));
    m_deleteScheme->setEnabled(m_schemes->currentIndex() > 0);
    m_updating = false;
}

void SettingsDialog::updateSchemeControls()
{
    m_updating = true;
    const int item = m_schemeItem->currentIndex();
    const bool isBold = item == 7;
    m_colorButton->setVisible(!isBold);
    m_bold->setVisible(isBold);
    m_bold->setChecked(m_scheme.playingBold);
    if (!isBold) {
        const QColor c = m_scheme.color(item);
        m_colorButton->setStyleSheet(QStringLiteral("background-color: %1; border: 1px solid palette(mid);").arg(c.name()));
    }
    const QFont f = m_scheme.listFont();
    m_fontName->setCurrentFont(f);
    const int sizeIdx = m_fontSize->findData(m_scheme.fontSize);
    m_fontSize->setCurrentIndex(sizeIdx < 0 ? 0 : sizeIdx);
    m_deleteScheme->setEnabled(m_schemes->currentIndex() > 0);
    m_updating = false;
}

QWidget *SettingsDialog::createDevicePage()
{
    auto *page = new QGroupBox(tr("Device"), this);
    auto *device = new QComboBox(page);
    device->addItem(tr("System default"), QByteArray());
    for (const QAudioDevice &d : QMediaDevices::audioOutputs()) {
        device->addItem(d.description(), d.id());
        if (d.id() == m_settings.outputDevice)
            device->setCurrentIndex(device->count() - 1);
    }
    connect(device, &QComboBox::activated, this, [this, device](int i) {
        m_settings.outputDevice = device->itemData(i).toByteArray();
        emitChanged();
    });
    auto *buffer = new QSpinBox(page);
    buffer->setRange(500, 10000);
    buffer->setSingleStep(500);
    buffer->setSuffix(tr(" ms"));
    buffer->setValue(m_settings.bufferLength);
    connect(buffer, &QSpinBox::valueChanged, this, [this](int v) {
        m_settings.bufferLength = v;
        emitChanged();
    });

    const double gain = std::pow(m_settings.volume / 100.0, 2);
    const QString dB = gain > 0 ? QString::number(20 * std::log10(gain), 'f', 1) + QStringLiteral(" dB")
                                : QStringLiteral("-oo dB");

    auto *form = new QFormLayout;
    form->addRow(tr("Billy output:"), device);
    form->addRow(tr("Buffer length:"), buffer);
    auto *layout = new QVBoxLayout(page);
    layout->addLayout(form);
    layout->addSpacing(8);
    layout->addWidget(new QLabel(tr("Using gapless playback for track changes"), page));
    layout->addWidget(new QLabel(tr("No EQ or filters: pure direct output"), page));
    layout->addWidget(new QLabel(tr("Billy output volume: %1 (%2)").arg(m_settings.volume).arg(dB), page));
    if (!m_outputInfo.isEmpty())
        layout->addWidget(new QLabel(tr("Sound details: %1").arg(m_outputInfo), page));
    layout->addStretch(1);
    return page;
}

QWidget *SettingsDialog::createSetupPage()
{
    auto *page = new QGroupBox(tr("Setup"), this);
    auto *e1 = new QRadioButton(tr("Play with &empty list"), page);
    auto *e2 = new QRadioButton(tr("&Add file to existing list"), page);
    auto *e3 = new QRadioButton(tr("Play and open its &folder (default)"), page);
    auto *group = new QButtonGroup(page);
    group->addButton(e1, int(EnqueueOption::EmptyList));
    group->addButton(e2, int(EnqueueOption::AddToList));
    group->addButton(e3, int(EnqueueOption::PlayAndOpenFolder));
    group->button(int(m_settings.enqueueOption))->setChecked(true);
    connect(group, &QButtonGroup::idClicked, this, [this](int id) {
        m_settings.enqueueOption = EnqueueOption(id);
        emitChanged();
    });

    auto *multi = new QCheckBox(tr("&Multiple instances"), page);
    multi->setToolTip(tr("Allows you to have multiple instances of Billy and play/mix them."));
    multi->setChecked(m_settings.multipleInstances);
    connect(multi, &QCheckBox::toggled, this, [this](bool on) {
        m_settings.multipleInstances = on;
        emitChanged();
    });
    auto *resume = new QCheckBox(tr("Auto &resume"), page);
    resume->setToolTip(tr("Resume your previous playing state when starting Billy."));
    resume->setChecked(m_settings.autoResume);
    connect(resume, &QCheckBox::toggled, this, [this](bool on) {
        m_settings.autoResume = on;
        emitChanged();
    });
    auto *tray = new QCheckBox(tr("Show tray icon (F11 minimizes to tray)"), page);
    tray->setChecked(m_settings.minimizeToTray);
    connect(tray, &QCheckBox::toggled, this, [this](bool on) {
        m_settings.minimizeToTray = on;
        emitChanged();
    });
    auto *softmute = new QSpinBox(page);
    softmute->setRange(0, 100);
    softmute->setSuffix(QStringLiteral(" %"));
    softmute->setValue(m_settings.softmutePercent);
    connect(softmute, &QSpinBox::valueChanged, this, [this](int v) {
        m_settings.softmutePercent = v;
        emitChanged();
    });

    auto *layout = new QVBoxLayout(page);
    layout->addWidget(new QLabel(tr("Opening a file from the file manager"), page));
    layout->addWidget(e1);
    layout->addWidget(e2);
    layout->addWidget(e3);
    layout->addSpacing(8);
    layout->addWidget(multi);
    layout->addWidget(resume);
    layout->addWidget(tray);
    auto *form = new QFormLayout;
    form->addRow(tr("Soft mute volume:"), softmute);
    layout->addLayout(form);
    layout->addSpacing(8);
    layout->addWidget(hint(tr("Global hotkeys: Billy registers itself as an MPRIS media player, so the media keys of "
                              "your keyboard and your desktop's media controls work while Billy is in the background."),
                           page));
    layout->addStretch(1);
    return page;
}
