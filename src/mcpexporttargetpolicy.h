/*
 * Copyright (c) 2026 Meltytech, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef MCPEXPORTTARGETPOLICY_H
#define MCPEXPORTTARGETPOLICY_H

#include <QMap>
#include <QString>

#include <functional>

namespace McpExportTargetPolicy {
struct EnumerationLimits
{
    int maximumDirectoryEntries{250000};
    int maximumSequenceMembers{100000};
};

using ConsumerProperties = QMap<QString, QString>;
using PathAuthorizer = std::function<bool(const QString &path, bool mustExist)>;

// Validates the exact avformat consumer attributes that will be serialized into the export job.
bool validateConsumerProperties(const QString &consumerTarget,
                                bool imageSequence,
                                int pass,
                                const ConsumerProperties &properties,
                                const PathAuthorizer &pathAuthorizer,
                                QString *errorMessage = nullptr);

// Validates the requested output path. Image sequences are checked by streaming the destination
// directory so every existing member is covered without an unbounded allocation.
bool validateConsumerTarget(const QString &requestedTarget,
                            const QString &consumerTarget,
                            bool imageSequence,
                            bool overwrite,
                            const PathAuthorizer &pathAuthorizer,
                            QString *errorMessage = nullptr,
                            EnumerationLimits limits = EnumerationLimits());
} // namespace McpExportTargetPolicy

#endif // MCPEXPORTTARGETPOLICY_H
