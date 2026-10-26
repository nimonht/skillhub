#include "app.h"

#include <QClipboard>
#include <QDate>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <QStandardPaths>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QUrl>
#include <algorithm>
#include <utility>

#ifndef SKILLHUB_VERSION
#define SKILLHUB_VERSION "dev"
#endif

namespace {

constexpr int kMaxColumns = 6;  // agent columns in the list; more fold into one overflow column (the backend folds the same way)

QString stripFrontmatter(QString t) {
    t.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    if (!t.startsWith(QStringLiteral("---\n"))) return t;
    const int end = t.indexOf(QStringLiteral("\n---"), 4);
    if (end < 0) return t;
    const int nl = t.indexOf(QLatin1Char('\n'), end + 1);
    return nl < 0 ? QString() : t.mid(nl + 1).trimmed();
}

QString dateOf(const QString &iso) {
    const QDateTime d = QDateTime::fromString(iso, Qt::ISODateWithMs);
    return d.isValid() ? d.toLocalTime().toString(QStringLiteral("yyyy-MM-dd")) : QString();
}

// the agent's brand mark as {vb: [x, y, w, h], paths: [{d, eo}]}, or an empty map (the tile then shows a monogram)
QVariantMap iconOf(const QString &id) {
    static const QJsonObject icons = [] {
        QFile f(QStringLiteral(":/data/provider-icons.json"));
        if (!f.open(QIODevice::ReadOnly)) return QJsonObject();
        return QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("icons")).toObject();
    }();
    const QJsonObject o = icons.value(id).toObject();
    if (o.isEmpty()) return QVariantMap();
    return QVariantMap{{QStringLiteral("vb"), o.value(QStringLiteral("vb")).toArray().toVariantList()},
                       {QStringLiteral("paths"), o.value(QStringLiteral("paths")).toArray().toVariantList()}};
}

// agent tiles and providers as the backend sends them, plus the brand mark that only the window knows
QVariantList withIcons(const QJsonArray &list) {
    QVariantList out;
    out.reserve(list.size());
    for (const QJsonValue &v : list) {
        QVariantMap m = v.toObject().toVariantMap();
        m.insert(QStringLiteral("icon"), iconOf(m.value(QStringLiteral("id")).toString()));
        out << m;
    }
    return out;
}

QString plural(int n) { return n == 1 ? QString() : QStringLiteral("s"); }

QString cleanUserPath(QString p, const QString &home) {
    p = p.trimmed();
    if (p.startsWith(QLatin1String("file://"))) p = QUrl(p).toLocalFile();
    if (p == QLatin1String("~") || p.startsWith(QLatin1String("~/"))) p = home + p.mid(1);
    return p;
}

}  // namespace

// ---- SkillModel --------------------------------------------------------------------------------------------------

QHash<int, QByteArray> SkillModel::roleNames() const {
    return {{NameRole, "name"},   {DescriptionRole, "description"}, {SourceRole, "source"}, {StateRole, "status"},
            {NoteRole, "note"},   {LinksRole, "links"},             {ProblemRole, "problem"}, {RankRole, "rank"}};
}

QVariant SkillModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= rows_.size()) return {};
    const SkillRow &s = rows_[index.row()];
    switch (role) {
    case NameRole: return s.name;
    case DescriptionRole: return s.description;
    case SourceRole: return s.source;
    case StateRole: return s.status;
    case NoteRole: return s.note;
    case ProblemRole: return s.problem;
    case RankRole: return s.rank;
    case LinksRole: return QVariant(s.links);
    }
    return {};
}

void SkillModel::reindex() {
    index_.clear();
    index_.reserve(rows_.size());
    for (int i = 0; i < rows_.size(); ++i) index_.insert(rows_[i].name, i);
}

void SkillModel::reset(const QVector<SkillRow> &rows) {
    beginResetModel();
    rows_ = rows;
    reindex();
    endResetModel();
}

void SkillModel::append(const QVector<SkillRow> &rows) { upsert(rows); }

void SkillModel::upsert(const QVector<SkillRow> &rows) {
    QVector<int> changed;
    QVector<SkillRow> added;
    for (const SkillRow &r : rows) {
        const auto it = index_.constFind(r.name);
        if (it == index_.constEnd()) {
            added << r;
            continue;
        }
        rows_[*it] = r;
        changed << *it;
    }
    // one signal per run of neighbouring rows: the sort proxy then looks at the rows that changed, not at everything
    // between the first and the last of them (an update of 1,000 of 10,000 skills touches rows all over the list)
    std::sort(changed.begin(), changed.end());
    for (qsizetype i = 0; i < changed.size();) {
        qsizetype j = i;
        while (j + 1 < changed.size() && changed[j + 1] == changed[j] + 1) ++j;
        emit dataChanged(index(changed[i]), index(changed[j]));
        i = j + 1;
    }
    if (added.isEmpty()) return;
    beginInsertRows({}, int(rows_.size()), int(rows_.size() + added.size() - 1));
    for (const SkillRow &r : added) {
        index_.insert(r.name, int(rows_.size()));
        rows_ << r;
    }
    endInsertRows();
}

