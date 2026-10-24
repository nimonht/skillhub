#include "app.h"
#include "platform.h"

#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QDate>
#include <QtConcurrent>

using namespace hub;

#ifndef SKILLHUB_VERSION
#define SKILLHUB_VERSION "dev"
#endif

namespace {

QString linkKey(LinkState s) {
    switch (s) {
    case LinkState::Native: return QStringLiteral("shared");
    case LinkState::Linked: return QStringLiteral("link");
    case LinkState::Missing: return QStringLiteral("missing");
    case LinkState::Copy: return QStringLiteral("copy");
    case LinkState::Foreign: return QStringLiteral("foreign");
    }
    return {};
}

bool badLink(LinkState s) { return s == LinkState::Missing || s == LinkState::Copy || s == LinkState::Foreign; }

QString tilde(QString p) {
    const QString home = platform::homeDir();
    if (p == home || p.startsWith(home + QLatin1Char('/'))) p.replace(0, home.size(), QStringLiteral("~"));
    return p;
}

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

QString monogram(const QString &label) {
    // two letters: initials of the first two words, or of two camel-case parts ("OpenCode" -> OC), else the first two letters
    QStringList parts;
    QString cur;
    for (const QChar c : label) {
        if (c.isSpace() || c == QLatin1Char('-')) { if (!cur.isEmpty()) parts << cur; cur.clear(); continue; }
        if (c.isUpper() && !cur.isEmpty() && cur.back().isLower()) { parts << cur; cur.clear(); }
        cur += c;
    }
    if (!cur.isEmpty()) parts << cur;
    if (parts.size() >= 2) return (QString(parts[0][0]) + parts[1][0]).toUpper();
    return label.left(2).toUpper();
}

// the name under a column: the vendor's own short form when the full label would be cut off ("Claude Code" -> "Claude")
QString shortLabel(const QString &label) {
    static const QStringList tails = {QStringLiteral(" Code CLI"), QStringLiteral(" for Terminal"), QStringLiteral(" Code"),
                                      QStringLiteral(" CLI"),      QStringLiteral(" Agent"),        QStringLiteral(" Build"),
                                      QStringLiteral(" Vibe")};
    QString s = label;
    if (s.startsWith(QLatin1String("GitHub "))) s = s.mid(7);
    if (s.size() <= 9) return s;
    for (const QString &t : tails)
        if (s.endsWith(t) && s.size() > t.size()) return s.left(s.size() - t.size());
    return s;
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

QString mechanismOf(const AgentDef &a) {
    if (a.mode == QLatin1String("native")) return QStringLiteral("Reads the shared folder directly");
    if (a.mode == QLatin1String("config")) return QStringLiteral("Shared folder listed in ") + tilde(a.configFile);
    return QStringLiteral("Symlinks in ") + tilde(a.dir);
}

int stateRank(const QString &k) {
    static const QHash<QString, int> r = {{QStringLiteral("available"), 0}, {QStringLiteral("error"), 1}, {QStringLiteral("tracked"), 2},
                                          {QStringLiteral("updated"), 3},   {QStringLiteral("current"), 4}, {QStringLiteral("local"), 5}};
    return r.value(k, 9);
}

}  // namespace

// ---- SkillModel --------------------------------------------------------------------------------------------------

QString SkillModel::stateKey(const Skill &s, bool justUpdated) {
    if (justUpdated) return QStringLiteral("updated");
    switch (s.update) {
    case Skill::Untracked: return QStringLiteral("local");
    case Skill::Unknown: return QStringLiteral("tracked");
    case Skill::UpToDate: return QStringLiteral("current");
    case Skill::Available: return QStringLiteral("available");
    case Skill::Error: return QStringLiteral("error");
    }
    return {};
}

bool SkillModel::hasProblem(const Skill &s) {
    if (s.update == Skill::Error) return true;
    for (auto it = s.agents.constBegin(); it != s.agents.constEnd(); ++it)
        if (badLink(it.value())) return true;
    return false;
}

void SkillModel::setSkills(const QVector<Skill> &skills, const QVector<AgentDef> &agents, const QSet<QString> &justUpdated) {
    beginResetModel();
    skills_ = skills;
    agents_ = agents;
    updated_ = justUpdated;
    endResetModel();
}

const Skill *SkillModel::find(const QString &name) const {
    for (const Skill &s : skills_)
        if (s.name == name) return &s;
    return nullptr;
}

QHash<int, QByteArray> SkillModel::roleNames() const {
    return {{NameRole, "name"},   {DescriptionRole, "description"}, {SourceRole, "source"}, {StateRole, "status"},
            {NoteRole, "note"},   {LinksRole, "links"},             {ProblemRole, "problem"}, {RankRole, "rank"}};
}

QVariant SkillModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= skills_.size()) return {};
    const Skill &s = skills_[index.row()];
    const QString state = stateKey(s, updated_.contains(s.name) && s.update == Skill::UpToDate);
    switch (role) {
    case NameRole: return s.name;
    case DescriptionRole: return s.description;
    case SourceRole: return s.src.source;
    case StateRole: return state;
    case NoteRole: return s.note;
    case ProblemRole: return hasProblem(s);
    case RankRole: return stateRank(state);
    case LinksRole: {
        QVariantList l;
        for (const AgentDef &a : agents_) l << linkKey(s.agents.value(a.id, LinkState::Missing));
        return l;
    }
    }
    return {};
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

