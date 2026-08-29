// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MainWindow.h"
#include "pg/SshTunnel.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    // ssh runs this program to ask for a tunnel's password; answer and exit.
    if (slonisko::pg::SshTunnel::handleAskpass())
        return 0;

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("slonisko"));
    QApplication::setApplicationDisplayName(QStringLiteral("Slonisko"));
    QApplication::setApplicationVersion(QStringLiteral(SLONISKO_VERSION));
    QApplication::setOrganizationName(QStringLiteral("slonisko"));

    slonisko::MainWindow window;
    window.show();
    return QApplication::exec();
}
