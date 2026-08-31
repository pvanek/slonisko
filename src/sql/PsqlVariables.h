// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QStringList>

#include <vector>

namespace slonisko::sql {

// psql variables, as \set, \unset and :name use them, so scripts written
// for psql run unchanged.
class PsqlVariables
{
public:
    // A substitution made in a statement, in bytes of the original text.
    struct Replacement
    {
        qsizetype offset = 0;
        qsizetype length = 0; // Of ":name" in the original.
        qsizetype newLength = 0; // Of the value put in its place.
    };

    // Runs \set, \unset or \echo, like psql. Returns false for other
    // meta-commands. output gets what psql would print, if anything.
    bool apply(const QByteArray &command, QString *output = nullptr);

    // Replaces :name with the value, :'name' with it as a quoted literal and
    // :"name" as a quoted identifier, outside strings, comments and dollar
    // quotes. Unset variables are left as they are and listed in unset.
    QByteArray substitute(const QByteArray &sql, std::vector<Replacement> *replacements = nullptr,
                          QStringList *unset = nullptr) const;

    // Maps a byte offset in substituted text back to the original text.
    static qsizetype originalOffset(qsizetype offset, const std::vector<Replacement> &replacements);

    bool contains(const QString &name) const { return m_values.contains(name); }
    QString value(const QString &name) const { return m_values.value(name); }
    void set(const QString &name, const QString &value) { m_values.insert(name, value); }
    void unset(const QString &name) { m_values.remove(name); }
    const QMap<QString, QString> &values() const { return m_values; }

private:
    // A meta-command's arguments, with quoting and :name substitution done.
    QStringList arguments(QByteArrayView text) const;

    QMap<QString, QString> m_values;
};

} // namespace slonisko::sql
