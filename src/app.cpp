#include "app.h"

#include "gui/terminal/term_look.h"
#include "gui/window_effects.h"
#include "sh_palette.h"

#ifdef SKILLHUB_HAVE_DBUS
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#endif

#include <QClipboard>
#include <QDate>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocale>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QPalette>
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
        scope_.clear();  // a new backend starts on the user's own skills
        scopeCentral_.clear();
        emit scopeChanged();
        emit toast(QStringLiteral("warn"), tr("The background service restarted"));
    }
    loadProjects();
    loadSettings();
    watchPortalAccent();
    core_.call(QStringLiteral("system.info"), {}, [this](const CoreReply &r) {
        if (!r.ok()) return;
        shells_ = r.result.toObject().value(QStringLiteral("shells")).toArray().toVariantList();
        emit countsChanged();
    });
    refreshAutomation();
    loadSnapshot();
    emit countsChanged();  // centralPath is known now
}

QString AppController::tilde(const QString &path) const {
    const QString home = core_.init().value(QStringLiteral("paths")).toObject().value(QStringLiteral("home")).toString();
    if (!home.isEmpty() && (path == home || path.startsWith(home + QLatin1Char('/')))) return QStringLiteral("~") + path.mid(home.size());
    return path;
}

QString AppController::scopeLabel() const {
    for (const QVariant &v : projects_)
        if (v.toMap().value(QStringLiteral("path")).toString() == scope_) return v.toMap().value(QStringLiteral("name")).toString();
    return scope_.isEmpty() ? tr("Your skills") : scope_.section(QLatin1Char('/'), -1);
}

void AppController::loadProjects() {
    core_.call(QStringLiteral("projects.list"), {}, [this](const CoreReply &r) {
        if (!r.ok()) return;
        const QJsonObject o = r.result.toObject();
        projects_ = o.value(QStringLiteral("projects")).toArray().toVariantList();
        suggested_.clear();
        for (const QJsonValue &v : o.value(QStringLiteral("suggested")).toArray()) suggested_ << v.toString();
        emit projectsChanged();
        emit scopeChanged();  // the label is the project's name
    });
}

void AppController::setScope(const QString &path) {
    if (busy_ || path == scope_) return;
    core_.call(QStringLiteral("scope.set"), {{QStringLiteral("scope"), path.isEmpty() ? QStringLiteral("user") : path}}, [this, path](const CoreReply &r) {
        if (!r.ok()) {
            emit toast(QStringLiteral("error"), r.message());
            return;
        }
        scope_ = path;
        scopeCentral_ = path.isEmpty() ? QString() : r.result.toObject().value(QStringLiteral("central")).toString();
        select(QString());
        setDetailMode(QStringLiteral("skill"));
        emit scopeChanged();
        emit countsChanged();
    });
}

void AppController::removeProject(const QString &path) {
    core_.call(QStringLiteral("projects.remove"), {{QStringLiteral("path"), path}}, [this, path](const CoreReply &r) {
        if (!r.ok()) emit toast(QStringLiteral("error"), r.message());
        if (scope_ == path) setScope(QString());
        loadProjects();
    });
}

void AppController::setProjectAuto(const QString &path, bool on) {
    core_.call(QStringLiteral("projects.setAuto"), {{QStringLiteral("path"), path}, {QStringLiteral("on"), on}}, [this](const CoreReply &r) {
        if (!r.ok()) emit toast(QStringLiteral("error"), r.message());
        loadProjects();
    });
}

// ---- terminal ----

QString AppController::terminalShell() const {
    const QString chosen = setting(QStringLiteral("terminal"), QStringLiteral("shell"), QString()).toString();
    if (!chosen.isEmpty() && QFile::exists(chosen)) return chosen;
#ifdef Q_OS_WIN
    for (const QString &name : {QStringLiteral("pwsh"), QStringLiteral("powershell"), QStringLiteral("cmd")}) {
        const QString found = QStandardPaths::findExecutable(name);
        if (!found.isEmpty()) return found;
    }
    return qEnvironmentVariable("ComSpec");
#else
    const QString shell = qEnvironmentVariable("SHELL");
    return !shell.isEmpty() && QFile::exists(shell) ? shell : QStringLiteral("/bin/sh");
#endif
}

