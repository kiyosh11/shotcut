/*
 * Copyright (c) 2026 Meltytech, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "mcpxmlpathvalidator.h"

#include "mcpbridgepolicy.h"

#include <QByteArray>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QPair>
#include <QRegularExpression>
#include <QStringList>
#include <QUrl>
#include <QVector>
#include <QXmlStreamReader>

#include <utility>

struct McpXmlPathValidator::ValidationState
{
    QSet<QString> validatedProjects;
    QSet<QString> activeProjects;
    int referenceCount{0};
    int xmlElementCount{0};
    qint64 projectBytes{0};
};

namespace {
constexpr int kMaximumNestedProjectDepth = 8;
constexpr int kMaximumResourceReferences = 10000;
constexpr int kMaximumXmlElements = 200000;
constexpr int kMaximumXmlDepth = 256;
constexpr qsizetype kMaximumPropertyNameCharacters = 512;
constexpr qsizetype kMaximumPropertyValueCharacters = 1024 * 1024;
constexpr qint64 kMaximumProjectBytes = 64 * 1024 * 1024;
constexpr qint64 kSignatureBytes = 16 * 1024;
constexpr int kMaximumSequenceMembers = 4096;
constexpr int kMaximumSequenceDirectoryEntries = 100000;

enum class ReferenceKind { LocalPath, SafePseudoResource, Rejected };

struct ResolvedReference
{
    ReferenceKind kind{ReferenceKind::Rejected};
    QString path;
    QString error;
};

struct MltElement
{
    QString name;
    QVector<QPair<QString, QString>> properties;
};

struct ActiveProjectGuard
{
    QSet<QString> &paths;
    QString path;
    ~ActiveProjectGuard() { paths.remove(path); }
};

bool fail(QString *errorMessage, const QString &message)
{
    if (errorMessage)
        *errorMessage = message;
    return false;
}

bool isMltElement(const QString &name)
{
    const QString lower = name.toLower();
    return lower == QStringLiteral("producer") || lower == QStringLiteral("video")
           || lower == QStringLiteral("filter") || lower == QStringLiteral("playlist")
           || lower == QStringLiteral("tractor") || lower == QStringLiteral("track")
           || lower == QStringLiteral("transition") || lower == QStringLiteral("chain");
}

bool isUncPath(const QString &path)
{
    return path.startsWith(QStringLiteral("\\\\")) || path.startsWith(QStringLiteral("//"));
}

bool isWindowsDriveAbsolutePath(const QString &path)
{
    static const QRegularExpression drivePath(QStringLiteral("^[A-Za-z]:[\\\\/]"));
    return drivePath.match(path).hasMatch();
}

enum class ImageSequenceStatus { NotSequence, Valid, Invalid };

struct ImageSequencePattern
{
    ImageSequenceStatus status{ImageSequenceStatus::NotSequence};
    QRegularExpression fileNameExpression;
};

ImageSequencePattern imageSequencePattern(const QString &path)
{
    if (!path.contains(QLatin1Char('%')))
        return {};

    const QFileInfo info(path);
    const QString fileName = info.fileName();
    if (path.count(QLatin1Char('%')) != 1 || info.path().contains(QLatin1Char('%')))
        return {ImageSequenceStatus::Invalid, {}};

    static const QRegularExpression specifier(QStringLiteral("%(?:0([1-9][0-9]*))?d"));
    const auto match = specifier.match(fileName);
    if (!match.hasMatch())
        return {ImageSequenceStatus::Invalid, {}};
    if (!match.captured(1).isEmpty()) {
        bool ok = false;
        const int width = match.captured(1).toInt(&ok);
        if (!ok || width > 64)
            return {ImageSequenceStatus::Invalid, {}};
    }

    const QString prefix = fileName.left(match.capturedStart());
    const QString suffix = fileName.mid(match.capturedEnd());
    const QString expression = QStringLiteral("^%1-?[0-9]+%2$")
                                   .arg(QRegularExpression::escape(prefix),
                                        QRegularExpression::escape(suffix));
    return {ImageSequenceStatus::Valid, QRegularExpression(expression)};
}

bool isOpaqueLoaderSuffix(const QString &path)
{
    static const QStringList suffixes{
        QStringLiteral("m3u"),           QStringLiteral("m3u8"),     QStringLiteral("pls"),
        QStringLiteral("xspf"),          QStringLiteral("asx"),      QStringLiteral("wax"),
        QStringLiteral("wvx"),           QStringLiteral("wpl"),      QStringLiteral("smil"),
        QStringLiteral("smi"),           QStringLiteral("ram"),      QStringLiteral("qtl"),
        QStringLiteral("cue"),           QStringLiteral("ffconcat"), QStringLiteral("concat"),
        QStringLiteral("mpd"),           QStringLiteral("f4m"),      QStringLiteral("ism"),
        QStringLiteral("ismc"),          QStringLiteral("otio"),     QStringLiteral("html"),
        QStringLiteral("htm"),           QStringLiteral("qml"),      QStringLiteral("sdp"),
        QStringLiteral("kdenlivetitle"), QStringLiteral("svg"),      QStringLiteral("svgz"),
        QStringLiteral("json"),          QStringLiteral("aep"),      QStringLiteral("aepx"),
        QStringLiteral("avd"),           QStringLiteral("lot"),      QStringLiteral("lottie"),
        QStringLiteral("rawr"),          QStringLiteral("riv"),      QStringLiteral("tgs"),
        QStringLiteral("melt"),          QStringLiteral("inigo"),
    };
    return suffixes.contains(QFileInfo(path).suffix().toLower());
}

bool isMltLoaderSuffix(const QString &path)
{
    static const QStringList suffixes{
        QStringLiteral("mlt"),
        QStringLiteral("westley"),
        QStringLiteral("kdenlive"),
        QStringLiteral("graphics"),
        QStringLiteral("jfx"),
        QStringLiteral("jef"),
        QStringLiteral("kino"),
        QStringLiteral("story"),
    };
    return suffixes.contains(QFileInfo(path).suffix().toLower());
}

bool isMltXmlFileService(const QString &service)
{
    const QString lower = service.trimmed().toLower();
    return lower == QStringLiteral("xml") || lower == QStringLiteral("xml-nogl")
           || lower == QStringLiteral("xml-clip");
}

bool isInlineMltXmlService(const QString &service)
{
    return service.trimmed().compare(QStringLiteral("xml-string"), Qt::CaseInsensitive) == 0;
}

bool isPseudoResource(const QString &service, const QString &name, const QString &value)
{
    if (value.isEmpty())
        return true;

    const QString lowerValue = value.toLower();
    if (lowerValue == QStringLiteral("<producer>") || lowerValue == QStringLiteral("<tractor>")
        || lowerValue == QStringLiteral("<playlist>") || lowerValue == QStringLiteral("<chain>")) {
        return true;
    }

    const QString lowerService = service.trimmed().toLower();
    const QString lowerName = name.toLower();
    if (lowerName == QStringLiteral("resource")
        && (lowerService == QStringLiteral("color") || lowerService == QStringLiteral("colour"))) {
        return true;
    }
    if (lowerService == QStringLiteral("blank") && lowerValue == QStringLiteral("blank"))
        return true;

    static const QRegularExpression builtInLuma(QStringLiteral(
                                                    "^%luma(?:0[1-9]|1[0-9]|2[0-2])\\.pgm$"),
                                                QRegularExpression::CaseInsensitiveOption);
    return (lowerService == QStringLiteral("luma")
            || lowerService == QStringLiteral("movit.luma_mix"))
           && builtInLuma.match(value).hasMatch();
}

QString stripMltDecorations(const QString &service, const QString &name, const QString &value)
{
    QString result = value;
    if (name.compare(QStringLiteral("resource"), Qt::CaseInsensitive) == 0) {
        if (service.trimmed().compare(QStringLiteral("timewarp"), Qt::CaseInsensitive) == 0) {
            static const QRegularExpression speedPrefix(
                QStringLiteral("^[+-]?[0-9]+(?:[\\.,][0-9]+)?:"));
            const auto match = speedPrefix.match(result);
            if (match.hasMatch())
                result.remove(0, match.capturedLength());
        }

        const qsizetype query = result.lastIndexOf(QStringLiteral("\\?"));
        if (query > 0)
            result.truncate(query);
    }
    return result;
}

ResolvedReference resolveReference(const QString &service,
                                   const QString &name,
                                   const QString &value,
                                   const QDir &baseDirectory,
                                   bool allowPseudoResources)
{
    if (allowPseudoResources && isPseudoResource(service, name, value))
        return {ReferenceKind::SafePseudoResource, QString(), QString()};

    const QString reference = stripMltDecorations(service, name, value);
    if (allowPseudoResources && isPseudoResource(service, name, reference))
        return {ReferenceKind::SafePseudoResource, QString(), QString()};
    if (reference.isEmpty())
        return {ReferenceKind::Rejected, QString(), QStringLiteral("empty filesystem reference")};
    if (isUncPath(reference)) {
        return {ReferenceKind::Rejected,
                QString(),
                QStringLiteral("UNC and network-share paths are not accepted through MCP")};
    }

    QString localPath;
    if (isWindowsDriveAbsolutePath(reference)) {
        localPath = reference;
    } else {
        QUrl url(reference);
        const QString scheme = url.scheme().toLower();
        if (!scheme.isEmpty()) {
            if (scheme == QStringLiteral("file")) {
                if (!url.host().isEmpty()
                    && url.host().compare(QStringLiteral("localhost"), Qt::CaseInsensitive) != 0) {
                    return {ReferenceKind::Rejected,
                            QString(),
                            QStringLiteral("network file URLs are not accepted through MCP")};
                }
                if (url.host().compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0)
                    url.setHost(QString());
                localPath = url.toLocalFile();
                if (localPath.isEmpty() || isUncPath(localPath)) {
                    return {ReferenceKind::Rejected,
                            QString(),
                            QStringLiteral("invalid or network-backed file URL")};
                }
            } else if (scheme == QStringLiteral("qrc")) {
                if (allowPseudoResources)
                    return {ReferenceKind::SafePseudoResource, QString(), QString()};
                return {ReferenceKind::Rejected,
                        QString(),
                        QStringLiteral("qrc resources cannot be used as an MLT project root")};
            } else {
                return {ReferenceKind::Rejected,
                        QString(),
                        QStringLiteral("URL scheme '%1' is not allowed").arg(scheme.left(32))};
            }
        } else {
            localPath = reference;
        }
    }

    if (QFileInfo(localPath).isRelative() && !isWindowsDriveAbsolutePath(localPath))
        localPath = baseDirectory.absoluteFilePath(localPath);
    return {ReferenceKind::LocalPath, QDir::cleanPath(localPath), QString()};
}
bool isAllowedProducerService(const QString &service)
{
    const QString lower = service.trimmed().toLower();
    static const QStringList services{
        QStringLiteral("avformat"),
        QStringLiteral("avformat-novalidate"),
        QStringLiteral("blank"),
        QStringLiteral("blipflash"),
        QStringLiteral("chain"),
        QStringLiteral("color"),
        QStringLiteral("colour"),
        QStringLiteral("count"),
        QStringLiteral("frei0r.test_pat_b"),
        QStringLiteral("hold"),
        QStringLiteral("luma"),
        QStringLiteral("noise"),
        QStringLiteral("pango"),
        QStringLiteral("pixbuf"),
        QStringLiteral("playlist"),
        QStringLiteral("qimage"),
        QStringLiteral("qtext"),
        QStringLiteral("timewarp"),
        QStringLiteral("tone"),
        QStringLiteral("tractor"),
        QStringLiteral("xml"),
        QStringLiteral("xml-clip"),
        QStringLiteral("xml-nogl"),
    };
    return lower.isEmpty() || services.contains(lower);
}

bool isQualifiedPathProperty(const QString &element, const QString &service, const QString &name)
{
    const QString lowerElement = element.toLower();
    const QString lowerService = service.trimmed().toLower();
    const QString lowerName = name.toLower();
    const bool producerLike = lowerElement == QStringLiteral("producer")
                              || lowerElement == QStringLiteral("video")
                              || lowerElement == QStringLiteral("chain");
    const bool commonQualified = lowerName == QStringLiteral("resource")
                                 || lowerName == QStringLiteral("luma")
                                 || lowerName == QStringLiteral("luma.resource")
                                 || lowerName == QStringLiteral("composite.luma")
                                 || lowerName == QStringLiteral("producer.resource");

    if (producerLike
        && (commonQualified || lowerName == QStringLiteral("src")
            || (lowerName == QStringLiteral("argument")
                && lowerService == QStringLiteral("timewarp")))) {
        return true;
    }
    if (lowerElement == QStringLiteral("filter") || lowerElement == QStringLiteral("transition")) {
        if (commonQualified || lowerName == QStringLiteral("src")
            || lowerName == QStringLiteral("filename") || lowerName == QStringLiteral("av.file")) {
            return true;
        }
        if (lowerElement == QStringLiteral("filter")
            && (lowerName == QStringLiteral("av.filename")
                || lowerName == QStringLiteral("filter.resource"))) {
            return true;
        }
    }
    if (lowerName == QStringLiteral("warp_resource"))
        return true;
    return producerLike && lowerName == QStringLiteral("shotcut:resource");
}

bool requiresAbsoluteReference(const QString &element, const QString &name)
{
    const QString lowerElement = element.toLower();
    const QString lowerName = name.toLower();
    if (lowerElement == QStringLiteral("transition")
        && (lowerName == QStringLiteral("src") || lowerName == QStringLiteral("filename")
            || lowerName == QStringLiteral("av.file"))) {
        return true;
    }
    static const QStringList names{
        QStringLiteral("warp_resource"),
        QStringLiteral("shotcut:resource"),
        QStringLiteral("gps.file"),
        QStringLiteral("bg_img_path"),
        QStringLiteral("results"),
        QStringLiteral("analysisfile"),
        QStringLiteral("shader_path"),
    };
    return names.contains(lowerName);
}

bool hasAbsoluteReferenceSyntax(const QString &service, const QString &name, const QString &value)
{
    const QString reference = stripMltDecorations(service, name, value);
    if (isWindowsDriveAbsolutePath(reference) || isUncPath(reference))
        return true;
    const QUrl url(reference);
    if (!url.scheme().isEmpty())
        return url.scheme().compare(QStringLiteral("file"), Qt::CaseInsensitive) == 0;
    return QFileInfo(reference).isAbsolute();
}

bool isSafeProfileIdentifier(const QString &value)
{
    if (value.isEmpty())
        return true;
    static const QRegularExpression identifier(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$"));
    return identifier.match(value).hasMatch();
}

QByteArray normalizedPrefix(QByteArray prefix)
{
    if (prefix.startsWith(QByteArray::fromHex("efbbbf")))
        prefix.remove(0, 3);
    return prefix.trimmed().toLower();
}

bool hasOpaqueContentSignature(const QByteArray &rawPrefix)
{
    const QByteArray prefix = normalizedPrefix(rawPrefix);
    if (prefix.startsWith("#extm3u") || prefix.startsWith("ffconcat version")
        || prefix.startsWith("[playlist]") || prefix.startsWith("<asx")
        || prefix.startsWith("<smil") || prefix.startsWith("<mpd")
        || prefix.startsWith("<smoothstreamingmedia") || prefix.startsWith("<html")
        || prefix.startsWith("<!doctype html") || prefix.startsWith("<kdenlivetitle")
        || prefix.startsWith("<svg") || prefix.startsWith("import qtquick")
        || prefix.startsWith("import shotcut")) {
        return true;
    }
    if (prefix.startsWith("<?xml")) {
        return prefix.contains("<asx") || prefix.contains("<smil") || prefix.contains("<mpd")
               || prefix.contains("<smoothstreamingmedia") || prefix.contains("<html")
               || prefix.contains("<kdenlivetitle") || prefix.contains("<svg");
    }
    if (prefix.startsWith("v=0") && prefix.contains("\no=") && prefix.contains("\ns="))
        return true;
    return prefix.startsWith('{') && prefix.contains("\"layers\"")
           && (prefix.contains("\"assets\"") || prefix.contains("\"fr\""));
}

bool looksLikeMltXml(const QByteArray &rawPrefix)
{
    const QByteArray prefix = normalizedPrefix(rawPrefix);
    return prefix.startsWith("<mlt") || prefix.startsWith("<westley")
           || (prefix.startsWith("<?xml")
               && (prefix.contains("<mlt") || prefix.contains("<westley")));
}
} // namespace

McpXmlPathValidator::McpXmlPathValidator(PathAuthorizer authorizePath,
                                         ServiceAuthorizer authorizeService)
    : m_authorizePath(std::move(authorizePath))
    , m_authorizeService(std::move(authorizeService))
{}

bool McpXmlPathValidator::authorizeLocalPath(const QString &path,
                                             bool mustExist,
                                             QString *normalized,
                                             QString *errorMessage) const
{
    if (!m_authorizePath || !m_authorizePath(path, mustExist, normalized)) {
        return fail(errorMessage,
                    QStringLiteral("filesystem reference is missing or outside allowed roots: %1")
                        .arg(QDir::toNativeSeparators(path).left(512)));
    }
    return true;
}

bool McpXmlPathValidator::validateProject(const QString &fileName, QString *errorMessage) const
{
    if (errorMessage)
        errorMessage->clear();
    ValidationState state;
    return validateProjectInternal(fileName, state, 0, errorMessage);
}

bool McpXmlPathValidator::validateMedia(const QString &fileName, QString *errorMessage) const
{
    if (errorMessage)
        errorMessage->clear();

    QString normalized;
    if (!authorizeLocalPath(fileName, true, &normalized, errorMessage))
        return false;
    if (!QFileInfo(normalized).isFile())
        return fail(errorMessage, QStringLiteral("media reference is not a regular file"));

    ValidationState state;
    const bool xmlIsMltProject
        = QFileInfo(normalized).suffix().compare(QStringLiteral("xml"), Qt::CaseInsensitive) == 0;
    return validateNormalizedMedia(normalized, xmlIsMltProject, state, 0, errorMessage);
}

bool McpXmlPathValidator::validateNormalizedMedia(const QString &fileName,
                                                  bool xmlIsMltProject,
                                                  ValidationState &state,
                                                  int depth,
                                                  QString *errorMessage) const
{
    if (isOpaqueLoaderSuffix(fileName)) {
        return fail(errorMessage,
                    QStringLiteral("playlist, manifest, markup, and active-content media loaders "
                                   "are not accepted through MCP"));
    }

    QByteArray prefix;
    const QFileInfo info(fileName);
    if (info.exists()) {
        QFile file(fileName);
        if (!file.open(QIODevice::ReadOnly))
            return fail(errorMessage, QStringLiteral("media reference could not be inspected"));
        prefix = file.read(kSignatureBytes);
        if (hasOpaqueContentSignature(prefix)) {
            return fail(errorMessage,
                        QStringLiteral("media content uses a nested or active-content loader that "
                                       "is not accepted through MCP"));
        }
    }

    const QString suffix = info.suffix().toLower();
    const bool projectCandidate = xmlIsMltProject || isMltLoaderSuffix(fileName)
                                  || looksLikeMltXml(prefix);
    if (!projectCandidate)
        return true;
    if (fileName.contains(QLatin1Char('%')) || fileName.contains(QLatin1Char('?'))
        || fileName.contains(QLatin1Char('#'))) {
        return fail(errorMessage,
                    QStringLiteral("encoded or parameterized nested MLT project references are "
                                   "not accepted through MCP"));
    }
    return validateProjectInternal(fileName, state, depth, errorMessage);
}
bool McpXmlPathValidator::validateProjectInternal(const QString &fileName,
                                                  ValidationState &state,
                                                  int depth,
                                                  QString *errorMessage) const
{
    QString normalized;
    if (!authorizeLocalPath(fileName, true, &normalized, errorMessage))
        return false;
    const QFileInfo projectInfo(normalized);
    if (!projectInfo.isFile())
        return fail(errorMessage, QStringLiteral("project reference is not a regular file"));
    if (state.activeProjects.contains(normalized))
        return fail(errorMessage, QStringLiteral("nested MLT project cycle is not accepted"));
    if (state.validatedProjects.contains(normalized))
        return true;
    if (depth > kMaximumNestedProjectDepth) {
        return fail(errorMessage,
                    QStringLiteral("nested project depth exceeds the MCP safety limit"));
    }
    const qint64 projectBytes = projectInfo.size();
    if (projectBytes < 0 || projectBytes > kMaximumProjectBytes - state.projectBytes) {
        return fail(errorMessage,
                    QStringLiteral("aggregate nested project size exceeds the MCP safety limit"));
    }
    state.projectBytes += projectBytes;
    state.activeProjects.insert(normalized);
    ActiveProjectGuard activeGuard{state.activeProjects, normalized};

    QFile file(normalized);
    if (!file.open(QIODevice::ReadOnly))
        return fail(errorMessage, QStringLiteral("project XML could not be opened for validation"));

    QDir baseDirectory(projectInfo.canonicalPath());
    QXmlStreamReader xml(&file);
    QVector<MltElement> elements;
    bool sawMltRoot = false;
    int documentDepth = 0;

    auto validateElement = [&](const MltElement &element) {
        QString service;
        QString filterId;
        QString nestedFilterService;
        QSet<QString> seenDiscriminators;
        for (const auto &property : element.properties) {
            const QString name = property.first.toLower();
            const bool isDiscriminator = name == QStringLiteral("mlt_service")
                                         || name == QStringLiteral("shotcut:filter")
                                         || name == QStringLiteral("shotcut:proxy")
                                         || name == QStringLiteral("filter");
            if (isDiscriminator && seenDiscriminators.contains(name)) {
                return fail(errorMessage,
                            QStringLiteral("duplicate MLT service or Shotcut discriminator is not "
                                           "accepted through MCP"));
            }
            if (isDiscriminator)
                seenDiscriminators.insert(name);
            if (name == QStringLiteral("mlt_service"))
                service = property.second.trimmed();
            else if (name == QStringLiteral("shotcut:filter"))
                filterId = property.second.trimmed();
            else if (name == QStringLiteral("filter"))
                nestedFilterService = property.second;
        }

        const QString lowerService = service.toLower();
        const QString lowerFilterId = filterId.toLower();
        const bool producerLoader
            = element.name.compare(QStringLiteral("producer"), Qt::CaseInsensitive) == 0
              || element.name.compare(QStringLiteral("video"), Qt::CaseInsensitive) == 0
              || element.name.compare(QStringLiteral("chain"), Qt::CaseInsensitive) == 0;
        if (producerLoader && lowerService == QStringLiteral("webvfx")) {
            return fail(errorMessage,
                        QStringLiteral("WebVfx content loaders are not accepted in projects opened "
                                       "through MCP"));
        }
        if (producerLoader && isInlineMltXmlService(lowerService)) {
            return fail(errorMessage,
                        QStringLiteral("inline MLT XML producers are not accepted through MCP"));
        }
        if (producerLoader && !isAllowedProducerService(lowerService)) {
            return fail(errorMessage,
                        QStringLiteral("unknown or active MLT producer loaders are not accepted "
                                       "through MCP"));
        }
        const bool filterElement = element.name.compare(QStringLiteral("filter"),
                                                        Qt::CaseInsensitive)
                                   == 0;
        const bool transitionElement = element.name.compare(QStringLiteral("transition"),
                                                            Qt::CaseInsensitive)
                                       == 0;
        if ((filterElement || transitionElement)
            && (!m_authorizeService || !m_authorizeService(element.name, service, filterId))) {
            return fail(errorMessage,
                        QStringLiteral("filter or transition service is not approved for "
                                       "noninteractive MCP project opening"));
        }
        if (filterElement && !McpBridgePolicy::filterIdentitiesActiveAllowed(filterId, service)) {
            return fail(errorMessage,
                        QStringLiteral("active-content and manual-only filter services are not "
                                       "accepted through MCP"));
        }
        if (filterElement && lowerService == QStringLiteral("mask_start")
            && seenDiscriminators.contains(QStringLiteral("filter"))) {
            if (!McpBridgePolicy::nestedFilterParameterWriteAllowed(service,
                                                                    QStringLiteral("filter"),
                                                                    nestedFilterService,
                                                                    false)) {
                return fail(errorMessage,
                            QStringLiteral("active or unknown nested mask filters are not accepted "
                                           "through MCP"));
            }
        }
        for (const auto &property : element.properties) {
            const QString name = property.first;
            const QString lowerName = name.toLower();
            const QString value = property.second;

            if (filterElement && !McpBridgePolicy::avFilterPropertyAllowed(service, name)) {
                return fail(errorMessage,
                            QStringLiteral("unapproved dynamic AVFilter options are not accepted "
                                           "through MCP"));
            }
            if (!McpBridgePolicy::nestedFilterPathParameterWriteAllowed(service, name, false)) {
                return fail(errorMessage,
                            QStringLiteral("unsupported nested mask filter path properties are "
                                           "not accepted through MCP"));
            }
            if (!McpBridgePolicy::maskApplyTransitionParameterWriteAllowed(service,
                                                                           name,
                                                                           value,
                                                                           false)) {
                return fail(errorMessage,
                            QStringLiteral("Mask: Apply transition service must be qtblend for "
                                           "noninteractive MCP project opening"));
            }
            if (!McpBridgePolicy::nestedTransitionPathParameterWriteAllowed(service,
                                                                            name,
                                                                            value.isEmpty())) {
                return fail(errorMessage,
                            QStringLiteral("unsupported nested Mask: Apply transition path "
                                           "properties are not accepted through MCP"));
            }
            if (filterElement
                && !McpBridgePolicy::affineBackgroundParameterWriteAllowed(service,
                                                                           name,
                                                                           value,
                                                                           false)) {
                return fail(errorMessage,
                            QStringLiteral("affine filter background must be an exact built-in "
                                           "color or colour producer for noninteractive MCP "
                                           "project opening"));
            }
            if (filterElement
                && !McpBridgePolicy::nestedProducerPathParameterWriteAllowed(service,
                                                                             name,
                                                                             value.isEmpty())) {
                return fail(errorMessage,
                            QStringLiteral("unsupported nested affine producer path properties are "
                                           "not accepted through MCP"));
            }
            if (filterElement
                && !McpBridgePolicy::dustFactoryParameterWriteAllowed(service, name, false)) {
                return fail(errorMessage,
                            QStringLiteral("explicit Dust producer factories are not accepted "
                                           "through MCP"));
            }
            if (transitionElement
                && !McpBridgePolicy::transitionProducerFactoryParameterWriteAllowed(service,
                                                                                    name,
                                                                                    value,
                                                                                    false)) {
                return fail(errorMessage,
                            QStringLiteral("transition producer factory must be empty or exactly "
                                           "loader for noninteractive MCP project opening"));
            }
            if (transitionElement
                && !McpBridgePolicy::nestedTransitionProducerPathAllowed(service, name)) {
                return fail(errorMessage,
                            QStringLiteral("nested transition producer and luma paths are not "
                                           "accepted through MCP"));
            }

            const bool richTextIdentity = lowerFilterId == QStringLiteral("richtext")
                                          || lowerFilterId == QStringLiteral("qtext")
                                          || lowerService == QStringLiteral("richtext")
                                          || lowerService == QStringLiteral("qtext");
            if (richTextIdentity
                && (lowerName == QStringLiteral("html") || lowerName == QStringLiteral("resource"))
                && !value.isEmpty()) {
                return fail(errorMessage,
                            QStringLiteral("rich-text external content is not accepted in projects "
                                           "opened through MCP"));
            }
            if (lowerService == QStringLiteral("webvfx") && lowerName == QStringLiteral("resource")
                && !value.isEmpty()) {
                return fail(errorMessage,
                            QStringLiteral("WebVfx content loaders are not accepted in projects "
                                           "opened through MCP"));
            }
            if (isInlineMltXmlService(lowerService) && lowerName == QStringLiteral("resource")
                && !value.isEmpty()) {
                return fail(errorMessage,
                            QStringLiteral(
                                "inline MLT XML producers are not accepted through MCP"));
            }

            const auto filterPathKind
                = McpBridgePolicy::safestFilterPathKind(McpBridgePolicy::filterPathKind(filterId,
                                                                                        name),
                                                        McpBridgePolicy::filterPathKind(service,
                                                                                        name));
            const bool knownPath = isQualifiedPathProperty(element.name, service, name)
                                   || filterPathKind != McpBridgePolicy::FilterPathKind::NotPath;
            if (!knownPath)
                continue;
            if (isPseudoResource(service, name, value))
                continue;
            if (value.contains(QLatin1Char('?')) || value.contains(QLatin1Char('#'))) {
                return fail(errorMessage,
                            QStringLiteral("query and fragment path decorations are not accepted "
                                           "through MCP"));
            }
            if (value.startsWith(QStringLiteral("plain:"), Qt::CaseInsensitive)) {
                return fail(errorMessage,
                            QStringLiteral("plain: path decorations are not accepted through MCP"));
            }
            if (isMltXmlFileService(lowerService)
                && (value.contains(QLatin1Char('%')) || value.contains(QLatin1Char('?'))
                    || value.contains(QLatin1Char('#')))) {
                return fail(errorMessage,
                            QStringLiteral("encoded or parameterized nested MLT project references "
                                           "are not accepted through MCP"));
            }
            if (requiresAbsoluteReference(element.name, name)
                && !hasAbsoluteReferenceSyntax(service, name, value)) {
                return fail(errorMessage,
                            QStringLiteral(
                                "this plugin path must be an explicit absolute local path "
                                "when opened through MCP"));
            }
            const auto reference = resolveReference(service, name, value, baseDirectory, true);
            if (reference.kind == ReferenceKind::SafePseudoResource)
                continue;
            if (reference.kind == ReferenceKind::Rejected)
                return fail(errorMessage, reference.error);
            if (reference.path.contains(QLatin1Char('*'))
                || reference.path.contains(QLatin1Char('?'))
                || reference.path.contains(QLatin1Char('['))
                || reference.path.contains(QLatin1Char(']'))) {
                return fail(errorMessage,
                            QStringLiteral("filesystem glob syntax is not accepted through MCP"));
            }
            const bool lutFile = McpBridgePolicy::isLutFileParameter(service, name)
                                 || McpBridgePolicy::isLutFileParameter(filterId, name);
            const QUrl rawReference(value);
            if (lutFile
                && ((!isWindowsDriveAbsolutePath(value) && !rawReference.scheme().isEmpty())
                    || QFileInfo(reference.path).isSymLink()
                    || reference.path.contains(QLatin1Char('%')))) {
                return fail(errorMessage,
                            QStringLiteral("LUT filter files must use a non-symlink local path"));
            }
            if (++state.referenceCount > kMaximumResourceReferences) {
                return fail(errorMessage,
                            QStringLiteral("project resource count exceeds the MCP safety limit"));
            }

            const bool readReference = filterPathKind
                                       != McpBridgePolicy::FilterPathKind::WritablePath;
            if (QFileInfo(reference.path)
                    .fileName()
                    .contains(QStringLiteral(".all."), Qt::CaseInsensitive)) {
                return fail(errorMessage,
                            QStringLiteral("all-files image sequence syntax is not accepted "
                                           "through MCP"));
            }
            const auto sequence = imageSequencePattern(reference.path);
            if (sequence.status == ImageSequenceStatus::Invalid) {
                return fail(errorMessage,
                            QStringLiteral("image sequences require exactly one %d or %0Nd token "
                                           "in the file name"));
            }
            if (sequence.status == ImageSequenceStatus::Valid) {
                if (readReference && isOpaqueLoaderSuffix(reference.path)) {
                    return fail(errorMessage,
                                QStringLiteral("playlist, manifest, markup, and active-content "
                                               "image sequences are not accepted through MCP"));
                }

                QString safePattern;
                if (!authorizeLocalPath(reference.path, false, &safePattern, errorMessage))
                    return false;
                const QFileInfo safePatternInfo(safePattern);
                if (safePatternInfo.exists() && !safePatternInfo.isFile()) {
                    return fail(errorMessage,
                                QStringLiteral("image sequence pattern is not a regular file"));
                }

                QString safeDirectory;
                const QString patternDirectory = QFileInfo(reference.path).absolutePath();
                if (!authorizeLocalPath(patternDirectory, true, &safeDirectory, errorMessage))
                    return false;
                if (!QFileInfo(safeDirectory).isDir()) {
                    return fail(errorMessage,
                                QStringLiteral("image sequence parent is not a directory"));
                }

                int inspectedEntries = 0;
                int sequenceMembers = 0;
                QDirIterator iterator(safeDirectory,
                                      QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden
                                          | QDir::System,
                                      QDirIterator::NoIteratorFlags);
                while (iterator.hasNext()) {
                    iterator.next();
                    if (++inspectedEntries > kMaximumSequenceDirectoryEntries) {
                        return fail(errorMessage,
                                    QStringLiteral("image sequence directory exceeds the MCP scan "
                                                   "limit"));
                    }
                    const QFileInfo entry = iterator.fileInfo();
                    if (!sequence.fileNameExpression.match(entry.fileName()).hasMatch())
                        continue;
                    if (++sequenceMembers > kMaximumSequenceMembers) {
                        return fail(errorMessage,
                                    QStringLiteral("image sequence member count exceeds the MCP "
                                                   "safety limit"));
                    }
                    if (++state.referenceCount > kMaximumResourceReferences) {
                        return fail(errorMessage,
                                    QStringLiteral("project resource count exceeds the MCP safety "
                                                   "limit"));
                    }

                    QString safeMember;
                    if (!authorizeLocalPath(entry.absoluteFilePath(),
                                            true,
                                            &safeMember,
                                            errorMessage))
                        return false;
                    const QFileInfo safeMemberInfo(safeMember);
                    if (!safeMemberInfo.isFile()) {
                        return fail(errorMessage,
                                    QStringLiteral("image sequence member is not a regular file"));
                    }
                    const bool memberXmlIsMltProject
                        = producerLoader && lowerService.isEmpty()
                          && safeMemberInfo.suffix().compare(QStringLiteral("xml"),
                                                             Qt::CaseInsensitive)
                                 == 0;
                    if (readReference
                        && !validateNormalizedMedia(safeMember,
                                                    memberXmlIsMltProject,
                                                    state,
                                                    depth + 1,
                                                    errorMessage)) {
                        return false;
                    }
                }
                if (readReference && sequenceMembers == 0) {
                    return fail(errorMessage,
                                QStringLiteral("image sequence does not contain any readable "
                                               "members"));
                }
                continue;
            }

            const bool mustExist = readReference;
            QString safePath;
            if (!authorizeLocalPath(reference.path, mustExist, &safePath, errorMessage))
                return false;
            const QFileInfo safeInfo(safePath);
            if (mustExist && !safeInfo.isFile()) {
                return fail(errorMessage,
                            QStringLiteral("project resource is not a regular file: %1")
                                .arg(QDir::toNativeSeparators(safePath).left(512)));
            }
            if (!mustExist && safeInfo.exists() && !safeInfo.isFile()) {
                return fail(errorMessage,
                            QStringLiteral("project output reference is not a regular file"));
            }
            if (!McpBridgePolicy::filterInputSuffixAllowed(service, name, safeInfo.suffix())
                || !McpBridgePolicy::filterInputSuffixAllowed(filterId, name, safeInfo.suffix())) {
                return fail(errorMessage,
                            QStringLiteral("LUT filter files must use a supported LUT extension"));
            }
            const bool xmlIsMltProject = isMltXmlFileService(lowerService)
                                         || (producerLoader && lowerService.isEmpty()
                                             && safeInfo.suffix().compare(QStringLiteral("xml"),
                                                                          Qt::CaseInsensitive)
                                                    == 0);
            if (readReference
                && !validateNormalizedMedia(safePath,
                                            xmlIsMltProject,
                                            state,
                                            depth + 1,
                                            errorMessage)) {
                return false;
            }
        }
        return true;
    };
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference
            || token == QXmlStreamReader::ProcessingInstruction) {
            return fail(errorMessage,
                        QStringLiteral("DTD, entity, and processing-instruction XML constructs are "
                                       "not accepted through MCP"));
        }
        if (token == QXmlStreamReader::StartElement) {
            if (++documentDepth > kMaximumXmlDepth)
                return fail(errorMessage, QStringLiteral("project XML nesting is too deep"));
            if (++state.xmlElementCount > kMaximumXmlElements)
                return fail(errorMessage, QStringLiteral("project XML element limit exceeded"));

            const QString elementName = xml.name().toString();
            const QString lowerElement = elementName.toLower();
            QSet<QString> seenSensitiveAttributes;
            int profileSelectorCount = 0;
            for (const auto &attribute : xml.attributes()) {
                if (attribute.name().size() > kMaximumPropertyNameCharacters
                    || attribute.value().size() > kMaximumPropertyValueCharacters) {
                    return fail(errorMessage,
                                QStringLiteral("project XML attribute exceeds the safety limit"));
                }
                const QString attributeName = attribute.name().toString().toLower();
                const bool isSensitive = attributeName == QStringLiteral("root")
                                         || attributeName == QStringLiteral("profile")
                                         || attributeName == QStringLiteral("name")
                                         || attributeName == QStringLiteral("mlt_service")
                                         || attributeName == QStringLiteral("shotcut:filter")
                                         || attributeName == QStringLiteral("shotcut:proxy")
                                         || (lowerElement == QStringLiteral("property")
                                             && attributeName == QStringLiteral("value"));
                if (isSensitive && seenSensitiveAttributes.contains(attributeName)) {
                    return fail(errorMessage,
                                QStringLiteral(
                                    "duplicate security-sensitive XML attributes are not "
                                    "accepted through MCP"));
                }
                if (isSensitive)
                    seenSensitiveAttributes.insert(attributeName);
                if (lowerElement == QStringLiteral("property")
                    && (attributeName == QStringLiteral("name")
                        || attributeName == QStringLiteral("value"))
                    && attribute.name().toString() != attributeName) {
                    return fail(errorMessage,
                                QStringLiteral("MLT property name and value attributes must use "
                                               "canonical lowercase spelling"));
                }
                if ((lowerElement == QStringLiteral("mlt")
                     || lowerElement == QStringLiteral("profile")
                     || lowerElement == QStringLiteral("profileinfo"))
                    && (attributeName == QStringLiteral("name")
                        || attributeName == QStringLiteral("profile"))
                    && !attribute.value().isEmpty() && ++profileSelectorCount > 1) {
                    return fail(errorMessage,
                                QStringLiteral("multiple MLT profile selectors are not accepted"));
                }
            }
            if (lowerElement == QStringLiteral("consumer")) {
                return fail(errorMessage,
                            QStringLiteral("MLT consumer elements are not accepted through MCP"));
            }
            if (lowerElement == QStringLiteral("link")) {
                return fail(errorMessage,
                            QStringLiteral("MLT link elements are not accepted through MCP"));
            }
            if (sawMltRoot
                && (lowerElement == QStringLiteral("mlt")
                    || lowerElement == QStringLiteral("westley"))) {
                return fail(errorMessage,
                            QStringLiteral("secondary MLT or westley roots are not accepted"));
            }
            if (lowerElement == QStringLiteral("mlt") || lowerElement == QStringLiteral("profile")
                || lowerElement == QStringLiteral("profileinfo")) {
                for (const auto &attribute : xml.attributes()) {
                    const QString attributeName = attribute.name().toString().toLower();
                    if ((attributeName == QStringLiteral("name")
                         || attributeName == QStringLiteral("profile"))
                        && !isSafeProfileIdentifier(attribute.value().toString())) {
                        return fail(errorMessage,
                                    QStringLiteral("external or path-like MLT profiles are not "
                                                   "accepted through MCP"));
                    }
                }
            }
            if (!sawMltRoot) {
                if (lowerElement != QStringLiteral("mlt"))
                    return fail(errorMessage, QStringLiteral("file is not an MLT XML project"));
                sawMltRoot = true;

                const QString root = xml.attributes().value(QStringLiteral("root")).toString();
                if (!root.isEmpty()) {
                    if (isUncPath(root)
                        || (!QFileInfo(root).isAbsolute() && !isWindowsDriveAbsolutePath(root))) {
                        return fail(errorMessage,
                                    QStringLiteral("MLT project roots must be canonical absolute "
                                                   "local directories"));
                    }
                    const auto resolvedRoot = resolveReference(QString(),
                                                               QStringLiteral("root"),
                                                               root,
                                                               baseDirectory,
                                                               false);
                    if (resolvedRoot.kind != ReferenceKind::LocalPath)
                        return fail(errorMessage, resolvedRoot.error);
                    QString safeRoot;
                    if (!authorizeLocalPath(resolvedRoot.path, true, &safeRoot, errorMessage))
                        return false;
                    if (!QFileInfo(safeRoot).isDir())
                        return fail(errorMessage,
                                    QStringLiteral("MLT project root is not a directory"));
                    const QString cleanRoot = QDir::fromNativeSeparators(QDir::cleanPath(root));
#ifdef Q_OS_WIN
                    constexpr auto comparison = Qt::CaseInsensitive;
#else
                    constexpr auto comparison = Qt::CaseSensitive;
#endif
                    if (cleanRoot.compare(safeRoot, comparison) != 0) {
                        return fail(errorMessage,
                                    QStringLiteral("MLT project root must already be canonical"));
                    }
                    baseDirectory.setPath(safeRoot);
                }
            }
            if (isMltElement(elementName)) {
                MltElement element;
                element.name = elementName;
                for (const auto &attribute : xml.attributes()) {
                    element.properties.append(
                        qMakePair(attribute.name().toString(), attribute.value().toString()));
                }
                elements.append(element);
            } else if (lowerElement == QStringLiteral("property") && !elements.isEmpty()) {
                const auto attributes = xml.attributes();
                if (!attributes.hasAttribute(QStringLiteral("name")))
                    return fail(errorMessage, QStringLiteral("MLT property is missing its name"));
                const QString propertyName = attributes.value(QStringLiteral("name")).toString();
                if (propertyName.isEmpty())
                    return fail(errorMessage, QStringLiteral("MLT property name is empty"));
                if (propertyName.size() > kMaximumPropertyNameCharacters)
                    return fail(errorMessage, QStringLiteral("project property name is too long"));
                const bool hasAttributeValue = attributes.hasAttribute(QStringLiteral("value"));
                const QString attributeValue = attributes.value(QStringLiteral("value")).toString();
                const QString textValue = xml.readElementText(
                    QXmlStreamReader::ErrorOnUnexpectedElement);
                --documentDepth;
                if (xml.hasError()) {
                    return fail(errorMessage,
                                QStringLiteral("nested elements are not accepted inside MLT "
                                               "properties"));
                }
                if (hasAttributeValue && !textValue.isEmpty()) {
                    return fail(errorMessage,
                                QStringLiteral("ambiguous MLT property values are not accepted"));
                }
                const QString propertyValue = hasAttributeValue ? attributeValue : textValue;
                if (propertyValue.size() > kMaximumPropertyValueCharacters)
                    return fail(errorMessage, QStringLiteral("project property value is too large"));
                elements.last().properties.append(qMakePair(propertyName, propertyValue));
            }
        } else if (token == QXmlStreamReader::EndElement) {
            --documentDepth;
            if (!elements.isEmpty()
                && elements.last().name.compare(xml.name().toString(), Qt::CaseInsensitive) == 0) {
                const MltElement element = elements.takeLast();
                if (!validateElement(element))
                    return false;
            }
        }
    }

    if (xml.hasError()) {
        return fail(errorMessage,
                    QStringLiteral("project XML is invalid: %1").arg(xml.errorString().left(512)));
    }
    if (!sawMltRoot)
        return fail(errorMessage, QStringLiteral("file is not an MLT XML project"));
    state.validatedProjects.insert(normalized);
    return true;
}
