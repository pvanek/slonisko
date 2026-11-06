// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "schemadiff/Diff.h"

#include <QTest>

using namespace slonisko::schemadiff;
using namespace slonisko::docgen;

namespace {

Column column(const char *name, const char *type, bool notNull = false, const char *def = "")
{
    Column c;
    c.name = QString::fromUtf8(name);
    c.type = QString::fromUtf8(type);
    c.notNull = notNull;
    c.defaultValue = QString::fromUtf8(def);
    return c;
}

Relation table(const char *name, std::vector<Column> columns, Oid oid = 1)
{
    Relation r;
    r.oid = oid;
    r.schema = QStringLiteral("public");
    r.name = QString::fromUtf8(name);
    r.kind = 'r';
    r.columns = std::move(columns);
    return r;
}

Relation view(const char *name, const char *definition, Oid oid)
{
    Relation r;
    r.oid = oid;
    r.schema = QStringLiteral("public");
    r.name = QString::fromUtf8(name);
    r.kind = 'v';
    r.definition = QString::fromUtf8(definition);
    return r;
}

Model model()
{
    Model m;
    m.schemas.push_back({QStringLiteral("public"), {}, {}});
    return m;
}

const Change *find(const Diff &diff, ChangeKind kind, ObjectType type, const QString &name)
{
    const auto it = std::ranges::find_if(diff.changes, [&](const Change &c) {
        return c.kind == kind && c.type == type && c.name == name;
    });
    return it == diff.changes.end() ? nullptr : &*it;
}

qsizetype position(const QString &script, const char *text)
{
    return script.indexOf(QString::fromUtf8(text));
}

bool hasHazard(const Change &change, HazardType type)
{
    return std::ranges::any_of(change.hazards, [type](const Hazard &h) { return h.type == type; });
}

} // namespace

