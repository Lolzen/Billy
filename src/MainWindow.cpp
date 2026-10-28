#include "MainWindow.h"
#include "PlaylistModel.h"
#include "audio/Decoder.h"
#include "dialogs/DirDialog.h"
#include "dialogs/FavoritesDialog.h"
#include "dialogs/FindDialog.h"
#include "dialogs/InfoDialog.h"
#include "dialogs/MultiRenameDialog.h"
#include "dialogs/RenameDialog.h"
#include "dialogs/SettingsDialog.h"
#include "dialogs/SleepTimerDialog.h"
#include "dialogs/TransferDialog.h"
#include "widgets/Glyphs.h"
#include "widgets/PlaylistView.h"
#include "widgets/SheepProgress.h"
#include "PlayStats.h"
#include "audio/Spectrum.h"
#include "skin/SkinPlaylistDelegate.h"
#include "skin/SkinView.h"
#ifdef BILLY_HAVE_DBUS
#include "Mpris.h"
#include <QDBusConnection>
#include <QDBusMessage>
#endif

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kTickMs = 40;
const QString kProjectUrl = QStringLiteral("https://github.com/Lolzen/Billy");

// Station links often point to a .pls or .m3u file instead of the stream.
bool isRemotePlaylist(const QUrl &url)
{
    const QString p = url.path().toLower();
    return p.endsWith(QLatin1String(".pls")) || p.endsWith(QLatin1String(".m3u"));
}

bool isPlaylistFile(const QString &path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    return ext == QLatin1String("m3u") || ext == QLatin1String("m3u8") || ext == QLatin1String("pls")
        || ext == QLatin1String("txt");
}

} // namespace

MainWindow::MainWindow()
{
    setWindowTitle(QStringLiteral("Billy"));
    setWindowIcon(Glyphs::appIcon());
    m_settings.load();
    m_scheme = m_settings.activeScheme.isEmpty()
        ? Scheme::systemScheme()
        : Scheme::load(QDir(Settings::schemesDir()).filePath(m_settings.activeScheme + QStringLiteral(".billyscheme")));

    m_engine = new AudioEngine(this);
    m_model = new PlaylistModel(this);
    m_model->setPlayMode(m_settings.playMode);

    createWidgets();
    createActions();
    createTray();

    connect(m_engine, &AudioEngine::decodingStarted, this, &MainWindow::onDecodingStarted);
    connect(m_engine, &AudioEngine::trackStarted, this, &MainWindow::onTrackStarted);
    connect(m_engine, &AudioEngine::finished, this, &MainWindow::onFinished);
    connect(m_engine, &AudioEngine::error, this, &MainWindow::onEngineError);
    connect(m_engine, &AudioEngine::stateChanged, this, &MainWindow::onStateChanged);
    connect(m_engine, &AudioEngine::streamMetadata, this, &MainWindow::onStreamMetadata);
    connect(m_engine, &AudioEngine::formatChanged, m_statusFormat, &QLabel::setText);
    connect(m_engine, &AudioEngine::streamInfo, this, [this](quint64, const QString &codec, int kbps) {
        m_streamCodec = codec;
        m_streamKbps = kbps;
        updateSkinInfo();
        showStatus(kbps > 0 ? tr("Playing internet stream at %1 kbps").arg(kbps) : tr("Playing internet stream"));
    });

    // Anything that may change "what plays next".
    connect(m_model, &PlaylistModel::queueChanged, this, &MainWindow::refreshNext);
    connect(m_model, &QAbstractItemModel::rowsInserted, this, [this] { refreshNext(); updateStatusBar(); });
    connect(m_model, &QAbstractItemModel::rowsRemoved, this, [this] { refreshNext(); updateStatusBar(); });
    connect(m_model, &QAbstractItemModel::layoutChanged, this, &MainWindow::refreshNext);
    connect(m_model, &QAbstractItemModel::modelReset, this, [this] { refreshNext(); updateStatusBar(); });
    connect(m_model, &PlaylistModel::urlsDropped, this, &MainWindow::handleUrlsDropped);

    m_tick = new QTimer(this);
    m_tick->setInterval(kTickMs);
    connect(m_tick, &QTimer::timeout, this, &MainWindow::updateTick);
    m_tick->start();

#ifdef BILLY_HAVE_DBUS
    m_mpris = new Mpris(this);
#endif

    qApp->installEventFilter(this);
    applySettings();

    resize(394, 289);
    if (!m_settings.geometry.isEmpty())
        restoreGeometry(m_settings.geometry);
    if (m_settings.maximized)
        setWindowState(windowState() | Qt::WindowMaximized);
    applyLook(m_settings.alternativeLook, true);
}

MainWindow::~MainWindow()
{
    qApp->removeEventFilter(this);
    cancelScan();
    if (m_timeCancel)
        *m_timeCancel = true;
}

// ------------------------------------------------------------------ setup

void MainWindow::createWidgets()
{
    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_topPanel = new QWidget(central);
    m_topPanel->setFixedHeight(42);
    m_topPanel->installEventFilter(this);
    m_progress = new SheepProgress(SheepProgress::Normal, Qt::Horizontal, m_topPanel);
    m_progress->setMaximum(1000);
    m_progress->setCursor(Qt::PointingHandCursor);
    m_meter6 = new SheepProgress(SheepProgress::StereoLeds, Qt::Vertical, m_topPanel);
    m_meter6->setMaximum(7);
    m_meter6->setLedGeometry(3, 2, 2);
    m_meter6->setToolTip(tr("Calibrated level meter: -60 -30 -15 -6 -3 -1 dB"));
    m_meter14 = new SheepProgress(SheepProgress::StereoLeds, Qt::Horizontal, m_topPanel);
    m_meter14->setMaximum(14);
    m_meter14->setLedGeometry(9, 1, 2);
    m_scale14 = new MeterScale(m_topPanel);
    m_scale14->setScale({{0, QStringLiteral("-oo")}, {3, QStringLiteral("30")}, {5, QStringLiteral("20")},
                         {7, QStringLiteral("12")}, {9, QStringLiteral("8")}, {11, QStringLiteral("4")},
                         {13, QStringLiteral("0")}},
                        14);
    m_scale14->setCursor(Qt::SizeVerCursor);
    m_scale14->setToolTip(tr("Drag up/down to change the volume"));
    m_timeLabel = new QLabel(m_topPanel);
    m_volumeLabel = new QLabel(QStringLiteral("100 %"), m_topPanel);
    m_volumeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_volumeLabel->setCursor(Qt::SizeVerCursor);
    m_volumeLabel->setToolTip(tr("Tip: Numpad + - to change volume. Press the pause key to softmute (quickly change volume)"));
    for (QWidget *w : {static_cast<QWidget *>(m_volumeLabel), static_cast<QWidget *>(m_scale14),
                       static_cast<QWidget *>(m_meter6)})
        w->installEventFilter(this);

    m_playBar = new QWidget(m_topPanel);
    const QColor glyphColor = palette().color(QPalette::ButtonText);
    const QList<QPair<Glyphs::Glyph, QString>> buttons = {
        {Glyphs::Play, tr("Play")},         {Glyphs::Pause, tr("Pause")}, {Glyphs::Stop, tr("Stop")},
        {Glyphs::Previous, tr("Previous")}, {Glyphs::Next, tr("Next")},
    };
    int x = 0;
    for (const auto &b : buttons) {
        auto *button = new QToolButton(m_playBar);
        button->setAutoRaise(true);
        button->setIcon(Glyphs::icon(b.first, glyphColor));
        button->setIconSize(QSize(16, 16));
        button->setToolTip(b.second);
        button->setFocusPolicy(Qt::NoFocus);
        button->setGeometry(x, 0, 23, 22);
        x += 23;
        m_playButtons << button;
    }
    m_playBar->setFixedSize(x, 22);
    connect(m_playButtons[0], &QToolButton::clicked, this, &MainWindow::playSelected);
    connect(m_playButtons[1], &QToolButton::clicked, this, &MainWindow::togglePause);
    connect(m_playButtons[2], &QToolButton::clicked, this, &MainWindow::stopPlayback);
    connect(m_playButtons[3], &QToolButton::clicked, this, &MainWindow::previousTrack);
    connect(m_playButtons[4], &QToolButton::clicked, this, &MainWindow::nextTrack);

    connect(m_progress, &SheepProgress::mousePressed, this, [this](double f, Qt::MouseButton button) {
        if (button != Qt::LeftButton || m_engine->state() == AudioEngine::Stopped)
            return;
        m_seekDragging = true;
        m_progress->setValue(f * 1000);
    });
    connect(m_progress, &SheepProgress::mouseDragged, this, [this](double f) {
        if (!m_seekDragging)
            return;
        m_progress->setValue(f * 1000);
        const double d = m_engine->duration();
        if (d > 0)
            m_timeLabel->setText(Audio::formatTime(f * d) + QStringLiteral(" / ") + Audio::formatTime(d));
    });
    connect(m_progress, &SheepProgress::mouseReleased, this, [this](double f) {
        if (!m_seekDragging)
            return;
        m_seekDragging = false;
        const double d = m_engine->duration();
        if (d > 0)
            seekTo(f * d);
    });

    m_view = new PlaylistView(central);
    m_view->setModel(m_model);
    m_view->setContextMenuPolicy(Qt::ActionsContextMenu);
    connect(m_view, &QListView::activated, this, [this](const QModelIndex &i) { playId(m_model->idAt(i.row())); });
    connect(m_view, &PlaylistView::typeAhead, this, [this](const QString &t) { find(t); });
    connect(m_view, &PlaylistView::seekRequested, this, [this](int dir, Qt::KeyboardModifiers mods) {
        double step = 4;
        if (mods & Qt::ShiftModifier)
            step = 0.1;
        else if (mods & Qt::ControlModifier)
            step = 60;
        seekBy(dir * step);
    });
    connect(m_view, &PlaylistView::moveRequested, this, &MainWindow::moveSelection);

    layout->addWidget(m_topPanel);
    layout->addWidget(m_view, 1);
    m_classicPage = central;
    m_classicLayout = layout;
    m_stack = new QStackedWidget(this);
    m_stack->addWidget(central);
    setCentralWidget(m_stack);
    createSkin();

    // Status bar: files | play mode | messages | format of the playing file
    m_statusFiles = new QLabel(this);
    m_statusMode = new QLabel(this);
    m_statusText = new QLabel(this);
    m_statusFormat = new QLabel(this);
    m_statusFiles->setMinimumWidth(75);
    m_statusMode->setMinimumWidth(75);
    m_statusMode->setCursor(Qt::PointingHandCursor);
    m_statusMode->setToolTip(tr("Click to change the play mode"));
    m_statusMode->installEventFilter(this);
    m_statusFormat->setMinimumWidth(125);
    statusBar()->addWidget(m_statusFiles);
    statusBar()->addWidget(m_statusMode);
    statusBar()->addWidget(m_statusText, 1);
    statusBar()->addPermanentWidget(m_statusFormat);
    statusBar()->setSizeGripEnabled(true);
    m_statusClear = new QTimer(this);
    m_statusClear->setSingleShot(true);
    connect(m_statusClear, &QTimer::timeout, this, [this] { m_statusText->setText(m_sleepText); });
}

