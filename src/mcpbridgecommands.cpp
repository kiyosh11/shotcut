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

#include <QCoreApplication>
#include <QEventLoop>
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

QString requiredString(const QJsonObject &object, const QString &name)
{
    const auto value = object.value(name);
    return value.isString() ? value.toString() : QString();
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
    if (value.isNull() || value.isUndefined())
        return true;
    if (!value.isDouble()) {
        error = QStringLiteral("expected_revision must be an integer");
        return false;
    }
    const int expected = value.toInt(-1);
    const int actual = m_window.undoStack()->index();
    if (expected != actual) {
        error = QStringLiteral("Project revision conflict: expected %1, current %2. Read a new snapshot.")
                    .arg(expected)
                    .arg(actual);
        return false;
    }
    return true;
}

McpBridge::RpcResult McpBridge::openProject(const QJsonObject &params)
{
    QString path = requiredString(params, QStringLiteral("path"));
    if (path.isEmpty())
        return RpcResult::failure(-32602, QStringLiteral("path is required"));
    if (!path.endsWith(QStringLiteral(".mlt"), Qt::CaseInsensitive)
        && !path.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive))
        return RpcResult::failure(-32602, QStringLiteral("Only .mlt and .xml projects can be opened"));

    QString normalized;
    if (!pathAllowed(path, true, &normalized))
        return RpcResult::failure(-32004,
                                  QStringLiteral("Project path is outside SHOTCUT_MCP_ALLOWED_ROOTS"));

    const bool discard = params.value(QStringLiteral("discard_unsaved")).toBool(false);
    const bool wasModified = m_window.isWindowModified();
    if (wasModified && !discard) {
        return RpcResult::failure(
            -32002,
            QStringLiteral("The current project has unsaved edits; save it or set discard_unsaved"));
    }

    if (discard)
        m_window.setWindowModified(false);
    m_window.open(normalized, nullptr, false);
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    const QString opened = normalizedPathForPolicy(m_window.fileName(), true);
    if (opened.compare(normalized, Qt::CaseInsensitive) != 0) {
        if (wasModified)
            m_window.setWindowModified(true);
        return RpcResult::failure(-32003, QStringLiteral("Shotcut did not open the requested project"));
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
    if (!path.endsWith(QStringLiteral(".mlt"), Qt::CaseInsensitive)
        && !path.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive))
        return RpcResult::failure(-32602, QStringLiteral("Save path must end in .mlt or .xml"));

    QString normalized;
    if (!pathAllowed(path, false, &normalized))
        return RpcResult::failure(-32004,
                                  QStringLiteral("Save path is outside SHOTCUT_MCP_ALLOWED_ROOTS"));

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
        return RpcResult::failure(-32602,
                                  QStringLiteral("label must contain between 1 and 120 characters"));

    const auto operations = params.value(QStringLiteral("operations")).toArray();
    if (operations.isEmpty() || operations.size() > 500)
        return RpcResult::failure(-32602,
                                  QStringLiteral("operations must contain between 1 and 500 items"));

    for (int index = 0; index < operations.size(); ++index) {
        if (!operations.at(index).isObject())
            return RpcResult::failure(-32602,
                                      QStringLiteral("Operation %1 must be an object").arg(index));
        QString error;
        if (!validateOperation(operations.at(index).toObject(), error)) {
            return RpcResult::failure(
                -32602,
                QStringLiteral("Operation %1 is invalid: %2").arg(index).arg(error));
        }
    }

    if (params.value(QStringLiteral("dry_run")).toBool(false)) {
        return RpcResult::success(QJsonObject{
            {QStringLiteral("valid"), true},
            {QStringLiteral("dry_run"), true},
            {QStringLiteral("revision"), m_window.undoStack()->index()},
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
        return RpcResult::failure(
            -32003,
            QStringLiteral("Edit plan rolled back after operation %1: %2").arg(applied).arg(applyError),
            QJsonObject{{QStringLiteral("revision"), stack->index()}});
    }

    return RpcResult::success(QJsonObject{
        {QStringLiteral("applied"), applied},
        {QStringLiteral("revision"), stack->index()},
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
        {QStringLiteral("revision"), stack->index()},
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
        return RpcResult::failure(-32004,
                                  QStringLiteral("Export target is outside SHOTCUT_MCP_ALLOWED_ROOTS"));
    if (QFileInfo::exists(normalized) && !params.value(QStringLiteral("overwrite")).toBool(false))
        return RpcResult::failure(-32002,
                                  QStringLiteral("Export target exists and overwrite is false"));
    if (JOBS.targetIsInProgress(normalized))
        return RpcResult::failure(-32002, QStringLiteral("An export to this target is already active"));

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
        return true;
    }

    if (type == QStringLiteral("add_subtitle_track")) {
        if (requiredString(operation, QStringLiteral("name")).trimmed().isEmpty()) {
            error = QStringLiteral("subtitle track name is required");
            return false;
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

    if (type == QStringLiteral("remove_track") || type == QStringLiteral("set_track_state"))
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

    if (type == QStringLiteral("replace_subtitles")) {
        const auto items = operation.value(QStringLiteral("items")).toArray();
        qint64 previousEnd = -1;
        for (const auto &value : items) {
            const auto item = value.toObject();
            const qint64 start = static_cast<qint64>(
                item.value(QStringLiteral("start_ms")).toDouble(-1));
            const qint64 end = static_cast<qint64>(
                item.value(QStringLiteral("end_ms")).toDouble(-1));
            if (start < 0 || end <= start || start < previousEnd
                || !item.value(QStringLiteral("text")).isString()) {
                error = QStringLiteral(
                    "subtitles must be sorted, non-overlapping, and have start_ms < end_ms");
                return false;
            }
            previousEnd = end;
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
    } else if (type == QStringLiteral("trim_clip")) {
        const QString edge = requiredString(operation, QStringLiteral("edge"));
        int delta;
        if ((edge != QStringLiteral("in") && edge != QStringLiteral("out"))
            || !jsonInteger(operation, QStringLiteral("delta_frames"), &delta) || delta == 0) {
            error = QStringLiteral("trim edge or delta_frames is invalid");
            return false;
        }
    } else if (type == QStringLiteral("split_clip")
               || type == QStringLiteral("add_transition")) {
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
    } else if (type == QStringLiteral("set_clip_gain")
               && !operation.value(QStringLiteral("gain")).isDouble()) {
        error = QStringLiteral("gain must be numeric");
        return false;
    } else if ((type == QStringLiteral("add_filter")
                || type == QStringLiteral("set_filter_parameters"))
               && !operation.value(QStringLiteral("parameters")).isObject()) {
        error = QStringLiteral("parameters must be an object");
        return false;
    }
    return true;
}

bool McpBridge::applyOperation(const QJsonObject &operation, QString &error)
{
    const QString type = operation.value(QStringLiteral("op")).toString();
    if (type == QStringLiteral("add_filter")
        || type == QStringLiteral("set_filter_parameters"))
        return applyFilterOperation(operation, error);
    if (type == QStringLiteral("add_subtitle_track")
        || type == QStringLiteral("replace_subtitles"))
        return applySubtitleOperation(operation, error);
    return applyTimelineOperation(operation, error);
}
