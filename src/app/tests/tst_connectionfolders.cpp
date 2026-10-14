// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "BrowserModel.h"
#include "ConnectionBrowser.h"
#include "config/ProfileStore.h"

#include <QAbstractItemModelTester>
#include <QMimeData>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeView>

#include <memory>

using namespace slonisko;
using NodeType = BrowserModel::NodeType;

namespace {

config::ConnectionProfile profile(const QString &name, const QString &folder = {})
{
    config::ConnectionProfile p;
    p.id = QUuid::createUuid();
    p.name = name;
    p.folder = folder;
    return p;
}

} // namespace

// Folders of connections, in the model and as the browser stores them. No
// server needed: nothing here connects.
class TestConnectionFolders : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init()
    {
        m_model = std::make_unique<BrowserModel>();
        m_tester = std::make_unique<QAbstractItemModelTester>(
            m_model.get(), QAbstractItemModelTester::FailureReportingMode::QtTest);
        m_dir.emplace();
        m_settings.emplace(m_dir->filePath(QStringLiteral("test.ini")), QSettings::IniFormat);
    }

    void cleanup()
    {
        m_tester.reset();
        m_model.reset();
        m_settings.reset();
        m_dir.reset();
    }

    // The tree from the request: folders first, then connections.
    void tree()
    {
        m_model->setProfiles({profile(QStringLiteral("conn5"), QStringLiteral("stage")),
                              profile(QStringLiteral("conn1"), QStringLiteral("dev")),
                              profile(QStringLiteral("conn3"), QStringLiteral("stage/env1")),
                              profile(QStringLiteral("conn2"), QStringLiteral("stage/env1")),
                              profile(QStringLiteral("conn4"), QStringLiteral("stage/foo")),
                              profile(QStringLiteral("top"))},
                             {QStringLiteral("empty")});
        QCOMPARE(dump(),
                 QStringLiteral("dev[conn1] empty stage[env1[conn2 conn3] foo[conn4] "
                                "conn5] top"));

        const QModelIndex env1 = m_model->folderIndex(QStringLiteral("stage/env1"));
        QCOMPARE(env1.data(BrowserModel::NodeTypeRole).value<NodeType>(), NodeType::Category);
        QCOMPARE(env1.data(BrowserModel::FolderRole).toString(), QStringLiteral("stage/env1"));
        QVERIFY(env1.data(BrowserModel::ProfileIdRole).toUuid().isNull());
        QVERIFY(!m_model->sessionOf(env1));
        const QModelIndex conn2 = m_model->index(0, 0, env1);
        QCOMPARE(conn2.data(BrowserModel::FolderRole).toString(), QStringLiteral("stage/env1"));
        QCOMPARE(m_model->profileIndex(conn2.data(BrowserModel::ProfileIdRole).toUuid()), conn2);
        QCOMPARE(
            m_model->folders(),
            (QStringList {QStringLiteral("dev"), QStringLiteral("empty"), QStringLiteral("stage"),
                          QStringLiteral("stage/env1"), QStringLiteral("stage/foo")}));
    }

    void updateKeepsItsPlace()
    {
        auto a = profile(QStringLiteral("a"));
        auto b = profile(QStringLiteral("b"));
        m_model->setProfiles({a, b});
        QPersistentModelIndex index = m_model->profileIndex(a.id);

        a.name = QStringLiteral("c");
        m_model->updateProfile(a);
        QCOMPARE(dump(), QStringLiteral("c b")); // Renamed in place.

        a.folder = QStringLiteral("x/y");
        m_model->updateProfile(a); // New folders and moved ones go to the end.
        QCOMPARE(dump(), QStringLiteral("b x[y[c]]"));
        QVERIFY(index.isValid()); // Moved, not removed and added again.
        QCOMPARE(QModelIndex(index), m_model->profileIndex(a.id));

        m_model->removeProfile(a.id);
        QCOMPARE(dump(), QStringLiteral("b x[y]")); // Folders stay.
    }

    void moveFolders()
    {
        m_model->setProfiles({profile(QStringLiteral("conn1"), QStringLiteral("dev")),
                              profile(QStringLiteral("conn2"), QStringLiteral("stage/env1")),
                              profile(QStringLiteral("conn3"), QStringLiteral("stage/foo")),
                              profile(QStringLiteral("conn4"), QStringLiteral("old/foo"))});
        QPersistentModelIndex env1 = m_model->folderIndex(QStringLiteral("stage/env1"));

        m_model->moveFolder(QStringLiteral("stage/env1"), QStringLiteral("dev/env1"));
        QCOMPARE(dump(),
                 QStringLiteral("dev[conn1 env1[conn2]] old[foo[conn4]] stage[foo[conn3]]"));
        QCOMPARE(QModelIndex(env1), m_model->folderIndex(QStringLiteral("dev/env1")));
        const QUuid conn2 = m_model->index(0, 0, env1).data(BrowserModel::ProfileIdRole).toUuid();
        QCOMPARE(m_model->profile(conn2)->folder, QStringLiteral("dev/env1"));

        // Into a folder that has one of the same name: their contents merge.
        m_model->moveFolder(QStringLiteral("old"), QStringLiteral("stage"));
        QCOMPARE(dump(), QStringLiteral("dev[conn1 env1[conn2]] stage[foo[conn3 conn4]]"));

        // Not into itself.
        m_model->moveFolder(QStringLiteral("stage"), QStringLiteral("stage/foo/x"));
        QCOMPARE(dump(), QStringLiteral("dev[conn1 env1[conn2]] stage[foo[conn3 conn4]]"));

        m_model->removeFolder(QStringLiteral("stage"));
        QCOMPARE(dump(), QStringLiteral("dev[conn1 env1[conn2]] foo[conn3 conn4]"));
        m_model->removeFolder(QStringLiteral("foo"));
        QCOMPARE(dump(), QStringLiteral("dev[conn1 env1[conn2]] conn3 conn4"));
    }

    // Folders and connections stay where they were put, and setProfiles()
    // puts them back there; what the order does not know follows, sorted.
    void manualOrder()
    {
        const auto conn1 = profile(QStringLiteral("conn1"));
        const auto conn2 = profile(QStringLiteral("conn2"), QStringLiteral("stage"));
        const auto conn3 = profile(QStringLiteral("conn3"), QStringLiteral("stage"));
        m_model->setProfiles({conn1, conn2, conn3}, {QStringLiteral("dev")});
        QCOMPARE(dump(), QStringLiteral("dev stage[conn2 conn3] conn1"));

        m_model->setRow(m_model->folderIndex(QStringLiteral("dev")), 2);
        m_model->setRow(m_model->profileIndex(conn3.id), 0);
        QCOMPARE(dump(), QStringLiteral("stage[conn3 conn2] conn1 dev"));
        const QStringList order = m_model->order();
        QCOMPARE(order.first(), QStringLiteral("folder:stage"));

        const auto conn4 = profile(QStringLiteral("a new one"));
        m_model->setProfiles({conn4, conn3, conn2, conn1}, {QStringLiteral("dev")}, order);
        QCOMPARE(dump(), QStringLiteral("stage[conn3 conn2] conn1 dev a new one"));
    }

    void dragAndDrop()
    {
        const auto conn = profile(QStringLiteral("conn"));
        m_model->setProfiles({conn}, {QStringLiteral("a/b"), QStringLiteral("c")});
        QSignalSpy moved(m_model.get(), &BrowserModel::moveRequested);

        std::unique_ptr<QMimeData> data(m_model->mimeData(
            {m_model->profileIndex(conn.id), m_model->folderIndex(QStringLiteral("a"))}));
        QVERIFY(data);
        const QModelIndex c = m_model->folderIndex(QStringLiteral("c"));
        QVERIFY(m_model->canDropMimeData(data.get(), Qt::MoveAction, -1, 0, c));
        // Not into what is dragged, nor among a connection's own children.
        QVERIFY(!m_model->canDropMimeData(data.get(), Qt::MoveAction, -1, 0,
                                          m_model->folderIndex(QStringLiteral("a/b"))));
        QVERIFY(!m_model->canDropMimeData(data.get(), Qt::MoveAction, -1, 0,
                                          m_model->profileIndex(conn.id)));

        QVERIFY(m_model->dropMimeData(data.get(), Qt::MoveAction, -1, 0, c));
        QCOMPARE(moved.size(), 1);
        QCOMPARE(moved[0][0].value<QList<QUuid>>(), QList<QUuid> {conn.id});
        QCOMPARE(moved[0][1].toStringList(), QStringList {QStringLiteral("a")});
        QCOMPARE(moved[0][2].toString(), QStringLiteral("c"));
        QCOMPARE(moved[0][3].toInt(), -1); // Onto the folder: at its end.
        QCOMPARE(dump(), QStringLiteral("a[b] c conn")); // The model only tells.

        // Dropped on the top level, below conn: its row without what is
        // dragged, which is above it.
        QVERIFY(m_model->dropMimeData(data.get(), Qt::MoveAction, 3, 0, {}));
        QCOMPARE(moved[1][2].toString(), QString());
        QCOMPARE(moved[1][3].toInt(), 1);
    }

    // What the browser does with folders is stored, and read back.
    void browserStoresFolders()
    {
        config::ProfileStore store(*m_settings);
        auto conn1 = profile(QStringLiteral("conn1"), QStringLiteral("dev"));
        auto conn2 = profile(QStringLiteral("conn2"));
        store.save(conn1);
        store.save(conn2);

        {
            ConnectionBrowser browser(*m_settings, false);
            BrowserModel *model = browser.model();
            browser.addFolder(QStringLiteral("stage/env1"));
            browser.moveItems({conn2.id}, {}, QStringLiteral("stage/env1"));
            browser.moveItems({}, {QStringLiteral("dev")}, QStringLiteral("stage"));
            QVERIFY(!browser.renameFolder(QStringLiteral("stage/dev"), QStringLiteral("env1")));
            QVERIFY(browser.renameFolder(QStringLiteral("stage/env1"), QStringLiteral("env2")));
            QCOMPARE(model->profile(conn2.id)->folder, QStringLiteral("stage/env2"));
            browser.view()->expand(model->folderIndex(QStringLiteral("stage")));
            QCoreApplication::processEvents(); // Queued moves from drops, if any.
        }

        ConnectionBrowser browser(*m_settings, false);
        BrowserModel *model = browser.model();
        QCOMPARE(model->profile(conn1.id)->folder, QStringLiteral("stage/dev"));
        QCOMPARE(model->profile(conn2.id)->folder, QStringLiteral("stage/env2"));
        QCOMPARE(model->folders(),
                 (QStringList {QStringLiteral("stage"), QStringLiteral("stage/env2"),
                               QStringLiteral("stage/dev")}));
        QVERIFY(browser.view()->isExpanded(model->folderIndex(QStringLiteral("stage"))));
        QVERIFY(!browser.view()->isExpanded(model->folderIndex(QStringLiteral("stage/dev"))));

        // Move to Folder offers everywhere else, and for a folder nothing
        // within it.
        QCOMPARE(browser.moveTargets(model->profileIndex(conn2.id)),
                 (QStringList {QString(), QStringLiteral("stage"), QStringLiteral("stage/dev")}));
        QCOMPARE(browser.moveTargets(model->folderIndex(QStringLiteral("stage/dev"))),
                 (QStringList {QString(), QStringLiteral("stage/env2")}));
        QCOMPARE(browser.moveTargets(model->folderIndex(QStringLiteral("stage"))), QStringList());

        // The first folder dropped below the last item goes to the end, and
        // stays there.
        browser.addFolder(QStringLiteral("other"));
        QCOMPARE(model->order().first(), QStringLiteral("folder:stage"));
        browser.moveItems({}, {QStringLiteral("stage")}, QString(), model->rowCount() - 1);
        QCOMPARE(model->index(model->rowCount() - 1, 0).data().toString(), QStringLiteral("stage"));
        {
            ConnectionBrowser again(*m_settings, false);
            QCOMPARE(again.model()->order(), model->order());
        }

        browser.removeFolder(QStringLiteral("stage"));
        QCOMPARE(config::ProfileStore(*m_settings).folders().size(), 3);
        QCOMPARE(model->profile(conn1.id)->folder, QStringLiteral("dev"));
    }

private:
    // The tree on one line: names, with folders' contents in brackets.
    QString dump(const QModelIndex &parent = {}) const
    {
        QStringList out;
        for (int row = 0; row < m_model->rowCount(parent); ++row) {
            const QModelIndex index = m_model->index(row, 0, parent);
            QString text = index.data().toString();
            if (index.data(BrowserModel::NodeTypeRole).value<NodeType>() == NodeType::Category
                && m_model->rowCount(index) > 0)
                text += QLatin1Char('[') + dump(index) + QLatin1Char(']');
            out << text;
        }
        return out.join(QLatin1Char(' '));
    }

    std::unique_ptr<BrowserModel> m_model;
    std::unique_ptr<QAbstractItemModelTester> m_tester;
    std::optional<QTemporaryDir> m_dir;
    std::optional<QSettings> m_settings;
};

QTEST_MAIN(TestConnectionFolders)
#include "tst_connectionfolders.moc"