QAction *MainWindow::addAct(const QString &text, const QList<QKeySequence> &keys, const std::function<void()> &slot)
{
    auto *a = new QAction(text, this);
    if (!keys.isEmpty())
        a->setShortcuts(keys);
    a->setShortcutContext(Qt::WindowShortcut);
    connect(a, &QAction::triggered, this, [slot] { slot(); });
    addAction(a);
    return a;
}

void MainWindow::createActions()
{
    using K = QKeySequence;
    const auto CTRL = Qt::CTRL, ALT = Qt::ALT, SHIFT = Qt::SHIFT;

    // File
    QAction *openDir = addAct(tr("&Open folder..."), {K(Qt::Key_F4)}, [this] { openFolderDialog(false); });
    QAction *addDir = addAct(tr("&Add folder..."), {K(Qt::Key_F3)}, [this] { openFolderDialog(true); });
    QAction *addFile = addAct(tr("Add &file(s)..."), {K(CTRL | Qt::Key_L)}, [this] { addFiles(); });
    QAction *addUrlAc = addAct(tr("Add &url..."), {K(CTRL | Qt::Key_R)}, [this] { addUrl(); });
    QAction *openList = addAct(tr("&Open playlist..."), {K(CTRL | Qt::Key_O)}, [this] { openPlaylist(); });
    QAction *saveList = addAct(tr("&Save playlist..."), {K(CTRL | Qt::Key_S)}, [this] { savePlaylist(); });
    QAction *exitAc = addAct(tr("&Exit"), {K(Qt::Key_Escape)}, [this] {
        if (m_scanCancel || m_timeCancel)
            cancelScan(); // ESC aborts a running operation first
        else
            close();
    });

    // Edit
    QAction *props = addAct(tr("Properties..."), {K(Qt::Key_F2)}, [this] { fileInfo(); });
    QAction *remove = addAct(tr("&Remove"), {K(Qt::Key_Delete)}, [this] { removeSelected(); });
    QAction *trash = addAct(tr("&Delete..."), {K(SHIFT | Qt::Key_Delete)}, [this] { trashSelected(); });
    QAction *explore = addAct(tr("&Explore..."), {K(CTRL | Qt::Key_E)}, [this] { exploreSelected(); });
    QAction *refresh = addAct(tr("Check for new files"), {K(Qt::Key_F5)}, [this] { refreshList(); });
    QAction *jump = addAct(tr("Select &playing item"), {K(Qt::Key_QuoteLeft), K(Qt::Key_AsciiTilde)},
                           [this] { jumpToPlaying(); });
    QAction *sleep = addAct(tr("Sleep &timer..."), {K(ALT | Qt::Key_T)}, [this] {
        if (!m_sleepTimer) {
            m_sleepTimer = new SleepTimerDialog(this);
            connect(m_sleepTimer, &SleepTimerDialog::statusChanged, this, [this](const QString &t) {
                m_sleepText = t;
                if (!m_statusClear->isActive())
                    m_statusText->setText(t);
            });
            connect(m_sleepTimer, &SleepTimerDialog::expired, this, [this](bool shutdown) {
                if (shutdown && !QProcess::startDetached(QStringLiteral("systemctl"), {QStringLiteral("poweroff")}))
                    QProcess::startDetached(QStringLiteral("loginctl"), {QStringLiteral("poweroff")});
                close();
            });
        }
        m_sleepTimer->show();
        m_sleepTimer->raise();
        m_sleepTimer->activateWindow();
    });
    QAction *copySel = addAct(tr("Copy to MP3 player..."), {K(CTRL | Qt::Key_M)}, [this] { copyToDevice(); });
    QAction *crop = addAct(tr("&Crop selected or queued items"), {K(CTRL | Qt::Key_Delete)}, [this] { cropList(); });
    QAction *missing = addAct(tr("Remove &missing files from list"), {K(ALT | Qt::Key_Delete)}, [this] {
        showStatus(tr("%n missing file(s) removed.", nullptr, m_model->removeMissing()));
    });
    QAction *dups = addAct(tr("&Remove duplicates"), {K(CTRL | ALT | Qt::Key_D)}, [this] {
        showStatus(tr("%n duplicated item(s) have been removed.", nullptr, m_model->removeDuplicates()));
    });
    QAction *cut = addAct(tr("Cu&t playlist entry"), {K(CTRL | Qt::Key_X)}, [this] { copyEntries(true); });
    QAction *copy = addAct(tr("&Copy playlist entry"), {K(CTRL | Qt::Key_C)}, [this] { copyEntries(false); });
    QAction *paste = addAct(tr("&Paste playlist entry"), {K(CTRL | Qt::Key_V)}, [this] { pasteEntries(); });
    QAction *edit = addAct(tr("&Edit playlist entry..."), {K(SHIFT | Qt::Key_F2)}, [this] { editEntry(); });
    QAction *clear = addAct(tr("&Clear list"), {K(CTRL | Qt::Key_N)}, [this] {
        m_model->clear();
        m_settings.sourceFolders.clear();
    });
    QAction *clearHistory = addAct(tr("&Clear shuffle/queue history"), {K(CTRL | SHIFT | Qt::Key_C)},
                                   [this] { m_model->clearHistory(); });
    QAction *randomize = addAct(tr("&Randomize list"), {K(CTRL | ALT | Qt::Key_S)}, [this] { m_model->randomize(); });
    QAction *total = addAct(tr("Playing length selected item(s)..."), {K(CTRL | ALT | Qt::Key_T)}, [this] { totalTime(); });
    QAction *configDir = addAct(tr("&Explore configuration folder"), {K(CTRL | ALT | Qt::Key_E)},
                                [] { QDesktopServices::openUrl(QUrl::fromLocalFile(Settings::configDir())); });
    QAction *multiRename = addAct(tr("&Multiple filename renamer"), {}, [this] {
        const QList<int> rows = m_view->selectedRows();
        if (rows.size() < 2) {
            QMessageBox::information(this, tr("Rename multiple files"), tr("Select multiple items first, then press F2."));
            return;
        }
        fileInfo();
    });
    QAction *findAc = addAct(tr("&Find..."), {K(CTRL | Qt::Key_F)}, [this] { find(); });

    // Play
    QAction *play = addAct(tr("&Play"), {K(Qt::Key_Return), K(Qt::Key_Enter)}, [this] { playSelected(); });
    QAction *pause = addAct(tr("Pa&use"), {K(Qt::Key_Space)}, [this] { togglePause(); });
    QAction *stop = addAct(tr("&Stop"), {}, [this] { stopPlayback(); });
    QAction *next = addAct(tr("&Next"), {K(Qt::Key_Tab)}, [this] { nextTrack(); });
    QAction *prev = addAct(tr("&Previous"), {K(SHIFT | Qt::Key_Tab), K(Qt::Key_Backtab)}, [this] { previousTrack(); });
    QAction *queue = addAct(tr("&Queue"), {K(Qt::Key_Insert)}, [this] { queueSelected(); });
    addAct(tr("Total rewind"), {K(Qt::Key_Backspace)}, [this] { seekTo(0); });
    addAct(tr("Soft mute"), {K(Qt::Key_Pause)}, [this] { softmute(); });
    addAct(tr("Next"), {K(ALT | Qt::Key_Pause)}, [this] { nextTrack(); });
    addAct(tr("Previous"), {K(SHIFT | Qt::Key_Pause)}, [this] { previousTrack(); });
    addAct(tr("Reset volume"), {K(CTRL | Qt::Key_Space)}, [this] {
        m_volumeBeforeSoftmute = -1;
        setVolume(100);
        showStatus(tr("Billy volume reset to 100 %. Use your desktop's sound settings for the system mixer."));
    });

    // Favorites
    QAction *favAdd = addAct(tr("&Add playlist to favorites..."), {K(CTRL | Qt::Key_D)}, [this] { addToFavorites(); });
    QAction *favOrganize = addAct(tr("&Organize favorites..."), {K(CTRL | Qt::Key_B)}, [this] { organizeFavorites(); });
    for (int i = 0; i < 10; ++i) {
        QAction *fav = addAct(QString(), {K(CTRL | Qt::Key(Qt::Key_0 + (i + 1) % 10))}, [this, i] {
            const QStringList favs = FavoritesDialog::favorites();
            if (i < favs.size())
                loadPlaylistFile(favs.at(i));
        });
        m_favoriteActions << fav;
    }

    // Options
    QAction *prefs = addAct(tr("&Settings..."), {K(Qt::Key_F6)}, [this] { openSettings(); });
    m_modeGroup = new QActionGroup(this);
    const QList<QPair<PlayMode, QKeySequence>> modes = {
        {PlayMode::RepeatList, K(ALT | Qt::Key_N)}, {PlayMode::RepeatFile, K(ALT | Qt::Key_R)},
        {PlayMode::PlayList, K(ALT | Qt::Key_L)},   {PlayMode::PlayFile, K(ALT | Qt::Key_F)},
        {PlayMode::Shuffle, K(ALT | Qt::Key_S)},
    };
    const QStringList modeNames = {tr("&Repeat list"), tr("Repeat &file"), tr("&Play list"), tr("Play f&ile"), tr("&Shuffle")};
    for (int i = 0; i < modes.size(); ++i) {
        const PlayMode mode = modes[i].first;
        QAction *a = addAct(modeNames[i], {modes[i].second}, [this, mode] { setPlayMode(mode); });
        a->setCheckable(true);
        a->setData(int(mode));
        m_modeGroup->addAction(a);
    }
    QAction *modeToggle = addAct(tr("&Toggle"), {K(Qt::Key_F9)}, [this] {
        setPlayMode(PlayMode((int(m_settings.playMode) + 1) % 5));
    });

    // View
    m_viewGroup = new QActionGroup(this);
    const QStringList viewNames = {tr("View as &filename"), tr("View as full &path"), tr("View as &relative path")};
    for (int i = 0; i < 3; ++i) {
        QAction *a = addAct(viewNames[i], {}, [this, i] { setViewFormat(ViewFormat(i)); });
        a->setCheckable(true);
        a->setData(i);
        m_viewGroup->addAction(a);
    }
    QAction *viewToggle = addAct(tr("&Toggle"), {K(Qt::Key_F8)}, [this] {
        setViewFormat(ViewFormat((int(m_settings.viewFormat) + 1) % 3));
    });
    m_playBarAction = addAct(tr("Controls"), {}, [this] {
        m_settings.showPlayBar = m_playBarAction->isChecked();
        applySettings();
    });
    m_playBarAction->setCheckable(true);
    m_progressAction = addAct(tr("Progress"), {}, [this] {
        m_settings.showProgressPanel = m_progressAction->isChecked();
        applySettings();
    });
    m_progressAction->setCheckable(true);
    m_vuGroup = new QActionGroup(this);
    const QStringList vuNames = {tr("&No level meter"), tr("&6 Led level meter"), tr("&14 Led level meter")};
    for (int i = 0; i < 3; ++i) {
        QAction *a = addAct(vuNames[i], {}, [this, i] { setVuStyle(VuStyle(i)); });
        a->setCheckable(true);
        a->setData(i);
        m_vuGroup->addAction(a);
    }
    QAction *vuToggle = addAct(tr("&Toggle level meters"), {K(CTRL | SHIFT | Qt::Key_L)}, [this] {
        setVuStyle(VuStyle((int(m_settings.vuStyle) + 1) % 3));
    });
    QAction *minimize = addAct(tr("&Minimize to tray"), {K(Qt::Key_F11)}, [this] { minimizeToTray(); });
    addAct(tr("Maximize"), {K(Qt::Key_F12)}, [this] { isMaximized() ? showNormal() : showMaximized(); });

    // Help
    QAction *shortcuts = addAct(tr("Shortcuts..."), {K(Qt::Key_F1)}, [this] {
        InfoDialog dlg(tr("Shortcuts"), InfoDialog::shortcutsText(), this);
        dlg.exec();
    });
    QAction *docs = addAct(tr("&Documentation..."), {K(CTRL | Qt::Key_F1)},
                           [] { QDesktopServices::openUrl(QUrl(kProjectUrl)); });
    QAction *about = addAct(tr("&About..."), {}, [this] {
        InfoDialog dlg(tr("About Billy"), InfoDialog::aboutHtml(), this, true);
        dlg.exec();
    });

    // Menus
    QMenu *file = menuBar()->addMenu(tr("&File"));
    file->addActions({openDir, addDir, addFile, addUrlAc});
    file->addSeparator();
    file->addActions({openList, saveList});
    file->addSeparator();
    file->addAction(exitAc);

    QMenu *editMenu = menuBar()->addMenu(tr("&Edit"));
    editMenu->addActions({props, remove, trash, explore});
    QMenu *tools = editMenu->addMenu(tr("&Tools"));
    tools->addActions({refresh, jump, multiRename, sleep, copySel, crop, missing, dups, cut, copy, paste, edit,
                       clear, clearHistory, randomize, total, configDir});
    editMenu->addSeparator();
    editMenu->addAction(findAc);

    QMenu *playMenu = menuBar()->addMenu(tr("&Play"));
    playMenu->addActions({play, pause, stop});
    playMenu->addSeparator();
    playMenu->addActions({next, prev, queue});

    m_favoritesMenu = menuBar()->addMenu(tr("F&avorites"));
    m_favoritesMenu->addActions({favAdd, favOrganize});
    m_favoritesMenu->addSeparator();
    connect(m_favoritesMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildFavoritesMenu);

    QMenu *options = menuBar()->addMenu(tr("&Options"));
    options->addAction(prefs);
    options->addSeparator();
    options->addActions(m_modeGroup->actions());
    options->addAction(modeToggle);

    QMenu *view = menuBar()->addMenu(tr("&View"));
    view->addActions(m_viewGroup->actions());
    view->addAction(viewToggle);
    view->addSeparator();
    view->addActions({m_playBarAction, m_progressAction});
    QMenu *meters = view->addMenu(tr("&Level meter"));
    meters->addActions(m_vuGroup->actions());
    meters->addAction(vuToggle);
    view->addSeparator();
    m_lookAction = addAct(tr("&Alternative look"), {K(CTRL | SHIFT | Qt::Key_A)}, [this] {
        applyLook(m_lookAction->isChecked());
    });
    m_lookAction->setCheckable(true);
    view->addAction(m_lookAction);
    view->addAction(minimize);

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    help->addActions({shortcuts, docs});
    help->addSeparator();
    help->addAction(about);

    // Playlist context menu
    auto *sep = new QAction(this);
    sep->setSeparator(true);
    m_view->addActions({queue, remove, explore, sep, props});
}

