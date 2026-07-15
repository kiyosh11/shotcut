/*
 * Copyright (c) 2026 Meltytech, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

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
        QCOMPARE(McpBridgePolicy::filterPathKind("vidstab", "filename"),
                 FilterPathKind::WritablePath);
        QCOMPARE(McpBridgePolicy::filterPathKind("bigsh0t_stabilize_360", "analysisFile"),
                 FilterPathKind::WritablePath);
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