bool SkillFilter::filterAcceptsRow(int row, const QModelIndex &parent) const {
    const QModelIndex i = sourceModel()->index(row, 0, parent);
    const QString state = sourceModel()->data(i, SkillModel::StateRole).toString();
    if (mode_ == QLatin1String("updates") && state != QLatin1String("available")) return false;
    if (mode_ == QLatin1String("local") && state != QLatin1String("local")) return false;
    if (mode_ == QLatin1String("problems") && !sourceModel()->data(i, SkillModel::ProblemRole).toBool()) return false;
    const QString needle = text_.trimmed();
    if (needle.isEmpty()) return true;
    for (int role : {int(SkillModel::NameRole), int(SkillModel::DescriptionRole), int(SkillModel::SourceRole)})
        if (sourceModel()->data(i, role).toString().contains(needle, Qt::CaseInsensitive)) return true;
    return false;
}

bool SkillFilter::lessThan(const QModelIndex &l, const QModelIndex &r) const {
    const auto str = [&](const QModelIndex &i, int role) { return sourceModel()->data(i, role).toString(); };
    const auto byName = [&] { return QString::compare(str(l, SkillModel::NameRole), str(r, SkillModel::NameRole), Qt::CaseInsensitive) < 0; };
    if (key_ == QLatin1String("status")) {
        const int a = sourceModel()->data(l, SkillModel::RankRole).toInt(), b = sourceModel()->data(r, SkillModel::RankRole).toInt();
        return a != b ? a < b : byName();
    }
    if (key_ == QLatin1String("source")) {
        const int c = QString::compare(str(l, SkillModel::SourceRole), str(r, SkillModel::SourceRole), Qt::CaseInsensitive);
        return c != 0 ? c < 0 : byName();
    }
    return byName();
}

// ---- LogModel --------------------------------------------------------------------------------------------------------

QHash<int, QByteArray> LogModel::roleNames() const { return {{TimeRole, "time"}, {TextRole, "text"}, {LevelRole, "level"}}; }

QVariant LogModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= lines_.size()) return {};
    const Line &l = lines_[index.row()];
    return role == TimeRole ? l.time : role == TextRole ? l.text : role == LevelRole ? l.level : QVariant();
}

void LogModel::add(const QString &text) {
    Line l;
    l.time = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    l.text = tilde(text);
    const QString low = text.toLower();
    l.level = (text.startsWith(QStringLiteral("FAILED")) || low.contains(QStringLiteral("failed"))) ? QStringLiteral("error")
              : (text.startsWith(QStringLiteral("CONFLICT")) || low.startsWith(QStringLiteral("refusing"))) ? QStringLiteral("warn")
                                                                                                         : QStringLiteral("info");
    if (lines_.size() >= 400) {
        beginRemoveRows({}, 0, 0);
        lines_.removeFirst();
        endRemoveRows();
    }
    beginInsertRows({}, lines_.size(), lines_.size());
    lines_.push_back(l);
    endInsertRows();
}

