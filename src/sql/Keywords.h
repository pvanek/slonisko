// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QByteArrayView>

#include <optional>
#include <vector>

namespace slonisko::sql {

// PostgreSQL's keywords, read from the parser in the linked libpg_query, so
// they always match it. See the warning in Keywords.cpp.

// PostgreSQL's keyword categories (see "SQL Key Words" in its manual).
enum class KeywordCategory {
    Unreserved, // Usable as any name, e.g. "name", "comment".
    ColumnName, // Usable as a column name, not as a function or type.
    TypeFunctionName, // Usable as a function or type name.
    Reserved, // Needs quoting to be used as a name.
};

// The category of a keyword, case-insensitively, or nullopt for other words.
std::optional<KeywordCategory> keywordCategory(QByteArrayView word);

// Built-in type names, like text, int8 or jsonb, including those that are
// also keywords, like integer or varchar.
bool isBuiltinTypeName(QByteArrayView word);

// All keywords, lowercase and sorted.
std::vector<QByteArray> keywords();

// Whether a name must be double-quoted to be used as an identifier: it is not
// a lowercase letter or _ followed by lowercase letters, digits, _ and $, or
// it is a reserved-ish keyword.
bool needsQuoting(QByteArrayView identifier);

} // namespace slonisko::sql
