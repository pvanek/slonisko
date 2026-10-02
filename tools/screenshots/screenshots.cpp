// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

// Takes the manual's screenshots: drives the real main window against the
// sample database that shop.sql creates, and saves PNGs into docs/images.
// run.sh sets up everything around it; see docs/development/building.md.

#include "CompletionPopup.h"
#include "ConnectionBrowser.h"
#include "ConnectionDialog.h"
#include "EditorPage.h"
#include "ErdView.h"
#include "ExportDialog.h"
#include "MainWindow.h"
#include "ObjectPage.h"
#include "PageTabWidget.h"
#include "PlanView.h"
#include "ResultModel.h"
#include "ResultPanel.h"
#include "ResultView.h"
#include "Session.h"
#include "SqlEditor.h"
#include "config/PasswordStore.h"
#include "config/ProfileStore.h"

#include <QApplication>
#include <QDir>
#include <QScreen>
#include <QSettings>
#include <QStyle>
#include <QTableView>
#include <QToolButton>
#include <QTabWidget>
#include <QTest>
#include <QTreeView>

#include <libpq-fe.h>

#include <cstdio>
#include <functional>

using namespace slonisko;

namespace {

QString g_out;
QByteArray g_conninfo;

[[noreturn]] void die(const QString &message)
{
    std::fprintf(stderr, "screenshots: %s\n", qPrintable(message));
    std::exit(1);
}

void waitFor(const std::function<bool()> &done, const char *what, int ms = 15000)
{
    if (!QTest::qWaitFor(done, ms))
        die(QStringLiteral("timed out waiting for %1").arg(QLatin1String(what)));
}

// Lets layouts, highlighting and repaints settle before a picture is taken.
void settle(int ms = 400)
{
    QTest::qWait(ms);
}

void save(const QPixmap &pixmap, const QString &name)
{
    const QString path = QDir(g_out).filePath(name + QStringLiteral(".png"));
    if (!pixmap.save(path))
        die(QStringLiteral("cannot write %1").arg(path));
    std::printf("%s\n", qPrintable(path));
}

void shoot(QWidget *widget, const QString &name)
{
    settle();
    save(widget->grab(), name);
}

unsigned int queryOid(const char *sql)
{
    PGconn *conn = PQconnectdb(g_conninfo.constData());
    if (PQstatus(conn) != CONNECTION_OK)
        die(QString::fromUtf8(PQerrorMessage(conn)));
    PGresult *res = PQexec(conn, sql);
    const unsigned int oid
        = PQresultStatus(res) == PGRES_TUPLES_OK ? QByteArray(PQgetvalue(res, 0, 0)).toUInt() : 0;
    PQclear(res);
    PQfinish(conn);
    if (!oid)
        die(QStringLiteral("no oid for %1").arg(QLatin1String(sql)));
    return oid;
}

// Expands the browser along a path of item names, loading as it goes.
QModelIndex expand(QTreeView *view, const QStringList &path)
{
    QAbstractItemModel *model = view->model();
    QModelIndex at;
    for (const QString &name : path) {
        if (model->canFetchMore(at))
            model->fetchMore(at);
        view->expand(at);
        QModelIndex found;
        waitFor(
            [&] {
                for (int row = 0; row < model->rowCount(at); ++row) {
                    const QModelIndex child = model->index(row, 0, at);
                    if (child.data().toString() == name)
                        found = child;
                }
                return found.isValid();
            },
            "a browser item");
        at = found;
    }
    if (model->canFetchMore(at))
        model->fetchMore(at);
    view->expand(at);
    waitFor([&] { return model->rowCount(at) > 0; }, "browser children");
    return at;
}

void type(EditorPage *page, const QString &sql)
{
    page->editor()->setText(sql);
    page->editor()->setFocus();
}

void cursorAt(EditorPage *page, const QString &word, int offset = 0)
{
    const qsizetype pos = page->editor()->text().toUtf8().indexOf(word.toUtf8());
    if (pos < 0)
        die(QStringLiteral("no %1 in the script").arg(word));
    page->editor()->SendScintilla(QsciScintillaBase::SCI_GOTOPOS,
                                  static_cast<unsigned long>(pos + word.size() + offset));
}

void runAndWait(EditorPage *page)
{
    page->run();
    waitFor([page] { return !page->isRunning(); }, "the statement");
    settle();
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("slonisko"));
    QApplication::setOrganizationName(QStringLiteral("yarpen.cz"));
    QApplication::setOrganizationDomain(QStringLiteral("yarpen.cz"));