void LogModel::clear() {
    beginResetModel();
    lines_.clear();
    endResetModel();
}

// ---- AppController -----------------------------------------------------------------------------------------------------

AppController::AppController(QObject *parent) : QObject(parent) {
    QSettings s;
    themeMode_ = qEnvironmentVariable("SKILLHUB_THEME", s.value(QStringLiteral("theme"), QStringLiteral("system")).toString());
    reduce_ = s.value(QStringLiteral("reduceMotion"), false).toBool();
    forcedStill_ = qEnvironmentVariableIntValue("SKILLHUB_REDUCE_MOTION") == 1;
    noHover_ = qEnvironmentVariableIntValue("SKILLHUB_NO_HOVER") == 1;

    filter_.setSourceModel(&model_);
    connect(&store_, &Store::log, &log_, &LogModel::add);
    connect(&jobWatcher_, &QFutureWatcher<QVector<Skill>>::finished, this, &AppController::finished);
    connect(&discoverWatcher_, &QFutureWatcher<DiscoverResult>::finished, this, [this] {
        const DiscoverResult r = discoverWatcher_.result();
        pending_ = r.items;
        candidates_.clear();
        for (const Candidate &c : r.items)
            candidates_ << QVariantMap{{QStringLiteral("name"), c.name},
                                       {QStringLiteral("description"), c.description},
                                       {QStringLiteral("installed"), model_.find(c.name) != nullptr}};
        candidateError_ = r.error;
        discovering_ = false;
        setBusy(false);
        emit candidatesChanged();
    });
    refresh();
    refreshAutomation();
}

QString AppController::centralPath() const { return tilde(store_.central()); }
QString AppController::version() const { return QStringLiteral(SKILLHUB_VERSION); }

void AppController::setBusy(bool busy, const QString &text, double progress) {
    busy_ = busy;
    busyText_ = busy ? text : QString();
    progress_ = busy ? progress : -1;
    emit busyChanged();
}

void AppController::setThemeMode(const QString &m) {
    if (m == themeMode_) return;
    themeMode_ = m;
    QSettings().setValue(QStringLiteral("theme"), m);
    emit themeChanged();
}

void AppController::setReduceMotion(bool on) {
    if (on == reduce_) return;
    reduce_ = on;
    QSettings().setValue(QStringLiteral("reduceMotion"), on);
    emit themeChanged();
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
    selected_ = name;
    if (mode_ != QLatin1String("skill")) setDetailMode(QStringLiteral("skill"));
    rebuildDetail();
}

void AppController::dismiss() {
    const bool hadAdd = mode_ == QLatin1String("add");
    if (hadAdd) { closeAdd(); }
    selected_.clear();
    focus_.clear();
    mode_ = QStringLiteral("skill");
    rebuildDetail();
    emit focusChanged();
    emit detailModeChanged();
}

void AppController::closeAdd() {
    candidates_.clear();
    pending_.clear();
    candidateError_.clear();
    candidateRepo_.clear();
    emit candidatesChanged();
    setDetailMode(focus_.isEmpty() ? QStringLiteral("skill") : QStringLiteral("agent"));
}

void AppController::refresh() {
    if (!busy_) store_.reloadProviders();  // agents may have been installed or removed since the last scan
    applyScan(model_.skills());
}

void AppController::applyScan(const QVector<Skill> &verdicts) {
    QVector<Skill> fresh = store_.scan();
    QHash<QString, const Skill *> v;
    for (const Skill &o : verdicts) v.insert(o.name, &o);
    for (Skill &s : fresh) {
        const auto it = v.constFind(s.name);
        if (it == v.constEnd() || !s.src.tracked() || (*it)->update == Skill::Untracked || (*it)->src.hash != s.src.hash) continue;
        s.update = (*it)->update;
        s.newHash = (*it)->newHash;
        s.newPath = (*it)->newPath;
        s.note = (*it)->note;
    }
    model_.setSkills(fresh, store_.agents(), justUpdated_);
    strays_ = store_.strays().size();
    wiringOk_ = store_.wiringOk();
    if (!selected_.isEmpty() && !model_.find(selected_)) selected_.clear();
    rebuildDerived();
    rebuildDetail();
    loaded_ = true;
    emit countsChanged();
}

