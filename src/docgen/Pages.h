// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "catalog/Erd.h"
#include "docgen/Document.h"
#include "docgen/Model.h"

namespace slonisko::docgen {

struct PageOptions
{
    QString title; // Empty: the database's name.
    QString description; // Text for the home page, paragraphs apart by blank lines.
    QString generator; // Named at the foot of every page.
    QString generatedAt;
    bool definitions = true; // SQL definitions of tables, views, routines.
    bool diagrams = true;
    bool symbolIndex = true;
    // Partitions get pages of their own; otherwise they are only listed on
    // their parent's.
    bool partitions = false;
};

// The pages for a model: a home page, one per schema, one per table, view,
// routine and type, the sequences of a schema together and an index of
// every name. What is worth fixing is the quality report's business.
Site buildSite(const Model &model, const PageOptions &options);

// The diagram of a table and its neighbours, or of a whole schema with the
// tables its keys point to elsewhere.
catalog::ErdGraph tableGraph(const Model &model, Oid table);
catalog::ErdGraph schemaGraph(const Model &model, const QString &schema);

// "1.5 MB", as the pages give sizes.
QString formatSize(qint64 bytes);

// CREATE TABLE and the rest that make a table, put together from the
// catalog; pg_dump's is the authoritative one.
QString tableDefinition(const Relation &relation);

} // namespace slonisko::docgen
