/*
 * Copyright (c) 2026 Meltytech, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "mcpxmlpathvalidator.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest/QtTest>

namespace {
bool writeFile(const QString &fileName, const QByteArray &data = QByteArray("fixture"))
{
    if (!QDir().mkpath(QFileInfo(fileName).absolutePath()))
        return false;
    QFile file(fileName);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(data) == data.size();
}

QString xmlProperty(const QString &name, const QString &value)
{
    return QStringLiteral("<property name=\"%1\">%2</property>")
        .arg(name.toHtmlEscaped(), value.toHtmlEscaped());
}

QString projectXml(const QString &element,
                   const QString &service,
                   const QString &propertyName,
                   const QString &propertyValue,
                   const QString &filterId = QString(),
                   const QString &root = QString())
{
    QString properties = xmlProperty(propertyName, propertyValue);
    properties += xmlProperty(QStringLiteral("mlt_service"), service);
    if (!filterId.isEmpty())
        properties += xmlProperty(QStringLiteral("shotcut:filter"), filterId);
    const QString rootAttribute = root.isEmpty()
                                      ? QString()
                                      : QStringLiteral(" root=\"%1\"").arg(root.toHtmlEscaped());
    return QStringLiteral("<mlt%1><%2>%3</%2></mlt>").arg(rootAttribute, element, properties);
}

McpXmlPathValidator validatorForRootWithServices(
    const QString &root, McpXmlPathValidator::ServiceAuthorizer authorizeService)
{
    const QString safeRoot = QDir::fromNativeSeparators(QFileInfo(root).canonicalFilePath());
    return McpXmlPathValidator(
        [safeRoot](const QString &path, bool mustExist, QString *normalized) {
            const QFileInfo info(path);
            if (!info.isAbsolute() || (mustExist && !info.exists()))
                return false;
            if (info.isSymLink() && !info.exists())
                return false;

            QString candidate;
            if (info.exists()) {
                candidate = QDir::fromNativeSeparators(info.canonicalFilePath());
            } else {
                const QString parent = QDir::fromNativeSeparators(
                    QFileInfo(info.absolutePath()).canonicalFilePath());
                if (parent.isEmpty())
                    return false;
                candidate = QDir::cleanPath(parent + QLatin1Char('/') + info.fileName());
            }

            QString prefix = safeRoot;
            if (!prefix.endsWith(QLatin1Char('/')))
                prefix.append(QLatin1Char('/'));
#ifdef Q_OS_WIN
            constexpr auto comparison = Qt::CaseInsensitive;
#else
            constexpr auto comparison = Qt::CaseSensitive;
#endif
            if (candidate.compare(safeRoot, comparison) != 0
                && !candidate.startsWith(prefix, comparison)) {
                return false;
            }
            if (normalized)
                *normalized = candidate;
            return true;
        },
        authorizeService);
}

McpXmlPathValidator validatorForRoot(const QString &root)
{
    return validatorForRootWithServices(root, [](const QString &, const QString &, const QString &) {
        return true;
    });
}
} // namespace

class TestMcpXmlPathValidator : public QObject
{
    Q_OBJECT

private slots:
    void acceptsRelativeResourceInsideRoot();
    void buffersServiceAndResourceRegardlessOfOrder();
    void rejectsAbsoluteResourceOutsideRoot();
    void acceptsFileUrlInsideRoot();
    void rejectsFileUrlOutsideRoot();
    void rejectsNetworkUrl();
    void rejectsNetworkFileReferences();
    void rejectsNestedProjectEscape();
    void rejectsNestedXmlServiceEscape();
    void rejectsPaddedExplicitXmlLoader();
    void rejectsAlternateLoaderEscape();
    void rejectsRelativeMltRoot();
    void acceptsAbsoluteMltRoot();
    void rejectsMltRootOutsideRoot();
    void acceptsContainedGenericXmlResource();
    void acceptsImageSequenceInsideRoot();
    void rejectsImageSequenceSymlinkEscape();
    void rejectsMalformedImageSequences();
    void rejectsAlternateImageSequenceSyntax();
    void rejectsGlobAndQueryDecorations();
    void acceptsKnownPseudoResources();
    void allowsQtextArgument();
    void rejectsRichTextAndWebVfxExternalContent();
    void rejectsPlainAndTrimmedWebVfx();
    void rejectsOpaqueManifestInsertion();
    void rejectsDisguisedAndSequenceManifests();
    void rejectsQualifiedPathFields_data();
    void rejectsQualifiedPathFields();
    void rejectsRelativeTransitionExtraPaths();
    void rejectsProxyOriginalEscape();
    void rejectsPluginRelativePath();
    void rejectsPlaceboShaderPaths();
    void rejectsMismatchedFilterIdentity();
    void rejectsActiveFilterServices();
    void rejectsActiveContentFilterServices();
    void rejectsUnsupportedAvfilterServices();
    void restrictsSupportedAvfilterOptionsAndLuts();
    void rejectsActiveNestedMaskFilter();
    void restrictsMaskApplyTransitionFactoryAndPaths();
    void restrictsAffineBackgroundFactoryAndPaths();
    void restrictsDustAndTransitionProducerFactories();
    void rejectsKnownActiveProducerLoaders();
    void rejectsAepxActiveContent();
    void acceptsAttributeValuedProperties();
    void rejectsAttributeValuedProperties();
    void rejectsDtdAndSecondaryRoot();
    void rejectsUnsafeMltElementsAndProfiles();
    void rejectsParameterizedMltReference();
    void rejectsUnknownProducerLoader();
    void rejectsUnauthorizedPluginServices();
    void rejectsDuplicateSecurityDiscriminators();
    void rejectsMutualProjectCycle();
    void rejectsOversizedProject();
};

void TestMcpXmlPathValidator::acceptsRelativeResourceInsideRoot()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString media = QDir(root.path()).filePath(QStringLiteral("media/clip.mp4"));
    QVERIFY(writeFile(media));
    const QString project = QDir(root.path()).filePath(QStringLiteral("project.mlt"));
    QVERIFY(writeFile(project,
                      projectXml(QStringLiteral("producer"),
                                 QStringLiteral("avformat"),
                                 QStringLiteral("resource"),
                                 QStringLiteral("media/clip.mp4"))
                          .toUtf8()));

    QString error;
    QVERIFY2(validatorForRoot(root.path()).validateProject(project, &error), qPrintable(error));
}

void TestMcpXmlPathValidator::buffersServiceAndResourceRegardlessOfOrder()
{
    QTemporaryDir root;
    QTemporaryDir outside;
    QVERIFY(root.isValid());
    QVERIFY(outside.isValid());
    const QString media = QDir(outside.path()).filePath(QStringLiteral("outside.mp4"));
    QVERIFY(writeFile(media));
    const QString project = QDir(root.path()).filePath(QStringLiteral("ordered.mlt"));
    const QString xml = QStringLiteral("<mlt><producer>%1%2</producer></mlt>")
                            .arg(xmlProperty(QStringLiteral("resource"), media),
                                 xmlProperty(QStringLiteral("mlt_service"),
                                             QStringLiteral("avformat")));
    QVERIFY(writeFile(project, xml.toUtf8()));

    QString error;
    QVERIFY(!validatorForRoot(root.path()).validateProject(project, &error));
    QVERIFY(!error.isEmpty());
}

void TestMcpXmlPathValidator::rejectsAbsoluteResourceOutsideRoot()
{
    QTemporaryDir root;
    QTemporaryDir outside;
    QVERIFY(root.isValid());
    QVERIFY(outside.isValid());
    const QString media = QDir(outside.path()).filePath(QStringLiteral("secret.png"));
    QVERIFY(writeFile(media));
    const QString project = QDir(root.path()).filePath(QStringLiteral("outside.mlt"));
    QVERIFY(writeFile(project,
                      projectXml(QStringLiteral("producer"),
                                 QStringLiteral("qimage"),
                                 QStringLiteral("resource"),
                                 media)
                          .toUtf8()));

    QString error;
    QVERIFY(!validatorForRoot(root.path()).validateProject(project, &error));
    QVERIFY(error.contains(QStringLiteral("outside allowed roots")));
}

void TestMcpXmlPathValidator::acceptsFileUrlInsideRoot()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString media = QDir(root.path()).filePath(QStringLiteral("inside.png"));
    QVERIFY(writeFile(media));
    const QString project = QDir(root.path()).filePath(QStringLiteral("file-url.mlt"));
    QVERIFY(writeFile(project,
                      projectXml(QStringLiteral("producer"),
                                 QStringLiteral("qimage"),
                                 QStringLiteral("resource"),
                                 QUrl::fromLocalFile(media).toString())
                          .toUtf8()));

    QString error;
    QVERIFY2(validatorForRoot(root.path()).validateProject(project, &error), qPrintable(error));
}

void TestMcpXmlPathValidator::rejectsFileUrlOutsideRoot()
{
    QTemporaryDir root;
    QTemporaryDir outside;
    QVERIFY(root.isValid());
    QVERIFY(outside.isValid());
    const QString media = QDir(outside.path()).filePath(QStringLiteral("outside.png"));
    QVERIFY(writeFile(media));
    const QString project = QDir(root.path()).filePath(QStringLiteral("file-url-outside.mlt"));
    QVERIFY(writeFile(project,
                      projectXml(QStringLiteral("producer"),
                                 QStringLiteral("qimage"),
                                 QStringLiteral("resource"),
                                 QUrl::fromLocalFile(media).toString())
                          .toUtf8()));

    QString error;
    QVERIFY(!validatorForRoot(root.path()).validateProject(project, &error));
    QVERIFY(!error.isEmpty());
}

void TestMcpXmlPathValidator::rejectsNetworkUrl()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString project = QDir(root.path()).filePath(QStringLiteral("network.mlt"));
    QVERIFY(writeFile(project,
                      projectXml(QStringLiteral("producer"),
                                 QStringLiteral("avformat"),
                                 QStringLiteral("resource"),
                                 QStringLiteral("HTTPS://127.0.0.1/private.m3u8"))
                          .toUtf8()));

    QString error;
    QVERIFY(!validatorForRoot(root.path()).validateProject(project, &error));
    QVERIFY(error.contains(QStringLiteral("not allowed")));
}

void TestMcpXmlPathValidator::rejectsNestedProjectEscape()
{
    QTemporaryDir root;
    QTemporaryDir outside;
    QVERIFY(root.isValid());
    QVERIFY(outside.isValid());
    const QString media = QDir(outside.path()).filePath(QStringLiteral("nested-secret.mp4"));
    QVERIFY(writeFile(media));
    const QString nested = QDir(root.path()).filePath(QStringLiteral("nested.mlt"));
    QVERIFY(writeFile(nested,
                      projectXml(QStringLiteral("producer"),
                                 QStringLiteral("avformat"),
                                 QStringLiteral("resource"),
                                 media)
                          .toUtf8()));
    const QString project = QDir(root.path()).filePath(QStringLiteral("outer.mlt"));
    QVERIFY(writeFile(project,
                      projectXml(QStringLiteral("producer"),
                                 QStringLiteral("xml"),
                                 QStringLiteral("resource"),
                                 QStringLiteral("nested.mlt"))
                          .toUtf8()));

    QString error;
    QVERIFY(!validatorForRoot(root.path()).validateProject(project, &error));
    QVERIFY(!error.isEmpty());
}

void TestMcpXmlPathValidator::rejectsRelativeMltRoot()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString media = QDir(root.path()).filePath(QStringLiteral("assets/clip.mp4"));
    QVERIFY(writeFile(media));
    const QString project = QDir(root.path()).filePath(QStringLiteral("rooted.mlt"));
    QVERIFY(writeFile(project,
                      projectXml(QStringLiteral("producer"),
                                 QStringLiteral("avformat"),
                                 QStringLiteral("resource"),
                                 QStringLiteral("clip.mp4"),
                                 QString(),
                                 QStringLiteral("assets"))
                          .toUtf8()));

    QString error;
    QVERIFY(!validatorForRoot(root.path()).validateProject(project, &error));
    QVERIFY(error.contains(QStringLiteral("absolute")));
}

void TestMcpXmlPathValidator::rejectsMltRootOutsideRoot()
{
    QTemporaryDir root;
    QTemporaryDir outside;
    QVERIFY(root.isValid());
    QVERIFY(outside.isValid());
    const QString project = QDir(root.path()).filePath(QStringLiteral("bad-root.mlt"));
    QVERIFY(writeFile(project,
                      projectXml(QStringLiteral("producer"),
                                 QStringLiteral("color"),
                                 QStringLiteral("resource"),
                                 QStringLiteral("black"),
                                 QString(),
                                 outside.path())
                          .toUtf8()));

    QString error;
    QVERIFY(!validatorForRoot(root.path()).validateProject(project, &error));
    QVERIFY(!error.isEmpty());
}

void TestMcpXmlPathValidator::acceptsImageSequenceInsideRoot()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(QDir().mkpath(QDir(root.path()).filePath(QStringLiteral("frames"))));
    QVERIFY(writeFile(QDir(root.path()).filePath(QStringLiteral("frames/frame-00001.png"))));
    const QString project = QDir(root.path()).filePath(QStringLiteral("sequence.mlt"));
    QVERIFY(writeFile(project,
                      projectXml(QStringLiteral("producer"),
                                 QStringLiteral("qimage"),
                                 QStringLiteral("resource"),
                                 QStringLiteral("frames/frame-%05d.png"))
                          .toUtf8()));

    QString error;
    QVERIFY2(validatorForRoot(root.path()).validateProject(project, &error), qPrintable(error));
}

void TestMcpXmlPathValidator::acceptsKnownPseudoResources()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString project = QDir(root.path()).filePath(QStringLiteral("pseudo.mlt"));
    QString xml = QStringLiteral("<mlt>");
    xml += projectXml(QStringLiteral("producer"),
                      QStringLiteral("color"),
                      QStringLiteral("resource"),
                      QStringLiteral("#000000"))
               .mid(5)
               .chopped(6);
    xml += projectXml(QStringLiteral("producer"),
                      QStringLiteral("luma"),
                      QStringLiteral("resource"),
                      QStringLiteral("%luma01.pgm"))
               .mid(5)
               .chopped(6);
    xml += projectXml(QStringLiteral("producer"),
                      QStringLiteral("tractor"),
                      QStringLiteral("resource"),
                      QStringLiteral("<producer>"))
               .mid(5)
               .chopped(6);
    xml += QStringLiteral("</mlt>");
    QVERIFY(writeFile(project, xml.toUtf8()));

    QString error;
    QVERIFY2(validatorForRoot(root.path()).validateProject(project, &error), qPrintable(error));
}

void TestMcpXmlPathValidator::rejectsRichTextAndWebVfxExternalContent()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString asset = QDir(root.path()).filePath(QStringLiteral("asset.html"));
    QVERIFY(writeFile(asset, QByteArray("<html></html>")));

    const QString richText = QDir(root.path()).filePath(QStringLiteral("rich.mlt"));
    QVERIFY(writeFile(richText,
                      projectXml(QStringLiteral("filter"),
                                 QStringLiteral("qtext"),
                                 QStringLiteral("html"),
                                 QStringLiteral("<img src=\"asset.png\">"),
                                 QStringLiteral("qtext"))
                          .toUtf8()));
    QString error;
    QVERIFY(!validatorForRoot(root.path()).validateProject(richText, &error));

    const QString webvfx = QDir(root.path()).filePath(QStringLiteral("webvfx.mlt"));
    QVERIFY(writeFile(webvfx,
                      projectXml(QStringLiteral("filter"),
                                 QStringLiteral("webvfx"),
                                 QStringLiteral("resource"),
                                 asset)
                          .toUtf8()));
    error.clear();
    QVERIFY(!validatorForRoot(root.path()).validateProject(webvfx, &error));
}

void TestMcpXmlPathValidator::rejectsOpaqueManifestInsertion()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString manifest = QDir(root.path()).filePath(QStringLiteral("PLAYLIST.M3U8"));
    QVERIFY(writeFile(manifest, QByteArray("#EXTM3U\nhttps://127.0.0.1/segment.ts\n")));

    QString error;
    QVERIFY(!validatorForRoot(root.path()).validateMedia(manifest, &error));
    QVERIFY(error.contains(QStringLiteral("manifest")));
}

void TestMcpXmlPathValidator::acceptsAbsoluteMltRoot()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString assets = QDir(root.path()).filePath(QStringLiteral("assets"));
    const QString media = QDir(assets).filePath(QStringLiteral("clip.mp4"));
    QVERIFY(writeFile(media));
    const QString canonicalAssets = QFileInfo(assets).canonicalFilePath();
    QVERIFY(!canonicalAssets.isEmpty());
    const QString project = QDir(root.path()).filePath(QStringLiteral("absolute-root.mlt"));
    QVERIFY(writeFile(project,
                      projectXml(QStringLiteral("producer"),
                                 QStringLiteral("avformat"),
                                 QStringLiteral("resource"),
                                 QStringLiteral("clip.mp4"),
                                 QString(),
                                 canonicalAssets)
                          .toUtf8()));

    QString error;
    QVERIFY2(validatorForRoot(root.path()).validateProject(project, &error), qPrintable(error));
}

void TestMcpXmlPathValidator::acceptsContainedGenericXmlResource()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString config = QDir(root.path()).filePath(QStringLiteral("filter-config.xml"));
    QVERIFY(writeFile(config, QByteArray("<?xml version=\"1.0\"?><configuration/>")));
    const QString project = QDir(root.path()).filePath(QStringLiteral("generic-xml.mlt"));
    QVERIFY(writeFile(project,
                      projectXml(QStringLiteral("filter"),
                                 QStringLiteral("custom.filter"),
                                 QStringLiteral("resource"),
                                 QStringLiteral("filter-config.xml"))
                          .toUtf8()));

    QString error;
    QVERIFY2(validatorForRoot(root.path()).validateProject(project, &error), qPrintable(error));
}

void TestMcpXmlPathValidator::rejectsNestedXmlServiceEscape()
{
    QTemporaryDir root;
    QTemporaryDir outside;
    QVERIFY(root.isValid());
    QVERIFY(outside.isValid());
    const QString secret = QDir(outside.path()).filePath(QStringLiteral("secret.mp4"));
    QVERIFY(writeFile(secret));
    const QString nested = QDir(root.path()).filePath(QStringLiteral("nested.xml"));
    QVERIFY(writeFile(nested,
                      projectXml(QStringLiteral("producer"),
                                 QStringLiteral("avformat"),
                                 QStringLiteral("resource"),
                                 secret)
                          .toUtf8()));
    const QString project = QDir(root.path()).filePath(QStringLiteral("outer-xml.mlt"));
    QVERIFY(writeFile(project,
                      projectXml(QStringLiteral("producer"),
                                 QStringLiteral("xml"),
                                 QStringLiteral("resource"),
                                 QStringLiteral("nested.xml"))
                          .toUtf8()));

    QString error;
    QVERIFY(!validatorForRoot(root.path()).validateProject(project, &error));
    QVERIFY(!error.isEmpty());
}

void TestMcpXmlPathValidator::rejectsAlternateLoaderEscape()
{
    QTemporaryDir root;
    QTemporaryDir outside;
    QVERIFY(root.isValid());
    QVERIFY(outside.isValid());
    const QString secret = QDir(outside.path()).filePath(QStringLiteral("alternate-secret.mp4"));
    QVERIFY(writeFile(secret));
    const QString nested = QDir(root.path()).filePath(QStringLiteral("nested.kdenlive"));
    QVERIFY(writeFile(nested,
                      projectXml(QStringLiteral("producer"),
                                 QStringLiteral("avformat"),
                                 QStringLiteral("resource"),
                                 secret)
                          .toUtf8()));
    const QString project = QDir(root.path()).filePath(QStringLiteral("outer-alternate.mlt"));
    QVERIFY(writeFile(project,
                      projectXml(QStringLiteral("producer"),
                                 QStringLiteral("avformat"),
                                 QStringLiteral("resource"),
                                 QStringLiteral("nested.kdenlive"))
                          .toUtf8()));

    QString error;
    QVERIFY(!validatorForRoot(root.path()).validateProject(project, &error));
}

void TestMcpXmlPathValidator::rejectsNetworkFileReferences()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto validator = validatorForRoot(root.path());
    QString error;

    const QString fileUrlProject = QDir(root.path()).filePath(QStringLiteral("remote-file-url.mlt"));
    QVERIFY(writeFile(fileUrlProject,
                      projectXml(QStringLiteral("producer"),
                                 QStringLiteral("avformat"),
                                 QStringLiteral("resource"),
                                 QStringLiteral("file://example.invalid/share/clip.mp4"))
                          .toUtf8()));
    QVERIFY(!validator.validateProject(fileUrlProject, &error));
    QVERIFY(error.contains(QStringLiteral("network"), Qt::CaseInsensitive));

    const QString uncProject = QDir(root.path()).filePath(QStringLiteral("unc.mlt"));
    QVERIFY(writeFile(uncProject,
                      projectXml(QStringLiteral("producer"),
                                 QStringLiteral("avformat"),
                                 QStringLiteral("resource"),
                                 QStringLiteral("//example.invalid/share/clip.mp4"))
                          .toUtf8()));
    error.clear();
    QVERIFY(!validator.validateProject(uncProject, &error));
    QVERIFY(error.contains(QStringLiteral("network"), Qt::CaseInsensitive));
}

void TestMcpXmlPathValidator::rejectsMutualProjectCycle()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString projectA = QDir(root.path()).filePath(QStringLiteral("a.mlt"));
    const QString projectB = QDir(root.path()).filePath(QStringLiteral("b.mlt"));
    QVERIFY(writeFile(projectA,
                      projectXml(QStringLiteral("producer"),
                                 QStringLiteral("xml"),
                                 QStringLiteral("resource"),
                                 QStringLiteral("b.mlt"))
                          .toUtf8()));
    QVERIFY(writeFile(projectB,
                      projectXml(QStringLiteral("producer"),
                                 QStringLiteral("xml"),
                                 QStringLiteral("resource"),
                                 QStringLiteral("a.mlt"))
                          .toUtf8()));

    QString error;
    QVERIFY(!validatorForRoot(root.path()).validateProject(projectA, &error));
    QVERIFY(error.contains(QStringLiteral("cycle")));
}

void TestMcpXmlPathValidator::rejectsOversizedProject()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString project = QDir(root.path()).filePath(QStringLiteral("oversized.mlt"));
    QFile file(project);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(file.resize(64LL * 1024 * 1024 + 1));
    file.close();

    QString error;
    QVERIFY(!validatorForRoot(root.path()).validateProject(project, &error));
    QVERIFY(error.contains(QStringLiteral("size")));
}

void TestMcpXmlPathValidator::allowsQtextArgument()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString project = QDir(root.path()).filePath(QStringLiteral("qtext-argument.mlt"));
    QVERIFY(writeFile(project,
                      projectXml(QStringLiteral("filter"),
                                 QStringLiteral("qtext"),
                                 QStringLiteral("argument"),
                                 QStringLiteral("ordinary title text"),
                                 QStringLiteral("qtext"))
                          .toUtf8()));

    QString error;
    QVERIFY2(validatorForRoot(root.path()).validateProject(project, &error), qPrintable(error));
}

void TestMcpXmlPathValidator::rejectsPlainAndTrimmedWebVfx()
{
    QTemporaryDir root;
    QTemporaryDir outside;
    QVERIFY(root.isValid());
    QVERIFY(outside.isValid());
    const QString outsideHtml = QDir(outside.path()).filePath(QStringLiteral("outside.html"));
    QVERIFY(writeFile(outsideHtml, QByteArray("<html></html>")));
    const QString project = QDir(root.path()).filePath(QStringLiteral("plain-webvfx.mlt"));
    QVERIFY(writeFile(project,
                      projectXml(QStringLiteral("producer"),
                                 QStringLiteral(" WeBvFx "),
                                 QStringLiteral("resource"),
                                 QStringLiteral("plain:") + outsideHtml)
                          .toUtf8()));

    QString error;
    QVERIFY(!validatorForRoot(root.path()).validateProject(project, &error));
    QVERIFY(error.contains(QStringLiteral("WebVfx")));
}

void TestMcpXmlPathValidator::rejectsDisguisedAndSequenceManifests()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto validator = validatorForRoot(root.path());
    const QString disguised = QDir(root.path()).filePath(QStringLiteral("renamed.mp4"));
    QVERIFY(writeFile(disguised, QByteArray("#EXTM3U\nhttps://127.0.0.1/segment.ts\n")));
    QString error;
    QVERIFY(!validator.validateMedia(disguised, &error));
    QVERIFY(error.contains(QStringLiteral("content")));

    const QString project = QDir(root.path()).filePath(QStringLiteral("sequence-manifest.mlt"));
    QVERIFY(writeFile(project,
                      projectXml(QStringLiteral("producer"),
                                 QStringLiteral("qimage"),
                                 QStringLiteral("resource"),
                                 QStringLiteral("frames/frame-%05d.m3u8"))
                          .toUtf8()));
    error.clear();
    QVERIFY(!validator.validateProject(project, &error));
    QVERIFY(error.contains(QStringLiteral("image sequences"), Qt::CaseInsensitive));
}

void TestMcpXmlPathValidator::rejectsQualifiedPathFields_data()
{
    QTest::addColumn<QString>("element");
    QTest::addColumn<QString>("service");
    QTest::addColumn<QString>("propertyName");
    QTest::newRow("legacy-video-src")
        << QStringLiteral("video") << QStringLiteral("qimage") << QStringLiteral("src");
    QTest::newRow("timewarp-argument")
        << QStringLiteral("producer") << QStringLiteral("timewarp") << QStringLiteral("argument");
    QTest::newRow("filter-av-filename")
        << QStringLiteral("filter") << QStringLiteral("avfilter.test")
        << QStringLiteral("av.filename");
    QTest::newRow("filter-resource") << QStringLiteral("filter") << QStringLiteral("maskfromfile")
                                     << QStringLiteral("filter.resource");
    QTest::newRow("filter-src") << QStringLiteral("filter") << QStringLiteral("custom.filter")
                                << QStringLiteral("src");
    QTest::newRow("transition-src") << QStringLiteral("transition")
                                    << QStringLiteral("custom.transition") << QStringLiteral("src");
    QTest::newRow("transition-filename")
        << QStringLiteral("transition") << QStringLiteral("custom.transition")
        << QStringLiteral("filename");
    QTest::newRow("transition-av-file")
        << QStringLiteral("transition") << QStringLiteral("custom.transition")
        << QStringLiteral("av.file");
}

void TestMcpXmlPathValidator::rejectsQualifiedPathFields()
{
    QFETCH(QString, element);
    QFETCH(QString, service);
    QFETCH(QString, propertyName);
    QTemporaryDir root;
    QTemporaryDir outside;
    QVERIFY(root.isValid());
    QVERIFY(outside.isValid());
    const QString secret = QDir(outside.path()).filePath(QStringLiteral("qualified-secret.dat"));
    QVERIFY(writeFile(secret));
    const QString project = QDir(root.path()).filePath(QStringLiteral("qualified-path.mlt"));
    QVERIFY(writeFile(project, projectXml(element, service, propertyName, secret).toUtf8()));

    QString error;
    QVERIFY(!validatorForRoot(root.path()).validateProject(project, &error));
    QVERIFY(!error.isEmpty());
}

void TestMcpXmlPathValidator::rejectsProxyOriginalEscape()
{
    QTemporaryDir root;
    QTemporaryDir outside;
    QVERIFY(root.isValid());
    QVERIFY(outside.isValid());
    const QString proxy = QDir(root.path()).filePath(QStringLiteral("proxy.mp4"));
    const QString original = QDir(outside.path()).filePath(QStringLiteral("original.mp4"));
    QVERIFY(writeFile(proxy));
    QVERIFY(writeFile(original));
    const QStringList proxyValues{QStringLiteral("1"),
                                  QStringLiteral("2"),
                                  QStringLiteral("-1"),
                                  QStringLiteral("01"),
                                  QString()};
    for (qsizetype i = 0; i < proxyValues.size(); ++i) {
        QString properties = xmlProperty(QStringLiteral("resource"), proxy);
        properties += xmlProperty(QStringLiteral("shotcut:resource"), original);
        if (!proxyValues.at(i).isNull())
            properties += xmlProperty(QStringLiteral("shotcut:proxy"), proxyValues.at(i));
        properties += xmlProperty(QStringLiteral("mlt_service"), QStringLiteral("avformat"));
        const QString xml = QStringLiteral("<mlt><producer>%1</producer></mlt>").arg(properties);
        const QString project
            = QDir(root.path()).filePath(QStringLiteral("proxy-original-%1.mlt").arg(i));
        QVERIFY(writeFile(project, xml.toUtf8()));

        QString error;
        QVERIFY(!validatorForRoot(root.path()).validateProject(project, &error));
        QVERIFY(!error.isEmpty());
    }
}

void TestMcpXmlPathValidator::rejectsPluginRelativePath()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString data = QDir(root.path()).filePath(QStringLiteral("track.gpx"));
    QVERIFY(writeFile(data));
    const QString project = QDir(root.path()).filePath(QStringLiteral("plugin-relative.mlt"));
    QVERIFY(writeFile(project,
                      projectXml(QStringLiteral("filter"),
                                 QStringLiteral("gpstext"),
                                 QStringLiteral("gps.file"),
                                 QStringLiteral("track.gpx"))
                          .toUtf8()));

    QString error;
    QVERIFY(!validatorForRoot(root.path()).validateProject(project, &error));
    QVERIFY(error.contains(QStringLiteral("absolute")));
}

void TestMcpXmlPathValidator::rejectsDtdAndSecondaryRoot()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto validator = validatorForRoot(root.path());
    const QString dtdProject = QDir(root.path()).filePath(QStringLiteral("dtd.mlt"));
    QVERIFY(writeFile(dtdProject,
                      QByteArray("<!DOCTYPE mlt [<!ENTITY injected SYSTEM \"file:///outside\">]>")
                          + QByteArray("<mlt><producer resource=\"&injected;\"/></mlt>")));
    QString error;
    QVERIFY(!validator.validateProject(dtdProject, &error));
    QVERIFY(error.contains(QStringLiteral("DTD")));

    const QString nestedRootProject
        = QDir(root.path()).filePath(QStringLiteral("secondary-root.mlt"));
    QVERIFY(writeFile(nestedRootProject,
                      QByteArray("<mlt><playlist><mlt root=\"/outside\"/></playlist></mlt>")));
    error.clear();
    QVERIFY(!validator.validateProject(nestedRootProject, &error));
    QVERIFY(error.contains(QStringLiteral("secondary")));
}

void TestMcpXmlPathValidator::rejectsUnsafeMltElementsAndProfiles()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto validator = validatorForRoot(root.path());
    const QString consumerProject = QDir(root.path()).filePath(QStringLiteral("consumer.mlt"));
    QVERIFY(writeFile(consumerProject,
                      QByteArray("<mlt><consumer mlt_service=\"avformat\" ")
                          + QByteArray("target=\"file:///outside.mp4\"/></mlt>")));
    QString error;
    QVERIFY(!validator.validateProject(consumerProject, &error));
    QVERIFY(error.contains(QStringLiteral("consumer")));

    const QString linkProject = QDir(root.path()).filePath(QStringLiteral("link.mlt"));
    QVERIFY(writeFile(linkProject,
                      QByteArray("<mlt><chain><link mlt_service=\"timeremap\"/>")
                          + QByteArray("</chain></mlt>")));
    error.clear();
    QVERIFY(!validator.validateProject(linkProject, &error));
    QVERIFY(error.contains(QStringLiteral("link")));

    const QString profileProject = QDir(root.path()).filePath(QStringLiteral("profile.mlt"));
    QVERIFY(writeFile(profileProject,
                      QByteArray("<mlt profile=\"../../outside-profile\">")
                          + QByteArray("<playlist/></mlt>")));
    error.clear();
    QVERIFY(!validator.validateProject(profileProject, &error));
    QVERIFY(error.contains(QStringLiteral("profile")));
}

void TestMcpXmlPathValidator::rejectsParameterizedMltReference()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString nested = QDir(root.path()).filePath(QStringLiteral("nested.xml"));
    QVERIFY(writeFile(nested, QByteArray("<mlt><playlist/></mlt>")));
    const QString project = QDir(root.path()).filePath(QStringLiteral("parameterized.mlt"));
    QVERIFY(writeFile(project,
                      projectXml(QStringLiteral("producer"),
                                 QStringLiteral("xml"),
                                 QStringLiteral("resource"),
                                 QStringLiteral("nested.xml?terminate_on_pause=1"))
                          .toUtf8()));

    QString error;
    QVERIFY(!validatorForRoot(root.path()).validateProject(project, &error));
    QVERIFY(error.contains(QStringLiteral("query"), Qt::CaseInsensitive));
}

void TestMcpXmlPathValidator::rejectsPaddedExplicitXmlLoader()
{
    QTemporaryDir root;
    QTemporaryDir outside;
    QVERIFY(root.isValid());
    QVERIFY(outside.isValid());
    const QString secret = QDir(outside.path()).filePath(QStringLiteral("padded-secret.mp4"));
    QVERIFY(writeFile(secret));
    QByteArray nestedXml("<!--");
    nestedXml.append(20 * 1024, 'x');
    nestedXml.append("-->");
    nestedXml.append(projectXml(QStringLiteral("producer"),
                                QStringLiteral("avformat"),
                                QStringLiteral("resource"),
                                secret)
                         .toUtf8());
    const QString nested = QDir(root.path()).filePath(QStringLiteral("padded.mp4"));
    QVERIFY(writeFile(nested, nestedXml));
    const QString project = QDir(root.path()).filePath(QStringLiteral("padded-outer.mlt"));
    QVERIFY(writeFile(project,
                      projectXml(QStringLiteral("producer"),
                                 QStringLiteral("xml"),
                                 QStringLiteral("resource"),
                                 QStringLiteral("padded.mp4"))
                          .toUtf8()));

    QString error;
    QVERIFY(!validatorForRoot(root.path()).validateProject(project, &error));
    QVERIFY(!error.isEmpty());
}

void TestMcpXmlPathValidator::rejectsUnknownProducerLoader()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString media = QDir(root.path()).filePath(QStringLiteral("inside.bin"));
    QVERIFY(writeFile(media));
    const QString project = QDir(root.path()).filePath(QStringLiteral("unknown-loader.mlt"));
    QVERIFY(writeFile(project,
                      projectXml(QStringLiteral("producer"),
                                 QStringLiteral("mystery_active_loader"),
                                 QStringLiteral("resource"),
                                 QStringLiteral("inside.bin"))
                          .toUtf8()));

    QString error;
    QVERIFY(!validatorForRoot(root.path()).validateProject(project, &error));
    QVERIFY(error.contains(QStringLiteral("loader")));
}

void TestMcpXmlPathValidator::rejectsUnauthorizedPluginServices()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto validator = validatorForRootWithServices(
        root.path(), [](const QString &element, const QString &service, const QString &filterId) {
            if (element.compare(QStringLiteral("filter"), Qt::CaseInsensitive) == 0) {
                return service.compare(QStringLiteral("qtext"), Qt::CaseInsensitive) == 0
                       && (filterId.isEmpty()
                           || filterId.compare(QStringLiteral("qtext"), Qt::CaseInsensitive) == 0);
            }
            if (element.compare(QStringLiteral("transition"), Qt::CaseInsensitive) == 0) {
                return service.compare(QStringLiteral("mix"), Qt::CaseInsensitive) == 0;
            }
            return true;
        });

    const QString allowed = QDir(root.path()).filePath(QStringLiteral("allowed-filter.mlt"));
    QVERIFY(writeFile(allowed,
                      projectXml(QStringLiteral("filter"),
                                 QStringLiteral("qtext"),
                                 QStringLiteral("argument"),
                                 QStringLiteral("plain title"),
                                 QStringLiteral("qtext"))
                          .toUtf8()));
    QString error;
    QVERIFY2(validator.validateProject(allowed, &error), qPrintable(error));

    const QString unknownFilter = QDir(root.path()).filePath(QStringLiteral("unknown-filter.mlt"));
    QVERIFY(writeFile(unknownFilter,
                      projectXml(QStringLiteral("filter"),
                                 QStringLiteral("openfx.dynamic"),
                                 QStringLiteral("amount"),
                                 QStringLiteral("1"),
                                 QStringLiteral("innocent"))
                          .toUtf8()));
    error.clear();
    QVERIFY(!validator.validateProject(unknownFilter, &error));
    QVERIFY(error.contains(QStringLiteral("not approved"), Qt::CaseInsensitive));

    const QString mismatchedFilter
        = QDir(root.path()).filePath(QStringLiteral("mismatched-filter.mlt"));
    QVERIFY(writeFile(mismatchedFilter,
                      projectXml(QStringLiteral("filter"),
                                 QStringLiteral("qtext"),
                                 QStringLiteral("argument"),
                                 QStringLiteral("plain title"),
                                 QStringLiteral("innocent"))
                          .toUtf8()));
    error.clear();
    QVERIFY(!validator.validateProject(mismatchedFilter, &error));
    QVERIFY(error.contains(QStringLiteral("not approved"), Qt::CaseInsensitive));

    const QString unknownTransition
        = QDir(root.path()).filePath(QStringLiteral("unknown-transition.mlt"));
    QVERIFY(writeFile(unknownTransition,
                      QByteArray("<mlt><transition mlt_service=\"openfx.dynamic\"/></mlt>")));
    error.clear();
    QVERIFY(!validator.validateProject(unknownTransition, &error));
    QVERIFY(error.contains(QStringLiteral("not approved"), Qt::CaseInsensitive));

    const QString allowedTransition
        = QDir(root.path()).filePath(QStringLiteral("allowed-transition.mlt"));
    QVERIFY(
        writeFile(allowedTransition, QByteArray("<mlt><transition mlt_service=\"mix\"/></mlt>")));
    error.clear();
    QVERIFY2(validator.validateProject(allowedTransition, &error), qPrintable(error));
}

void TestMcpXmlPathValidator::rejectsDuplicateSecurityDiscriminators()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto validator = validatorForRoot(root.path());
    const QStringList names{QStringLiteral("mlt_service"),
                            QStringLiteral("shotcut:filter"),
                            QStringLiteral("shotcut:proxy")};
    for (const QString &name : names) {
        QString fileStem = name;
        fileStem.replace(QLatin1Char(':'), QLatin1Char('_'));
        const QString project = QDir(root.path()).filePath(fileStem + QStringLiteral(".mlt"));
        const QString xml = QStringLiteral("<mlt><producer>%1%2</producer></mlt>")
                                .arg(xmlProperty(name, QStringLiteral("first")),
                                     xmlProperty(name.toUpper(), QStringLiteral("second")));
        QVERIFY(writeFile(project, xml.toUtf8()));
        QString error;
        QVERIFY(!validator.validateProject(project, &error));
        QVERIFY(error.contains(QStringLiteral("duplicate"), Qt::CaseInsensitive));
    }

    const QString canonicalRoot = QFileInfo(root.path()).canonicalFilePath();
    const QString duplicateRoot = QDir(root.path()).filePath(QStringLiteral("duplicate-root.mlt"));
    const QString xml = QStringLiteral("<mlt root=\"%1\" ROOT=\"%1\"><playlist/></mlt>")
                            .arg(canonicalRoot.toHtmlEscaped());
    QVERIFY(writeFile(duplicateRoot, xml.toUtf8()));
    QString error;
    QVERIFY(!validator.validateProject(duplicateRoot, &error));
    QVERIFY(error.contains(QStringLiteral("duplicate"), Qt::CaseInsensitive));
}

void TestMcpXmlPathValidator::rejectsImageSequenceSymlinkEscape()
{
#ifdef Q_OS_WIN
    QSKIP("symbolic-link creation is not reliably available on Windows CI");
#else
    QTemporaryDir root;
    QTemporaryDir outside;
    QVERIFY(root.isValid());
    QVERIFY(outside.isValid());
    const QString frames = QDir(root.path()).filePath(QStringLiteral("frames"));
    QVERIFY(QDir().mkpath(frames));
    const QString outsideMedia = QDir(outside.path()).filePath(QStringLiteral("outside.png"));
    QVERIFY(writeFile(outsideMedia));
    const QString link = QDir(frames).filePath(QStringLiteral("frame-00001.png"));
    QVERIFY(QFile::link(outsideMedia, link));
    QVERIFY(QFileInfo(link).isSymLink());
    const QString project = QDir(root.path()).filePath(QStringLiteral("sequence-symlink.mlt"));
    QVERIFY(writeFile(project,
                      projectXml(QStringLiteral("producer"),
                                 QStringLiteral("qimage"),
                                 QStringLiteral("resource"),
                                 QStringLiteral("frames/frame-%05d.png"))
                          .toUtf8()));

    QString error;
    QVERIFY(!validatorForRoot(root.path()).validateProject(project, &error));
    QVERIFY(!error.isEmpty());
#endif
}

void TestMcpXmlPathValidator::rejectsMalformedImageSequences()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QStringList patterns{QStringLiteral("frames/frame-%04d-%d.png"),
                               QStringLiteral("frames/frame-%000d.png"),
                               QStringLiteral("frames/frame-%s.png")};
    for (qsizetype i = 0; i < patterns.size(); ++i) {
        const QString project
            = QDir(root.path()).filePath(QStringLiteral("malformed-sequence-%1.mlt").arg(i));
        QVERIFY(writeFile(project,
                          projectXml(QStringLiteral("producer"),
                                     QStringLiteral("qimage"),
                                     QStringLiteral("resource"),
                                     patterns.at(i))
                              .toUtf8()));
        QString error;
        QVERIFY(!validatorForRoot(root.path()).validateProject(project, &error));
        QVERIFY(error.contains(QStringLiteral("sequence"), Qt::CaseInsensitive));
    }
}

void TestMcpXmlPathValidator::rejectsPlaceboShaderPaths()
{
    QTemporaryDir root;
    QTemporaryDir outside;
    QVERIFY(root.isValid());
    QVERIFY(outside.isValid());
    const QString insideShader = QDir(root.path()).filePath(QStringLiteral("shaders/test.glsl"));
    const QString outsideShader = QDir(outside.path()).filePath(QStringLiteral("outside.glsl"));
    QVERIFY(writeFile(insideShader));
    QVERIFY(writeFile(outsideShader));
    const QStringList values{QStringLiteral("shaders/test.glsl"), outsideShader};
    for (qsizetype i = 0; i < values.size(); ++i) {
        const QString project
            = QDir(root.path()).filePath(QStringLiteral("placebo-shader-%1.mlt").arg(i));
        QVERIFY(writeFile(project,
                          projectXml(QStringLiteral("filter"),
                                     QStringLiteral("placebo.shader"),
                                     QStringLiteral("shader_path"),
                                     values.at(i),
                                     QStringLiteral("placebo.shader"))
                              .toUtf8()));
        QString error;
        QVERIFY(!validatorForRoot(root.path()).validateProject(project, &error));
        QVERIFY(!error.isEmpty());
    }
}

void TestMcpXmlPathValidator::rejectsMismatchedFilterIdentity()
{
    QTemporaryDir root;
    QTemporaryDir outside;
    QVERIFY(root.isValid());
    QVERIFY(outside.isValid());
    const QString outsideFile = QDir(outside.path()).filePath(QStringLiteral("identity.dat"));
    QVERIFY(writeFile(outsideFile));
    const QStringList documents{
        projectXml(QStringLiteral("filter"),
                   QStringLiteral("qtext"),
                   QStringLiteral("html"),
                   QStringLiteral("remote markup"),
                   QStringLiteral("innocent")),
        projectXml(QStringLiteral("filter"),
                   QStringLiteral("gpstext"),
                   QStringLiteral("gps.file"),
                   outsideFile,
                   QStringLiteral("innocent")),
        projectXml(QStringLiteral("filter"),
                   QStringLiteral("placebo.shader"),
                   QStringLiteral("shader_path"),
                   outsideFile,
                   QStringLiteral("innocent")),
        projectXml(QStringLiteral("filter"),
                   QStringLiteral("innocent"),
                   QStringLiteral("shader_path"),
                   outsideFile,
                   QStringLiteral("placebo.shader")),
    };
    for (qsizetype i = 0; i < documents.size(); ++i) {
        const QString project
            = QDir(root.path()).filePath(QStringLiteral("identity-mismatch-%1.mlt").arg(i));
        QVERIFY(writeFile(project, documents.at(i).toUtf8()));
        QString error;
        QVERIFY(!validatorForRoot(root.path()).validateProject(project, &error));
        QVERIFY(!error.isEmpty());
    }
}

void TestMcpXmlPathValidator::rejectsActiveFilterServices()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString rack = QDir(root.path()).filePath(QStringLiteral("rack.xml"));
    QVERIFY(writeFile(rack));
    const QStringList families{QStringLiteral("jack"),
                               QStringLiteral("jackrack"),
                               QStringLiteral("ladspa"),
                               QStringLiteral("lv2"),
                               QStringLiteral("vst2"),
                               QStringLiteral("openfx")};
    QStringList identities;
    for (const QString &family : families)
        identities << family << family + QStringLiteral(".plugin");

    for (qsizetype i = 0; i < identities.size(); ++i) {
        const QString project
            = QDir(root.path()).filePath(QStringLiteral("manual-service-%1.mlt").arg(i));
        QVERIFY(writeFile(project,
                          projectXml(QStringLiteral("filter"),
                                     identities.at(i),
                                     QStringLiteral("src"),
                                     rack,
                                     QStringLiteral("innocent"))
                              .toUtf8()));
        QString error;
        QVERIFY(!validatorForRoot(root.path()).validateProject(project, &error));
        QVERIFY(error.contains(QStringLiteral("manual"), Qt::CaseInsensitive)
                || error.contains(QStringLiteral("active"), Qt::CaseInsensitive));

        const QString idProject
            = QDir(root.path()).filePath(QStringLiteral("manual-id-%1.mlt").arg(i));
        QVERIFY(writeFile(idProject,
                          projectXml(QStringLiteral("filter"),
                                     QStringLiteral("frei0r.alphaspot"),
                                     QStringLiteral("0"),
                                     QStringLiteral("1"),
                                     identities.at(i))
                              .toUtf8()));
        error.clear();
        QVERIFY(!validatorForRoot(root.path()).validateProject(idProject, &error));
        QVERIFY(error.contains(QStringLiteral("manual"), Qt::CaseInsensitive)
                || error.contains(QStringLiteral("active"), Qt::CaseInsensitive));
    }

    const QString nearMiss = QDir(root.path()).filePath(QStringLiteral("manual-near-miss.mlt"));
    QVERIFY(writeFile(nearMiss,
                      projectXml(QStringLiteral("filter"),
                                 QStringLiteral("frei0r.alphaspot"),
                                 QStringLiteral("0"),
                                 QStringLiteral("1"),
                                 QStringLiteral("openfxed"))
                          .toUtf8()));
    QString error;
    QVERIFY2(validatorForRoot(root.path()).validateProject(nearMiss, &error), qPrintable(error));
}

void TestMcpXmlPathValidator::rejectsKnownActiveProducerLoaders()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString payload = QDir(root.path()).filePath(QStringLiteral("payload.txt"));
    QVERIFY(writeFile(payload));
    const QStringList services{QStringLiteral("melt_file"),
                               QStringLiteral("kdenlivetitle"),
                               QStringLiteral("glaxnimate")};
    for (const QString &service : services) {
        const QString propertyName = service == QStringLiteral("kdenlivetitle")
                                         ? QStringLiteral("xmldata")
                                         : QStringLiteral("resource");
        const QString propertyValue = service == QStringLiteral("kdenlivetitle") ? QStringLiteral(
                                          "<kdenlivetitle><content url=\"outside.svg\"/>"
                                          "</kdenlivetitle>")
                                                                                 : payload;
        const QString project
            = QDir(root.path()).filePath(QStringLiteral("active-producer-%1.mlt").arg(service));
        QVERIFY(
            writeFile(project,
                      projectXml(QStringLiteral("producer"), service, propertyName, propertyValue)
                          .toUtf8()));
        QString error;
        QVERIFY(!validatorForRoot(root.path()).validateProject(project, &error));
        QVERIFY(error.contains(QStringLiteral("loader"), Qt::CaseInsensitive));
    }
}

void TestMcpXmlPathValidator::rejectsAttributeValuedProperties()
{
    QTemporaryDir root;
    QTemporaryDir outside;
    QVERIFY(root.isValid());
    QVERIFY(outside.isValid());
    const QString outsideMedia = QDir(outside.path()).filePath(QStringLiteral("outside.mp4"));
    QVERIFY(writeFile(outsideMedia));
    const QString escapedOutside = outsideMedia.toHtmlEscaped();
    const QStringList documents{
        QStringLiteral("<mlt><producer><property name=\"resource\" value=\"%1\"/>"
                       "<property name=\"mlt_service\" value=\"avformat\"/>"
                       "</producer></mlt>")
            .arg(escapedOutside),
        QStringLiteral("<mlt><producer><property name=\"resource\" "
                       "value=\"https://127.0.0.1/media\"/>"
                       "<property name=\"mlt_service\" value=\"avformat\"/>"
                       "</producer></mlt>"),
        QStringLiteral("<mlt><producer><property name=\"resource\" value=\"%1\"/>"
                       "<property name=\"mlt_service\" value=\"melt_file\"/>"
                       "</producer></mlt>")
            .arg(escapedOutside),
        QStringLiteral("<mlt><producer><property name=\"resource\" value=\"inside.mp4\">"
                       "%1</property><property name=\"mlt_service\" value=\"avformat\"/>"
                       "</producer></mlt>")
            .arg(escapedOutside),
        QStringLiteral("<mlt><producer><property name=\"resource\" value=\"one\" "
                       "VALUE=\"two\"/><property name=\"mlt_service\" "
                       "value=\"avformat\"/></producer></mlt>"),
        QStringLiteral("<mlt><producer><property name=\"resource\" value=\"one\">"
                       "<nested/></property><property name=\"mlt_service\" "
                       "value=\"avformat\"/></producer></mlt>"),
    };
    for (qsizetype i = 0; i < documents.size(); ++i) {
        const QString project
            = QDir(root.path()).filePath(QStringLiteral("attribute-property-%1.mlt").arg(i));
        QVERIFY(writeFile(project, documents.at(i).toUtf8()));
        QString error;
        QVERIFY(!validatorForRoot(root.path()).validateProject(project, &error));
        QVERIFY(!error.isEmpty());
    }
}

void TestMcpXmlPathValidator::rejectsAlternateImageSequenceSyntax()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QStringList patterns{QStringLiteral("frames/frame-%i.png"),
                               QStringLiteral("frames/frame-%1234d.png"),
                               QStringLiteral("frames/.all.png")};
    for (qsizetype i = 0; i < patterns.size(); ++i) {
        const QString placeholder = QDir(root.path()).filePath(patterns.at(i));
        QVERIFY(writeFile(placeholder));
        const QString project
            = QDir(root.path()).filePath(QStringLiteral("alternate-sequence-%1.mlt").arg(i));
        QVERIFY(writeFile(project,
                          projectXml(QStringLiteral("producer"),
                                     QStringLiteral("qimage"),
                                     QStringLiteral("resource"),
                                     patterns.at(i))
                              .toUtf8()));
        QString error;
        QVERIFY(!validatorForRoot(root.path()).validateProject(project, &error));
        QVERIFY(!error.isEmpty());
    }
}

void TestMcpXmlPathValidator::rejectsAepxActiveContent()
{
    QTemporaryDir root;
    QTemporaryDir outside;
    QVERIFY(root.isValid());
    QVERIFY(outside.isValid());
    const QString outsideImage = QDir(outside.path()).filePath(QStringLiteral("outside.png"));
    QVERIFY(writeFile(outsideImage));
    const QString aepx = QDir(root.path()).filePath(QStringLiteral("active.aepx"));
    const QByteArray content = QStringLiteral("<aepx><fullpath>%1</fullpath></aepx>")
                                   .arg(outsideImage.toHtmlEscaped())
                                   .toUtf8();
    QVERIFY(writeFile(aepx, content));

    const auto validator = validatorForRoot(root.path());
    QString error;
    QVERIFY(!validator.validateMedia(aepx, &error));
    QVERIFY(error.contains(QStringLiteral("loader"), Qt::CaseInsensitive));

    const QString project = QDir(root.path()).filePath(QStringLiteral("glaxnimate.mlt"));
    QVERIFY(writeFile(project,
                      projectXml(QStringLiteral("producer"),
                                 QStringLiteral("glaxnimate"),
                                 QStringLiteral("resource"),
                                 aepx)
                          .toUtf8()));
    error.clear();
    QVERIFY(!validator.validateProject(project, &error));
    QVERIFY(error.contains(QStringLiteral("loader"), Qt::CaseInsensitive));
}

void TestMcpXmlPathValidator::acceptsAttributeValuedProperties()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString media = QDir(root.path()).filePath(QStringLiteral("inside.mp4"));
    QVERIFY(writeFile(media));
    const QString project = QDir(root.path()).filePath(QStringLiteral("attribute-values.mlt"));
    const QString xml = QStringLiteral("<mlt><producer><property name=\"resource\" value=\"%1\"/>"
                                       "<property name=\"mlt_service\" value=\"avformat\"/>"
                                       "</producer></mlt>")
                            .arg(media.toHtmlEscaped());
    QVERIFY(writeFile(project, xml.toUtf8()));

    QString error;
    QVERIFY2(validatorForRoot(root.path()).validateProject(project, &error), qPrintable(error));
}

void TestMcpXmlPathValidator::rejectsGlobAndQueryDecorations()
{
    QTemporaryDir root;
    QTemporaryDir outside;
    QVERIFY(root.isValid());
    QVERIFY(outside.isValid());
    const QString frames = QDir(root.path()).filePath(QStringLiteral("frames"));
    QVERIFY(QDir().mkpath(frames));
    const QString inside = QDir(frames).filePath(QStringLiteral("clip.png"));
    QVERIFY(writeFile(inside));
#ifndef Q_OS_WIN
    const QString literalPattern = QDir(frames).filePath(QStringLiteral("*.png"));
    QVERIFY(writeFile(literalPattern));
    const QString outsideMedia = QDir(outside.path()).filePath(QStringLiteral("outside.png"));
    QVERIFY(writeFile(outsideMedia));
    const QString link = QDir(frames).filePath(QStringLiteral("frame0001.png"));
    QVERIFY(QFile::link(outsideMedia, link));
    QVERIFY(QFileInfo(link).isSymLink());
#endif

    const QStringList references{
        QStringLiteral("frames/*.png\\?pattern_type=glob"),
        QStringLiteral("frames/[0-9].png"),
        QStringLiteral("frames/clip.png?pattern_type=glob"),
        QStringLiteral("frames/clip.png#fragment"),
        QUrl::fromLocalFile(inside).toString() + QStringLiteral("#fragment"),
    };
    for (qsizetype i = 0; i < references.size(); ++i) {
        const QString project
            = QDir(root.path()).filePath(QStringLiteral("glob-query-%1.mlt").arg(i));
        QVERIFY(writeFile(project,
                          projectXml(QStringLiteral("producer"),
                                     QStringLiteral("avformat"),
                                     QStringLiteral("resource"),
                                     references.at(i))
                              .toUtf8()));
        QString error;
        QVERIFY(!validatorForRoot(root.path()).validateProject(project, &error));
        QVERIFY(!error.isEmpty());
    }
}

void TestMcpXmlPathValidator::rejectsRelativeTransitionExtraPaths()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString media = QDir(root.path()).filePath(QStringLiteral("inside.dat"));
    QVERIFY(writeFile(media));
    const QStringList names{QStringLiteral("src"),
                            QStringLiteral("filename"),
                            QStringLiteral("av.file")};
    for (qsizetype i = 0; i < names.size(); ++i) {
        const QString project
            = QDir(root.path()).filePath(QStringLiteral("relative-transition-%1.mlt").arg(i));
        QVERIFY(writeFile(project,
                          projectXml(QStringLiteral("transition"),
                                     QStringLiteral("custom.transition"),
                                     names.at(i),
                                     QStringLiteral("inside.dat"))
                              .toUtf8()));
        QString error;
        QVERIFY(!validatorForRoot(root.path()).validateProject(project, &error));
        QVERIFY(error.contains(QStringLiteral("absolute"), Qt::CaseInsensitive));
    }

    const QString relativeResource
        = QDir(root.path()).filePath(QStringLiteral("relative-transition-resource.mlt"));
    QVERIFY(writeFile(relativeResource,
                      projectXml(QStringLiteral("transition"),
                                 QStringLiteral("custom.transition"),
                                 QStringLiteral("resource"),
                                 QStringLiteral("inside.dat"))
                          .toUtf8()));
    QString error;
    QVERIFY2(validatorForRoot(root.path()).validateProject(relativeResource, &error),
             qPrintable(error));
}

void TestMcpXmlPathValidator::rejectsActiveNestedMaskFilter()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString rack = QDir(root.path()).filePath(QStringLiteral("rack.xml"));
    QVERIFY(writeFile(rack));
    const QString activeProperties
        = xmlProperty(QStringLiteral("mlt_service"), QStringLiteral("mask_start"))
          + xmlProperty(QStringLiteral("shotcut:filter"), QStringLiteral("innocent"))
          + xmlProperty(QStringLiteral("filter"), QStringLiteral("jackrack"))
          + xmlProperty(QStringLiteral("filter.src"), rack);
    const QString activeProject
        = QDir(root.path()).filePath(QStringLiteral("nested-active-filter.mlt"));
    QVERIFY(
        writeFile(activeProject,
                  QStringLiteral("<mlt><filter>%1</filter></mlt>").arg(activeProperties).toUtf8()));
    QString error;
    QVERIFY(!validatorForRoot(root.path()).validateProject(activeProject, &error));
    QVERIFY(error.contains(QStringLiteral("nested"), Qt::CaseInsensitive));

    const QString duplicateProperties
        = xmlProperty(QStringLiteral("mlt_service"), QStringLiteral("mask_start"))
          + xmlProperty(QStringLiteral("filter"), QStringLiteral("shape"))
          + xmlProperty(QStringLiteral("FILTER"), QStringLiteral("jackrack"));
    const QString duplicateProject
        = QDir(root.path()).filePath(QStringLiteral("duplicate-nested-filter.mlt"));
    QVERIFY(writeFile(duplicateProject,
                      QStringLiteral("<mlt><filter>%1</filter></mlt>")
                          .arg(duplicateProperties)
                          .toUtf8()));
    error.clear();
    QVERIFY(!validatorForRoot(root.path()).validateProject(duplicateProject, &error));
    QVERIFY(error.contains(QStringLiteral("duplicate"), Qt::CaseInsensitive));

    const QString allowedProperties
        = xmlProperty(QStringLiteral("mlt_service"), QStringLiteral("mask_start"))
          + xmlProperty(QStringLiteral("shotcut:filter"), QStringLiteral("maskGlaxnimate"))
          + xmlProperty(QStringLiteral("filter"), QStringLiteral("shape"))
          + xmlProperty(QStringLiteral("filter.resource"), rack);
    const QString allowedProject
        = QDir(root.path()).filePath(QStringLiteral("allowed-nested-filter.mlt"));
    QVERIFY(
        writeFile(allowedProject,
                  QStringLiteral("<mlt><filter>%1</filter></mlt>").arg(allowedProperties).toUtf8()));
    error.clear();
    QVERIFY2(validatorForRoot(root.path()).validateProject(allowedProject, &error),
             qPrintable(error));

    const QStringList safeSelectors{QString(),
                                    QStringLiteral("0"),
                                    QStringLiteral("shape"),
                                    QStringLiteral("frei0r.alphaspot"),
                                    QStringLiteral("frei0r.bluescreen0r")};
    for (qsizetype i = 0; i < safeSelectors.size(); ++i) {
        const QString project
            = QDir(root.path()).filePath(QStringLiteral("safe-mask-selector-%1.mlt").arg(i));
        QVERIFY(writeFile(project,
                          projectXml(QStringLiteral("filter"),
                                     QStringLiteral("mask_start"),
                                     QStringLiteral("filter"),
                                     safeSelectors.at(i),
                                     QStringLiteral("maskShape"))
                              .toUtf8()));
        error.clear();
        QVERIFY2(validatorForRoot(root.path()).validateProject(project, &error), qPrintable(error));
    }

    const QStringList unsafeSelectors{QStringLiteral(" shape"),
                                      QStringLiteral("shape "),
                                      QStringLiteral("SHAPE"),
                                      QStringLiteral("shape\n"),
                                      QStringLiteral("shape\r"),
                                      QStringLiteral("shape\t")};
    for (qsizetype i = 0; i < unsafeSelectors.size(); ++i) {
        const QString project
            = QDir(root.path()).filePath(QStringLiteral("raw-mask-selector-%1.mlt").arg(i));
        QVERIFY(writeFile(project,
                          projectXml(QStringLiteral("filter"),
                                     QStringLiteral("mask_start"),
                                     QStringLiteral("filter"),
                                     unsafeSelectors.at(i),
                                     QStringLiteral("maskShape"))
                              .toUtf8()));
        error.clear();
        QVERIFY2(!validatorForRoot(root.path()).validateProject(project, &error),
                 qPrintable(QStringLiteral("accepted unsafe mask selector at %1: [%2]")
                                .arg(i)
                                .arg(unsafeSelectors.at(i))));
        QVERIFY(error.contains(QStringLiteral("nested"), Qt::CaseInsensitive));
    }

    const QString omittedProject
        = QDir(root.path()).filePath(QStringLiteral("omitted-mask-selector.mlt"));
    QVERIFY(
        writeFile(omittedProject,
                  QStringLiteral("<mlt><filter>%1</filter></mlt>")
                      .arg(xmlProperty(QStringLiteral("mlt_service"), QStringLiteral("mask_start")))
                      .toUtf8()));
    error.clear();
    QVERIFY2(validatorForRoot(root.path()).validateProject(omittedProject, &error),
             qPrintable(error));
}

void TestMcpXmlPathValidator::rejectsActiveContentFilterServices()
{
    QTemporaryDir root;
    QTemporaryDir outside;
    QVERIFY(root.isValid());
    QVERIFY(outside.isValid());
    const QString outsideModels = QDir(outside.path()).filePath(QStringLiteral("models"));
    QVERIFY(QDir().mkpath(outsideModels));
    const QStringList documents{
        projectXml(QStringLiteral("filter"),
                   QStringLiteral("opencv.tracker"),
                   QStringLiteral("modelsfolder"),
                   outsideModels,
                   QStringLiteral("tracker")),
        projectXml(QStringLiteral("filter"),
                   QStringLiteral("placebo.shader"),
                   QStringLiteral("shader_text"),
                   QStringLiteral("//!HOOK MAIN\n//!BIND HOOKED"),
                   QStringLiteral("innocent")),
    };
    for (qsizetype i = 0; i < documents.size(); ++i) {
        const QString project
            = QDir(root.path()).filePath(QStringLiteral("active-content-filter-%1.mlt").arg(i));
        QVERIFY(writeFile(project, documents.at(i).toUtf8()));
        QString error;
        QVERIFY(!validatorForRoot(root.path()).validateProject(project, &error));
        QVERIFY(error.contains(QStringLiteral("active"), Qt::CaseInsensitive));
    }
}

void TestMcpXmlPathValidator::rejectsUnsupportedAvfilterServices()
{
    QTemporaryDir root;
    QTemporaryDir outside;
    QVERIFY(root.isValid());
    QVERIFY(outside.isValid());
    const QString outsideFile = QDir(outside.path()).filePath(QStringLiteral("outside.dat"));
    QVERIFY(writeFile(outsideFile));
    const QStringList services{QStringLiteral("avfilter.drawtext"),
                               QStringLiteral("avfilter.arnndn"),
                               QStringLiteral("avfilter.subtitles"),
                               QStringLiteral("avfilter.zmq"),
                               QStringLiteral("avfilter.movie"),
                               QStringLiteral("avfilter.amovie"),
                               QStringLiteral("avfilter.gblur")};
    const QStringList properties{QStringLiteral("av.fontfile"),
                                 QStringLiteral("av.model"),
                                 QStringLiteral("av.filename"),
                                 QStringLiteral("av.bind_address"),
                                 QStringLiteral("av.filename"),
                                 QStringLiteral("av.filename"),
                                 QStringLiteral("av.radius")};
    const QStringList values{outsideFile,
                             outsideFile,
                             outsideFile,
                             QStringLiteral("tcp://127.0.0.1:5555"),
                             outsideFile,
                             outsideFile,
                             QStringLiteral("5")};
    for (qsizetype i = 0; i < services.size(); ++i) {
        const QString project
            = QDir(root.path()).filePath(QStringLiteral("unsupported-avfilter-%1.mlt").arg(i));
        QVERIFY(writeFile(project,
                          projectXml(QStringLiteral("filter"),
                                     services.at(i),
                                     properties.at(i),
                                     values.at(i),
                                     QStringLiteral("innocent"))
                              .toUtf8()));
        QString error;
        QVERIFY(!validatorForRoot(root.path()).validateProject(project, &error));
        QVERIFY(error.contains(QStringLiteral("active"), Qt::CaseInsensitive)
                || error.contains(QStringLiteral("AVFilter"), Qt::CaseInsensitive));
    }
}

void TestMcpXmlPathValidator::restrictsSupportedAvfilterOptionsAndLuts()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString validOption = QDir(root.path()).filePath(QStringLiteral("valid-av-option.mlt"));
    QVERIFY(writeFile(validOption,
                      projectXml(QStringLiteral("filter"),
                                 QStringLiteral("avfilter.hue"),
                                 QStringLiteral("av.h"),
                                 QStringLiteral("90"))
                          .toUtf8()));
    QString error;
    QVERIFY2(validatorForRoot(root.path()).validateProject(validOption, &error), qPrintable(error));

    const QString invalidOption
        = QDir(root.path()).filePath(QStringLiteral("invalid-av-option.mlt"));
    QVERIFY(writeFile(invalidOption,
                      projectXml(QStringLiteral("filter"),
                                 QStringLiteral("avfilter.hue"),
                                 QStringLiteral("av.fontfile"),
                                 QStringLiteral("font.ttf"))
                          .toUtf8()));
    error.clear();
    QVERIFY(!validatorForRoot(root.path()).validateProject(invalidOption, &error));
    QVERIFY(error.contains(QStringLiteral("AVFilter"), Qt::CaseInsensitive));

    const QString cube = QDir(root.path()).filePath(QStringLiteral("looks/test.cube"));
    const QString text = QDir(root.path()).filePath(QStringLiteral("looks/test.txt"));
    QVERIFY(writeFile(cube));
    QVERIFY(writeFile(text));
    const QString validLut = QDir(root.path()).filePath(QStringLiteral("valid-lut.mlt"));
    QVERIFY(writeFile(validLut,
                      projectXml(QStringLiteral("filter"),
                                 QStringLiteral("avfilter.lut3d"),
                                 QStringLiteral("av.file"),
                                 QStringLiteral("looks/test.cube"))
                          .toUtf8()));
    error.clear();
    QVERIFY2(validatorForRoot(root.path()).validateProject(validLut, &error), qPrintable(error));

    const QString invalidLut = QDir(root.path()).filePath(QStringLiteral("invalid-lut.mlt"));
    QVERIFY(writeFile(invalidLut,
                      projectXml(QStringLiteral("filter"),
                                 QStringLiteral("avfilter.lut3d"),
                                 QStringLiteral("av.file"),
                                 QStringLiteral("looks/test.txt"))
                          .toUtf8()));
    error.clear();
    QVERIFY(!validatorForRoot(root.path()).validateProject(invalidLut, &error));
    QVERIFY(error.contains(QStringLiteral("extension"), Qt::CaseInsensitive));

    const QString urlLut = QDir(root.path()).filePath(QStringLiteral("url-lut.mlt"));
    QVERIFY(writeFile(urlLut,
                      projectXml(QStringLiteral("filter"),
                                 QStringLiteral("avfilter.lut3d"),
                                 QStringLiteral("av.file"),
                                 QUrl::fromLocalFile(cube).toString())
                          .toUtf8()));
    error.clear();
    QVERIFY(!validatorForRoot(root.path()).validateProject(urlLut, &error));
    QVERIFY(!error.isEmpty());

#ifndef Q_OS_WIN
    const QString linkedCube = QDir(root.path()).filePath(QStringLiteral("looks/linked.cube"));
    QVERIFY(QFile::link(cube, linkedCube));
    QVERIFY(QFileInfo(linkedCube).isSymLink());
    const QString symlinkLut = QDir(root.path()).filePath(QStringLiteral("symlink-lut.mlt"));
    QVERIFY(writeFile(symlinkLut,
                      projectXml(QStringLiteral("filter"),
                                 QStringLiteral("avfilter.lut3d"),
                                 QStringLiteral("av.file"),
                                 QStringLiteral("looks/linked.cube"))
                          .toUtf8()));
    error.clear();
    QVERIFY(!validatorForRoot(root.path()).validateProject(symlinkLut, &error));
    QVERIFY(!error.isEmpty());
#endif
}

void TestMcpXmlPathValidator::restrictsMaskApplyTransitionFactoryAndPaths()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());

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
    for (qsizetype i = 0; i < unsafeTransitions.size(); ++i) {
        const QString project
            = QDir(root.path()).filePath(QStringLiteral("mask-apply-unsafe-%1.mlt").arg(i));
        QVERIFY(writeFile(project,
                          projectXml(QStringLiteral("filter"),
                                     QStringLiteral("mask_apply"),
                                     QStringLiteral("transition"),
                                     unsafeTransitions.at(i),
                                     QStringLiteral("mask_apply"))
                              .toUtf8()));
        QString error;
        QVERIFY2(!validatorForRoot(root.path()).validateProject(project, &error),
                 qPrintable(QStringLiteral("accepted unsafe Mask Apply transition at %1: [%2]")
                                .arg(i)
                                .arg(unsafeTransitions.at(i))));
        QVERIFY(error.contains(QStringLiteral("transition"), Qt::CaseInsensitive));
    }

    const QString unsafePath
        = QDir(root.path()).filePath(QStringLiteral("mask-apply-transition-path.mlt"));
    QVERIFY(writeFile(unsafePath,
                      projectXml(QStringLiteral("filter"),
                                 QStringLiteral("mask_apply"),
                                 QStringLiteral("transition.resource"),
                                 QStringLiteral("payload.dat"),
                                 QStringLiteral("mask_apply"))
                          .toUtf8()));
    QString error;
    QVERIFY(!validatorForRoot(root.path()).validateProject(unsafePath, &error));
    QVERIFY(error.contains(QStringLiteral("transition path"), Qt::CaseInsensitive));

    const QStringList safeTransitions{QString(), QStringLiteral("qtblend")};
    for (qsizetype i = 0; i < safeTransitions.size(); ++i) {
        const QString project
            = QDir(root.path()).filePath(QStringLiteral("mask-apply-safe-%1.mlt").arg(i));
        QVERIFY(writeFile(project,
                          projectXml(QStringLiteral("filter"),
                                     QStringLiteral("mask_apply"),
                                     QStringLiteral("transition"),
                                     safeTransitions.at(i),
                                     QStringLiteral("mask_apply"))
                              .toUtf8()));
        error.clear();
        QVERIFY2(validatorForRoot(root.path()).validateProject(project, &error), qPrintable(error));
    }

    const QString omitted = QDir(root.path()).filePath(QStringLiteral("mask-apply-omitted.mlt"));
    QVERIFY(
        writeFile(omitted,
                  QStringLiteral("<mlt><filter>%1%2</filter></mlt>")
                      .arg(xmlProperty(QStringLiteral("mlt_service"), QStringLiteral("mask_apply")),
                           xmlProperty(QStringLiteral("shotcut:filter"),
                                       QStringLiteral("mask_apply")))
                      .toUtf8()));
    error.clear();
    QVERIFY2(validatorForRoot(root.path()).validateProject(omitted, &error), qPrintable(error));

    const QString safeOption
        = QDir(root.path()).filePath(QStringLiteral("mask-apply-transition-threads.mlt"));
    QVERIFY(writeFile(safeOption,
                      projectXml(QStringLiteral("filter"),
                                 QStringLiteral("mask_apply"),
                                 QStringLiteral("transition.threads"),
                                 QStringLiteral("0"),
                                 QStringLiteral("mask_apply"))
                          .toUtf8()));
    error.clear();
    QVERIFY2(validatorForRoot(root.path()).validateProject(safeOption, &error), qPrintable(error));
}

void TestMcpXmlPathValidator::restrictsAffineBackgroundFactoryAndPaths()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());

    const QStringList unsafeBackgrounds{
        QString(),
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
    for (qsizetype i = 0; i < unsafeBackgrounds.size(); ++i) {
        const QString project
            = QDir(root.path()).filePath(QStringLiteral("affine-unsafe-%1.mlt").arg(i));
        QVERIFY(writeFile(project,
                          projectXml(QStringLiteral("filter"),
                                     QStringLiteral("affine"),
                                     QStringLiteral("background"),
                                     unsafeBackgrounds.at(i),
                                     QStringLiteral("sizePosition"))
                              .toUtf8()));
        QString error;
        QVERIFY2(!validatorForRoot(root.path()).validateProject(project, &error),
                 qPrintable(QStringLiteral("accepted unsafe affine background at %1: [%2]")
                                .arg(i)
                                .arg(unsafeBackgrounds.at(i))));
        QVERIFY(error.contains(QStringLiteral("background"), Qt::CaseInsensitive));
    }

    const QString unsafePath
        = QDir(root.path()).filePath(QStringLiteral("affine-producer-path.mlt"));
    QVERIFY(writeFile(unsafePath,
                      projectXml(QStringLiteral("filter"),
                                 QStringLiteral("affine"),
                                 QStringLiteral("producer.resource"),
                                 QStringLiteral("payload.dat"),
                                 QStringLiteral("sizePosition"))
                          .toUtf8()));
    QString error;
    QVERIFY(!validatorForRoot(root.path()).validateProject(unsafePath, &error));
    QVERIFY(error.contains(QStringLiteral("producer path"), Qt::CaseInsensitive));

    const QStringList safeBackgrounds{
        QStringLiteral("color:0"),
        QStringLiteral("colour:0"),
        QStringLiteral("color:#fff"),
        QStringLiteral("colour:#FfFf"),
        QStringLiteral("color:#012345"),
        QStringLiteral("colour:#01234567"),
    };
    for (qsizetype i = 0; i < safeBackgrounds.size(); ++i) {
        const QString project
            = QDir(root.path()).filePath(QStringLiteral("affine-safe-%1.mlt").arg(i));
        QVERIFY(writeFile(project,
                          projectXml(QStringLiteral("filter"),
                                     QStringLiteral("affine"),
                                     QStringLiteral("background"),
                                     safeBackgrounds.at(i),
                                     QStringLiteral("sizePosition"))
                              .toUtf8()));
        error.clear();
        QVERIFY2(validatorForRoot(root.path()).validateProject(project, &error), qPrintable(error));
    }

    const QString omitted = QDir(root.path()).filePath(QStringLiteral("affine-omitted.mlt"));
    QVERIFY(writeFile(omitted,
                      QStringLiteral("<mlt><filter>%1%2</filter></mlt>")
                          .arg(xmlProperty(QStringLiteral("mlt_service"), QStringLiteral("affine")),
                               xmlProperty(QStringLiteral("shotcut:filter"),
                                           QStringLiteral("sizePosition")))
                          .toUtf8()));
    error.clear();
    QVERIFY2(validatorForRoot(root.path()).validateProject(omitted, &error), qPrintable(error));

    const QString safeOption
        = QDir(root.path()).filePath(QStringLiteral("affine-producer-aspect.mlt"));
    QVERIFY(writeFile(safeOption,
                      projectXml(QStringLiteral("filter"),
                                 QStringLiteral("affine"),
                                 QStringLiteral("producer.aspect_ratio"),
                                 QStringLiteral("1"),
                                 QStringLiteral("sizePosition"))
                          .toUtf8()));
    error.clear();
    QVERIFY2(validatorForRoot(root.path()).validateProject(safeOption, &error), qPrintable(error));
}

void TestMcpXmlPathValidator::restrictsDustAndTransitionProducerFactories()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());

    const QStringList dustFactories{QString(),
                                    QStringLiteral("loader"),
                                    QStringLiteral("Loader"),
                                    QStringLiteral(" loader"),
                                    QStringLiteral("loader\n")};
    for (qsizetype i = 0; i < dustFactories.size(); ++i) {
        const QString project
            = QDir(root.path()).filePath(QStringLiteral("dust-factory-%1.mlt").arg(i));
        QVERIFY(writeFile(project,
                          projectXml(QStringLiteral("filter"),
                                     QStringLiteral("dust"),
                                     QStringLiteral("factory"),
                                     dustFactories.at(i),
                                     QStringLiteral("dust"))
                              .toUtf8()));
        QString error;
        QVERIFY(!validatorForRoot(root.path()).validateProject(project, &error));
        QVERIFY(error.contains(QStringLiteral("Dust"), Qt::CaseInsensitive));
    }

    const QString dustOmitted = QDir(root.path()).filePath(QStringLiteral("dust-omitted.mlt"));
    QVERIFY(writeFile(dustOmitted,
                      QStringLiteral("<mlt><filter>%1%2</filter></mlt>")
                          .arg(xmlProperty(QStringLiteral("mlt_service"), QStringLiteral("dust")),
                               xmlProperty(QStringLiteral("shotcut:filter"), QStringLiteral("dust")))
                          .toUtf8()));
    QString error;
    QVERIFY2(validatorForRoot(root.path()).validateProject(dustOmitted, &error), qPrintable(error));

    const QStringList transitionServices{QStringLiteral("luma"), QStringLiteral("composite")};
    const QStringList safeFactories{QString(), QStringLiteral("loader")};
    for (const QString &service : transitionServices) {
        const QString omitted
            = QDir(root.path()).filePath(QStringLiteral("%1-factory-omitted.mlt").arg(service));
        QVERIFY(writeFile(omitted,
                          QStringLiteral("<mlt><transition>%1</transition></mlt>")
                              .arg(xmlProperty(QStringLiteral("mlt_service"), service))
                              .toUtf8()));
        error.clear();
        QVERIFY2(validatorForRoot(root.path()).validateProject(omitted, &error), qPrintable(error));

        for (qsizetype i = 0; i < safeFactories.size(); ++i) {
            const QString project
                = QDir(root.path())
                      .filePath(QStringLiteral("%1-factory-safe-%2.mlt").arg(service).arg(i));
            QVERIFY(writeFile(project,
                              projectXml(QStringLiteral("transition"),
                                         service,
                                         QStringLiteral("factory"),
                                         safeFactories.at(i))
                                  .toUtf8()));
            error.clear();
            QVERIFY2(validatorForRoot(root.path()).validateProject(project, &error),
                     qPrintable(error));
        }

        const QStringList unsafeFactories{QStringLiteral("Loader"),
                                          QStringLiteral(" loader"),
                                          QStringLiteral("loader "),
                                          QStringLiteral("loader\n"),
                                          QStringLiteral("loader\r"),
                                          QStringLiteral("loader\t"),
                                          QStringLiteral("avformat")};
        for (qsizetype i = 0; i < unsafeFactories.size(); ++i) {
            const QString project
                = QDir(root.path())
                      .filePath(QStringLiteral("%1-factory-unsafe-%2.mlt").arg(service).arg(i));
            QVERIFY(writeFile(project,
                              projectXml(QStringLiteral("transition"),
                                         service,
                                         QStringLiteral("factory"),
                                         unsafeFactories.at(i))
                                  .toUtf8()));
            error.clear();
            QVERIFY2(!validatorForRoot(root.path()).validateProject(project, &error),
                     qPrintable(QStringLiteral("accepted unsafe %1 factory at %2: [%3]")
                                    .arg(service)
                                    .arg(i)
                                    .arg(unsafeFactories.at(i))));
            QVERIFY(error.contains(QStringLiteral("factory"), Qt::CaseInsensitive));
        }

        const QStringList unsafeNames{QStringLiteral("producer.resource"),
                                      QStringLiteral("producer.src"),
                                      QStringLiteral("producer.av.file"),
                                      QStringLiteral("luma.resource"),
                                      QStringLiteral("luma.url")};
        for (qsizetype i = 0; i < unsafeNames.size(); ++i) {
            const QString project
                = QDir(root.path())
                      .filePath(QStringLiteral("%1-nested-path-%2.mlt").arg(service).arg(i));
            QVERIFY(writeFile(project,
                              projectXml(QStringLiteral("transition"),
                                         service,
                                         unsafeNames.at(i),
                                         QStringLiteral("payload.dat"))
                                  .toUtf8()));
            error.clear();
            QVERIFY(!validatorForRoot(root.path()).validateProject(project, &error));
            QVERIFY(error.contains(QStringLiteral("nested"), Qt::CaseInsensitive));
        }

        const QString safeOption
            = QDir(root.path()).filePath(QStringLiteral("%1-luma-softness.mlt").arg(service));
        QVERIFY(writeFile(safeOption,
                          projectXml(QStringLiteral("transition"),
                                     service,
                                     QStringLiteral("luma.softness"),
                                     QStringLiteral("0.25"))
                              .toUtf8()));
        error.clear();
        QVERIFY2(validatorForRoot(root.path()).validateProject(safeOption, &error),
                 qPrintable(error));
    }
}

QTEST_GUILESS_MAIN(TestMcpXmlPathValidator)
#include "test_mcpxmlpathvalidator.moc"