void AppController::rebuildDerived() {
    counts_[0] = counts_[1] = counts_[2] = counts_[3] = 0;
    for (const Skill &s : model_.skills()) {
        if (s.update == Skill::Available) ++counts_[0];
        if (s.update == Skill::Untracked) ++counts_[1];
        if (SkillModel::hasProblem(s)) ++counts_[2];
        if (s.update == Skill::UpToDate || s.update == Skill::Available || s.update == Skill::Error) ++counts_[3];
    }
    // all managed agents; the list shows at most kMaxColumns columns, the rest fold into one overflow column
    constexpr int kMaxColumns = 6;
    agents_.clear();
    const auto &defs = store_.agents();
    int hiddenBad = 0, hiddenIssue = 0;
    QStringList hiddenNames;
    for (int i = 0; i < defs.size(); ++i) {
        const AgentDef &a = defs[i];
        int ok = 0, bad = 0;
        for (const Skill &s : model_.skills()) {
            const LinkState st = s.agents.value(a.id, LinkState::Missing);
            if (badLink(st)) ++bad; else ++ok;
        }
        const bool wired = store_.providerWired(a);
        const QString health = (bad > 0 || !wired) ? QStringLiteral("issue") : QStringLiteral("ok");
        agents_ << QVariantMap{{QStringLiteral("id"), a.id},
                               {QStringLiteral("label"), a.label},
                               {QStringLiteral("short"), shortLabel(a.label)},
                               {QStringLiteral("monogram"), monogram(a.label)},
                               {QStringLiteral("icon"), iconOf(a.id)},
                               {QStringLiteral("mode"), a.mode},
                               {QStringLiteral("mechanism"), mechanismOf(a)},
                               {QStringLiteral("folder"), tilde(a.mode == QLatin1String("link") ? a.dir : a.mode == QLatin1String("config") ? a.configFile : a.dir)},
                               {QStringLiteral("detected"), a.detected},
                               {QStringLiteral("detectedBy"), a.detectedBy},
                               {QStringLiteral("wired"), wired},
                               {QStringLiteral("ok"), ok},
                               {QStringLiteral("problems"), bad},
                               {QStringLiteral("total"), model_.rowCount()},
                               {QStringLiteral("health"), health}};
        if (defs.size() > kMaxColumns && i >= kMaxColumns - 1) {
            hiddenBad += bad;
            if (health == QLatin1String("issue")) ++hiddenIssue;
            hiddenNames << a.label;
        }
    }
    hidden_ = defs.size() > kMaxColumns ? defs.size() - (kMaxColumns - 1) : 0;
    columns_ = hidden_ > 0 ? kMaxColumns : defs.size();
    portW_ = columns_ <= 0 ? 72 : qBound(56, 360 / columns_, 72);
    overflow_ = hidden_ > 0 ? QVariantMap{{QStringLiteral("count"), hidden_},
                                          {QStringLiteral("problems"), hiddenBad},
                                          {QStringLiteral("health"), hiddenIssue > 0 ? QStringLiteral("issue") : QStringLiteral("ok")},
                                          {QStringLiteral("names"), hiddenNames.join(QStringLiteral(", "))}}
                            : QVariantMap();

    providerList_.clear();
    for (const AgentDef &a : store_.providers()) {
        providerList_ << QVariantMap{{QStringLiteral("id"), a.id},
                                     {QStringLiteral("label"), a.label},
                                     {QStringLiteral("monogram"), monogram(a.label)},
                                     {QStringLiteral("icon"), iconOf(a.id)},
                                     {QStringLiteral("mode"), a.mode},
                                     {QStringLiteral("mechanism"), mechanismOf(a)},
                                     {QStringLiteral("folder"), tilde(a.mode == QLatin1String("config") ? a.configFile : a.dir)},
                                     {QStringLiteral("detected"), a.detected},
                                     {QStringLiteral("detectedBy"), a.detectedBy},
                                     {QStringLiteral("managed"), a.managed},
                                     {QStringLiteral("custom"), a.custom}};
    }
    emit agentsChanged();
}

