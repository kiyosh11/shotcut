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
#include "models/markersmodel.h"
#include "models/multitrackmodel.h"
#include "settings.h"
#include "shotcut_mlt_properties.h"
#include "util.h"

#include <MltChain.h>
#include <MltProducer.h>
#include <MltProperties.h>
#include <QColor>
#include <QUndoStack>
#include <QtMath>

#include <limits>
#include <memory>
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
        if (!name.isEmpty() && model->getTrackName(newIndex) != name) {
            error = QStringLiteral("Shotcut did not retain the requested track name");
            return false;
        }
        return true;
    }

    const int track = type == QStringLiteral("move_clip")
                          ? operation.value(QStringLiteral("from_track")).toInt(-1)
                          : operation.value(QStringLiteral("track")).toInt(-1);
    if (type == QStringLiteral("remove_track")) {
        const int previousCount = model->trackList().size();
        stack->push(new Timeline::RemoveTrackCommand(*model, track));
        if (model->trackList().size() != previousCount - 1) {
            error = QStringLiteral("Shotcut did not remove the requested track");
            return false;
        }
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

    if (type == QStringLiteral("set_clip_speed")) {
        auto info = model->getClipInfo(track, clip);
        if (!info || !info->producer || !info->producer->is_valid()) {
            error = QStringLiteral("clip media is unavailable");
            return false;
        }

        const double oldSpeed = Util::GetSpeedFromProducer(info->producer);
        const double newSpeed = operation.value(QStringLiteral("speed")).toDouble();
        const double speedRatio = oldSpeed / newSpeed;
        const double requestedLength = static_cast<double>(info->length) * speedRatio;
        if (!qIsFinite(speedRatio) || speedRatio <= 0.0 || !qIsFinite(requestedLength)
            || requestedLength > std::numeric_limits<int>::max()) {
            error = QStringLiteral("requested speed would exceed Shotcut's frame range");
            return false;
        }
        const QString resource = Util::GetFilenameFromProducer(info->producer, false);
        std::unique_ptr<Mlt::Producer> changed;
        if (qFuzzyCompare(newSpeed, 1.0)) {
            changed.reset(new Mlt::Chain(MLT.profile(), resource.toUtf8().constData()));
        } else {
            Mlt::Properties speedProperties;
            speedProperties.set("speed", newSpeed);
            const QString speedText = QString::fromLatin1(speedProperties.get("speed"));
            const QString timewarpResource
                = QStringLiteral("timewarp:%1:%2").arg(speedText, resource);
            changed.reset(new Mlt::Producer(MLT.profile(), timewarpResource.toUtf8().constData()));
            if (changed)
                changed->set(kShotcutProducerProperty, "avformat");
        }
        if (!changed || !changed->is_valid()) {
            error = QStringLiteral("MLT could not reopen this clip at the requested speed");
            return false;
        }

        Util::passProducerProperties(info->producer, changed.get());
        Util::updateCaption(changed.get());
        Mlt::Controller::copyFilters(*info->producer, *changed, false, MLT.FILTER_INDEX_ALL);
        const int length = qMax(1, qRound(requestedLength));
        const int sourceIn = info->frame_in;
        const int sourceOut = info->frame_out;
        const int in = qBound(0, qRound(sourceIn * speedRatio), length - 1);
        const int out = qBound(in, qRound(sourceOut * speedRatio), length - 1);
        changed->set("length", changed->frames_to_time(length, mlt_time_clock));
        changed->set_in_and_out(in, out);
        if (!qFuzzyCompare(newSpeed, 1.0))
            changed->set("warp_pitch", operation.value(QStringLiteral("preserve_pitch")).toBool());
        MLT.adjustClipFilters(*changed, sourceIn, sourceOut, in - sourceIn, sourceOut - out, 0);

        auto *command = new Timeline::UpdateCommand(*timeline, track, clip, info->start);
        command->setXmlAfter(MLT.XML(changed.get()));
        const bool ripple = operation.value(QStringLiteral("ripple")).toBool(true);
        command->setRipple(ripple);
        command->setRippleAllTracks(
            ripple && operation.value(QStringLiteral("ripple_all_tracks")).toBool());
        timeline->setSelection();
        stack->push(command);

        auto updated = model->getClipInfo(track, clip);
        const double updatedSpeed = updated && updated->producer
                                        ? Util::GetSpeedFromProducer(updated->producer)
                                        : 0.0;
        if (!updated || !updated->producer || !qIsFinite(updatedSpeed)
            || qAbs(updatedSpeed - newSpeed) > 0.000001) {
            error = QStringLiteral("Shotcut did not apply the requested clip speed");
            return false;
        }
        return true;
    }

    if (type == QStringLiteral("set_clip_gain")) {
        const double gain = operation.value(QStringLiteral("gain")).toDouble();
        stack->push(new Timeline::ChangeGainCommand(*model, track, clip, gain));
        const auto modelIndex = model->index(clip, 0, model->index(track));
        const double retainedGain = model->data(modelIndex, MultitrackModel::GainRole).toDouble();
        if (!qIsFinite(retainedGain) || qAbs(retainedGain - gain) > 0.000001) {
            error = QStringLiteral("Shotcut did not retain the requested clip gain");
            return false;
        }
        return true;
    }

    if (type == QStringLiteral("set_clip_fade")) {
        const int duration = operation.value(QStringLiteral("duration_frames")).toInt();
        if (operation.value(QStringLiteral("edge")).toString() == QStringLiteral("in"))
            stack->push(new Timeline::FadeInCommand(*model, track, clip, duration));
        else
            stack->push(new Timeline::FadeOutCommand(*model, track, clip, duration));
        const auto modelIndex = model->index(clip, 0, model->index(track));
        const int role = operation.value(QStringLiteral("edge")).toString() == QStringLiteral("in")
                             ? MultitrackModel::FadeInRole
                             : MultitrackModel::FadeOutRole;
        if (model->data(modelIndex, role).toInt() != duration) {
            error = QStringLiteral("Shotcut did not retain the requested clip fade");
            return false;
        }
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
        const auto retainedIndex = model->index(track);
        const bool nameMatches = !operation.contains(QStringLiteral("name"))
                                 || model->getTrackName(track) == name;
        const bool mutedMatches = !operation.value(QStringLiteral("muted")).isBool()
                                  || model->data(retainedIndex, MultitrackModel::IsMuteRole).toBool()
                                         == operation.value(QStringLiteral("muted")).toBool();
        const bool hiddenMatches
            = !operation.value(QStringLiteral("hidden")).isBool()
              || model->data(retainedIndex, MultitrackModel::IsHiddenRole).toBool()
                     == operation.value(QStringLiteral("hidden")).toBool();
        const bool compositeMatches
            = !operation.value(QStringLiteral("composite")).isBool()
              || model->data(retainedIndex, MultitrackModel::IsCompositeRole).toBool()
                     == operation.value(QStringLiteral("composite")).toBool();
        const bool lockedMatches
            = !operation.value(QStringLiteral("locked")).isBool()
              || model->data(retainedIndex, MultitrackModel::IsLockedRole).toBool()
                     == operation.value(QStringLiteral("locked")).toBool();
        if (!nameMatches || !mutedMatches || !hiddenMatches || !compositeMatches || !lockedMatches) {
            error = QStringLiteral("Shotcut did not retain the requested track state");
            return false;
        }
        return true;
    }

    error = QStringLiteral("timeline operation is not implemented");
    return false;
}

