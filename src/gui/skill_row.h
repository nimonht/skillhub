// One row of the skill list as the backend sends it (docs/protocol.md, <Skill>). Built on the backend client's worker
// thread straight from the JSON, so the interface thread only swaps finished rows in.
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QMetaType>
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <QVector>

struct SkillRow {
    QString name, description, source, sourceType, status, note;
    QString path, upstreamPath, hash, installedAt, updatedAt;
    QStringList links;  // per managed agent, in the order of the agent columns
    bool problem = false, parked = false, localEdits = false, hold = false, live = false;
    int versions = 0;
    // sort keys, folded once here: sorting 10,000 rows compares them hundreds of thousands of times
    QString nameKey, sourceKey;
    int rank = 9;

    static SkillRow fromJson(const QJsonObject &o) {
        SkillRow r;
        r.name = o.value(QStringLiteral("name")).toString();
        r.description = o.value(QStringLiteral("description")).toString();
        r.source = o.value(QStringLiteral("source")).toString();
        r.sourceType = o.value(QStringLiteral("sourceType")).toString();
        r.status = o.value(QStringLiteral("status")).toString();
        r.note = o.value(QStringLiteral("note")).toString();
        r.path = o.value(QStringLiteral("path")).toString();
        r.upstreamPath = o.value(QStringLiteral("upstreamPath")).toString();
        r.hash = o.value(QStringLiteral("hash")).toString();
        r.installedAt = o.value(QStringLiteral("installedAt")).toString();
        r.updatedAt = o.value(QStringLiteral("updatedAt")).toString();
        for (const QJsonValue &l : o.value(QStringLiteral("links")).toArray()) r.links << l.toString();
        r.problem = o.value(QStringLiteral("problem")).toBool();
        r.parked = o.value(QStringLiteral("parked")).toBool();
        r.localEdits = o.value(QStringLiteral("localEdits")).toBool();
        r.hold = o.value(QStringLiteral("hold")).toBool();
        r.live = o.value(QStringLiteral("live")).toBool();
        r.versions = o.value(QStringLiteral("versions")).toInt();
        r.nameKey = r.name.toCaseFolded();
        r.sourceKey = r.source.toCaseFolded();
        r.rank = rankOf(r.status);
        return r;
    }

    // the order of the status column: what needs attention first
    static int rankOf(const QString &status) {
        static const QStringList order = {QStringLiteral("available"), QStringLiteral("error"), QStringLiteral("tracked"),
                                          QStringLiteral("updated"), QStringLiteral("current"), QStringLiteral("local")};
        const qsizetype i = order.indexOf(status);
        return i < 0 ? 9 : int(i);
    }

    static bool badLink(const QString &l) {
        return l == QLatin1String("missing") || l == QLatin1String("copy") || l == QLatin1String("foreign");
    }
};

using SkillRows = QSharedPointer<const QVector<SkillRow>>;
Q_DECLARE_METATYPE(SkillRows)
