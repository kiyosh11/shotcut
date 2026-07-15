/*
 * Copyright (c) 2026 Meltytech, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "mcpbridge.h"

#include "Logger.h"
#include "controllers/filtercontroller.h"
#include "docks/encodedock.h"
#include "docks/timelinedock.h"
#include "jobqueue.h"
#include "mainwindow.h"
#include "mltcontroller.h"
#include "models/attachedfiltersmodel.h"
#include "models/metadatamodel.h"
#include "models/multitrackmodel.h"
#include "models/subtitlesmodel.h"
#include "qmltypes/qmlmetadata.h"
#include "shotcut_mlt_properties.h"

#include <MltFilter.h>
#include <MltProducer.h>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QLockFile>
#include <QScopedPointer>
#include <QScopedValueRollback>
#include <QUndoStack>

namespace {
constexpr qsizetype kMaximumMessageBytes = 16 * 1024 * 1024;

QString trackTypeName(TrackType type)
{
    switch (type) {
    case AudioTrackType:
        return QStringLiteral("audio");
    case VideoTrackType:
        return QStringLiteral("video");
    case PlaylistTrackType:
        return QStringLiteral("playlist");
    case BlackTrackType:
        return QStringLiteral("black");
    case SilentTrackType:
        return QStringLiteral("silent");
    }
    return QStringLiteral("unknown");
}

QString jobState(const AbstractJob &job)
{
    if (!job.ran())
        return QStringLiteral("queued");
    if (job.state() == QProcess::Running)
        return job.paused() ? QStringLiteral("paused") : QStringLiteral("running");
    if (job.stopped())
        return QStringLiteral("stopped");
    if (job.exitStatus() != QProcess::NormalExit || job.exitCode() != 0)
        return QStringLiteral("failed");
    return QStringLiteral("completed");
}
} // namespace

McpBridge::RpcResult McpBridge::RpcResult::success(const QJsonValue &value)
{
    RpcResult result;
    result.ok = true;
    result.value = value;
    return result;
}

McpBridge::RpcResult McpBridge::RpcResult::failure(int code,
                                                   const QString &message,
                                                   const QJsonValue &data)
{
    RpcResult result;
    result.code = code;
    result.message = message;
    result.data = data;
    return result;
}

McpBridge::McpBridge(MainWindow &window, QObject *parent)
    : QObject(parent)
    , m_window(window)
{
    connect(&m_server, &QLocalServer::newConnection, this, &McpBridge::onNewConnection);
    connect(m_window.undoStack(), &QUndoStack::indexChanged, this, [this](int) {
        advanceRevision();
    });
    connect(&m_window, &MainWindow::producerOpened, this, [this](bool) { advanceRevision(); });
}

void McpBridge::advanceRevision()
{
    constexpr qint64 maximumExactJsonInteger = 9007199254740991LL;
    if (m_revision < maximumExactJsonInteger)
        ++m_revision;
}

McpBridge::~McpBridge()
{
    m_server.close();
    if (m_ownsEndpoint) {
        QLocalServer::removeServer(m_endpoint);
        m_ownsEndpoint = false;
    }
    if (m_endpointLock && m_endpointLock->isLocked())
        m_endpointLock->unlock();
}

bool McpBridge::startFromEnvironment()
{
    if (qEnvironmentVariableIntValue("SHOTCUT_MCP_ENABLE") != 1)
        return false;

    m_token = qgetenv("SHOTCUT_MCP_TOKEN");
    if (m_token.size() < 32) {
        LOG_WARNING() << "MCP bridge disabled: SHOTCUT_MCP_TOKEN must be at least 32 characters";
        return false;
    }

    m_endpoint = qEnvironmentVariable("SHOTCUT_MCP_ENDPOINT");
    if (m_endpoint.isEmpty()) {
#ifdef Q_OS_WIN
        m_endpoint = QStringLiteral("shotcut-mcp");
#else
        m_endpoint = QDir::temp().filePath(QStringLiteral("shotcut-mcp"));
#endif
    }

    loadAllowedRoots();

    const QByteArray endpointHash
        = QCryptographicHash::hash(m_endpoint.toUtf8(), QCryptographicHash::Sha256).toHex().left(24);
    const QString lockPath = QDir::temp().filePath(
        QStringLiteral("shotcut-mcp-%1.lock").arg(QString::fromLatin1(endpointHash)));
    m_endpointLock.reset(new QLockFile(lockPath));
    m_endpointLock->setStaleLockTime(0);
    if (!m_endpointLock->tryLock(0)) {
        LOG_WARNING() << "MCP bridge endpoint is already owned by another Shotcut instance";
        m_endpointLock.reset();
        return false;
    }

    QLocalSocket probe;
    probe.connectToServer(m_endpoint, QIODevice::ReadWrite);
    if (probe.waitForConnected(100)) {
        probe.abort();
        LOG_WARNING() << "MCP bridge endpoint is already served by another process";
        m_endpointLock->unlock();
        m_endpointLock.reset();
        return false;
    }

    QLocalServer::removeServer(m_endpoint);
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server.listen(m_endpoint)) {
        LOG_WARNING() << "MCP bridge failed to listen:" << m_server.errorString();
        m_endpointLock->unlock();
        m_endpointLock.reset();
        return false;
    }
    m_ownsEndpoint = true;
    LOG_INFO() << "MCP bridge listening on a same-user local socket";
    return true;
}

void McpBridge::onNewConnection()
{
    while (auto *socket = m_server.nextPendingConnection()) {
        m_buffers.insert(socket, QByteArray());
        connect(socket, &QLocalSocket::readyRead, this, [this, socket]() { onReadyRead(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket]() {
            m_buffers.remove(socket);
            socket->deleteLater();
        });
    }
}

void McpBridge::onReadyRead(QLocalSocket *socket)
{
    auto it = m_buffers.find(socket);
    if (it == m_buffers.end())
        return;
    it.value().append(socket->readAll());

    if (it.value().size() > kMaximumMessageBytes) {
        writeResponse(socket,
                      QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                  {QStringLiteral("id"), QJsonValue::Null},
                                  {QStringLiteral("error"),
                                   QJsonObject{{QStringLiteral("code"), -32600},
                                               {QStringLiteral("message"),
                                                QStringLiteral("Request exceeds size limit")}}}});
        return;
    }

    const auto newline = it.value().indexOf('\n');
    if (newline < 0)
        return;

    const QByteArray line = it.value().left(newline).trimmed();
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        writeResponse(socket,
                      QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                  {QStringLiteral("id"), QJsonValue::Null},
                                  {QStringLiteral("error"),
                                   QJsonObject{{QStringLiteral("code"), -32700},
                                               {QStringLiteral("message"),
                                                QStringLiteral("Invalid JSON request")}}}});
        return;
    }

    const auto request = document.object();
    const auto id = request.value(QStringLiteral("id"));
    if (request.value(QStringLiteral("jsonrpc")).toString() != QStringLiteral("2.0")
        || !request.value(QStringLiteral("method")).isString()
        || !request.value(QStringLiteral("params")).isObject() || id.isUndefined()) {
        writeResponse(socket,
                      QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                  {QStringLiteral("id"), id.isUndefined() ? QJsonValue::Null : id},
                                  {QStringLiteral("error"),
                                   QJsonObject{{QStringLiteral("code"), -32600},
                                               {QStringLiteral("message"),
                                                QStringLiteral("Invalid JSON-RPC request")}}}});
        return;
    }

    auto params = request.value(QStringLiteral("params")).toObject();
    const auto requestToken = params.take(QStringLiteral("token")).toString().toUtf8();
    if (requestToken.isEmpty() || requestToken != m_token) {
        writeResponse(socket,
                      QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                  {QStringLiteral("id"), id},
                                  {QStringLiteral("error"),
                                   QJsonObject{{QStringLiteral("code"), -32001},
                                               {QStringLiteral("message"),
                                                QStringLiteral("Bridge authentication failed")}}}});
        return;
    }

    if (m_busy) {
        const QJsonObject busyError{
            {QStringLiteral("code"), -32005},
            {QStringLiteral("message"), QStringLiteral("Shotcut is processing another MCP request")},
        };
        writeResponse(socket,
                      QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                  {QStringLiteral("id"), id},
                                  {QStringLiteral("error"), busyError}});
        return;
    }

    QScopedValueRollback<bool> requestGuard(m_busy, true);
    const auto result = dispatch(request.value(QStringLiteral("method")).toString(), params);
    QJsonObject response{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                         {QStringLiteral("id"), id}};
    if (result.ok) {
        response.insert(QStringLiteral("result"), result.value);
    } else {
        QJsonObject error{{QStringLiteral("code"), result.code},
                          {QStringLiteral("message"), result.message}};
        if (!result.data.isUndefined() && !result.data.isNull())
            error.insert(QStringLiteral("data"), result.data);
        response.insert(QStringLiteral("error"), error);
    }
    writeResponse(socket, response);
}

void McpBridge::writeResponse(QLocalSocket *socket, const QJsonObject &response)
{
    socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact));
    socket->write("\n");
    socket->flush();
    socket->disconnectFromServer();
}

McpBridge::RpcResult McpBridge::dispatch(const QString &method, QJsonObject params)
{
    if (method == QStringLiteral("editor.status"))
        return RpcResult::success(editorStatus());
    if (method == QStringLiteral("project.snapshot"))
        return RpcResult::success(projectSnapshot());
    if (method == QStringLiteral("project.open"))
        return openProject(params);
    if (method == QStringLiteral("project.save"))
        return saveProject(params);
    if (method == QStringLiteral("timeline.apply"))
        return applyEditPlan(params);
    if (method == QStringLiteral("history.undo"))
        return changeHistory(params, false);
    if (method == QStringLiteral("history.redo"))
        return changeHistory(params, true);
    if (method == QStringLiteral("export.start"))
        return startExport(params);
    if (method == QStringLiteral("export.status"))
        return exportStatus(params);
    return RpcResult::failure(-32601, QStringLiteral("Unknown bridge method: %1").arg(method));
}

QJsonObject McpBridge::editorStatus() const
{
    const auto *stack = m_window.undoStack();
    QJsonArray roots;
    for (const auto &root : m_allowedRoots)
        roots.append(root);

    QJsonArray presets;
    if (m_window.encodeDock()) {
        for (const auto &preset : m_window.encodeDock()->presetNames())
            presets.append(preset);
    }

    QJsonArray filterCatalog;
    auto *metadataModel = m_window.filterController()->metadataModel();
    for (int index = 0; index < metadataModel->sourceRowCount(); ++index) {
        const auto *metadata = metadataModel->getFromSource(index);
        if (!metadata || metadata->isHidden() || metadata->isDeprecated() || metadata->isTrackOnly()
            || metadata->isOutputOnly()) {
            continue;
        }
        const auto metadataType = metadata->type();
        if (metadataType != QmlMetadata::Filter && metadataType != QmlMetadata::Link
            && metadataType != QmlMetadata::FilterSet) {
            continue;
        }
        const QString id = metadata->uniqueId();
        if (id.isEmpty())
            continue;
        QString catalogType = QStringLiteral("filter");
        if (metadataType == QmlMetadata::Link)
            catalogType = QStringLiteral("link");
        else if (metadataType == QmlMetadata::FilterSet)
            catalogType = QStringLiteral("filter_set");
        filterCatalog.append(QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("name"), metadata->name()},
            {QStringLiteral("type"), catalogType},
            {QStringLiteral("audio"), metadata->isAudio()},
            {QStringLiteral("clip_only"), metadata->isClipOnly()},
            {QStringLiteral("allow_multiple"), metadata->allowMultiple()},
            {QStringLiteral("needs_gpu"), metadata->needsGPU()},
            {QStringLiteral("gpu_compatible"), metadata->isGpuCompatible()},
            {QStringLiteral("keywords"), metadata->keywords()},
        });
    }

    return QJsonObject{
        {QStringLiteral("bridge_protocol"), 1},
        {QStringLiteral("connected"), true},
        {QStringLiteral("project_path"), m_window.fileName()},
        {QStringLiteral("modified"), m_window.isWindowModified()},
        {QStringLiteral("revision"), static_cast<double>(m_revision)},
        {QStringLiteral("can_undo"), stack && stack->canUndo()},
        {QStringLiteral("can_redo"), stack && stack->canRedo()},
        {QStringLiteral("allowed_roots"), roots},
        {QStringLiteral("export_presets"), presets},
        {QStringLiteral("filter_catalog"), filterCatalog},
        {QStringLiteral("jobs"), exportJobs()},
    };
}

QJsonObject McpBridge::projectSnapshot() const
{
    auto *timeline = m_window.timelineDock();
    auto *model = timeline->model();
    QJsonArray tracks;

    for (int trackIndex = 0; trackIndex < model->trackList().size(); ++trackIndex) {
        const auto trackModelIndex = model->index(trackIndex);
        QJsonArray clips;
        const int clipCount = timeline->clipCount(trackIndex);
        for (int clipIndex = 0; clipIndex < clipCount; ++clipIndex) {
            const auto index = model->makeIndex(trackIndex, clipIndex);
            QJsonObject clip{
                {QStringLiteral("index"), clipIndex},
                {QStringLiteral("uuid"),
                 m_window.timelineClipUuid(trackIndex, clipIndex).toString(QUuid::WithoutBraces)},
                {QStringLiteral("name"), model->data(index, MultitrackModel::NameRole).toString()},
                {QStringLiteral("resource"),
                 model->data(index, MultitrackModel::ResourceRole).toString()},
                {QStringLiteral("service"),
                 model->data(index, MultitrackModel::ServiceRole).toString()},
                {QStringLiteral("blank"), model->data(index, MultitrackModel::IsBlankRole).toBool()},
                {QStringLiteral("transition"),
                 model->data(index, MultitrackModel::IsTransitionRole).toBool()},
                {QStringLiteral("start"), model->data(index, MultitrackModel::StartRole).toInt()},
                {QStringLiteral("duration"),
                 model->data(index, MultitrackModel::DurationRole).toInt()},
                {QStringLiteral("in"), model->data(index, MultitrackModel::InPointRole).toInt()},
                {QStringLiteral("out"), model->data(index, MultitrackModel::OutPointRole).toInt()},
                {QStringLiteral("speed"), model->data(index, MultitrackModel::SpeedRole).toDouble()},
                {QStringLiteral("fade_in"), model->data(index, MultitrackModel::FadeInRole).toInt()},
                {QStringLiteral("fade_out"),
                 model->data(index, MultitrackModel::FadeOutRole).toInt()},
                {QStringLiteral("gain"), model->data(index, MultitrackModel::GainRole).toDouble()},
            };

            QJsonArray filters;
            Mlt::Producer producer = timeline->producerForClip(trackIndex, clipIndex);
            if (producer.is_valid() && !producer.is_blank()) {
                AttachedFiltersModel attachedFilters;
                attachedFilters.setProducer(&producer);
                for (int filterIndex = 0; filterIndex < attachedFilters.rowCount(); ++filterIndex) {
                    QScopedPointer<Mlt::Service> service(attachedFilters.getService(filterIndex));
                    if (!service || !service->is_valid())
                        continue;
                    const auto *metadata = attachedFilters.getMetadata(filterIndex);
                    QString id = metadata ? metadata->uniqueId() : QString();
                    if (id.isEmpty())
                        id = QString::fromUtf8(service->get("mlt_service"));
                    const QString serviceType = metadata && metadata->type() == QmlMetadata::Link
                                                    ? QStringLiteral("link")
                                                    : QStringLiteral("filter");
                    QJsonObject parameters;
                    for (int propertyIndex = 0; propertyIndex < service->count(); ++propertyIndex) {
                        const QString name = QString::fromUtf8(service->get_name(propertyIndex));
                        if (name.startsWith(QLatin1Char('_'))
                            || name == QStringLiteral("mlt_service"))
                            continue;
                        if (name.startsWith(QStringLiteral("shotcut:")))
                            continue;
                        const QString value = QString::fromUtf8(service->get(propertyIndex));
                        if (value.size() <= 4096)
                            parameters.insert(name, value);
                    }
                    filters.append(QJsonObject{
                        {QStringLiteral("index"), filterIndex},
                        {QStringLiteral("id"), id},
                        {QStringLiteral("type"), serviceType},
                        {QStringLiteral("disabled"), service->get_int("disable") != 0},
                        {QStringLiteral("parameters"), parameters},
                    });
                }
            }
            clip.insert(QStringLiteral("filters"), filters);
            clips.append(clip);
        }

        const auto track = model->trackList().at(trackIndex);
        tracks.append(QJsonObject{
            {QStringLiteral("index"), trackIndex},
            {QStringLiteral("name"), model->getTrackName(trackIndex)},
            {QStringLiteral("type"), trackTypeName(track.type)},
            {QStringLiteral("muted"),
             model->data(trackModelIndex, MultitrackModel::IsMuteRole).toBool()},
            {QStringLiteral("hidden"),
             model->data(trackModelIndex, MultitrackModel::IsHiddenRole).toBool()},
            {QStringLiteral("composite"),
             model->data(trackModelIndex, MultitrackModel::IsCompositeRole).toBool()},
            {QStringLiteral("locked"),
             model->data(trackModelIndex, MultitrackModel::IsLockedRole).toBool()},
            {QStringLiteral("clips"), clips},
        });
    }

    QJsonArray selection;
    for (const auto &point : timeline->selection()) {
        const QJsonObject selectedClip{
            {QStringLiteral("track"), point.y()},
            {QStringLiteral("clip"), point.x()},
        };
        selection.append(selectedClip);
    }

    QJsonArray subtitleTracks;
    auto *subtitles = timeline->subtitlesModel();
    for (int trackIndex = 0; trackIndex < subtitles->trackCount(); ++trackIndex) {
        const auto track = subtitles->getTrack(trackIndex);
        QJsonArray items;
        for (int itemIndex = 0; itemIndex < subtitles->itemCount(trackIndex); ++itemIndex) {
            const auto &item = subtitles->getItem(trackIndex, itemIndex);
            const qsizetype textSize = static_cast<qsizetype>(item.text.size());
            const QString subtitleText = QString::fromUtf8(item.text.data(), textSize);
            items.append(QJsonObject{
                {QStringLiteral("index"), itemIndex},
                {QStringLiteral("start_ms"), static_cast<double>(item.start)},
                {QStringLiteral("end_ms"), static_cast<double>(item.end)},
                {QStringLiteral("text"), subtitleText},
            });
        }
        subtitleTracks.append(QJsonObject{
            {QStringLiteral("index"), trackIndex},
            {QStringLiteral("name"), track.name},
            {QStringLiteral("language"), track.lang},
            {QStringLiteral("items"), items},
        });
    }

    return QJsonObject{
        {QStringLiteral("revision"), static_cast<double>(m_revision)},
        {QStringLiteral("project_path"), m_window.fileName()},
        {QStringLiteral("modified"), m_window.isWindowModified()},
        {QStringLiteral("position"), timeline->position()},
        {QStringLiteral("current_track"), timeline->currentTrack()},
        {QStringLiteral("profile"),
         QJsonObject{{QStringLiteral("width"), MLT.profile().width()},
                     {QStringLiteral("height"), MLT.profile().height()},
                     {QStringLiteral("fps"), MLT.profile().fps()},
                     {QStringLiteral("progressive"), MLT.profile().progressive() != 0}}},
        {QStringLiteral("selection"), selection},
        {QStringLiteral("tracks"), tracks},
        {QStringLiteral("subtitle_tracks"), subtitleTracks},
    };
}

QJsonArray McpBridge::exportJobs(const QString &target) const
{
    QJsonArray result;
    QString requestedTarget;
    if (!target.isEmpty())
        requestedTarget = QFileInfo(target).absoluteFilePath();
    for (auto *job : JOBS.jobs()) {
        if (!job)
            continue;
        if (!requestedTarget.isEmpty()) {
            const QString jobTarget = QFileInfo(job->target()).absoluteFilePath();
            if (jobTarget != requestedTarget)
                continue;
        }
        result.append(QJsonObject{
            {QStringLiteral("label"), job->label()},
            {QStringLiteral("target"), job->target()},
            {QStringLiteral("state"), jobState(*job)},
        });
    }
    return result;
}

void McpBridge::loadAllowedRoots()
{
    m_allowedRoots.clear();
    auto configured = qEnvironmentVariable("SHOTCUT_MCP_ALLOWED_ROOTS")
                          .split(QDir::listSeparator(), Qt::SkipEmptyParts);
    if (configured.isEmpty())
        configured.append(QDir::homePath());

    for (const auto &root : configured) {
        QFileInfo info(root);
        QString normalized = QDir::fromNativeSeparators(info.canonicalFilePath());
        if (!normalized.isEmpty() && !m_allowedRoots.contains(normalized))
            m_allowedRoots.append(normalized);
    }
}

QString McpBridge::normalizedPathForPolicy(const QString &path, bool mustExist) const
{
    QFileInfo info(path);
    if (!info.isAbsolute() || (mustExist && !info.exists()))
        return QString();
    if (info.exists())
        return QDir::fromNativeSeparators(info.canonicalFilePath());

    QFileInfo parent(info.absolutePath());
    const QString parentPath = QDir::fromNativeSeparators(parent.canonicalFilePath());
    if (parentPath.isEmpty())
        return QString();
    return QDir::cleanPath(parentPath + QLatin1Char('/') + info.fileName());
}

bool McpBridge::pathAllowed(const QString &path, bool mustExist, QString *normalized) const
{
    const QString candidate = normalizedPathForPolicy(path, mustExist);
    if (candidate.isEmpty())
        return false;

    for (const auto &root : m_allowedRoots) {
        const auto comparison =
#ifdef Q_OS_WIN
            Qt::CaseInsensitive;
#else
            Qt::CaseSensitive;
#endif
        QString prefix = root;
        if (!prefix.endsWith(QLatin1Char('/')))
            prefix.append(QLatin1Char('/'));
        if (candidate.compare(root, comparison) == 0 || candidate.startsWith(prefix, comparison)) {
            if (normalized)
                *normalized = candidate;
            return true;
        }
    }
    return false;
}