static QStringList bannerArguments(const QString &shell);

QStringList AppController::terminalArguments() const {
#ifdef Q_OS_WIN
    return setting(QStringLiteral("terminal"), QStringLiteral("banner"), true).toBool() ? bannerArguments(terminalShell()) : QStringList{};
#else
    const QString name = terminalShell().section(QLatin1Char('/'), -1);
    return name == QLatin1String("sh") || name == QLatin1String("dash") ? QStringList{} : QStringList{QStringLiteral("-l")};
#endif
}

QString AppController::terminalFolder() const {
    if (!scope_.isEmpty()) return scope_;
    return core_.init().value(QStringLiteral("paths")).toObject().value(QStringLiteral("home")).toString();
}

QVariantMap AppController::terminalLook() const { return importTerminalLook(setting(QStringLiteral("terminal"), QStringLiteral("importLook"), QString()).toString()); }

void AppController::requestBanner() {
    const QJsonObject params{{QStringLiteral("colors"), QStringLiteral("truecolor")},
                             {QStringLiteral("icons"), setting(QStringLiteral("terminal"), QStringLiteral("icons"), QStringLiteral("unicode")).toString()},
                             {QStringLiteral("logo"), setting(QStringLiteral("terminal"), QStringLiteral("logo"), QStringLiteral("builtin")).toString()}};
    core_.call(QStringLiteral("fetch.render"), params, [this](const CoreReply &r) {
        QString text = r.ok() ? r.result.toObject().value(QStringLiteral("ansi")).toString() : QString();
        emit bannerReady(text.replace(QLatin1Char('\n'), QLatin1String("\r\n")));
    });
}

static const struct { const char *id, *label, *program; } kTerminals[] = {
    {"kitty", "kitty", "kitty"}, {"ghostty", "Ghostty", "ghostty"}, {"alacritty", "Alacritty", "alacritty"}, {"wezterm", "WezTerm", "wezterm"},
    {"foot", "foot", "foot"}, {"konsole", "Konsole", "konsole"}, {"gnome-terminal", "GNOME Terminal", "gnome-terminal"}, {"xterm", "xterm", "xterm"},
    {"wt", "Windows Terminal", "wt"},
};

QVariantList AppController::externalTerminals() const {
    QVariantList out;
    for (const auto &t : kTerminals)
        if (!QStandardPaths::findExecutable(QString::fromLatin1(t.program)).isEmpty()) out << QVariantMap{{QStringLiteral("id"), QString::fromLatin1(t.id)}, {QStringLiteral("label"), QString::fromLatin1(t.label)}};
#ifdef Q_OS_MACOS
    out << QVariantMap{{QStringLiteral("id"), QStringLiteral("terminal-app")}, {QStringLiteral("label"), QStringLiteral("Terminal")}};
#endif
    return out;
}

// what a shell runs to show the banner and stay open
static QStringList bannerArguments(const QString &shell) {
    const QString name = shell.section(QLatin1Char('/'), -1).section(QLatin1Char('\\'), -1).toLower();
#ifdef Q_OS_WIN
    const QString fetch = QStringLiteral("skillhub-cli fetch");  // on PATH, see main(): no shell agrees on how to quote a path
#else
    const QString fetch = QStringLiteral("skillhub fetch");
#endif
    if (name.startsWith(QLatin1String("fish"))) return {QStringLiteral("-C"), fetch};
    if (name.startsWith(QLatin1String("nu"))) return {QStringLiteral("-e"), fetch};
    if (name.startsWith(QLatin1String("pwsh")) || name.startsWith(QLatin1String("powershell"))) return {QStringLiteral("-NoExit"), QStringLiteral("-Command"), fetch};
    if (name.startsWith(QLatin1String("cmd"))) return {QStringLiteral("/K"), fetch};
    return {QStringLiteral("-c"), fetch + QStringLiteral("; exec ") + shell + QStringLiteral(" -l")};
}

