/*
 * Copyright (c) 2026 Meltytech, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef EXPORTTEMPORARYFILEUTILS_H
#define EXPORTTEMPORARYFILEUTILS_H

#include <QTemporaryFile>

namespace ExportTemporaryFileUtils {
inline bool ensureOpen(QTemporaryFile *temporaryFile)
{
    return temporaryFile && (temporaryFile->isOpen() || temporaryFile->open());
}
} // namespace ExportTemporaryFileUtils

#endif // EXPORTTEMPORARYFILEUTILS_H
