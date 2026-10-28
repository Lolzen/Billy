#pragma once

#include <QDateTime>
#include <QString>

// How often and when each file was played ("Track played 8 times").
// Stored in PlayHistory.ini in the configuration folder.
namespace PlayStats {

struct Entry
{
    int count = 0;
    QDateTime last; // invalid: never
};

Entry get(const QString &path);
// Counts a play and returns the entry as it was before.
Entry record(const QString &path);

} // namespace PlayStats