void AppController::openExternalTerminal(const QString &id) {
    const QString shell = terminalShell(), dir = terminalFolder();
    const QStringList run = QStringList{shell} + bannerArguments(shell);
    const QString custom = setting(QStringLiteral("terminal"), QStringLiteral("externalTemplate"), QString()).toString();
    QString program;
    QStringList args;
    if (!custom.isEmpty()) {
        args = QProcess::splitCommand(custom);
        for (QString &a : args) a.replace(QLatin1String("{dir}"), dir).replace(QLatin1String("{shell}"), shell);
        if (!args.isEmpty()) program = args.takeFirst();
    } else if (id == QLatin1String("kitty")) {
        program = id;
        args = QStringList{QStringLiteral("--directory"), dir} + run;
    } else if (id == QLatin1String("ghostty")) {
        program = id;
        args = QStringList{QStringLiteral("--working-directory=") + dir, QStringLiteral("-e")} + run;
    } else if (id == QLatin1String("alacritty")) {
        program = id;
        args = QStringList{QStringLiteral("--working-directory"), dir, QStringLiteral("-e")} + run;
    } else if (id == QLatin1String("wezterm")) {
        program = id;
        args = QStringList{QStringLiteral("start"), QStringLiteral("--cwd"), dir, QStringLiteral("--")} + run;
    } else if (id == QLatin1String("foot")) {
        program = id;
        args = QStringList{QStringLiteral("-D"), dir} + run;
    } else if (id == QLatin1String("konsole")) {
        program = id;
        args = QStringList{QStringLiteral("--workdir"), dir, QStringLiteral("-e")} + run;
    } else if (id == QLatin1String("gnome-terminal")) {
        program = id;
        args = QStringList{QStringLiteral("--working-directory=") + dir, QStringLiteral("--")} + run;
    } else if (id == QLatin1String("xterm")) {
        program = id;
        args = QStringList{QStringLiteral("-e")} + run;
    } else if (id == QLatin1String("wt")) {
        program = id;
        args = QStringList{QStringLiteral("-d"), dir} + run;
    } else if (id == QLatin1String("terminal-app")) {
        program = QStringLiteral("open");
        args = QStringList{QStringLiteral("-a"), QStringLiteral("Terminal"), dir};
    }
    if (program.isEmpty() || !QProcess::startDetached(program, args, dir)) emit toast(QStringLiteral("error"), tr("Could not start %1").arg(id));
}

// window state, not a setting: this machine has seen the tour of this version
bool AppController::tourNeeded() const { return !QSettings().contains(QStringLiteral("tour/completedVersion")); }

void AppController::tourDone() { QSettings().setValue(QStringLiteral("tour/completedVersion"), QStringLiteral(SKILLHUB_VERSION)); }

void AppController::mergeLocks() { startJob(QStringLiteral("lock.merge"), {}, tr("Merging the lock files"), QStringLiteral("mergeLocks"), false); }

void AppController::measurePaths() { startJob(QStringLiteral("paths.sizes"), {}, tr("Measuring folders"), QStringLiteral("sizes"), false); }

void AppController::clearCache() { startJob(QStringLiteral("cache.clear"), {}, tr("Clearing the cache"), QStringLiteral("clearCache"), false); }

void AppController::restartBackend() {
    if (busy_) return;
    core_.stop();
    core_.start();
}

QString AppController::backendInfo() const {
    const QJsonObject init = core_.init();
    return tr("%1, process %2").arg(init.value(QStringLiteral("version")).toString()).arg(init.value(QStringLiteral("pid")).toInt());
}

QString AppController::materialNote() const {
    return window_effects::note(setting(QStringLiteral("appearance"), QStringLiteral("material"), QStringLiteral("solid")).toString());
}

QString AppController::material() const {
    if (setting(QStringLiteral("appearance"), QStringLiteral("reduceTransparency"), false).toBool() || !window_effects::canBeTranslucent()) return QStringLiteral("solid");
    return setting(QStringLiteral("appearance"), QStringLiteral("material"), QStringLiteral("solid")).toString();
}

