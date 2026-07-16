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
        QCOMPARE(McpBridgePolicy::filterPathKind("placebo.shader", "shader_path"),
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

    void matchesShotcutClipFilterApplicability()
    {
        using McpBridgePolicy::FilterApplicability;
        QCOMPARE(McpBridgePolicy::clipFilterApplicability(false, false, true, false, "avformat"),
                 FilterApplicability::Allowed);
        QCOMPARE(McpBridgePolicy::clipFilterApplicability(false, true, true, false, "avformat"),
                 FilterApplicability::RequiresGpu);
        QCOMPARE(McpBridgePolicy::clipFilterApplicability(false, false, false, false, "avformat"),
                 FilterApplicability::Allowed);
        QCOMPARE(McpBridgePolicy::clipFilterApplicability(true, true, true, false, "avformat"),
                 FilterApplicability::Allowed);
        QCOMPARE(McpBridgePolicy::clipFilterApplicability(true, false, false, false, "avformat"),
                 FilterApplicability::GpuIncompatible);
        QCOMPARE(McpBridgePolicy::clipFilterApplicability(false, false, true, true, "xml-clip"),
                 FilterApplicability::ReverseUnsupported);
        QCOMPARE(McpBridgePolicy::clipFilterApplicability(false, false, true, true, "avformat"),
                 FilterApplicability::Allowed);
    }

    void blocksActiveFilterServices()
    {
        const QStringList manualOnlyFamilies{
            QStringLiteral("jack"),
            QStringLiteral("jackrack"),
            QStringLiteral("ladspa"),
            QStringLiteral("lv2"),
            QStringLiteral("vst2"),
            QStringLiteral("openfx"),
        };
        for (const QString &family : manualOnlyFamilies) {
            QVERIFY(!McpBridgePolicy::activeFilterServiceAllowed(family));
            QVERIFY(
                !McpBridgePolicy::activeFilterServiceAllowed(family + QStringLiteral(".plugin")));
            QVERIFY(!McpBridgePolicy::activeFilterServiceAllowed(
                QStringLiteral("  ") + family.toUpper() + QStringLiteral(".PLUGIN  ")));
        }
        QVERIFY(McpBridgePolicy::activeFilterServiceAllowed("jackal"));
        QVERIFY(McpBridgePolicy::activeFilterServiceAllowed("openfxed"));
        QVERIFY(!McpBridgePolicy::filterIdentitiesActiveAllowed("vst2.1934451059", "safe"));
        QVERIFY(!McpBridgePolicy::filterIdentitiesActiveAllowed("safe", "openfx.plugin"));
        QVERIFY(McpBridgePolicy::filterIdentitiesActiveAllowed("safe", "frei0r.alphaspot"));
        QVERIFY(!McpBridgePolicy::activeFilterServiceAllowed("placebo.shader"));
        QVERIFY(!McpBridgePolicy::activeFilterServiceAllowed("opencv.tracker"));
        QVERIFY(McpBridgePolicy::activeFilterServiceAllowed("frei0r.alphaspot"));
        QVERIFY(McpBridgePolicy::activeFilterServiceAllowed("avfilter.lut3d"));
        QVERIFY(McpBridgePolicy::activeFilterServiceAllowed("avfilter.gblur"));
        QVERIFY(!McpBridgePolicy::activeFilterServiceAllowed("avfilter.drawtext"));
        QVERIFY(!McpBridgePolicy::activeFilterServiceAllowed("avfilter.arnndn"));
        QVERIFY(!McpBridgePolicy::activeFilterServiceAllowed("avfilter.subtitles"));
        QVERIFY(!McpBridgePolicy::activeFilterServiceAllowed("avfilter.zmq"));
        QVERIFY(!McpBridgePolicy::activeFilterServiceAllowed("avfilter.movie"));
    }

    void validatesAttachedFilterIdentity()
    {
        QVERIFY(McpBridgePolicy::attachedFilterIdentityAllowed("blur_gaussian_av",
                                                               "avfilter.gblur",
                                                               "AVFILTER.GBLUR",
                                                               true,
                                                               true));
        QVERIFY(!McpBridgePolicy::attachedFilterIdentityAllowed("blur_gaussian_av",
                                                                "avfilter.gblur",
                                                                "placebo.shader",
                                                                true,
                                                                true));
        QVERIFY(!McpBridgePolicy::attachedFilterIdentityAllowed("blur_gaussian_av",
                                                                "avfilter.gblur",
                                                                "avfilter.gblur",
                                                                false,
                                                                true));
        QVERIFY(!McpBridgePolicy::attachedFilterIdentityAllowed("blur_gaussian_av",
                                                                "avfilter.gblur",
                                                                "avfilter.gblur",
                                                                true,
                                                                false));
        QVERIFY(!McpBridgePolicy::attachedFilterIdentityAllowed("placebo.shader",
                                                                "placebo.shader",
                                                                "placebo.shader",
                                                                true,
                                                                true));
    }

    void blocksNestedMaskPathsAndUnsafeExistingSelectors()
    {
        QVERIFY(!McpBridgePolicy::nestedFilterPathParameterWriteAllowed("mask_start",
                                                                        "filter.src",
                                                                        false));
        QVERIFY(!McpBridgePolicy::nestedFilterPathParameterWriteAllowed("mask_start",
                                                                        "filter.av.file",
                                                                        false));
        QVERIFY(!McpBridgePolicy::nestedFilterPathParameterWriteAllowed("mask_start",
                                                                        "filter.shader_path",
                                                                        false));
        QVERIFY(McpBridgePolicy::nestedFilterPathParameterWriteAllowed("mask_start",
                                                                       "filter.src",
                                                                       true));
        QVERIFY(McpBridgePolicy::nestedFilterPathParameterWriteAllowed("mask_start",
                                                                       "filter.resource",
                                                                       false));
        QVERIFY(McpBridgePolicy::nestedFilterPathParameterWriteAllowed("frei0r.alphaspot",
                                                                       "filter.src",
                                                                       false));
        QVERIFY(!McpBridgePolicy::nestedFilterParameterWriteAllowed("mask_start",
                                                                    "filter",
                                                                    "jackrack",
                                                                    false));
        QVERIFY(McpBridgePolicy::nestedFilterParameterWriteAllowed("mask_start",
                                                                   "filter",
                                                                   "shape",
                                                                   false));
    }

    void rejectsCStringTruncationInputs()
    {
        QString nulName = QStringLiteral("filter");
        nulName.append(QChar(u'\0'));
        nulName.append(QLatin1Char('x'));
        QVERIFY(!McpBridgePolicy::filterParameterNameAllowed(nulName));

        QString controlName = QStringLiteral("filter");
        controlName.append(QChar(u'\n'));
        controlName.append(QStringLiteral("src"));
        QVERIFY(!McpBridgePolicy::filterParameterNameAllowed(controlName));

        QString delName = QStringLiteral("filter");
        delName.append(QChar(u'\x007f'));
        delName.append(QStringLiteral("src"));
        QVERIFY(!McpBridgePolicy::filterParameterNameAllowed(delName));
        QVERIFY(!McpBridgePolicy::filterParameterNameAllowed("mlt_service"));
        QVERIFY(!McpBridgePolicy::filterParameterNameAllowed("shotcut:filter"));
        QVERIFY(McpBridgePolicy::filterParameterNameAllowed("filter.resource"));

        QString nulValue = QStringLiteral("shape");
        nulValue.append(QChar(u'\0'));
        nulValue.append(QStringLiteral("jackrack"));
        QVERIFY(!McpBridgePolicy::cStringValueAllowed(nulValue));
        QVERIFY(McpBridgePolicy::cStringValueAllowed(QStringLiteral("line one\nline two")));
    }

    void normalizesTypedFactoryValues()
    {
        QString rawValue = QStringLiteral("stale");
        bool reset = false;
        QVERIFY(!McpBridgePolicy::filterStringOrResetValue(QJsonValue(QJsonValue::Undefined),
                                                           &rawValue,
                                                           &reset));
        QVERIFY(McpBridgePolicy::filterStringOrResetValue(QJsonValue(QJsonValue::Null),
                                                          &rawValue,
                                                          &reset));
        QVERIFY(reset);
        QVERIFY(rawValue.isEmpty());

        QVERIFY(McpBridgePolicy::filterStringOrResetValue(QJsonValue(QString()), &rawValue, &reset));
        QVERIFY(!reset);
        QVERIFY(rawValue.isEmpty());
        QVERIFY(McpBridgePolicy::maskApplyTransitionParameterWriteAllowed("mask_apply",
                                                                          "transition",
                                                                          rawValue,
                                                                          reset));
        QVERIFY(McpBridgePolicy::filterStringOrResetValue(QJsonValue(QStringLiteral("qtblend")),
                                                          &rawValue,
                                                          &reset));
        QCOMPARE(rawValue, QStringLiteral("qtblend"));
        QVERIFY(McpBridgePolicy::maskApplyTransitionParameterWriteAllowed("mask_apply",
                                                                          "transition",
                                                                          rawValue,
                                                                          reset));
        QVERIFY(!McpBridgePolicy::filterStringOrResetValue(QJsonValue(0.0), &rawValue, &reset));
        QVERIFY(!McpBridgePolicy::filterStringOrResetValue(QJsonValue(true), &rawValue, &reset));

        QVERIFY(McpBridgePolicy::maskStartSelectorValue(QJsonValue(0.0), &rawValue, &reset));
        QVERIFY(!reset);
        QCOMPARE(rawValue, QStringLiteral("0"));
        QVERIFY(McpBridgePolicy::nestedFilterParameterWriteAllowed("mask_start",
                                                                   "filter",
                                                                   rawValue,
                                                                   reset));
        QVERIFY(!McpBridgePolicy::maskStartSelectorValue(QJsonValue(1.0), &rawValue, &reset));
        QVERIFY(!McpBridgePolicy::maskStartSelectorValue(QJsonValue(true), &rawValue, &reset));
        QVERIFY(McpBridgePolicy::maskStartSelectorValue(QJsonValue(QJsonValue::Null),
                                                        &rawValue,
                                                        &reset));
        QVERIFY(reset);
        QVERIFY(McpBridgePolicy::nestedFilterParameterWriteAllowed("mask_start",
                                                                   "filter",
                                                                   rawValue,
                                                                   reset));

        QVERIFY(
            McpBridgePolicy::filterStringOrResetValue(QJsonValue(QStringLiteral("color:#12345678")),
                                                      &rawValue,
                                                      &reset));
        QVERIFY(McpBridgePolicy::affineBackgroundParameterWriteAllowed("affine",
                                                                       "background",
                                                                       rawValue,
                                                                       reset));
        QVERIFY(McpBridgePolicy::filterStringOrResetValue(QJsonValue(QString()), &rawValue, &reset));
        QVERIFY(!McpBridgePolicy::affineBackgroundParameterWriteAllowed("affine",
                                                                        "background",
                                                                        rawValue,
                                                                        reset));
        QVERIFY(McpBridgePolicy::filterStringOrResetValue(QJsonValue(QJsonValue::Null),
                                                          &rawValue,
                                                          &reset));
        QVERIFY(McpBridgePolicy::affineBackgroundParameterWriteAllowed("affine",
                                                                       "background",
                                                                       rawValue,
                                                                       reset));
    }

    void restrictsNestedMaskFactoriesAndPaths()
    {
        QVERIFY(
            McpBridgePolicy::nestedFilterParameterWriteAllowed("mask_start", "filter", "", false));
        QVERIFY(
            McpBridgePolicy::nestedFilterParameterWriteAllowed("mask_start", "filter", "0", false));
        QVERIFY(McpBridgePolicy::nestedFilterParameterWriteAllowed("mask_start",
                                                                   "filter",
                                                                   "shape",
                                                                   false));
        QVERIFY(McpBridgePolicy::nestedFilterParameterWriteAllowed("mask_start",
                                                                   "filter",
                                                                   "frei0r.alphaspot",
                                                                   false));
        QVERIFY(McpBridgePolicy::nestedFilterParameterWriteAllowed("mask_start",
                                                                   "filter",
                                                                   "unsafe",
                                                                   true));
        const QStringList unsafeMaskSelectors{
            QStringLiteral(" shape"),
            QStringLiteral("shape "),
            QStringLiteral("SHAPE"),
            QStringLiteral("shape\n"),
            QStringLiteral("shape\r"),
            QStringLiteral("shape\t"),
            QStringLiteral("frei0r.alphaspot "),
        };
        for (const QString &selector : unsafeMaskSelectors) {
            QVERIFY(!McpBridgePolicy::nestedFilterParameterWriteAllowed("mask_start",
                                                                        "filter",
                                                                        selector,
                                                                        false));
        }

        QVERIFY(McpBridgePolicy::maskApplyTransitionServiceAllowed("mask_apply", ""));
        QVERIFY(McpBridgePolicy::maskApplyTransitionServiceAllowed("mask_apply", "qtblend"));
        const QStringList unsafeTransitions{
            QStringLiteral("webvfx"),
            QStringLiteral("frei0r.cairoblend"),
            QStringLiteral(" qtblend"),
            QStringLiteral("qtblend "),
            QStringLiteral("QTBLEND"),
            QStringLiteral("qtblend\n"),
            QStringLiteral("qtblend\r"),
            QStringLiteral("qtblend\t"),
        };
        for (const QString &transition : unsafeTransitions) {
            QVERIFY(!McpBridgePolicy::maskApplyTransitionServiceAllowed("mask_apply", transition));
            QVERIFY(!McpBridgePolicy::maskApplyTransitionParameterWriteAllowed("mask_apply",
                                                                               "transition",
                                                                               transition,
                                                                               false));
        }
        QVERIFY(McpBridgePolicy::maskApplyTransitionServiceAllowed("some.filter", "webvfx"));
        QVERIFY(McpBridgePolicy::maskApplyTransitionParameterWriteAllowed("mask_apply",
                                                                          "transition",
                                                                          "qtblend",
                                                                          false));
        QVERIFY(McpBridgePolicy::maskApplyTransitionParameterWriteAllowed("mask_apply",
                                                                          "transition",
                                                                          "",
                                                                          false));
        QVERIFY(McpBridgePolicy::maskApplyTransitionParameterWriteAllowed("mask_apply",
                                                                          "transition",
                                                                          "webvfx",
                                                                          true));
        QVERIFY(!McpBridgePolicy::nestedTransitionPathParameterWriteAllowed("mask_apply",
                                                                            "transition.resource",
                                                                            false));
        QVERIFY(!McpBridgePolicy::nestedTransitionPathParameterWriteAllowed("mask_apply",
                                                                            "transition.av.file",
                                                                            false));
        QVERIFY(McpBridgePolicy::nestedTransitionPathParameterWriteAllowed("mask_apply",
                                                                           "transition.threads",
                                                                           false));
        QVERIFY(McpBridgePolicy::nestedTransitionPathParameterWriteAllowed("mask_apply",
                                                                           "transition.resource",
                                                                           true));

        const QStringList safeBackgrounds{
            QStringLiteral("color:0"),
            QStringLiteral("colour:0"),
            QStringLiteral("color:#fff"),
            QStringLiteral("colour:#FfFf"),
            QStringLiteral("color:#012345"),
            QStringLiteral("colour:#01234567"),
        };
        QVERIFY(McpBridgePolicy::affineBackgroundServiceAllowed("affine", ""));
        for (const QString &background : safeBackgrounds) {
            QVERIFY(McpBridgePolicy::affineBackgroundServiceAllowed("affine", background));
            QVERIFY(McpBridgePolicy::affineBackgroundParameterWriteAllowed("affine",
                                                                           "background",
                                                                           background,
                                                                           false));
        }
        const QStringList unsafeBackgrounds{
            QStringLiteral("Color:0"),
            QStringLiteral("COLOR:#fff"),
            QStringLiteral(" color:#fff"),
            QStringLiteral("color:#fff "),
            QStringLiteral("color:#00000000\n"),
            QStringLiteral("color:#fff\r"),
            QStringLiteral("color:#fff\t"),
            QStringLiteral("color:#12"),
            QStringLiteral("color:#12345"),
            QStringLiteral("color:#1234567"),
            QStringLiteral("color:#123456789"),
            QStringLiteral("color:#12g"),
            QStringLiteral("avformat:clip.m3u8"),
            QStringLiteral("https://example.invalid/video"),
            QStringLiteral("C:/payload.mp4"),
        };
        for (const QString &background : unsafeBackgrounds) {
            QVERIFY(!McpBridgePolicy::affineBackgroundServiceAllowed("affine", background));
            QVERIFY(!McpBridgePolicy::affineBackgroundParameterWriteAllowed("affine",
                                                                            "background",
                                                                            background,
                                                                            false));
        }
        QVERIFY(!McpBridgePolicy::affineBackgroundParameterWriteAllowed("affine",
                                                                        "background",
                                                                        "",
                                                                        false));
        QVERIFY(McpBridgePolicy::affineBackgroundParameterWriteAllowed("affine",
                                                                       "background",
                                                                       "clip.m3u8",
                                                                       true));
        QVERIFY(!McpBridgePolicy::nestedProducerPathParameterWriteAllowed("affine",
                                                                          "producer.resource",
                                                                          false));
        QVERIFY(!McpBridgePolicy::nestedProducerPathParameterWriteAllowed("affine",
                                                                          "producer.av.file",
                                                                          false));
        QVERIFY(McpBridgePolicy::nestedProducerPathParameterWriteAllowed("affine",
                                                                         "producer.aspect_ratio",
                                                                         false));
        QVERIFY(McpBridgePolicy::nestedProducerPathParameterWriteAllowed("affine",
                                                                         "producer.resource",
                                                                         true));

        QVERIFY(McpBridgePolicy::dustFactoryServiceAllowed("dust", ""));
        QVERIFY(!McpBridgePolicy::dustFactoryServiceAllowed("dust", "loader"));
        QVERIFY(!McpBridgePolicy::dustFactoryServiceAllowed("dust", " loader"));
        QVERIFY(McpBridgePolicy::dustFactoryParameterWriteAllowed("dust", "factory", true));
        QVERIFY(!McpBridgePolicy::dustFactoryParameterWriteAllowed("dust", "factory", false));
        QVERIFY(McpBridgePolicy::dustFactoryParameterWriteAllowed("safe", "factory", false));

        QVERIFY(McpBridgePolicy::transitionProducerFactoryAllowed("luma", ""));
        QVERIFY(McpBridgePolicy::transitionProducerFactoryAllowed("luma", "loader"));
        QVERIFY(McpBridgePolicy::transitionProducerFactoryAllowed("composite", "loader"));
        const QStringList unsafeFactories{QStringLiteral("Loader"),
                                          QStringLiteral(" loader"),
                                          QStringLiteral("loader "),
                                          QStringLiteral("loader\n"),
                                          QStringLiteral("avformat")};
        for (const QString &factory : unsafeFactories) {
            QVERIFY(!McpBridgePolicy::transitionProducerFactoryAllowed("luma", factory));
            QVERIFY(!McpBridgePolicy::transitionProducerFactoryAllowed("composite", factory));
        }
        QVERIFY(McpBridgePolicy::transitionProducerFactoryAllowed("mix", "avformat"));
        QVERIFY(McpBridgePolicy::transitionProducerFactoryParameterWriteAllowed("luma",
                                                                                "factory",
                                                                                "loader",
                                                                                false));
        QVERIFY(McpBridgePolicy::transitionProducerFactoryParameterWriteAllowed("luma",
                                                                                "factory",
                                                                                "avformat",
                                                                                true));
        QVERIFY(!McpBridgePolicy::transitionProducerFactoryParameterWriteAllowed("luma",
                                                                                 "factory",
                                                                                 "Loader",
                                                                                 false));
        QVERIFY(!McpBridgePolicy::nestedTransitionProducerPathAllowed("luma", "producer.resource"));
        QVERIFY(!McpBridgePolicy::nestedTransitionProducerPathAllowed("luma", "producer.av.file"));
        QVERIFY(!McpBridgePolicy::nestedTransitionProducerPathAllowed("composite", "luma.resource"));
        QVERIFY(!McpBridgePolicy::nestedTransitionProducerPathAllowed("composite", "luma.url"));
        QVERIFY(McpBridgePolicy::nestedTransitionProducerPathAllowed("composite", "luma.softness"));
        QVERIFY(McpBridgePolicy::nestedTransitionProducerPathAllowed("mix", "producer.resource"));
    }

    void restrictsAvFilterOptionsAndLutSuffixes()
    {
        QVERIFY(McpBridgePolicy::avFilterPropertyAllowed("avfilter.adeclick", "av.window"));
        QVERIFY(McpBridgePolicy::avFilterPropertyAllowed("avfilter.haas", "av.right_phase"));
        QVERIFY(McpBridgePolicy::avFilterPropertyAllowed("avfilter.hue", "av.h"));
        QVERIFY(McpBridgePolicy::avFilterPropertyAllowed("avfilter.lut3d", "av.file"));
        QVERIFY(McpBridgePolicy::avFilterPropertyAllowed("avfilter.lut3d", "av.interp"));
        QVERIFY(McpBridgePolicy::avFilterPropertyAllowed("avfilter.gblur", "av.sigma"));
        QVERIFY(McpBridgePolicy::avFilterPropertyAllowed("avfilter.gblur", "av.sigmaV"));
        QVERIFY(McpBridgePolicy::avFilterPropertyAllowed("avfilter.gblur", "av.planes"));
        QVERIFY(McpBridgePolicy::avFilterPropertyAllowed("avfilter.vflip", "disable"));
        QVERIFY(!McpBridgePolicy::avFilterPropertyAllowed("avfilter.vflip", "av.file"));
        QVERIFY(!McpBridgePolicy::avFilterPropertyAllowed("avfilter.hue", "av.fontfile"));
        QVERIFY(!McpBridgePolicy::avFilterPropertyAllowed("some.filter", "av.file"));
        QVERIFY(McpBridgePolicy::filterInputSuffixAllowed("avfilter.lut3d", "av.file", "CUBE"));
        QVERIFY(McpBridgePolicy::filterInputSuffixAllowed("avfilter.lut3d", "av.file", "3dl"));
        QVERIFY(!McpBridgePolicy::filterInputSuffixAllowed("avfilter.lut3d", "av.file", "txt"));
        QVERIFY(McpBridgePolicy::filterInputSuffixAllowed("some.filter", "resource", "txt"));
    }

    void evaluatesBothFilterIdentities()
    {
        using McpBridgePolicy::FilterPathKind;
        QVERIFY(
            McpBridgePolicy::filterIdentitiesActiveAllowed("blur_gaussian_av", "avfilter.gblur"));
        QVERIFY(!McpBridgePolicy::filterIdentitiesActiveAllowed("innocent", "placebo.shader"));
        QVERIFY(
            !McpBridgePolicy::filterIdentitiesActiveAllowed("placebo.shader", "frei0r.alphaspot"));
        QVERIFY(McpBridgePolicy::avFilterPropertyAllowed("avfilter.gblur", "av.sigma"));
        QVERIFY(!McpBridgePolicy::avFilterPropertyAllowed("blur_gaussian_av", "av.sigma"));
        QCOMPARE(McpBridgePolicy::filterPathKindForIdentities("lut3d", "avfilter.lut3d", "av.file"),
                 FilterPathKind::ExistingFile);
        QCOMPARE(McpBridgePolicy::filterPathKindForIdentities("unknown",
                                                              "frei0r.alphaspot",
                                                              "filename"),
                 FilterPathKind::ExistingFile);
        QVERIFY(McpBridgePolicy::nestedFilterParameterWriteAllowed("mask_start",
                                                                   "filter",
                                                                   "shape",
                                                                   false));
        QVERIFY(McpBridgePolicy::nestedFilterParameterWriteAllowed("mask_start",
                                                                   "filter",
                                                                   "frei0r.alphaspot",
                                                                   false));
        QVERIFY(!McpBridgePolicy::nestedFilterParameterWriteAllowed("mask_start",
                                                                    "filter",
                                                                    "jackrack",
                                                                    false));
        QVERIFY(McpBridgePolicy::nestedFilterParameterWriteAllowed("mask_start",
                                                                   "filter",
                                                                   "jackrack",
                                                                   true));
        QVERIFY(McpBridgePolicy::coreTransitionServiceAllowed("mix"));
        QVERIFY(McpBridgePolicy::coreTransitionServiceAllowed("qtblend"));
        QVERIFY(!McpBridgePolicy::coreTransitionServiceAllowed("openfx.dynamic"));
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
        QVERIFY(
            !McpBridgePolicy::isBuiltInValue("maskFromFile", "filter.resource", "%luma../outside"));
    }
};

QTEST_GUILESS_MAIN(TestMcpBridgePolicy)
#include "test_mcpbridgepolicy.moc"
