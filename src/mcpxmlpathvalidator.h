/*
 * Copyright (c) 2026 Meltytech, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef MCPXMLPATHVALIDATOR_H
#define MCPXMLPATHVALIDATOR_H

#include <QSet>
#include <QString>

#include <functional>

class McpXmlPathValidator
{
public:
    using PathAuthorizer = std::function<bool(const QString &, bool, QString *)>;
    using ServiceAuthorizer = std::function<bool(const QString &, const QString &, const QString &)>;

    explicit McpXmlPathValidator(PathAuthorizer authorizePath,
                                 ServiceAuthorizer authorizeService = {});

    bool validateProject(const QString &fileName, QString *errorMessage = nullptr) const;
    bool validateMedia(const QString &fileName, QString *errorMessage = nullptr) const;

private:
    struct ValidationState;

    bool validateProjectInternal(const QString &fileName,
                                 ValidationState &state,
                                 int depth,
                                 QString *errorMessage) const;
    bool validateNormalizedMedia(const QString &fileName,
                                 bool xmlIsMltProject,
                                 ValidationState &state,
                                 int depth,
                                 QString *errorMessage) const;
    bool authorizeLocalPath(const QString &path,
                            bool mustExist,
                            QString *normalized,
                            QString *errorMessage) const;

    PathAuthorizer m_authorizePath;
    ServiceAuthorizer m_authorizeService;
};

#endif // MCPXMLPATHVALIDATOR_H