void SkillModel::remove(const QStringList &names) {
    for (const QString &n : names) {
        const auto it = index_.constFind(n);
        if (it == index_.constEnd()) continue;
        const int row = *it;
        beginRemoveRows({}, row, row);
        rows_.remove(row);
        reindex();
        endRemoveRows();
    }
}

const SkillRow *SkillModel::find(const QString &name) const {
    const auto it = index_.constFind(name);
    return it == index_.constEnd() ? nullptr : &rows_[*it];
}

// ---- SkillFilter ---------------------------------------------------------------------------------------------------

SkillFilter::SkillFilter(QObject *parent) : QSortFilterProxyModel(parent) {
    setDynamicSortFilter(true);
    setSortCaseSensitivity(Qt::CaseInsensitive);
    sort(0, Qt::AscendingOrder);
}

void SkillFilter::apply() {
    invalidate();
    sort(0, asc_ ? Qt::AscendingOrder : Qt::DescendingOrder);
    emit changed();
}
void SkillFilter::setText(const QString &t) { if (t != text_) { text_ = t; apply(); } }
void SkillFilter::setMode(const QString &m) { if (m != mode_) { mode_ = m; apply(); } }
void SkillFilter::setSortKey(const QString &k) { if (k != key_) { key_ = k; apply(); } }
void SkillFilter::setAscending(bool a) { if (a != asc_) { asc_ = a; apply(); } }

QString SkillFilter::nameAt(int row) const { return data(index(row, 0), SkillModel::NameRole).toString(); }

int SkillFilter::rowOf(const QString &name) const {
    for (int r = 0; r < rowCount(); ++r)
        if (nameAt(r) == name) return r;
    return -1;
}

// the row itself, not QVariants through data(): sorting 10,000 rows calls this hundreds of thousands of times
const SkillRow &SkillFilter::row(const QModelIndex &i) const {
    return static_cast<const SkillModel *>(sourceModel())->rows()[i.row()];
}

bool SkillFilter::filterAcceptsRow(int r, const QModelIndex &parent) const {
    const SkillRow &s = row(sourceModel()->index(r, 0, parent));
    if (mode_ == QLatin1String("updates") && s.status != QLatin1String("available")) return false;
    if (mode_ == QLatin1String("local") && s.status != QLatin1String("local")) return false;
    if (mode_ == QLatin1String("problems") && !s.problem) return false;
    const QString needle = text_.trimmed();
    if (needle.isEmpty()) return true;
    return s.name.contains(needle, Qt::CaseInsensitive) || s.description.contains(needle, Qt::CaseInsensitive) ||
           s.source.contains(needle, Qt::CaseInsensitive);
}

bool SkillFilter::lessThan(const QModelIndex &l, const QModelIndex &r) const {
    const SkillRow &a = row(l), &b = row(r);
    if (key_ == QLatin1String("status") && a.rank != b.rank) return a.rank < b.rank;
    if (key_ == QLatin1String("source") && a.sourceKey != b.sourceKey) return a.sourceKey < b.sourceKey;
    return a.nameKey < b.nameKey;
}

// ---- LogModel --------------------------------------------------------------------------------------------------------

QHash<int, QByteArray> LogModel::roleNames() const { return {{TimeRole, "time"}, {TextRole, "text"}, {LevelRole, "level"}}; }

QVariant LogModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= lines_.size()) return {};
    const Line &l = lines_[index.row()];
    return role == TimeRole ? l.time : role == TextRole ? l.text : role == LevelRole ? l.level : QVariant();
}

// a batch of lines in one insert; the oldest go once there are more than 400
void LogModel::add(const QJsonArray &lines) {
    if (lines.isEmpty()) return;
    const int keep = 400;
    const int incoming = int(qMin<qsizetype>(lines.size(), keep));
    const int drop = int(qMax<qsizetype>(0, lines_.size() + incoming - keep));
    if (drop > 0) {
        beginRemoveRows({}, 0, drop - 1);
        lines_.remove(0, drop);
        endRemoveRows();
    }
    beginInsertRows({}, int(lines_.size()), int(lines_.size()) + incoming - 1);
    for (qsizetype i = lines.size() - incoming; i < lines.size(); ++i) {
        const QJsonObject o = lines[i].toObject();
        lines_.push_back({o.value(QStringLiteral("time")).toString(), o.value(QStringLiteral("text")).toString(),
                          o.value(QStringLiteral("level")).toString()});
    }
    endInsertRows();
}

