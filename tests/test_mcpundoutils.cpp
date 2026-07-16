/*
 * Copyright (c) 2026 Meltytech, LLC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "mcpundoutils.h"

#include <QTest>

namespace {
class IncrementCommand : public QUndoCommand
{
public:
    IncrementCommand(int &value, int amount)
        : m_value(value)
        , m_amount(amount)
    {}

    void redo() override { m_value += m_amount; }
    void undo() override { m_value -= m_amount; }

private:
    int &m_value;
    int m_amount;
};
} // namespace

class TestMcpUndoUtils : public QObject
{
    Q_OBJECT

private slots:
    void rollbackRemovesFailedMacro();
};

void TestMcpUndoUtils::rollbackRemovesFailedMacro()
{
    int value = 0;
    QUndoStack stack;
    stack.setClean();
    const auto before = McpUndo::capture(stack);

    stack.beginMacro(QStringLiteral("failed MCP plan"));
    stack.push(new IncrementCommand(value, 2));
    stack.push(new IncrementCommand(value, 3));
    stack.endMacro();

    QCOMPARE(value, 5);
    QVERIFY(McpUndo::rollbackLatestMacro(stack, before));
    QCOMPARE(value, 0);
    QCOMPARE(stack.index(), before.index);
    QCOMPARE(stack.count(), before.count);
    QVERIFY(stack.isClean());
    QVERIFY(!stack.canRedo());
}

QTEST_GUILESS_MAIN(TestMcpUndoUtils)

#include "test_mcpundoutils.moc"