void MainWindow::createTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;
    m_tray = new QSystemTrayIcon(Glyphs::appIcon(), this);
    m_tray->setToolTip(QStringLiteral("Billy"));
    auto *menu = new QMenu(this);
    for (int v : {100, 60, 30, 0})
        menu->addAction(QStringLiteral("&%1%").arg(v), this, [this, v] { setVolume(v); });
    menu->addSeparator();
    menu->addAction(tr("&Play"), this, &MainWindow::mprisPlay);
    menu->addAction(tr("Pa&use"), this, &MainWindow::togglePause);
    menu->addAction(tr("&Stop"), this, &MainWindow::stopPlayback);
    menu->addAction(tr("&Next"), this, &MainWindow::nextTrack);
    menu->addAction(tr("Previous"), this, &MainWindow::previousTrack);
    menu->addAction(tr("&Copy playing title"), this, [this] {
        if (m_model->currentId())
            QApplication::clipboard()->setText(mprisTitle());
    });
    menu->addSeparator();
    QAction *open = menu->addAction(tr("&Open"), this, &MainWindow::mprisRaise);
    menu->setDefaultAction(open);
    menu->addAction(tr("&Exit"), this, &MainWindow::close);
    m_tray->setContextMenu(menu);
    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
            if (isVisible() && !isMinimized())
                minimizeToTray();
            else
                mprisRaise();
        } else if (reason == QSystemTrayIcon::MiddleClick) {
            togglePause();
        }
    });
}

void MainWindow::applySettings()
{
    m_model->setViewFormat(m_settings.viewFormat, m_settings.musicFolders, m_settings.relativeDelimiter);
    m_view->setScheme(m_scheme);
    m_engine->setBufferLength(m_settings.bufferLength);
    m_engine->setOutputDevice(m_settings.outputDevice);
    m_engine->setVolume(m_settings.volume);

    for (QAction *a : m_modeGroup->actions())
        a->setChecked(a->data().toInt() == int(m_settings.playMode));
    for (QAction *a : m_viewGroup->actions())
        a->setChecked(a->data().toInt() == int(m_settings.viewFormat));
    for (QAction *a : m_vuGroup->actions())
        a->setChecked(a->data().toInt() == int(m_settings.vuStyle));
    m_playBarAction->setChecked(m_settings.showPlayBar);
    m_progressAction->setChecked(m_settings.showProgressPanel);

    m_topPanel->setVisible(m_settings.showProgressPanel || m_settings.showPlayBar);
    m_playBar->setVisible(m_settings.showPlayBar);
    m_progress->setVisible(m_settings.showProgressPanel);
    m_timeLabel->setVisible(m_settings.showProgressPanel);
    m_meter6->setVisible(m_settings.vuStyle == VuStyle::Led6);
    m_meter14->setVisible(m_settings.vuStyle == VuStyle::Led14);
    m_scale14->setVisible(m_settings.vuStyle == VuStyle::Led14);
    m_volumeLabel->setVisible(m_settings.vuStyle != VuStyle::Led14);
    if (m_tray)
        m_tray->setVisible(m_settings.minimizeToTray);
    layoutTopPanel();
    updateStatusBar();
}

void MainWindow::layoutTopPanel()
{
    const int w = m_topPanel->width();
    int right = w - 2;
    if (m_settings.vuStyle == VuStyle::Led6) {
        m_meter6->setGeometry(right - 16, 2, 16, 37);
        right -= 16 + 4;
    }
    int progressRight = right;
    if (m_settings.vuStyle == VuStyle::Led14) {
        const int mw = 143;
        m_meter14->setGeometry(right - mw, 2, mw, 16);
        m_scale14->setGeometry(right - mw, 22, mw, 16);
        progressRight = right - mw - 6;
    } else {
        const int lw = m_volumeLabel->fontMetrics().horizontalAdvance(QStringLiteral("100 %")) + 6;
        m_volumeLabel->setGeometry(right - lw, 21, lw, 18);
    }
    m_progress->setGeometry(2, 2, qMax(20, progressRight - 2), 16);
    const int barX = 0;
    m_playBar->move(barX, 19);
    const int timeX = m_settings.showPlayBar ? m_playBar->width() + 10 : 4;
    m_timeLabel->setGeometry(timeX, 21, qMax(10, right - timeX - 60), 18);
}

// --------------------------------------------------------- engine events

void MainWindow::playId(quint64 id, double startSeconds)
{
    const QString path = m_model->pathOf(id);
    if (path.isEmpty())
        return;
    m_errorsInRow = 0;
    m_streamTitle.clear();
    m_model->setCurrentId(id);
    recordPlay(id);
    if (PlaylistModel::isUrl(path))
        showStatus(tr("Connecting %1").arg(path), 30000);
    m_engine->play(path, id, startSeconds);
    updateTitle();
}

void MainWindow::onDecodingStarted(quint64 id)
{
    m_decodingId = id;
    refreshNext();
}

void MainWindow::refreshNext()
{
    if (!m_decodingId || m_engine->state() == AudioEngine::Stopped)
        return;
    const quint64 next = m_model->nextId(m_decodingId, false);
    m_engine->setNext(m_decodingId, m_model->pathOf(next), next);
}

void MainWindow::onTrackStarted(quint64 id)
{
    m_streamTitle.clear();
    m_model->setCurrentId(id);
    recordPlay(id);
    updateTitle();
    if (m_mpris)
        m_mpris->notifyChanged();
}

void MainWindow::onFinished()
{
    m_decodingId = 0;
    showStatus(tr("Stopped."));
    updateTitle();
}

void MainWindow::onEngineError(quint64 id, const QString &message)
{
    showStatus(QStringLiteral("%1: %2").arg(QFileInfo(m_model->pathOf(id)).fileName(), message), 10000);
    // Skipping is done by the gapless chain; stop when nothing is playable.
    if (++m_errorsInRow >= qMax(1, m_model->count()))
        stopPlayback();
}

void MainWindow::onStateChanged()
{
    const auto state = m_engine->state();
    m_playButtons[1]->setDown(state == AudioEngine::Paused);
    m_skin->setPlaying(state == AudioEngine::Playing);
    if (state != AudioEngine::Playing)
        m_skin->setSpectrum({});
    if (state == AudioEngine::Stopped)
        m_skin->setProgress(-1, QString());
    if (state == AudioEngine::Stopped) {
        m_progress->setValue(0);
        m_timeLabel->clear();
        m_meter6->setLevels(0, 0);
        m_meter14->setLevels(0, 0);
        m_dbLeft = m_dbRight = -100;
    }
    updateTitle();
    if (m_mpris)
        m_mpris->notifyChanged();
}