void LogModel::addText(const QString &text, const QString &level) {
    add(QJsonArray{QJsonObject{{QStringLiteral("time"), QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"))},
                               {QStringLiteral("text"), text},
                               {QStringLiteral("level"), level}}});
}

void LogModel::clear() {
    beginResetModel();
    lines_.clear();
    endResetModel();
}

// ---- AppController: start, state ---------------------------------------------------------------------------------------

AppController::AppController(QObject *parent) : QObject(parent) {
    const QString forcedTheme = qEnvironmentVariable("SKILLHUB_THEME");
    themeForced_ = !forcedTheme.isEmpty();
    if (themeForced_) themeMode_ = forcedTheme;
    forcedStill_ = qEnvironmentVariableIntValue("SKILLHUB_REDUCE_MOTION") == 1;
    noHover_ = qEnvironmentVariableIntValue("SKILLHUB_NO_HOVER") == 1;
    filter_.setSourceModel(&model_);
    connect(&core_, &CoreClient::ready, this, &AppController::onReady);
    connect(&core_, &CoreClient::notification, this, &AppController::onNotification);
    connect(&core_, &CoreClient::failed, this, [this](const QString &why) {
        setBusy(false);
        log_.addText(QStringLiteral("FAILED to start the background service: ") + why, QStringLiteral("error"));
        emit toast(QStringLiteral("error"), tr("Skill Hub's background service did not start: %1").arg(why));
    });
    core_.start();
}

void AppController::onReady(bool restarted) {
    if (restarted) {
        job_.clear();
        setBusy(false);
        emit toast(QStringLiteral("warn"), tr("The background service restarted"));
    }
    loadSettings();
    refreshAutomation();
    loadSnapshot();
    emit countsChanged();  // centralPath is known now
}

QString AppController::tilde(const QString &path) const {
    const QString home = core_.init().value(QStringLiteral("paths")).toObject().value(QStringLiteral("home")).toString();
    if (!home.isEmpty() && (path == home || path.startsWith(home + QLatin1Char('/')))) return QStringLiteral("~") + path.mid(home.size());
    return path;
}

QString AppController::centralPath() const {
    return tilde(core_.init().value(QStringLiteral("paths")).toObject().value(QStringLiteral("central")).toString());
}

QString AppController::version() const { return QStringLiteral(SKILLHUB_VERSION); }

void AppController::loadSnapshot() {
    loading_ = true;
    stale_ = false;
    loadPage(0, -1);
}

// pages one after another; a delta in between makes the revisions differ, and the load starts over
void AppController::loadPage(int page, qint64 rev) {
    core_.call(QStringLiteral("state.snapshot"), {{QStringLiteral("page"), page}, {QStringLiteral("pageSize"), 1000}}, [this, page, rev](const CoreReply &r) {
        if (!r.ok()) return;  // the backend went away: it is started again, and the load with it
        const QJsonObject o = r.result.toObject();
        const qint64 got = o.value(QStringLiteral("rev")).toInteger();
        if (page > 0 && got != rev) {
            loadSnapshot();
            return;
        }
        const QVector<SkillRow> rows = r.rows ? *r.rows : QVector<SkillRow>();
        if (page == 0) {
            model_.reset(rows);
            applyState(o);
            rev_ = got;
        } else {
            model_.append(rows);
        }
        if (page + 1 < o.value(QStringLiteral("pages")).toInt()) {
            loadPage(page + 1, got);
            return;
        }
        loading_ = false;
        loaded_ = true;
        if (stale_) {
            loadSnapshot();
            return;
        }
        if (!selected_.isEmpty() && !model_.find(selected_)) selected_.clear();
        rebuildDetail();
        emit countsChanged();
        emit focusChanged();
    });
}

void AppController::applyState(const QJsonObject &o) {
    if (o.contains(QStringLiteral("counts"))) counts_ = o.value(QStringLiteral("counts")).toObject();
    if (o.contains(QStringLiteral("agents"))) {
        agents_ = withIcons(o.value(QStringLiteral("agents")).toArray());
        const QJsonObject over = o.value(QStringLiteral("overflow")).toObject();
        hidden_ = over.value(QStringLiteral("count")).toInt();
        overflow_ = over.toVariantMap();
        columns_ = hidden_ > 0 ? kMaxColumns : int(agents_.size());
        portW_ = columns_ <= 0 ? 72 : qBound(56, 360 / columns_, 72);
    }
    if (o.contains(QStringLiteral("providers"))) providerList_ = withIcons(o.value(QStringLiteral("providers")).toArray());
    if (o.contains(QStringLiteral("agents")) || o.contains(QStringLiteral("providers"))) emit agentsChanged();
    emit countsChanged();
}

void AppController::applyDelta(const QJsonObject &p, const SkillRows &rows) {
    if (loading_) {  // the snapshot being loaded is already older: load it again when it is done
        stale_ = true;
        return;
    }
    if (p.value(QStringLiteral("baseRev")).toInteger() != rev_) {
        loadSnapshot();
        return;
    }
    rev_ = p.value(QStringLiteral("rev")).toInteger();
    QStringList removes;
    for (const QJsonValue &v : p.value(QStringLiteral("removes")).toArray()) removes << v.toString();
    if (!removes.isEmpty()) model_.remove(removes);
    if (rows) model_.upsert(*rows);
    applyState(p);
    if (!selected_.isEmpty() && !model_.find(selected_)) selected_.clear();
    rebuildDetail();
    emit focusChanged();  // the focused agent's list of issues
}

void AppController::onNotification(const QString &method, const QJsonObject &params, const SkillRows &rows) {
    if (method == QLatin1String("state.delta")) applyDelta(params, rows);
    else if (method == QLatin1String("state.resync")) loadSnapshot();
    else if (method == QLatin1String("event.log")) log_.add(params.value(QStringLiteral("lines")).toArray());
    else if (method == QLatin1String("event.progress") && params.value(QStringLiteral("job")).toString() == job_ && progress_ >= 0) {
        const double total = params.value(QStringLiteral("total")).toDouble();
        if (total > 0) {
            progress_ = params.value(QStringLiteral("done")).toDouble() / total;
            emit busyChanged();
        }
    } else if (method == QLatin1String("event.jobDone") && params.value(QStringLiteral("job")).toString() == job_) {
        jobDone(params);
    } else if (method == QLatin1String("settings.changed")) {
        const QJsonObject s = params.value(QStringLiteral("settings")).toObject();
        if (!themeForced_ && s.contains(QStringLiteral("theme")) && s.value(QStringLiteral("theme")).toString() != themeMode_) {
            themeMode_ = s.value(QStringLiteral("theme")).toString();
            emit themeChanged();
        }
        if (s.value(QStringLiteral("reduceMotion")).toBool() != reduce_) {
            reduce_ = s.value(QStringLiteral("reduceMotion")).toBool();
            emit themeChanged();
        }
    }
}

// ---- settings ---------------------------------------------------------------------------------------------------------

// From the backend's settings.json. The first 2.0 start takes the theme and motion settings over from 1.x's QSettings.
void AppController::loadSettings() {
    core_.call(QStringLiteral("settings.get"), {}, [this](const CoreReply &r) {
        if (!r.ok()) return;
        QJsonObject s = r.result.toObject();
        QSettings old;
        QJsonObject moved;
        if (!s.contains(QStringLiteral("theme")) && old.contains(QStringLiteral("theme"))) moved.insert(QStringLiteral("theme"), old.value(QStringLiteral("theme")).toString());
        if (!s.contains(QStringLiteral("reduceMotion")) && old.contains(QStringLiteral("reduceMotion"))) moved.insert(QStringLiteral("reduceMotion"), old.value(QStringLiteral("reduceMotion")).toBool());
        if (!moved.isEmpty()) {
            core_.call(QStringLiteral("settings.set"), {{QStringLiteral("patch"), moved}});
            for (auto it = moved.begin(); it != moved.end(); ++it) s.insert(it.key(), it.value());
        }
        if (!themeForced_) themeMode_ = s.value(QStringLiteral("theme")).toString(QStringLiteral("system"));
        reduce_ = s.value(QStringLiteral("reduceMotion")).toBool();
        emit themeChanged();
    });
}

void AppController::saveSetting(const QString &key, const QJsonValue &value) {
    core_.call(QStringLiteral("settings.set"), {{QStringLiteral("patch"), QJsonObject{{key, value}}}});
}

void AppController::setThemeMode(const QString &m) {
    if (m == themeMode_) return;
    themeMode_ = m;
    saveSetting(QStringLiteral("theme"), m);
    emit themeChanged();
}

void AppController::setReduceMotion(bool on) {
    if (on == reduce_) return;
    reduce_ = on;
    saveSetting(QStringLiteral("reduceMotion"), on);
    emit themeChanged();
}

// ---- selection and detail --------------------------------------------------------------------------------------------------

void AppController::setBusy(bool busy, const QString &text, double progress) {
    busy_ = busy;
    busyText_ = busy ? text : QString();
    progress_ = busy ? progress : -1;
    emit busyChanged();
}

void AppController::setFocusAgent(const QString &id) {
    focus_ = id == focus_ ? QString() : id;  // clicking the focused agent again releases it
    if (!focus_.isEmpty()) setDetailMode(QStringLiteral("agent"));
    else if (mode_ == QLatin1String("agent")) setDetailMode(QStringLiteral("skill"));
    emit focusChanged();
}

void AppController::setDetailMode(const QString &m) {
    if (m == mode_) return;
    mode_ = m;
    emit detailModeChanged();
}

void AppController::select(const QString &name) {
    const bool changed = name != selected_;
    selected_ = name;
    if (mode_ != QLatin1String("skill")) setDetailMode(QStringLiteral("skill"));
    if (changed) preview_.clear();
    rebuildDetail();
    if (changed && !name.isEmpty()) readPreview(name);
}

void AppController::readPreview(const QString &name) {
    core_.call(QStringLiteral("skills.read"), {{QStringLiteral("name"), name}}, [this, name](const CoreReply &r) {
        if (name != selected_) return;  // another skill was selected meanwhile
        preview_ = r.ok() ? stripFrontmatter(r.result.toObject().value(QStringLiteral("text")).toString()) : QString();
        emit selectedChanged();
    });
}

void AppController::dismiss() {
    if (mode_ == QLatin1String("add")) closeAdd();
    selected_.clear();
    focus_.clear();
    mode_ = QStringLiteral("skill");
    rebuildDetail();
    emit focusChanged();
    emit detailModeChanged();
}

void AppController::closeAdd() {
    candidates_.clear();
    candidateError_.clear();
    candidateRepo_.clear();
    emit candidatesChanged();
    setDetailMode(focus_.isEmpty() ? QStringLiteral("skill") : QStringLiteral("agent"));
}

QVariantMap AppController::agentDetail() const {
    for (int k = 0; k < agents_.size(); ++k) {
        const QVariantMap m = agents_[k].toMap();
        if (m.value(QStringLiteral("id")).toString() != focus_) continue;
        QVariantMap out = m;
        QStringList issues;
        for (const SkillRow &s : model_.rows())
            if (k < s.links.size() && SkillRow::badLink(s.links[k])) issues << s.name;
        out.insert(QStringLiteral("issues"), issues);
        return out;
    }
    return {};
}

void AppController::rebuildDetail() {
    detail_.clear();
    const SkillRow *s = selected_.isEmpty() ? nullptr : model_.find(selected_);
    if (s) {
        QVariantList links;
        for (int k = 0; k < agents_.size(); ++k)
            links << QVariantMap{{QStringLiteral("label"), agents_[k].toMap().value(QStringLiteral("label"))},
                                 {QStringLiteral("state"), k < s->links.size() ? s->links[k] : QStringLiteral("missing")}};
        detail_ = QVariantMap{{QStringLiteral("name"), s->name},
                              {QStringLiteral("description"), s->description},
                              {QStringLiteral("state"), s->status},
                              {QStringLiteral("note"), s->note},
                              {QStringLiteral("path"), s->path},
                              {QStringLiteral("source"), s->source},
                              {QStringLiteral("upstreamPath"), s->upstreamPath},
                              {QStringLiteral("hash"), s->hash.left(8)},
                              {QStringLiteral("installed"), dateOf(s->installedAt)},
                              {QStringLiteral("updated"), dateOf(s->updatedAt)},
                              {QStringLiteral("tracked"), !s->source.isEmpty()},
                              {QStringLiteral("links"), links}};
    } else {
        preview_.clear();
    }
    emit selectedChanged();
}

// ---- jobs ------------------------------------------------------------------------------------------------------------------

void AppController::startJob(const QString &method, const QJsonObject &params, const QString &busyText, const QString &kind, bool determinate) {
    if (busy_) return;
    jobKind_ = kind;
    job_ = QStringLiteral("?");  // until the backend names it
    setBusy(true, busyText, determinate ? 0.0 : -1.0);
    core_.call(method, params, [this](const CoreReply &r) {
        if (!r.ok()) {
            job_.clear();
            discovering_ = false;
            setBusy(false);
            emit candidatesChanged();
            emit toast(QStringLiteral("error"), r.message());
            return;
        }
        job_ = r.result.toObject().value(QStringLiteral("job")).toString();
    });
}

// The toast for a finished job, as 1.x composed it from its own results.
void AppController::jobDone(const QJsonObject &done) {
    const QString kind = std::exchange(jobKind_, {}), name = std::exchange(jobName_, {});
    job_.clear();
    setBusy(false);
    const QJsonObject sum = done.value(QStringLiteral("summary")).toObject();
    const QJsonArray results = done.value(QStringLiteral("results")).toArray();
    const QString error = done.value(QStringLiteral("error")).toString();
    QString msg, level = QStringLiteral("ok");
    if (kind == QLatin1String("check") || kind == QLatin1String("check-quiet")) {
        const int avail = sum.value(QStringLiteral("available")).toInt();
        if (avail) {
            msg = tr("%1 update%2 available").arg(avail).arg(plural(avail));
            level = QStringLiteral("warn");
        } else if (kind == QLatin1String("check")) {
            msg = sum.value(QStringLiteral("checked")).toInt() > 0 ? tr("Every tracked skill is current") : tr("No skill has an update source yet");
        }
    } else if (kind == QLatin1String("update")) {
        const int n = sum.value(QStringLiteral("updated")).toInt();
        msg = n ? tr("Updated %1 skill%2").arg(n).arg(plural(n)) : tr("Nothing to update");
    } else if (kind == QLatin1String("install")) {
        const int n = sum.value(QStringLiteral("installed")).toInt();
        msg = tr("Installed %1 skill%2").arg(n).arg(plural(n));
        if (!error.isEmpty()) {
            msg = error;
            level = QStringLiteral("error");
        }
    } else if (kind == QLatin1String("import")) {
        const int ok = sum.value(QStringLiteral("installed")).toInt(), failed = sum.value(QStringLiteral("failed")).toInt();
        msg = tr("Installed %1 skill%2").arg(ok).arg(plural(ok)) + (failed ? tr(", %1 failed (see Activity)").arg(failed) : QString());
        level = failed ? QStringLiteral("warn") : QStringLiteral("ok");
        if (!error.isEmpty()) {
            msg = error;
            level = QStringLiteral("error");
        }
    } else if (kind == QLatin1String("adopt")) {
        msg = tr("Links repaired");
    } else if (kind == QLatin1String("remove")) {
        msg = done.value(QStringLiteral("ok")).toBool() ? tr("Removed %1 (backup kept)").arg(name) : tr("Could not remove %1").arg(name);
        level = done.value(QStringLiteral("ok")).toBool() ? QStringLiteral("ok") : QStringLiteral("error");
    } else if (kind == QLatin1String("track")) {
        if (!done.value(QStringLiteral("ok")).toBool()) {
            msg = error;
            level = QStringLiteral("error");
        } else {  // look up the new source right away, as 1.x did
            startJob(QStringLiteral("skills.check"), {{QStringLiteral("names"), QJsonArray{name}}}, tr("Checking %1").arg(name), QStringLiteral("check-quiet"), false);
            return;
        }
    } else if (kind == QLatin1String("discover")) {
        discovering_ = false;
        candidates_.clear();
        for (const QJsonValue &v : done.value(QStringLiteral("candidates")).toArray()) {
            const QJsonObject c = v.toObject();
            candidates_ << QVariantMap{{QStringLiteral("name"), c.value(QStringLiteral("name")).toString()},
                                       {QStringLiteral("description"), c.value(QStringLiteral("description")).toString()},
                                       {QStringLiteral("installed"), c.value(QStringLiteral("installed")).toBool()}};
        }
        candidateError_ = error;
        emit candidatesChanged();
        return;
    } else if (kind == QLatin1String("setManaged") && !error.isEmpty()) {
        msg = error;
        level = QStringLiteral("error");
    }
    if (kind == QLatin1String("check") || kind == QLatin1String("check-quiet") || kind == QLatin1String("update")) {
        int failed = 0;
        QString firstNote;
        for (const QJsonValue &v : results) {
            const QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("status")).toString() != QLatin1String("error")) continue;
            if (!failed++) firstNote = o.value(QStringLiteral("name")).toString() + QStringLiteral(": ") + o.value(QStringLiteral("note")).toString();
        }
        if (failed > 0) {
            msg = (msg.isEmpty() ? QString() : msg + QStringLiteral(". ")) + tr("%1 skill%2 could not be checked (%3)").arg(failed).arg(plural(failed)).arg(firstNote);
            level = QStringLiteral("error");
        }
    }
    if (done.value(QStringLiteral("cancelled")).toBool()) {
        msg = tr("Stopped");
        level = QStringLiteral("warn");
    }
    if (!msg.isEmpty()) emit toast(level, msg);
}