    // The style's own light palette, whatever the desktop running this uses.
    QApplication::setPalette(QApplication::style()->standardPalette());

    const QStringList args = app.arguments();
    if (args.size() != 3)
        die(QStringLiteral("usage: screenshots <conninfo> <output directory>"));
    g_conninfo = args[1].toUtf8();
    g_out = args[2];
    QDir().mkpath(g_out);

    // run.sh points XDG_CONFIG_HOME at an empty directory, so this profile
    // is all the browser knows.
    config::ConnectionProfile profile;
    profile.id = QUuid::createUuid();
    profile.name = QStringLiteral("Shop (staging)");
    profile.color = QStringLiteral("#3a7d44");
    profile.passwordMode = config::PasswordMode::None;
    profile.sslMode = QStringLiteral("disable");
    PQconninfoOption *options = PQconninfoParse(g_conninfo.constData(), nullptr);
    if (!options)
        die(QStringLiteral("bad conninfo"));
    for (const PQconninfoOption *o = options; o->keyword; ++o) {
        if (!o->val)
            continue;
        const QByteArray key = o->keyword;
        const QString value = QString::fromUtf8(o->val);
        if (key == "host")
            profile.host = value;
        else if (key == "port")
            profile.port = value.toInt();
        else if (key == "user")
            profile.user = value;
        else if (key == "dbname")
            profile.database = value;
    }
    PQconninfoFree(options);
    {
        QSettings settings;
        config::ProfileStore(settings).save(profile);
    }

    MainWindow window;
    window.move(0, 0);
    window.resize(1100, 720);
    window.show();
    if (!QTest::qWaitForWindowExposed(&window))
        die(QStringLiteral("the window never showed"));

    // The connection dialog, for a server that looks like a real one.
    {
        QSettings settings;
        config::PasswordStore passwords(settings);
        config::ConnectionProfile shown = profile;
        shown.host = QStringLiteral("db.staging.example.com");
        shown.port = 5432;
        shown.user = QStringLiteral("shop_app");
        shown.passwordMode = config::PasswordMode::Save;
        ConnectionDialog dialog(shown, &passwords, &window);
        dialog.show();
        shoot(&dialog, QStringLiteral("connection-dialog"));
    }

    ConnectionBrowser *browser = window.browser();
    browser->connectProfile(profile.id);
    waitFor(
        [browser] {
            const auto sessions = browser->connectedSessions();
            return !sessions.empty() && sessions.front()->state() == Session::State::Connected;
        },
        "the connection");
    Session *session = browser->connectedSessions().front();

    const QModelIndex tables
        = expand(browser->view(),
                 {profile.name, QStringLiteral("shop"), QStringLiteral("Schemas"),
                  QStringLiteral("shop"), QStringLiteral("Tables")});
    browser->view()->setCurrentIndex(tables);

    // The main window: browser, a script and its rows.
    window.mainTabs()->clear();
    EditorPage *page = window.newEditor(session, QStringLiteral("shop"));
    waitFor([page] { return page->connection()->state() == pg::Connection::State::Ready; },
            "the editor's connection");
    type(page,
         QStringLiteral("-- Best-selling products of the last quarter\n"
                        "SELECT p.sku, p.name, c.name AS category,\n"
                        "       sum(i.quantity) AS sold,\n"
                        "       sum(i.quantity * i.unit_price) AS revenue\n"
                        "FROM shop.order_item i\n"
                        "JOIN shop.\"order\" o ON o.id = i.order_id\n"
                        "JOIN shop.product p ON p.id = i.product_id\n"
                        "JOIN shop.category c ON c.id = p.category_id\n"
                        "WHERE o.ordered_at > now() - interval '3 months'\n"
                        "  AND o.status <> 'cancelled'\n"
                        "GROUP BY p.sku, p.name, c.name\n"
                        "ORDER BY revenue DESC\n"
                        "LIMIT 50;\n"));
    cursorAt(page, QStringLiteral("LIMIT 50"), -1);
    runAndWait(page);
    page->resultPanel()->results()->sizeColumns();
    shoot(&window, QStringLiteral("main-window"));

