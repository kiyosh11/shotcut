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
#include "mcpxmlpathvalidator.h"

#include "Logger.h"
#include "actions.h"
#include "controllers/filtercontroller.h"
#include "docks/encodedock.h"
#include "docks/timelinedock.h"
#include "jobqueue.h"
#include "localpath.h"
#include "mainwindow.h"
#include "mltcontroller.h"
#include "models/attachedfiltersmodel.h"
#include "models/metadatamodel.h"
#include "models/multitrackmodel.h"
#include "models/subtitlesmodel.h"
#include "qmltypes/qmlmetadata.h"
#include "qmltypes/qmlutilities.h"
#include "settings.h"
#include "shotcut_mlt_properties.h"

#include <MltAnimation.h>
#include <MltFilter.h>
#include <MltProducer.h>
#include <QAction>
#include <QColor>
#include <QCryptographicHash>
#include <QDir>
#include <QDockWidget>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLocalSocket>
#include <QLockFile>
#include <QMenu>
#include <QScopedPointer>
#include <QScopedValueRollback>
#include <QUndoStack>
#include <QVBoxLayout>
#include <QWidget>

namespace {
constexpr qsizetype kMaximumMessageBytes = 16 * 1024 * 1024;
constexpr int kBridgeProtocolVersion = 2;

bool pathWithinRoot(const QString &candidatePath, const QString &rootPath)
{
    const QString candidate = LocalPath::normalized(candidatePath);
    QString root = LocalPath::normalized(rootPath);
    if (candidate.isEmpty() || root.isEmpty())
        return false;
    if (candidate.compare(root, LocalPath::caseSensitivity()) == 0)
        return true;
    if (!root.endsWith(QLatin1Char('/')))
        root.append(QLatin1Char('/'));
    return candidate.startsWith(root, LocalPath::caseSensitivity());
}

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

QString profileColorspaceName(int colorspace)
{
    switch (colorspace) {
    case 601:
        return QStringLiteral("bt601");
    case 709:
        return QStringLiteral("bt709");
    case 2020:
        return QStringLiteral("bt2020");
    default:
        return QStringLiteral("unknown");
    }
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

std::unique_ptr<McpBridge> McpBridge::createFromEnvironment(MainWindow &window)
{
    if (qEnvironmentVariableIntValue("SHOTCUT_MCP_ENABLE") != 1)
        return {};

    std::unique_ptr<McpBridge> bridge(new McpBridge(window));
    if (!bridge->startFromEnvironment())
        return {};
    return bridge;
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
    connect(&m_window, &MainWindow::profileChanged, this, [this]() { refreshAutomationDock(); });
    connect(&m_window,
            &QWidget::windowTitleChanged,
            this,
            [this](const QString &) { refreshAutomationDock(); });
}

void McpBridge::advanceRevision()
{
    constexpr qint64 maximumExactJsonInteger = 9007199254740991LL;
    if (m_revision < maximumExactJsonInteger)
        ++m_revision;
    refreshAutomationDock();
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
    setupAutomationDock();
    return true;
}

void McpBridge::setupAutomationDock()
{
    if (m_automationDock)
        return;

    m_automationDock = new QDockWidget(tr("AI Automation"), &m_window);
    m_automationDock->setObjectName(QStringLiteral("McpAutomationDock"));
    m_automationDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    m_automationDock->setMinimumWidth(300);

    auto *content = new QWidget(m_automationDock);
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(10);

    auto *title = new QLabel(tr("Shotcut MCP"), content);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);
    layout->addWidget(title);

    m_automationConnectionLabel = new QLabel(content);
    m_automationConnectionLabel->setTextFormat(Qt::PlainText);
    QFont connectionFont = m_automationConnectionLabel->font();
    connectionFont.setBold(true);
    m_automationConnectionLabel->setFont(connectionFont);
    layout->addWidget(m_automationConnectionLabel);

    auto *description = new QLabel(
        tr("A local, authenticated AI client can inspect the live project and submit typed, "
           "revision-checked edits. Timeline edits use Shotcut history; profile changes "
           "explicitly report their required history reset."),
        content);
    description->setWordWrap(true);
    layout->addWidget(description);

    auto *rule = new QFrame(content);
    rule->setFrameShape(QFrame::HLine);
    rule->setFrameShadow(QFrame::Sunken);
    layout->addWidget(rule);

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_automationProjectLabel = new QLabel(content);
    m_automationProjectLabel->setTextFormat(Qt::PlainText);
    m_automationProjectLabel->setTextInteractionFlags(Qt::TextSelectableByMouse
                                                       | Qt::TextSelectableByKeyboard);
    m_automationProjectLabel->setFocusPolicy(Qt::TabFocus);
    m_automationProjectLabel->setWordWrap(true);
    m_automationRevisionLabel = new QLabel(content);
    m_automationRevisionLabel->setTextFormat(Qt::PlainText);
    m_automationActivityLabel = new QLabel(content);
    m_automationActivityLabel->setTextFormat(Qt::PlainText);
    m_automationActivityLabel->setWordWrap(true);
    form->addRow(tr("Project"), m_automationProjectLabel);
    form->addRow(tr("Revision"), m_automationRevisionLabel);
    form->addRow(tr("Last request"), m_automationActivityLabel);

    auto *endpointLabel = new QLabel(content);
    endpointLabel->setTextFormat(Qt::PlainText);
    endpointLabel->setText(m_endpoint);
    endpointLabel->setTextInteractionFlags(Qt::TextSelectableByMouse
                                           | Qt::TextSelectableByKeyboard);
    endpointLabel->setFocusPolicy(Qt::TabFocus);
    endpointLabel->setWordWrap(true);
    form->addRow(tr("Local endpoint"), endpointLabel);

    auto *rootsLabel = new QLabel(content);
    rootsLabel->setTextFormat(Qt::PlainText);
    rootsLabel->setText(m_allowedRoots.isEmpty()
                            ? tr("None - file access disabled")
                            : m_allowedRoots.join(QLatin1Char('\n')));
    rootsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse
                                        | Qt::TextSelectableByKeyboard);
    rootsLabel->setFocusPolicy(Qt::TabFocus);
    rootsLabel->setWordWrap(true);
    form->addRow(tr("Allowed folders"), rootsLabel);
    layout->addLayout(form);