void AppController::rescanProviders() {
    if (busy_) return;
    refresh();
    emit toast(QStringLiteral("ok"), QStringLiteral("%1 agents managed, %2 known").arg(store_.agents().size()).arg(store_.providers().size()));
}

void AppController::setProviderManaged(const QString &id, bool on) {
    if (busy_) return;
    QString err;
    if (!store_.setProviderManaged(id, on, &err)) {
        emit toast(QStringLiteral("error"), err);
        return;
    }
    store_.syncWatcher();
    // switching an agent on links the skills into it; switching it off leaves what is there untouched
    if (on && !busy_) adopt();
    else refresh();
}

QString AppController::addCustomProvider(const QString &label, const QString &dir, bool readsShared) {
    if (busy_) return QStringLiteral("busy");
    QString err;
    const QString id = store_.addCustomProvider(label, dir, readsShared ? QStringLiteral("native") : QStringLiteral("link"), &err);
    if (id.isEmpty()) {
        emit toast(QStringLiteral("error"), err);
        return err;
    }
    store_.syncWatcher();
    emit toast(QStringLiteral("ok"), QStringLiteral("Added %1").arg(label.trimmed()));
    if (!busy_) adopt();
    else refresh();
    return QString();
}

void AppController::removeCustomProvider(const QString &id) {
    if (busy_) return;
    if (!store_.removeCustomProvider(id)) return;
    if (focus_ == id) focus_.clear();
    store_.syncWatcher();
    refresh();
}

QVariantMap AppController::agentDetail() const {
    for (const QVariant &v : agents_) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("id")).toString() != focus_) continue;
        QVariantMap out = m;
        QStringList issues;
        for (const Skill &s : model_.skills())
            if (badLink(s.agents.value(focus_, LinkState::Missing))) issues << s.name;
        out.insert(QStringLiteral("issues"), issues);
        return out;
    }
    return {};
}

void AppController::rebuildDetail() {
    detail_.clear();
    preview_.clear();
    const Skill *s = selected_.isEmpty() ? nullptr : model_.find(selected_);
    if (s) {
        const QString state = SkillModel::stateKey(*s, justUpdated_.contains(s->name) && s->update == Skill::UpToDate);
        QVariantList links;
        for (const AgentDef &a : store_.agents()) {
            const LinkState ls = s->agents.value(a.id, LinkState::Missing);
            links << QVariantMap{{QStringLiteral("label"), a.label}, {QStringLiteral("state"), linkKey(ls)}};
        }
        detail_ = QVariantMap{{QStringLiteral("name"), s->name},
                              {QStringLiteral("description"), s->description},
                              {QStringLiteral("state"), state},
                              {QStringLiteral("note"), s->note},
                              {QStringLiteral("path"), tilde(s->path)},
                              {QStringLiteral("source"), s->src.source},
                              {QStringLiteral("upstreamPath"), s->src.skillPath},
                              {QStringLiteral("hash"), s->src.hash.left(8)},
                              {QStringLiteral("installed"), dateOf(s->src.installedAt)},
                              {QStringLiteral("updated"), dateOf(s->src.updatedAt)},
                              {QStringLiteral("tracked"), s->src.tracked()},
                              {QStringLiteral("links"), links}};
        QFile f(s->path + QStringLiteral("/SKILL.md"));
        if (f.open(QIODevice::ReadOnly)) preview_ = stripFrontmatter(QString::fromUtf8(f.read(80 * 1024)));
    }
    emit selectedChanged();
}

bool AppController::automationSupported() const { return platform::automationSupported(); }

void AppController::refreshAutomation() {
    store_.syncWatcher();  // agents may have come or gone since the watcher unit was written
    timer_ = store_.unitState(QStringLiteral("skillhub-update.timer")) == QLatin1String("enabled") ? QStringLiteral("on") : QStringLiteral("off");
    watch_ = store_.unitState(QStringLiteral("skillhub-adopt.path")) == QLatin1String("enabled") ? QStringLiteral("on") : QStringLiteral("off");
    emit automationChanged();
}

