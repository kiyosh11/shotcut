/*
 * Copyright (c) 2026 Meltytech, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "mcpbridge.h"

#include "commands/timelinecommands.h"
#include "docks/timelinedock.h"
#include "mainwindow.h"
#include "mltcontroller.h"
#include "models/multitrackmodel.h"

#include <MltProducer.h>
#include <QUndoStack>

#include <vector>

bool McpBridge::applyTimelineOperation(const QJsonObject &operation, QString &error)
{
    const QString type = operation.value(QStringLiteral("op")).toString();
    auto *timeline = m_window.timelineDock();
    auto *model = timeline->model();
    auto *stack = m_window.undoStack();

    if (type == QStringLiteral("add_track")) {
        const bool video = operation.value(QStringLiteral("kind")).toString()
                           == QStringLiteral("video");
        int newIndex = -1;
        if (operation.value(QStringLiteral("index")).isDouble()) {
            const int requestedIndex = operation.value(QStringLiteral("index")).toInt();
            if (!video && model->trackList().isEmpty()) {
                auto *command = new Timeline::AddTrackCommand(*model, false);
                stack->push(command);
                newIndex = command->trackIndex();
            } else {
                auto *command = new Timeline::InsertTrackCommand(*model,
                                                                 requestedIndex,
                                                                 video ? VideoTrackType
                                                                       : AudioTrackType);
                stack->push(command);
                newIndex = command->trackIndex();
            }
        } else {
            auto *command = new Timeline::AddTrackCommand(*model, video);
            stack->push(command);
            newIndex = command->trackIndex();
        }

        const TrackType expectedType = video ? VideoTrackType : AudioTrackType;
        if (newIndex < 0 || newIndex >= model->trackList().size()
            || model->trackList().at(newIndex).type != expectedType) {
            error = QStringLiteral("Shotcut did not create the requested track");
            return false;
        }

        const QString name = operation.value(QStringLiteral("name")).toString().trimmed();
        if (!name.isEmpty())
            stack->push(new Timeline::NameTrackCommand(*model, newIndex, name));
        return true;
    }

    const int track = type == QStringLiteral("move_clip")
                          ? operation.value(QStringLiteral("from_track")).toInt(-1)
                          : operation.value(QStringLiteral("track")).toInt(-1);
    if (type == QStringLiteral("remove_track")) {
        stack->push(new Timeline::RemoveTrackCommand(*model, track));
        return true;
    }

    if (type == QStringLiteral("insert_media")) {
        QString path;
        if (!pathAllowed(operation.value(QStringLiteral("path")).toString(), true, &path)) {
            error = QStringLiteral("media path is missing or outside allowed roots");
            return false;
        }
        if (!validateMediaResourcePaths(path, error))
            return false;
        Mlt::Producer producer(MLT.profile(), path.toUtf8().constData());
        if (!producer.is_valid()) {
            error = QStringLiteral("MLT could not open media: %1").arg(path);
            return false;
        }
        if (operation.value(QStringLiteral("in_frame")).isDouble()
            || operation.value(QStringLiteral("out_frame")).isDouble()) {
            const int in = operation.value(QStringLiteral("in_frame")).toInt(producer.get_in());
            const int out = operation.value(QStringLiteral("out_frame")).toInt(producer.get_out());
            if (in < 0 || out < in || out >= producer.get_length()) {
                error = QStringLiteral("invalid source in/out range");
                return false;
            }
            producer.set_in_and_out(in, out);
        }
        stack->push(new Timeline::InsertCommand(*model,
                                                *timeline->markersModel(),
                                                track,
                                                operation.value(QStringLiteral("position")).toInt(),
                                                MLT.XML(&producer),
                                                false));
        return true;
    }

    const int clip = operation.value(QStringLiteral("clip")).toInt(-1);
    if (type == QStringLiteral("move_clip")) {
        auto info = model->getClipInfo(track, clip);
        if (!info) {
            error = QStringLiteral("clip information is unavailable");
            return false;
        }
        const int toTrack = operation.value(QStringLiteral("to_track")).toInt();
        const int positionDelta = operation.value(QStringLiteral("position")).toInt() - info->start;
        const bool ripple = operation.value(QStringLiteral("ripple")).toBool();
        auto *command
            = new Timeline::MoveClipCommand(*timeline, toTrack - track, positionDelta, ripple);
        command->addClip(track, clip);
        stack->push(command);
        return true;
    }

    if (type == QStringLiteral("trim_clip")) {
        const int delta = operation.value(QStringLiteral("delta_frames")).toInt();
        const bool ripple = operation.value(QStringLiteral("ripple")).toBool();
        if (operation.value(QStringLiteral("edge")).toString() == QStringLiteral("in")) {
            if (!model->trimClipInValid(track, clip, delta, ripple)) {
                error = QStringLiteral("trim-in exceeds available clip media or timeline space");
                return false;
            }
            stack->push(new Timeline::TrimClipInCommand(*model,
                                                        *timeline->markersModel(),
                                                        track,
                                                        clip,
                                                        delta,
                                                        ripple));
        } else {
            if (!model->trimClipOutValid(track, clip, delta, ripple)) {
                error = QStringLiteral("trim-out exceeds available clip media or timeline space");
                return false;
            }
            stack->push(new Timeline::TrimClipOutCommand(*model,
                                                         *timeline->markersModel(),
                                                         track,
                                                         clip,
                                                         delta,
                                                         ripple));
        }
        return true;
    }

    if (type == QStringLiteral("split_clip")) {
        const int position = operation.value(QStringLiteral("position")).toInt();
        auto info = model->getClipInfo(track, clip);
        if (!info || position <= info->start || position >= info->start + info->frame_count) {
            error = QStringLiteral("split position must be inside the clip");
            return false;
        }
        stack->push(new Timeline::SplitCommand(*model,
                                               std::vector<int>{track},
                                               std::vector<int>{clip},
                                               position));
        return true;
    }

    if (type == QStringLiteral("remove_clip")) {
        if (operation.value(QStringLiteral("ripple")).toBool()) {
            auto *markers = timeline->markersModel();
            stack->push(new Timeline::RemoveCommand(*model, *markers, track, clip));
        } else {
            stack->push(new Timeline::LiftCommand(*model, track, clip));
        }
        return true;
    }

    if (type == QStringLiteral("set_clip_gain")) {
        const double gain = operation.value(QStringLiteral("gain")).toDouble();
        stack->push(new Timeline::ChangeGainCommand(*model, track, clip, gain));
        return true;
    }

    if (type == QStringLiteral("set_clip_fade")) {
        const int duration = operation.value(QStringLiteral("duration_frames")).toInt();
        if (operation.value(QStringLiteral("edge")).toString() == QStringLiteral("in"))
            stack->push(new Timeline::FadeInCommand(*model, track, clip, duration));
        else
            stack->push(new Timeline::FadeOutCommand(*model, track, clip, duration));
        return true;
    }

    if (type == QStringLiteral("add_transition")) {
        const int position = operation.value(QStringLiteral("position")).toInt();
        const bool ripple = operation.value(QStringLiteral("ripple")).toBool();
        if (!model->addTransitionValid(track, track, clip, position, ripple)) {
            error = QStringLiteral("transition position is not valid for this clip");
            return false;
        }
        auto *command = new Timeline::AddTransitionCommand(*timeline, track, clip, position, ripple);
        stack->push(command);
        if (command->getTransitionIndex() < 0) {
            error = QStringLiteral("Shotcut could not create the transition");
            return false;
        }
        return true;
    }

    if (type == QStringLiteral("set_track_state")) {
        const auto index = model->index(track);
        const QString name = operation.value(QStringLiteral("name")).toString();
        if (operation.contains(QStringLiteral("name")) && name != model->getTrackName(track))
            stack->push(new Timeline::NameTrackCommand(*model, track, name));
        if (operation.value(QStringLiteral("muted")).isBool()
            && operation.value(QStringLiteral("muted")).toBool()
                   != model->data(index, MultitrackModel::IsMuteRole).toBool())
            stack->push(new Timeline::MuteTrackCommand(*model, track));
        if (operation.value(QStringLiteral("hidden")).isBool()
            && operation.value(QStringLiteral("hidden")).toBool()
                   != model->data(index, MultitrackModel::IsHiddenRole).toBool())
            stack->push(new Timeline::HideTrackCommand(*model, track));
        if (operation.value(QStringLiteral("composite")).isBool()
            && operation.value(QStringLiteral("composite")).toBool()
                   != model->data(index, MultitrackModel::IsCompositeRole).toBool())
            stack->push(new Timeline::CompositeTrackCommand(
                *model, track, operation.value(QStringLiteral("composite")).toBool()));
        if (operation.value(QStringLiteral("locked")).isBool()
            && operation.value(QStringLiteral("locked")).toBool()
                   != model->data(index, MultitrackModel::IsLockedRole).toBool()) {
            const bool locked = operation.value(QStringLiteral("locked")).toBool();
            stack->push(new Timeline::LockTrackCommand(*model, track, locked));
        }
        return true;
    }

    error = QStringLiteral("timeline operation is not implemented");
    return false;
}
