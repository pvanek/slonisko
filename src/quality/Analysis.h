// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "docgen/Model.h"
#include "quality/Health.h"

#include <QDate>
#include <QString>

#include <optional>
#include <vector>

namespace slonisko::quality {

// The quality report's findings: the checks of the catalog, graded,
// and what the statistics and the server's state add to them.

enum class Severity {
    Critical, // Data or availability at risk now.
    Warning, // Worth fixing.
    Notice, // Worth knowing.
};

QString severityName(Severity severity);

enum class Section {
    Schema, // How tables, keys and routines are made.
    Indexes, // Missing, duplicate, redundant, unused, invalid.
    Dependencies, // What is broken or left half done between objects.
    Duplicates, // Constraints, triggers, definitions and names twice.
    Health, // The server and the database as they run.
};

std::vector<Section> allSections();
QString sectionTitle(Section section);
QString sectionExplanation(Section section);
// Its page, and the start of its topics' anchors: "indexes".
QString sectionKey(Section section);

struct Issue
{
    Severity severity = Severity::Warning;
    QString object; // schema.table index, a setting, a session...
    QString message;
    QString fix; // SQL, or empty.
};

// One thing looked at, with what it found, possibly nothing.
struct Topic
{
    Section section = Section::Schema;
    QString id; // Unique, for anchors: "unused-indexes".
    QString title;
    QString explanation;
    std::vector<Issue> issues;
    // Why it could not be looked at, such as a statistics view the account
    // may not read; empty when it was.
    QString unavailable;
    // Whether its fixes belong among the statements to run.
    bool scripted = true;
};

// Where a finding starts to count. The defaults are what is commonly
// taken for trouble; tests lower them.
struct Thresholds
{
    double idleInTransactionSeconds = 5 * 60;
    double longTransactionSeconds = 60 * 60;
    double criticalTransactionSeconds = 24 * 60 * 60;
    double preparedSeconds = 5 * 60;
    // Unused indexes count only when the statistics are older than this,
    // and the index larger.
    qint64 statisticsAgeSeconds = 7 * 24 * 60 * 60;
    qint64 unusedIndexBytes = 1024 * 1024;
    // A table read whole more often than through an index, this big.
    qint64 sequentialScanRows = 10'000;
    qint64 deadRows = 10'000;
    double deadRowsShare = 0.2;
    qint64 staleAnalyzeRows = 1'000;
    double staleAnalyzeShare = 0.2;
    double sequenceUsedShare = 0.75;
    double sequenceCriticalShare = 0.9;
    double connectionsShare = 0.8;
    double connectionsCriticalShare = 0.95;
    double cacheHitShare = 0.99;
    double cacheHitCriticalShare = 0.9;
    qint64 cacheBlocks = 10'000; // Fewer reads say nothing about the cache.
    qint64 retainedWalBytes = 10LL * 1024 * 1024 * 1024;
    qint64 tempBytes = 1024LL * 1024 * 1024;
    qint64 wraparoundAge = 1'000'000'000;
    qint64 wraparoundCriticalAge = 1'500'000'000;
    int endOfLifeWarningDays = 183;
};

// Everything looked at, in the order of the report, nothing found
// included. Without health, the topics needing statistics are left out.
std::vector<Topic> analyse(const docgen::Model &model, const Health *health,
                           const Thresholds &thresholds = {});

// When a major version stops getting fixes, or a null date for one not
// known here.
QDate endOfLife(int serverVersion);

int count(const std::vector<Topic> &topics, Section section, Severity severity);
int count(const std::vector<Topic> &topics, Severity severity);
// The most severe of every issue; none when there are none.
std::optional<Severity> worst(const std::vector<Topic> &topics);

} // namespace slonisko::quality
