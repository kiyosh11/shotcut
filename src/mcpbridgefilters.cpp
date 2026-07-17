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
#include "commands/subtitlecommands.h"
#include "controllers/filtercontroller.h"
#include "docks/timelinedock.h"
#include "mainwindow.h"
#include "models/attachedfiltersmodel.h"
#include "models/keyframesmodel.h"
#include "models/subtitlesmodel.h"
#include "qmltypes/qmlfilter.h"
#include "qmltypes/qmlmetadata.h"
#include "shotcut_mlt_properties.h"

#include <MltAnimation.h>
#include <MltProducer.h>
#include <QUndoStack>
#include <QtMath>

bool McpBridge::applySubtitleOperation(const QJsonObject &operation, QString &error)
{
    auto *subtitles = m_window.timelineDock()->subtitlesModel();
    auto *stack = m_window.undoStack();
    const QString type = operation.value(QStringLiteral("op")).toString();

    if (type == QStringLiteral("add_subtitle_track")) {
        SubtitlesModel::SubtitleTrack track{
            operation.value(QStringLiteral("name")).toString().trimmed(),
            operation.value(QStringLiteral("language")).toString(),
        };
        const int newIndex = subtitles->trackCount();
        stack->push(new Subtitles::InsertTrackCommand(*subtitles, track, newIndex));
        if (subtitles->trackCount() != newIndex + 1) {
            error = QStringLiteral("Shotcut did not add the subtitle track");
            return false;
        }
        const auto retained = subtitles->getTrack(newIndex);
        if (retained.name != track.name || retained.lang != track.lang) {
            error = QStringLiteral("Shotcut did not retain the subtitle track metadata");
            return false;
        }
        return true;
    }

    QList<Subtitles::SubtitleItem> items;
    for (const auto &value : operation.value(QStringLiteral("items")).toArray()) {
        const auto item = value.toObject();
        items.append(Subtitles::SubtitleItem{
            static_cast<int64_t>(item.value(QStringLiteral("start_ms")).toDouble()),
            static_cast<int64_t>(item.value(QStringLiteral("end_ms")).toDouble()),
            item.value(QStringLiteral("text")).toString().toUtf8().toStdString(),
        });
    }
    const int track = operation.value(QStringLiteral("track")).toInt();
    QList<Subtitles::SubtitleItem> previousItems;
    previousItems.reserve(subtitles->itemCount(track));
    for (int index = 0; index < subtitles->itemCount(track); ++index)
        previousItems.append(subtitles->getItem(track, index));
    if (!previousItems.isEmpty())
        stack->push(new Subtitles::RemoveSubtitlesCommand(*subtitles, track, previousItems));
    if (!items.isEmpty())
        stack->push(new Subtitles::OverwriteSubtitlesCommand(*subtitles, track, items));

    if (subtitles->itemCount(track) != items.size()) {
        error = QStringLiteral("Shotcut did not replace the subtitle track contents");
        return false;
    }
    for (int index = 0; index < items.size(); ++index) {
        const auto &expected = items.at(index);
        const auto &retained = subtitles->getItem(track, index);
        if (retained.start != expected.start || retained.end != expected.end
            || retained.text != expected.text) {
            error = QStringLiteral("Shotcut did not retain the replacement subtitles");
            return false;
        }
    }
    return true;
}

