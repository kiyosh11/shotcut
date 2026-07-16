/*
 * Copyright (c) 2026 Meltytech, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef LOCALPATH_H
#define LOCALPATH_H

#include <QDir>
#include <QFileInfo>
#include <QString>

namespace LocalPath {
inline Qt::CaseSensitivity caseSensitivity()
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    // Windows and the default macOS filesystems are case-insensitive.
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

inline QString normalized(const QString &path)
{
    if (path.isEmpty())
        return QString();

    const QFileInfo info(path);
    QString result = info.canonicalFilePath();
    if (result.isEmpty()) {
        const QString parent = QFileInfo(info.absolutePath()).canonicalFilePath();
        result = parent.isEmpty() ? info.absoluteFilePath()
                                  : QDir(parent).absoluteFilePath(info.fileName());
    }
    return QDir::cleanPath(QDir::fromNativeSeparators(result));
}

inline bool equal(const QString &left,
                  const QString &right,
                  Qt::CaseSensitivity sensitivity = caseSensitivity())
{
    if (left.isEmpty() || right.isEmpty())
        return left.isEmpty() && right.isEmpty();
    return normalized(left).compare(normalized(right), sensitivity) == 0;
}
} // namespace LocalPath

#endif // LOCALPATH_H
