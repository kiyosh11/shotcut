/*
 * Copyright (c) 2026 Meltytech, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef MCPUNDOUTILS_H
#define MCPUNDOUTILS_H

#include <QUndoCommand>
#include <QUndoStack>

namespace McpUndo {

struct UndoState
{
    int index;
    int count;
    bool clean;
};

inline UndoState capture(const QUndoStack &stack)
{
    return UndoState{stack.index(), stack.count(), stack.isClean()};
}

inline bool rollbackLatestMacro(QUndoStack &stack, const UndoState &before)
{
    if (stack.index() > before.index && stack.count() > before.count) {
        auto *failedMacro = const_cast<QUndoCommand *>(stack.command(stack.index() - 1));
        if (!failedMacro)
            return false;

        // QUndoStack checks isObsolete() after undoing the command. This both
        // reverts the macro and removes it instead of leaving it in redo history.
        failedMacro->setObsolete(true);
        stack.undo();
    }

    const bool restored = stack.index() == before.index && stack.count() == before.count;
    if (restored && before.clean)
        stack.setClean();
    return restored;
}

} // namespace McpUndo

#endif // MCPUNDOUTILS_H
