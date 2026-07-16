/*
 * Copyright (c) 2026 Meltytech, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef MLTXMLPROPERTY_H
#define MLTXMLPROPERTY_H

#include <QString>
#include <QXmlStreamAttributes>

namespace MltXmlProperty {
inline QString value(const QXmlStreamAttributes &attributes, const QString &text)
{
    return attributes.value(QStringLiteral("value")).toString() + text;
}
} // namespace MltXmlProperty

#endif // MLTXMLPROPERTY_H