// the window regained focus, or F5: the backend reads the shared folder again; changes come as a delta
void AppController::refresh() {
    core_.call(QStringLiteral("state.snapshot"), {{QStringLiteral("rescan"), true}, {QStringLiteral("page"), 0}, {QStringLiteral("pageSize"), 1}});
}

void AppController::checkUpdates() { startJob(QStringLiteral("skills.check"), {}, tr("Checking upstream"), QStringLiteral("check"), false); }

void AppController::checkUpdatesQuietly() { startJob(QStringLiteral("skills.check"), {}, tr("Checking upstream"), QStringLiteral("check-quiet"), false); }

void AppController::updateAll() { startJob(QStringLiteral("skills.update"), {}, tr("Updating skills"), QStringLiteral("update"), true); }

void AppController::updateSkill(const QString &name) {
    startJob(QStringLiteral("skills.update"), {{QStringLiteral("names"), QJsonArray{name}}}, tr("Updating %1").arg(name), QStringLiteral("update"), false);
}

void AppController::adopt() { startJob(QStringLiteral("adopt"), {}, tr("Adopting strays and repairing links"), QStringLiteral("adopt"), false); }

void AppController::removeSkill(const QString &name) {
    if (busy_) return;
    if (selected_ == name) select(QString());
    startJob(QStringLiteral("skills.remove"), {{QStringLiteral("name"), name}}, tr("Removing %1").arg(name), QStringLiteral("remove"), false);
    jobName_ = name;
}

