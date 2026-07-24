/*
 * Copyright (c) 2026 Meltytech, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef MCPKEYFRAMEINTERPOLATION_H
#define MCPKEYFRAMEINTERPOLATION_H

#include "models/keyframesmodel.h"

#include <QMetaEnum>

namespace McpKeyframeInterpolation {

inline QString wireName(const char *enumKey)
{
    if (!enumKey)
        return QStringLiteral("unknown");

    QString key = QString::fromLatin1(enumKey);
    const QString interpolationSuffix = QStringLiteral("Interpolation");
    if (key.endsWith(interpolationSuffix))
        key.chop(interpolationSuffix.size());

    QString name;
    name.reserve(key.size() + 8);
    for (qsizetype index = 0; index < key.size(); ++index) {
        const QChar character = key.at(index);
        if (index > 0 && character.isUpper())
            name.append(QLatin1Char('_'));
        name.append(character.toLower());
    }
    return name;
}

inline QString name(mlt_keyframe_type type)
{
    const QMetaEnum interpolation = QMetaEnum::fromType<KeyframesModel::InterpolationType>();
    return wireName(interpolation.valueToKey(static_cast<int>(type)));
}

inline bool parse(const QString &name, KeyframesModel::InterpolationType *result)
{
    KeyframesModel::InterpolationType alias = KeyframesModel::LinearInterpolation;
    bool hasAlias = true;
    if (name == QStringLiteral("hold"))
        alias = KeyframesModel::DiscreteInterpolation;
    else if (name == QStringLiteral("smooth"))
        alias = KeyframesModel::SmoothNaturalInterpolation;
    else if (name == QStringLiteral("ease_in"))
        alias = KeyframesModel::EaseInCubic;
    else if (name == QStringLiteral("ease_out"))
        alias = KeyframesModel::EaseOutCubic;
    else if (name == QStringLiteral("ease_in_out"))
        alias = KeyframesModel::EaseInOutCubic;
    else
        hasAlias = false;

    if (hasAlias) {
        if (result)
            *result = alias;
        return true;
    }

    const QMetaEnum interpolation = QMetaEnum::fromType<KeyframesModel::InterpolationType>();
    for (int index = 0; index < interpolation.keyCount(); ++index) {
        if (wireName(interpolation.key(index)) == name) {
            if (result) {
                *result = static_cast<KeyframesModel::InterpolationType>(interpolation.value(index));
            }
            return true;
        }
    }
    return false;
}

} // namespace McpKeyframeInterpolation

#endif // MCPKEYFRAMEINTERPOLATION_H