bool AppController::glassReady() const {
#ifdef SKILLHUB_HAVE_SHADERS
    return !forcedStill_ && QQuickWindow::graphicsApi() != QSGRendererInterface::Software;
#else
    return false;
#endif
}

void AppController::applyMaterial(QQuickWindow *window, bool dark) { window_effects::apply(window, material(), dark); }

void AppController::addProject(const QString &path) {
    core_.call(QStringLiteral("projects.add"), {{QStringLiteral("path"), path}}, [this, path](const CoreReply &r) {
        if (!r.ok()) {
            emit toast(QStringLiteral("error"), r.message());
            return;
        }
        loadProjects();
        setScope(path);
    });
}

void AppController::copyToProject(const QString &name, const QString &project) {
    startJob(QStringLiteral("skills.copyToProject"), {{QStringLiteral("name"), name}, {QStringLiteral("project"), project}}, tr("Copying %1").arg(name), QStringLiteral("copy"), false);
}

QString AppController::centralPath() const {
    if (!scopeCentral_.isEmpty()) return tilde(scopeCentral_);
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
    } else if (method == QLatin1String("lock.split")) {
        lockSplit_ = params.value(QStringLiteral("onlyInOther")).toInt();
        emit lockSplitChanged();
    } else if (method == QLatin1String("settings.changed")) {
        applySettings(params.value(QStringLiteral("settings")).toObject());
    }
}

// ---- settings ---------------------------------------------------------------------------------------------------------

// From the backend's settings.json. The first 2.0 start takes the theme and motion settings over from 1.x's QSettings.
void AppController::loadSettings() {
    core_.call(QStringLiteral("settings.get"), {}, [this](const CoreReply &r) {
        if (!r.ok()) return;
        QJsonObject s = r.result.toObject();
        QJsonObject look = s.value(QStringLiteral("appearance")).toObject();
        // first start of 2.0: from 1.x's QSettings. A 2.0 test build kept these two at the top level, so those count too.
        QSettings old;
        bool moved = false;
        for (const QString &key : {QStringLiteral("theme"), QStringLiteral("reduceMotion")}) {
            if (look.contains(key)) continue;
            if (s.contains(key)) look.insert(key, s.value(key));
            else if (old.contains(key)) look.insert(key, QJsonValue::fromVariant(old.value(key)));
            else continue;
            moved = true;
        }
        if (moved) {
            s.insert(QStringLiteral("appearance"), look);
            core_.call(QStringLiteral("settings.set"), {{QStringLiteral("patch"), QJsonObject{{QStringLiteral("appearance"), look}}}});
        }
        applySettings(s);
    });
}

void AppController::applySettings(const QJsonObject &s) {
    settings_ = s;
    const QJsonObject look = s.value(QStringLiteral("appearance")).toObject();
    if (!themeForced_) themeMode_ = look.value(QStringLiteral("theme")).toString(QStringLiteral("system"));
    reduce_ = look.value(QStringLiteral("reduceMotion")).toBool();
    const QJsonObject accent = look.value(QStringLiteral("accent")).toObject();
    const QString source = accent.value(QStringLiteral("source")).toString(QStringLiteral("default"));
    const QString seed = source == QLatin1String("custom") ? accent.value(QStringLiteral("color")).toString() : source == QLatin1String("system") ? systemAccent() : QString();
    paletteDark_ = derivePalette(seed, true);
    paletteLight_ = derivePalette(seed, false);
    ++settingsRev_;
    emit settingsChanged();
    emit themeChanged();
}

QVariant AppController::setting(const QString &section, const QString &key, const QVariant &fallback) const {
    const QJsonValue v = settings_.value(section).toObject().value(key);
    return v.isUndefined() ? fallback : v.toVariant();
}

void AppController::setSetting(const QString &section, const QString &key, const QVariant &value) {
    QJsonObject part = settings_.value(section).toObject();
    part.insert(key, QJsonValue::fromVariant(value));
    QJsonObject next = settings_;
    next.insert(section, part);
    applySettings(next);
    core_.call(QStringLiteral("settings.set"), {{QStringLiteral("patch"), QJsonObject{{section, part}}}});
    if (section == QLatin1String("appearance") && key == QLatin1String("uiScale")) QSettings().setValue(QStringLiteral("ui/scale"), value);  // read before the window exists
}

