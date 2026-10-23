// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "catalog/Plan.h"

#include <QString>

namespace slonisko::catalog {

// A plan as text other tools draw.

// Graphviz: a box per node saying what the diagram's does, tinted by its own
// share, the rows flowing up along edges as thick as the diagram's.
QString planToDot(const Plan &plan);

// Mermaid's flowchart, bottom to top as the rows flow; the nodes taking a
// fifth of the plan or more are marked hot, a twentieth warm.
QString planToMermaid(const Plan &plan);

// The folded stacks flamegraph.pl, speedscope and the like read: a line per
// node, the operations from the root down to it joined by semicolons, then
// what it took itself, in whole microseconds with ANALYZE and hundredths of
// a cost unit without. Nodes that took nothing are left out.
QString planToFoldedStacks(const Plan &plan);

} // namespace slonisko::catalog
