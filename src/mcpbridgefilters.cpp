/*
 * Copyright (c) 2026 Meltytech, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "mcpbridge.h"

#include "commands/subtitlecommands.h"
#include "controllers/filtercontroller.h"
#include "docks/timelinedock.h"
#include "mainwindow.h"
#include "models/attachedfiltersmodel.h"
#include "models/subtitlesmodel.h"
#include "qmltypes/qmlfilter.h"
#include "qmltypes/qmlmetadata.h"

#include <MltProducer.h>
#include <QUndoStack>
#include <QtMath>

bool McpBridge::applySubtitleOperation(const QJsonObject &operation, QString &error)
{
    Q_UNUSED(error);
    auto *subtitles = m_window.timelineDock()->subtitlesModel();
    auto *stack = m_window.undoStack();
    const QString type = operation.value(QStringLiteral("op")).toString();

    if (type == QStringLiteral("add_subtitle_track")) {
        SubtitlesModel::SubtitleTrack track{
            operation.value(QStringLiteral("name")).toString().trimmed(),
            operation.value(QStringLiteral("language")).toString(),
        };
        stack->push(new Subtitles::InsertTrackCommand(*subtitles, track, subtitles->trackCount()));
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
    stack->push(new Subtitles::OverwriteSubtitlesCommand(*subtitles, track, items));
    return true;
}

bool McpBridge::applyFilterOperation(const QJsonObject &operation, QString &error)
{
    const int track = operation.value(QStringLiteral("track")).toInt(-1);
    const int clip = operation.value(QStringLiteral("clip")).toInt(-1);
    const QString type = operation.value(QStringLiteral("op")).toString();

    if (type == QStringLiteral("add_filter")) {
        Mlt::Producer producer = m_window.timelineDock()->producerForClip(track, clip);
        auto *controller = m_window.filterController();
        controller->setCurrentFilter(QmlFilter::DeselectCurrentFilter);
        controller->setProducer(&producer);
        auto *metadata = controller->metadata(
            operation.value(QStringLiteral("filter_id")).toString());
        if (!metadata) {
            error = QStringLiteral("unknown filter_id");
            return false;
        }
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
    auto *controller = m_window.filterController();
    controller->setCurrentFilter(QmlFilter::DeselectCurrentFilter);
    controller->setProducer(&producer);
    if (filterIndex < 0 || filterIndex >= controller->attachedModel()->rowCount()) {
        error = QStringLiteral("filter_index does not exist");
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
        if (value.isString()) {
            filter->set(it.key(), value.toString());
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
                return false;
            }
        } else if (value.isNull()) {
            filter->resetProperty(it.key());
        } else {
            error = QStringLiteral("filter parameter '%1' must be string, boolean, number, or null")
                        .arg(it.key());
            filter->endUndoCommand();
            filter->stopUndoTracking();
            return false;
        }
    }
    filter->endUndoCommand();
    filter->stopUndoTracking();
    return true;
}
