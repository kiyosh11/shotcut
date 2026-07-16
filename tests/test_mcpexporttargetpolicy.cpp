/*
 * Copyright (c) 2026 Meltytech, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "encodepresetutils.h"
#include "exporttemporaryfileutils.h"
#include "mcpexporttargetpolicy.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class TestMcpExportTargetPolicy : public QObject
{
    Q_OBJECT

    static McpExportTargetPolicy::PathAuthorizer allowAll()
    {
        return [](const QString &, bool) { return true; };
    }

    static McpExportTargetPolicy::ConsumerProperties safeConsumer(
        const QString &target, const QString &muxer = QStringLiteral("mp4"))
    {
        McpExportTargetPolicy::ConsumerProperties properties{
            {QStringLiteral("mlt_service"), QStringLiteral("avformat")},
            {QStringLiteral("target"), target},
            {QStringLiteral("vcodec"), QStringLiteral("libx264")},
            {QStringLiteral("acodec"), QStringLiteral("aac")},
        };
        if (!muxer.isEmpty())
            properties.insert(QStringLiteral("f"), muxer);
        return properties;
    }

    static void createFile(const QString &path)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("frame"), qint64(5));
    }

private slots:
    void parsesPresetProfileFolders()
    {
        QCOMPARE(EncodePresetUtils::profileName(
                     QStringLiteral("consumer/avformat/atsc_1080p_25/high")),
                 QStringLiteral("atsc_1080p_25"));
        QVERIFY(EncodePresetUtils::profileName(QStringLiteral("consumer/avformat/high")).isEmpty());
        QVERIFY(EncodePresetUtils::profileName(QStringLiteral("consumer/avformat//high")).isEmpty());
        QVERIFY(
            EncodePresetUtils::profileName(QStringLiteral("producer/avformat/atsc_1080p_25/high"))
                .isEmpty());
    }
    void opensReturnedExportTemporaryFilesSafely()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QTemporaryFile file(QDir(temporary.path()).filePath(QStringLiteral("Shotcut.XXXXXX")));
        QVERIFY(!file.isOpen());
        QVERIFY(ExportTemporaryFileUtils::ensureOpen(&file));
        QVERIFY(file.isOpen());
        const QString fileName = file.fileName();
        QVERIFY(ExportTemporaryFileUtils::ensureOpen(&file));
        QCOMPARE(file.fileName(), fileName);
        QVERIFY(!ExportTemporaryFileUtils::ensureOpen(nullptr));
    }

    void validatesSafeConsumerProperties()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString target = QDir(temporary.path()).filePath(QStringLiteral("render.mp4"));
        auto properties = safeConsumer(target);
        properties.insert(QStringLiteral("width"), QStringLiteral("1920"));
        properties.insert(QStringLiteral("height"), QStringLiteral("1080"));
        properties.insert(QStringLiteral("subtitle.0.lang"), QStringLiteral("eng"));
        properties.insert(QStringLiteral("x264-params"), QStringLiteral("bff=1"));
        properties.insert(QStringLiteral("x265-params"),
                          QStringLiteral("crf=20:keyint=60:master-display=G(8500,39850)B(6550,2300)"
                                         "R(35400,14600)WP(15635,16450)L(10000000,50):"
                                         "max-cll=1000,400"));
        properties.insert(QStringLiteral("svtav1-params"),
                          QStringLiteral("rc=0:tbr=4500:pred-struct=2:mastering-display="
                                         "G(8500,39850)B(6550,2300)R(35400,14600)"
                                         "WP(15635,16450)L(10000000,50):max-cll=1000,400"));
        QString error;
        QVERIFY(McpExportTargetPolicy::validateConsumerProperties(target,
                                                                  false,
                                                                  0,
                                                                  properties,
                                                                  allowAll(),
                                                                  &error));

        properties = {
            {QStringLiteral("MLT_SERVICE"), QStringLiteral("AvFormat")},
            {QStringLiteral("TARGET"), target},
            {QStringLiteral("f"), QStringLiteral("mp4")},
        };
        QVERIFY(McpExportTargetPolicy::validateConsumerProperties(target,
                                                                  false,
                                                                  0,
                                                                  properties,
                                                                  allowAll(),
                                                                  &error));
    }

    void validatesDefaultConsumerShapes()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString target = QDir(temporary.path()).filePath(QStringLiteral("render.mp4"));
        QString error;

        auto native8 = safeConsumer(target, QStringLiteral("mp4"));
        native8.insert(QStringLiteral("movflags"), QStringLiteral("+faststart"));
        native8.insert(QStringLiteral("crf"), QStringLiteral("23"));
        native8.insert(QStringLiteral("vcodec"), QStringLiteral("libx264"));
        native8.insert(QStringLiteral("preset"), QStringLiteral("fast"));
        native8.insert(QStringLiteral("acodec"), QStringLiteral("aac"));
        QVERIFY(McpExportTargetPolicy::validateConsumerProperties(target,
                                                                  false,
                                                                  0,
                                                                  native8,
                                                                  allowAll(),
                                                                  &error));

        auto tenBit = safeConsumer(target, QStringLiteral("mp4"));
        tenBit.insert(QStringLiteral("movflags"), QStringLiteral("+faststart"));
        tenBit.insert(QStringLiteral("crf"), QStringLiteral("23"));
        tenBit.insert(QStringLiteral("vcodec"), QStringLiteral("libx265"));
        tenBit.insert(QStringLiteral("pix_fmt"), QStringLiteral("yuv420p10le"));
        tenBit.insert(QStringLiteral("preset"), QStringLiteral("medium"));
        tenBit.insert(QStringLiteral("vprofile"), QStringLiteral("main10"));
        tenBit.insert(QStringLiteral("vtag"), QStringLiteral("hvc1"));
        tenBit.insert(QStringLiteral("acodec"), QStringLiteral("aac"));
        tenBit.insert(QStringLiteral("x265-params"), QStringLiteral("keyint=125:bframes=3:crf=23:"));
        QVERIFY(McpExportTargetPolicy::validateConsumerProperties(target,
                                                                  false,
                                                                  0,
                                                                  tenBit,
                                                                  allowAll(),
                                                                  &error));

        auto qsv = tenBit;
        qsv.insert(QStringLiteral("vcodec"), QStringLiteral("hevc_qsv"));
        qsv.insert(QStringLiteral("pix_fmt"), QStringLiteral("p010le"));
        qsv.insert(QStringLiteral("load_plugin"), QStringLiteral("hevc_hw"));
        qsv.remove(QStringLiteral("x265-params"));
        QVERIFY(McpExportTargetPolicy::validateConsumerProperties(target,
                                                                  false,
                                                                  0,
                                                                  qsv,
                                                                  allowAll(),
                                                                  &error));
    }
    void rejectsConsumerOverridesAndUnknownProperties()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QDir directory(temporary.path());
        const QString target = directory.filePath(QStringLiteral("render.mp4"));
        QString error;

        auto properties = safeConsumer(target);
        properties.insert(QStringLiteral("target"),
                          directory.filePath(QStringLiteral("outside.mp4")));
        QVERIFY(!McpExportTargetPolicy::validateConsumerProperties(target,
                                                                   false,
                                                                   0,
                                                                   properties,
                                                                   allowAll(),
                                                                   &error));

        properties = safeConsumer(target);
        properties.insert(QStringLiteral("mlt_service"), QStringLiteral("xml"));
        QVERIFY(!McpExportTargetPolicy::validateConsumerProperties(target,
                                                                   false,
                                                                   0,
                                                                   properties,
                                                                   allowAll(),
                                                                   &error));

        properties = safeConsumer(target);
        properties.insert(QStringLiteral("TARGET"), target);
        QVERIFY(!McpExportTargetPolicy::validateConsumerProperties(target,
                                                                   false,
                                                                   0,
                                                                   properties,
                                                                   allowAll(),
                                                                   &error));
        QVERIFY(error.contains(QStringLiteral("duplicate"), Qt::CaseInsensitive));

        properties = safeConsumer(target);
        properties.insert(QStringLiteral("unclassified_option"), QStringLiteral("fast"));
        QVERIFY(!McpExportTargetPolicy::validateConsumerProperties(target,
                                                                   false,
                                                                   0,
                                                                   properties,
                                                                   allowAll(),
                                                                   &error));
        QVERIFY(error.contains(QStringLiteral("manual export"), Qt::CaseInsensitive));
    }

    void rejectsUnsafeConstrainedPropertyValues_data()
    {
        QTest::addColumn<QString>("propertyName");
        QTest::addColumn<QString>("value");
        QTest::newRow("movflags") << QStringLiteral("movflags")
                                  << QStringLiteral("+faststart+frag_keyframe");
        QTest::newRow("preset") << QStringLiteral("preset") << QStringLiteral("ultrafast");
        QTest::newRow("vtag") << QStringLiteral("vtag") << QStringLiteral("avc1");
        QTest::newRow("load-plugin") << QStringLiteral("load_plugin") << QStringLiteral("hevc_sw");
    }

    void rejectsUnsafeConstrainedPropertyValues()
    {
        QFETCH(QString, propertyName);
        QFETCH(QString, value);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString target = QDir(temporary.path()).filePath(QStringLiteral("render.mp4"));
        auto properties = safeConsumer(target, QStringLiteral("mp4"));
        properties.insert(propertyName, value);
        QString error;
        QVERIFY(!McpExportTargetPolicy::validateConsumerProperties(target,
                                                                   false,
                                                                   0,
                                                                   properties,
                                                                   allowAll(),
                                                                   &error));
        QVERIFY(error.contains(QStringLiteral("value"), Qt::CaseInsensitive));
    }
    void rejectsTwoPassConsumers()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString target = QDir(temporary.path()).filePath(QStringLiteral("render.mp4"));
        const auto properties = safeConsumer(target);
        QString error;
        QVERIFY(!McpExportTargetPolicy::validateConsumerProperties(target,
                                                                   false,
                                                                   1,
                                                                   properties,
                                                                   allowAll(),
                                                                   &error));
        QVERIFY(!McpExportTargetPolicy::validateConsumerProperties(target,
                                                                   false,
                                                                   2,
                                                                   properties,
                                                                   allowAll(),
                                                                   &error));
        QVERIFY(error.contains(QStringLiteral("sidecar"), Qt::CaseInsensitive));
    }

    void rejectsUnsafeMuxers_data()
    {
        QTest::addColumn<QString>("muxer");
        for (const QString &muxer : {QStringLiteral("tee"),
                                     QStringLiteral("segment"),
                                     QStringLiteral("stream_segment"),
                                     QStringLiteral("hls"),
                                     QStringLiteral("dash"),
                                     QStringLiteral("fifo"),
                                     QStringLiteral("image2pipe"),
                                     QStringLiteral("webm_chunk"),
                                     QStringLiteral("smoothstreaming")}) {
            QTest::newRow(qPrintable(muxer)) << muxer;
        }
    }

    void rejectsUnsafeMuxers()
    {
        QFETCH(QString, muxer);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString target = QDir(temporary.path()).filePath(QStringLiteral("render.mp4"));
        const auto properties = safeConsumer(target, muxer);
        QString error;
        QVERIFY(!McpExportTargetPolicy::validateConsumerProperties(target,
                                                                   false,
                                                                   0,
                                                                   properties,
                                                                   allowAll(),
                                                                   &error));
        QVERIFY(error.contains(QStringLiteral("muxer"), Qt::CaseInsensitive));
    }

    void rejectsNonCanonicalMuxerTokens()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString target = QDir(temporary.path()).filePath(QStringLiteral("render.m3u8"));
        QString error;

        QVERIFY(McpExportTargetPolicy::validateConsumerProperties(
            target, false, 0, safeConsumer(target, QStringLiteral("mp4")), allowAll(), &error));

        for (const QString &muxer :
             {QStringLiteral(" mp4"), QStringLiteral("mp4 "), QStringLiteral("MP4")}) {
            error.clear();
            QVERIFY(!McpExportTargetPolicy::validateConsumerProperties(target,
                                                                       false,
                                                                       0,
                                                                       safeConsumer(target, muxer),
                                                                       allowAll(),
                                                                       &error));
            QVERIFY(error.contains(QStringLiteral("muxer"), Qt::CaseInsensitive));
        }

        auto properties = safeConsumer(target, QString());
        properties.insert(QStringLiteral("F"), QStringLiteral("mp4"));
        error.clear();
        QVERIFY(!McpExportTargetPolicy::validateConsumerProperties(target,
                                                                   false,
                                                                   0,
                                                                   properties,
                                                                   allowAll(),
                                                                   &error));
        QVERIFY(error.contains(QStringLiteral("muxer"), Qt::CaseInsensitive));
    }

    void rejectsUnsafeTargetSuffixAndAcceptsImage2()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QDir directory(temporary.path());
        QString error;

        const QString hlsTarget = directory.filePath(QStringLiteral("render.m3u8"));
        QVERIFY(!McpExportTargetPolicy::validateConsumerProperties(
            hlsTarget, false, 0, safeConsumer(hlsTarget, QString()), allowAll(), &error));

        const QString imageTarget = directory.filePath(QStringLiteral("render-%05d.png"));
        QVERIFY(McpExportTargetPolicy::validateConsumerProperties(imageTarget,
                                                                  true,
                                                                  0,
                                                                  safeConsumer(imageTarget,
                                                                               QStringLiteral(
                                                                                   "image2")),
                                                                  allowAll(),
                                                                  &error));
        QVERIFY(!McpExportTargetPolicy::validateConsumerProperties(imageTarget,
                                                                   false,
                                                                   0,
                                                                   safeConsumer(imageTarget,
                                                                                QStringLiteral(
                                                                                    "image2")),
                                                                   allowAll(),
                                                                   &error));
    }

    void rejectsSideOutputProperties_data()
    {
        QTest::addColumn<QString>("propertyName");
        for (const QString &name : {QStringLiteral("passlogfile"),
                                    QStringLiteral("vstats_file"),
                                    QStringLiteral("segment_list"),
                                    QStringLiteral("hls_segment_filename"),
                                    QStringLiteral("hls_key_info_file"),
                                    QStringLiteral("init_seg_name"),
                                    QStringLiteral("media_seg_name"),
                                    QStringLiteral("header_filename"),
                                    QStringLiteral("fpre"),
                                    QStringLiteral("apre"),
                                    QStringLiteral("vpre")}) {
            QTest::newRow(qPrintable(name)) << name;
        }
    }

    void rejectsSideOutputProperties()
    {
        QFETCH(QString, propertyName);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString target = QDir(temporary.path()).filePath(QStringLiteral("render.mp4"));
        auto properties = safeConsumer(target);
        properties.insert(propertyName, QStringLiteral("outside"));
        QString error;
        QVERIFY(!McpExportTargetPolicy::validateConsumerProperties(target,
                                                                   false,
                                                                   0,
                                                                   properties,
                                                                   allowAll(),
                                                                   &error));
        QVERIFY(error.contains(QStringLiteral("manual export"), Qt::CaseInsensitive));
    }

    void rejectsCompoundSideOutputParameters_data()
    {
        QTest::addColumn<QString>("propertyName");
        QTest::addColumn<QString>("value");
        QTest::newRow("x265-stats")
            << QStringLiteral("x265-params") << QStringLiteral("stats=/tmp/out.log:keyint=50");
        QTest::newRow("x265-csv") << QStringLiteral("x265-params")
                                  << QStringLiteral("csv=/tmp/out.csv");
        QTest::newRow("x265-recon")
            << QStringLiteral("x265-params") << QStringLiteral("recon=/tmp/recon.yuv");
        QTest::newRow("x265-analysis")
            << QStringLiteral("x265-params") << QStringLiteral("analysis-save=/tmp/analysis.dat");
        QTest::newRow("x265-qpfile")
            << QStringLiteral("x265-params") << QStringLiteral("qpfile=/tmp/frames.qp");
        QTest::newRow("svt-stats")
            << QStringLiteral("svtav1-params") << QStringLiteral("stat-file=/tmp/out.log");
        QTest::newRow("svt-recon")
            << QStringLiteral("svtav1-params") << QStringLiteral("recon-file=/tmp/recon.yuv");
        QTest::newRow("x264-stats")
            << QStringLiteral("x264-params") << QStringLiteral("stats=/tmp/out.log");
        QTest::newRow("x265-bare-csv") << QStringLiteral("x265-params") << QStringLiteral("csv");
        QTest::newRow("x265-bare-stats")
            << QStringLiteral("x265-params") << QStringLiteral("keyint=50:stats");
        QTest::newRow("x264-bare-unknown")
            << QStringLiteral("x264-params") << QStringLiteral("bff=1:annexb");
        QTest::newRow("x264-semicolon-injection")
            << QStringLiteral("x264-params") << QStringLiteral("bff=1;stats=/tmp/out.log");
    }

    void rejectsCompoundSideOutputParameters()
    {
        QFETCH(QString, propertyName);
        QFETCH(QString, value);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString target = QDir(temporary.path()).filePath(QStringLiteral("render.mp4"));
        auto properties = safeConsumer(target);
        properties.insert(propertyName, value);
        QString error;
        QVERIFY(!McpExportTargetPolicy::validateConsumerProperties(target,
                                                                   false,
                                                                   0,
                                                                   properties,
                                                                   allowAll(),
                                                                   &error));
        QVERIFY(error.contains(QStringLiteral("codec parameter"), Qt::CaseInsensitive));
    }

    void validatesCoverArtAgainstAllowedRoots()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QDir directory(temporary.path());
        const QString target = directory.filePath(QStringLiteral("render.mp4"));
        const QString cover = directory.filePath(QStringLiteral("cover.png"));
        createFile(cover);
        auto properties = safeConsumer(target);
        properties.insert(QStringLiteral("attached_pic"), cover);
        QString error;
        QVERIFY(McpExportTargetPolicy::validateConsumerProperties(target,
                                                                  false,
                                                                  0,
                                                                  properties,
                                                                  allowAll(),
                                                                  &error));

        const auto rejectCover = [cover](const QString &path, bool mustExist) {
            return !mustExist || path != cover;
        };
        QVERIFY(!McpExportTargetPolicy::validateConsumerProperties(target,
                                                                   false,
                                                                   0,
                                                                   properties,
                                                                   rejectCover,
                                                                   &error));

        properties.insert(QStringLiteral("attached_pic"), temporary.path());
        QVERIFY(!McpExportTargetPolicy::validateConsumerProperties(target,
                                                                   false,
                                                                   0,
                                                                   properties,
                                                                   allowAll(),
                                                                   &error));
    }

    void validatesSingleFileTargets()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString target = QDir(temporary.path()).filePath(QStringLiteral("render.mp4"));
        QString error;
        QVERIFY(McpExportTargetPolicy::validateConsumerTarget(target,
                                                              target,
                                                              false,
                                                              false,
                                                              allowAll(),
                                                              &error));

        createFile(target);
        QVERIFY(!McpExportTargetPolicy::validateConsumerTarget(target,
                                                               target,
                                                               false,
                                                               false,
                                                               allowAll(),
                                                               &error));
        QVERIFY(error.contains(QStringLiteral("overwrite"), Qt::CaseInsensitive));
        QVERIFY(McpExportTargetPolicy::validateConsumerTarget(target,
                                                              target,
                                                              false,
                                                              true,
                                                              allowAll(),
                                                              &error));

        const QString directory = QDir(temporary.path()).filePath(QStringLiteral("folder.mp4"));
        QVERIFY(QDir().mkpath(directory));
        QVERIFY(!McpExportTargetPolicy::validateConsumerTarget(directory,
                                                               directory,
                                                               false,
                                                               true,
                                                               allowAll(),
                                                               &error));
        QVERIFY(error.contains(QStringLiteral("regular file"), Qt::CaseInsensitive));
    }

    void rejectsSingleFileSymlinks()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QDir directory(temporary.path());
        const QString source = directory.filePath(QStringLiteral("source.mp4"));
        const QString target = directory.filePath(QStringLiteral("render.mp4"));
        createFile(source);
        if (!QFile::link(source, target) || !QFileInfo(target).isSymLink())
            QSKIP("This platform does not permit creating a symbolic link in the test runner");

        QString error;
        QVERIFY(!McpExportTargetPolicy::validateConsumerTarget(target,
                                                               target,
                                                               false,
                                                               true,
                                                               allowAll(),
                                                               &error));
        QVERIFY(error.contains(QStringLiteral("symbolic link"), Qt::CaseInsensitive));
    }

    void requiresRequestedAndConsumerAuthorization()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QDir directory(temporary.path());
        const QString requested = directory.filePath(QStringLiteral("render.png"));
        const QString consumer = directory.filePath(QStringLiteral("render-%05d.png"));
        QString error;

        const auto rejectRequested = [requested](const QString &path, bool) {
            return path != requested;
        };
        QVERIFY(!McpExportTargetPolicy::validateConsumerTarget(requested,
                                                               consumer,
                                                               true,
                                                               true,
                                                               rejectRequested,
                                                               &error));
        QVERIFY(error.contains(QStringLiteral("outside allowed roots"), Qt::CaseInsensitive));

        const auto rejectConsumer = [consumer](const QString &path, bool) {
            return path != consumer;
        };
        QVERIFY(!McpExportTargetPolicy::validateConsumerTarget(requested,
                                                               consumer,
                                                               true,
                                                               true,
                                                               rejectConsumer,
                                                               &error));
        QVERIFY(error.contains(QStringLiteral("outside allowed roots"), Qt::CaseInsensitive));
    }

    void requiresTheExactGeneratedPattern()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString requested = QDir(temporary.path()).filePath(QStringLiteral("render.png"));
        const QString expected = QDir(temporary.path()).filePath(QStringLiteral("render-%05d.png"));
        QString error;
        QVERIFY(McpExportTargetPolicy::validateConsumerTarget(requested,
                                                              expected,
                                                              true,
                                                              false,
                                                              allowAll(),
                                                              &error));
        QVERIFY(!McpExportTargetPolicy::validateConsumerTarget(requested,
                                                               QDir(temporary.path())
                                                                   .filePath(QStringLiteral(
                                                                       "other-%05d.png")),
                                                               true,
                                                               false,
                                                               allowAll(),
                                                               &error));
        QVERIFY(error.contains(QStringLiteral("unexpected pattern"), Qt::CaseInsensitive));
    }

    void rejectsUserSuppliedImageSequenceTokens()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QDir directory(temporary.path());
        QString error;

        const QString requestedWithToken = directory.filePath(QStringLiteral("render%%.png"));
        const QFileInfo requestedInfo(requestedWithToken);
        const QString consumerWithToken = QStringLiteral("%1/%2-%05d.%3")
                                              .arg(requestedInfo.path(),
                                                   requestedInfo.baseName(),
                                                   requestedInfo.completeSuffix());
        QVERIFY(!McpExportTargetPolicy::validateConsumerTarget(requestedWithToken,
                                                               consumerWithToken,
                                                               true,
                                                               true,
                                                               allowAll(),
                                                               &error));
        QVERIFY(error.contains(QStringLiteral("frame-number token"), Qt::CaseInsensitive));

        const QString tokenDirectory = directory.filePath(QStringLiteral("frames%%"));
        QVERIFY(QDir().mkpath(tokenDirectory));
        const QString requestedInTokenDirectory
            = QDir(tokenDirectory).filePath(QStringLiteral("render.png"));
        const QFileInfo directoryRequestInfo(requestedInTokenDirectory);
        const QString consumerInTokenDirectory = QStringLiteral("%1/%2-%05d.%3")
                                                     .arg(directoryRequestInfo.path(),
                                                          directoryRequestInfo.baseName(),
                                                          directoryRequestInfo.completeSuffix());
        error.clear();
        QVERIFY(!McpExportTargetPolicy::validateConsumerTarget(requestedInTokenDirectory,
                                                               consumerInTokenDirectory,
                                                               true,
                                                               true,
                                                               allowAll(),
                                                               &error));
        QVERIFY(error.contains(QStringLiteral("frame-number token"), Qt::CaseInsensitive));
    }

    void findsAllPrintfSequenceMembers()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QDir directory(temporary.path());
        const QString requested = directory.filePath(QStringLiteral("render.png"));
        const QString pattern = directory.filePath(QStringLiteral("render-%05d.png"));
        createFile(directory.filePath(QStringLiteral("render-1234.png")));
        createFile(directory.filePath(QStringLiteral("render-100000.png")));

        QString error;
        QVERIFY(!McpExportTargetPolicy::validateConsumerTarget(requested,
                                                               pattern,
                                                               true,
                                                               false,
                                                               allowAll(),
                                                               &error));
        QVERIFY(error.contains(QStringLiteral("overwrite"), Qt::CaseInsensitive));
        QVERIFY(McpExportTargetPolicy::validateConsumerTarget(requested,
                                                              pattern,
                                                              true,
                                                              true,
                                                              allowAll(),
                                                              &error));

        QFile::remove(directory.filePath(QStringLiteral("render-100000.png")));
        createFile(directory.filePath(QStringLiteral("render--0001.png")));
        QVERIFY(!McpExportTargetPolicy::validateConsumerTarget(requested,
                                                               pattern,
                                                               true,
                                                               false,
                                                               allowAll(),
                                                               &error));
    }

    void rejectsNonRegularAndSymlinkMembers()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QDir directory(temporary.path());
        const QString requested = directory.filePath(QStringLiteral("render.png"));
        const QString pattern = directory.filePath(QStringLiteral("render-%05d.png"));
        const QString member = directory.filePath(QStringLiteral("render-00001.png"));
        QVERIFY(QDir().mkpath(member));

        QString error;
        QVERIFY(!McpExportTargetPolicy::validateConsumerTarget(requested,
                                                               pattern,
                                                               true,
                                                               true,
                                                               allowAll(),
                                                               &error));
        QVERIFY(error.contains(QStringLiteral("regular files"), Qt::CaseInsensitive));
        QVERIFY(QDir().rmdir(member));

        const QString source = directory.filePath(QStringLiteral("outside.png"));
        createFile(source);
        if (!QFile::link(source, member) || !QFileInfo(member).isSymLink())
            QSKIP("This platform does not permit creating a symbolic link in the test runner");
        QVERIFY(!McpExportTargetPolicy::validateConsumerTarget(requested,
                                                               pattern,
                                                               true,
                                                               true,
                                                               allowAll(),
                                                               &error));
        QVERIFY(error.contains(QStringLiteral("symbolic links"), Qt::CaseInsensitive));
    }

    void authorizesEveryExistingMember()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QDir directory(temporary.path());
        const QString requested = directory.filePath(QStringLiteral("render.png"));
        const QString pattern = directory.filePath(QStringLiteral("render-%05d.png"));
        const QString member = directory.filePath(QStringLiteral("render-00001.png"));
        createFile(member);

        const auto rejectMember = [member](const QString &path, bool mustExist) {
            return !mustExist || path != member;
        };
        QString error;
        QVERIFY(!McpExportTargetPolicy::validateConsumerTarget(requested,
                                                               pattern,
                                                               true,
                                                               true,
                                                               rejectMember,
                                                               &error));
        QVERIFY(error.contains(QStringLiteral("outside allowed roots"), Qt::CaseInsensitive));
    }

    void enforcesStreamingEnumerationLimits()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QDir directory(temporary.path());
        const QString requested = directory.filePath(QStringLiteral("render.png"));
        const QString pattern = directory.filePath(QStringLiteral("render-%05d.png"));
        createFile(directory.filePath(QStringLiteral("one.txt")));
        createFile(directory.filePath(QStringLiteral("two.txt")));

        McpExportTargetPolicy::EnumerationLimits limits;
        limits.maximumDirectoryEntries = 1;
        limits.maximumSequenceMembers = 10;
        QString error;
        QVERIFY(!McpExportTargetPolicy::validateConsumerTarget(
            requested, pattern, true, true, allowAll(), &error, limits));
        QVERIFY(error.contains(QStringLiteral("too many entries"), Qt::CaseInsensitive));

        QFile::remove(directory.filePath(QStringLiteral("one.txt")));
        QFile::remove(directory.filePath(QStringLiteral("two.txt")));
        createFile(directory.filePath(QStringLiteral("render-00001.png")));
        createFile(directory.filePath(QStringLiteral("render-00002.png")));
        limits.maximumDirectoryEntries = 10;
        limits.maximumSequenceMembers = 1;
        QVERIFY(!McpExportTargetPolicy::validateConsumerTarget(
            requested, pattern, true, true, allowAll(), &error, limits));
        QVERIFY(error.contains(QStringLiteral("too many existing members"), Qt::CaseInsensitive));
    }
};

QTEST_GUILESS_MAIN(TestMcpExportTargetPolicy)
#include "test_mcpexporttargetpolicy.moc"
