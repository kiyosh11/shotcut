/*
 * Copyright (c) 2026 Meltytech, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "mcpbridge.h"

#include "mcpkeyframeinterpolation.h"
#include "mcpbridgepolicy.h"
#include "mcpexporttargetpolicy.h"

#include "mcpundoutils.h"

#include "docks/encodedock.h"
#include "docks/timelinedock.h"
#include "jobqueue.h"
#include "mainwindow.h"
#include "models/attachedfiltersmodel.h"
#include "models/multitrackmodel.h"
#include "player.h"
#include "qmltypes/qmlmetadata.h"
#include "settings.h"
#include "shotcut_mlt_properties.h"
#include "util.h"

#include <MltAnimation.h>
#include <MltProducer.h>
#include <MltService.h>
#include <QAction>
#include <QActionGroup>
#include <QColor>
#include <QFileInfo>
#include <QJsonArray>
#include <QPoint>
#include <QRegularExpression>
#include <QScopedPointer>
#include <QSize>
#include <QUndoStack>
#include <QtMath>

#include <limits>

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

bool markerColor(const QJsonValue &value, QColor *result = nullptr)
{
    static const QRegularExpression expression(QStringLiteral("^#[0-9A-Fa-f]{6}$"));
    if (!value.isString() || !expression.match(value.toString()).hasMatch())
        return false;
    const QColor color(value.toString());
    if (!color.isValid())
        return false;
    if (result)
        *result = color;
    return true;
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
    const MainWindow::ProjectResourceValidator resourceValidator =
        [this](const QString &checkedFile, QString *errorMessage) {
            QString validationError;
            const bool valid = validateProjectResourcePaths(checkedFile, validationError);
            if (errorMessage)
                *errorMessage = validationError;
            return valid;
        };
    QString openError;
    const bool openCallSucceeded = m_window.openProjectNonInteractive(normalized,
                                                                      nullptr,
                                                                      false,
                                                                      true,
                                                                      resourceValidator,
                                                                      &openError);
    const QString opened = normalizedPathForPolicy(m_window.fileName(), true);
    if (!openCallSucceeded || opened.compare(normalized, pathCaseSensitivity()) != 0) {
        if (wasModified)
            m_window.setWindowModified(true);
        return RpcResult::failure(-32003,
                                  openError.isEmpty()
                                      ? QStringLiteral("Shotcut did not open that project")
                                      : openError);
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

    const QFileInfo saveTarget(normalized);
    if (!QFileInfo(saveTarget.absolutePath()).isWritable())
        return RpcResult::failure(-32003, QStringLiteral("Save directory is not writable"));

    const QString currentPath = normalizedPathForPolicy(m_window.fileName(), true);
    const bool differentPath = normalized.compare(currentPath, pathCaseSensitivity()) != 0;
    const bool overwrite = params.value(QStringLiteral("overwrite")).toBool(false);
    if (QFileInfo::exists(normalized) && differentPath && !overwrite)
        return RpcResult::failure(-32002, QStringLiteral("Save target exists; overwrite is false"));

    const bool relativePaths = params.value(QStringLiteral("relative_paths")).toBool(true);
    if (!m_window.saveProjectAsNonInteractive(normalized, relativePaths))
        return RpcResult::failure(-32003, QStringLiteral("Shotcut failed to save the project"));
    return RpcResult::success(editorStatus());
}

McpBridge::RpcResult McpBridge::controlEditor(const QJsonObject &params)
{
    QString revisionError;
    if (!checkRevision(params, revisionError))
        return RpcResult::failure(-32002, revisionError);

    const auto commandValue = params.value(QStringLiteral("command"));
    if (!commandValue.isObject())
        return RpcResult::failure(-32602, QStringLiteral("command must be an object"));
    const auto command = commandValue.toObject();
    const QString action = requiredString(command, QStringLiteral("action"));
    auto *timeline = m_window.timelineDock();
    auto *model = timeline->model();
    if (!m_window.multitrack() || !model->tractor())
        return RpcResult::failure(-32003, QStringLiteral("timeline is not loaded"));
    const auto activateProject = [&]() { m_window.seekTimeline(timeline->position()); };

    if (action == QStringLiteral("seek") || action == QStringLiteral("seek_relative")) {
        int requested = 0;
        if (action == QStringLiteral("seek")) {
            if (!jsonInteger(command, QStringLiteral("position"), &requested))
                return RpcResult::failure(-32602, QStringLiteral("position must be an integer"));
        } else {
            int frames = 0;
            if (!jsonInteger(command, QStringLiteral("frames"), &frames))
                return RpcResult::failure(-32602, QStringLiteral("frames must be an integer"));
            const qint64 target = static_cast<qint64>(timeline->position()) + frames;
            if (target < 0 || target > std::numeric_limits<int>::max())
                return RpcResult::failure(-32602, QStringLiteral("relative seek is out of range"));
            requested = static_cast<int>(target);
        }
        const int maximum = model->tractor() ? qMax(0, model->tractor()->get_length()) : 0;
        if (requested < 0 || requested > maximum)
            return RpcResult::failure(
                -32602, QStringLiteral("position must be between 0 and %1").arg(maximum));
        m_window.seekTimeline(requested);
    } else if (action == QStringLiteral("select_clip")) {
        int track = -1;
        int clip = -1;
        if (!jsonInteger(command, QStringLiteral("track"), &track)
            || !jsonInteger(command, QStringLiteral("clip"), &clip) || !clipExists(track, clip)) {
            return RpcResult::failure(-32602, QStringLiteral("clip does not exist"));
        }
        activateProject();
        timeline->setCurrentTrack(track);
        timeline->setSelection(QList<QPoint>{QPoint(clip, track)});
    } else if (action == QStringLiteral("select_track")) {
        int track = -1;
        if (!jsonInteger(command, QStringLiteral("track"), &track) || !trackExists(track))
            return RpcResult::failure(-32602, QStringLiteral("track does not exist"));
        activateProject();
        timeline->setCurrentTrack(track);
        timeline->setSelection(QList<QPoint>(), track, false);
    } else if (action == QStringLiteral("clear_selection")) {
        activateProject();
        timeline->setSelection();
    } else if (action == QStringLiteral("play")) {
        const auto speedValue = command.value(QStringLiteral("speed"));
        const double speed = speedValue.isUndefined() ? 1.0 : speedValue.toDouble();
        if ((!speedValue.isUndefined() && !speedValue.isDouble()) || !qIsFinite(speed)
            || qFuzzyIsNull(speed) || qAbs(speed) > 16.0) {
            return RpcResult::failure(-32602,
                                      QStringLiteral(
                                          "speed must be non-zero and between -16 and 16"));
        }
        activateProject();
        m_window.player()->play(speed);
    } else if (action == QStringLiteral("pause")) {
        activateProject();
        m_window.player()->pause();
    } else if (action == QStringLiteral("stop")) {
        activateProject();
        m_window.player()->stop();
    } else {
        return RpcResult::failure(-32602,
                                  QStringLiteral("unsupported editor action '%1'").arg(action));
    }

    QJsonArray selection;
    for (const auto &point : timeline->selection()) {
        selection.append(QJsonObject{{QStringLiteral("track"), point.y()},
                                     {QStringLiteral("clip"), point.x()}});
    }
    return RpcResult::success(QJsonObject{
        {QStringLiteral("action"), action},
        {QStringLiteral("position"), timeline->position()},
        {QStringLiteral("current_track"), timeline->currentTrack()},
        {QStringLiteral("selection"), selection},
        {QStringLiteral("revision"), static_cast<double>(m_revision)},
    });
}

McpBridge::RpcResult McpBridge::setProjectProfile(const QJsonObject &params)
{
    QString revisionError;
    if (!checkRevision(params, revisionError))
        return RpcResult::failure(-32002, revisionError);

    int width = 0;
    int height = 0;
    int frameRateNumerator = 0;
    int frameRateDenominator = 0;
    if (!jsonInteger(params, QStringLiteral("width"), &width) || width < 64 || width > 8192
        || width % 2 != 0 || !jsonInteger(params, QStringLiteral("height"), &height)
        || height < 64 || height > 8192 || height % 2 != 0) {
        return RpcResult::failure(
            -32602, QStringLiteral("width and height must be even values from 64 through 8192"));
    }
    if (!jsonInteger(params, QStringLiteral("frame_rate_num"), &frameRateNumerator)
        || !jsonInteger(params, QStringLiteral("frame_rate_den"), &frameRateDenominator)
        || frameRateNumerator <= 0 || frameRateDenominator <= 0
        || static_cast<double>(frameRateNumerator) / frameRateDenominator < 1.0
        || static_cast<double>(frameRateNumerator) / frameRateDenominator > 240.0) {
        return RpcResult::failure(
            -32602, QStringLiteral("frame rate must be a positive rational from 1 to 240"));
    }

    const bool hasDisplayNumerator = params.contains(QStringLiteral("display_aspect_num"));
    const bool hasDisplayDenominator = params.contains(QStringLiteral("display_aspect_den"));
    if (hasDisplayNumerator != hasDisplayDenominator) {
        return RpcResult::failure(
            -32602, QStringLiteral("supply both display aspect fields or neither"));
    }
    int displayNumerator = width;
    int displayDenominator = height;
    if (hasDisplayNumerator
        && (!jsonInteger(params, QStringLiteral("display_aspect_num"), &displayNumerator)
            || !jsonInteger(params, QStringLiteral("display_aspect_den"), &displayDenominator)
            || displayNumerator <= 0 || displayDenominator <= 0 || displayNumerator > 8192
            || displayDenominator > 8192)) {
        return RpcResult::failure(-32602, QStringLiteral("display aspect values are invalid"));
    }
    const int displayDivisor = Util::greatestCommonDivisor(displayNumerator, displayDenominator);
    displayNumerator /= displayDivisor;
    displayDenominator /= displayDivisor;
    const int frameRateDivisor = Util::greatestCommonDivisor(frameRateNumerator,
                                                             frameRateDenominator);
    frameRateNumerator /= frameRateDivisor;
    frameRateDenominator /= frameRateDivisor;

    const QString colorspaceName = requiredString(params, QStringLiteral("colorspace"));
    int colorspace = 709;
    if (colorspaceName == QStringLiteral("bt601"))
        colorspace = 601;
    else if (colorspaceName == QStringLiteral("bt2020"))
        colorspace = 2020;
    else if (colorspaceName != QStringLiteral("bt709"))
        return RpcResult::failure(-32602, QStringLiteral("colorspace is invalid"));

    const QString dynamicRange = requiredString(params, QStringLiteral("dynamic_range"));
    if (dynamicRange != QStringLiteral("sdr") && dynamicRange != QStringLiteral("hlg")
        && dynamicRange != QStringLiteral("pq")) {
        return RpcResult::failure(-32602, QStringLiteral("dynamic_range is invalid"));
    }
    if (dynamicRange != QStringLiteral("sdr") && colorspace != 2020) {
        return RpcResult::failure(-32602,
                                  QStringLiteral(
                                      "HLG and PQ profiles require the BT.2020 colorspace"));
    }
    if (!params.value(QStringLiteral("progressive")).isBool()) {
        return RpcResult::failure(-32602, QStringLiteral("progressive must be boolean"));
    }
    if (!params.value(QStringLiteral("clear_undo_history")).isBool()) {
        return RpcResult::failure(-32602, QStringLiteral("clear_undo_history must be boolean"));
    }

    auto &profile = MLT.profile();
    const bool progressive = params.value(QStringLiteral("progressive")).toBool();
    const bool unchanged = profile.width() == width && profile.height() == height
                           && profile.frame_rate_num() == frameRateNumerator
                           && profile.frame_rate_den() == frameRateDenominator
                           && (profile.progressive() != 0) == progressive
                           && profile.display_aspect_num() == displayNumerator
                           && profile.display_aspect_den() == displayDenominator
                           && profile.colorspace() == colorspace
                           && ((dynamicRange == QStringLiteral("sdr")
                                && (MLT.colorTrc().isEmpty()
                                    || MLT.colorTrc() == QStringLiteral("sdr")))
                               || (dynamicRange == QStringLiteral("hlg")
                                   && MLT.colorTrc() == QStringLiteral("arib-std-b67"))
                               || (dynamicRange == QStringLiteral("pq")
                                   && MLT.colorTrc() == QStringLiteral("smpte2084")));
    if (unchanged) {
        m_window.setVideoModeMenu();
        return RpcResult::success(QJsonObject{
            {QStringLiteral("changed"), false},
            {QStringLiteral("project_reloaded"), false},
            {QStringLiteral("history_cleared"), false},
            {QStringLiteral("profile"), projectSnapshot().value(QStringLiteral("profile"))},
            {QStringLiteral("revision"), static_cast<double>(m_revision)},
        });
    }

    auto *stack = m_window.undoStack();
    const bool hadHistory = stack && (stack->canUndo() || stack->canRedo());
    if (hadHistory
        && !params.value(QStringLiteral("clear_undo_history")).toBool()) {
        return RpcResult::failure(
            -32002,
            QStringLiteral("profile changes reload the project; set clear_undo_history to true"));
    }

    QString xml;
    auto *producer = MLT.producer();
    if (m_window.timelineDock()->model()->rowCount() > 0) {
        producer = m_window.multitrack();
    } else if (m_window.isPlaylistValid()) {
        producer = m_window.playlist();
    } else if (MLT.isMultitrack() || MLT.isPlaylist()) {
        producer = MLT.savedProducer();
    }
    if (producer && producer->is_valid()) {
        MLT.fixLengthProperties(*producer);
        xml = MLT.XML(producer);
        if (xml.isEmpty()) {
            return RpcResult::failure(
                -32003, QStringLiteral("Shotcut could not serialize the active project"));
        }
    }
    if (!xml.isEmpty()
        && (!MLT.consumer() || !MLT.consumer()->is_valid() || !MLT.producer()
            || !MLT.producer()->is_valid())) {
        return RpcResult::failure(
            -32003, QStringLiteral("Shotcut is not ready to reload the active project"));
    }

    profile.set_explicit(1);
    profile.set_width(width);
    profile.set_height(height);
    profile.set_display_aspect(displayNumerator, displayDenominator);
    const QSize sampleAspect(displayNumerator * height, displayDenominator * width);
    const int aspectDivisor = Util::greatestCommonDivisor(sampleAspect.width(),
                                                          sampleAspect.height());
    profile.set_sample_aspect(sampleAspect.width() / aspectDivisor,
                              sampleAspect.height() / aspectDivisor);
    profile.set_frame_rate(frameRateNumerator, frameRateDenominator);
    profile.set_progressive(progressive ? 1 : 0);
    profile.set_colorspace(colorspace);
    MLT.updatePreviewProfile();
    MLT.setPreviewScale(Settings.playerPreviewScale());
    QString colorTransfer;
    if (dynamicRange == QStringLiteral("hlg"))
        colorTransfer = QStringLiteral("arib-std-b67");
    else if (dynamicRange == QStringLiteral("pq"))
        colorTransfer = QStringLiteral("smpte2084");
    MLT.setColorTrc(colorTransfer);
    emit m_window.profileChanged();

    if (!xml.isEmpty()) {
        MLT.reload(xml);
        // reload() replaces the active producer with the serialized project. Reapply the
        // request-provided transfer characteristic to that replacement producer as well.
        MLT.setColorTrc(colorTransfer);
        emit m_window.producerOpened(false);
    }
    m_window.setVideoModeMenu();
    if (stack && (stack->canUndo() || stack->canRedo()))
        stack->clear();
    m_window.setWindowModified(true);
    advanceRevision();
    return RpcResult::success(QJsonObject{
        {QStringLiteral("changed"), true},
        {QStringLiteral("project_reloaded"), !xml.isEmpty()},
        {QStringLiteral("history_cleared"), hadHistory},
        {QStringLiteral("profile"), projectSnapshot().value(QStringLiteral("profile"))},
        {QStringLiteral("revision"), static_cast<double>(m_revision)},
    });
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
        // A dry run cannot safely mutate and undo the project merely to simulate later
        // operations. Keep it deterministic by validating potentially invalidating operations
        // against the starting snapshot. Real plans validate sequentially inside one undo macro.
        static const QStringList snapshotInvalidatingOperations{
            QStringLiteral("add_track"),
            QStringLiteral("remove_track"),
            QStringLiteral("insert_media"),
            QStringLiteral("move_clip"),
            QStringLiteral("trim_clip"),
            QStringLiteral("split_clip"),
            QStringLiteral("remove_clip"),
            QStringLiteral("set_clip_speed"),
            QStringLiteral("set_clip_gain"),
            QStringLiteral("set_clip_fade"),
            QStringLiteral("add_transition"),
            QStringLiteral("set_track_state"),
            QStringLiteral("add_filter"),
            QStringLiteral("set_filter_parameters"),
            QStringLiteral("remove_filter"),
            QStringLiteral("set_filter_keyframe"),
            QStringLiteral("remove_filter_keyframe"),
            QStringLiteral("add_marker"),
            QStringLiteral("update_marker"),
            QStringLiteral("remove_marker"),
            QStringLiteral("add_subtitle_track"),
        };
        for (int index = 0; index + 1 < operations.size(); ++index) {
            const QString type = operations.at(index)
                                     .toObject()
                                     .value(QStringLiteral("op"))
                                     .toString();
            if (snapshotInvalidatingOperations.contains(type)) {
                return RpcResult::failure(
                    -32602,
                    QStringLiteral(
                        "Dry-run operation %1 (%2) can invalidate validation of following "
                        "operations and must be last; dry-run staged plans against a fresh "
                        "snapshot, or submit the real plan for sequential validation in one "
                        "undo transaction")
                        .arg(index)
                        .arg(type));
            }
        }
        for (int index = 0; index < operations.size(); ++index) {
            QString error;
            if (!validateOperation(operations.at(index).toObject(), error)) {
                return RpcResult::failure(
                    -32602,
                    QStringLiteral(
                        "Operation %1 is invalid against the current snapshot: %2. "
                        "Stage dry runs after state-changing edits and re-read the snapshot.")
                        .arg(index)
                        .arg(error));
            }
        }
        if (m_window.undoStack()->canRedo()) {
            return RpcResult::failure(
                -32002,
                QStringLiteral(
                    "Apply or discard the existing redo history before submitting an edit plan"));
        }
        return RpcResult::success(QJsonObject{
            {QStringLiteral("valid"), true},
            {QStringLiteral("dry_run"), true},
            {QStringLiteral("revision"), static_cast<double>(m_revision)},
            {QStringLiteral("operation_count"), operations.size()},
        });
    }

    auto *stack = m_window.undoStack();
    if (stack->canRedo()) {
        return RpcResult::failure(
            -32002,
                QStringLiteral(
                    "Apply or discard the existing redo history before submitting an edit plan"));
    }

    if (m_window.multitrack() && m_window.timelineDock()->model()->tractor())
        m_window.seekTimeline(m_window.timelineDock()->position());

    const auto undoState = McpUndo::capture(*stack);
    stack->beginMacro(label);
    int applied = 0;
    QString applyError;
    for (const auto &value : operations) {
        const auto operation = value.toObject();
        if (!validateOperation(operation, applyError)) {
            applyError = QStringLiteral("sequential validation failed: %1").arg(applyError);
            break;
        }
        if (!applyOperation(operation, applyError))
            break;
        ++applied;
    }
    stack->endMacro();

    if (applied != operations.size()) {
        const bool historyRestored = McpUndo::rollbackLatestMacro(*stack, undoState);
        const QString message = historyRestored
                                    ? QStringLiteral("Edit plan rolled back at operation %1: %2")
                                          .arg(applied)
                                          .arg(applyError)
                                    : QStringLiteral("Edit plan failed at operation %1 and the "
                                                     "undo stack could not be restored: "
                                                     "%2")
                                          .arg(applied)
                                          .arg(applyError);
        QJsonObject data{
            {QStringLiteral("revision"), static_cast<double>(m_revision)},
            {QStringLiteral("history_restored"), historyRestored},
        };
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
    const QFileInfo output(normalized);
    const bool overwrite = params.value(QStringLiteral("overwrite")).toBool(false);
    if (output.exists() && !output.isFile())
        return RpcResult::failure(-32002, QStringLiteral("Export target must be a regular file"));
    if (output.exists() && !overwrite)
        return RpcResult::failure(-32002, QStringLiteral("Export target exists; set overwrite"));
    if (exportTargetInProgress(normalized))
        return RpcResult::failure(-32002, QStringLiteral("An export to this target is active"));

    QString error;
    const QString preset = params.value(QStringLiteral("preset")).toString();
    const auto targetPreflight = [this, overwrite](const QString &requestedTarget,
                                                   const QString &consumerTarget,
                                                   bool imageSequence,
                                                   int pass,
                                                   const QMap<QString, QString> &consumerProperties,
                                                   QString *errorMessage) {
        const McpExportTargetPolicy::PathAuthorizer pathAuthorizer =
            [this](const QString &path, bool mustExist) { return pathAllowed(path, mustExist); };
        if (!McpExportTargetPolicy::validateConsumerProperties(consumerTarget,
                                                               imageSequence,
                                                               pass,
                                                               consumerProperties,
                                                               pathAuthorizer,
                                                               errorMessage)) {
            return false;
        }
        return McpExportTargetPolicy::validateConsumerTarget(requestedTarget,
                                                             consumerTarget,
                                                             imageSequence,
                                                             overwrite,
                                                             pathAuthorizer,
                                                             errorMessage);
    };
    if (!m_window.encodeDock()->exportToFile(normalized, preset, &error, targetPreflight))
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
        QStringLiteral("set_clip_speed"),
        QStringLiteral("set_clip_gain"),
        QStringLiteral("set_clip_fade"),
        QStringLiteral("add_transition"),
        QStringLiteral("set_track_state"),
        QStringLiteral("add_filter"),
        QStringLiteral("set_filter_parameters"),
        QStringLiteral("set_filter_state"),
        QStringLiteral("remove_filter"),
        QStringLiteral("set_filter_keyframe"),
        QStringLiteral("remove_filter_keyframe"),
        QStringLiteral("add_marker"),
        QStringLiteral("update_marker"),
        QStringLiteral("remove_marker"),
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
            if (!jsonInteger(operation, QStringLiteral("index"), &index)) {
                error = QStringLiteral("index must be an integer");
                return false;
            }

            const auto &tracks = m_window.timelineDock()->model()->trackList();
            int firstAudioTrack = tracks.size();
            for (int trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
                if (tracks.at(trackIndex).type == AudioTrackType) {
                    firstAudioTrack = trackIndex;
                    break;
                }
            }
            const int minimum = kind == QStringLiteral("video")
                                    ? 0
                                    : (tracks.isEmpty() ? 0 : qMax(firstAudioTrack, 1));
            const int maximum = kind == QStringLiteral("video") ? firstAudioTrack : tracks.size();
            if (index < minimum || index > maximum) {
                error = QStringLiteral("%1 tracks can only be inserted at indexes %2 through %3")
                            .arg(kind)
                            .arg(minimum)
                            .arg(maximum);
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

    if (type == QStringLiteral("add_marker") || type == QStringLiteral("update_marker")
        || type == QStringLiteral("remove_marker")) {
        auto *timeline = m_window.timelineDock();
        if (!timeline->model()->tractor()) {
            error = QStringLiteral("timeline is not loaded");
            return false;
        }
        const auto existingMarkers = timeline->markersModel()->getMarkers();
        int markerIndex = -1;
        if (type != QStringLiteral("add_marker")) {
            if (!jsonInteger(operation, QStringLiteral("marker_index"), &markerIndex)
                || markerIndex < 0 || markerIndex >= existingMarkers.size()) {
                error = QStringLiteral("marker does not exist");
                return false;
            }
            if (type == QStringLiteral("remove_marker"))
                return true;
        }

        if (type == QStringLiteral("update_marker")
            && !operation.contains(QStringLiteral("text"))
            && !operation.contains(QStringLiteral("start"))
            && !operation.contains(QStringLiteral("end"))
            && !operation.contains(QStringLiteral("color"))) {
            error = QStringLiteral("update_marker must change at least one field");
            return false;
        }
        if (operation.contains(QStringLiteral("text"))
            && (!operation.value(QStringLiteral("text")).isString()
                || operation.value(QStringLiteral("text")).toString().size() > 512
                || !McpBridgePolicy::cStringValueAllowed(
                    operation.value(QStringLiteral("text")).toString()))) {
            error = QStringLiteral(
                "marker text must be a null-free string of at most 512 characters");
            return false;
        }
        if (type == QStringLiteral("add_marker")
            && !operation.value(QStringLiteral("text")).isString()) {
            error = QStringLiteral("marker text is required");
            return false;
        }
        if (operation.contains(QStringLiteral("color"))
            && !markerColor(operation.value(QStringLiteral("color")))) {
            error = QStringLiteral("marker color must use #RRGGBB format");
            return false;
        }

        int start = type == QStringLiteral("add_marker") ? -1
                                                           : existingMarkers.at(markerIndex).start;
        int end = type == QStringLiteral("add_marker") ? -1 : existingMarkers.at(markerIndex).end;
        const bool wasPointMarker = type == QStringLiteral("update_marker") && start == end;
        if (operation.contains(QStringLiteral("start"))
            && !jsonInteger(operation, QStringLiteral("start"), &start)) {
            error = QStringLiteral("marker start must be an integer");
            return false;
        }
        if (operation.contains(QStringLiteral("end"))
            && !jsonInteger(operation, QStringLiteral("end"), &end)) {
            error = QStringLiteral("marker end must be an integer");
            return false;
        }
        if (wasPointMarker && operation.contains(QStringLiteral("start"))
            && !operation.contains(QStringLiteral("end"))) {
            end = start;
        }
        if (type == QStringLiteral("add_marker") && start < 0) {
            error = QStringLiteral("marker start must be a non-negative frame");
            return false;
        }
        if (type == QStringLiteral("add_marker") && !operation.contains(QStringLiteral("end")))
            end = start;
        const int maximum = qMax(0, timeline->model()->tractor()->get_length());
        if (start < 0 || end < start || start > maximum || end > maximum) {
            error = QStringLiteral("marker range must be ordered and inside the timeline");
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

    if (type == QStringLiteral("set_track_state")) {
        if (!operation.contains(QStringLiteral("name"))
            && !operation.contains(QStringLiteral("muted"))
            && !operation.contains(QStringLiteral("hidden"))
            && !operation.contains(QStringLiteral("composite"))
            && !operation.contains(QStringLiteral("locked"))) {
            error = QStringLiteral("set_track_state must change at least one field");
            return false;
        }
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
        if (!validateMediaResourcePaths(normalized, error))
            return false;
        int position;
        if (!jsonInteger(operation, QStringLiteral("position"), &position) || position < 0) {
            error = QStringLiteral("position must be a non-negative frame");
            return false;
        }
        Mlt::Producer producer(MLT.profile(), normalized.toUtf8().constData());
        if (!producer.is_valid()) {
            error = QStringLiteral("MLT could not open the requested media");
            return false;
        }
        int in = producer.get_in();
        int out = producer.get_out();
        if (operation.contains(QStringLiteral("in_frame"))
            && !jsonInteger(operation, QStringLiteral("in_frame"), &in)) {
            error = QStringLiteral("in_frame must be an integer");
            return false;
        }
        if (operation.contains(QStringLiteral("out_frame"))
            && !jsonInteger(operation, QStringLiteral("out_frame"), &out)) {
            error = QStringLiteral("out_frame must be an integer");
            return false;
        }
        if (in < 0 || out < in || out >= producer.get_length()) {
            error = QStringLiteral("source in/out range is invalid");
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
        if (operation.contains(QStringLiteral("ripple"))
            && !operation.value(QStringLiteral("ripple")).isBool()) {
            error = QStringLiteral("ripple must be boolean");
            return false;
        }
        const bool ripple = operation.value(QStringLiteral("ripple")).toBool();
        auto *model = m_window.timelineDock()->model();
        const bool valid = edge == QStringLiteral("in")
                               ? model->trimClipInValid(track, clip, delta, ripple)
                               : model->trimClipOutValid(track, clip, delta, ripple);
        if (!valid) {
            error = QStringLiteral("trim exceeds available clip media or timeline space");
            return false;
        }
    } else if (type == QStringLiteral("split_clip")) {
        int position;
        if (!jsonInteger(operation, QStringLiteral("position"), &position) || position < 0) {
            error = QStringLiteral("position must be a non-negative frame");
            return false;
        }
        const auto info = m_window.timelineDock()->model()->getClipInfo(track, clip);
        if (!info || position <= info->start || position >= info->start + info->frame_count) {
            error = QStringLiteral("split position must be inside the clip");
            return false;
        }
    } else if (type == QStringLiteral("add_transition")) {
        int position;
        if (!jsonInteger(operation, QStringLiteral("position"), &position) || position < 0) {
            error = QStringLiteral("position must be a non-negative frame");
            return false;
        }
        if (operation.contains(QStringLiteral("ripple"))
            && !operation.value(QStringLiteral("ripple")).isBool()) {
            error = QStringLiteral("ripple must be boolean");
            return false;
        }
        const bool ripple = operation.value(QStringLiteral("ripple")).toBool();
        if (!m_window.timelineDock()->model()->addTransitionValid(track,
                                                                  track,
                                                                  clip,
                                                                  position,
                                                                  ripple)) {
            error = QStringLiteral("transition position is not valid for this clip");
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
        const auto info = m_window.timelineDock()->model()->getClipInfo(track, clip);
        if (!info || !info->producer || !info->producer->is_valid()
            || info->producer->is_blank()) {
            error = QStringLiteral("fade target must be a media clip, not a timeline gap");
            return false;
        }
        const auto clipIndex = m_window.timelineDock()->model()->index(
            clip, 0, m_window.timelineDock()->model()->index(track));
        if (m_window.timelineDock()
                ->model()
                ->data(clipIndex, MultitrackModel::IsTransitionRole)
                .toBool()) {
            error = QStringLiteral("fade target must be a media clip, not a transition");
            return false;
        }
        if (duration > info->frame_count) {
            error = QStringLiteral("fade duration must not exceed the clip length");
            return false;
        }
    } else if (type == QStringLiteral("set_clip_gain")) {
        const auto gain = operation.value(QStringLiteral("gain"));
        if (!gain.isDouble() || !qIsFinite(gain.toDouble()) || gain.toDouble() < -120.0
            || gain.toDouble() > 60.0) {
            error = QStringLiteral("gain must be between -120 and 60 dB");
            return false;
        }
        const auto info = m_window.timelineDock()->model()->getClipInfo(track, clip);
        if (!info || !info->producer || !info->producer->is_valid()
            || info->producer->is_blank()
            || (info->producer->get("audio_index")
                && info->producer->get_int("audio_index") == -1)) {
            error = QStringLiteral("clip does not contain an enabled audio stream");
            return false;
        }
        const auto clipIndex = m_window.timelineDock()->model()->index(
            clip, 0, m_window.timelineDock()->model()->index(track));
        if (m_window.timelineDock()
                ->model()
                ->data(clipIndex, MultitrackModel::IsTransitionRole)
                .toBool()) {
            error = QStringLiteral("gain target must be a media clip, not a transition");
            return false;
        }
        if (!m_window.timelineDock()
                 ->model()
                 ->data(clipIndex, MultitrackModel::GainEnabledRole)
                 .toBool()) {
            error = QStringLiteral("clip gain is animated and cannot be replaced by a constant");
            return false;
        }
    } else if (type == QStringLiteral("set_clip_speed")) {
        const auto speedValue = operation.value(QStringLiteral("speed"));
        const double speed = speedValue.toDouble();
        if (!speedValue.isDouble() || !qIsFinite(speed) || speed < 0.05 || speed > 20.0) {
            error = QStringLiteral("speed must be between 0.05 and 20");
            return false;
        }
        static const QStringList booleanOptions{
            QStringLiteral("preserve_pitch"),
            QStringLiteral("ripple"),
            QStringLiteral("ripple_all_tracks"),
        };
        for (const auto &option : booleanOptions) {
            if (operation.contains(option) && !operation.value(option).isBool()) {
                error = QStringLiteral("%1 must be boolean").arg(option);
                return false;
            }
        }
        auto info = m_window.timelineDock()->model()->getClipInfo(track, clip);
        if (!info || !info->producer || !info->producer->is_valid() || info->producer->is_blank()) {
            error = QStringLiteral("clip media is unavailable");
            return false;
        }
        const QString service = QString::fromUtf8(info->producer->get("mlt_service"));
        const QString shotcutProducer
            = QString::fromUtf8(info->producer->get(kShotcutProducerProperty));
        if (!service.startsWith(QStringLiteral("avformat"))
            && !shotcutProducer.startsWith(QStringLiteral("avformat"))
            && service != QStringLiteral("timewarp")) {
            error = QStringLiteral("constant speed is only available for file-based media clips");
            return false;
        }
        QString normalized;
        if (!pathAllowed(Util::GetFilenameFromProducer(info->producer, false), true, &normalized)) {
            error = QStringLiteral("clip media is outside allowed roots");
            return false;
        }
        const double oldSpeed = Util::GetSpeedFromProducer(info->producer);
        const double requestedMediaLength = static_cast<double>(info->length) * oldSpeed / speed;
        if (!qIsFinite(oldSpeed) || oldSpeed <= 0.0 || !qIsFinite(requestedMediaLength)
            || requestedMediaLength > std::numeric_limits<int>::max()) {
            error = QStringLiteral("requested speed would exceed Shotcut's frame range");
            return false;
        }
        const double speedRatio = oldSpeed / speed;
        const int newLength = qMax(1, qRound(static_cast<double>(info->length) * speedRatio));
        const int newIn = qBound(0,
                                 qRound(static_cast<double>(info->frame_in) * speedRatio),
                                 newLength - 1);
        const int newOut = qBound(newIn,
                                  qRound(static_cast<double>(info->frame_out) * speedRatio),
                                  newLength - 1);
        const int newDuration = newOut - newIn + 1;
        const bool ripple = operation.value(QStringLiteral("ripple")).toBool(true);
        if (!ripple && newDuration > info->frame_count) {
            error = QStringLiteral("slowing a clip requires ripple to protect following clips");
            return false;
        }
    } else if (type == QStringLiteral("add_filter")
               || type == QStringLiteral("set_filter_parameters")
               || type == QStringLiteral("set_filter_state")
               || type == QStringLiteral("remove_filter")
               || type == QStringLiteral("set_filter_keyframe")
               || type == QStringLiteral("remove_filter_keyframe")) {
        QString filterId;
        QString filterService;
        QmlMetadata *metadata = nullptr;
        Mlt::Producer producer = m_window.timelineDock()->producerForClip(track, clip);
        AttachedFiltersModel attachedFilters;
        attachedFilters.setProducer(&producer);
        if (type == QStringLiteral("add_filter")) {
            filterId = requiredString(operation, QStringLiteral("filter_id"));
            if (filterId.isEmpty() || filterId.size() > 512) {
                error = QStringLiteral("filter_id must be 1 to 512 characters");
                return false;
            }
            metadata = editableClipFilterMetadata(filterId);
            if (!metadata) {
                error = QStringLiteral("filter_id is unknown or not editable on clips");
                return false;
            }
            if (!validateClipFilterAddition(metadata, producer, error))
                return false;
            filterService = metadata->mlt_service();
        }
        if (type != QStringLiteral("add_filter")) {
            int filterIndex;
            if (!jsonInteger(operation, QStringLiteral("filter_index"), &filterIndex)
                || filterIndex < 0) {
                error = QStringLiteral("filter_index is invalid");
                return false;
            }
            if (!resolveEditableAttachedFilter(attachedFilters,
                                               filterIndex,
                                               filterId,
                                               filterService,
                                               error)) {
                return false;
            }
            metadata = attachedFilters.getMetadata(filterIndex);
        }

        if (type == QStringLiteral("set_filter_state")) {
            if (!operation.value(QStringLiteral("disabled")).isBool()) {
                error = QStringLiteral("disabled must be boolean");
                return false;
            }
            return true;
        }
        if (type == QStringLiteral("remove_filter"))
            return true;

        if (type == QStringLiteral("set_filter_keyframe")
            || type == QStringLiteral("remove_filter_keyframe")) {
            const QString property = requiredString(operation, QStringLiteral("property"));
            if (!McpBridgePolicy::filterParameterNameAllowed(property) || !metadata
                || !metadata->keyframes() || !metadata->keyframes()->parameter(property)) {
                error = QStringLiteral("property is not keyframe-capable on this filter");
                return false;
            }
            const auto *parameter = metadata->keyframes()->parameter(property);
            if (parameter->isRectangle() || parameter->isColor()) {
                error = QStringLiteral("this operation currently supports numeric keyframes only");
                return false;
            }
            int position = -1;
            auto info = m_window.timelineDock()->model()->getClipInfo(track, clip);
            if (!jsonInteger(operation, QStringLiteral("position"), &position) || position < 0
                || !info) {
                error = QStringLiteral("keyframe position must be a non-negative frame");
                return false;
            }
            const int filterIndex = operation.value(QStringLiteral("filter_index")).toInt(-1);
            QScopedPointer<Mlt::Service> service(attachedFilters.getService(filterIndex));
            if (!service || !service->is_valid()) {
                error = QStringLiteral("filter is unavailable");
                return false;
            }
            if (service->time_to_frames(service->get(kShotcutAnimInProperty)) > 0
                || service->time_to_frames(service->get(kShotcutAnimOutProperty)) > 0) {
                error = QStringLiteral(
                    "filter uses Shotcut simple keyframes; convert it to advanced keyframes first");
                return false;
            }
            int filterSourceIn = 0;
            int filterSourceOut = -1;
            int playlistStart = 0;
            if (!filterClipContext(track,
                                   clip,
                                   filterSourceIn,
                                   filterSourceOut,
                                   playlistStart)) {
                error = QStringLiteral("filter clip timing is unavailable");
                return false;
            }
            Q_UNUSED(playlistStart)
            int clipOffset = 0;
            int keyframeDuration = 0;
            if (!filterKeyframeTiming(producer,
                                      *service,
                                      filterSourceIn,
                                      filterSourceOut,
                                      clipOffset,
                                      keyframeDuration)
                || position < clipOffset || position >= clipOffset + keyframeDuration) {
                error = QStringLiteral(
                    "keyframe position must be inside the filter's active range");
                return false;
            }
            const int animationPosition = position - clipOffset;
            if (!service->get_animation(property.toUtf8().constData())) {
                service->anim_get_double(property.toUtf8().constData(), 0, keyframeDuration);
            }
            Mlt::Animation animation(service->get_animation(property.toUtf8().constData()));
            if (type == QStringLiteral("remove_filter_keyframe")) {
                if (!animation.is_valid() || !animation.is_key(animationPosition)
                    || animation.key_count() <= 1) {
                    error = QStringLiteral(
                        "keyframe does not exist or is the filter's last keyframe");
                    return false;
                }
                return true;
            }
            const auto value = operation.value(QStringLiteral("value"));
            if (!value.isDouble() || !qIsFinite(value.toDouble())
                || qAbs(value.toDouble()) > 1.0e12) {
                error = QStringLiteral("keyframe value must be a finite number");
                return false;
            }
            double minimum = parameter->minimum();
            double maximum = parameter->maximum();
            if (parameter->rangeType() == QmlKeyframesParameter::ClipLength) {
                const int filterIn = filterSourceIn + clipOffset;
                minimum = 0.0;
                maximum = static_cast<double>(qMax(0, producer.get_length() - filterIn))
                          / MLT.profile().fps();
            }
            if (!metadata->keyframes()->allowOvershoot()
                && (value.toDouble() < minimum || value.toDouble() > maximum)) {
                error = QStringLiteral("keyframe value is outside the parameter range");
                return false;
            }
            const bool hasInterpolation = operation.contains(QStringLiteral("interpolation"));
            if (hasInterpolation
                && (!operation.value(QStringLiteral("interpolation")).isString()
                    || !McpKeyframeInterpolation::parse(
                        operation.value(QStringLiteral("interpolation")).toString(), nullptr))) {
                error = QStringLiteral("interpolation is invalid");
                return false;
            }
            if (!hasInterpolation
                && (!animation.is_valid() || !animation.is_key(animationPosition))) {
                error = QStringLiteral("interpolation is required when creating a keyframe");
                return false;
            }
            return true;
        }

        const auto parametersValue = operation.value(QStringLiteral("parameters"));
        if (!parametersValue.isObject()) {
            error = QStringLiteral("parameters must be an object");
            return false;
        }
        const auto parameters = parametersValue.toObject();
        if (type == QStringLiteral("set_filter_parameters") && parameters.isEmpty()) {
            error = QStringLiteral("set_filter_parameters must change at least one property");
            return false;
        }
        for (auto it = parameters.constBegin(); it != parameters.constEnd(); ++it) {
            const QString name = it.key();
            const auto value = it.value();
            if (!McpBridgePolicy::filterParameterNameAllowed(name)) {
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
            if (value.isString() && !McpBridgePolicy::cStringValueAllowed(value.toString())) {
                error = QStringLiteral("filter parameter '%1' contains an embedded null").arg(name);
                return false;
            }
            if (value.isString() && value.toString().size() > 1024 * 1024) {
                error = QStringLiteral("filter parameter '%1' is too large").arg(name);
                return false;
            }
            if (!normalizeFilterPathParameter(filterId,
                                              filterService,
                                              name,
                                              value,
                                              nullptr,
                                              error)) {
                return false;
            }
        }
    }
    return true;
}

bool McpBridge::applyOperation(const QJsonObject &operation, QString &error)
{
    const QString type = operation.value(QStringLiteral("op")).toString();
    const bool filterOperation = type == QStringLiteral("add_filter")
                                 || type == QStringLiteral("set_filter_parameters")
                                 || type == QStringLiteral("set_filter_state")
                                 || type == QStringLiteral("remove_filter")
                                 || type == QStringLiteral("set_filter_keyframe")
                                 || type == QStringLiteral("remove_filter_keyframe");
    if (filterOperation)
        return applyFilterOperation(operation, error);
    const bool subtitleOperation = type == QStringLiteral("add_subtitle_track")
                                   || type == QStringLiteral("replace_subtitles");
    if (subtitleOperation)
        return applySubtitleOperation(operation, error);
    const bool markerOperation = type == QStringLiteral("add_marker")
                                 || type == QStringLiteral("update_marker")
                                 || type == QStringLiteral("remove_marker");
    if (markerOperation)
        return applyMarkerOperation(operation, error);
    return applyTimelineOperation(operation, error);
}