bool McpBridge::applyMarkerOperation(const QJsonObject &operation, QString &error)
{
    auto *model = m_window.timelineDock()->markersModel();
    const QString type = operation.value(QStringLiteral("op")).toString();

    if (type == QStringLiteral("add_marker")) {
        Markers::Marker marker;
        marker.text = operation.value(QStringLiteral("text")).toString();
        marker.start = operation.value(QStringLiteral("start")).toInt();
        marker.end = operation.contains(QStringLiteral("end"))
                         ? operation.value(QStringLiteral("end")).toInt()
                         : marker.start;
        marker.color = operation.contains(QStringLiteral("color"))
                           ? QColor(operation.value(QStringLiteral("color")).toString())
                           : Settings.markerColor();
        const int previousCount = model->getMarkers().size();
        model->append(marker);
        if (model->getMarkers().size() != previousCount + 1) {
            error = QStringLiteral("Shotcut did not add the marker");
            return false;
        }
        return true;
    }

    const int markerIndex = operation.value(QStringLiteral("marker_index")).toInt(-1);
    auto markers = model->getMarkers();
    if (markerIndex < 0 || markerIndex >= markers.size()) {
        error = QStringLiteral("marker does not exist");
        return false;
    }
    if (type == QStringLiteral("remove_marker")) {
        model->remove(markerIndex);
        if (model->getMarkers().size() != markers.size() - 1) {
            error = QStringLiteral("Shotcut did not remove the marker");
            return false;
        }
        return true;
    }

    auto marker = markers.at(markerIndex);
    const bool wasPointMarker = marker.start == marker.end;
    if (operation.contains(QStringLiteral("text")))
        marker.text = operation.value(QStringLiteral("text")).toString();
    if (operation.contains(QStringLiteral("start"))) {
        marker.start = operation.value(QStringLiteral("start")).toInt();
        if (wasPointMarker && !operation.contains(QStringLiteral("end")))
            marker.end = marker.start;
    }
    if (operation.contains(QStringLiteral("end")))
        marker.end = operation.value(QStringLiteral("end")).toInt();
    if (operation.contains(QStringLiteral("color")))
        marker.color = QColor(operation.value(QStringLiteral("color")).toString());
    model->update(markerIndex, marker);
    const auto updatedMarkers = model->getMarkers();
    if (markerIndex >= updatedMarkers.size()) {
        error = QStringLiteral("Shotcut did not update the marker");
        return false;
    }
    const auto &updated = updatedMarkers.at(markerIndex);
    if (updated.text != marker.text || updated.start != marker.start || updated.end != marker.end
        || updated.color.rgb() != marker.color.rgb()) {
        error = QStringLiteral("Shotcut did not retain the marker values");
        return false;
    }
    return true;
}
