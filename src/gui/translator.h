// The window in other languages. qsTr() in QML and tr() in C++ look their text up here: i18n/<code>.json, embedded, maps each
// English text to its translation. Qt's .ts and .qm tools would add a build dependency to every package recipe; this needs none.
// tests/i18n_check.py keeps the catalogs complete.
#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QTranslator>

class JsonTranslator : public QTranslator {
public:
    // "en", "vi", or "system": the first of the system's languages that has a catalog, else English
    static QString resolve(const QString &choice);
    static QStringList available();   // the catalogs built in, English first
    bool use(const QString &code);    // false for English, or a catalog that cannot be read: the text stays as written
    QString code() const { return code_; }

    QString translate(const char *context, const char *sourceText, const char *disambiguation = nullptr, int n = -1) const override;
    bool isEmpty() const override { return table_.isEmpty(); }

private:
    QHash<QString, QString> table_;
    QString code_ = QStringLiteral("en");
};
