#include "PlayStats.h"
#include "Settings.h"

#include <QCryptographicHash>
#include <QSettings>

namespace PlayStats {

namespace {

QString key(const QString &path)
{
    return QString::fromLatin1(QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Md5).toHex());
}

QSettings &store()
{
    static QSettings settings(Settings::configDir() + QStringLiteral("/PlayHistory.ini"), QSettings::IniFormat);
    return settings;
}

} // namespace

Entry get(const QString &path)
{
    QSettings &s = store();
    const QString k = key(path);
    Entry e;
    e.count = s.value(k + QStringLiteral("/count"), 0).toInt();
    e.last = s.value(k + QStringLiteral("/last")).toDateTime();
    return e;
}

Entry record(const QString &path)
{
    const Entry before = get(path);
    QSettings &s = store();
    const QString k = key(path);
    s.setValue(k + QStringLiteral("/count"), before.count + 1);
    s.setValue(k + QStringLiteral("/last"), QDateTime::currentDateTime());
    s.setValue(k + QStringLiteral("/path"), path);
    return before;
}

} // namespace PlayStats