void AppController::trackSkill(const QString &name, const QString &repo) {
    if (busy_) return;
    startJob(QStringLiteral("skills.track"), {{QStringLiteral("name"), name}, {QStringLiteral("source"), repo.trimmed()}}, tr("Linking source"),
             QStringLiteral("track"), false);
    jobName_ = name;
}

void AppController::discover(const QString &repo) {
    if (busy_) return;
    candidateRepo_ = repo.trimmed();
    candidateError_.clear();
    candidates_.clear();
    discovering_ = true;
    emit candidatesChanged();
    startJob(QStringLiteral("sources.discover"), {{QStringLiteral("input"), candidateRepo_}}, tr("Reading %1").arg(candidateRepo_),
             QStringLiteral("discover"), false);
}

void AppController::installChosen(const QStringList &names) {
    if (busy_ || names.isEmpty()) return;
    const QString first = names.first(), repo = candidateRepo_;
    startJob(QStringLiteral("skills.install"), {{QStringLiteral("source"), repo}, {QStringLiteral("skills"), QJsonArray::fromStringList(names)}},
             tr("Installing skills"), QStringLiteral("install"), true);
    closeAdd();
    select(first);
}

// ---- agents and automation -------------------------------------------------------------------------------------------

void AppController::rescanProviders() {
    core_.call(QStringLiteral("providers.rescan"), {}, [this](const CoreReply &r) {
        if (!r.ok()) return;
        const QJsonObject o = r.result.toObject();
        emit toast(QStringLiteral("ok"), tr("%1 agents managed, %2 known").arg(o.value(QStringLiteral("managed")).toInt()).arg(o.value(QStringLiteral("known")).toInt()));
    });
}

