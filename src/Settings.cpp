#include "Settings.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QPalette>
#include <QSettings>
#include <QStandardPaths>

namespace {

QColor readColor(QSettings &s, const char *key, const QColor &fallback)
{
    const QString v = s.value(QLatin1String(key)).toString();
    const QColor c(v);
    return c.isValid() ? c : fallback;
}

} // namespace

Scheme Scheme::systemScheme()
{
    const QPalette p = QApplication::palette();
    Scheme s;
    s.name = QObject::tr("System default");
    s.systemDefault = true;
    s.background = p.color(QPalette::Base);
    s.font = p.color(QPalette::Text);
    s.fontSelected = p.color(QPalette::HighlightedText);
    s.selection = p.color(QPalette::Highlight);
    s.selectionPlaying = p.color(QPalette::Highlight).darker(115);
    s.playing = p.color(QPalette::Text);
    s.playingSelected = p.color(QPalette::HighlightedText);
    s.playingBold = true;
    // Billy's classic accents for queued and already shuffled items.
    s.queued = QColor(0x00, 0x80, 0x00);
    s.shuffled = p.color(QPalette::Disabled, QPalette::Text);
    s.radio = p.color(QPalette::Link);
    return s;
}

Scheme Scheme::load(const QString &file)
{
    Scheme s = systemScheme();
    QSettings ini(file, QSettings::IniFormat);
    ini.beginGroup(QStringLiteral("Scheme"));
    s.name = QFileInfo(file).completeBaseName();
    s.systemDefault = false;
    s.background = readColor(ini, "background_color", s.background);
    s.font = readColor(ini, "font_color", s.font);
    s.fontSelected = readColor(ini, "font_color_selected", s.fontSelected);
    s.selection = readColor(ini, "selection_color", s.selection);
    s.selectionPlaying = readColor(ini, "selection_color_playingitem", s.selectionPlaying);
    s.playing = readColor(ini, "playingitem_color", s.playing);
    s.playingSelected = readColor(ini, "playingitem_color_selected", s.playingSelected);
    s.playingBold = ini.value(QStringLiteral("playingitem_bold"), true).toBool();
    s.queued = readColor(ini, "queueditem_color", s.queued);
    s.shuffled = readColor(ini, "shuffleditem_color", s.shuffled);
    s.radio = readColor(ini, "urlitem_color", s.radio);
    s.fontName = ini.value(QStringLiteral("font_name")).toString();
    s.fontSize = ini.value(QStringLiteral("font_size"), 0).toInt();
    return s;
}

bool Scheme::save(const QString &file) const
{
    QSettings ini(file, QSettings::IniFormat);
    ini.beginGroup(QStringLiteral("Scheme"));
    ini.setValue(QStringLiteral("background_color"), background.name());
    ini.setValue(QStringLiteral("font_color"), font.name());
    ini.setValue(QStringLiteral("font_color_selected"), fontSelected.name());
    ini.setValue(QStringLiteral("selection_color"), selection.name());
    ini.setValue(QStringLiteral("selection_color_playingitem"), selectionPlaying.name());
    ini.setValue(QStringLiteral("playingitem_color"), playing.name());
    ini.setValue(QStringLiteral("playingitem_color_selected"), playingSelected.name());
    ini.setValue(QStringLiteral("playingitem_bold"), playingBold);
    ini.setValue(QStringLiteral("queueditem_color"), queued.name());
    ini.setValue(QStringLiteral("shuffleditem_color"), shuffled.name());
    ini.setValue(QStringLiteral("urlitem_color"), radio.name());
    ini.setValue(QStringLiteral("font_name"), fontName);
    ini.setValue(QStringLiteral("font_size"), fontSize);
    ini.endGroup();
    ini.sync();
    return ini.status() == QSettings::NoError;
}

QFont Scheme::listFont() const
{
    QFont f = QApplication::font();
    if (!fontName.isEmpty())
        f.setFamily(fontName);
    if (fontSize > 0)
        f.setPointSize(fontSize);
    return f;
}