void AppController::setTimer(bool on) {
    QString err;
    const bool ok = on ? store_.installUnits(true, false, &err) : store_.removeUnits(true, false, &err);
    if (!ok) emit toast(QStringLiteral("error"), err);
    else emit toast(QStringLiteral("ok"), on ? QStringLiteral("Daily update timer enabled") : QStringLiteral("Daily update timer removed"));
    refreshAutomation();
}

void AppController::setWatcher(bool on) {
    QString err;
    const bool ok = on ? store_.installUnits(false, true, &err) : store_.removeUnits(false, true, &err);
    if (!ok) emit toast(QStringLiteral("error"), err);
    else emit toast(QStringLiteral("ok"), on ? QStringLiteral("Stray-skill watcher enabled") : QStringLiteral("Stray-skill watcher removed"));
    refreshAutomation();
}

void AppController::openFolder() { QDesktopServices::openUrl(QUrl::fromLocalFile(store_.central())); }
void AppController::copyText(const QString &text) { QGuiApplication::clipboard()->setText(text); emit toast(QStringLiteral("ok"), QStringLiteral("Copied")); }

// ---- jobs ------------------------------------------------------------------------------------------------------------------

void AppController::run(const QString &what, Job job, bool determinate, const QString &doneText) {
    if (busy_) return;
    doneText_ = doneText;
    jobUpdated_.clear();
    setBusy(true, what, determinate ? 0.0 : -1.0);
    const QVector<Skill> copy = model_.skills();
    Store *st = &store_;
    AppController *self = this;
    jobWatcher_.setFuture(QtConcurrent::run([st, job, copy, self, determinate]() {
        const Progress p = [self, determinate](double v) {
            if (!determinate) return;
            QMetaObject::invokeMethod(self, [self, v] { self->progress_ = v; emit self->busyChanged(); }, Qt::QueuedConnection);
        };
        return job(*st, copy, p);
    }));
}

void AppController::finished() {
    const QVector<Skill> result = jobWatcher_.result();
    for (const QString &n : jobUpdated_) justUpdated_.insert(n);
    setBusy(false);
    applyScan(result.isEmpty() ? model_.skills() : result);
    int avail = 0;
    for (const Skill &s : model_.skills()) avail += s.update == Skill::Available;
    QString msg, level = QStringLiteral("ok");
    if (doneText_ == QLatin1String("check") || doneText_ == QLatin1String("check-quiet")) {
        if (avail) {
            msg = QStringLiteral("%1 update%2 available").arg(avail).arg(avail == 1 ? "" : "s");
            level = QStringLiteral("warn");
        } else if (doneText_ == QLatin1String("check")) {
            msg = counts_[3] > 0 ? QStringLiteral("Every tracked skill is current") : QStringLiteral("No skill has an update source yet");
        }
    } else if (doneText_ == QLatin1String("update")) {
        msg = jobUpdated_.isEmpty() ? QStringLiteral("Nothing to update") : QStringLiteral("Updated %1 skill%2").arg(jobUpdated_.size()).arg(jobUpdated_.size() == 1 ? "" : "s");
    } else if (doneText_ == QLatin1String("import")) {
        msg = QStringLiteral("Installed %1 skill%2").arg(importOk_).arg(importOk_ == 1 ? "" : "s") +
              (importFailed_ ? QStringLiteral(", %1 failed (see Activity)").arg(importFailed_) : QString());
        level = importFailed_ ? QStringLiteral("warn") : QStringLiteral("ok");
    } else if (!doneText_.isEmpty()) {
        msg = doneText_;
    }
    int failed = 0;
    QString firstNote;
    for (const Skill &s : model_.skills())
        if (s.update == Skill::Error) { ++failed; if (firstNote.isEmpty()) firstNote = s.name + QStringLiteral(": ") + s.note; }
    if (failed > 0) {
        msg = (msg.isEmpty() ? QString() : msg + QStringLiteral(". ")) + QStringLiteral("%1 skill%2 could not be checked (%3)").arg(failed).arg(failed == 1 ? "" : "s").arg(firstNote);
        level = QStringLiteral("error");
    }
    if (!msg.isEmpty()) emit toast(level, msg);
}