void AppController::setThemeMode(const QString &m) { setSetting(QStringLiteral("appearance"), QStringLiteral("theme"), m); }

void AppController::setReduceMotion(bool on) { setSetting(QStringLiteral("appearance"), QStringLiteral("reduceMotion"), on); }

bool AppController::compactDensity() const { return setting(QStringLiteral("appearance"), QStringLiteral("density"), QString()).toString() == QLatin1String("compact"); }

QString AppController::systemAccent() const {
    if (!portalAccent_.isEmpty()) return portalAccent_;
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    const QColor c = QGuiApplication::palette().color(QPalette::Accent);
    if (c.isValid()) return c.name().toUpper();
#endif
    return QString();
}

// Linux desktops tell their accent through the settings portal, as a colour in three doubles
void AppController::watchPortalAccent() {
#ifdef SKILLHUB_HAVE_DBUS
    static const QString service = QStringLiteral("org.freedesktop.portal.Desktop"), path = QStringLiteral("/org/freedesktop/portal/desktop"),
                         iface = QStringLiteral("org.freedesktop.portal.Settings");
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return;
    bus.connect(service, path, iface, QStringLiteral("SettingChanged"), this, SLOT(onPortalSetting(QString, QString, QDBusVariant)));
    QDBusMessage read = QDBusMessage::createMethodCall(service, path, iface, QStringLiteral("Read"));
    read << QStringLiteral("org.freedesktop.appearance") << QStringLiteral("accent-color");
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(read, 2000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        QDBusPendingReply<QVariant> r = *w;
        w->deleteLater();
        if (!r.isError()) setPortalAccent(r.value());
    });
#endif
}

#ifdef SKILLHUB_HAVE_DBUS
void AppController::setPortalAccent(const QVariant &value) {
    QVariant v = value;
    while (v.canConvert<QDBusVariant>() && v.typeId() == qMetaTypeId<QDBusVariant>()) v = v.value<QDBusVariant>().variant();
    if (v.typeId() != qMetaTypeId<QDBusArgument>()) return;
    const QDBusArgument arg = v.value<QDBusArgument>();
    double r = -1, g = -1, b = -1;
    arg.beginStructure();
    arg >> r >> g >> b;
    arg.endStructure();
    if (r < 0 || r > 1 || g < 0 || g > 1 || b < 0 || b > 1) return;  // the portal's way of saying "no preference"
    portalAccent_ = QColor::fromRgbF(r, g, b).name().toUpper();
    applySettings(settings_);
}

void AppController::onPortalSetting(const QString &group, const QString &key, const QDBusVariant &value) {
    if (group == QLatin1String("org.freedesktop.appearance") && key == QLatin1String("accent-color")) setPortalAccent(value.variant());
}
#endif

QVariant AppController::oklch(const QString &hex) const {
    double l = 0, c = 0, h = 0;
    if (!sh_oklch_from_hex(hex.toUtf8().constData(), &l, &c, &h)) return QVariant();
    return QVariantMap{{QStringLiteral("l"), l}, {QStringLiteral("c"), c}, {QStringLiteral("h"), h}};
}

QString AppController::hexOf(double l, double c, double h) const {
    char out[8];
    sh_oklch_to_hex(l, c, h, out);
    return QString::fromLatin1(out);
}

double AppController::contrast(const QColor &a, const QColor &b) const {
    return sh_palette_contrast(a.name().toUpper().toLatin1().constData(), b.name().toUpper().toLatin1().constData());
}

QVariantMap AppController::derivePalette(const QString &seed, bool dark) const {
    sh_palette p;
    if (!sh_palette_derive(seed.toUtf8().constData(), dark, &p)) return {};
    return {{QStringLiteral("action"), QString::fromLatin1(p.action)},       {QStringLiteral("actionInk"), QString::fromLatin1(p.action_ink)},
            {QStringLiteral("mark"), QString::fromLatin1(p.mark)},           {QStringLiteral("markText"), QString::fromLatin1(p.mark_text)},
            {QStringLiteral("markLow"), QString::fromLatin1(p.mark_low)},    {QStringLiteral("focus"), QString::fromLatin1(p.focus)},
            {QStringLiteral("selected"), QString::fromLatin1(p.selected)},   {QStringLiteral("inkOk"), QString::fromLatin1(p.ink_ok)},
            {QStringLiteral("clash"), QString::fromLatin1(p.clash)}};
}