QColor Scheme::color(int item) const
{
    switch (item) {
    case 0: return background;
    case 1: return font;
    case 2: return fontSelected;
    case 3: return selection;
    case 4: return selectionPlaying;
    case 5: return playing;
    case 6: return playingSelected;
    case 8: return queued;
    case 9: return shuffled;
    case 10: return radio;
    default: return {};
    }
}

void Scheme::setColor(int item, const QColor &c)
{
    switch (item) {
    case 0: background = c; break;
    case 1: font = c; break;
    case 2: fontSelected = c; break;
    case 3: selection = c; break;
    case 4: selectionPlaying = c; break;
    case 5: playing = c; break;
    case 6: playingSelected = c; break;
    case 8: queued = c; break;
    case 9: shuffled = c; break;
    case 10: radio = c; break;
    default: break;
    }
    systemDefault = false;
}

QStringList Settings::schemeItemNames()
{
    return {
        QObject::tr("Background color"),
        QObject::tr("Font color"),
        QObject::tr("Font color selected"),
        QObject::tr("Selection color"),
        QObject::tr("Selection color playing item"),
        QObject::tr("Playing item color"),
        QObject::tr("Playing item color selected"),
        QObject::tr("Playing item bold"),
        QObject::tr("Queued item color"),
        QObject::tr("Shuffled item color"),
        QObject::tr("Radio item color"),
    };
}

QString Settings::playModeName(PlayMode mode)
{
    switch (mode) {
    case PlayMode::RepeatList: return QObject::tr("Repeat");
    case PlayMode::RepeatFile: return QObject::tr("Repeat file");
    case PlayMode::PlayList: return QObject::tr("Play list");
    case PlayMode::PlayFile: return QObject::tr("Play file");
    case PlayMode::Shuffle: return QObject::tr("Shuffle");
    }
    return {};
}

QString Settings::configDir()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/Billy");
    QDir().mkpath(dir);
    return dir;
}

QString Settings::iniFile() { return configDir() + QStringLiteral("/Billy.ini"); }
QString Settings::playlistFile() { return configDir() + QStringLiteral("/Playlist.txt"); }

QString Settings::favoritesDir()
{
    const QString dir = configDir() + QStringLiteral("/Favorites");
    QDir().mkpath(dir);
    return dir;
}

QString Settings::schemesDir()
{
    const QString dir = configDir() + QStringLiteral("/Schemes");
    QDir().mkpath(dir);
    return dir;
}

