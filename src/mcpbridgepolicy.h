/*
 * Copyright (c) 2026 Meltytech, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef MCPBRIDGEPOLICY_H
#define MCPBRIDGEPOLICY_H

#include <QJsonValue>
#include <QString>
#include <QStringList>

namespace McpBridgePolicy {
enum class FilterPathKind { NotPath, ExistingFile, WritablePath };
enum class FilterApplicability { Allowed, RequiresGpu, GpuIncompatible, ReverseUnsupported };

inline bool cStringValueAllowed(const QString &value)
{
    return !value.contains(QChar(u'\0'));
}

inline bool filterStringOrResetValue(const QJsonValue &value, QString *rawValue, bool *reset)
{
    if (value.isNull()) {
        if (rawValue)
            rawValue->clear();
        if (reset)
            *reset = true;
        return true;
    }
    if (!value.isString())
        return false;
    if (rawValue)
        *rawValue = value.toString();
    if (reset)
        *reset = false;
    return true;
}

inline bool maskStartSelectorValue(const QJsonValue &value, QString *rawValue, bool *reset)
{
    if (filterStringOrResetValue(value, rawValue, reset))
        return true;
    if (!value.isDouble() || value.toDouble() != 0.0)
        return false;
    if (rawValue)
        *rawValue = QStringLiteral("0");
    if (reset)
        *reset = false;
    return true;
}

inline bool filterParameterNameAllowed(const QString &name)
{
    if (name.isEmpty() || name.size() > 256 || name.startsWith(QLatin1Char('_'))
        || name == QStringLiteral("mlt_service") || name.startsWith(QStringLiteral("shotcut:"))) {
        return false;
    }
    for (const QChar character : name) {
        const auto code = character.unicode();
        if (code < 0x20 || code == 0x7f)
            return false;
    }
    return true;
}

inline FilterApplicability clipFilterApplicability(bool gpuMode,
                                                   bool needsGpu,
                                                   bool gpuCompatible,
                                                   bool seekReverse,
                                                   const QString &producerService)
{
    if (!gpuMode && needsGpu)
        return FilterApplicability::RequiresGpu;
    if (gpuMode && !gpuCompatible)
        return FilterApplicability::GpuIncompatible;
    if (seekReverse && producerService == QStringLiteral("xml-clip"))
        return FilterApplicability::ReverseUnsupported;
    return FilterApplicability::Allowed;
}

inline bool activeFilterFamilyManualOnly(const QString &identity)
{
    const QString id = identity.trimmed().toLower();
    static const QStringList families{
        QStringLiteral("jack"),
        QStringLiteral("jackrack"),
        QStringLiteral("ladspa"),
        QStringLiteral("lv2"),
        QStringLiteral("vst2"),
        QStringLiteral("openfx"),
    };
    for (const QString &family : families) {
        if (id == family || id.startsWith(family + QLatin1Char('.')))
            return true;
    }
    return false;
}

inline bool activeFilterServiceAllowed(const QString &service)
{
    const QString id = service.trimmed().toLower();
    if (id == QStringLiteral("placebo.shader") || id == QStringLiteral("opencv.tracker")
        || activeFilterFamilyManualOnly(id)) {
        return false;
    }
    if (id == QStringLiteral("avfilter") || id.startsWith(QStringLiteral("avfilter."))) {
        static const QStringList supported{
            QStringLiteral("avfilter.lut3d"),
            QStringLiteral("avfilter.adeclick"),
            QStringLiteral("avfilter.deband"),
            QStringLiteral("avfilter.random"),
            QStringLiteral("avfilter.vaguedenoiser"),
            QStringLiteral("avfilter.vibrance"),
            QStringLiteral("avfilter.chromahold"),
            QStringLiteral("avfilter.haas"),
            QStringLiteral("avfilter.smartblur"),
            QStringLiteral("avfilter.tmix"),
            QStringLiteral("avfilter.hue"),
            QStringLiteral("avfilter.fspp"),
            QStringLiteral("avfilter.gblur"),
            QStringLiteral("avfilter.vflip"),
            QStringLiteral("avfilter.hflip"),
            QStringLiteral("avfilter.noise"),
        };
        return supported.contains(id);
    }
    return true;
}

inline bool filterIdentitiesActiveAllowed(const QString &filterId, const QString &service)
{
    return activeFilterServiceAllowed(filterId) && activeFilterServiceAllowed(service);
}

inline bool attachedFilterIdentityAllowed(const QString &filterId,
                                          const QString &metadataService,
                                          const QString &actualService,
                                          bool actualIsFilter,
                                          bool bundledPairAllowed)
{
    const QString expected = metadataService.trimmed();
    const QString actual = actualService.trimmed();
    return actualIsFilter && bundledPairAllowed && !filterId.trimmed().isEmpty()
           && !actual.isEmpty() && expected.compare(actual, Qt::CaseInsensitive) == 0
           && filterIdentitiesActiveAllowed(filterId, actual);
}

inline bool coreTransitionServiceAllowed(const QString &service)
{
    const QString id = service.trimmed().toLower();
    static const QStringList services{
        QStringLiteral("affine"),
        QStringLiteral("composite"),
        QStringLiteral("frei0r.cairoblend"),
        QStringLiteral("luma"),
        QStringLiteral("mix"),
        QStringLiteral("movit.luma_mix"),
        QStringLiteral("movit.overlay"),
        QStringLiteral("qtblend"),
    };
    return services.contains(id);
}

inline bool nestedFilterParameterWriteAllowed(const QString &service,
                                              const QString &name,
                                              const QString &value,
                                              bool reset)
{
    if (service.trimmed().compare(QStringLiteral("mask_start"), Qt::CaseInsensitive) != 0
        || name.trimmed().compare(QStringLiteral("filter"), Qt::CaseInsensitive) != 0) {
        return true;
    }
    if (reset)
        return true;
    static const QStringList allowed{
        QString(),
        QStringLiteral("0"),
        QStringLiteral("shape"),
        QStringLiteral("frei0r.alphaspot"),
        QStringLiteral("frei0r.bluescreen0r"),
    };
    return allowed.contains(value);
}

inline bool sensitiveNestedPathTail(const QString &tail)
{
    static const QStringList pathNames{
        QStringLiteral("resource"),
        QStringLiteral("src"),
        QStringLiteral("filename"),
        QStringLiteral("file"),
        QStringLiteral("path"),
        QStringLiteral("url"),
        QStringLiteral("av.file"),
        QStringLiteral("av.filename"),
        QStringLiteral("filter.resource"),
        QStringLiteral("gps.file"),
        QStringLiteral("bg_img_path"),
        QStringLiteral("shader_path"),
        QStringLiteral("results"),
        QStringLiteral("analysisfile"),
        QStringLiteral("warp_resource"),
        QStringLiteral("luma"),
        QStringLiteral("luma.resource"),
        QStringLiteral("producer.resource"),
    };
    return pathNames.contains(tail.trimmed().toLower());
}

inline bool nestedFilterPathParameterWriteAllowed(const QString &service,
                                                  const QString &name,
                                                  bool reset)
{
    if (service.trimmed().compare(QStringLiteral("mask_start"), Qt::CaseInsensitive) != 0)
        return true;
    const QString key = name.trimmed().toLower();
    if (!key.startsWith(QStringLiteral("filter.")) || key == QStringLiteral("filter.resource"))
        return true;
    return reset || !sensitiveNestedPathTail(key.mid(7));
}

inline bool maskApplyTransitionServiceAllowed(const QString &service,
                                              const QString &transitionService)
{
    if (service.trimmed().compare(QStringLiteral("mask_apply"), Qt::CaseInsensitive) != 0)
        return true;
    return transitionService.isEmpty() || transitionService == QStringLiteral("qtblend");
}

inline bool maskApplyTransitionParameterWriteAllowed(const QString &service,
                                                     const QString &name,
                                                     const QString &value,
                                                     bool reset)
{
    if (service.trimmed().compare(QStringLiteral("mask_apply"), Qt::CaseInsensitive) != 0
        || name.trimmed().compare(QStringLiteral("transition"), Qt::CaseInsensitive) != 0) {
        return true;
    }
    return reset || value.isEmpty() || value == QStringLiteral("qtblend");
}

inline bool nestedTransitionPathParameterWriteAllowed(const QString &service,
                                                      const QString &name,
                                                      bool reset)
{
    if (service.trimmed().compare(QStringLiteral("mask_apply"), Qt::CaseInsensitive) != 0)
        return true;
    const QString key = name.trimmed().toLower();
    if (!key.startsWith(QStringLiteral("transition.")))
        return true;
    return reset || !sensitiveNestedPathTail(key.mid(11));
}

inline bool affineColorProducerAllowed(const QString &background)
{
    QString payload;
    if (background.startsWith(QStringLiteral("color:")))
        payload = background.mid(6);
    else if (background.startsWith(QStringLiteral("colour:")))
        payload = background.mid(7);
    else
        return false;

    if (payload == QStringLiteral("0"))
        return true;
    if (!payload.startsWith(QLatin1Char('#')))
        return false;
    const qsizetype digits = payload.size() - 1;
    if (digits != 3 && digits != 4 && digits != 6 && digits != 8)
        return false;
    for (qsizetype i = 1; i < payload.size(); ++i) {
        const ushort code = payload.at(i).unicode();
        if (!((code >= '0' && code <= '9') || (code >= 'a' && code <= 'f')
              || (code >= 'A' && code <= 'F'))) {
            return false;
        }
    }
    return true;
}

inline bool affineBackgroundServiceAllowed(const QString &service, const QString &background)
{
    if (service.trimmed().compare(QStringLiteral("affine"), Qt::CaseInsensitive) != 0)
        return true;
    return background.isEmpty() || affineColorProducerAllowed(background);
}

inline bool affineBackgroundParameterWriteAllowed(const QString &service,
                                                  const QString &name,
                                                  const QString &value,
                                                  bool reset)
{
    if (service.trimmed().compare(QStringLiteral("affine"), Qt::CaseInsensitive) != 0
        || name.trimmed().compare(QStringLiteral("background"), Qt::CaseInsensitive) != 0) {
        return true;
    }
    return reset || affineColorProducerAllowed(value);
}

inline bool nestedProducerPathParameterWriteAllowed(const QString &service,
                                                    const QString &name,
                                                    bool reset)
{
    if (service.trimmed().compare(QStringLiteral("affine"), Qt::CaseInsensitive) != 0)
        return true;
    const QString key = name.trimmed().toLower();
    if (!key.startsWith(QStringLiteral("producer.")))
        return true;
    return reset || !sensitiveNestedPathTail(key.mid(9));
}

inline bool dustFactoryServiceAllowed(const QString &service, const QString &factory)
{
    if (service.trimmed().compare(QStringLiteral("dust"), Qt::CaseInsensitive) != 0)
        return true;
    return factory.isEmpty();
}

inline bool dustFactoryParameterWriteAllowed(const QString &service, const QString &name, bool reset)
{
    if (service.trimmed().compare(QStringLiteral("dust"), Qt::CaseInsensitive) != 0
        || name.trimmed().compare(QStringLiteral("factory"), Qt::CaseInsensitive) != 0) {
        return true;
    }
    return reset;
}

inline bool transitionProducerFactoryAllowed(const QString &service, const QString &factory)
{
    const QString id = service.trimmed().toLower();
    if (id != QStringLiteral("luma") && id != QStringLiteral("composite"))
        return true;
    return factory.isEmpty() || factory == QStringLiteral("loader");
}

inline bool isTransitionProducerFactoryParameter(const QString &service, const QString &name)
{
    const QString id = service.trimmed().toLower();
    return (id == QStringLiteral("luma") || id == QStringLiteral("composite"))
           && name.trimmed().compare(QStringLiteral("factory"), Qt::CaseInsensitive) == 0;
}

inline bool transitionProducerFactoryParameterWriteAllowed(const QString &service,
                                                           const QString &name,
                                                           const QString &value,
                                                           bool reset)
{
    if (!isTransitionProducerFactoryParameter(service, name))
        return true;
    return reset || transitionProducerFactoryAllowed(service, value);
}

inline bool nestedTransitionProducerPathAllowed(const QString &service, const QString &name)
{
    const QString id = service.trimmed().toLower();
    if (id != QStringLiteral("luma") && id != QStringLiteral("composite"))
        return true;

    const QString key = name.trimmed().toLower();
    if (key.startsWith(QStringLiteral("producer.")))
        return !sensitiveNestedPathTail(key.mid(9));
    if (key.startsWith(QStringLiteral("luma.")))
        return !sensitiveNestedPathTail(key.mid(5));
    return true;
}

inline bool avFilterPropertyAllowed(const QString &service, const QString &name)
{
    const QString id = service.trimmed().toLower();
    const QString key = name.trimmed().toLower();
    if (!key.startsWith(QStringLiteral("av.")))
        return true;
    static const QStringList allowed{
        QStringLiteral("avfilter.adeclick|av.window"),
        QStringLiteral("avfilter.adeclick|av.overlap"),
        QStringLiteral("avfilter.adeclick|av.aorder"),
        QStringLiteral("avfilter.adeclick|av.order"),
        QStringLiteral("avfilter.adeclick|av.threshold"),
        QStringLiteral("avfilter.adeclick|av.burst"),
        QStringLiteral("avfilter.adeclick|av.method"),
        QStringLiteral("avfilter.haas|av.level_in"),
        QStringLiteral("avfilter.haas|av.level_out"),
        QStringLiteral("avfilter.haas|av.side_gain"),
        QStringLiteral("avfilter.haas|av.middle_source"),
        QStringLiteral("avfilter.haas|av.middle_phase"),
        QStringLiteral("avfilter.haas|av.left_delay"),
        QStringLiteral("avfilter.haas|av.left_balance"),
        QStringLiteral("avfilter.haas|av.left_gain"),
        QStringLiteral("avfilter.haas|av.left_phase"),
        QStringLiteral("avfilter.haas|av.right_delay"),
        QStringLiteral("avfilter.haas|av.right_balance"),
        QStringLiteral("avfilter.haas|av.right_gain"),
        QStringLiteral("avfilter.haas|av.right_phase"),
        QStringLiteral("avfilter.chromahold|av.color"),
        QStringLiteral("avfilter.chromahold|av.similarity"),
        QStringLiteral("avfilter.deband|av.1thr"),
        QStringLiteral("avfilter.deband|av.2thr"),
        QStringLiteral("avfilter.deband|av.3thr"),
        QStringLiteral("avfilter.deband|av.4thr"),
        QStringLiteral("avfilter.deband|av.range"),
        QStringLiteral("avfilter.deband|av.direction"),
        QStringLiteral("avfilter.deband|av.blur"),
        QStringLiteral("avfilter.deband|av.coupling"),
        QStringLiteral("avfilter.fspp|av.quality"),
        QStringLiteral("avfilter.fspp|av.qp"),
        QStringLiteral("avfilter.fspp|av.strength"),
        QStringLiteral("avfilter.gblur|av.sigma"),
        QStringLiteral("avfilter.gblur|av.sigmav"),
        QStringLiteral("avfilter.gblur|av.planes"),
        QStringLiteral("avfilter.hue|av.h"),
        QStringLiteral("avfilter.hue|av.b"),
        QStringLiteral("avfilter.hue|av.s"),
        QStringLiteral("avfilter.lut3d|av.file"),
        QStringLiteral("avfilter.lut3d|av.interp"),
        QStringLiteral("avfilter.random|av.frames"),
        QStringLiteral("avfilter.noise|av.all_strength"),
        QStringLiteral("avfilter.noise|av.all_flags"),
        QStringLiteral("avfilter.smartblur|av.luma_radius"),
        QStringLiteral("avfilter.smartblur|av.chroma_radius"),
        QStringLiteral("avfilter.smartblur|av.luma_strength"),
        QStringLiteral("avfilter.smartblur|av.chroma_strength"),
        QStringLiteral("avfilter.smartblur|av.luma_threshold"),
        QStringLiteral("avfilter.smartblur|av.chroma_threshold"),
        QStringLiteral("avfilter.tmix|av.frames"),
        QStringLiteral("avfilter.tmix|av.weights"),
        QStringLiteral("avfilter.vaguedenoiser|av.method"),
        QStringLiteral("avfilter.vaguedenoiser|av.nsteps"),
        QStringLiteral("avfilter.vaguedenoiser|av.threshold"),
        QStringLiteral("avfilter.vaguedenoiser|av.percent"),
        QStringLiteral("avfilter.vibrance|av.intensity"),
        QStringLiteral("avfilter.vibrance|av.rbal"),
        QStringLiteral("avfilter.vibrance|av.gbal"),
        QStringLiteral("avfilter.vibrance|av.bbal"),
    };
    return allowed.contains(id + QLatin1Char('|') + key);
}

inline bool isLutFileParameter(const QString &service, const QString &name)
{
    return service.trimmed().compare(QStringLiteral("avfilter.lut3d"), Qt::CaseInsensitive) == 0
           && name.trimmed().compare(QStringLiteral("av.file"), Qt::CaseInsensitive) == 0;
}

inline bool filterInputSuffixAllowed(const QString &service,
                                     const QString &name,
                                     const QString &suffix)
{
    if (!isLutFileParameter(service, name))
        return true;
    static const QStringList suffixes{QStringLiteral("dat"),
                                      QStringLiteral("3dl"),
                                      QStringLiteral("cube"),
                                      QStringLiteral("m3d"),
                                      QStringLiteral("csp")};
    return suffixes.contains(suffix.toLower());
}

// Kept independent of the live editor so policy classification can be unit-tested.
inline FilterPathKind filterPathKind(const QString &filterId, const QString &name)
{
    const QString id = filterId.trimmed().toLower();
    const QString key = name.trimmed().toLower();
    if (id == QStringLiteral("avfilter.lut3d") && key == QStringLiteral("av.file"))
        return FilterPathKind::ExistingFile;
    if (id == QStringLiteral("placebo.shader") && key == QStringLiteral("shader_path"))
        return FilterPathKind::ExistingFile;
    if ((id == QStringLiteral("maskfromfile") || id == QStringLiteral("maskglaxnimate"))
        && key == QStringLiteral("filter.resource")) {
        return FilterPathKind::ExistingFile;
    }
    if ((id == QStringLiteral("gpsgraphic") || id == QStringLiteral("gpstext"))
        && key == QStringLiteral("resource")) {
        return FilterPathKind::ExistingFile;
    }
    if (id == QStringLiteral("gpstext") && key == QStringLiteral("gps.file"))
        return FilterPathKind::ExistingFile;
    if (id == QStringLiteral("gpsgraphic") && key == QStringLiteral("bg_img_path"))
        return FilterPathKind::ExistingFile;
    if (id == QStringLiteral("vidstab") && key == QStringLiteral("results"))
        return FilterPathKind::ExistingFile;
    if (id == QStringLiteral("vidstab") && key == QStringLiteral("filename"))
        return FilterPathKind::WritablePath;
    if ((id == QStringLiteral("bigsh0t_stabilize_360")
         || id == QStringLiteral("frei0r.bigsh0t_stabilize_360"))
        && key == QStringLiteral("analysisfile")) {
        return FilterPathKind::WritablePath;
    }
    return FilterPathKind::NotPath;
}

inline FilterPathKind safestFilterPathKind(FilterPathKind first, FilterPathKind second)
{
    if (first == FilterPathKind::ExistingFile || second == FilterPathKind::ExistingFile)
        return FilterPathKind::ExistingFile;
    if (first == FilterPathKind::WritablePath || second == FilterPathKind::WritablePath)
        return FilterPathKind::WritablePath;
    return FilterPathKind::NotPath;
}

inline FilterPathKind filterPathKindForIdentities(const QString &filterId,
                                                  const QString &service,
                                                  const QString &name)
{
    const auto specific = safestFilterPathKind(filterPathKind(filterId, name),
                                               filterPathKind(service, name));
    if (specific != FilterPathKind::NotPath)
        return specific;

    const QString key = name.trimmed().toLower();
    static const QStringList genericPathNames{
        QStringLiteral("resource"),
        QStringLiteral("src"),
        QStringLiteral("filename"),
        QStringLiteral("file"),
        QStringLiteral("path"),
        QStringLiteral("url"),
        QStringLiteral("luma"),
        QStringLiteral("luma.resource"),
        QStringLiteral("composite.luma"),
        QStringLiteral("producer.resource"),
        QStringLiteral("av.file"),
        QStringLiteral("av.filename"),
        QStringLiteral("filter.resource"),
        QStringLiteral("warp_resource"),
        QStringLiteral("gps.file"),
        QStringLiteral("bg_img_path"),
        QStringLiteral("results"),
        QStringLiteral("analysisfile"),
        QStringLiteral("shader_path"),
    };
    return genericPathNames.contains(key) ? FilterPathKind::ExistingFile : FilterPathKind::NotPath;
}

// Rich text markup and resource files can load nested external content outside allowed roots.
// MCP callers can reset these properties, but must use plain text and styling for new content.
inline bool richTextParameterWriteAllowed(const QString &filterId, const QString &name, bool reset)
{
    const QString id = filterId.trimmed().toLower();
    const QString key = name.trimmed().toLower();
    const bool isRichText = id == QStringLiteral("richtext") || id == QStringLiteral("qtext");
    const bool isExternalContent = key == QStringLiteral("html")
                                   || key == QStringLiteral("resource");
    return !isRichText || !isExternalContent || reset;
}

inline bool isBuiltInValue(const QString &filterId, const QString &name, const QString &value)
{
    if (filterId.compare(QStringLiteral("maskFromFile"), Qt::CaseInsensitive) != 0
        || name.compare(QStringLiteral("filter.resource"), Qt::CaseInsensitive) != 0
        || value.size() != 11 || !value.startsWith(QStringLiteral("%luma"))
        || !value.endsWith(QStringLiteral(".pgm"))) {
        return false;
    }
    bool ok = false;
    const int number = value.mid(5, 2).toInt(&ok);
    return ok && number >= 1 && number <= 22
           && value.mid(5, 2) == QString::number(number).rightJustified(2, QLatin1Char('0'));
}
} // namespace McpBridgePolicy

#endif // MCPBRIDGEPOLICY_H
