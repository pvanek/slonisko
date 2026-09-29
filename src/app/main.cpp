// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MainWindow.h"
#include "pg/SshTunnel.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char *argv[])
{
    // ssh runs this program to ask for a tunnel's password; answer and exit.
    if (slonisko::pg::SshTunnel::handleAskpass())
        return 0;

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("slonisko"));
    QApplication::setApplicationDisplayName(QStringLiteral("Slonisko"));
    QApplication::setApplicationVersion(QStringLiteral(SLONISKO_VERSION));
    QApplication::setOrganizationName(QStringLiteral("yarpen.cz"));
    QApplication::setOrganizationDomain(QStringLiteral("yarpen.cz"));
    // Lets Wayland shells match windows with slonisko.desktop and its icon.
    QGuiApplication::setDesktopFileName(QStringLiteral("slonisko"));

    QIcon icon;
    for (const int size : {16, 22, 24, 32, 48, 64, 128, 256, 512})
        icon.addFile(QStringLiteral(":/icons/slonisko-%1.png").arg(size), QSize(size, size));
    QApplication::setWindowIcon(icon);

    slonisko::MainWindow window;
    window.show();
    return QApplication::exec();
}