bool McpBridge::applyFilterOperation(const QJsonObject &operation, QString &error)
{
    const int track = operation.value(QStringLiteral("track")).toInt(-1);
    const int clip = operation.value(QStringLiteral("clip")).toInt(-1);
    const QString type = operation.value(QStringLiteral("op")).toString();
    Mlt::Producer producer = m_window.timelineDock()->producerForClip(track, clip);
    int filterSourceIn = 0;
    int filterSourceOut = -1;
    int playlistStart = 0;
    if (!producer.is_valid()
        || !filterClipContext(track,
                              clip,
                              filterSourceIn,
                              filterSourceOut,
                              playlistStart)) {
        error = QStringLiteral("filter clip timing is unavailable");
        return false;
    }
    producer.set(kFilterInProperty, filterSourceIn);
    producer.set(kFilterOutProperty, filterSourceOut);
    producer.set(kPlaylistStartProperty, playlistStart);
    producer.set(kMultitrackItemProperty,
                 QStringLiteral("%1:%2").arg(clip).arg(track).toLatin1().constData());

    if (type == QStringLiteral("add_filter")) {
        auto *metadata = editableClipFilterMetadata(
            operation.value(QStringLiteral("filter_id")).toString());
        if (!metadata) {
            error = QStringLiteral("filter_id is unknown or not editable on clips");
            return false;
        }
        if (!validateClipFilterAddition(metadata, producer, error))
            return false;

        auto *controller = m_window.filterController();
        controller->setCurrentFilter(QmlFilter::DeselectCurrentFilter);
        controller->setProducer(&producer);
        const int filterIndex = controller->attachedModel()->add(metadata);
        if (filterIndex < 0) {
            error = QStringLiteral("Shotcut could not add this filter to the clip");
            return false;
        }
        const auto parameters = operation.value(QStringLiteral("parameters")).toObject();
        if (!parameters.isEmpty())
            return applyFilterParameters(track, clip, filterIndex, parameters, error);
        return true;
    }

    auto *controller = m_window.filterController();
    controller->setCurrentFilter(QmlFilter::DeselectCurrentFilter);
    controller->setProducer(&producer);
    const int filterIndex = operation.value(QStringLiteral("filter_index")).toInt(-1);
    QString filterId;
    QString filterService;
    if (!resolveEditableAttachedFilter(*controller->attachedModel(),
                                       filterIndex,
                                       filterId,
                                       filterService,
                                       error)) {
        return false;
    }

    if (type == QStringLiteral("set_filter_state")) {
        auto *attached = controller->attachedModel();
        const QModelIndex index = attached->index(filterIndex);
        const bool currentlyDisabled = attached->data(index, Qt::CheckStateRole).toInt()
                                       != Qt::Checked;
        const bool requestedDisabled = operation.value(QStringLiteral("disabled")).toBool();
        if (currentlyDisabled != requestedDisabled)
            attached->setData(index, QVariant(), Qt::CheckStateRole);
        const bool updatedDisabled = attached->data(index, Qt::CheckStateRole).toInt()
                                     != Qt::Checked;
        if (updatedDisabled != requestedDisabled) {
            error = QStringLiteral("Shotcut did not update the filter state");
            return false;
        }
        return true;
    }

    if (type == QStringLiteral("remove_filter")) {
        auto *attached = controller->attachedModel();
        const int previousCount = attached->rowCount();
        attached->remove(filterIndex);
        if (attached->rowCount() != previousCount - 1) {
            error = QStringLiteral("Shotcut did not remove the filter");
            return false;
        }
        return true;
    }

    if (type == QStringLiteral("set_filter_keyframe")
        || type == QStringLiteral("remove_filter_keyframe")) {
        auto *metadata = controller->attachedModel()->getMetadata(filterIndex);
        controller->setCurrentFilter(filterIndex);
        auto *filter = controller->currentFilter();
        if (!metadata || !filter || !metadata->keyframes()) {
            error = QStringLiteral("filter keyframe metadata is unavailable");
            return false;
        }
        const QString property = operation.value(QStringLiteral("property")).toString();
        const int clipPosition = operation.value(QStringLiteral("position")).toInt(-1);
        int clipOffset = 0;
        int keyframeDuration = 0;
        if (!filterKeyframeTiming(filter->producer(),
                                  filter->service(),
                                  filterSourceIn,
                                  filterSourceOut,
                                  clipOffset,
                                  keyframeDuration)) {
            error = QStringLiteral("filter keyframe timing is unavailable");
            return false;
        }
        const int position = clipPosition - clipOffset;
        if (position < 0 || position >= keyframeDuration) {
            error = QStringLiteral("keyframe position is outside the filter's active range");
            return false;
        }
        const int existingCount = filter->keyframeCount(property);
        if (type == QStringLiteral("remove_filter_keyframe")) {
            KeyframesModel keyframes;
            keyframes.load(filter, metadata);
            const int parameterIndex = keyframes.parameterIndex(property);
            const int keyframeIndex = keyframes.keyframeIndex(parameterIndex, position);
            if (parameterIndex < 0 || keyframeIndex < 0) {
                error = QStringLiteral("Shotcut did not remove the keyframe");
                return false;
            }
            keyframes.remove(parameterIndex, keyframeIndex);
            Mlt::Animation updatedAnimation = filter->getAnimation(property);
            if (!updatedAnimation.is_valid() || updatedAnimation.is_key(position)) {
                error = QStringLiteral("Shotcut did not remove the keyframe");
                return false;
            }
            return true;
        }

        const double value = operation.value(QStringLiteral("value")).toDouble();
        const bool hasInterpolation = operation.contains(QStringLiteral("interpolation"));
        auto interpolation = KeyframesModel::LinearInterpolation;
        if (hasInterpolation
            && !McpKeyframeInterpolation::parse(
                operation.value(QStringLiteral("interpolation")).toString(), &interpolation)) {
            error = QStringLiteral("interpolation is invalid");
            return false;
        }
        if (existingCount > 0) {
            KeyframesModel keyframes;
            keyframes.load(filter, metadata);
            const int parameterIndex = keyframes.parameterIndex(property);
            const int existingIndex = keyframes.keyframeIndex(parameterIndex, position);
            if (parameterIndex < 0) {
                error = QStringLiteral("Shotcut did not expose the keyframe parameter");
                return false;
            }
            if (existingIndex >= 0) {
                keyframes.setKeyframeValue(parameterIndex, existingIndex, value);
                if (hasInterpolation)
                    keyframes.setInterpolation(parameterIndex, existingIndex, interpolation);
            } else {
                keyframes.addKeyframe(parameterIndex, value, position, interpolation);
            }
        } else {
            const auto *parameter = metadata->keyframes()->parameter(property);
            filter->startUndoTracking();
            filter->startUndoParameterCommand(QStringLiteral("AI filter keyframe"));
            filter->set(property, value, position, mlt_keyframe_type(interpolation));
            filter->updateUndoCommand(property);
            if (parameter) {
                for (const auto &gangedProperty : parameter->gangedProperties()) {
                    filter->set(gangedProperty,
                                value,
                                position,
                                mlt_keyframe_type(interpolation));
                    filter->updateUndoCommand(gangedProperty);
                }
            }
            filter->endUndoCommand();
            filter->stopUndoTracking();
            filter->startUndoTracking();
        }
        Mlt::Animation animation = filter->getAnimation(property);
        int keyIndex = -1;
        if (animation.is_valid()) {
            for (int index = 0; index < animation.key_count(); ++index) {
                if (animation.key_get_frame(index) == position) {
                    keyIndex = index;
                    break;
                }
            }
        }
        const double retainedValue = filter->getDouble(property, position);
        const double tolerance = qMax(0.000001, qAbs(value) * 0.000000001);
        if (!animation.is_valid() || keyIndex < 0
            || (hasInterpolation
                && animation.key_get_type(keyIndex) != mlt_keyframe_type(interpolation))
            || !qIsFinite(retainedValue) || qAbs(retainedValue - value) > tolerance) {
            error = QStringLiteral("Shotcut did not retain the keyframe value");
            return false;
        }
        return true;
    }

    return applyFilterParameters(track,
                                 clip,
                                 operation.value(QStringLiteral("filter_index")).toInt(-1),
                                 operation.value(QStringLiteral("parameters")).toObject(),
                                 error);
}

