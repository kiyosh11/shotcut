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

class MainWindow;
class QLocalSocket;

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
    bool applyFilterParameters(int track,
                               int clip,
                               int filterIndex,
                               const QJsonObject &parameters,
                               QString &error);
    bool checkRevision(const QJsonObject &params, QString &error) const;
    bool trackExists(int track) const;
    bool clipExists(int track, int clip) const;
    bool pathAllowed(const QString &path, bool mustExist, QString *normalized = nullptr) const;
    QString normalizedPathForPolicy(const QString &path, bool mustExist) const;
    void loadAllowedRoots();

    MainWindow &m_window;
    QLocalServer m_server;
    QHash<QLocalSocket *, QByteArray> m_buffers;
    QByteArray m_token;
    QString m_endpoint;
    QStringList m_allowedRoots;
};

#endif // MCPBRIDGE_H