QVariant AppController::layoutState(const QString &key) const {
    QSettings s;
    const QVariant v = s.value(QStringLiteral("layout/") + key);
    return v.isValid() ? v : QVariant(false);
}

void AppController::saveLayout(const QString &key, const QVariant &state) {
    QSettings s;
    if (state.isValid()) s.setValue(QStringLiteral("layout/") + key, state);
    else s.remove(QStringLiteral("layout/") + key);
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
    if (changed) loadVersions();
}

// "2026-10-25T09:20:41.715Z", or a 1.x backup's 20261025-092041: the moment in local time
static QString whenOf(const QString &iso, const QString &stamp) {
    QDateTime t = QDateTime::fromString(iso, Qt::ISODateWithMs);
    if (!t.isValid()) t = QDateTime::fromString(stamp.left(15), QStringLiteral("yyyyMMdd-HHmmss"));
    return t.isValid() ? QLocale().toString(t.toLocalTime(), QLocale::ShortFormat) : stamp;
}

static QString versionLabel(const QString &reason) {
    if (reason == QLatin1String("update")) return AppController::tr("Before an update");
    if (reason == QLatin1String("rollback")) return AppController::tr("Before a rollback");
    if (reason == QLatin1String("install")) return AppController::tr("Before it was installed again");
    return AppController::tr("Backup");
}

void AppController::loadVersions() {
    const QString name = selected_;
    if (name.isEmpty()) {
        versions_.clear();
        onHold_ = false;
        emit versionsChanged();
        return;
    }
    core_.call(QStringLiteral("skills.history"), {{QStringLiteral("name"), name}}, [this, name](const CoreReply &r) {
        if (name != selected_ || !r.ok()) return;
        const QJsonObject o = r.result.toObject();
        versions_.clear();
        for (const QJsonValue &v : o.value(QStringLiteral("versions")).toArray()) {
            const QJsonObject x = v.toObject();
            const QString stamp = x.value(QStringLiteral("stamp")).toString();
            versions_ << QVariantMap{{QStringLiteral("stamp"), stamp},
                                     {QStringLiteral("when"), whenOf(x.value(QStringLiteral("createdAt")).toString(), stamp)},
                                     {QStringLiteral("label"), versionLabel(x.value(QStringLiteral("reason")).toString())}};
        }
        onHold_ = o.value(QStringLiteral("hold")).toBool();
        emit versionsChanged();
    });
}

void AppController::offerUndo(const QString &method, const QStringList &names, const QString &text) {
    undoMethod_ = method;
    undoNames_ = names;
    emit undoOffered(text);
}

void AppController::undoLast() {
    if (busy_ || undoNames_.isEmpty()) return;
    const QString method = undoMethod_;
    const QStringList names = undoNames_;
    undoNames_.clear();
    if (method == QLatin1String("skills.rollback")) startJob(method, {{QStringLiteral("names"), QJsonArray::fromStringList(names)}}, tr("Rolling back"), QStringLiteral("rollback"), false);
    else startJob(method, {{QStringLiteral("name"), names.first()}}, tr("Restoring %1").arg(names.first()), QStringLiteral("restore"), false);
}

void AppController::openReview(const QStringList &names) {
    if (busy_) return;
    reviews_.clear();
    reviewOpen_ = reviewLoading_ = true;
    emit reviewChanged();
    startJob(QStringLiteral("skills.diff"), {{QStringLiteral("names"), QJsonArray::fromStringList(names)}}, tr("Reading what changed"), QStringLiteral("diff"), false);
}

void AppController::closeReview() {
    reviewOpen_ = reviewLoading_ = false;
    reviews_.clear();
    emit reviewChanged();
}

