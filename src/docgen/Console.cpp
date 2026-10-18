// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "docgen/Console.h"

#include <QCoreApplication>
#include <QFile>

#include <cstdio>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace slonisko::docgen {

QTextStream &err()
{
    static QTextStream stream(stderr);
    return stream;
}

QString askPassword()
{
    err() << "Password: " << Qt::flush;
#ifdef Q_OS_WIN
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    const bool console = GetConsoleMode(input, &mode);
    if (console)
        SetConsoleMode(input, mode & ~DWORD(ENABLE_ECHO_INPUT));
#else
    termios saved {};
    const bool terminal = tcgetattr(STDIN_FILENO, &saved) == 0;
    if (terminal) {
        termios silent = saved;
        silent.c_lflag &= ~tcflag_t(ECHO);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &silent);
    }
#endif
    QTextStream in(stdin);
    const QString password = in.readLine();
#ifdef Q_OS_WIN
    if (console)
        SetConsoleMode(input, mode);
#else
    if (terminal)
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
#endif
    err() << "\n" << Qt::flush;
    return password;
}

std::optional<QString> textOrFile(const QString &value)
{
    if (!value.startsWith(QLatin1Char('@')))
        return value;
    QFile file(value.mid(1));
    if (!file.open(QIODevice::ReadOnly)) {
        err() << QCoreApplication::applicationName() << ": cannot read " << file.fileName() << ": "
              << file.errorString() << "\n";
        return std::nullopt;
    }
    return QString::fromUtf8(file.readAll());
}

} // namespace slonisko::docgen
