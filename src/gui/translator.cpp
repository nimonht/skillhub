#include "gui/translator.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>

static QString catalogPath(const QString &code) { return QStringLiteral(":/i18n/%1.json").arg(code); }

QStringList JsonTranslator::available() {
    QStringList out{QStringLiteral("en")};
    for (const QString &file : QDir(QStringLiteral(":/i18n")).entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name))
        out << file.section(QLatin1Char('.'), 0, 0);
    return out;
}

QString JsonTranslator::resolve(const QString &choice) {
    const QStringList built = available();
    if (choice != QLatin1String("system") && built.contains(choice)) return choice;
    for (const QString &tag : QLocale::system().uiLanguages()) {   // "vi-VN", "en-US", ...: the language part is enough
        const QString lang = tag.section(QLatin1Char('-'), 0, 0).section(QLatin1Char('_'), 0, 0).toLower();
        if (built.contains(lang)) return lang;
    }
    return QStringLiteral("en");
}

bool JsonTranslator::use(const QString &code) {
    table_.clear();
    code_ = QStringLiteral("en");
    if (code == QLatin1String("en")) return false;
    QFile f(catalogPath(code));
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QJsonObject catalog = QJsonDocument::fromJson(f.readAll()).object();
    for (auto it = catalog.begin(); it != catalog.end(); ++it) {
        const QString text = it.value().toString();
        if (!text.isEmpty() && !it.key().startsWith(QLatin1Char('@'))) table_.insert(it.key(), text);   // "@...": notes for translators
    }
    if (table_.isEmpty()) return false;
    code_ = code;
    return true;
}

// qsTr("Clear", "selection"): the same English, another meaning, is "selection|Clear" in the catalog
QString JsonTranslator::translate(const char *, const char *sourceText, const char *disambiguation, int) const {
    if (!sourceText) return QString();
    const QString text = QString::fromUtf8(sourceText);
    if (disambiguation && *disambiguation) {
        const QString meant = table_.value(QString::fromUtf8(disambiguation) + QLatin1Char('|') + text);
        if (!meant.isEmpty()) return meant;
    }
    return table_.value(text);   // empty: Qt shows the English text
}