void Settings::load()
{
    QSettings s(iniFile(), QSettings::IniFormat);
    // Qt writes keys without a group into the [General] section, like Billy.ini on Windows.
    playMode = PlayMode(qBound(0, s.value(QStringLiteral("PlayMode"), 0).toInt(), 4));
    viewFormat = ViewFormat(qBound(0, s.value(QStringLiteral("ViewFormat"), 0).toInt(), 2));
    sortWay = SortWay(qBound(0, s.value(QStringLiteral("SortingWay"), 1).toInt(), 1));
    vuStyle = VuStyle(qBound(0, s.value(QStringLiteral("VUStyle"), 1).toInt(), 2));
    enqueueOption = EnqueueOption(qBound(1, s.value(QStringLiteral("EnqueueOption"), 3).toInt(), 3));
    musicFolders = s.value(QStringLiteral("RootMedia")).toStringList();
    lastOpenedDir = s.value(QStringLiteral("LastOpenedDir")).toString();
    sourceFolders = s.value(QStringLiteral("SourceFolders")).toStringList();
    playlistFolder = s.value(QStringLiteral("PlaylistFolder")).toString();
    devicePath = s.value(QStringLiteral("DevicePath")).toString();
    dirDialogRoot = s.value(QStringLiteral("DirDialogRoot")).toString();
    relativeDelimiter = s.value(QStringLiteral("RelativeDelimiter"), relativeDelimiter).toString();
    activeScheme = s.value(QStringLiteral("ActiveScheme")).toString();
    outputDevice = s.value(QStringLiteral("OutputDevice")).toByteArray();
    bufferLength = s.value(QStringLiteral("BufferLength"), 3000).toInt();
    volume = qBound(0, s.value(QStringLiteral("Volume"), 100).toInt(), 100);
    softmutePercent = qBound(0, s.value(QStringLiteral("softmute_percent"), 30).toInt(), 100);
    multipleInstances = s.value(QStringLiteral("MultipleInstances"), false).toBool();
    minimizeToTray = s.value(QStringLiteral("minimize_to_tray"), true).toBool();
    showPlayBar = s.value(QStringLiteral("PlayBar"), true).toBool();
    showProgressPanel = s.value(QStringLiteral("ProgressPanel"), true).toBool();
    includeSubfolders = s.value(QStringLiteral("IncludeSubfolders"), true).toBool();

    s.beginGroup(QStringLiteral("Window"));
    geometry = s.value(QStringLiteral("Geometry")).toByteArray();
    maximized = s.value(QStringLiteral("Maximized"), false).toBool();
    s.endGroup();

    s.beginGroup(QStringLiteral("Playlist"));
    autoResume = s.value(QStringLiteral("AutoResume"), false).toBool();
    resumeRow = s.value(QStringLiteral("ResumeItem"), -1).toInt();
    resumePos = s.value(QStringLiteral("ResumePos"), 0).toDouble();
    resumePlaying = s.value(QStringLiteral("ResumeState"), false).toBool();
    s.endGroup();
}

void Settings::save() const
{
    QSettings s(iniFile(), QSettings::IniFormat);
    s.setValue(QStringLiteral("PlayMode"), int(playMode));
    s.setValue(QStringLiteral("ViewFormat"), int(viewFormat));
    s.setValue(QStringLiteral("SortingWay"), int(sortWay));
    s.setValue(QStringLiteral("VUStyle"), int(vuStyle));
    s.setValue(QStringLiteral("EnqueueOption"), int(enqueueOption));
    s.setValue(QStringLiteral("RootMedia"), musicFolders);
    s.setValue(QStringLiteral("LastOpenedDir"), lastOpenedDir);
    s.setValue(QStringLiteral("SourceFolders"), sourceFolders);
    s.setValue(QStringLiteral("PlaylistFolder"), playlistFolder);
    s.setValue(QStringLiteral("DevicePath"), devicePath);
    s.setValue(QStringLiteral("DirDialogRoot"), dirDialogRoot);
    s.setValue(QStringLiteral("RelativeDelimiter"), relativeDelimiter);
    s.setValue(QStringLiteral("ActiveScheme"), activeScheme);
    s.setValue(QStringLiteral("OutputDevice"), outputDevice);
    s.setValue(QStringLiteral("BufferLength"), bufferLength);
    s.setValue(QStringLiteral("Volume"), volume);
    s.setValue(QStringLiteral("softmute_percent"), softmutePercent);
    s.setValue(QStringLiteral("MultipleInstances"), multipleInstances);
    s.setValue(QStringLiteral("minimize_to_tray"), minimizeToTray);
    s.setValue(QStringLiteral("PlayBar"), showPlayBar);
    s.setValue(QStringLiteral("ProgressPanel"), showProgressPanel);
    s.setValue(QStringLiteral("IncludeSubfolders"), includeSubfolders);

    s.beginGroup(QStringLiteral("Window"));
    s.setValue(QStringLiteral("Geometry"), geometry);
    s.setValue(QStringLiteral("Maximized"), maximized);
    s.endGroup();

    s.beginGroup(QStringLiteral("Playlist"));
    s.setValue(QStringLiteral("AutoResume"), autoResume);
    s.setValue(QStringLiteral("ResumeItem"), resumeRow);
    s.setValue(QStringLiteral("ResumePos"), resumePos);
    s.setValue(QStringLiteral("ResumeState"), resumePlaying);
    s.endGroup();
}
