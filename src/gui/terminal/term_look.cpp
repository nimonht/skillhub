#include "gui/terminal/term_look.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>

static QStringList linesOf(const QString &path) {
    QFile f(path);
    QStringList out;
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    QTextStream in(&f);
    while (!in.atEnd()) out << in.readLine().trimmed();
    return out;
}

static QString configHome() { return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation); }

static QVariantMap finish(QVariantMap look, QVector<QString> palette) {
    QVariantList list;
    bool whole = true;
    for (const QString &c : palette) {
        list << c;
        whole = whole && !c.isEmpty();
    }
    if (whole) look.insert(QStringLiteral("palette"), list);
    return look;
}

static QVariantMap readKitty() {
    QVariantMap look;
    QVector<QString> palette(16);
    for (const QString &line : linesOf(configHome() + QStringLiteral("/kitty/kitty.conf"))) {
        const QStringList parts = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (parts.size() < 2 || line.startsWith(QLatin1Char('#'))) continue;
        const QString &key = parts[0];
        if (key == QLatin1String("font_family")) look.insert(QStringLiteral("fontFamily"), parts.mid(1).join(QLatin1Char(' ')));
        else if (key == QLatin1String("font_size")) look.insert(QStringLiteral("fontSize"), qRound(parts[1].toDouble() * 4 / 3));
        else if (key == QLatin1String("foreground") || key == QLatin1String("background")) look.insert(key, parts[1]);
        else if (key.startsWith(QLatin1String("color")) && key.mid(5).toInt() < 16 && key.size() <= 7) palette[key.mid(5).toInt()] = parts[1];
    }
    return finish(look, palette);
}

static QVariantMap readGhostty() {
    QVariantMap look;
    QVector<QString> palette(16);
    for (const QString &line : linesOf(configHome() + QStringLiteral("/ghostty/config"))) {
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq < 0 || line.startsWith(QLatin1Char('#'))) continue;
        const QString key = line.left(eq).trimmed(), value = line.mid(eq + 1).trimmed();
        if (key == QLatin1String("font-family")) look.insert(QStringLiteral("fontFamily"), QString(value).remove(QLatin1Char('"')));
        else if (key == QLatin1String("font-size")) look.insert(QStringLiteral("fontSize"), qRound(value.toDouble() * 4 / 3));
        else if (key == QLatin1String("foreground") || key == QLatin1String("background")) look.insert(key, value.startsWith(QLatin1Char('#')) ? value : QLatin1Char('#') + value);
        else if (key == QLatin1String("palette")) {
            const QStringList kv = value.split(QLatin1Char('='));
            if (kv.size() == 2 && kv[0].trimmed().toInt() < 16) palette[kv[0].trimmed().toInt()] = kv[1].trimmed();
        }
    }
    return finish(look, palette);
}

static QVariantMap readAlacritty() {
    static const QStringList names = {QStringLiteral("black"), QStringLiteral("red"), QStringLiteral("green"), QStringLiteral("yellow"),
                                      QStringLiteral("blue"), QStringLiteral("magenta"), QStringLiteral("cyan"), QStringLiteral("white")};
    QVariantMap look;
    QVector<QString> palette(16);
    QString section;
    for (const QString &line : linesOf(configHome() + QStringLiteral("/alacritty/alacritty.toml"))) {
        if (line.startsWith(QLatin1Char('['))) {
            section = line;
            continue;
        }
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq < 0) continue;
        const QString key = line.left(eq).trimmed();
        QString value = line.mid(eq + 1).trimmed();
        value.remove(QLatin1Char('"')).remove(QLatin1Char('\''));
        if (section == QLatin1String("[font]") && key == QLatin1String("size")) look.insert(QStringLiteral("fontSize"), qRound(value.toDouble() * 4 / 3));
        else if (section == QLatin1String("[font.normal]") && key == QLatin1String("family")) look.insert(QStringLiteral("fontFamily"), value);
        else if (section == QLatin1String("[colors.primary]") && (key == QLatin1String("foreground") || key == QLatin1String("background"))) look.insert(key, value.replace(QLatin1String("0x"), QLatin1String("#")));
        else if (section == QLatin1String("[colors.normal]") && names.contains(key)) palette[names.indexOf(key)] = value.replace(QLatin1String("0x"), QLatin1String("#"));
        else if (section == QLatin1String("[colors.bright]") && names.contains(key)) palette[8 + names.indexOf(key)] = value.replace(QLatin1String("0x"), QLatin1String("#"));
    }
    return finish(look, palette);
}

QVariantMap importTerminalLook(const QString &from) {
    if (from == QLatin1String("kitty")) return readKitty();
    if (from == QLatin1String("ghostty")) return readGhostty();
    if (from == QLatin1String("alacritty")) return readAlacritty();
    return {};
}
