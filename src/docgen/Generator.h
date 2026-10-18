// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "docgen/Model.h"
#include "docgen/Pages.h"
#include "docgen/Writer.h"

#include <QObject>

#include <functional>

namespace slonisko::pg {
class QueryRunner;
}

namespace slonisko::docgen {

// The way from a connection to a directory of documentation, shared by
// slonisko-docgen and the program's browser.

struct Options
{
    SchemaFilter schemas;
    PageOptions pages;
    WriteOptions output;
};

// Reads the model through a runner, its queries one after another. done
// gets the model, or the error that stopped it; it is not called once
// context is gone.
void loadModel(pg::QueryRunner *runner, const SchemaFilter &filter, QObject *context,
               std::function<void(const Model &model, const QString &error)> done);

// Builds the pages and writes them. Touches nothing but
// the output directory and Graphviz, so it may run on a thread of its own.
// For HTML diagrams without a Graphviz program given, dot is looked for on
// the PATH.
WriteResult generate(const Model &model, const Options &options);

// "slonisko-docgen 0.1.2", for the pages' footers.
QString generatorName();

} // namespace slonisko::docgen