void AppController::checkUpdates() {
    justUpdated_.clear();
    run(QStringLiteral("Checking upstream"), [](Store &st, QVector<Skill> sk, const Progress &) {
        st.checkUpdates(sk);
        return sk;
    }, false, QStringLiteral("check"));
}

void AppController::checkUpdatesQuietly() {
    justUpdated_.clear();
    run(QStringLiteral("Checking upstream"), [](Store &st, QVector<Skill> sk, const Progress &) {
        st.checkUpdates(sk);
        return sk;
    }, false, QStringLiteral("check-quiet"));
}

void AppController::updateAll() {
    AppController *self = this;
    run(QStringLiteral("Updating skills"), [self](Store &st, QVector<Skill> sk, const Progress &p) {
        st.checkUpdates(sk);
        int total = 0, done = 0;
        for (const Skill &s : sk) total += s.update == Skill::Available;
        for (Skill &s : sk) {
            if (s.update != Skill::Available) continue;
            if (st.applyUpdate(s)) self->jobUpdated_ << s.name;
            p(double(++done) / total);
        }
        return sk;
    }, true, QStringLiteral("update"));
}

void AppController::updateSkill(const QString &name) {
    AppController *self = this;
    run(QStringLiteral("Updating ") + name, [self, name](Store &st, QVector<Skill> sk, const Progress &) {
        st.checkUpdates(sk, {name});
        for (Skill &s : sk)
            if (s.name == name && s.update == Skill::Available && st.applyUpdate(s)) self->jobUpdated_ << name;
        return sk;
    }, false, QStringLiteral("update"));
}

void AppController::adopt() {
    run(QStringLiteral("Adopting strays and repairing links"), [](Store &st, QVector<Skill> sk, const Progress &) {
        st.adopt();
        return sk;
    }, false, QStringLiteral("Links repaired"));
}

void AppController::removeSkill(const QString &name) {
    if (busy_) return;
    if (selected_ == name) selected_.clear();
    run(QStringLiteral("Removing ") + name, [name](Store &st, QVector<Skill> sk, const Progress &) {
        st.removeSkill(name);
        return sk;
    }, false, QStringLiteral("Removed %1 (backup kept)").arg(name));
}

void AppController::trackSkill(const QString &name, const QString &repo) {
    AppController *self = this;
    run(QStringLiteral("Linking source"), [self, name, repo](Store &st, QVector<Skill> sk, const Progress &) {
        QString err;
        if (!st.track(name, repo, {}, &err)) {
            QMetaObject::invokeMethod(self, [self, err] { emit self->toast(QStringLiteral("error"), err); }, Qt::QueuedConnection);
            return sk;
        }
        QVector<Skill> fresh = st.scan();
        st.checkUpdates(fresh, {name});
        return fresh;
    }, false);
}

void AppController::discover(const QString &repo) {
    if (busy_) return;
    candidateRepo_ = repo.trimmed();
    candidateError_.clear();
    candidates_.clear();
    discovering_ = true;
    setBusy(true, QStringLiteral("Reading ") + candidateRepo_);
    emit candidatesChanged();
    Store *st = &store_;
    const QString r = candidateRepo_;
    discoverWatcher_.setFuture(QtConcurrent::run([st, r]() {
        DiscoverResult out;
        out.items = st->discover(r, &out.error);
        return out;
    }));
}

