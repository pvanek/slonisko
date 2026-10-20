// SPDX-FileCopyrightText: 2026 Petr Vanek
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ValueViewer.h"

#include "ResultTextView.h"
#include "SqlEditor.h"
#include "ValueLexer.h"

#include "ui_ValueViewer.h"

#include <QClipboard>
#include <QEvent>
#include <QGuiApplication>
#include <QLocale>
#include <QPushButton>
#include <QSettings>

namespace slonisko {

namespace {

const QString FormatKey = QStringLiteral("valueViewer/format");
const QString WrapKey = QStringLiteral("valueViewer/wrap");

} // namespace

ValueViewer::ValueViewer(const QString &title, Oid type, const QByteArray &value, QWidget *parent)
    : QDialog(parent), m_ui(std::make_unique<Ui::ValueViewer>()), m_value(value)
{
    m_ui->setupUi(this);
    setWindowTitle(title);
    // Not the block selection the grid's text view explains in its tooltip:
    // here it is mostly lines that get copied.
    m_ui->text->setToolTip(QString());
    m_ui->text->setMarginType(0, QsciScintilla::NumberMargin);
    m_lexer = new ValueLexer(this);
    m_ui->text->setLexer(m_lexer);
    SqlEditor::applyMarginColors(m_ui->text);

    const QSettings settings;
    m_ui->format->setChecked(settings.value(FormatKey, true).toBool());
    setWrapLines(settings.value(WrapKey, false).toBool());
    m_ui->syntax->setCurrentIndex(int(catalog::guessSyntax(type, value)));

    connect(m_ui->syntax, &QComboBox::currentIndexChanged, this, &ValueViewer::showValue);
    connect(m_ui->format, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(FormatKey, on);
        showValue();
    });
    connect(m_ui->wrap, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(WrapKey, on);
        setWrapLines(on);
    });
    connect(m_ui->copy, &QPushButton::clicked, this,
            [this] { QGuiApplication::clipboard()->setText(text()); });
    connect(m_ui->buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    showValue();
    m_ui->text->setFocus();
}

ValueViewer::~ValueViewer() = default;

catalog::ValueSyntax ValueViewer::syntax() const
{
    return catalog::ValueSyntax(m_ui->syntax->currentIndex());
}

void ValueViewer::setSyntax(catalog::ValueSyntax syntax)
{
    m_ui->syntax->setCurrentIndex(int(syntax));
}

bool ValueViewer::isFormatted() const
{
    return m_ui->format->isChecked();
}

void ValueViewer::setFormatted(bool formatted)
{
    m_ui->format->setChecked(formatted);
}

bool ValueViewer::wrapsLines() const
{
    return m_ui->wrap->isChecked();
}

void ValueViewer::setWrapLines(bool wrap)
{
    m_ui->wrap->setChecked(wrap);
    m_ui->text->setWrapMode(wrap ? QsciScintilla::WrapWord : QsciScintilla::WrapNone);
}

QString ValueViewer::text() const
{
    return m_ui->text->text();
}

QString ValueViewer::status() const
{
    return m_ui->status->text();
}

ResultTextView *ValueViewer::editor() const
{
    return m_ui->text;
}

void ValueViewer::showValue()
{
    const catalog::ValueSyntax syntax = this->syntax();
    m_ui->format->setEnabled(syntax != catalog::ValueSyntax::Text);
    QByteArray shown = m_value;
    QString problem;
    if (syntax != catalog::ValueSyntax::Text && isFormatted()) {
        if (std::optional<QByteArray> pretty = catalog::pretty(syntax, m_value))
            shown = std::move(*pretty);
        else if (syntax == catalog::ValueSyntax::Json)
            problem = tr("Not valid JSON, so shown as it is.");
        else
            problem = tr("Not a well-formed array, so shown as it is.");
    }
    // Tokens and text from the same bytes: Scintilla's positions are bytes
    // of the UTF-8 it holds.
    m_lexer->setTokens(catalog::tokenize(syntax, shown));
    m_ui->text->setText(QString::fromUtf8(shown));
    m_ui->text->recolor();

    const int lines = m_ui->text->lines();
    m_ui->text->setMarginWidth(0, QString(QString::number(lines).size() + 1, QLatin1Char('9')));
    const QString size = tr("%1 characters, %n line(s)", nullptr, lines)
                             .arg(QLocale().toString(QString::fromUtf8(m_value).size()));
    m_ui->status->setText(problem.isEmpty() ? size : problem);
    m_ui->status->setToolTip(problem.isEmpty() ? QString() : size);
}

void ValueViewer::changeEvent(QEvent *event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::PaletteChange
        || event->type() == QEvent::ApplicationPaletteChange) {
        m_lexer->applyPalette();
        SqlEditor::applyMarginColors(m_ui->text);
    }
}

} // namespace slonisko