    auto *safety = new QLabel(
        tr("The session token is never displayed. File access stays inside the allowed folders, "
           "and the bridge does not expose a shell or arbitrary application actions."),
        content);
    safety->setWordWrap(true);
    safety->setProperty("class", QStringLiteral("information"));
    layout->addWidget(safety);
    layout->addStretch();

    m_automationDock->setWidget(content);
    const bool layoutRestored = m_window.restoreDockWidget(m_automationDock);
    if (!layoutRestored)
        m_window.addDockWidget(Qt::RightDockWidgetArea, m_automationDock);
    auto *toggleAction = m_automationDock->toggleViewAction();
    Actions.add(QStringLiteral("mcpAutomationDockAction"), toggleAction, tr("View"));
    const auto savedShortcuts = Settings.shortcuts(toggleAction->objectName());
    if (!savedShortcuts.isEmpty())
        Actions.overrideShortcuts(toggleAction->objectName(), savedShortcuts);
    if (auto *viewMenu = m_window.findChild<QMenu *>(QStringLiteral("menuView")))
        viewMenu->addAction(toggleAction);
    if (!layoutRestored) {
        m_automationDock->show();
        m_automationDock->raise();
    }
    refreshAutomationDock();
}

void McpBridge::refreshAutomationDock(const QString &request, bool succeeded)
{
    if (!m_automationDock)
        return;

    if (m_automationConnectionLabel) {
        m_automationConnectionLabel->setText(
            tr("Bridge active - %n open local connection(s)",
               nullptr,
               static_cast<int>(m_buffers.size())));
    }
    if (m_automationProjectLabel) {
        const QString path = m_window.fileName();
        const QString display = path.isEmpty() ? tr("Untitled") : QFileInfo(path).fileName();
        m_automationProjectLabel->setText(display);
        m_automationProjectLabel->setToolTip(
            QStringLiteral("<p>%1</p>").arg(path.toHtmlEscaped()));
    }
    if (m_automationRevisionLabel)
        m_automationRevisionLabel->setText(QString::number(m_revision));
    if (m_automationActivityLabel && !request.isEmpty()) {
        const QString displayRequest = request.left(128);
        m_automationActivityLabel->setText(
            succeeded ? tr("%1 completed").arg(displayRequest)
                      : tr("%1 failed").arg(displayRequest));
    } else if (m_automationActivityLabel && m_automationActivityLabel->text().isEmpty()) {
        m_automationActivityLabel->setText(tr("Waiting for a request"));
    }
}