class TestSchemaDiff : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void same()
    {
        Model m = model();
        m.relations.push_back(table("t", {column("id", "integer", true)}));
        const Diff diff = compare(m, m);
        QVERIFY(diff.isEmpty());
        QVERIFY(script(diff, {}).contains(QLatin1String("nothing to do")));
    }

    void addedTable()
    {
        Model reference = model();
        Relation t = table("Orders", {column("id", "integer", true), column("note", "text")});
        t.comment = QStringLiteral("It's here.");
        Constraint pk;
        pk.name = QStringLiteral("orders_pkey");
        pk.type = 'p';
        pk.definition = QStringLiteral("PRIMARY KEY (id)");
        t.constraints.push_back(pk);
        Index pkIndex;
        pkIndex.name = QStringLiteral("orders_pkey");
        pkIndex.primary = true;
        pkIndex.definition = QStringLiteral(
            "CREATE UNIQUE INDEX orders_pkey ON public.\"Orders\" USING btree (id)");
        Index note;
        note.name = QStringLiteral("orders_note");
        note.definition
            = QStringLiteral("CREATE INDEX orders_note ON public.\"Orders\" USING btree (note)");
        t.indexes = {pkIndex, note};
        reference.relations.push_back(t);

        const Diff diff = compare(model(), reference);
        QCOMPARE(diff.changes.size(), std::size_t(1));
        const Change &change = diff.changes.front();
        QCOMPARE(change.kind, ChangeKind::Added);
        QCOMPARE(change.name, QStringLiteral("public.Orders"));
        // A new table's index blocks nobody.
        QVERIFY(change.hazards.empty());

        const QString sql = script(diff, {});
        QVERIFY2(
            sql.contains(QLatin1String(
                "CREATE TABLE public.\"Orders\" (\n    id integer NOT NULL,\n    note text\n);")),
            qPrintable(sql));
        // The primary key's index is the constraint's.
        QCOMPARE(sql.count(QLatin1String("CREATE UNIQUE INDEX")), 0);
        QVERIFY(position(sql, "CREATE TABLE") < position(sql, "ADD CONSTRAINT orders_pkey"));
        QVERIFY(position(sql, "ADD CONSTRAINT orders_pkey")
                < position(sql, "CREATE INDEX orders_note"));
        QVERIFY(
            sql.contains(QLatin1String("COMMENT ON TABLE public.\"Orders\" IS 'It''s here.';")));
        QVERIFY(sql.contains(QLatin1String("BEGIN;")));
        QVERIFY(sql.trimmed().endsWith(QLatin1String("COMMIT;")));
    }

    void columns()
    {
        Model target = model();
        target.relations.push_back(table(
            "t",
            {column("id", "integer"), column("old", "text"), column("n", "integer", false, "1")}));
        Model reference = model();
        reference.relations.push_back(table(
            "t",
            {column("id", "bigint", true), column("n", "integer"), column("fresh", "text", true)}));
        const Diff diff = compare(target, reference);

        const Change *removed
            = find(diff, ChangeKind::Removed, ObjectType::Column, QStringLiteral("public.t.old"));
        QVERIFY(removed);
        QVERIFY(hasHazard(*removed, HazardType::DataLoss));
        QCOMPARE(removed->statements,
                 QStringList {QStringLiteral("ALTER TABLE public.t DROP COLUMN old;")});

        const Change *id
            = find(diff, ChangeKind::Changed, ObjectType::Column, QStringLiteral("public.t.id"));
        QVERIFY(id);
        QCOMPARE(id->details, QStringLiteral("type integer → bigint, not null"));
        QVERIFY(hasHazard(*id, HazardType::Rewrite));
        QVERIFY(hasHazard(*id, HazardType::Blocking));
        QCOMPARE(
            id->statements,
            (QStringList {QStringLiteral(
                              "ALTER TABLE public.t ALTER COLUMN id TYPE bigint USING id::bigint;"),
                          QStringLiteral("ALTER TABLE public.t ALTER COLUMN id SET NOT NULL;")}));

        const Change *n
            = find(diff, ChangeKind::Changed, ObjectType::Column, QStringLiteral("public.t.n"));
        QVERIFY(n);
        QCOMPARE(n->statements,
                 QStringList {QStringLiteral("ALTER TABLE public.t ALTER COLUMN n DROP DEFAULT;")});

        const Change *fresh
            = find(diff, ChangeKind::Added, ObjectType::Column, QStringLiteral("public.t.fresh"));
        QVERIFY(fresh);
        // NOT NULL without a default cannot be added to rows already there.
        QVERIFY(hasHazard(*fresh, HazardType::Manual));
    }

    void retypeKeepsDefault()
    {
        Model target = model();
        target.relations.push_back(table("t", {column("n", "integer", false, "0")}));
        Model reference = model();
        reference.relations.push_back(table("t", {column("n", "bigint", false, "0")}));
        const Diff diff = compare(target, reference);
        QCOMPARE(diff.changes.size(), std::size_t(1));
        QCOMPARE(
            diff.changes[0].statements,
            (QStringList {
                QStringLiteral("ALTER TABLE public.t ALTER COLUMN n DROP DEFAULT;"),
                QStringLiteral("ALTER TABLE public.t ALTER COLUMN n TYPE bigint USING n::bigint;"),
                QStringLiteral("ALTER TABLE public.t ALTER COLUMN n SET DEFAULT 0;")}));
    }

    void views()
    {
        // b reads a, which reads t; t's column changes type, so both are
        // made again: dropped b first, made a first.
        Model target = model();
        target.relations.push_back(table("t", {column("x", "integer")}, 1));
        target.relations.push_back(view("a", "SELECT x FROM public.t", 2));
        target.relations.push_back(view("b", "SELECT x FROM public.a", 3));
        target.dependencies = {{2, 1}, {3, 2}};
        Model reference = target;
        reference.relations[0].columns[0].type = QStringLiteral("bigint");

        const Diff diff = compare(target, reference);
        const Change *a
            = find(diff, ChangeKind::Changed, ObjectType::View, QStringLiteral("public.a"));
        const Change *b
            = find(diff, ChangeKind::Changed, ObjectType::View, QStringLiteral("public.b"));
        QVERIFY(a);
        QVERIFY(b);
        QCOMPARE(b->details, QStringLiteral("made again for what it reads"));
        const QString sql = script(diff, {});
        QVERIFY(position(sql, "DROP VIEW public.b") < position(sql, "DROP VIEW public.a"));
        QVERIFY(position(sql, "DROP VIEW public.a") < position(sql, "ALTER COLUMN x TYPE"));
        QVERIFY(position(sql, "ALTER COLUMN x TYPE") < position(sql, "CREATE VIEW public.a"));
        QVERIFY(position(sql, "CREATE VIEW public.a") < position(sql, "CREATE VIEW public.b"));
        QVERIFY(sql.contains(QLatin1String("CREATE VIEW public.b AS\nSELECT x FROM public.a;")));
    }

    void droppedTables()
    {
        // b references a; both go.
        Model target = model();
        Relation a = table("a", {column("id", "integer")}, 1);
        Relation b = table("b", {column("a_id", "integer")}, 2);
        Constraint fk;
        fk.name = QStringLiteral("b_a_fkey");
        fk.type = 'f';
        fk.referencedOid = 1;
        fk.definition = QStringLiteral("FOREIGN KEY (a_id) REFERENCES public.a(id)");
        b.constraints.push_back(fk);
        target.relations = {a, b};
        const Diff diff = compare(target, model());
        QCOMPARE(diff.changes.size(), std::size_t(2));
        const QString sql = script(diff, {});
        QVERIFY(position(sql, "ALTER TABLE public.b DROP CONSTRAINT b_a_fkey;")
                < position(sql, "DROP TABLE public.a;"));
        QVERIFY(sql.contains(QLatin1String("Hazard, data loss")));
    }

    void enums()
    {
        UserType status;
        status.schema = QStringLiteral("public");
        status.name = QStringLiteral("status");
        status.kind = 'e';
        status.labels = {QStringLiteral("new"), QStringLiteral("sent")};
        Model target = model();
        target.types.push_back(status);
        Model reference = target;
        reference.types[0].labels
            = {QStringLiteral("new"), QStringLiteral("paid"), QStringLiteral("sent")};
        // A table that uses the new value at once.
        reference.relations.push_back(
            table("t", {column("s", "public.status", false, "'paid'::public.status")}));

        Diff diff = compare(target, reference);
        const QString sql = script(diff, {});
        const QString add = QStringLiteral(
            "ALTER TYPE public.status ADD VALUE IF NOT EXISTS 'paid' AFTER 'new';");
        QVERIFY2(sql.contains(add), qPrintable(sql));
        // Committed before the transaction that uses it begins.
        QVERIFY(sql.indexOf(add) < position(sql, "BEGIN;"));
        QVERIFY(position(sql, "BEGIN;") < position(sql, "CREATE TABLE"));

        // Values cannot go, nor change their order.
        reference.types[0].labels = {QStringLiteral("new")};
        diff = compare(target, reference);
        const Change *change
            = find(diff, ChangeKind::Changed, ObjectType::Type, QStringLiteral("public.status"));
        QVERIFY(change);
        QVERIFY(hasHazard(*change, HazardType::Manual));
        QVERIFY(change->statements.front().startsWith(QLatin1String("-- ")));
        QVERIFY(change->hazards.front().message.contains(QLatin1String("sent")));
    }

    void routines()
    {
        Routine f;
        f.schema = QStringLiteral("public");
        f.name = QStringLiteral("f");
        f.arguments = QStringLiteral("x integer");
        f.result = QStringLiteral("integer");
        f.definition = QStringLiteral("CREATE OR REPLACE FUNCTION public.f(x integer)\n RETURNS "
                                      "integer\n LANGUAGE sql\nAS $function$SELECT x$function$");
        Model target = model();
        target.routines.push_back(f);
        Model reference = target;
        reference.routines[0].definition.replace(QLatin1String("SELECT x"),
                                                 QLatin1String("SELECT x + 1"));

        Diff diff = compare(target, reference);
        QCOMPARE(diff.changes.size(), std::size_t(1));
        QCOMPARE(diff.changes[0].name, QStringLiteral("public.f(x integer)"));
        QCOMPARE(diff.changes[0].statements,
                 QStringList {reference.routines[0].definition + QLatin1Char(';')});

        // What it returns cannot be replaced.
        reference.routines[0].result = QStringLiteral("bigint");
        diff = compare(target, reference);
        QCOMPARE(diff.changes[0].statements.front(),
                 QStringLiteral("DROP FUNCTION public.f(x integer);"));

        diff = compare(target, model());
        QCOMPARE(diff.changes[0].kind, ChangeKind::Removed);
        QCOMPARE(script(diff, {}).count(QLatin1String("DROP FUNCTION public.f(x integer);")), 1);
    }

    void concurrentIndexes()
    {
        Model target = model();
        target.relations.push_back(table("t", {column("x", "integer")}));
        Model reference = target;
        Index index;
        index.name = QStringLiteral("t_x");
        index.definition = QStringLiteral("CREATE INDEX t_x ON public.t USING btree (x)");
        reference.relations[0].indexes.push_back(index);

        Diff diff = compare(target, reference);
        QVERIFY(diff.transactional());
        QVERIFY(hasHazard(diff.changes[0], HazardType::Blocking));

        diff = compare(target, reference, {.concurrentIndexes = true});
        QVERIFY(!diff.transactional());
        QVERIFY(diff.changes[0].hazards.empty());
        const QString sql = script(diff, {});
        QVERIFY(sql.contains(
            QLatin1String("CREATE INDEX CONCURRENTLY t_x ON public.t USING btree (x);")));
        QVERIFY(!sql.contains(QLatin1String("BEGIN;")));
        QVERIFY(sql.contains(QLatin1String("SET check_function_bodies = off;")));

        // Dropped the same way.
        diff = compare(reference, target, {.concurrentIndexes = true});
        QCOMPARE(diff.changes[0].statements,
                 QStringList {QStringLiteral("DROP INDEX CONCURRENTLY public.t_x;")});
    }

    void foreignKeyOnExistingTable()
    {
        Model target = model();
        target.relations.push_back(table("t", {column("x", "integer")}));
        Model reference = target;
        Constraint check;
        check.name = QStringLiteral("t_x_check");
        check.type = 'c';
        check.definition = QStringLiteral("CHECK (x > 0)");
        reference.relations[0].constraints.push_back(check);
        const Diff diff = compare(target, reference);
        QCOMPARE(diff.changes[0].statements,
                 (QStringList {
                     QStringLiteral(
                         "ALTER TABLE public.t ADD CONSTRAINT t_x_check CHECK (x > 0) NOT VALID;"),
                     QStringLiteral("ALTER TABLE public.t VALIDATE CONSTRAINT t_x_check;")}));
    }

    void partitions()
    {
        Relation parent = table("m", {column("at", "date", true)}, 1);
        parent.kind = 'p';
        parent.partitionKey = QStringLiteral("RANGE (at)");
        Relation part = table("m_2026", {column("at", "date", true)}, 2);
        part.parentSchema = QStringLiteral("public");
        part.parentName = QStringLiteral("m");
        part.partitionBound = QStringLiteral("FOR VALUES FROM ('2026-01-01') TO ('2027-01-01')");
        Index index;
        index.name = QStringLiteral("m_at");
        index.definition = QStringLiteral("CREATE INDEX m_at ON ONLY public.m USING btree (at)");
        parent.indexes.push_back(index);
        Model reference = model();
        reference.relations = {part, parent};

        const Diff diff = compare(model(), reference);
        const QString sql = script(diff, {});
        QVERIFY(sql.contains(QLatin1String(
            "CREATE TABLE public.m (\n    at date NOT NULL\n) PARTITION BY RANGE (at);")));
        QVERIFY(sql.contains(
            QLatin1String("CREATE TABLE public.m_2026 PARTITION OF public.m FOR VALUES")));
        QVERIFY(position(sql, "CREATE TABLE public.m (") < position(sql, "PARTITION OF"));
        // So that the partitions get theirs too.
        QVERIFY(sql.contains(QLatin1String("CREATE INDEX m_at ON public.m USING btree (at);")));

        // Dropping the table takes its partitions along.
        const Diff dropped = compare(reference, model());
        QCOMPARE(dropped.changes.size(), std::size_t(1));
    }

    void serialSequence()
    {
        Model reference = model();
        reference.relations.push_back(
            table("t", {column("id", "integer", true, "nextval('public.t_id_seq'::regclass)")}));
        Sequence sequence;
        sequence.schema = QStringLiteral("public");
        sequence.name = QStringLiteral("t_id_seq");
        sequence.type = QStringLiteral("integer");
        sequence.maximum = 2147483647;
        sequence.ownedBy = QStringLiteral("public.t.id");
        reference.sequences.push_back(sequence);
        const QString sql = script(compare(model(), reference), {});
        QVERIFY(position(sql, "CREATE SEQUENCE public.t_id_seq")
                < position(sql, "CREATE TABLE public.t"));
        QVERIFY(position(sql, "CREATE TABLE public.t") < position(sql, "OWNED BY public.t.id"));

        // An identity column's sequence is the column's own.
        reference.relations[0].columns[0].identity = QStringLiteral("ALWAYS");
        reference.relations[0].columns[0].defaultValue.clear();
        QVERIFY(
            !script(compare(model(), reference), {}).contains(QLatin1String("CREATE SEQUENCE")));
    }

    void lines()
    {
        Model target = model();
        target.relations.push_back(table("t", {column("x", "integer")}));
        Model reference = model();
        reference.relations.push_back(table("t", {column("x", "bigint")}));
        std::vector<std::vector<std::pair<int, int>>> lines;
        const QStringList sql
            = script(compare(target, reference), {}, &lines).split(QLatin1Char('\n'));
        QCOMPARE(lines.size(), std::size_t(1));
        QCOMPARE(lines[0].size(), std::size_t(1));
        const auto [first, last] = lines[0][0];
        QVERIFY(sql[first].startsWith(QLatin1String("-- changed column public.t.x")));
        QVERIFY(sql[last].contains(QLatin1String("TYPE bigint")));
    }
};

QTEST_GUILESS_MAIN(TestSchemaDiff)
#include "tst_schemadiff.moc"