void AppController::applyReview(const QStringList &names) {
    if (busy_ || names.isEmpty()) return;
    QJsonObject stages;
    for (const QVariant &v : reviews_) {
        const QVariantMap r = v.toMap();
        if (names.contains(r.value(QStringLiteral("name")).toString())) stages.insert(r.value(QStringLiteral("name")).toString(), r.value(QStringLiteral("stage")).toString());
    }
    closeReview();
    startJob(QStringLiteral("skills.update"), {{QStringLiteral("stages"), stages}}, tr("Updating skills"), QStringLiteral("update"), true);
}

void AppController::rollback(const QString &name, const QString &stamp) {
    startJob(QStringLiteral("skills.rollback"), {{QStringLiteral("name"), name}, {QStringLiteral("stamp"), stamp}}, tr("Rolling back %1").arg(name), QStringLiteral("rollback"), false);
}

void AppController::setAgentEnabled(const QStringList &names, const QString &agent, bool on) {
    startJob(QStringLiteral("skills.setAgentEnabled"),
             {{QStringLiteral("names"), QJsonArray::fromStringList(names)}, {QStringLiteral("agent"), agent}, {QStringLiteral("enabled"), on}},
             tr("Changing which agents see it"), QStringLiteral("setAgentEnabled"), false);
}

