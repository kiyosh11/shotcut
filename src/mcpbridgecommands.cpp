/*
 * Copyright (c) 2026 Meltytech, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "mcpbridge.h"

#include "docks/encodedock.h"
#include "docks/timelinedock.h"
#include "jobqueue.h"
#include "mainwindow.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QUndoStack>
#include <QtMath>

namespace {
bool jsonInteger(const QJsonObject &object, const QString &name, int *result)
{
    const auto value = object.value(name);
    if (!value.isDouble() || !qIsFinite(value.toDouble())
        || value.toDouble() != static_cast<double>(value.toInt()))
        return false;
    *result = value.toInt();
    return true;
}

bool jsonInteger64(const QJsonObject &object, const QString &name, qint64 *result)
{
    constexpr double maximumExactJsonInteger = 9007199254740991.0;
    const auto value = object.value(name);
    const double number = value.toDouble();
    if (!value.isDouble() || !qIsFinite(number) || qAbs(number) > maximumExactJsonInteger)
        return false;
    const qint64 integer = static_cast<qint64>(number);
    if (number != static_cast<double>(integer))
        return false;
    *result = integer;
    return true;
}

QString requiredString(const QJsonObject &object, const QString &name)
{
    const auto value = object.value(name);
    return value.isString() ? value.toString() : QString();
}

bool hasProjectExtension(const QString &path)
{
    return path.endsWith(QStringLiteral(".mlt")) || path.endsWith(QStringLiteral(".xml"));
}

Qt::CaseSensitivity pathCaseSensitivity()
{
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}
} // namespace

bool McpBridge::trackExists(int track) const
{
    return track >= 0 && track < m_window.timelineDock()->model()->trackList().size();
}

bool McpBridge::clipExists(int track, int clip) const
{
    return trackExists(track) && clip >= 0 && clip < m_window.timelineDock()->clipCount(track);
}

bool McpBridge::checkRevision(const QJsonObject &params, QString &error) const
{
    const auto value = params.value(QStringLiteral("expected_revision"));
    if (value.isNull() || value.isUndefined()) {
        error = QStringLiteral("expected_revision is required; read editor status or a snapshot");
        return false;
    }
    qint64 expected = -1;
    if (!jsonInteger64(params, QStringLiteral("expected_revision"), &expected)) {
        error = QStringLiteral("expected_revision must be an integer");
        return false;
    }
    const qint64 actual = m_revision;
    if (expected != actual) {
        error = QStringLiteral("Revision conflict: expected %1, current %2");
        error = error.arg(expected).arg(actual);
        return false;
    }
    return true;
}

McpBridge::RpcResult McpBridge::openProject(const QJsonObject &params)
{
    QString revisionError;
    if (!checkRevision(params, revisionError))
        return RpcResult::failure(-32002, revisionError);

    QString path = requiredString(params, QStringLiteral("path"));
    if (path.isEmpty())
        return RpcResult::failure(-32602, QStringLiteral("path is required"));
    if (!hasProjectExtension(path))
        return RpcResult::failure(-32602, QStringLiteral("Use a lowercase .mlt or .xml extension"));

    QString normalized;
    if (!pathAllowed(path, true, &normalized))
        return RpcResult::failure(-32004, QStringLiteral("Project path is outside allowed roots"));

    const bool discard = params.value(QStringLiteral("discard_unsaved")).toBool(false);
    const bool wasModified = m_window.isWindowModified();
    if (wasModified && !discard)
        return RpcResult::failure(-32002, QStringLiteral("Save or discard current unsaved edits"));

    if (discard)
        m_window.setWindowModified(false);
    m_window.open(normalized, nullptr, false);
    const QString opened = normalizedPathForPolicy(m_window.fileName(), true);
    if (opened.compare(normalized, pathCaseSensitivity()) != 0) {
        if (wasModified)
            m_window.setWindowModified(true);
        return RpcResult::failure(-32003, QStringLiteral("Shotcut did not open that project"));
    }
    return RpcResult::success(editorStatus());
}

McpBridge::RpcResult McpBridge::saveProject(const QJsonObject &params)
{
    QString revisionError;
    if (!checkRevision(params, revisionError))
        return RpcResult::failure(-32002, revisionError);

    QString path = params.value(QStringLiteral("path")).toString();
    if (path.isEmpty()) {
        path = m_window.fileName();
        if (path.isEmpty() || path == m_window.untitledFileName())
            return RpcResult::failure(-32602, QStringLiteral("An absolute save path is required"));
    }
    if (!hasProjectExtension(path))
        return RpcResult::failure(-32602, QStringLiteral("Use a lowercase .mlt or .xml save path"));

    QString normalized;
    if (!pathAllowed(path, false, &normalized))
        return RpcResult::failure(-32004, QStringLiteral("Save path is outside allowed roots"));

    const QString currentPath = normalizedPathForPolicy(m_window.fileName(), true);
    const bool differentPath = normalized.compare(currentPath, pathCaseSensitivity()) != 0;
    const bool overwrite = params.value(QStringLiteral("overwrite")).toBool(false);
    if (QFileInfo::exists(normalized) && differentPath && !overwrite)
        return RpcResult::failure(-32002, QStringLiteral("Save target exists; overwrite is false"));

    const bool relativePaths = params.value(QStringLiteral("relative_paths")).toBool(true);
    if (!m_window.saveProjectAs(normalized, relativePaths))
        return RpcResult::failure(-32003, QStringLiteral("Shotcut failed to save the project"));
    return RpcResult::success(editorStatus());
}

McpBridge::RpcResult McpBridge::applyEditPlan(const QJsonObject &params)
{
    QString revisionError;
    if (!checkRevision(params, revisionError))
        return RpcResult::failure(-32002, revisionError);

    QString label = requiredString(params, QStringLiteral("label")).trimmed();
    if (label.isEmpty() || label.size() > 120)
        return RpcResult::failure(-32602, QStringLiteral("label must be 1 to 120 characters"));

    const auto operations = params.value(QStringLiteral("operations")).toArray();
    if (operations.isEmpty() || operations.size() > 500)
        return RpcResult::failure(-32602, QStringLiteral("operations must contain 1 to 500 items"));

    for (int index = 0; index < operations.size(); ++index) {
        if (!operations.at(index).isObject())
            return RpcResult::failure(-32602,
                                      QStringLiteral("Operation %1 must be an object").arg(index));
    }

    if (params.value(QStringLiteral("dry_run")).toBool(false)) {
        for (int index = 0; index < operations.size(); ++index) {
            QString error;
            if (!validateOperation(operations.at(index).toObject(), error)) {
                return RpcResult::failure(
                    -32602,
                    QStringLiteral(
                        "Operation %1 is invalid against the current snapshot: %2. "
                        "After structural edits, apply and re-read the snapshot before addressing "
                        "new tracks or clips by index.")
                        .arg(index)
                        .arg(error));
            }
        }
        return RpcResult::success(QJsonObject{
            {QStringLiteral("valid"), true},
            {QStringLiteral("dry_run"), true},
            {QStringLiteral("revision"), static_cast<double>(m_revision)},
            {QStringLiteral("operation_count"), operations.size()},
        });
    }

    auto *stack = m_window.undoStack();
    const int beforeRevision = stack->index();
    stack->beginMacro(label);
    int applied = 0;
    QString applyError;
    for (const auto &value : operations) {
        if (!validateOperation(value.toObject(), applyError)
            || !applyOperation(value.toObject(), applyError))
            break;
        ++applied;
    }
    stack->endMacro();

    if (applied != operations.size()) {
        if (stack->index() != beforeRevision && stack->canUndo())
            stack->undo();
        const QString message = QStringLiteral("Edit plan rolled back at operation %1: %2")
                                    .arg(applied)
                                    .arg(applyError);
        QJsonObject data;
        data.insert(QStringLiteral("revision"), static_cast<double>(m_revision));
        return RpcResult::failure(-32003, message, data);
    }

    return RpcResult::success(QJsonObject{
        {QStringLiteral("applied"), applied},
        {QStringLiteral("revision"), static_cast<double>(m_revision)},
        {QStringLiteral("undo_label"), label},
    });
}

McpBridge::RpcResult McpBridge::changeHistory(const QJsonObject &params, bool redo)
{
    QString revisionError;
    if (!checkRevision(params, revisionError))
        return RpcResult::failure(-32002, revisionError);

    int steps = 1;
    if (params.contains(QStringLiteral("steps"))
        && (!jsonInteger(params, QStringLiteral("steps"), &steps) || steps < 1 || steps > 100))
        return RpcResult::failure(-32602, QStringLiteral("steps must be between 1 and 100"));

    auto *stack = m_window.undoStack();
    int changed = 0;
    while (changed < steps && (redo ? stack->canRedo() : stack->canUndo())) {
        redo ? stack->redo() : stack->undo();
        ++changed;
    }
    return RpcResult::success(QJsonObject{
        {QStringLiteral("changed"), changed},
        {QStringLiteral("revision"), static_cast<double>(m_revision)},
        {QStringLiteral("can_undo"), stack->canUndo()},
        {QStringLiteral("can_redo"), stack->canRedo()},
    });
}

McpBridge::RpcResult McpBridge::startExport(const QJsonObject &params)
{
    QString revisionError;
    if (!checkRevision(params, revisionError))
        return RpcResult::failure(-32002, revisionError);

    QString normalized;
    if (!pathAllowed(requiredString(params, QStringLiteral("target")), false, &normalized))
        return RpcResult::failure(-32004, QStringLiteral("Export target is outside allowed roots"));
    if (QFileInfo::exists(normalized) && !params.value(QStringLiteral("overwrite")).toBool(false))
        return RpcResult::failure(-32002, QStringLiteral("Export target exists; set overwrite"));
    if (JOBS.targetIsInProgress(normalized))
        return RpcResult::failure(-32002, QStringLiteral("An export to this target is active"));

    QString error;
    const QString preset = params.value(QStringLiteral("preset")).toString();
    if (!m_window.encodeDock()->exportToFile(normalized, preset, &error))
        return RpcResult::failure(-32003, error.isEmpty() ? QStringLiteral("Export failed") : error);

    return RpcResult::success(QJsonObject{
        {QStringLiteral("queued"), true},
        {QStringLiteral("target"), normalized},
        {QStringLiteral("jobs"), exportJobs(normalized)},
    });
}

McpBridge::RpcResult McpBridge::exportStatus(const QJsonObject &params) const
{
    QString target = params.value(QStringLiteral("target")).toString();
    if (!target.isEmpty()) {
        QString normalized;
        if (!pathAllowed(target, false, &normalized))
            return RpcResult::failure(-32004, QStringLiteral("Target is outside allowed roots"));
        target = normalized;
    }
    return RpcResult::success(QJsonObject{
        {QStringLiteral("target"), target},
        {QStringLiteral("jobs"), exportJobs(target)},
    });
}

bool McpBridge::validateOperation(const QJsonObject &operation, QString &error) const
{
    const QString type = requiredString(operation, QStringLiteral("op"));
    static const QStringList supported{
        QStringLiteral("add_track"),
        QStringLiteral("remove_track"),
        QStringLiteral("insert_media"),
        QStringLiteral("move_clip"),
        QStringLiteral("trim_clip"),
        QStringLiteral("split_clip"),
        QStringLiteral("remove_clip"),
        QStringLiteral("set_clip_gain"),
        QStringLiteral("set_clip_fade"),
        QStringLiteral("add_transition"),
        QStringLiteral("set_track_state"),
        QStringLiteral("add_filter"),
        QStringLiteral("set_filter_parameters"),
        QStringLiteral("add_subtitle_track"),
        QStringLiteral("replace_subtitles"),
    };
    if (!supported.contains(type)) {
        error = QStringLiteral("unsupported op '%1'").arg(type);
        return false;
    }

    if (type == QStringLiteral("add_track")) {
        const QString kind = requiredString(operation, QStringLiteral("kind"));
        if (kind != QStringLiteral("audio") && kind != QStringLiteral("video")) {
            error = QStringLiteral("kind must be audio or video");
            return false;
        }
        if (operation.contains(QStringLiteral("index"))) {
            int index;
            if (!jsonInteger(operation, QStringLiteral("index"), &index) || index < 0
                || index > m_window.timelineDock()->model()->trackList().size()) {
                error = QStringLiteral("index is outside the track insertion range");
                return false;
            }
        }
        if (operation.contains(QStringLiteral("name"))
            && (!operation.value(QStringLiteral("name")).isString()
                || operation.value(QStringLiteral("name")).toString().size() > 256)) {
            error = QStringLiteral("track name must be a string of at most 256 characters");
            return false;
        }
        return true;
    }

    if (type == QStringLiteral("add_subtitle_track")) {
        const QString name = requiredString(operation, QStringLiteral("name")).trimmed();
        if (name.isEmpty() || name.size() > 256) {
            error = QStringLiteral("subtitle track name must be 1 to 256 characters");
            return false;
        }
        return true;
    }

    if (type == QStringLiteral("replace_subtitles")) {
        int subtitleTrack = -1;
        const auto *subtitles = m_window.timelineDock()->subtitlesModel();
        const bool hasTrackIndex = jsonInteger(operation, QStringLiteral("track"), &subtitleTrack);
        if (!hasTrackIndex || subtitleTrack < 0 || subtitleTrack >= subtitles->trackCount()) {
            error = QStringLiteral("subtitle track does not exist");
            return false;
        }

        const auto itemsValue = operation.value(QStringLiteral("items"));
        if (!itemsValue.isArray()) {
            error = QStringLiteral("items must be an array");
            return false;
        }

        qint64 previousEnd = -1;
        for (const auto &value : itemsValue.toArray()) {
            if (!value.isObject()) {
                error = QStringLiteral("each subtitle must be an object");
                return false;
            }
            const auto item = value.toObject();
            const auto startValue = item.value(QStringLiteral("start_ms"));
            const auto endValue = item.value(QStringLiteral("end_ms"));
            constexpr double maximumExactJsonInteger = 9007199254740991.0;
            const double startNumber = startValue.toDouble();
            const double endNumber = endValue.toDouble();
            const bool finiteTimes = startValue.isDouble() && endValue.isDouble()
                                     && qIsFinite(startNumber) && qIsFinite(endNumber);
            const bool inRange = qAbs(startNumber) <= maximumExactJsonInteger
                                 && qAbs(endNumber) <= maximumExactJsonInteger;
            if (!finiteTimes || !inRange) {
                error = QStringLiteral("subtitle times must be exact finite integers");
                return false;
            }
            const qint64 start = static_cast<qint64>(startNumber);
            const qint64 end = static_cast<qint64>(endNumber);
            const bool integerTimes = startNumber == static_cast<double>(start)
                                      && endNumber == static_cast<double>(end);
            const bool orderedTimes = start >= 0 && end > start && start >= previousEnd;
            const bool hasText = item.value(QStringLiteral("text")).isString();
            if (!integerTimes || !orderedTimes || !hasText) {
                error = QStringLiteral("subtitles must be sorted, non-overlapping integer ranges");
                return false;
            }
            previousEnd = end;
        }
        return true;
    }

    int track = -1;
    if (!jsonInteger(operation, QStringLiteral("track"), &track)
        && !jsonInteger(operation, QStringLiteral("from_track"), &track)) {
        error = QStringLiteral("track is required");
        return false;
    }
    if (!trackExists(track)) {
        error = QStringLiteral("track %1 does not exist").arg(track);
        return false;
    }

    if (type == QStringLiteral("set_track_state")) {
        if (operation.contains(QStringLiteral("name"))
            && (!operation.value(QStringLiteral("name")).isString()
                || operation.value(QStringLiteral("name")).toString().size() > 256)) {
            error = QStringLiteral("track name must be a string of at most 256 characters");
            return false;
        }
        static const QStringList booleanStates{
            QStringLiteral("muted"),
            QStringLiteral("hidden"),
            QStringLiteral("composite"),
            QStringLiteral("locked"),
        };
        for (const auto &state : booleanStates) {
            if (operation.contains(state) && !operation.value(state).isBool()) {
                error = QStringLiteral("track state '%1' must be boolean").arg(state);
                return false;
            }
        }
        return true;
    }
    if (m_window.timelineDock()->isTrackLocked(track)) {
        error = QStringLiteral("track %1 is locked").arg(track);
        return false;
    }
    if (type == QStringLiteral("remove_track"))
        return true;

    if (type == QStringLiteral("insert_media")) {
        QString normalized;
        if (!pathAllowed(requiredString(operation, QStringLiteral("path")), true, &normalized)) {
            error = QStringLiteral("media path is missing or outside allowed roots");
            return false;
        }
        int position;
        if (!jsonInteger(operation, QStringLiteral("position"), &position) || position < 0) {
            error = QStringLiteral("position must be a non-negative frame");
            return false;
        }
        return true;
    }

    int clip = -1;
    if (!jsonInteger(operation, QStringLiteral("clip"), &clip) || !clipExists(track, clip)) {
        error = QStringLiteral("clip does not exist");
        return false;
    }

    if (type == QStringLiteral("move_clip")) {
        int toTrack;
        int position;
        if (!jsonInteger(operation, QStringLiteral("to_track"), &toTrack) || !trackExists(toTrack)
            || !jsonInteger(operation, QStringLiteral("position"), &position) || position < 0) {
            error = QStringLiteral("move destination is invalid");
            return false;
        }
        if (m_window.timelineDock()->isTrackLocked(toTrack)) {
            error = QStringLiteral("destination track %1 is locked").arg(toTrack);
            return false;
        }
    } else if (type == QStringLiteral("trim_clip")) {
        const QString edge = requiredString(operation, QStringLiteral("edge"));
        int delta;
        if ((edge != QStringLiteral("in") && edge != QStringLiteral("out"))
            || !jsonInteger(operation, QStringLiteral("delta_frames"), &delta) || delta == 0) {
            error = QStringLiteral("trim edge or delta_frames is invalid");
            return false;
        }
    } else if (type == QStringLiteral("split_clip") || type == QStringLiteral("add_transition")) {
        int position;
        if (!jsonInteger(operation, QStringLiteral("position"), &position) || position < 0) {
            error = QStringLiteral("position must be a non-negative frame");
            return false;
        }
    } else if (type == QStringLiteral("set_clip_fade")) {
        int duration;
        const QString edge = requiredString(operation, QStringLiteral("edge"));
        if ((edge != QStringLiteral("in") && edge != QStringLiteral("out"))
            || !jsonInteger(operation, QStringLiteral("duration_frames"), &duration)
            || duration < 0) {
            error = QStringLiteral("fade edge or duration_frames is invalid");
            return false;
        }
    } else if (type == QStringLiteral("set_clip_gain")) {
        const auto gain = operation.value(QStringLiteral("gain"));
        if (!gain.isDouble() || !qIsFinite(gain.toDouble()) || gain.toDouble() < -120.0
            || gain.toDouble() > 60.0) {
            error = QStringLiteral("gain must be between -120 and 60 dB");
            return false;
        }
    } else if (type == QStringLiteral("add_filter")
               || type == QStringLiteral("set_filter_parameters")) {
        if (type == QStringLiteral("add_filter")) {
            const QString filterId = requiredString(operation, QStringLiteral("filter_id"));
            if (filterId.isEmpty() || filterId.size() > 512) {
                error = QStringLiteral("filter_id must be 1 to 512 characters");
                return false;
            }
        }
        if (type == QStringLiteral("set_filter_parameters")) {
            int filterIndex;
            if (!jsonInteger(operation, QStringLiteral("filter_index"), &filterIndex)
                || filterIndex < 0) {
                error = QStringLiteral("filter_index is invalid");
                return false;
            }
        }

        const auto parametersValue = operation.value(QStringLiteral("parameters"));
        if (!parametersValue.isObject()) {
            error = QStringLiteral("parameters must be an object");
            return false;
        }
        const auto parameters = parametersValue.toObject();
        for (auto it = parameters.constBegin(); it != parameters.constEnd(); ++it) {
            const QString name = it.key();
            const auto value = it.value();
            if (name.isEmpty() || name.size() > 256 || name.startsWith(QLatin1Char('_'))
                || name == QStringLiteral("mlt_service")
                || name.startsWith(QStringLiteral("shotcut:"))) {
                error = QStringLiteral("filter parameter '%1' is reserved or invalid").arg(name);
                return false;
            }
            if (!value.isString() && !value.isBool() && !value.isDouble() && !value.isNull()) {
                error = QStringLiteral(
                            "filter parameter '%1' must be string, boolean, number, or null")
                            .arg(name);
                return false;
            }
            if (value.isDouble()
                && (!qIsFinite(value.toDouble()) || qAbs(value.toDouble()) > 1.0e12)) {
                error = QStringLiteral("filter parameter '%1' exceeds numeric limit").arg(name);
                return false;
            }
            if (value.isString()) {
                const QString stringValue = value.toString();
                if (stringValue.size() > 1024 * 1024) {
                    error = QStringLiteral("filter parameter '%1' is too large").arg(name);
                    return false;
                }
                QFileInfo possiblePath(stringValue);
                if (possiblePath.isAbsolute() && !pathAllowed(stringValue, true)) {
                    error = QStringLiteral(
                                "filter parameter '%1' references a path outside allowed roots")
                                .arg(name);
                    return false;
                }
            }
        }
    }
    return true;
}

bool McpBridge::applyOperation(const QJsonObject &operation, QString &error)
{
    const QString type = operation.value(QStringLiteral("op")).toString();
    const bool filterOperation = type == QStringLiteral("add_filter")
                                 || type == QStringLiteral("set_filter_parameters");
    if (filterOperation)
        return applyFilterOperation(operation, error);
    const bool subtitleOperation = type == QStringLiteral("add_subtitle_track")
                                   || type == QStringLiteral("replace_subtitles");
    if (subtitleOperation)
        return applySubtitleOperation(operation, error);
    return applyTimelineOperation(operation, error);
}
