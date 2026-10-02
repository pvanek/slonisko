// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sql/Keywords.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

// ============================================================================
// WARNING: PostgreSQL internals, not libpg_query's public API.
//
// The keyword list is the one compiled into libpg_query, so it always matches
// the parser we link (PostgreSQL's src/common/keywords.c and kwlookup.c).
// libpg_query does not install the headers that declare it, so the
// declarations below are copied from PostgreSQL's src/include/common/
// kwlookup.h and keywords.h. They must match the linked library exactly:
//
//  - When upgrading libpg_query, compare ScanKeywordList below with its
//    src/postgres/include/common/kwlookup.h (unchanged since PostgreSQL 12)
//    and the *_KEYWORD values with common/keywords.h. A mismatch is not
//    caught by the compiler; tst_lexer's keywords() test would fail, or the
//    program would crash.
//  - The symbols must be linkable: they are in the static library and in a
//    Linux shared one, but a Windows DLL build of libpg_query would not
//    export them (link it statically there, as the bundled build does).
// ============================================================================
extern "C" {

using ScanKeywordHashFunc = int (*)(const void *key, std::size_t keylen);

struct ScanKeywordList
{
    const char *kw_string; // All keywords in order, separated by NULs.
    const std::uint16_t *kw_offsets; // Offsets to the start of each keyword.
    ScanKeywordHashFunc hash; // Perfect hash function for keywords.
    int num_keywords;
    int max_kw_len;
};

extern const ScanKeywordList ScanKeywords;
extern const std::uint8_t ScanKeywordCategories[];

// The keyword's index in ScanKeywords, or -1. ASCII case-insensitive; str
// must be NUL-terminated.
int ScanKeywordLookup(const char *str, const ScanKeywordList *keywords);

} // extern "C"

namespace slonisko::sql {

namespace {

// PostgreSQL's UNRESERVED_KEYWORD, COL_NAME_KEYWORD, TYPE_FUNC_NAME_KEYWORD
// and RESERVED_KEYWORD, which KeywordCategory mirrors in order.
static_assert(int(KeywordCategory::Unreserved) == 0 && int(KeywordCategory::ColumnName) == 1
              && int(KeywordCategory::TypeFunctionName) == 2
              && int(KeywordCategory::Reserved) == 3);

// Built-in type names, keywords among them, like text or integer. Keep sorted.
constexpr const char *TypeNames[] = {
    "anyarray",      "anyelement", "bigint",       "bigserial", "bit",          "bool",
    "boolean",       "box",        "bytea",        "char",      "character",    "cidr",
    "circle",        "cstring",    "date",         "daterange", "decimal",      "double",
    "event_trigger", "float",      "float4",       "float8",    "inet",         "int",
    "int2",          "int4",       "int4range",    "int8",      "int8range",    "integer",
    "interval",      "json",       "jsonb",        "jsonpath",  "line",         "lseg",
    "macaddr",       "macaddr8",   "money",        "name",      "numeric",      "numrange",
    "oid",           "path",       "pg_lsn",       "point",     "polygon",      "real",
    "record",        "regclass",   "regnamespace", "regproc",   "regprocedure", "regrole",
    "regtype",       "serial",     "serial2",      "serial4",   "serial8",      "smallint",
    "smallserial",   "text",       "time",         "timestamp", "timestamptz",  "timetz",
    "trigger",       "tsquery",    "tsrange",      "tstzrange", "tsvector",     "txid_snapshot",
    "uuid",          "varbit",     "varchar",      "void",      "xid",          "xid8",
    "xml",
};

// Copies word into a NUL-terminated, ASCII-lowercased buffer. False if it
// does not fit, and so is longer than any keyword.
bool lowered(QByteArrayView word, char *out, std::size_t size)
{
    if (word.size() <= 0 || std::size_t(word.size()) >= size)
        return false;
    for (qsizetype i = 0; i < word.size(); ++i) {
        const char c = word[i];
        out[i] = (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
    }
    out[word.size()] = '\0';
    return true;
}

} // namespace

std::optional<KeywordCategory> keywordCategory(QByteArrayView word)
{
    char buffer[64];
    if (!lowered(word, buffer, sizeof buffer))
        return std::nullopt;
    const int index = ScanKeywordLookup(buffer, &ScanKeywords);
    if (index < 0)
        return std::nullopt;
    return KeywordCategory(ScanKeywordCategories[index]);
}

bool isBuiltinTypeName(QByteArrayView word)
{
    char buffer[64];
    if (!lowered(word, buffer, sizeof buffer))
        return false;
    return std::binary_search(std::begin(TypeNames), std::end(TypeNames), buffer,
                              [](const char *a, const char *b) { return std::strcmp(a, b) < 0; });
}

std::vector<QByteArray> keywords()
{
    std::vector<QByteArray> out;
    out.reserve(std::size_t(ScanKeywords.num_keywords));
    for (int i = 0; i < ScanKeywords.num_keywords; ++i)
        out.emplace_back(ScanKeywords.kw_string + ScanKeywords.kw_offsets[i]);
    return out;
}

bool needsQuoting(QByteArrayView identifier)
{
    if (identifier.isEmpty())
        return true;
    const char first = identifier[0];
    if (!((first >= 'a' && first <= 'z') || first == '_'))
        return true;
    for (const char c : identifier) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '$'))
            return true;
    }
    const auto category = keywordCategory(identifier);
    return category && *category != KeywordCategory::Unreserved;
}

} // namespace slonisko::sql