    // Completion: a column list after a table alias.
    type(page, QStringLiteral("SELECT c.\nFROM shop.customer c\nWHERE c.country = 'CZ';\n"));
    cursorAt(page, QStringLiteral("SELECT c."));
    page->editor()->complete();
    waitFor([page] { return page->editor()->completionPopup()->isVisible(); }, "completion");
    {
        // The popup and the lines it completes, not the empty editor around them.
        QWidget *popup = page->editor()->completionPopup();
        const QPoint origin = page->editor()->mapToGlobal(QPoint());
        const QRect area = QRect(origin, popup->geometry().bottomRight() + QPoint(24, 24));
        settle();
        save(page->screen()->grabWindow(0, area.x(), area.y(), area.width(), area.height()),
             QStringLiteral("completion"));
    }
    QTest::keyClick(page->editor()->completionPopup(), Qt::Key_Escape);

    // An error, marked in the script and explained in Messages.
    type(
        page,
        QStringLiteral("SELECT id, full_name, emial\nFROM shop.customer\nWHERE country = 'CZ';\n"));
    cursorAt(page, QStringLiteral("emial"));
    runAndWait(page);
    page->resultPanel()->showMessages();
    shoot(page, QStringLiteral("error"));

    // The result views: grid, text, record.
    type(page,
         QStringLiteral("SELECT id, email, full_name, country, created_at\nFROM shop.customer\n"
                        "ORDER BY id;\n"));
    cursorAt(page, QStringLiteral("ORDER BY id"));
    runAndWait(page);
    ResultView *results = page->resultPanel()->results();
    results->sizeColumns();
    results->setViewMode(ResultView::ViewMode::Text);
    shoot(results, QStringLiteral("results-text"));
    results->setViewMode(ResultView::ViewMode::Record);
    results->table()->setCurrentIndex(results->model()->index(4, 0));
    shoot(results, QStringLiteral("results-record"));
    results->setViewMode(ResultView::ViewMode::Grid);

    // Editing: one changed value, waiting to be saved.
    waitFor([results] { return results->editToggle()->isEnabled(); }, "editable rows");
    results->editToggle()->click();
    results->model()->setData(results->model()->index(2, 2), QStringLiteral("Eva Dvořáková"));
    results->model()->setData(results->model()->index(5, 3), QStringLiteral("SK"));
    results->table()->setCurrentIndex(results->model()->index(5, 3));
    shoot(results, QStringLiteral("results-editing"));
    results->model()->discardChanges();
    results->editToggle()->click();

    // Export.
    {
        ExportDialog dialog({results->model()->rowCount(), 0, false}, QStringLiteral("customer"),
                            &window);
        dialog.show();
        shoot(&dialog, QStringLiteral("export-dialog"));
    }

    // A plan.
    type(page,
         QStringLiteral("SELECT c.country, count(*), sum(t.total)\n"
                        "FROM shop.order_total t\n"
                        "JOIN shop.customer c ON c.id = t.customer_id\n"
                        "WHERE t.status = 'shipped'\n"
                        "GROUP BY c.country;\n"));
    cursorAt(page, QStringLiteral("GROUP BY"));
    page->explain(true);
    waitFor([page] { return !page->isRunning(); }, "the plan");
    page->resultPanel()->showPlan();
    shoot(page->resultPanel(), QStringLiteral("plan"));

    // Object pages: a table, then the schema's diagram.
    ObjectPage *table = window.showObject(
        session, QStringLiteral("shop"), catalog::ObjectKind::Table,
        queryOid("SELECT 'shop.product'::regclass::oid"), QStringLiteral("shop.product"));
    waitFor([table] { return !table->detail().title.isEmpty(); }, "the table's details");
    table->tabs()->setCurrentIndex(1); // Columns: more to see than the overview.
    settle();
    save(table->grab(QRect(0, 0, table->width(), 300)), QStringLiteral("object-page"));

    ObjectPage *schema = window.showObject(
        session, QStringLiteral("shop"), catalog::ObjectKind::Schema,
        queryOid("SELECT oid FROM pg_namespace WHERE nspname = 'shop'"), QStringLiteral("shop"));
    waitFor([schema] { return !schema->detail().title.isEmpty(); }, "the schema's details");
    schema->tabs()->setCurrentWidget(schema->diagram()->parentWidget());
    waitFor([schema] { return !schema->diagram()->graph().nodes.empty(); }, "the diagram");
    schema->diagram()->fitDiagram();
    shoot(schema, QStringLiteral("diagram"));

    std::fflush(stdout);
    // Closing the window would ask about the open pages; nothing here needs keeping.
    std::_Exit(0);
}
