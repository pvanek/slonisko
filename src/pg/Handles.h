// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <libpq-fe.h>

#include <memory>

namespace slonisko::pg {

struct ConnDeleter
{
    void operator()(PGconn *c) const noexcept { PQfinish(c); }
};

struct ResultDeleter
{
    void operator()(PGresult *r) const noexcept { PQclear(r); }
};

struct NotifyDeleter
{
    void operator()(PGnotify *n) const noexcept { PQfreemem(n); }
};

using ConnPtr = std::unique_ptr<PGconn, ConnDeleter>;
using ResultPtr = std::unique_ptr<PGresult, ResultDeleter>;
using NotifyPtr = std::unique_ptr<PGnotify, NotifyDeleter>;

} // namespace slonisko::pg