void AppController::setProviderManaged(const QString &id, bool on) {
    startJob(QStringLiteral("providers.setManaged"), {{QStringLiteral("id"), id}, {QStringLiteral("on"), on}},
             on ? tr("Linking skills into the agent") : tr("Updating agents"), QStringLiteral("setManaged"), false);
}

void AppController::addCustomProvider(const QString &label, const QString &dir, bool readsShared) {
    if (busy_) {
        emit customProviderAdded(QStringLiteral("busy"));
        return;
    }
    const QString name = label.trimmed();
    core_.call(QStringLiteral("providers.addCustom"), {{QStringLiteral("label"), label}, {QStringLiteral("dir"), dir}, {QStringLiteral("readsShared"), readsShared}},
               [this, name](const CoreReply &r) {
                   if (!r.ok()) {
                       emit toast(QStringLiteral("error"), r.message());
                       emit customProviderAdded(r.message());
                       return;
                   }
                   emit toast(QStringLiteral("ok"), tr("Added %1").arg(name));
                   emit customProviderAdded(QString());
                   adopt();  // its skills get linked right away
               });
}

void AppController::removeCustomProvider(const QString &id) {
    if (busy_) return;
    if (focus_ == id) {
        focus_.clear();
        emit focusChanged();
    }
    core_.call(QStringLiteral("providers.removeCustom"), {{QStringLiteral("id"), id}}, [this](const CoreReply &r) {
        if (!r.ok()) emit toast(QStringLiteral("error"), r.message());
    });
}

