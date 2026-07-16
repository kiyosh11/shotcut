/*
 * Copyright (c) 2026 Meltytech, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "mltxmlproperty.h"

#include <QtTest/QtTest>

class TestMltXmlProperty : public QObject
{
    Q_OBJECT

private slots:
    void preservesAttributeAndTextSemantics()
    {
        QXmlStreamAttributes attributes;
        attributes.append(QXmlStreamAttribute(QStringLiteral("name"), QStringLiteral("resource")));
        attributes.append(
            QXmlStreamAttribute(QStringLiteral("value"), QStringLiteral("media/clip.mp4")));
        QCOMPARE(MltXmlProperty::value(attributes, QString()), QStringLiteral("media/clip.mp4"));
        QCOMPARE(MltXmlProperty::value(attributes, QStringLiteral("?suffix")),
                 QStringLiteral("media/clip.mp4?suffix"));
    }
};

QTEST_GUILESS_MAIN(TestMltXmlProperty)
#include "test_mltxmlproperty.moc"