void AppController::setHold(const QString &name, bool on) {
    startJob(QStringLiteral("skills.hold"), {{QStringLiteral("name"), name}, {QStringLiteral("on"), on}}, tr("Changing the hold"), QStringLiteral("hold"), false);
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
        QVariantList skills;
        for (const SkillRow &s : model_.rows()) {
            if (k < s.links.size() && SkillRow::badLink(s.links[k])) issues << s.name;
            skills << QVariantMap{{QStringLiteral("name"), s.name}, {QStringLiteral("state"), k < s.links.size() ? s.links[k] : QStringLiteral("missing")}};
        }
        out.insert(QStringLiteral("issues"), issues);
        out.insert(QStringLiteral("skills"), skills);
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
            links << QVariantMap{{QStringLiteral("id"), agents_[k].toMap().value(QStringLiteral("id"))},
                                 {QStringLiteral("mode"), agents_[k].toMap().value(QStringLiteral("mode"))},
                                 {QStringLiteral("label"), agents_[k].toMap().value(QStringLiteral("label"))},
                                 {QStringLiteral("state"), k < s->links.size() ? s->links[k] : QStringLiteral("missing")}};
        detail_ = QVariantMap{{QStringLiteral("name"), s->name},
                              {QStringLiteral("description"), s->description},
                              {QStringLiteral("state"), s->status},
                              {QStringLiteral("note"), s->note},
                              {QStringLiteral("path"), s->path},
                              {QStringLiteral("source"), s->source},
                              {QStringLiteral("sourceType"), s->sourceType},
                              {QStringLiteral("live"), s->live},
                              {QStringLiteral("upstreamPath"), s->upstreamPath},
                              {QStringLiteral("hash"), s->hash.left(8)},
                              {QStringLiteral("installed"), dateOf(s->installedAt)},
                              {QStringLiteral("updated"), dateOf(s->updatedAt)},
                              {QStringLiteral("tracked"), !s->source.isEmpty()},
                              {QStringLiteral("versions"), s->versions},
                              {QStringLiteral("parked"), s->parked},
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
        if (n && done.value(QStringLiteral("ok")).toBool()) {
            QStringList names;
            for (const QJsonValue &v : results)
                if (v.toObject().value(QStringLiteral("ok")).toBool()) names << v.toObject().value(QStringLiteral("name")).toString();
            offerUndo(QStringLiteral("skills.rollback"), names, msg);
            msg.clear();
        }
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
        if (done.value(QStringLiteral("ok")).toBool()) {
            offerUndo(QStringLiteral("skills.restore"), {name}, msg);
            msg.clear();
        }
    } else if (kind == QLatin1String("diff")) {
        reviewLoading_ = false;
        reviews_ = done.value(QStringLiteral("reviews")).toArray().toVariantList();
        emit reviewChanged();
        if (!error.isEmpty()) {
            closeReview();
            msg = error;
            level = QStringLiteral("error");
        } else if (reviews_.isEmpty()) {
            closeReview();
            msg = tr("Nothing to update");
        }
    } else if (kind == QLatin1String("rollback")) {
        msg = done.value(QStringLiteral("ok")).toBool() ? tr("Rolled back. Automatic updates leave it alone until upstream moves on") : error;
        level = done.value(QStringLiteral("ok")).toBool() ? QStringLiteral("ok") : QStringLiteral("error");
    } else if (kind == QLatin1String("restore")) {
        msg = done.value(QStringLiteral("ok")).toBool() ? tr("Restored") : error;
        level = done.value(QStringLiteral("ok")).toBool() ? QStringLiteral("ok") : QStringLiteral("error");
    } else if (kind == QLatin1String("mergeLocks")) {
        msg = error.isEmpty() ? tr("Lock files merged. Backups of both are next to them") : error;
        level = error.isEmpty() ? QStringLiteral("ok") : QStringLiteral("error");
        if (error.isEmpty()) {
            lockSplit_ = -1;
            emit lockSplitChanged();
        }
    } else if (kind == QLatin1String("sizes")) {
        pathSizes_ = done.value(QStringLiteral("sizes")).toArray().toVariantList();
        emit pathSizesChanged();
    } else if (kind == QLatin1String("clearCache")) {
        msg = error.isEmpty() ? tr("Cache cleared") : error;
        level = error.isEmpty() ? QStringLiteral("ok") : QStringLiteral("error");
        measurePaths();
    } else if (kind == QLatin1String("copy")) {
        msg = error.isEmpty() ? tr("Copied to the project") : error;
        level = error.isEmpty() ? QStringLiteral("ok") : QStringLiteral("error");
        loadProjects();
    } else if ((kind == QLatin1String("hold") || kind == QLatin1String("setAgentEnabled")) && !error.isEmpty()) {
        msg = error;
        level = QStringLiteral("error");
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
        candidateType_ = done.value(QStringLiteral("sourceType")).toString();
        candidateHost_ = done.value(QStringLiteral("host")).toString();
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
    if (kind == QLatin1String("update") || kind == QLatin1String("rollback") || kind == QLatin1String("hold")) loadVersions();
}

// the window regained focus, or F5: the backend reads the shared folder again; changes come as a delta
void AppController::refresh() {
    core_.call(QStringLiteral("state.snapshot"), {{QStringLiteral("rescan"), true}, {QStringLiteral("page"), 0}, {QStringLiteral("pageSize"), 1}});
}

void AppController::checkUpdates() { startJob(QStringLiteral("skills.check"), {}, tr("Checking upstream"), QStringLiteral("check"), false); }

void AppController::checkUpdatesQuietly() {
    if (!setting(QStringLiteral("updates"), QStringLiteral("checkAtLaunch"), true).toBool()) return;
    startJob(QStringLiteral("skills.check"), {}, tr("Checking upstream"), QStringLiteral("check-quiet"), false);
}

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

void AppController::installChosen(const QStringList &names, bool live) {
    if (busy_ || names.isEmpty()) return;
    const QString first = names.first(), repo = candidateRepo_;
    startJob(QStringLiteral("skills.install"),
             {{QStringLiteral("source"), repo},
              {QStringLiteral("skills"), QJsonArray::fromStringList(names)},
              {QStringLiteral("mode"), live ? QStringLiteral("link") : QStringLiteral("copy")}},
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
        mechanism_ = o.value(QStringLiteral("mechanism")).toString();
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
    QDesktopServices::openUrl(QUrl::fromLocalFile(!scopeCentral_.isEmpty() ? scopeCentral_ : core_.init().value(QStringLiteral("paths")).toObject().value(QStringLiteral("central")).toString()));
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
    const QVariantMap &accent = dark ? paletteDark_ : paletteLight_;
    const QColor link = !accent.isEmpty() ? QColor(accent.value(QStringLiteral("markText")).toString()) : dark ? QColor(QStringLiteral("#34E0A6")) : QColor(QStringLiteral("#07765A"));
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
