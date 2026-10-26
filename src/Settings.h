#pragma once

#include <QColor>
#include <QFont>
#include <QString>
#include <QStringList>

// Playlist colour scheme ("*.billyscheme" files in the Schemes folder).
struct Scheme
{
    QString name;
    bool systemDefault = true;
    QColor background;
    QColor font;
    QColor fontSelected;
    QColor selection;
    QColor selectionPlaying;
    QColor playing;
    QColor playingSelected;
    bool playingBold = true;
    QColor queued;
    QColor shuffled;
    QString fontName;
    int fontSize = 0; // 0: application default

    static Scheme systemScheme();
    static Scheme load(const QString &file);
    bool save(const QString &file) const;
    QFont listFont() const;

    // Index based access, matches Settings::schemeItemNames().
    QColor color(int item) const;
    void setColor(int item, const QColor &c);
};

enum class PlayMode { RepeatList = 0, RepeatFile, PlayList, PlayFile, Shuffle };
enum class ViewFormat { FileName = 0, FullPath, RelativePath };
enum class SortWay { FileName = 0, PathAndFileName };
enum class VuStyle { None = 0, Led6, Led14 };
enum class EnqueueOption { EmptyList = 1, AddToList = 2, PlayAndOpenFolder = 3 };

struct Settings
{
    PlayMode playMode = PlayMode::RepeatList;
    ViewFormat viewFormat = ViewFormat::FileName;
    SortWay sortWay = SortWay::PathAndFileName;
    VuStyle vuStyle = VuStyle::Led6;
    EnqueueOption enqueueOption = EnqueueOption::PlayAndOpenFolder;
    QStringList musicFolders;
    QString lastOpenedDir;
    QStringList sourceFolders;   // folders that make up the current list (for F5)
    QString playlistFolder;
    QString devicePath;          // "copy to MP3 player" target
    QString dirDialogRoot;
    QString relativeDelimiter = QStringLiteral(" / ");
    QString activeScheme;
    QByteArray outputDevice;
    int bufferLength = 3000;
    int volume = 100;
    int softmutePercent = 30;
    bool multipleInstances = false;
    bool autoResume = false;
    bool minimizeToTray = true;
    bool showPlayBar = true;
    bool showProgressPanel = true;
    bool includeSubfolders = true;
    QByteArray geometry;
    bool maximized = false;
    // Resume state.
    int resumeRow = -1;
    double resumePos = 0;
    bool resumePlaying = false;

    void load();
    void save() const;

    static QString configDir();
    static QString iniFile();
    static QString playlistFile();
    static QString favoritesDir();
    static QString schemesDir();
    static QStringList schemeItemNames();
    static QString playModeName(PlayMode mode);
};
