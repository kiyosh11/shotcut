/*
 * Copyright (c) 2026 Meltytech, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "localpath.h"
#include "mcpbridgepolicy.h"

#include <QtTest/QtTest>

class TestMcpBridgePolicy : public QObject
{
    Q_OBJECT

private slots:
    void classifiesFileBackedParameters()
    {
        using McpBridgePolicy::FilterPathKind;
        QCOMPARE(McpBridgePolicy::filterPathKind("avfilter.lut3d", "av.file"),
                 FilterPathKind::ExistingFile);
        QCOMPARE(McpBridgePolicy::filterPathKind("maskFromFile", "filter.resource"),
                 FilterPathKind::ExistingFile);
        QCOMPARE(McpBridgePolicy::filterPathKind("gpsgraphic", "resource"),
                 FilterPathKind::ExistingFile);
        QCOMPARE(McpBridgePolicy::filterPathKind("gpstext", "gps.file"),
                 FilterPathKind::ExistingFile);
        QCOMPARE(McpBridgePolicy::filterPathKind("vidstab", "results"),
                 FilterPathKind::ExistingFile);
        QCOMPARE(McpBridgePolicy::filterPathKind("vidstab", "filename"),
                 FilterPathKind::WritablePath);
        QCOMPARE(McpBridgePolicy::filterPathKind("bigsh0t_stabilize_360", "analysisFile"),
                 FilterPathKind::WritablePath);
    }

    void comparesLocalExportTargets()
    {
        const QString root = QDir::tempPath();
        const QString upper = QDir(root).absoluteFilePath(QStringLiteral("McpExport.mp4"));
        const QString lower = QDir(root).absoluteFilePath(QStringLiteral("mcpexport.mp4"));
        QVERIFY(LocalPath::equal(upper, lower, Qt::CaseInsensitive));
        QVERIFY(!LocalPath::equal(upper, lower, Qt::CaseSensitive));
        QVERIFY(LocalPath::equal(QDir(root).absoluteFilePath(QStringLiteral("folder/../clip.mp4")),
                                 QDir(root).absoluteFilePath(QStringLiteral("clip.mp4")),
                                 Qt::CaseSensitive));
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS) || defined(Q_OS_MAC)
        QVERIFY(LocalPath::equal(upper, lower));
#endif
    }

    void blocksRichTextExternalContentWrites()
    {
        QVERIFY(!McpBridgePolicy::richTextParameterWriteAllowed("richText", "html", false));
        QVERIFY(!McpBridgePolicy::richTextParameterWriteAllowed("richText", "resource", false));
        QVERIFY(!McpBridgePolicy::richTextParameterWriteAllowed("qtext", "html", false));
        QVERIFY(!McpBridgePolicy::richTextParameterWriteAllowed("QTEXT", "RESOURCE", false));
        QVERIFY(McpBridgePolicy::richTextParameterWriteAllowed("richText", "html", true));
        QVERIFY(McpBridgePolicy::richTextParameterWriteAllowed("qtext", "resource", true));
        QVERIFY(McpBridgePolicy::richTextParameterWriteAllowed("richText", "argument", false));
        QVERIFY(McpBridgePolicy::richTextParameterWriteAllowed("some.other.filter", "html", false));
    }

    void preservesOrdinaryResourceParameters()
    {
        QCOMPARE(McpBridgePolicy::filterPathKind("some.other.filter", "resource"),
                 McpBridgePolicy::FilterPathKind::NotPath);
    }

    void acceptsOnlyKnownBuiltInMaskTokens()
    {
        QVERIFY(McpBridgePolicy::isBuiltInValue("maskFromFile", "filter.resource", "%luma01.pgm"));
        QVERIFY(McpBridgePolicy::isBuiltInValue("maskFromFile", "filter.resource", "%luma22.pgm"));
        QVERIFY(!McpBridgePolicy::isBuiltInValue("maskFromFile", "filter.resource", "%luma00.pgm"));
        QVERIFY(!McpBridgePolicy::isBuiltInValue("maskFromFile", "filter.resource", "%luma23.pgm"));
        QVERIFY(!McpBridgePolicy::isBuiltInValue("maskFromFile",
                                                 "filter.resource",
                                                 "%luma../outside"));
    }
};

QTEST_GUILESS_MAIN(TestMcpBridgePolicy)
#include "test_mcpbridgepolicy.moc"