bool McpBridge::applyFilterParameters(
    int track, int clip, int filterIndex, const QJsonObject &parameters, QString &error)
{
    Mlt::Producer producer = m_window.timelineDock()->producerForClip(track, clip);
    int filterSourceIn = 0;
    int filterSourceOut = -1;
    int playlistStart = 0;
    if (!producer.is_valid()
        || !filterClipContext(track,
                              clip,
                              filterSourceIn,
                              filterSourceOut,
                              playlistStart)) {
        error = QStringLiteral("filter clip timing is unavailable");
        return false;
    }
    producer.set(kFilterInProperty, filterSourceIn);
    producer.set(kFilterOutProperty, filterSourceOut);
    producer.set(kPlaylistStartProperty, playlistStart);
    producer.set(kMultitrackItemProperty,
                 QStringLiteral("%1:%2").arg(clip).arg(track).toLatin1().constData());
    auto *controller = m_window.filterController();
    controller->setCurrentFilter(QmlFilter::DeselectCurrentFilter);
    controller->setProducer(&producer);
    QString filterId;
    QString filterService;
    if (!resolveEditableAttachedFilter(*controller->attachedModel(),
                                       filterIndex,
                                       filterId,
                                       filterService,
                                       error)) {
        return false;
    }

    controller->setCurrentFilter(filterIndex);
    auto *filter = controller->currentFilter();
    if (!filter) {
        error = QStringLiteral("filter is unavailable");
        return false;
    }

    filter->startUndoTracking();
    filter->startUndoParameterCommand(QStringLiteral("AI filter parameters"));
    for (auto it = parameters.constBegin(); it != parameters.constEnd(); ++it) {
        const auto value = it.value();
        QString parameterValue;
        if (!normalizeFilterPathParameter(filterId,
                                          filterService,
                                          it.key(),
                                          value,
                                          &parameterValue,
                                          error)) {
            filter->endUndoCommand();
            filter->stopUndoTracking();
            filter->startUndoTracking();
            return false;
        }
        if (value.isString()) {
            filter->set(it.key(), parameterValue);
        } else if (value.isBool()) {
            filter->set(it.key(), value.toBool());
        } else if (value.isDouble()) {
            const double number = value.toDouble();
            if (qIsFinite(number) && number == static_cast<double>(value.toInt()))
                filter->set(it.key(), value.toInt());
            else if (qIsFinite(number))
                filter->set(it.key(), number);
            else {
                error = QStringLiteral("filter parameter '%1' is not finite").arg(it.key());
                filter->endUndoCommand();
                filter->stopUndoTracking();
                filter->startUndoTracking();
                return false;
            }
        } else if (value.isNull()) {
            filter->resetProperty(it.key());
            // resetProperty() does not update the active UndoParameterCommand like the
            // typed set() overloads do. Capture the cleared property explicitly so redo
            // clears it again instead of restoring the value present before this request.
            filter->updateUndoCommand(it.key());
        } else {
            error = QStringLiteral("filter parameter '%1' must be string, boolean, number, or null")
                        .arg(it.key());
            filter->endUndoCommand();
            filter->stopUndoTracking();
            filter->startUndoTracking();
            return false;
        }
    }
    filter->endUndoCommand();
    filter->stopUndoTracking();
    filter->startUndoTracking();
    return true;
}