void MainWindow::updateTick()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const double dt = m_lastTick ? (now - m_lastTick) / 1000.0 : kTickMs / 1000.0;
    m_lastTick = now;
    if (!isVisible() || isMinimized())
        return; // all visuals are off while minimized
    const auto state = m_engine->state();
    if (state == AudioEngine::Stopped)
        return;
    if (m_skinActive)
        updateSkinTick();

    const bool buffering = m_engine->isBuffering();
    if (buffering != m_wasBuffering) {
        m_wasBuffering = buffering;
        if (buffering)
            showStatus(tr("Buffering..."), 60000);
        else
            showStatus(m_streamTitle.isEmpty() ? QString() : m_streamTitle, 1);
    }

    const double pos = m_engine->position();
    const double dur = m_engine->duration();
    if (!m_seekDragging) {
        m_progress->setValue(dur > 0 ? pos / dur * 1000 : 0);
        QString text = Audio::formatTime(pos);
        if (dur > 0)
            text += QStringLiteral(" / ") + Audio::formatTime(dur);
        if (state == AudioEngine::Paused)
            text += QStringLiteral("  ") + tr("(paused)");
        m_timeLabel->setText(text);
    }

    if (m_settings.vuStyle != VuStyle::None) {
        float l = 0, r = 0;
        m_engine->takeLevels(l, r);
        if (state == AudioEngine::Paused)
            l = r = 0;
        const double dbL = l > 0 ? 20 * std::log10(l) : -100;
        const double dbR = r > 0 ? 20 * std::log10(r) : -100;
        // Instant attack, smooth release.
        const double fall = 30.0 * dt;
        m_dbLeft = std::max(dbL, m_dbLeft - fall);
        m_dbRight = std::max(dbR, m_dbRight - fall);
        if (m_settings.vuStyle == VuStyle::Led6)
            m_meter6->setLevels(LevelScale::leds6(m_dbLeft), LevelScale::leds6(m_dbRight));
        else
            m_meter14->setLevels(LevelScale::leds14(m_dbLeft), LevelScale::leds14(m_dbRight));
    }
}

QString MainWindow::displayName(quint64 id) const
{
    const QString path = m_model->pathOf(id);
    if (path.isEmpty())
        return {};
    if (PlaylistModel::isUrl(path)) {
        const QString name = m_model->titleOf(id);
        return name.isEmpty() ? path : name;
    }
    return QFileInfo(path).completeBaseName();
}

void MainWindow::updateTitle()
{
    const quint64 id = m_model->currentId();
    QString title = QStringLiteral("Billy");
    if (id && m_engine->state() != AudioEngine::Stopped)
        title = (m_streamTitle.isEmpty() ? displayName(id) : m_streamTitle) + QStringLiteral(" - Billy");
    setWindowTitle(title);
    if (m_tray)
        m_tray->setToolTip(title);
}

void MainWindow::updateStatusBar()
{
    m_volumeLabel->setText(QStringLiteral("%1 %").arg(m_settings.volume));
    const int files = m_model->count();
    m_statusFiles->setText(files == 1 ? tr("1 File") : tr("%1 Files").arg(files));
    m_statusMode->setText(Settings::playModeName(m_settings.playMode));
}

void MainWindow::showStatus(const QString &text, int ms)
{
    if (m_skin)
        m_skin->showMessage(text, qMin(ms, 15000));
    m_statusText->setText(text);
    m_statusClear->start(ms);
}

// ------------------------------------------------------- playback control

void MainWindow::playSelected()
{
    const QModelIndex cur = m_view->currentIndex();
    quint64 id = cur.isValid() ? m_model->idAt(cur.row()) : 0;
    if (!id)
        id = m_model->currentId() ? m_model->currentId() : m_model->idAt(0);
    if (!id)
        return;
    if (m_engine->state() == AudioEngine::Paused && id == m_engine->currentId()) {
        m_engine->resume();
        return;
    }
    playId(id);
}

void MainWindow::togglePause()
{
    switch (m_engine->state()) {
    case AudioEngine::Playing:
        m_engine->pause();
        break;
    case AudioEngine::Paused:
        m_engine->resume();
        break;
    case AudioEngine::Stopped:
        mprisPlay();
        break;
    }
}

void MainWindow::stopPlayback()
{
    m_decodingId = 0;
    m_engine->stop();
}

void MainWindow::nextTrack()
{
    quint64 base = m_model->currentId();
    if (!base) {
        playSelected();
        return;
    }
    if (const quint64 next = m_model->nextId(base, true))
        playId(next);
}

void MainWindow::previousTrack()
{
    const quint64 base = m_model->currentId();
    if (!base) {
        playSelected();
        return;
    }
    if (const quint64 prev = m_model->previousId(base))
        playId(prev);
}

void MainWindow::seekTo(double seconds)
{
    if (m_engine->state() == AudioEngine::Stopped)
        return;
    const double d = m_engine->duration();
    if (d <= 0)
        return; // internet streams can't seek
    if (d > 0 && seconds >= d)
        seconds = d - 0.1;
    m_engine->seek(qMax(0.0, seconds));
}

void MainWindow::seekBy(double seconds)
{
    seekTo(m_engine->position() + seconds);
}

void MainWindow::setVolume(int percent)
{
    m_settings.volume = qBound(0, percent, 100);
    m_engine->setVolume(m_settings.volume);
    if (m_skin)
        m_skin->setVolume(m_settings.volume);
    updateStatusBar();
    if (m_mpris)
        m_mpris->notifyChanged();
}

void MainWindow::softmute()
{
    if (m_volumeBeforeSoftmute >= 0) {
        setVolume(m_volumeBeforeSoftmute);
        m_volumeBeforeSoftmute = -1;
    } else {
        m_volumeBeforeSoftmute = m_settings.volume;
        setVolume(m_settings.volume * m_settings.softmutePercent / 100);
    }
}

void MainWindow::setPlayMode(PlayMode mode)
{
    m_settings.playMode = mode;
    // Changing the play mode clears the queue and the shuffle history.
    m_model->setPlayMode(mode);
    for (QAction *a : m_modeGroup->actions())
        a->setChecked(a->data().toInt() == int(mode));
    updateStatusBar();
    refreshNext();
    if (m_mpris)
        m_mpris->notifyChanged();
}

void MainWindow::setViewFormat(ViewFormat f)
{
    if (f == ViewFormat::RelativePath && m_settings.musicFolders.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Billy"),
                                 tr("Before you start viewing in relative path style, please set your music folder "
                                    "first (Settings > Media)."));
    }
    m_settings.viewFormat = f;
    applySettings();
}

void MainWindow::setVuStyle(VuStyle style)
{
    m_settings.vuStyle = style;
    applySettings();
}

// -------------------------------------------------------- playlist commands

void MainWindow::openFolderDialog(bool add)
{
    DirDialog dlg(add ? tr("Add a folder with music files") : tr("Open a folder with music files"),
                  m_settings.musicFolders, m_settings.dirDialogRoot, m_settings.lastOpenedDir,
                  m_settings.includeSubfolders, this);
    connect(&dlg, &DirDialog::quickAddRequested, this, [this](const QString &dir, bool recursive) {
        startScan({dir}, recursive, QDateTime(), false, -1);
    });
    if (dlg.exec() != QDialog::Accepted)
        return;
    m_settings.dirDialogRoot = dlg.root();
    m_settings.includeSubfolders = dlg.includeSubfolders();
    const QStringList dirs = dlg.selectedDirs();
    if (dirs.isEmpty())
        return;
    m_settings.lastOpenedDir = dirs.first();
    startScan(dirs, dlg.includeSubfolders(), dlg.newerThan(), !add, -1);
}

