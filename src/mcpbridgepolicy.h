/*
 * Copyright (c) 2026 Meltytech, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef MCPBRIDGEPOLICY_H
#define MCPBRIDGEPOLICY_H

#include <QString>

namespace McpBridgePolicy {
enum class FilterPathKind { NotPath, ExistingFile, WritablePath };

// Kept independent of the live editor so policy classification can be unit-tested.
inline FilterPathKind filterPathKind(const QString &filterId, const QString &name)
{
    const QString id = filterId.toLower();
    const QString key = name.toLower();
    if (id == QStringLiteral("avfilter.lut3d") && key == QStringLiteral("av.file"))
        return FilterPathKind::ExistingFile;
    if ((id == QStringLiteral("maskfromfile") || id == QStringLiteral("maskglaxnimate"))
        && key == QStringLiteral("filter.resource")) {
        return FilterPathKind::ExistingFile;
    }
    if ((id == QStringLiteral("gpsgraphic") || id == QStringLiteral("gpstext"))
        && key == QStringLiteral("resource")) {
        return FilterPathKind::ExistingFile;
    }
    if (id == QStringLiteral("gpsgraphic") && key == QStringLiteral("bg_img_path"))
        return FilterPathKind::ExistingFile;
    if (id == QStringLiteral("vidstab") && key == QStringLiteral("filename"))
        return FilterPathKind::WritablePath;
    if ((id == QStringLiteral("bigsh0t_stabilize_360")
         || id == QStringLiteral("frei0r.bigsh0t_stabilize_360"))
        && key == QStringLiteral("analysisfile")) {
        return FilterPathKind::WritablePath;
    }
    return FilterPathKind::NotPath;
}

inline bool isBuiltInValue(const QString &filterId, const QString &name, const QString &value)
{
    if (filterId.compare(QStringLiteral("maskFromFile"), Qt::CaseInsensitive) != 0
        || name.compare(QStringLiteral("filter.resource"), Qt::CaseInsensitive) != 0
        || value.size() != 11 || !value.startsWith(QStringLiteral("%luma"))
        || !value.endsWith(QStringLiteral(".pgm"))) {
        return false;
    }
    bool ok = false;
    const int number = value.mid(5, 2).toInt(&ok);
    return ok && number >= 1 && number <= 22
           && value.mid(5, 2) == QString::number(number).rightJustified(2, QLatin1Char('0'));
}
} // namespace McpBridgePolicy

#endif // MCPBRIDGEPOLICY_H
