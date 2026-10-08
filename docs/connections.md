# Connections and transactions

```{index} connections, sessions
```

Connecting a saved profile opens a *session*: one SSH tunnel, if the profile
has one, shared by all of the session's connections. Each part of the
program then uses connections as follows.

| Who | Connection | Transactions |
|---|---|---|
| SQL editor page | Its own, one per editor | Yours: autocommit until you run `BEGIN` or press *Begin*; open until you commit or roll back |
| Explain Analyze | The editor's | Wrapped in `BEGIN`/`ROLLBACK`, or a savepoint inside an open transaction, so it changes nothing |
| Saving edited rows | The editor's | `BEGIN`/`COMMIT`, or a savepoint inside an open transaction; all rows or none |
| Result page (DBA Tools, System Info) | Its own, one per page | Autocommit; a running query can be stopped |
| Object tree, completion, diagrams | The session's, shared, one per database | Autocommit; its queries run one after another |

So N editors and M result pages on one profile use N + M + 1 connections,
plus one for each further database the object tree browses.

## What that means in practice

```{index} transactions, isolation
```

- **Editors are isolated** from each other and from the tree. What one
  editor has not committed, nothing else sees — including completion and
  highlighting, which learn about new tables only once their DDL is
  committed.
- **An open transaction is never lost silently.** Closing an editor or the
  window, or switching the editor to another connection, asks whether to
  commit or roll back. A failed transaction can only be rolled back, and a
  running statement is stopped only if you say so.
- **Disconnecting asks first** when any editor of that connection has a
  transaction open or a statement running.

## When the network goes away

```{index} reconnect, disconnect, network problems, hanging connection
```

A connection whose network went away does not always notice: a folder in
the tree keeps saying *Loading…*, an object's page never fills in, a
statement keeps running. *Reconnect* is the way out. It closes all of the
session's connections, and its SSH tunnel, without waiting for the server,
and opens them again with the same credentials, so no password is asked
for.

*Reconnect* and *Disconnect* are in three places, and do the same in each:

- the object tree's toolbar, and its context menu on any node of the
  connection, including one that is still loading;
- each SQL editor's toolbar, beside the connection box;
- the top right of an object's page.

Whatever was waiting fails at once. Editors stay on their connection and
connect again with it; a statement that was running is reported as ended
in *Messages*. Object pages and DBA Tools or System Info pages load again
by themselves, and the tree shows the connection's contents afresh. Like
disconnecting, reconnecting first asks when an editor has a transaction
open or a statement running, as either is rolled back. If the server cannot
be reached yet, the connection shows as failed and the editors wait;
*Reconnect* can be tried again.

## The transaction toolbar

```{index} commit, rollback, transaction state
```

Each editor shows what its connection is doing, next to the *Begin*,
*Commit* and *Rollback* buttons:

- a grey database icon — no transaction, every statement commits itself;
- amber — a transaction is open;
- red — the transaction failed and can only be rolled back.

The buttons do what their names say and leave the result grid alone, so the
rows you are looking at stay where they are.

## Passwords

```{index} passwords, wallet, QtKeychain
```

Passwords go to the system wallet through QtKeychain — KWallet, GNOME
Keyring, macOS Keychain or Windows Credential Manager, whichever the desktop
provides. Without a wallet, or when the program was built without
QtKeychain, they are kept in the settings file in plain text, and the
connection dialog says so. A profile can also be told to ask every time and
store nothing.