void AppController::refreshAutomation() {
    core_.call(QStringLiteral("automation.status"), {}, [this](const CoreReply &r) {
        if (!r.ok()) return;
        const QJsonObject o = r.result.toObject();
        automationSupported_ = o.value(QStringLiteral("supported")).toBool();
        timer_ = o.value(QStringLiteral("timer")).toString(QStringLiteral("off"));
        watch_ = o.value(QStringLiteral("watcher")).toString(QStringLiteral("off"));
        emit automationChanged();
    });
}

void AppController::setTimer(bool on) {
    core_.call(QStringLiteral("automation.set"), {{QStringLiteral("timer"), on}}, [this, on](const CoreReply &r) {
        if (!r.ok()) emit toast(QStringLiteral("error"), r.message());
        else emit toast(QStringLiteral("ok"), on ? tr("Daily update timer enabled") : tr("Daily update timer removed"));
        refreshAutomation();
    });
}

void AppController::setWatcher(bool on) {
    core_.call(QStringLiteral("automation.set"), {{QStringLiteral("watcher"), on}}, [this, on](const CoreReply &r) {
        if (!r.ok()) emit toast(QStringLiteral("error"), r.message());
        else emit toast(QStringLiteral("ok"), on ? tr("Stray-skill watcher enabled") : tr("Stray-skill watcher removed"));
        refreshAutomation();
    });
}

void AppController::openFolder() {
    QDesktopServices::openUrl(QUrl::fromLocalFile(core_.init().value(QStringLiteral("paths")).toObject().value(QStringLiteral("central")).toString()));
}

void AppController::copyText(const QString &text) {
    QGuiApplication::clipboard()->setText(text);
    emit toast(QStringLiteral("ok"), tr("Copied"));
}

