// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "docgen/Document.h"
#include "docgen/Model.h"
#include "docgen/Writer.h"
#include "quality/Analysis.h"
#include "quality/Health.h"

#include <QObject>

#include <functional>
#include <optional>

namespace slonisko::pg {
class QueryRunner;
}

namespace slonisko::quality {

// The database quality report: the way from a connection to a directory of
// pages, shared by slonisko-quality-report and the program's browser. It is
// written by docgen's writers, so it comes in every format the schema
// documentation does.

struct ReportOptions
{
    QString title; // Empty: "<database> quality report".
    QString description; // Text for the summary page, paragraphs apart by blank lines.
    QString generator; // Named at the foot of every page.
    QString generatedAt;
};

struct Options
{
    docgen::SchemaFilter schemas;
    ReportOptions report;
    docgen::WriteOptions output;
    // The statistics and the server's state; without, only the catalog.
    bool health = true;
    bool notices = true; // Leaving them out keeps a big report to what matters.
    Thresholds thresholds;
};

// A summary page with what to look at first, a page for each section, and
// the suggested statements together.
docgen::Site buildReport(const docgen::Model &model, const Health *health,
                         const std::vector<Topic> &topics, const ReportOptions &options);

struct Loaded
{
    docgen::Model model;
    std::optional<Health> health;
};

// Reads the catalog and then, when asked to, the server's state, through a
// runner. done gets them, or the error that stopped the catalog's reading;
// a part of the health that cannot be read is only noted in it. done is
// not called once context is gone.
void loadReport(pg::QueryRunner *runner, const docgen::SchemaFilter &filter, bool health,
                QObject *context,
                std::function<void(const Loaded &loaded, const QString &error)> done);

struct Generated
{
    docgen::WriteResult written;
    std::vector<Topic> topics; // What the report says, notices left out or not.
};

// Analyses, builds the pages and writes them. Touches nothing but the
// output directory, so it may run on a thread of its own.
Generated generate(const Loaded &loaded, const Options &options);

// "slonisko-quality-report 0.1.2", for the pages' footers.
QString generatorName();

} // namespace slonisko::quality
