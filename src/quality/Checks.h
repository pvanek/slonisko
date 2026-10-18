// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "docgen/Model.h"

#include <QString>

#include <vector>

namespace slonisko::quality {

// What a look over the catalog finds worth a second thought, with the SQL
// that would deal with it where there is one. Nothing here is an error:
// each finding is a question for whoever knows the schema. The quality
// report grades them and adds what the statistics say.

enum class Check {
    NoPrimaryKey,
    UnindexedForeignKey,
    ForeignKeyTypeMismatch,
    InvalidIndex,
    DuplicateIndex,
    RedundantIndex,
    DuplicateConstraint,
    NotValidConstraint,
    DisabledTrigger,
    DuplicateTrigger,
    RowSecurity,
    SecurityDefiner,
    SequenceTypeMismatch,
    UnusedSequence,
    IdenticalDefinition,
    NameCaseClash,
    NeedsQuoting,
    DiscouragedType,
    Undocumented,
};

struct Finding
{
    Check check = Check::NoPrimaryKey;
    QString schema;
    QString relation; // The table or view it is about.
    // The constraint, index, trigger or column, if it is about one; a
    // routine's signature or a sequence's name when relation is empty.
    QString object;
    QString message;
    QString fix; // SQL, or empty.
};

// Every check, in the order the report shows them.
std::vector<Check> allChecks();
QString checkTitle(Check check);
// Why the check is there, a sentence or two.
QString checkExplanation(Check check);

std::vector<Finding> runChecks(const docgen::Model &model);

} // namespace slonisko::quality