void MainWindow::startScan(const QStringList &dirs, bool recursive, const QDateTime &newerThan, bool replace,
                           int insertRow, std::function<void()> done)
{
    cancelScan();
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_scanCancel = cancel;
    showStatus(tr("Opening... Press ESC to abort"), 600000);
    setCursor(Qt::BusyCursor);
    const SortWay sortWay = m_settings.sortWay;

    QThread *thread = QThread::create([this, dirs, recursive, newerThan, cancel, sortWay, replace, insertRow, done] {
        QStringList all;
        for (const QString &d : dirs) {
            QStringList files = PlaylistModel::scanFolder(d, recursive, cancel.get(), newerThan);
            all += files;
        }
        PlaylistModel::sortPaths(all, sortWay);
        QMetaObject::invokeMethod(this, [this, all, dirs, cancel, replace, insertRow, done] {
            if (*cancel)
                return;
            m_scanCancel.reset();
            unsetCursor();
            if (all.isEmpty()) {
                showStatus(tr("No music files found in given folder."));
                return;
            }
            if (replace) {
                m_model->clear();
                m_settings.sourceFolders = dirs;
            } else {
                for (const QString &d : dirs)
                    if (!m_settings.sourceFolders.contains(d))
                        m_settings.sourceFolders << d;
            }
            const int first = m_model->addPaths(all, insertRow);
            showStatus(tr("%n file(s) added.", nullptr, all.size()), 3000);
            if (replace)
                m_view->selectRows({0});
            else
                m_view->scrollTo(m_model->index(first));
            if (done)
                done();
        });
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void MainWindow::cancelScan()
{
    if (m_scanCancel) {
        *m_scanCancel = true;
        m_scanCancel.reset();
        unsetCursor();
        showStatus(tr("Aborted."));
    }
    if (m_timeCancel) {
        *m_timeCancel = true;
        m_timeCancel.reset();
        showStatus(tr("Aborted."));
    }
}

void MainWindow::addFiles()
{
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Add music file(s) to playlist"),
                                                            m_settings.lastOpenedDir, Audio::fileDialogFilter());
    if (files.isEmpty())
        return;
    m_settings.lastOpenedDir = QFileInfo(files.first()).absolutePath();
    m_model->addPaths(files);
}

void MainWindow::openPlaylist()
{
    const QString file = QFileDialog::getOpenFileName(this, tr("Open playlist"), m_settings.playlistFolder,
                                                      tr("Playlist (*.txt *.m3u *.m3u8 *.pls);;All files (*)"));
    if (file.isEmpty())
        return;
    m_settings.playlistFolder = QFileInfo(file).absolutePath();
    loadPlaylistFile(file);
}

void MainWindow::loadPlaylistFile(const QString &file, bool play)
{
    QHash<QString, QString> titles;
    const QStringList paths = PlaylistModel::readPlaylistFile(file, &titles);
    if (paths.isEmpty()) {
        showStatus(tr("The playlist is empty or could not be read."));
        return;
    }
    m_model->clear();
    m_settings.sourceFolders.clear();
    m_model->addPaths(paths);
    m_model->applyTitles(titles);
    m_view->selectRows({0});
    showStatus(QFileInfo(file).completeBaseName(), 3000);
    if (play)
        playId(m_model->idAt(0));
}

void MainWindow::savePlaylist()
{
    if (m_model->count() == 0) {
        QMessageBox::information(this, QStringLiteral("Billy"), tr("Your playlist is empty, please add some files first."));
        return;
    }
    QString selected;
    QString file = QFileDialog::getSaveFileName(
        this, tr("Save playlist"), m_settings.playlistFolder,
        tr("Billy playlist (*.txt);;M3U playlist (*.m3u);;PLS playlist (*.pls);;All files (*)"), &selected);
    if (file.isEmpty())
        return;
    if (QFileInfo(file).suffix().isEmpty()) {
        if (selected.contains(QLatin1String("m3u")))
            file += QStringLiteral(".m3u");
        else if (selected.contains(QLatin1String("pls")))
            file += QStringLiteral(".pls");
        else
            file += QStringLiteral(".txt");
    }
    m_settings.playlistFolder = QFileInfo(file).absolutePath();
    if (!PlaylistModel::writePlaylistFile(file, m_model->paths(), m_model->titles()))
        QMessageBox::warning(this, QStringLiteral("Billy"), tr("Your playlist could not be saved."));
}

void MainWindow::refreshList()
{
    if (m_settings.sourceFolders.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Billy"), tr("Open a folder first."));
        return;
    }
    const int removed = m_model->removeMissing();
    const QStringList dirs = m_settings.sourceFolders;
    const bool recursive = m_settings.includeSubfolders;
    QSet<QString> known;
    for (const QString &p : m_model->paths())
        known.insert(p);
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_scanCancel = cancel;
    showStatus(tr("Checking for new files..."), 600000);
    QThread *thread = QThread::create([this, dirs, recursive, known, cancel, removed] {
        QList<QPair<QDateTime, QString>> found;
        for (const QString &d : dirs)
            for (const QString &p : PlaylistModel::scanFolder(d, recursive, cancel.get()))
                if (!known.contains(p))
                    found << qMakePair(QFileInfo(p).lastModified(), p);
        // New files are added in order of their file date.
        std::sort(found.begin(), found.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        QStringList paths;
        for (const auto &f : found)
            paths << f.second;
        QMetaObject::invokeMethod(this, [this, paths, cancel, removed] {
            if (*cancel)
                return;
            m_scanCancel.reset();
            if (!paths.isEmpty()) {
                const int first = m_model->addPaths(paths);
                QList<int> rows;
                for (int i = 0; i < paths.size(); ++i)
                    rows << first + i;
                m_view->selectRows(rows);
            }
            showStatus(tr("%n new file(s) found, ", nullptr, paths.size())
                       + tr("%n missing file(s) removed.", nullptr, removed));
        });
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void MainWindow::fileInfo()
{
    const QList<int> rows = m_view->selectedRows();
    if (rows.isEmpty())
        return;
    if (rows.size() > 1) {
        QList<int> existing;
        for (int r : rows)
            if (QFileInfo::exists(m_model->pathAt(r)))
                existing << r;
        MultiRenameDialog dlg(m_model->paths(existing), this);
        dlg.exec();
        // Update the list with the new names.
        QHash<QString, QString> renamed;
        for (const auto &p : dlg.renamed())
            renamed.insert(p.first, p.second);
        for (int r = 0; r < m_model->count(); ++r) {
            QString path = m_model->pathAt(r);
            bool changed = false;
            while (renamed.contains(path)) { // a file may be renamed more than once
                path = renamed.take(path);
                changed = true;
            }
            if (changed) {
                m_model->setPathAt(r, path);
                m_engine->updatePath(m_model->idAt(r), path);
            }
        }
        return;
    }
    const int row = rows.first();
    const QString path = m_model->pathAt(row);
    if (PlaylistModel::isUrl(path)) {
        showStreamInfo(row);
        return;
    }
    if (!QFileInfo::exists(path)) {
        QMessageBox::warning(this, QStringLiteral("Billy"), tr("File not found."));
        return;
    }
    RenameDialog dlg(path, this);
    if (dlg.exec() == QDialog::Accepted && !dlg.newPath().isEmpty()) {
        m_model->setPathAt(row, dlg.newPath());
        m_engine->updatePath(m_model->idAt(row), dlg.newPath());
        updateTitle();
    }
}

void MainWindow::editEntry()
{
    const QModelIndex cur = m_view->currentIndex();
    if (!cur.isValid())
        return;
    bool ok = false;
    const QString path = QInputDialog::getText(this, tr("Edit playlist entry"), tr("Path:"), QLineEdit::Normal,
                                               m_model->pathAt(cur.row()), &ok);
    if (ok && !path.trimmed().isEmpty())
        m_model->setPathAt(cur.row(), path.trimmed());
}

void MainWindow::removeSelected()
{
    const QList<int> rows = m_view->selectedRows();
    if (rows.isEmpty())
        return;
    const int first = rows.first();
    m_model->removeRowList(rows);
    if (m_model->count() > 0)
        m_view->selectRows({qMin(first, m_model->count() - 1)});
}

void MainWindow::trashSelected()
{
    const QList<int> rows = m_view->selectedRows();
    if (rows.isEmpty())
        return;
    const QStringList paths = m_model->paths(rows);
    const QString question = paths.size() == 1
        ? tr("Move \"%1\" to the trash?").arg(QFileInfo(paths.first()).fileName())
        : tr("Move %n file(s) to the trash?", nullptr, paths.size());
    if (QMessageBox::question(this, tr("Delete"), question) != QMessageBox::Yes)
        return;
    QList<int> done;
    QStringList failed;
    for (int r : rows) {
        const QString p = m_model->pathAt(r);
        if (PlaylistModel::isUrl(p)) {
            done << r; // nothing on disk, just remove the entry
            continue;
        }
        if (m_model->idAt(r) == m_engine->currentId())
            stopPlayback();
        if (QFile::moveToTrash(p) || !QFileInfo::exists(p))
            done << r;
        else
            failed << QFileInfo(p).fileName();
    }
    m_model->removeRowList(done);
    if (!failed.isEmpty())
        QMessageBox::warning(this, tr("Delete"), tr("Tried to recycle file:") + QStringLiteral("\n") + failed.join(QLatin1Char('\n')));
}

void MainWindow::cropList()
{
    QList<int> rows = m_view->selectedRows();
    if (rows.size() <= 1 && !m_model->queue().isEmpty()) {
        rows.clear();
        for (quint64 id : m_model->queue())
            rows << m_model->rowOf(id);
    }
    if (rows.size() < 2 && m_model->queue().isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Billy"), tr("Please select or queue at least two files first."));
        return;
    }
    m_model->crop(rows);
}

void MainWindow::exploreSelected()
{
    const QModelIndex cur = m_view->currentIndex();
    if (!cur.isValid()) {
        QMessageBox::information(this, QStringLiteral("Billy"), tr("No file selected to open its folder."));
        return;
    }
    const QString path = m_model->pathAt(cur.row());
    if (PlaylistModel::isUrl(path)) {
        QMessageBox::information(this, QStringLiteral("Billy"), tr("No file selected to open its folder."));
        return;
    }
#ifdef BILLY_HAVE_DBUS
    // Ask the file manager to highlight the file, fall back to opening the folder.
    QDBusMessage msg = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.FileManager1"),
                                                      QStringLiteral("/org/freedesktop/FileManager1"),
                                                      QStringLiteral("org.freedesktop.FileManager1"),
                                                      QStringLiteral("ShowItems"));
    msg << QStringList{QUrl::fromLocalFile(path).toString()} << QString();
    const QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 2000);
    if (reply.type() != QDBusMessage::ErrorMessage)
        return;
#endif
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
}

void MainWindow::copyEntries(bool cut)
{
    const QList<int> rows = m_view->selectedRows();
    if (rows.isEmpty())
        return;
    m_clipboard = m_model->paths(rows);
    auto *mime = new QMimeData;
    QList<QUrl> urls;
    for (const QString &p : m_clipboard)
        urls << QUrl::fromLocalFile(p);
    mime->setUrls(urls);
    mime->setText(m_clipboard.join(QLatin1Char('\n')));
    QApplication::clipboard()->setMimeData(mime);
    if (cut)
        removeSelected();
}

void MainWindow::pasteEntries()
{
    QStringList paths;
    const QMimeData *mime = QApplication::clipboard()->mimeData();
    if (mime && mime->hasUrls()) {
        for (const QUrl &u : mime->urls()) {
            if (u.isLocalFile() && Audio::isSupported(u.toLocalFile()))
                paths << u.toLocalFile();
            else if (!u.isLocalFile() && PlaylistModel::isUrl(u.toString()))
                paths << u.toString();
        }
    } else if (mime && mime->hasText()) {
        for (const QString &line : mime->text().split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            const QString p = line.trimmed();
            if ((QFileInfo::exists(p) && Audio::isSupported(p)) || PlaylistModel::isUrl(p))
                paths << p;
        }
    }
    if (paths.isEmpty())
        paths = m_clipboard;
    if (paths.isEmpty())
        return;
    const QModelIndex cur = m_view->currentIndex();
    const int row = cur.isValid() ? cur.row() + 1 : m_model->count();
    const int first = m_model->addPaths(paths, row);
    QList<int> rows;
    for (int i = 0; i < paths.size(); ++i)
        rows << first + i;
    m_view->selectRows(rows);
}

void MainWindow::moveSelection(int delta)
{
    const QList<int> rows = m_view->selectedRows();
    if (rows.isEmpty())
        return;
    const QModelIndex cur = m_view->currentIndex();
    const int curOffset = cur.isValid() ? cur.row() - rows.first() : 0;
    const QList<int> moved = m_model->moveRowList(rows, delta);
    if (!moved.isEmpty())
        m_view->selectRows(moved, qBound(moved.first(), moved.first() + curOffset, moved.last()));
}

void MainWindow::queueSelected()
{
    QList<int> rows = m_view->selectedRows();
    if (rows.isEmpty() && m_view->currentIndex().isValid())
        rows << m_view->currentIndex().row();
    m_model->toggleQueue(rows);
}

void MainWindow::jumpToPlaying()
{
    const int row = m_model->currentRow();
    if (row >= 0) {
        m_view->selectRows({row});
        m_view->scrollTo(m_model->index(row), QAbstractItemView::PositionAtCenter);
    }
}

void MainWindow::find(const QString &text)
{
    if (m_model->count() == 0)
        return;
    if (m_findDialog) {
        m_findDialog->appendText(text);
        m_findDialog->raise();
        m_findDialog->activateWindow();
        return;
    }
    auto *dlg = new FindDialog(m_model, text, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    m_findDialog = dlg;
    connect(dlg, &FindDialog::playRequested, this, [this](quint64 id) {
        playId(id);
        const int row = m_model->rowOf(id);
        m_view->selectRows({row});
    });
    connect(dlg, &FindDialog::queueRequested, this, [this](quint64 id) { m_model->toggleQueue({m_model->rowOf(id)}); });
    connect(dlg, &FindDialog::selectRequested, this, [this](quint64 id) {
        const int row = m_model->rowOf(id);
        m_view->selectRows({row});
        m_view->scrollTo(m_model->index(row), QAbstractItemView::PositionAtCenter);
    });
    dlg->show();
}

void MainWindow::totalTime()
{
    QList<int> rows = m_view->selectedRows();
    if (rows.size() <= 1)
        for (int r = 0, n = m_model->count(); r < n; ++r)
            rows << r;
    QStringList paths = m_model->paths(rows);
    paths.erase(std::remove_if(paths.begin(), paths.end(), &PlaylistModel::isUrl), paths.end());
    if (paths.isEmpty())
        return;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_timeCancel = cancel;
    showStatus(tr("Calculating time... Press ESC to abort"), 600000);
    QThread *thread = QThread::create([this, paths, cancel] {
        double total = 0;
        int unknown = 0;
        for (int i = 0; i < paths.size() && !*cancel; ++i) {
            const AudioInfo info = Audio::probe(paths.at(i), false);
            if (info.seconds > 0)
                total += info.seconds;
            else
                ++unknown;
            if (i % 25 == 0) {
                const int done = i;
                QMetaObject::invokeMethod(this, [this, done, n = paths.size(), cancel] {
                    if (!*cancel)
                        m_statusText->setText(tr("Calculating time... %1/%2 Press ESC to abort").arg(done).arg(n));
                });
            }
        }
        QMetaObject::invokeMethod(this, [this, total, unknown, n = paths.size(), cancel] {
            if (*cancel)
                return;
            m_timeCancel.reset();
            showStatus(QString(), 1);
            QString text = tr("%n item(s)", nullptr, n) + QStringLiteral("\n")
                + tr("Total time: %1 (%2 minutes)").arg(Audio::formatTime(total)).arg(qRound(total / 60));
            if (unknown)
                text += QStringLiteral("\n") + tr("%n file(s) could not be read.", nullptr, unknown);
            QMessageBox::information(this, tr("Playing length"), text);
        });
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void MainWindow::copyToDevice()
{
    const QList<int> rows = m_view->selectedRows();
    QStringList files;
    for (const QString &p : m_model->paths(rows))
        if (QFileInfo::exists(p))
            files << p;
    if (files.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Billy"), tr("Select the files to copy first."));
        return;
    }
    TransferDialog dlg(files, m_settings.devicePath, this);
    dlg.exec();
    m_settings.devicePath = dlg.target();
}

void MainWindow::loveMode()
{
    const QString file = QDir(Settings::configDir()).filePath(QStringLiteral("love.txt"));
    if (!QFileInfo::exists(file)) {
        QMessageBox::information(this, QStringLiteral("Billy"),
                                 tr("To enter Billy's love mode, first make a Barry White playlist.\n"
                                    "Save it with <Ctrl+S> as \"love.txt\" in your Billy configuration folder "
                                    "(Ctrl+Alt+E).\nMake sure \"Dark And Lovely\" is the first song of your list :)"));
        return;
    }
    loadPlaylistFile(file, true);
}

void MainWindow::addToFavorites()
{
    if (m_model->count() == 0) {
        QMessageBox::information(this, QStringLiteral("Billy"), tr("Your playlist is empty, please add some files first."));
        return;
    }
    QString suggestion = m_settings.sourceFolders.isEmpty() ? QString() : QFileInfo(m_settings.sourceFolders.first()).fileName();
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Add playlist to favorites"), tr("Name:"), QLineEdit::Normal,
                                               suggestion, &ok).trimmed();
    if (!ok || name.isEmpty())
        return;
    if (!FavoritesDialog::isValidName(name)) {
        QMessageBox::warning(this, QStringLiteral("Billy"),
                             tr("The title may not have any of the following characters:\n\\ / : ? \" < > |"));
        return;
    }
    const QString file = QDir(Settings::favoritesDir()).filePath(name + QStringLiteral(".txt"));
    if (QFileInfo::exists(file)
        && QMessageBox::question(this, QStringLiteral("Billy"),
                                 tr("A playlist with the same name already exists.\nWould you like to replace the existing file?"))
               != QMessageBox::Yes)
        return;
    if (!PlaylistModel::writePlaylistFile(file, m_model->paths(), m_model->titles()))
        QMessageBox::warning(this, QStringLiteral("Billy"), tr("Your playlist could not be saved."));
}

void MainWindow::organizeFavorites()
{
    FavoritesDialog dlg(this);
    dlg.exec();
}

void MainWindow::rebuildFavoritesMenu()
{
    const QList<QAction *> actions = m_favoritesMenu->actions();
    // Keep "add", "organize" and the separator.
    for (int i = 3; i < actions.size(); ++i)
        m_favoritesMenu->removeAction(actions.at(i));
    const QStringList favs = FavoritesDialog::favorites();
    for (int i = 0; i < favs.size(); ++i) {
        if (i < m_favoriteActions.size()) {
            QAction *a = m_favoriteActions.at(i);
            a->setText(QFileInfo(favs.at(i)).completeBaseName());
            m_favoritesMenu->addAction(a);
        } else {
            const QString file = favs.at(i);
            m_favoritesMenu->addAction(QFileInfo(file).completeBaseName(), this, [this, file] { loadPlaylistFile(file); });
        }
    }
}

void MainWindow::openSettings()
{
    SettingsDialog dlg(m_settings, m_scheme, m_engine->outputDescription(), this);
    connect(&dlg, &SettingsDialog::changed, this, [this, &dlg] {
        const int volume = m_settings.volume;
        m_settings = dlg.settings();
        m_settings.volume = volume;
        m_scheme = dlg.scheme();
        applySettings();
    });
    dlg.exec();
    m_settings.save();
}

void MainWindow::minimizeToTray()
{
    if (m_tray && m_tray->isVisible())
        hide();
    else
        showMinimized();
}

void MainWindow::handleUrlsDropped(const QList<QUrl> &urls, int row)
{
    QStringList files, dirs, streams;
    for (const QUrl &u : urls) {
        if (!u.isLocalFile()) {
            if (PlaylistModel::isUrl(u.toString()))
                streams << u.toString();
            continue;
        }
        const QString p = u.toLocalFile();
        const QFileInfo fi(p);
        if (fi.isDir())
            dirs << p;
        else if (Audio::isSupported(p))
            files << p;
        else if (isPlaylistFile(p))
            files += PlaylistModel::readPlaylistFile(p);
    }
    if (!streams.isEmpty())
        addStreams(streams, {}, false);
    if (!files.isEmpty()) {
        row = m_model->addPaths(files, row) + files.size();
    }
    if (!dirs.isEmpty())
        startScan(dirs, true, QDateTime(), false, row);
}

void MainWindow::openPaths(const QStringList &paths, bool external)
{
    QStringList files, dirs, lists, streams;
    for (const QString &raw : paths) {
        QString p = raw;
        if (PlaylistModel::isUrl(p)) {
            streams << p;
            continue;
        }
        if (p.startsWith(QLatin1String("file://")))
            p = QUrl(p).toLocalFile();
        const QFileInfo fi(p);
        if (fi.isDir())
            dirs << fi.absoluteFilePath();
        else if (isPlaylistFile(p))
            lists << fi.absoluteFilePath();
        else if (fi.exists())
            files << fi.absoluteFilePath();
    }
    if (external)
        mprisRaise();
    if (!streams.isEmpty()) {
        addStreams(streams, {}, true);
        return;
    }
    if (!lists.isEmpty()) {
        loadPlaylistFile(lists.first(), true);
        return;
    }
    if (!dirs.isEmpty() && files.isEmpty()) {
        m_settings.lastOpenedDir = dirs.first();
        startScan(dirs, true, QDateTime(), true, -1, [this] { playId(m_model->idAt(0)); });
        return;
    }
    if (files.isEmpty())
        return;

    switch (m_settings.enqueueOption) {
    case EnqueueOption::EmptyList:
        m_model->clear();
        m_settings.sourceFolders.clear();
        m_model->addPaths(files);
        playId(m_model->idAt(0));
        break;
    case EnqueueOption::AddToList: {
        const int first = m_model->addPaths(files);
        if (m_engine->state() == AudioEngine::Stopped)
            playId(m_model->idAt(first));
        break;
    }
    case EnqueueOption::PlayAndOpenFolder:
    default: {
        // Open the folder of the file (without subfolders) and play the file.
        const QString target = files.first();
        const QString dir = QFileInfo(target).absolutePath();
        m_settings.lastOpenedDir = dir;
        startScan({dir}, false, QDateTime(), true, -1, [this, target, files] {
            for (const QString &f : files.mid(1))
                if (m_model->paths().indexOf(f) < 0)
                    m_model->addPaths({f});
            const QStringList all = m_model->paths();
            int row = all.indexOf(target);
            if (row < 0)
                row = m_model->addPaths({target});
            m_view->selectRows({row});
            playId(m_model->idAt(row));
        });
        break;
    }
    }
}

void MainWindow::restoreSession()
{
    QHash<QString, QString> titles;
    const QStringList paths = PlaylistModel::readPlaylistFile(Settings::playlistFile(), &titles);
    m_model->addPaths(paths);
    m_model->applyTitles(titles);
    const int row = m_settings.resumeRow;
    if (row >= 0 && row < m_model->count()) {
        m_view->selectRows({row});
        if (m_settings.autoResume && m_settings.resumePlaying)
            playId(m_model->idAt(row), m_settings.resumePos);
        else
            m_model->setCurrentId(m_model->idAt(row));
    } else if (m_model->count() > 0) {
        m_view->selectRows({0});
    }
}

// ----------------------------------------------------------- internet radio

void MainWindow::addUrl()
{
    QString suggestion;
    const QString clip = QApplication::clipboard()->text().trimmed();
    if (PlaylistModel::isUrl(clip) && !clip.contains(QLatin1Char('\n')))
        suggestion = clip;
    bool ok = false;
    QString text = QInputDialog::getText(this, tr("Add URL"), tr("Internet radio URL:"), QLineEdit::Normal,
                                         suggestion, &ok).trimmed();
    if (!ok || text.isEmpty())
        return;
    if (!text.contains(QLatin1String("://")))
        text.prepend(QStringLiteral("http://"));
    const QUrl url(text, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty()) {
        QMessageBox::warning(this, tr("Add URL"), tr("Not a valid URL"));
        return;
    }
    addStreams({url.toString()}, {}, true);
}

void MainWindow::addStreams(const QStringList &urls, const QHash<QString, QString> &titles, bool play)
{
    QStringList direct;
    for (const QString &u : urls) {
        const QUrl url(u);
        if (isRemotePlaylist(url) && (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https")))
            resolveRemotePlaylist(url, play && direct.isEmpty());
        else
            direct << u;
    }
    if (direct.isEmpty())
        return;
    const QModelIndex cur = m_view->currentIndex();
    const int row = m_model->addPaths(direct, cur.isValid() ? cur.row() + 1 : -1);
    m_model->applyTitles(titles);
    m_view->selectRows({row});
    if (play)
        playId(m_model->idAt(row));
}

void MainWindow::resolveRemotePlaylist(const QUrl &url, bool play)
{
    if (!m_net)
        m_net = new QNetworkAccessManager(this);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Billy/" BILLY_VERSION));
    request.setTransferTimeout(15000);
    QNetworkReply *reply = m_net->get(request);
    showStatus(tr("Connecting %1").arg(url.host()), 15000);
    connect(reply, &QNetworkReply::finished, this, [this, reply, url, play] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            showStatus(tr("Could not connect to URL.") + QStringLiteral(" ") + reply->errorString(), 10000);
            return;
        }
        const QString text = QString::fromUtf8(reply->read(1 << 20));
        QHash<QString, QString> titles;
        QStringList streams = PlaylistModel::parsePlaylist(text, QString(), url.path().toLower().endsWith(QLatin1String(".pls")), &titles);
        streams.erase(std::remove_if(streams.begin(), streams.end(), [](const QString &s) { return !PlaylistModel::isUrl(s); }),
                      streams.end());
        if (streams.isEmpty()) {
            showStatus(tr("Error opening URL."), 10000);
            return;
        }
        // Stations list mirrors of the same stream; the first one is enough.
        QHash<QString, QString> first;
        if (titles.contains(streams.first()))
            first.insert(streams.first(), titles.value(streams.first()));
        addStreams({streams.first()}, first, play);
    });
}

void MainWindow::onStreamMetadata(quint64 id, const QString &name, const QString &title)
{
    if (!name.isEmpty() && m_model->titleOf(id).isEmpty())
        m_model->setTitle(id, name);
    m_streamTitle = title;
    if (!title.isEmpty() && !m_wasBuffering)
        showStatus(title, 600000);
    updateTitle();
    updateSkinInfo();
    if (m_mpris)
        m_mpris->notifyChanged();
}

void MainWindow::showStreamInfo(int row)
{
    const quint64 id = m_model->idAt(row);
    QStringList lines;
    const QString name = m_model->titleOf(id);
    if (!name.isEmpty())
        lines << tr("Station: %1").arg(name);
    lines << tr("URL: %1").arg(m_model->pathAt(row));
    if (id == m_engine->currentId() && m_engine->state() != AudioEngine::Stopped) {
        if (!m_streamTitle.isEmpty())
            lines << tr("Playing: %1").arg(m_streamTitle);
        if (!m_streamCodec.isEmpty() || m_streamKbps > 0)
            lines << tr("Format: %1 %2 kbps").arg(m_streamCodec).arg(m_streamKbps);
        lines << tr("Listening for: %1").arg(Audio::formatTime(m_engine->position()));
    }
    QMessageBox box(QMessageBox::Information, tr("Internet radio"), lines.join(QLatin1Char('\n')),
                    QMessageBox::Ok, this);
    QPushButton *rename = box.addButton(tr("&Rename..."), QMessageBox::ActionRole);
    box.setEscapeButton(QMessageBox::Ok);
    box.exec();
    if (box.clickedButton() == rename) {
        bool ok = false;
        const QString t = QInputDialog::getText(this, tr("Rename"), tr("Station name:"), QLineEdit::Normal,
                                                name, &ok).trimmed();
        if (ok)
            m_model->setTitle(id, t);
    }
}

// --------------------------------------------------------- alternative look

namespace {
QString skinMenuStyle()
{
    return QStringLiteral(
        "QMenu { background: #1b1d20; color: #e6e6e6; border: 1px solid #5c5c5c; padding: 3px; }"
        "QMenu::item { padding: 4px 22px 4px 22px; }"
        "QMenu::item:selected { background: rgba(120, 220, 190, 70); color: #ffffff; }"
        "QMenu::item:disabled { color: #777777; }"
        "QMenu::separator { height: 1px; background: #444444; margin: 3px 8px; }"
        "QMenu::indicator { width: 12px; height: 12px; left: 5px; }");
}
} // namespace

void MainWindow::createSkin()
{
    m_skin = new SkinView(m_stack);
    m_stack->addWidget(m_skin);
    m_skinDelegate = new SkinPlaylistDelegate(m_model, this);
    m_spectrum = std::make_unique<Spectrum>(2048);
    m_scopeBuffer.resize(size_t(m_spectrum->fftSize()));
    m_skin->setVolume(m_settings.volume);
    m_skin->setPage(m_settings.skinPage == 1 ? SkinView::PlaylistPage : SkinView::InfoPage);

    connect(m_skin, &SkinView::previousClicked, this, &MainWindow::previousTrack);
    connect(m_skin, &SkinView::playPauseClicked, this, &MainWindow::togglePause);
    connect(m_skin, &SkinView::nextClicked, this, &MainWindow::nextTrack);
    connect(m_skin, &SkinView::stopClicked, this, &MainWindow::stopPlayback);
    connect(m_skin, &SkinView::ejectClicked, this, [this] { openFolderDialog(false); });
    connect(m_skin, &SkinView::closeClicked, this, &MainWindow::close);
    connect(m_skin, &SkinView::minimizeClicked, this, &MainWindow::showMinimized);
    connect(m_skin, &SkinView::menuRequested, this, &MainWindow::showSkinMenu);
    connect(m_skin, &SkinView::seekRequested, this, [this](double f) {
        const double d = m_engine->duration();
        if (d > 0)
            seekTo(f * d);
    });
    connect(m_skin, &SkinView::volumeRequested, this, [this](int v) {
        m_volumeBeforeSoftmute = -1;
        setVolume(v);
    });
    connect(m_skin, &SkinView::pageChanged, this, [this](SkinView::Page page) {
        m_settings.skinPage = page == SkinView::PlaylistPage ? 1 : 0;
        if (page == SkinView::PlaylistPage)
            jumpToPlaying();
    });
}

void MainWindow::applyLook(bool alternative, bool initial)
{
    if (!initial && alternative == m_skinActive)
        return;
    if (!initial) {
        // Each look keeps its own window size.
        if (m_skinActive)
            m_settings.skinGeometry = saveGeometry();
        else
            m_settings.geometry = saveGeometry();
    }
    const bool wasVisible = isVisible();
    m_skinActive = alternative;
    m_settings.alternativeLook = alternative;
    m_lookAction->setChecked(alternative);

    if (alternative) {
        // A see-through playlist on the dark glass pane of the skin.
        Scheme glass = Scheme::systemScheme();
        glass.systemDefault = false;
        glass.background = Qt::transparent;
        glass.font = QColor(240, 240, 240);
        glass.fontSelected = Qt::white;
        glass.selection = QColor(255, 255, 255, 45);
        glass.selectionPlaying = QColor(255, 255, 255, 65);
        glass.playing = QColor(0xf3, 0xe7, 0xa1);
        glass.playingSelected = QColor(0xf3, 0xe7, 0xa1);
        glass.queued = QColor(0x9c, 0xff, 0x9c);
        glass.shuffled = QColor(255, 255, 255, 110);
        glass.radio = QColor(0x9c, 0xd8, 0xff);
        glass.fontName = m_scheme.fontName;
        glass.fontSize = m_scheme.fontSize;
        m_view->setScheme(glass);
        m_view->setFrameShape(QFrame::NoFrame);
        m_view->viewport()->setAutoFillBackground(false);
        // Only the scroll bar gets a style sheet; one on the view would stick after switching back.
        m_view->verticalScrollBar()->setStyleSheet(QStringLiteral(
            "QScrollBar:vertical { background: transparent; width: 8px; margin: 0; }"
            "QScrollBar::handle:vertical { background: rgba(255,255,255,70); border-radius: 3px; min-height: 24px; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
            "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }"));
        m_view->setItemDelegate(m_skinDelegate);
        m_view->setUniformItemSizes(false); // album headers make some rows taller
        m_view->setOverlay([this](QPainter &p) { m_skinDelegate->paintCovers(p, m_view); });
        m_skin->setPlaylistWidget(m_view);
        m_stack->setCurrentWidget(m_skin);
        for (QAction *a : menuBar()->actions())
            if (a->menu())
                a->menu()->setStyleSheet(skinMenuStyle());
        menuBar()->hide();
        statusBar()->hide();
        setWindowFlag(Qt::FramelessWindowHint, true);
        if (!m_settings.skinGeometry.isEmpty())
            restoreGeometry(m_settings.skinGeometry);
        else
            resize(700, 300); // the size of the original skin
        updateSkinInfo();
    } else {
        m_skin->setPlaylistWidget(nullptr);
        m_view->setOverlay({});
        m_view->setItemDelegate(m_view->classicDelegate());
        m_view->setUniformItemSizes(true);
        for (QAction *a : menuBar()->actions())
            if (a->menu())
                a->menu()->setStyleSheet(QString());
        m_view->verticalScrollBar()->setStyleSheet(QString());
        m_view->setFrameShape(QFrame::StyledPanel);
        m_view->viewport()->setAutoFillBackground(true);
        m_view->setScheme(m_scheme);
        m_classicLayout->insertWidget(1, m_view, 1);
        m_view->show();
        m_stack->setCurrentWidget(m_classicPage);
        menuBar()->show();
        statusBar()->show();
        setWindowFlag(Qt::FramelessWindowHint, false);
        if (!m_settings.geometry.isEmpty())
            restoreGeometry(m_settings.geometry);
        else
            resize(394, 289);
    }
    if (wasVisible)
        show(); // changing the window flags hides the window
}

void MainWindow::recordPlay(quint64 id)
{
    const QString path = m_model->pathOf(id);
    m_statsLine.clear();
    m_lastPlayedLine.clear();
    if (!path.isEmpty() && !PlaylistModel::isUrl(path)) {
        const PlayStats::Entry before = PlayStats::record(path);
        const int count = before.count + 1;
        m_statsLine = count == 1 ? tr("Track played for the first time") : tr("Track played %1 times").arg(count);
        if (before.last.isValid())
            m_lastPlayedLine = tr("Last played %1").arg(before.last.toString(QStringLiteral("yyyy-MM-dd hh:mm:ss")));
    }
    updateSkinInfo();
}

void MainWindow::updateSkinInfo()
{
    if (!m_skinActive)
        return;
    SkinView::TrackInfo info;
    const quint64 id = m_model->currentId();
    const QString path = m_model->pathOf(id);
    if (path.isEmpty()) {
        info.album = QStringLiteral("Billy");
        info.artist = tr("No nonsense, gapless audio player");
        info.details << tr("Open a folder with music files: F4 or the eject button.")
                     << tr("Internet radio: Ctrl+R")
                     << tr("Menu: the alien in the corner, or a right click.");
    } else if (PlaylistModel::isUrl(path)) {
        info.album = displayName(id);
        const int dash = m_streamTitle.indexOf(QLatin1String(" - "));
        if (dash > 0) {
            info.artist = m_streamTitle.left(dash);
            info.title = m_streamTitle.mid(dash + 3);
        } else {
            info.title = m_streamTitle;
        }
        info.details << tr("Internet radio");
        if (m_streamKbps > 0 || !m_streamCodec.isEmpty())
            info.details << tr("%1 kbps %2").arg(m_streamKbps).arg(m_streamCodec);
        info.details << path;
    } else {
        // Tags if there are any, otherwise the folder names (Artist/Album/Track).
        const AudioInfo audio = Audio::probe(path, true);
        const QFileInfo fi(path);
        info.title = audio.tagMap.value(QStringLiteral("title"), fi.completeBaseName());
        info.album = audio.tagMap.value(QStringLiteral("album"), fi.dir().dirName());
        QString artist = audio.tagMap.value(QStringLiteral("artist"));
        if (artist.isEmpty()) {
            QDir up = fi.dir();
            if (up.cdUp())
                artist = up.dirName();
        }
        info.artist = artist;
        if (!m_statsLine.isEmpty())
            info.details << m_statsLine;
        if (!m_lastPlayedLine.isEmpty())
            info.details << m_lastPlayedLine;
        QString format;
        if (audio.bitrate > 0)
            format = tr("%1 kbps ").arg(audio.bitrate);
        format += audio.type;
        if (audio.type == QLatin1String("MP3"))
            format += audio.vbr ? QStringLiteral(" VBR") : QStringLiteral(" CBR");
        format += QStringLiteral(" | %1 MB").arg(fi.size() / 1048576.0, 0, 'f', 2);
        info.details << QString() << format;
        info.cover = Audio::coverArt(path);
    }
    // An empty line in the details separates the statistics from the format.
    if (!info.details.isEmpty() && info.details.first().isEmpty())
        info.details.removeFirst();
    m_skin->setTrackInfo(info);
}

void MainWindow::updateSkinTick()
{
    const double pos = m_engine->position();
    const double dur = m_engine->duration();
    QString time = Audio::formatTime(pos);
    if (dur > 0)
        time += QStringLiteral(" / ") + Audio::formatTime(dur);
    m_skin->setProgress(dur > 0 ? pos / dur : -1, time);
    // Remaining time next to the playing item in the playlist.
    m_skinDelegate->setRemaining(dur > 0 ? QStringLiteral("-") + Audio::formatTime(dur - pos) : Audio::formatTime(pos));
    m_skinDelegate->setPaused(m_engine->state() == AudioEngine::Paused);
    if (m_skin->page() == SkinView::PlaylistPage) {
        const int row = m_model->currentRow();
        if (row >= 0)
            m_view->viewport()->update(m_view->visualRect(m_model->index(row)));
    }

    if (m_engine->state() != AudioEngine::Playing)
        return;
    const int rate = m_engine->recentSamples(m_scopeBuffer.data(), int(m_scopeBuffer.size()));
    if (rate > 0)
        m_skin->setSpectrum(m_spectrum->analyse(m_scopeBuffer.data(), rate, 64));
}

void MainWindow::showSkinMenu(const QPoint &globalPos, bool anchored)
{
    // The menu bar of the classic look, as one popup.
    QMenu menu(this);
    menu.setStyleSheet(skinMenuStyle());
    for (QAction *a : menuBar()->actions())
        menu.addAction(a);
    QPoint pos = globalPos;
    if (anchored) // open upwards, ending at the alien
        pos -= QPoint(menu.sizeHint().width(), menu.sizeHint().height());
    menu.exec(pos);
}

// ------------------------------------------------------------- MPRIS API

AudioEngine::State MainWindow::mprisState() const { return m_engine->state(); }
QString MainWindow::mprisPath() const { return m_engine->state() == AudioEngine::Stopped ? QString() : m_model->pathOf(m_model->currentId()); }
QString MainWindow::mprisTitle() const
{
    return m_streamTitle.isEmpty() ? displayName(m_model->currentId()) : m_streamTitle;
}
quint64 MainWindow::mprisTrackId() const { return m_model->currentId(); }
double MainWindow::mprisLength() const { return m_engine->duration(); }
double MainWindow::mprisPosition() const { return m_engine->position(); }

void MainWindow::mprisPlay()
{
    if (m_engine->state() == AudioEngine::Paused)
        m_engine->resume();
    else if (m_engine->state() == AudioEngine::Stopped) {
        const quint64 id = m_model->currentId();
        if (id && m_model->rowOf(id) >= 0)
            playId(id);
        else
            playSelected();
    }
}

void MainWindow::mprisPause()
{
    m_engine->pause();
}

void MainWindow::mprisRaise()
{
    show();
    if (isMinimized())
        showNormal();
    raise();
    activateWindow();
}

// ------------------------------------------------------------------ events

bool MainWindow::handleKeypad(QKeyEvent *ke)
{
    const bool keypad = ke->modifiers() & Qt::KeypadModifier;
    const Qt::KeyboardModifiers mods = ke->modifiers() & ~(Qt::KeypadModifier | Qt::ShiftModifier);
    if (mods != Qt::NoModifier)
        return false;
    const int key = ke->key();

    // Volume works with the normal + and - keys too.
    if (key == Qt::Key_Plus || key == Qt::Key_Minus) {
        setVolume(m_settings.volume + (key == Qt::Key_Plus ? 5 : -5));
        m_volumeBeforeSoftmute = -1;
        return true;
    }
    if (!keypad)
        return false;
    switch (key) {
    case Qt::Key_0:
    case Qt::Key_Insert:
        // Pause; a second press (while paused) stops and releases the device.
        if (m_engine->state() == AudioEngine::Playing)
            m_engine->pause();
        else if (m_engine->state() == AudioEngine::Paused)
            stopPlayback();
        else
            mprisPlay();
        return true;
    case Qt::Key_1:
    case Qt::Key_End:
        seekBy(-4);
        return true;
    case Qt::Key_3:
    case Qt::Key_PageDown:
        seekBy(4);
        return true;
    case Qt::Key_4:
    case Qt::Key_Left:
        previousTrack();
        return true;
    case Qt::Key_6:
    case Qt::Key_Right:
        nextTrack();
        return true;
    case Qt::Key_8:
    case Qt::Key_Up:
        setPlayMode(PlayMode((int(m_settings.playMode) + 1) % 5));
        return true;
    case Qt::Key_5:
    case Qt::Key_Clear:
    case Qt::Key_2:
    case Qt::Key_Down: {
        const int delta = (key == Qt::Key_2 || key == Qt::Key_Down) ? 1 : -1;
        const int row = qBound(0, m_view->currentIndex().row() + delta, m_model->count() - 1);
        if (m_model->count() > 0)
            m_view->selectRows({row});
        return true;
    }
    case Qt::Key_9:
    case Qt::Key_PageUp:
        queueSelected();
        return true;
    case Qt::Key_7:
    case Qt::Key_Home:
        loveMode();
        return true;
    case Qt::Key_Period:
    case Qt::Key_Comma:
    case Qt::Key_Delete:
        removeSelected();
        return true;
    case Qt::Key_Asterisk:
        openFolderDialog(false);
        return true;
    case Qt::Key_Slash:
        openFolderDialog(true);
        return true;
    case Qt::Key_Enter:
        playSelected();
        return true;
    default:
        return false;
    }
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // Keys typed into the playlist while the finder is still opening belong to the finder.
    if (m_findDialog && (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)) {
        auto *w = qobject_cast<QWidget *>(watched);
        auto *ke = static_cast<QKeyEvent *>(event);
        if (w && w->window() == this && !ke->text().isEmpty() && ke->text().at(0).isPrint()
            && (ke->modifiers() & ~(Qt::ShiftModifier | Qt::KeypadModifier)) == Qt::NoModifier) {
            event->accept();
            if (event->type() == QEvent::KeyPress) {
                m_findDialog->appendText(ke->text());
                m_findDialog->activateWindow();
            }
            return true;
        }
    }

    // Numpad keys and volume keys, while the main window has focus.
    if ((event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)
        && QApplication::activeWindow() == this && !qobject_cast<QLineEdit *>(QApplication::focusWidget())
        && watched == QApplication::focusWidget()) {
        auto *ke = static_cast<QKeyEvent *>(event);
        const bool keypad = ke->modifiers() & Qt::KeypadModifier;
        const int key = ke->key();
        const bool ours = keypad || key == Qt::Key_Plus || key == Qt::Key_Minus;
        if (ours && (ke->modifiers() & ~(Qt::KeypadModifier | Qt::ShiftModifier)) == Qt::NoModifier) {
            if (event->type() == QEvent::ShortcutOverride) {
                // Claim the key so that no shortcut eats it; handled on KeyPress.
                event->accept();
                return true;
            }
            if (handleKeypad(ke))
                return true;
        }
    }

    if (watched == m_topPanel && event->type() == QEvent::Resize)
        layoutTopPanel();

    // Click on the play mode panel cycles the play mode (right click: backwards).
    if (event->type() == QEvent::MouseButtonPress && watched == m_statusMode) {
        auto *me = static_cast<QMouseEvent *>(event);
        const int dir = me->button() == Qt::RightButton ? 4 : 1;
        setPlayMode(PlayMode((int(m_settings.playMode) + dir) % 5));
        return true;
    }

    // Dragging the volume label (or the level meters) up and down.
    if (watched == m_volumeLabel || watched == m_scale14 || watched == m_meter6) {
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton) {
                m_volumeDragging = true;
                m_volumeDragStartY = int(me->globalPosition().y());
                m_volumeDragStartValue = m_settings.volume;
            }
            return true;
        }
        case QEvent::MouseMove:
            if (m_volumeDragging) {
                auto *me = static_cast<QMouseEvent *>(event);
                m_volumeBeforeSoftmute = -1;
                setVolume(m_volumeDragStartValue + (m_volumeDragStartY - int(me->globalPosition().y())));
                if (watched != m_volumeLabel)
                    showStatus(tr("Volume %1 %").arg(m_settings.volume), 1500);
            }
            return true;
        case QEvent::MouseButtonRelease:
            m_volumeDragging = false;
            return true;
        case QEvent::Wheel: {
            auto *we = static_cast<QWheelEvent *>(event);
            m_volumeBeforeSoftmute = -1;
            setVolume(m_settings.volume + (we->angleDelta().y() > 0 ? 2 : -2));
            return true;
        }
        default:
            break;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::changeEvent(QEvent *e)
{
    QMainWindow::changeEvent(e);
    if (e->type() == QEvent::PaletteChange && m_scheme.systemDefault) {
        m_scheme = Scheme::systemScheme();
        m_view->setScheme(m_scheme);
    }
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    if (m_skinActive) {
        m_settings.skinGeometry = saveGeometry();
    } else {
        m_settings.geometry = saveGeometry();
        m_settings.maximized = isMaximized();
    }
    const int row = m_model->currentRow();
    m_settings.resumeRow = row >= 0 ? row : m_view->currentIndex().row();
    m_settings.resumePos = m_engine->state() == AudioEngine::Stopped ? 0 : m_engine->position();
    m_settings.resumePlaying = m_engine->state() == AudioEngine::Playing;
    if (m_volumeBeforeSoftmute >= 0)
        m_settings.volume = m_volumeBeforeSoftmute;
    m_settings.save();
    // A read-only Playlist.txt means "always start with this list".
    const QFileInfo playlist(Settings::playlistFile());
    if (!playlist.exists() || playlist.isWritable())
        PlaylistModel::writePlaylistFile(Settings::playlistFile(), m_model->paths(), m_model->titles());
    m_engine->stop();
    if (m_tray)
        m_tray->hide();
    e->accept();
    qApp->quit();
}
