/*
 * Copyright (c) 2026 Meltytech, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef MCPBRIDGE_H
#define MCPBRIDGE_H

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocalServer>
#include <QObject>
#include <QStringList>

#include <memory>

namespace Mlt {
class Producer;
}
class AttachedFiltersModel;
class MainWindow;
class QLocalSocket;
class QLockFile;
class QmlMetadata;

class McpBridge : public QObject
{
    Q_OBJECT

public:
    explicit McpBridge(MainWindow &window, QObject *parent = nullptr);
    ~McpBridge() override;

    bool startFromEnvironment();

private:
    struct RpcResult
    {
        bool ok{false};
        QJsonValue value;
        int code{-32603};
        QString message;
        QJsonValue data;

        static RpcResult success(const QJsonValue &value = QJsonObject());
        static RpcResult failure(int code,
                                 const QString &message,
                                 const QJsonValue &data = QJsonValue());
    };

    void onNewConnection();
    void onReadyRead(QLocalSocket *socket);
    void writeResponse(QLocalSocket *socket, const QJsonObject &response);
    RpcResult dispatch(const QString &method, QJsonObject params);

    QJsonObject editorStatus() const;
    QJsonObject projectSnapshot() const;
    QJsonArray exportJobs(const QString &target = QString()) const;
    bool exportTargetInProgress(const QString &target) const;

    RpcResult openProject(const QJsonObject &params);
    RpcResult saveProject(const QJsonObject &params);
    RpcResult applyEditPlan(const QJsonObject &params);
    RpcResult changeHistory(const QJsonObject &params, bool redo);
    RpcResult startExport(const QJsonObject &params);
    RpcResult exportStatus(const QJsonObject &params) const;

    bool validateOperation(const QJsonObject &operation, QString &error) const;
    bool applyOperation(const QJsonObject &operation, QString &error);
    bool applyTimelineOperation(const QJsonObject &operation, QString &error);
    bool applyFilterOperation(const QJsonObject &operation, QString &error);
    bool applySubtitleOperation(const QJsonObject &operation, QString &error);
    bool applyFilterParameters(
        int track, int clip, int filterIndex, const QJsonObject &parameters, QString &error);
    bool checkRevision(const QJsonObject &params, QString &error) const;
    bool trackExists(int track) const;
    bool clipExists(int track, int clip) const;
    bool isBundledFilterMetadata(const QmlMetadata *metadata) const;
    bool bundledMltServiceAllowed(const QString &element,
                                  const QString &service,
                                  const QString &filterId) const;
    bool resolveEditableAttachedFilter(const AttachedFiltersModel &attachedFilters,
                                       int filterIndex,
                                       QString &filterId,
                                       QString &filterService,
                                       QString &error) const;
    QmlMetadata *editableClipFilterMetadata(const QString &filterId) const;
    bool validateClipFilterAddition(QmlMetadata *metadata,
                                    Mlt::Producer &producer,
                                    QString &error) const;
    bool normalizeFilterPathParameter(const QString &filterId,
                                      const QString &service,
                                      const QString &name,
                                      const QJsonValue &value,
                                      QString *normalized,
                                      QString &error) const;
    bool pathAllowed(const QString &path, bool mustExist, QString *normalized = nullptr) const;
    QString normalizedPathForPolicy(const QString &path, bool mustExist) const;
    bool validateProjectResourcePaths(const QString &path, QString &error) const;
    bool validateMediaResourcePaths(const QString &path, QString &error) const;
    void loadAllowedRoots();
    void advanceRevision();

    MainWindow &m_window;
    QLocalServer m_server;
    std::unique_ptr<QLockFile> m_endpointLock;
    QHash<QLocalSocket *, QByteArray> m_buffers;
    QByteArray m_token;
    QString m_endpoint;
    QStringList m_allowedRoots;
    qint64 m_revision{1};
    bool m_busy{false};
    bool m_ownsEndpoint{false};
};

#endif // MCPBRIDGE_H
