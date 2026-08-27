// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QString>

#include <vector>

namespace slonisko::sql {

// A statement's span in the UTF-8 source, in bytes.
struct StatementSpan
{
    qsizetype offset = 0;
    qsizetype length = 0;
};

struct SplitResult
{
    std::vector<StatementSpan> statements;
    QString error; // Empty on success.
};

// Splits a script into statements using PostgreSQL's own scanner, so it
// handles dollar quoting, nested comments and DO blocks. Unlike the parser
// splitter, it works on scripts that do not parse.
SplitResult splitStatements(const QByteArray &utf8Script);

} // namespace slonisko::sql
