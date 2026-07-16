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
    auto finish = [&]() {
        const bool restored = stack.index() == before.index && stack.count() == before.count;
        if (restored && before.clean)
            stack.setClean();
        return restored;
    };

    if (stack.index() == before.index && stack.count() == before.count)
        return finish();
    if (stack.index() != before.index + 1 || stack.count() != before.count + 1)
        return false;

    // Undo while the macro is still active. QUndoStack deliberately skips undo()
    // for commands that have already been marked obsolete.
    stack.undo();
    if (stack.index() != before.index)
        return false;
    if (stack.count() == before.count)
        return finish();
    if (stack.count() != before.count + 1)
        return false;

    // The failed macro is now the next redo command. Marking it obsolete before
    // redo makes QUndoStack remove it without calling redo(), so no redo history
    // from the partially applied plan remains.
    auto *failedMacro = const_cast<QUndoCommand *>(stack.command(stack.index()));
    if (!failedMacro)
        return false;
    failedMacro->setObsolete(true);
    stack.redo();
    return finish();
}

} // namespace McpUndo

#endif // MCPUNDOUTILS_H
