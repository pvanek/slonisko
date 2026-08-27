// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MainWindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("slonisko"));
    QApplication::setApplicationDisplayName(QStringLiteral("Slonisko"));
    QApplication::setApplicationVersion(QStringLiteral(SLONISKO_VERSION));
    QApplication::setOrganizationName(QStringLiteral("slonisko"));

    slonisko::MainWindow window;
    window.show();
    return QApplication::exec();
}
