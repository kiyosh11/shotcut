/*
 * Copyright (c) 2026 Meltytech, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef ENCODEPRESETUTILS_H
#define ENCODEPRESETUTILS_H

#include <QString>
#include <QStringList>

namespace EncodePresetUtils {
inline QString profileName(const QString &presetKey)
{
    const QStringList parts = presetKey.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    if (parts.size() > 3 && parts.at(0) == QStringLiteral("consumer")
        && parts.at(1) == QStringLiteral("avformat")) {
        return parts.at(2);
    }
    return QString();
}
} // namespace EncodePresetUtils

#endif // ENCODEPRESETUTILS_H