void McpBridge::onNewConnection()
{
    while (auto *socket = m_server.nextPendingConnection()) {
        m_buffers.insert(socket, QByteArray());
        refreshAutomationDock();
        connect(socket, &QLocalSocket::readyRead, this, [this, socket]() { onReadyRead(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket]() {
            m_buffers.remove(socket);
            refreshAutomationDock();
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

    const auto protocolValue = params.take(QStringLiteral("bridge_protocol"));
    const bool validProtocol = protocolValue.isDouble()
                               && protocolValue.toDouble()
                                      == static_cast<double>(protocolValue.toInt())
                               && protocolValue.toInt() == kBridgeProtocolVersion;
    if (!validProtocol) {
        writeResponse(
            socket,
            QJsonObject{
                {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), id},
                {QStringLiteral("error"),
                 QJsonObject{
                     {QStringLiteral("code"), -32006},
                     {QStringLiteral("message"),
                      QStringLiteral("Incompatible Shotcut MCP bridge protocol")},
                     {QStringLiteral("data"),
                      QJsonObject{
                          {QStringLiteral("expected"), kBridgeProtocolVersion},
                          {QStringLiteral("received"), protocolValue},
                      }},
                 }},
            });
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
    const QString method = request.value(QStringLiteral("method")).toString();
    const auto result = dispatch(method, params);
    refreshAutomationDock(method, result.ok);
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
    QJsonObject versionedResponse = response;
    versionedResponse.insert(QStringLiteral("bridge_protocol"), kBridgeProtocolVersion);
    QByteArray payload = QJsonDocument(versionedResponse).toJson(QJsonDocument::Compact);
    if (payload.size() >= kMaximumMessageBytes) {
        versionedResponse = QJsonObject{
            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), response.value(QStringLiteral("id"))},
            {QStringLiteral("bridge_protocol"), kBridgeProtocolVersion},
            {QStringLiteral("error"),
             QJsonObject{
                 {QStringLiteral("code"), -32007},
                 {QStringLiteral("message"),
                  QStringLiteral("Shotcut MCP response exceeds the bridge size limit")},
                 {QStringLiteral("data"),
                  QJsonObject{{QStringLiteral("maximum_bytes"),
                               static_cast<double>(kMaximumMessageBytes)}}},
             }},
        };
        payload = QJsonDocument(versionedResponse).toJson(QJsonDocument::Compact);
    }
    socket->write(payload);
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
    if (method == QStringLiteral("editor.control"))
        return controlEditor(params);
    if (method == QStringLiteral("project.open"))
        return openProject(params);
    if (method == QStringLiteral("project.save"))
        return saveProject(params);
    if (method == QStringLiteral("project.set_profile"))
        return setProjectProfile(params);
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
        if (!metadata || !isBundledFilterMetadata(metadata) || metadata->isHidden()
            || metadata->isDeprecated() || metadata->isTrackOnly() || metadata->isOutputOnly()
            || !McpBridgePolicy::filterIdentitiesActiveAllowed(metadata->uniqueId(),
                                                               metadata->mlt_service())) {
            continue;
        }
        if (metadata->type() != QmlMetadata::Filter)
            continue;
        const QString id = metadata->uniqueId();
        if (id.isEmpty())
            continue;
        QJsonArray keyframeParameters;
        if (metadata->keyframes()) {
            for (int parameterIndex = 0;
                 parameterIndex < metadata->keyframes()->parameterCount();
                 ++parameterIndex) {
                const auto *parameter = metadata->keyframes()->parameter(parameterIndex);
                if (!parameter)
                    continue;
                keyframeParameters.append(QJsonObject{
                    {QStringLiteral("property"), parameter->property()},
                    {QStringLiteral("name"), parameter->name()},
                    {QStringLiteral("units"), parameter->units()},
                    {QStringLiteral("minimum"), parameter->minimum()},
                    {QStringLiteral("maximum"), parameter->maximum()},
                    {QStringLiteral("range_type"),
                     parameter->rangeType() == QmlKeyframesParameter::ClipLength
                         ? QStringLiteral("clip_length")
                         : QStringLiteral("min_max")},
                    {QStringLiteral("allow_overshoot"),
                     metadata->keyframes()->allowOvershoot()},
                    {QStringLiteral("numeric"), !parameter->isRectangle() && !parameter->isColor()},
                });
            }
        }
        filterCatalog.append(QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("name"), metadata->name()},
            {QStringLiteral("type"), QStringLiteral("filter")},
            {QStringLiteral("audio"), metadata->isAudio()},
            {QStringLiteral("clip_only"), metadata->isClipOnly()},
            {QStringLiteral("allow_multiple"), metadata->allowMultiple()},
            {QStringLiteral("needs_gpu"), metadata->needsGPU()},
            {QStringLiteral("gpu_compatible"), metadata->isGpuCompatible()},
            {QStringLiteral("keywords"), metadata->keywords()},
            {QStringLiteral("keyframe_parameters"), keyframeParameters},
        });
    }

    return QJsonObject{
        {QStringLiteral("bridge_protocol"), kBridgeProtocolVersion},
        {QStringLiteral("connected"), true},
        {QStringLiteral("open_connections"), static_cast<int>(m_buffers.size())},
        {QStringLiteral("project_path"), m_window.fileName()},
        {QStringLiteral("modified"), m_window.isWindowModified()},
        {QStringLiteral("revision"), static_cast<double>(m_revision)},
        {QStringLiteral("can_undo"), stack && stack->canUndo()},
        {QStringLiteral("can_redo"), stack && stack->canRedo()},
        {QStringLiteral("allowed_roots"), roots},
        {QStringLiteral("export_presets"), presets},
        {QStringLiteral("export_presets_policy_gated"), true},
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
            int filterSourceIn = 0;
            int filterSourceOut = -1;
            int playlistStart = 0;
            const bool hasFilterContext = filterClipContext(trackIndex,
                                                            clipIndex,
                                                            filterSourceIn,
                                                            filterSourceOut,
                                                            playlistStart);
            Q_UNUSED(playlistStart)
            if (producer.is_valid() && !producer.is_blank()) {
                AttachedFiltersModel attachedFilters;
                attachedFilters.setProducer(&producer);
                for (int filterIndex = 0; filterIndex < attachedFilters.rowCount(); ++filterIndex) {
                    QScopedPointer<Mlt::Service> service(attachedFilters.getService(filterIndex));
                    if (!service || !service->is_valid())
                        continue;
                    const QString actualService
                        = QString::fromUtf8(service->get("mlt_service")).trimmed();
                    const bool actualIsFilter = service->type() == mlt_service_filter_type;
                    const auto *metadata = attachedFilters.getMetadata(filterIndex);
                    QString id = actualService;
                    if (metadata && isBundledFilterMetadata(metadata)
                        && McpBridgePolicy::attachedFilterIdentityAllowed(
                            metadata->uniqueId(),
                            metadata->mlt_service(),
                            actualService,
                            actualIsFilter,
                            bundledMltServiceAllowed(QStringLiteral("filter"),
                                                     actualService,
                                                     metadata->uniqueId()))) {
                        id = metadata->uniqueId();
                    }
                    const QString serviceType = actualIsFilter ? QStringLiteral("filter")
                                                : service->type() == mlt_service_link_type
                                                    ? QStringLiteral("link")
                                                    : QStringLiteral("service");
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
                    QJsonArray keyframes;
                    QJsonObject keyframeRange;
                    bool hasAnyAdvancedKeyframes = false;
                    const int simpleAnimateIn = qMax(
                        0,
                        service->time_to_frames(service->get(kShotcutAnimInProperty)));
                    const int simpleAnimateOut = qMax(
                        0,
                        service->time_to_frames(service->get(kShotcutAnimOutProperty)));
                    if (metadata && actualIsFilter && isBundledFilterMetadata(metadata)
                        && metadata->keyframes()) {
                        int clipOffset = 0;
                        int keyframeDuration = 0;
                        if (hasFilterContext
                            && filterKeyframeTiming(producer,
                                                    *service,
                                                    filterSourceIn,
                                                    filterSourceOut,
                                                    clipOffset,
                                                    keyframeDuration)) {
                            keyframeRange = QJsonObject{
                                {QStringLiteral("start"), clipOffset},
                                {QStringLiteral("end"), clipOffset + keyframeDuration - 1},
                            };
                        }
                        for (int parameterIndex = 0;
                             parameterIndex < metadata->keyframes()->parameterCount();
                             ++parameterIndex) {
                            const auto *parameter
                                = metadata->keyframes()->parameter(parameterIndex);
                            if (!parameter)
                                continue;
                            const QString property = parameter->property();
                            if (keyframeDuration > 0
                                && !service->get_animation(property.toUtf8().constData())) {
                                service->anim_get_double(property.toUtf8().constData(),
                                                         0,
                                                         keyframeDuration);
                            }
                            Mlt::Animation animation(
                                service->get_animation(property.toUtf8().constData()));
                            if (animation.is_valid() && animation.key_count() > 0)
                                hasAnyAdvancedKeyframes = true;
                            if (parameter->isRectangle() || parameter->isColor())
                                continue;
                            if (!animation.is_valid() || animation.key_count() <= 0
                                || keyframeDuration <= 0) {
                                continue;
                            }
                            QJsonArray points;
                            for (int keyIndex = 0; keyIndex < animation.key_count(); ++keyIndex) {
                                const int frame = animation.key_get_frame(keyIndex);
                                points.append(QJsonObject{
                                    {QStringLiteral("position"), frame + clipOffset},
                                    {QStringLiteral("value"),
                                     service->anim_get_double(property.toUtf8().constData(),
                                                              frame,
                                                              keyframeDuration)},
                                    {QStringLiteral("interpolation"),
                                     McpKeyframeInterpolation::name(
                                         animation.key_get_type(keyIndex))},
                                });
                            }
                            double minimum = parameter->minimum();
                            double maximum = parameter->maximum();
                            if (parameter->rangeType() == QmlKeyframesParameter::ClipLength) {
                                const int filterIn = filterSourceIn + clipOffset;
                                minimum = 0.0;
                                maximum = static_cast<double>(
                                              qMax(0, producer.get_length() - filterIn))
                                          / MLT.profile().fps();
                            }
                            keyframes.append(QJsonObject{
                                {QStringLiteral("property"), property},
                                {QStringLiteral("minimum"), minimum},
                                {QStringLiteral("maximum"), maximum},
                                {QStringLiteral("units"), parameter->units()},
                                {QStringLiteral("allow_overshoot"),
                                 metadata->keyframes()->allowOvershoot()},
                                {QStringLiteral("points"), points},
                            });
                        }
                    }
                    QString keyframeMode = QStringLiteral("none");
                    if (simpleAnimateIn > 0 || simpleAnimateOut > 0)
                        keyframeMode = QStringLiteral("simple");
                    else if (hasAnyAdvancedKeyframes)
                        keyframeMode = QStringLiteral("advanced");
                    filters.append(QJsonObject{
                        {QStringLiteral("index"), filterIndex},
                        {QStringLiteral("id"), id},
                        {QStringLiteral("type"), serviceType},
                        {QStringLiteral("disabled"), service->get_int("disable") != 0},
                        {QStringLiteral("parameters"), parameters},
                        {QStringLiteral("keyframe_range"), keyframeRange},
                        {QStringLiteral("keyframe_mode"), keyframeMode},
                        {QStringLiteral("simple_keyframes"),
                         QJsonObject{{QStringLiteral("animate_in"), simpleAnimateIn},
                                     {QStringLiteral("animate_out"), simpleAnimateOut}}},
                        {QStringLiteral("keyframes"), keyframes},
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

    QJsonArray markers;
    const auto markerItems = timeline->markersModel()->getMarkers();
    for (int markerIndex = 0; markerIndex < markerItems.size(); ++markerIndex) {
        const auto &marker = markerItems.at(markerIndex);
        markers.append(QJsonObject{
            {QStringLiteral("index"), markerIndex},
            {QStringLiteral("text"), marker.text},
            {QStringLiteral("start"), marker.start},
            {QStringLiteral("end"), marker.end},
            {QStringLiteral("color"), marker.color.name(QColor::HexRgb)},
        });
    }

    QString dynamicRange = QStringLiteral("sdr");
    if (MLT.colorTrc() == QStringLiteral("arib-std-b67"))
        dynamicRange = QStringLiteral("hlg");
    else if (MLT.colorTrc() == QStringLiteral("smpte2084"))
        dynamicRange = QStringLiteral("pq");

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
                      {QStringLiteral("frame_rate_num"), MLT.profile().frame_rate_num()},
                      {QStringLiteral("frame_rate_den"), MLT.profile().frame_rate_den()},
                      {QStringLiteral("display_aspect_num"), MLT.profile().display_aspect_num()},
                      {QStringLiteral("display_aspect_den"), MLT.profile().display_aspect_den()},
                      {QStringLiteral("sample_aspect_num"), MLT.profile().sample_aspect_num()},
                      {QStringLiteral("sample_aspect_den"), MLT.profile().sample_aspect_den()},
                      {QStringLiteral("colorspace"),
                       profileColorspaceName(MLT.profile().colorspace())},
                      {QStringLiteral("colorspace_code"), MLT.profile().colorspace()},
                      {QStringLiteral("dynamic_range"), dynamicRange},
                      {QStringLiteral("progressive"), MLT.profile().progressive() != 0}}},
        {QStringLiteral("selection"), selection},
        {QStringLiteral("tracks"), tracks},
        {QStringLiteral("subtitle_tracks"), subtitleTracks},
        {QStringLiteral("markers"), markers},
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
            if (!LocalPath::equal(jobTarget, requestedTarget))
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

bool McpBridge::exportTargetInProgress(const QString &target) const
{
    return JOBS.targetIsInProgress(target);
}

bool McpBridge::isBundledFilterMetadata(const QmlMetadata *metadata) const
{
    if (!metadata || metadata->type() != QmlMetadata::Filter
        || metadata->objectName().startsWith(QStringLiteral("addOn."))) {
        return false;
    }

    QDir bundledRoot = QmlUtilities::qmlDir();
    if (!bundledRoot.cd(QStringLiteral("filters")))
        return false;
    return pathWithinRoot(metadata->path().absolutePath(), bundledRoot.absolutePath());
}

bool McpBridge::bundledMltServiceAllowed(const QString &element,
                                         const QString &service,
                                         const QString &filterId) const
{
    if (element.compare(QStringLiteral("transition"), Qt::CaseInsensitive) == 0) {
        return filterId.isEmpty() && McpBridgePolicy::coreTransitionServiceAllowed(service);
    }
    if (element.compare(QStringLiteral("filter"), Qt::CaseInsensitive) != 0)
        return false;

    auto *metadataModel = m_window.filterController()->metadataModel();
    for (int index = 0; index < metadataModel->sourceRowCount(); ++index) {
        const auto *metadata = metadataModel->getFromSource(index);
        if (!metadata || metadata->type() != QmlMetadata::Filter
            || !isBundledFilterMetadata(metadata)
            || metadata->mlt_service().compare(service, Qt::CaseInsensitive) != 0) {
            continue;
        }
        if (filterId.isEmpty() || metadata->uniqueId().compare(filterId, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

bool McpBridge::filterClipContext(int track,
                                  int clip,
                                  int &sourceIn,
                                  int &sourceOut,
                                  int &playlistStart) const
{
    auto *model = m_window.timelineDock()->model();
    const auto info = model->getClipInfo(track, clip);
    if (!info || !info->producer || !info->producer->is_valid())
        return false;

    sourceIn = info->frame_in;
    sourceOut = info->frame_out;
    playlistStart = info->start;
    const auto previous = model->getClipInfo(track, clip - 1);
    if (previous && previous->producer && previous->producer->is_valid()
        && previous->producer->get(kShotcutTransitionProperty)) {
        sourceIn -= previous->frame_count;
        playlistStart = previous->start;
    }
    const auto next = model->getClipInfo(track, clip + 1);
    if (next && next->producer && next->producer->is_valid()
        && next->producer->get(kShotcutTransitionProperty)) {
        sourceOut += next->frame_count;
    }
    return sourceOut >= sourceIn;
}

bool McpBridge::filterKeyframeTiming(Mlt::Producer &producer,
                                     Mlt::Service &service,
                                     int sourceIn,
                                     int sourceOut,
                                     int &clipOffset,
                                     int &duration) const
{
    if (!producer.is_valid() || !service.is_valid() || sourceOut < sourceIn)
        return false;

    int filterIn = service.get_int("in");
    int filterOut = service.get_int("out");
    if (service.type() == mlt_service_link_type || (filterIn == 0 && filterOut == 0)) {
        filterIn = sourceIn;
        filterOut = sourceOut;
    }
    if (filterIn < 0 || filterOut < filterIn)
        return false;

    clipOffset = filterIn - sourceIn;
    duration = filterOut - filterIn + 1;
    return duration > 0;
}

bool McpBridge::resolveEditableAttachedFilter(const AttachedFiltersModel &attachedFilters,
                                              int filterIndex,
                                              QString &filterId,
                                              QString &filterService,
                                              QString &error) const
{
    if (filterIndex < 0 || filterIndex >= attachedFilters.rowCount()) {
        error = QStringLiteral("filter_index does not exist");
        return false;
    }

    const auto *metadata = attachedFilters.getMetadata(filterIndex);
    QScopedPointer<Mlt::Service> actualFilter(attachedFilters.getService(filterIndex));
    if (!metadata || !isBundledFilterMetadata(metadata) || metadata->type() != QmlMetadata::Filter
        || !actualFilter || !actualFilter->is_valid()) {
        error = QStringLiteral("filter_index does not identify an editable bundled filter");
        return false;
    }

    const QString id = metadata->uniqueId();
    const QString actualService = QString::fromUtf8(actualFilter->get("mlt_service")).trimmed();
    const bool actualIsFilter = actualFilter->type() == mlt_service_filter_type;
    const bool bundledPair = bundledMltServiceAllowed(QStringLiteral("filter"), actualService, id);
    if (!McpBridgePolicy::attachedFilterIdentityAllowed(id,
                                                        metadata->mlt_service(),
                                                        actualService,
                                                        actualIsFilter,
                                                        bundledPair)) {
        error = QStringLiteral("filter_index does not identify an editable bundled filter");
        return false;
    }

    const QString nestedService = QString::fromUtf8(actualFilter->get("filter"));
    if (!McpBridgePolicy::nestedFilterParameterWriteAllowed(actualService,
                                                            QStringLiteral("filter"),
                                                            nestedService,
                                                            false)) {
        error = QStringLiteral("filter_index uses an active or unknown nested mask filter");
        return false;
    }
    const QString transitionService = QString::fromUtf8(actualFilter->get("transition"));
    if (!McpBridgePolicy::maskApplyTransitionServiceAllowed(actualService, transitionService)) {
        error = QStringLiteral("filter_index uses an unsafe Mask: Apply transition service");
        return false;
    }
    const QString affineBackground = QString::fromUtf8(actualFilter->get("background"));
    if (!McpBridgePolicy::affineBackgroundServiceAllowed(actualService, affineBackground)) {
        error = QStringLiteral("filter_index uses an unsafe affine background producer");
        return false;
    }
    const QString dustFactory = QString::fromUtf8(actualFilter->get("factory"));
    if (!McpBridgePolicy::dustFactoryServiceAllowed(actualService, dustFactory)) {
        error = QStringLiteral("filter_index uses an explicit Dust producer factory");
        return false;
    }

    filterId = id;
    filterService = actualService;
    return true;
}

QmlMetadata *McpBridge::editableClipFilterMetadata(const QString &filterId) const
{
    auto *metadata = m_window.filterController()->metadata(filterId);
    if (!metadata || !isBundledFilterMetadata(metadata))
        return nullptr;
    if (metadata->type() != QmlMetadata::Filter || metadata->uniqueId().isEmpty()
        || metadata->isHidden() || metadata->isDeprecated() || metadata->isTrackOnly()
        || metadata->isOutputOnly()
        || !McpBridgePolicy::filterIdentitiesActiveAllowed(metadata->uniqueId(),
                                                           metadata->mlt_service())) {
        return nullptr;
    }
    return metadata;
}

bool McpBridge::validateClipFilterAddition(QmlMetadata *metadata,
                                           Mlt::Producer &producer,
                                           QString &error) const
{
    if (!metadata || !producer.is_valid() || producer.is_blank()) {
        error = QStringLiteral("clip filter or target producer is unavailable");
        return false;
    }
    if (!isBundledFilterMetadata(metadata)) {
        error = QStringLiteral("extension and generated filters cannot be added through MCP");
        return false;
    }
    if (!McpBridgePolicy::filterIdentitiesActiveAllowed(metadata->uniqueId(),
                                                        metadata->mlt_service())) {
        error = QStringLiteral("active-content filters cannot be added through MCP");
        return false;
    }

    const auto applicability
        = McpBridgePolicy::clipFilterApplicability(Settings.playerGPU(),
                                                   metadata->needsGPU(),
                                                   metadata->isGpuCompatible(),
                                                   metadata->seekReverse(),
                                                   QString::fromUtf8(producer.get("mlt_service")));
    switch (applicability) {
    case McpBridgePolicy::FilterApplicability::RequiresGpu:
        error = QStringLiteral("filter requires GPU processing, but Shotcut is using CPU "
                               "processing");
        return false;
    case McpBridgePolicy::FilterApplicability::GpuIncompatible:
        error = QStringLiteral("filter is incompatible with Shotcut GPU processing");
        return false;
    case McpBridgePolicy::FilterApplicability::ReverseUnsupported:
        error = QStringLiteral("filter requires reverse seeking, which is not supported for "
                               "xml-clip producers");
        return false;
    case McpBridgePolicy::FilterApplicability::Allowed:
        break;
    }

    AttachedFiltersModel attachedFilters;
    attachedFilters.setProducer(&producer);
    if (!metadata->allowMultiple()) {
        for (int row = 0; row < attachedFilters.rowCount(); ++row) {
            const auto *attachedMetadata = attachedFilters.getMetadata(row);
            if (attachedMetadata && attachedMetadata->uniqueId() == metadata->uniqueId()) {
                error = QStringLiteral("filter is already attached and does not allow multiple "
                                       "instances");
                return false;
            }
        }
    }
    return true;
}

bool McpBridge::normalizeFilterPathParameter(const QString &filterId,
                                             const QString &service,
                                             const QString &name,
                                             const QJsonValue &value,
                                             QString *normalized,
                                             QString &error) const
{
    if (!McpBridgePolicy::filterParameterNameAllowed(name)) {
        error = QStringLiteral("filter parameter '%1' is reserved or invalid").arg(name);
        return false;
    }
    if (value.isString() && !McpBridgePolicy::cStringValueAllowed(value.toString())) {
        error = QStringLiteral("filter parameter '%1' contains an embedded null").arg(name);
        return false;
    }
    if (!McpBridgePolicy::filterIdentitiesActiveAllowed(filterId, service)) {
        error = QStringLiteral("active-content filter parameters cannot be changed through MCP");
        return false;
    }
    if (!McpBridgePolicy::nestedFilterPathParameterWriteAllowed(service, name, value.isNull())) {
        error = QStringLiteral("nested mask filter path parameters can only be reset through MCP");
        return false;
    }
    const bool maskApplyTransition
        = service.trimmed().compare(QStringLiteral("mask_apply"), Qt::CaseInsensitive) == 0
          && name.trimmed().compare(QStringLiteral("transition"), Qt::CaseInsensitive) == 0;
    if (maskApplyTransition) {
        QString rawValue;
        bool reset = false;
        if (!McpBridgePolicy::filterStringOrResetValue(value, &rawValue, &reset)
            || !McpBridgePolicy::maskApplyTransitionParameterWriteAllowed(service,
                                                                          name,
                                                                          rawValue,
                                                                          reset)) {
            error = QStringLiteral("Mask: Apply transition must be empty or exactly qtblend");
            return false;
        }
    }
    if (!McpBridgePolicy::nestedTransitionPathParameterWriteAllowed(service, name, value.isNull())) {
        error = QStringLiteral("nested Mask: Apply transition paths can only be reset through MCP");
        return false;
    }
    const bool affineBackground
        = service.trimmed().compare(QStringLiteral("affine"), Qt::CaseInsensitive) == 0
          && name.trimmed().compare(QStringLiteral("background"), Qt::CaseInsensitive) == 0;
    if (affineBackground) {
        QString rawValue;
        bool reset = false;
        if (!McpBridgePolicy::filterStringOrResetValue(value, &rawValue, &reset)
            || !McpBridgePolicy::affineBackgroundParameterWriteAllowed(service,
                                                                       name,
                                                                       rawValue,
                                                                       reset)) {
            error = QStringLiteral("affine background must be an exact built-in color producer");
            return false;
        }
    }
    if (!McpBridgePolicy::nestedProducerPathParameterWriteAllowed(service, name, value.isNull())) {
        error = QStringLiteral("nested affine producer paths can only be reset through MCP");
        return false;
    }
    if (!McpBridgePolicy::dustFactoryParameterWriteAllowed(service, name, value.isNull())) {
        error = QStringLiteral("the Dust producer factory can only be reset through MCP");
        return false;
    }
    if (McpBridgePolicy::isTransitionProducerFactoryParameter(service, name)) {
        QString rawValue;
        bool reset = false;
        if (!McpBridgePolicy::filterStringOrResetValue(value, &rawValue, &reset)
            || !McpBridgePolicy::transitionProducerFactoryParameterWriteAllowed(service,
                                                                                name,
                                                                                rawValue,
                                                                                reset)) {
            error = QStringLiteral("transition producer factory must be empty or exactly 'loader'");
            return false;
        }
    }
    if (!value.isNull() && !McpBridgePolicy::avFilterPropertyAllowed(service, name)) {
        error = QStringLiteral("filter parameter '%1' is not an approved option for '%2'")
                    .arg(name, filterId);
        return false;
    }
    if (!McpBridgePolicy::richTextParameterWriteAllowed(filterId, name, value.isNull())
        || !McpBridgePolicy::richTextParameterWriteAllowed(service, name, value.isNull())) {
        error = QStringLiteral("filter parameter '%1' for '%2' cannot be written through MCP; "
                               "use plain text and styling, or reset it to null")
                    .arg(name, filterId);
        return false;
    }

    if (service.trimmed().compare(QStringLiteral("mask_start"), Qt::CaseInsensitive) == 0
        && name.trimmed().compare(QStringLiteral("filter"), Qt::CaseInsensitive) == 0) {
        bool reset = false;
        QString nestedService;
        if (!McpBridgePolicy::maskStartSelectorValue(value, &nestedService, &reset)
            || !McpBridgePolicy::nestedFilterParameterWriteAllowed(service,
                                                                   name,
                                                                   nestedService,
                                                                   reset)) {
            error = QStringLiteral("mask filter selector must be reset or an exact safe token");
            return false;
        }
    }

    const auto kind = McpBridgePolicy::filterPathKindForIdentities(filterId, service, name);
    if (kind == McpBridgePolicy::FilterPathKind::NotPath) {
        if (normalized && value.isString())
            *normalized = value.toString();
        return true;
    }
    if (value.isNull())
        return true;
    if (!value.isString() || value.toString().isEmpty()) {
        error = QStringLiteral("filter parameter '%1' for '%2' must be an absolute filesystem "
                               "path or null")
                    .arg(name, filterId);
        return false;
    }
    if (McpBridgePolicy::isBuiltInValue(filterId, name, value.toString())) {
        if (normalized)
            *normalized = value.toString();
        return true;
    }

    const bool mustExist = kind == McpBridgePolicy::FilterPathKind::ExistingFile;
    const bool lutFile = McpBridgePolicy::isLutFileParameter(filterId, name)
                         || McpBridgePolicy::isLutFileParameter(service, name);
    if (lutFile && QFileInfo(value.toString()).isSymLink()) {
        error = QStringLiteral("LUT filter files cannot be symbolic links");
        return false;
    }
    QString safePath;
    if (!pathAllowed(value.toString(), mustExist, &safePath)) {
        error = mustExist
                    ? QStringLiteral("filter parameter '%1' for '%2' must reference an existing "
                                     "absolute file inside allowed roots")
                          .arg(name, filterId)
                    : QStringLiteral("filter parameter '%1' for '%2' must be an absolute path "
                                     "with an existing parent inside allowed roots")
                          .arg(name, filterId);
        return false;
    }

    const QFileInfo info(safePath);
    if ((mustExist && !info.isFile()) || (info.exists() && !info.isFile())) {
        error = QStringLiteral("filter parameter '%1' for '%2' must reference a file, not a "
                               "directory")
                    .arg(name, filterId);
        return false;
    }
    if (!McpBridgePolicy::filterInputSuffixAllowed(filterId, name, info.suffix())
        || !McpBridgePolicy::filterInputSuffixAllowed(service, name, info.suffix())) {
        error = QStringLiteral("LUT filter files must use a supported LUT extension");
        return false;
    }
    if (normalized)
        *normalized = safePath;
    return true;
}

bool McpBridge::validateProjectResourcePaths(const QString &path, QString &error) const
{
    const McpXmlPathValidator validator(
        [this](const QString &candidate, bool mustExist, QString *normalized) {
            return pathAllowed(candidate, mustExist, normalized);
        },
        [this](const QString &element, const QString &service, const QString &filterId) {
            return bundledMltServiceAllowed(element, service, filterId);
        });
    return validator.validateProject(path, &error);
}

bool McpBridge::validateMediaResourcePaths(const QString &path, QString &error) const
{
    const McpXmlPathValidator validator(
        [this](const QString &candidate, bool mustExist, QString *normalized) {
            return pathAllowed(candidate, mustExist, normalized);
        },
        [this](const QString &element, const QString &service, const QString &filterId) {
            return bundledMltServiceAllowed(element, service, filterId);
        });
    return validator.validateMedia(path, &error);
}
void McpBridge::loadAllowedRoots()
{
    m_allowedRoots.clear();
    auto configured = qEnvironmentVariable("SHOTCUT_MCP_ALLOWED_ROOTS")
                          .split(QDir::listSeparator(), Qt::SkipEmptyParts);

    for (const auto &root : configured) {
        QFileInfo info(root);
        QString normalized = QDir::fromNativeSeparators(info.canonicalFilePath());
        if (!normalized.isEmpty() && !m_allowedRoots.contains(normalized))
            m_allowedRoots.append(normalized);
    }
}

QString McpBridge::normalizedPathForPolicy(const QString &path, bool mustExist) const
{
    if (!McpBridgePolicy::cStringValueAllowed(path))
        return QString();
    if (QDir::fromNativeSeparators(path).startsWith(QStringLiteral("//")))
        return QString();
    QFileInfo info(path);
    if (!info.isAbsolute() || (mustExist && !info.exists()))
        return QString();
    if (info.isSymLink() && !info.exists())
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