// Qt's markdown defaults (huge headings, oversized fixed-pitch, blue links) look foreign next to the rest of the UI,
// so restyle the parsed document before turning it into HTML. Text colour comes from the QML item.
QString AppController::renderMarkdown(const QString &markdown, bool dark) const {
    QTextDocument doc;
    QFont base(QStringLiteral("Barlow Semi Condensed"));
    base.setPixelSize(14);
    doc.setDefaultFont(base);
    doc.setMarkdown(markdown);

    struct Span { int pos, len; QTextCharFormat fmt; };
    QVector<Span> spans;
    const QColor link = dark ? QColor(QStringLiteral("#34E0A6")) : QColor(QStringLiteral("#07765A"));
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
        const int level = b.blockFormat().headingLevel();
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            QTextCharFormat fmt = f.charFormat();
            bool changed = false;
            if (level > 0) {
                const int px = level == 1 ? 21 : level == 2 ? 18 : 15;
                fmt.setProperty(QTextFormat::FontPixelSize, px);
                fmt.setFontFamilies({QStringLiteral("Barlow Condensed")});
                fmt.setFontWeight(QFont::Bold);
                changed = true;
            }
            if (fmt.fontFixedPitch()) {
                fmt.setFontFamilies({QStringLiteral("monospace")});
                fmt.setProperty(QTextFormat::FontPixelSize, 12);
                changed = true;
            }
            if (fmt.isAnchor()) {
                fmt.setForeground(link);
                changed = true;
            }
            if (changed) spans.push_back({f.position(), f.length(), fmt});
        }
    }
    QTextCursor c(&doc);
    for (const Span &s : spans) {
        c.setPosition(s.pos);
        c.setPosition(s.pos + s.len, QTextCursor::KeepAnchor);
        c.setCharFormat(s.fmt);
    }
    doc.setIndentWidth(20);
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
        QTextBlockFormat bf = b.blockFormat();
        const bool head = bf.headingLevel() > 0;
        bf.setTopMargin(head ? 14 : 0);
        bf.setBottomMargin(head ? 6 : b.textList() ? 3 : 9);
        bf.setLineHeight(122, QTextBlockFormat::ProportionalHeight);
        c.setPosition(b.position());
        c.setBlockFormat(bf);
    }
    QString html = doc.toHtml();
    // keep only the body so the QML item's colour and font apply
    const int b0 = html.indexOf(QStringLiteral("<body"));
    const int b1 = html.indexOf(QLatin1Char('>'), b0);
    const int e = html.lastIndexOf(QStringLiteral("</body>"));
    if (b0 >= 0 && b1 > b0 && e > b1) html = html.mid(b1 + 1, e - b1 - 1);
    return html;
}

// ---- transfer ---------------------------------------------------------------------------------------------------------------

QVariantMap AppController::exportInfo() const {
    int tracked = 0, local = 0;
    for (const SkillRow &s : model_.rows()) (s.source.isEmpty() ? local : tracked)++;
    return {{QStringLiteral("tracked"), tracked}, {QStringLiteral("local"), local}};
}

QString AppController::documentsFolder() const {
    const QString d = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return d.isEmpty() ? QDir::homePath() : d;
}

QString AppController::defaultExportPath() const {
    QString dir = documentsFolder();
    const QString home = QDir::homePath();
    if (dir == home || dir.startsWith(home + QLatin1Char('/'))) dir = QStringLiteral("~") + dir.mid(home.size());
    return dir + QStringLiteral("/skillhub-skills-") + QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd")) + QStringLiteral(".json");
}

void AppController::exportTo(const QString &pathIn, bool includeLocal) {
    QString path = cleanUserPath(pathIn, QDir::homePath());
    if (path.isEmpty()) {
        emit toast(QStringLiteral("error"), tr("Choose where to save"));
        return;
    }
    if (QFileInfo(path).isDir()) path += QStringLiteral("/skillhub-skills-") + QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd")) + QStringLiteral(".json");
    core_.call(QStringLiteral("transfer.export"), {{QStringLiteral("path"), path}, {QStringLiteral("includeLocal"), includeLocal}}, [this, path](const CoreReply &r) {
        if (!r.ok()) {
            emit toast(QStringLiteral("error"), r.message());
            return;
        }
        const QJsonObject o = r.result.toObject();
        const int tracked = o.value(QStringLiteral("tracked")).toInt(), local = o.value(QStringLiteral("local")).toInt();
        const QJsonArray skipped = o.value(QStringLiteral("skipped")).toArray();
        for (const QJsonValue &k : skipped) log_.addText(QStringLiteral("not exported: ") + k.toString());
        log_.addText(QStringLiteral("exported %1 skills (%2 from GitHub, %3 local) to %4").arg(tracked + local).arg(tracked).arg(local).arg(path));
        emit toast(skipped.isEmpty() ? QStringLiteral("ok") : QStringLiteral("warn"),
                   tr("Exported %1 skills").arg(tracked + local) + (skipped.isEmpty() ? QString() : tr(", %1 skipped (see Activity)").arg(skipped.size())));
    });
}

void AppController::importPreview(const QString &pathIn) {
    importItems_.clear();
    importError_.clear();
    importPath_ = cleanUserPath(pathIn, QDir::homePath());
    core_.call(QStringLiteral("transfer.preview"), {{QStringLiteral("path"), importPath_}}, [this](const CoreReply &r) {
        importItems_.clear();
        if (!r.ok()) {
            importError_ = r.message();
        } else {
            for (const QJsonValue &v : r.result.toObject().value(QStringLiteral("items")).toArray()) importItems_ << v.toObject().toVariantMap();
            if (importItems_.isEmpty()) importError_ = tr("There are no skills in that file.");
        }
        setDetailMode(QStringLiteral("transfer"));
        emit importChanged();
    });
}

void AppController::importClear() {
    importItems_.clear();
    importError_.clear();
    emit importChanged();
}

void AppController::importInstall(const QStringList &names) {
    if (busy_ || names.isEmpty()) return;
    startJob(QStringLiteral("transfer.import"), {{QStringLiteral("path"), importPath_}, {QStringLiteral("names"), QJsonArray::fromStringList(names)}},
             tr("Installing from the export"), QStringLiteral("import"), true);
    importClear();
}