void AppController::installChosen(const QStringList &names) {
    if (busy_) return;
    const QVector<Candidate> chosen = [&] {
        QVector<Candidate> v;
        for (const Candidate &c : pending_)
            if (names.contains(c.name)) v << c;
        return v;
    }();
    if (chosen.isEmpty()) return;
    const QString first = chosen.first().name;
    run(QStringLiteral("Installing skills"), [chosen](Store &st, QVector<Skill> sk, const Progress &p) {
        int done = 0;
        for (const Candidate &c : chosen) {
            st.install(c);
            p(double(++done) / chosen.size());
        }
        return sk;
    }, true, QStringLiteral("Installed %1 skill%2").arg(chosen.size()).arg(chosen.size() == 1 ? "" : "s"));
    closeAdd();
    selected_ = first;
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

namespace {
QString cleanUserPath(QString p) {
    p = p.trimmed();
    if (p.startsWith(QLatin1String("file://"))) p = QUrl(p).toLocalFile();
    if (p == QLatin1String("~") || p.startsWith(QLatin1String("~/"))) p = platform::homeDir() + p.mid(1);
    return p;
}
}  // namespace

QVariantMap AppController::exportInfo() const {
    int tracked = 0, local = 0;
    for (const Skill &s : model_.skills()) (s.src.tracked() ? tracked : local)++;
    return {{QStringLiteral("tracked"), tracked}, {QStringLiteral("local"), local}};
}

QString AppController::documentsFolder() const {
    const QString d = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return d.isEmpty() ? platform::homeDir() : d;
}

QString AppController::defaultExportPath() const {
    return tilde(documentsFolder()) + QStringLiteral("/skillhub-skills-") + QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd")) + QStringLiteral(".json");
}

QString AppController::exportTo(const QString &pathIn, bool includeLocal) {
    QString path = cleanUserPath(pathIn);
    if (path.isEmpty()) return QStringLiteral("choose where to save");
    if (QFileInfo(path).isDir()) path += QStringLiteral("/skillhub-skills-") + QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd")) + QStringLiteral(".json");
    int tracked = 0, local = 0;
    QStringList skipped;
    QString err;
    if (!store_.writeExport(path, includeLocal, &tracked, &local, &skipped, &err)) {
        emit toast(QStringLiteral("error"), err);
        return err;
    }
    for (const QString &k : skipped) store_.log(QStringLiteral("not exported: ") + k);
    emit toast(skipped.isEmpty() ? QStringLiteral("ok") : QStringLiteral("warn"),
               QStringLiteral("Exported %1 skills").arg(tracked + local) + (skipped.isEmpty() ? QString() : QStringLiteral(", %1 skipped (see Activity)").arg(skipped.size())));
    store_.log(QStringLiteral("exported %1 skills (%2 from GitHub, %3 local) to %4").arg(tracked + local).arg(tracked).arg(local).arg(path));
    return QString();
}

void AppController::importPreview(const QString &pathIn) {
    importPending_.clear();
    importItems_.clear();
    importError_.clear();
    QString err;
    const QVector<ImportItem> items = store_.readExport(cleanUserPath(pathIn), &err);
    importError_ = err;
    for (const ImportItem &it : items) {
        const bool installable = it.problem.isEmpty() && !it.exists;
        if (installable) importPending_.push_back(it);
        importItems_ << QVariantMap{{QStringLiteral("name"), it.name},
                                    {QStringLiteral("description"), it.description},
                                    {QStringLiteral("local"), it.local},
                                    {QStringLiteral("source"), it.source},
                                    {QStringLiteral("files"), int(it.files.size())},
                                    {QStringLiteral("exists"), it.exists},
                                    {QStringLiteral("problem"), it.problem},
                                    {QStringLiteral("installable"), installable}};
    }
    if (items.isEmpty() && importError_.isEmpty()) importError_ = QStringLiteral("There are no skills in that file.");
    setDetailMode(QStringLiteral("transfer"));
    emit importChanged();
}

void AppController::importClear() {
    importPending_.clear();
    importItems_.clear();
    importError_.clear();
    emit importChanged();
}

void AppController::importInstall(const QStringList &names) {
    if (busy_) return;
    QVector<ImportItem> chosen;
    for (const ImportItem &it : importPending_)
        if (names.contains(it.name)) chosen << it;
    if (chosen.isEmpty()) return;
    importOk_ = importFailed_ = 0;
    AppController *self = this;
    run(QStringLiteral("Installing from the export"), [self, chosen](Store &st, QVector<Skill> sk, const Progress &p) {
        int done = 0;
        for (const ImportItem &it : chosen) {
            QString e;
            if (st.importItem(it, &e)) ++self->importOk_; else ++self->importFailed_;
            p(double(++done) / chosen.size());
        }
        st.relink();
        return sk;
    }, true, QStringLiteral("import"));
    importClear();
}
