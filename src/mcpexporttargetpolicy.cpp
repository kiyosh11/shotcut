/*
 * Copyright (c) 2026 Meltytech, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "mcpexporttargetpolicy.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

namespace {
Qt::CaseSensitivity pathCaseSensitivity()
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

bool fail(QString *errorMessage, const QString &message)
{
    if (errorMessage)
        *errorMessage = message;
    return false;
}

QString normalizedForComparison(const QString &path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

bool pathsEqual(const QString &left, const QString &right)
{
    return normalizedForComparison(left).compare(normalizedForComparison(right),
                                                 pathCaseSensitivity())
           == 0;
}

QString expectedImageSequenceTarget(const QFileInfo &requestedTarget)
{
    return QStringLiteral("%1/%2-%05d.%3")
        .arg(requestedTarget.path(), requestedTarget.baseName(), requestedTarget.completeSuffix());
}

bool isAsciiDecimal(const QString &value, int minimumDigits)
{
    if (value.size() < minimumDigits)
        return false;
    for (const QChar character : value) {
        if (character < QLatin1Char('0') || character > QLatin1Char('9'))
            return false;
    }
    return true;
}

bool isImageSequenceMember(const QString &fileName, const QFileInfo &requestedTarget)
{
    const QString prefix = requestedTarget.baseName() + QLatin1Char('-');
    const QString suffix = QLatin1Char('.') + requestedTarget.completeSuffix();
    const auto comparison = pathCaseSensitivity();
    if (!fileName.startsWith(prefix, comparison) || !fileName.endsWith(suffix, comparison)
        || fileName.size() <= prefix.size() + suffix.size()) {
        return false;
    }

    QString number = fileName.mid(prefix.size(), fileName.size() - prefix.size() - suffix.size());
    if (number.startsWith(QLatin1Char('-')))
        return isAsciiDecimal(number.mid(1), 4);
    return isAsciiDecimal(number, 5);
}

bool isAllowedConsumerProperty(const QString &name)
{
    static const QSet<QString> allowed{
        QStringLiteral("ab"),
        QStringLiteral("abr"),
        QStringLiteral("acodec"),
        QStringLiteral("alang"),
        QStringLiteral("an"),
        QStringLiteral("aq"),
        QStringLiteral("ar"),
        QStringLiteral("aspect"),
        QStringLiteral("attached_pic"),
        QStringLiteral("audio_off"),
        QStringLiteral("bf"),
        QStringLiteral("cbr"),
        QStringLiteral("channel_layout"),
        QStringLiteral("channels"),
        QStringLiteral("color_range"),
        QStringLiteral("color_trc"),
        QStringLiteral("compression_level"),
        QStringLiteral("cq"),
        QStringLiteral("crf"),
        QStringLiteral("deinterlacer"),
        QStringLiteral("f"),
        QStringLiteral("frame_rate_den"),
        QStringLiteral("frame_rate_num"),
        QStringLiteral("g"),
        QStringLiteral("height"),
        QStringLiteral("hw_encoding"),
        QStringLiteral("keyint_min"),
        QStringLiteral("load_plugin"),
        QStringLiteral("master_display"),
        QStringLiteral("max_cll"),
        QStringLiteral("meta.attr.artist.markup"),
        QStringLiteral("meta.attr.comment.markup"),
        QStringLiteral("meta.attr.copyright.markup"),
        QStringLiteral("meta.attr.date.markup"),
        QStringLiteral("meta.attr.description.markup"),
        QStringLiteral("meta.attr.genre.markup"),
        QStringLiteral("meta.attr.title.markup"),
        QStringLiteral("mlt_color_trc"),
        QStringLiteral("mlt_image_format"),
        QStringLiteral("mlt_service"),
        QStringLiteral("movflags"),
        QStringLiteral("pix_fmt"),
        QStringLiteral("preset"),
        QStringLiteral("progressive"),
        QStringLiteral("qmin"),
        QStringLiteral("qp_b"),
        QStringLiteral("qp_i"),
        QStringLiteral("qp_p"),
        QStringLiteral("qscale"),
        QStringLiteral("rate_control"),
        QStringLiteral("rc"),
        QStringLiteral("real_time"),
        QStringLiteral("rescale"),
        QStringLiteral("sc_threshold"),
        QStringLiteral("scale"),
        QStringLiteral("strict"),
        QStringLiteral("strict_gop"),
        QStringLiteral("svtav1-params"),
        QStringLiteral("target"),
        QStringLiteral("threads"),
        QStringLiteral("top_field_first"),
        QStringLiteral("vb"),
        QStringLiteral("vbr"),
        QStringLiteral("vbufsize"),
        QStringLiteral("vcodec"),
        QStringLiteral("vglobal_quality"),
        QStringLiteral("video_off"),
        QStringLiteral("vmaxrate"),
        QStringLiteral("vminrate"),
        QStringLiteral("vn"),
        QStringLiteral("vprofile"),
        QStringLiteral("vtag"),
        QStringLiteral("vq"),
        QStringLiteral("vquality"),
        QStringLiteral("vtune"),
        QStringLiteral("width"),
        QStringLiteral("x264-params"),
        QStringLiteral("x265-params"),
    };
    static const QRegularExpression subtitleProperty(QStringLiteral(
                                                         "^subtitle\\.[0-9]+\\.(feed|lang)$"),
                                                     QRegularExpression::CaseInsensitiveOption);
    return allowed.contains(name.toLower()) || subtitleProperty.match(name).hasMatch();
}

bool hasSafeConstrainedValue(const QString &propertyName, const QString &value)
{
    const QString name = propertyName.toLower();
    if (name == QStringLiteral("load_plugin"))
        return value == QStringLiteral("hevc_hw");
    if (name == QStringLiteral("movflags"))
        return value == QStringLiteral("+faststart");
    if (name == QStringLiteral("preset"))
        return value == QStringLiteral("fast") || value == QStringLiteral("medium");
    if (name == QStringLiteral("vtag"))
        return value == QStringLiteral("hvc1");
    return true;
}
bool isAllowedSingleFileMuxer(const QString &muxer)
{
    static const QSet<QString> allowed{
        QStringLiteral("3g2"),        QStringLiteral("3gp"),  QStringLiteral("ac3"),
        QStringLiteral("adts"),       QStringLiteral("aiff"), QStringLiteral("asf"),
        QStringLiteral("avi"),        QStringLiteral("caf"),  QStringLiteral("dv"),
        QStringLiteral("eac3"),       QStringLiteral("flac"), QStringLiteral("flv"),
        QStringLiteral("gif"),        QStringLiteral("ipod"), QStringLiteral("matroska"),
        QStringLiteral("mjpeg"),      QStringLiteral("mov"),  QStringLiteral("mp2"),
        QStringLiteral("mp3"),        QStringLiteral("mp4"),  QStringLiteral("mpeg"),
        QStringLiteral("mpegts"),     QStringLiteral("mxf"),  QStringLiteral("mxf_d10"),
        QStringLiteral("mxf_opatom"), QStringLiteral("nut"),  QStringLiteral("oga"),
        QStringLiteral("ogg"),        QStringLiteral("ogv"),  QStringLiteral("opus"),
        QStringLiteral("rawvideo"),   QStringLiteral("w64"),  QStringLiteral("wav"),
        QStringLiteral("webm"),
    };
    return allowed.contains(muxer);
}

bool isAllowedSingleFileSuffix(const QString &suffix)
{
    static const QSet<QString> allowed{
        QStringLiteral("3g2"),  QStringLiteral("3gp"),  QStringLiteral("aac"),
        QStringLiteral("ac3"),  QStringLiteral("aif"),  QStringLiteral("aiff"),
        QStringLiteral("amr"),  QStringLiteral("asf"),  QStringLiteral("avi"),
        QStringLiteral("caf"),  QStringLiteral("dv"),   QStringLiteral("eac3"),
        QStringLiteral("f4v"),  QStringLiteral("flac"), QStringLiteral("flv"),
        QStringLiteral("gif"),  QStringLiteral("m2t"),  QStringLiteral("m2ts"),
        QStringLiteral("m4a"),  QStringLiteral("m4v"),  QStringLiteral("mka"),
        QStringLiteral("mkv"),  QStringLiteral("mov"),  QStringLiteral("mp2"),
        QStringLiteral("mp3"),  QStringLiteral("mp4"),  QStringLiteral("mpeg"),
        QStringLiteral("mpg"),  QStringLiteral("mts"),  QStringLiteral("mxf"),
        QStringLiteral("nut"),  QStringLiteral("oga"),  QStringLiteral("ogg"),
        QStringLiteral("ogv"),  QStringLiteral("opus"), QStringLiteral("ts"),
        QStringLiteral("vob"),  QStringLiteral("w64"),  QStringLiteral("wav"),
        QStringLiteral("webm"), QStringLiteral("wmv"),
    };
    return allowed.contains(suffix.toLower());
}

bool isAllowedImageSequenceSuffix(const QString &suffix)
{
    static const QSet<QString> allowed{
        QStringLiteral("bmp"),
        QStringLiteral("dpx"),
        QStringLiteral("jpg"),
        QStringLiteral("jpeg"),
        QStringLiteral("png"),
        QStringLiteral("ppm"),
        QStringLiteral("tga"),
        QStringLiteral("targa"),
        QStringLiteral("tif"),
        QStringLiteral("tiff"),
        QStringLiteral("webp"),
    };
    return allowed.contains(suffix.toLower());
}

bool compoundParametersAreSafe(const QString &propertyName, const QString &value)
{
    static const QSet<QString> x264Allowed{QStringLiteral("bff"), QStringLiteral("tff")};
    static const QSet<QString> x265Allowed{
        QStringLiteral("bframes"),
        QStringLiteral("bitrate"),
        QStringLiteral("crf"),
        QStringLiteral("interlace"),
        QStringLiteral("keyint"),
        QStringLiteral("master-display"),
        QStringLiteral("max-cll"),
        QStringLiteral("scenecut"),
        QStringLiteral("vbv-bufsize"),
        QStringLiteral("vbv-maxrate"),
    };
    static const QSet<QString> svtAllowed{
        QStringLiteral("buf-sz"),
        QStringLiteral("enable-dg"),
        QStringLiteral("lp"),
        QStringLiteral("mastering-display"),
        QStringLiteral("max-cll"),
        QStringLiteral("pred-struct"),
        QStringLiteral("rc"),
        QStringLiteral("scd"),
        QStringLiteral("tbr"),
    };

    const QSet<QString> *allowed = nullptr;
    const QString key = propertyName.toLower();
    if (key == QStringLiteral("x264-params"))
        allowed = &x264Allowed;
    else if (key == QStringLiteral("x265-params"))
        allowed = &x265Allowed;
    else if (key == QStringLiteral("svtav1-params"))
        allowed = &svtAllowed;
    else
        return true;

    const auto tokens = value.split(QLatin1Char(':'), Qt::KeepEmptyParts);
    for (int index = 0; index < tokens.size(); ++index) {
        const QString token = tokens.at(index).trimmed();
        if (token.isEmpty()) {
            if (index == tokens.size() - 1)
                continue;
            return false;
        }
        if (token.contains(QLatin1Char(';')) || token.contains(QLatin1Char('\r'))
            || token.contains(QLatin1Char('\n'))) {
            return false;
        }
        const int equals = token.indexOf(QLatin1Char('='));
        if (equals <= 0 || equals == token.size() - 1
            || token.indexOf(QLatin1Char('='), equals + 1) >= 0) {
            return false;
        }
        if (!allowed->contains(token.left(equals).trimmed().toLower()))
            return false;
    }
    return true;
}
} // namespace

namespace McpExportTargetPolicy {
bool validateConsumerProperties(const QString &consumerTarget,
                                bool imageSequence,
                                int pass,
                                const ConsumerProperties &properties,
                                const PathAuthorizer &pathAuthorizer,
                                QString *errorMessage)
{
    if (!pathAuthorizer)
        return fail(errorMessage, QStringLiteral("Export target policy is unavailable."));
    if (pass != 0) {
        return fail(errorMessage,
                    QStringLiteral("Two-pass exports require sidecar files and must be started "
                                   "manually in Shotcut."));
    }

    QSet<QString> normalizedNames;
    QString service;
    QString target;
    QString muxer;
    for (auto it = properties.cbegin(); it != properties.cend(); ++it) {
        const QString normalizedName = it.key().toLower();
        if (normalizedNames.contains(normalizedName)) {
            return fail(errorMessage,
                        QStringLiteral("Export consumer contains duplicate property '%1'.")
                            .arg(it.key()));
        }
        normalizedNames.insert(normalizedName);
        if (!isAllowedConsumerProperty(it.key())) {
            return fail(errorMessage,
                        QStringLiteral("Export consumer property '%1' requires manual export.")
                            .arg(it.key()));
        }
        if (!hasSafeConstrainedValue(normalizedName, it.value())) {
            return fail(errorMessage,
                        QStringLiteral("Export consumer property '%1' has a value that requires "
                                       "manual export.")
                            .arg(it.key()));
        }
        if (normalizedName == QStringLiteral("mlt_service"))
            service = it.value().trimmed();
        else if (normalizedName == QStringLiteral("target"))
            target = it.value();
        else if (normalizedName == QStringLiteral("f")) {
            if (it.key() != QStringLiteral("f")) {
                return fail(errorMessage,
                            QStringLiteral("Export muxer property name must be canonical."));
            }
            muxer = it.value();
        } else if (normalizedName == QStringLiteral("attached_pic")) {
            const QFileInfo attachment(it.value());
            if (!attachment.isFile() || attachment.isSymLink()
                || !pathAuthorizer(it.value(), true)) {
                return fail(errorMessage,
                            QStringLiteral("Export cover art must be a regular file in an allowed "
                                           "root."));
            }
        }
        if (!compoundParametersAreSafe(normalizedName, it.value())) {
            return fail(errorMessage,
                        QStringLiteral("Export codec parameter '%1' requires manual export.")
                            .arg(it.key()));
        }
    }

    if (service.compare(QStringLiteral("avformat"), Qt::CaseInsensitive) != 0) {
        return fail(errorMessage, QStringLiteral("Export consumer service must be avformat."));
    }
    if (target.isEmpty() || !pathsEqual(target, consumerTarget)) {
        return fail(errorMessage,
                    QStringLiteral("Export consumer target differs from the authorized target."));
    }
    if (!QFileInfo(consumerTarget).isAbsolute()) {
        return fail(errorMessage, QStringLiteral("Export consumer target must be absolute."));
    }

    const QString suffix = QFileInfo(consumerTarget).suffix();
    if (imageSequence) {
        if ((!muxer.isEmpty() && muxer != QStringLiteral("image2"))
            || !isAllowedImageSequenceSuffix(suffix)) {
            return fail(errorMessage,
                        QStringLiteral("Export image sequence muxer must use the exact canonical "
                                       "image2 token."));
        }
    } else if ((!muxer.isEmpty() && !isAllowedSingleFileMuxer(muxer))
               || (muxer.isEmpty() && !isAllowedSingleFileSuffix(suffix))) {
        return fail(errorMessage,
                    QStringLiteral("Export muxer must be an exact canonical allowlisted "
                                   "single-file format."));
    }
    return true;
}

bool validateConsumerTarget(const QString &requestedTarget,
                            const QString &consumerTarget,
                            bool imageSequence,
                            bool overwrite,
                            const PathAuthorizer &pathAuthorizer,
                            QString *errorMessage,
                            EnumerationLimits limits)
{
    if (!pathAuthorizer)
        return fail(errorMessage, QStringLiteral("Export target policy is unavailable."));
    if (limits.maximumDirectoryEntries <= 0 || limits.maximumSequenceMembers <= 0) {
        return fail(errorMessage, QStringLiteral("Export target validation limits are invalid."));
    }
    if (!pathAuthorizer(requestedTarget, false) || !pathAuthorizer(consumerTarget, false)) {
        return fail(errorMessage,
                    QStringLiteral("Export consumer target is outside allowed roots."));
    }

    const QFileInfo consumerInfo(consumerTarget);
    if (!imageSequence) {
        if (!pathsEqual(requestedTarget, consumerTarget)) {
            return fail(errorMessage,
                        QStringLiteral(
                            "Export consumer target differs from the requested target."));
        }
        if (!consumerInfo.exists())
            return true;
        if (consumerInfo.isSymLink())
            return fail(errorMessage, QStringLiteral("Export target must not be a symbolic link."));
        if (!consumerInfo.isFile())
            return fail(errorMessage, QStringLiteral("Export target must be a regular file."));
        if (!overwrite)
            return fail(errorMessage, QStringLiteral("Export target exists; set overwrite."));
        return true;
    }

    const QFileInfo requestedInfo(requestedTarget);
    if (requestedTarget.contains(QLatin1Char('%'))) {
        return fail(errorMessage,
                    QStringLiteral("MCP image sequence targets must not contain '%' because "
                                   "Shotcut adds the only frame-number token."));
    }
    const QString expectedTarget = expectedImageSequenceTarget(requestedInfo);
    if (expectedTarget.count(QLatin1Char('%')) != 1
        || expectedTarget.count(QStringLiteral("%05d")) != 1) {
        return fail(errorMessage,
                    QStringLiteral("Export image sequence target has an invalid frame-number "
                                   "pattern."));
    }
    if (!pathsEqual(expectedTarget, consumerTarget)) {
        return fail(errorMessage,
                    QStringLiteral("Export image sequence target has an unexpected pattern."));
    }

    const QFileInfo directoryInfo(consumerInfo.absolutePath());
    if (!directoryInfo.exists() || !directoryInfo.isDir() || directoryInfo.isSymLink()
        || !pathAuthorizer(directoryInfo.absoluteFilePath(), true)) {
        return fail(errorMessage,
                    QStringLiteral("Export image sequence directory is outside allowed roots."));
    }

    int directoryEntries = 0;
    int sequenceMembers = 0;
    QDirIterator iterator(directoryInfo.absoluteFilePath(),
                          QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    while (iterator.hasNext()) {
        iterator.next();
        if (++directoryEntries > limits.maximumDirectoryEntries) {
            return fail(errorMessage,
                        QStringLiteral(
                            "Export directory has too many entries to validate safely."));
        }

        const QFileInfo member = iterator.fileInfo();
        if (!isImageSequenceMember(member.fileName(), requestedInfo))
            continue;
        if (++sequenceMembers > limits.maximumSequenceMembers) {
            return fail(errorMessage,
                        QStringLiteral("Export image sequence has too many existing members to "
                                       "validate safely."));
        }
        if (member.isSymLink()) {
            return fail(errorMessage,
                        QStringLiteral(
                            "Export image sequence members must not be symbolic links."));
        }
        if (!member.isFile()) {
            return fail(errorMessage,
                        QStringLiteral("Export image sequence members must be regular files."));
        }
        if (!pathAuthorizer(member.absoluteFilePath(), true)) {
            return fail(errorMessage,
                        QStringLiteral("Export image sequence member is outside allowed roots."));
        }
        if (!overwrite) {
            return fail(errorMessage,
                        QStringLiteral("Export image sequence already has frames; set overwrite."));
        }
    }
    return true;
}
} // namespace McpExportTargetPolicy
