#include "store.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>

#include "platform.h"

#include <algorithm>

namespace hub {

namespace {

QString nowIso() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }

QString stamp() { return QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")); }

// --- minimal YAML front matter reader (name / description are all we need) ---
QMap<QString, QString> parseFrontmatter(QString text) {
    QMap<QString, QString> out;
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    const QStringList lines = text.split(QLatin1Char('\n'));
    if (lines.isEmpty() || lines.first().trimmed() != QLatin1String("---")) return out;
    int end = -1;
    for (int i = 1; i < lines.size(); ++i) {
        if (lines[i].trimmed() == QLatin1String("---")) {
            end = i;
            break;
        }
    }
    if (end < 0) return out;
    for (int i = 1; i < end; ++i) {
        const QString &ln = lines[i];
        if (ln.isEmpty() || ln[0].isSpace() || ln[0] == QLatin1Char('#')) continue;
        const int c = ln.indexOf(QLatin1Char(':'));
        if (c <= 0) continue;
        const QString key = ln.left(c).trimmed();
        QString val = ln.mid(c + 1).trimmed();
        static const QSet<QString> block = {QStringLiteral("|"),  QStringLiteral(">"),  QStringLiteral(">-"),
                                            QStringLiteral("|-"), QStringLiteral(">+"), QStringLiteral("|+")};
        if (block.contains(val)) {
            QStringList parts;
            while (i + 1 < end && (lines[i + 1].isEmpty() || lines[i + 1][0].isSpace())) parts << lines[++i].trimmed();
            val = parts.join(QLatin1Char(' ')).simplified();
        } else if (val.size() >= 2 && ((val.startsWith(QLatin1Char('"')) && val.endsWith(QLatin1Char('"'))) ||
                                       (val.startsWith(QLatin1Char('\'')) && val.endsWith(QLatin1Char('\''))))) {
            val = val.mid(1, val.size() - 2);
        } else if (val.isEmpty()) {
            while (i + 1 < end && lines[i + 1].startsWith(QLatin1Char(' '))) ++i;  // nested map, skip
        }
        out.insert(key, val);
    }
    return out;
}

QMap<QString, QString> readFrontmatter(const QString &skillMd) {
    QFile f(skillMd);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return parseFrontmatter(QString::fromUtf8(f.read(64 * 1024)));
}

struct GitResult {
    bool ok = false;
    QString out;
    QByteArray raw;
    QString err;
};

GitResult runGit(const QStringList &args, const QString &workDir = {}, int timeoutMs = 180000) {
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    p.setProcessEnvironment(env);
    if (!workDir.isEmpty()) p.setWorkingDirectory(workDir);
    // long paths: some skill repositories nest deeper than the 260 characters Windows allows by default
    p.start(platform::gitProgram(), QStringList{QStringLiteral("-c"), QStringLiteral("core.longpaths=true")} + args);
    GitResult r;
    if (!p.waitForStarted(5000)) {
        r.err = QStringLiteral("git was not found: install Git (https://git-scm.com) and make sure it is on your PATH");
        return r;
    }
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(2000);
        r.err = QStringLiteral("git timed out");
        return r;
    }
    r.raw = p.readAllStandardOutput();
    r.out = QString::fromUtf8(r.raw);
    r.err = QString::fromUtf8(p.readAllStandardError()).trimmed();
    r.ok = p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
    return r;
}

bool runSimple(const QString &prog, const QStringList &args, QString *out = nullptr, int timeoutMs = 60000) {
    QProcess p;
    p.start(prog, args);
    if (!p.waitForStarted(5000)) return false;
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(2000);
        return false;
    }
    if (out) *out = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

QString dirDigest(const QString &root) {
    QCryptographicHash h(QCryptographicHash::Sha256);
    QStringList files;
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString p = it.next();
        const QString rel = QDir(root).relativeFilePath(p);
        if (rel.contains(QStringLiteral("__pycache__")) || rel.startsWith(QStringLiteral(".git/"))) continue;
        files << rel;
    }
    files.sort();
    for (const QString &rel : files) {
        h.addData(rel.toUtf8());
        QFile f(QDir(root).filePath(rel));
        if (f.open(QIODevice::ReadOnly)) h.addData(&f);
    }
    return QString::fromLatin1(h.result().toHex());
}

bool moveDir(const QString &src, const QString &dst) {
    QDir().mkpath(QFileInfo(dst).absolutePath());
    if (QDir().rename(src, dst)) return true;
    // another volume: copy, then remove. Never through a link, which would delete what it points to.
    if (platform::isLink(src)) return false;
    if (!platform::copyTree(src, dst)) {
        QDir(dst).removeRecursively();
        return false;
    }
    return QDir(src).removeRecursively();
}

// Write the plain files of a git tree into `dest`. Links and submodules are skipped, paths are checked, sizes are capped.
bool extractTree(const QString &repo, const QString &spec, const QString &dest, QString *err) {
    const GitResult ls = runGit({QStringLiteral("ls-tree"), QStringLiteral("-r"), QStringLiteral("-z"), spec}, repo, 60000);
    if (!ls.ok) {
        if (err) *err = ls.err.left(200);
        return false;
    }
    struct Entry {
        QString path;
        QByteArray sha;
        bool exec;
    };
    QVector<Entry> files;
    for (const QByteArray &rec : ls.raw.split('\0')) {
        const int tab = rec.indexOf('\t');
        if (tab < 0) continue;
        const QList<QByteArray> head = rec.left(tab).split(' ');
        if (head.size() != 3 || head[1] != "blob") continue;                 // submodules (commit) and trees
        if (head[0] != "100644" && head[0] != "100755") continue;            // 120000 is a symlink: never recreated
        const QString path = QString::fromUtf8(rec.mid(tab + 1));
        const QStringList seg = path.split(QLatin1Char('/'));
        bool sane = !path.isEmpty() && !path.startsWith(QLatin1Char('/')) && !path.contains(QLatin1Char('\\')) && seg.first() != QLatin1String(".git");
        for (const QString &part : seg) sane = sane && !part.isEmpty() && part != QLatin1String("..") && part != QLatin1String(".");
        if (!sane) continue;
        files.push_back({path, head[2], head[0] == "100755"});
    }
    if (files.size() > 5000) {
        if (err) *err = QStringLiteral("more than 5000 files");
        return false;
    }
    if (files.isEmpty()) return true;

    QProcess p;
    p.setWorkingDirectory(repo);
    p.start(platform::gitProgram(), {QStringLiteral("cat-file"), QStringLiteral("--batch")});
    if (!p.waitForStarted(5000)) {
        if (err) *err = QStringLiteral("cannot start git");
        return false;
    }
    for (const Entry &e : files) p.write(e.sha + '\n');
    p.closeWriteChannel();
    if (!p.waitForFinished(120000) || p.exitCode() != 0) {
        p.kill();
        if (err) *err = QStringLiteral("git cat-file failed");
        return false;
    }
    const QByteArray all = p.readAllStandardOutput();
    qsizetype pos = 0;
    qint64 total = 0;
    QDir().mkpath(dest);
    for (const Entry &e : files) {
        const qsizetype nl = all.indexOf('\n', pos);
        const QList<QByteArray> h = nl < 0 ? QList<QByteArray>() : all.mid(pos, nl - pos).split(' ');
        const qint64 size = h.size() == 3 && h[1] == "blob" ? h[2].toLongLong() : -1;
        if (size < 0 || nl + 1 + size > all.size()) {
            if (err) *err = QStringLiteral("unexpected git output for %1").arg(e.path);
            return false;
        }
        total += size;
        if (total > 128 * 1024 * 1024) {
            if (err) *err = QStringLiteral("more than 128 MB");
            return false;
        }
        const QString to = QDir(dest).filePath(e.path);
        QDir().mkpath(QFileInfo(to).absolutePath());
        QFile f(to);
        if (!f.open(QIODevice::WriteOnly) || f.write(all.constData() + nl + 1, size) != size) {
            if (err) *err = QStringLiteral("cannot write %1").arg(e.path);
            return false;
        }
        f.close();
        QFile::setPermissions(to, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther |
                                      (e.exec ? (QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther) : QFileDevice::Permissions()));
        pos = nl + 1 + size + 1;
    }
    return true;
}

bool isHiddenName(const QString &n) { return n.startsWith(QLatin1Char('.')); }

// rank a SKILL.md location inside a repo; lower is better, kNever = not a real skill
constexpr int kNever = 1 << 30;

int pathRank(const QString &p) {
    static const QStringList bad = {QStringLiteral("tests"), QStringLiteral("test"),     QStringLiteral("evals"),
                                    QStringLiteral("eval"),  QStringLiteral("fixtures"), QStringLiteral("node_modules"),
                                    QStringLiteral("oracle"), QStringLiteral("examples"), QStringLiteral("example")};
    const QStringList seg = p.split(QLatin1Char('/'));
    int rank = p.size();
    for (int i = 0; i < seg.size() - 1; ++i) {
        if (bad.contains(seg[i])) return kNever;
        if (seg[i].startsWith(QLatin1Char('.'))) rank += 1000;
    }
    if (seg.size() >= 3 && seg[seg.size() - 3] == QLatin1String("skills")) rank -= 200;
    return rank;
}

}  // namespace

// ------------------------------------------------------------------------------------------------

Store::Store(QObject *parent) : QObject(parent) {
    const QString home = platform::homeDir();
    central_ = home + QStringLiteral("/.agents/skills");
    lock_ = home + QStringLiteral("/.agents/.skill-lock.json");
    backup_ = home + QStringLiteral("/.agents/.backup");
    cache_ = qEnvironmentVariableIsSet("SKILLHUB_CACHE")  // tests: keep the cloned repositories inside the throw-away home
                 ? QDir::cleanPath(qEnvironmentVariable("SKILLHUB_CACHE"))
                 : QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/skillhub");

    reloadProviders();
}

// ---- lock file ----------------------------------------------------------------------------------

static QJsonObject loadLock(const QString &path) {
    QFile f(path);
    QJsonObject root;
    if (f.open(QIODevice::ReadOnly)) root = QJsonDocument::fromJson(f.readAll()).object();
    if (!root.contains(QStringLiteral("version"))) root.insert(QStringLiteral("version"), 3);
    if (!root.contains(QStringLiteral("skills"))) root.insert(QStringLiteral("skills"), QJsonObject());
    return root;
}

static bool saveLock(const QString &path, const QJsonObject &root) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return f.commit();
}

static Source sourceFromJson(const QJsonObject &o) {
    Source s;
    s.source = o.value(QStringLiteral("source")).toString();
    s.url = o.value(QStringLiteral("sourceUrl")).toString();
    s.skillPath = o.value(QStringLiteral("skillPath")).toString();
    s.hash = o.value(QStringLiteral("skillFolderHash")).toString();
    s.installedAt = o.value(QStringLiteral("installedAt")).toString();
    s.updatedAt = o.value(QStringLiteral("updatedAt")).toString();
    return s;
}

static QJsonObject sourceToJson(const Source &s, const QJsonObject &base = {}) {
    QJsonObject o = base;  // keep fields written by other tools
    o.insert(QStringLiteral("source"), s.source);
    o.insert(QStringLiteral("sourceType"), QStringLiteral("github"));
    o.insert(QStringLiteral("sourceUrl"), s.url);
    o.insert(QStringLiteral("skillPath"), s.skillPath);
    o.insert(QStringLiteral("skillFolderHash"), s.hash);
    o.insert(QStringLiteral("installedAt"), s.installedAt);
    o.insert(QStringLiteral("updatedAt"), s.updatedAt);
    return o;
}

// ---- scan -----------------------------------------------------------------------------------------

static LinkState stateFor(const AgentDef &a, const QString &name, const QString &centralSkill, const QString &central) {
    if (a.mode == QLatin1String("link")) {
        const QFileInfo fi(a.dir + QLatin1Char('/') + name);
        if (platform::isLink(fi.filePath())) {
            if (fi.exists() && fi.canonicalFilePath() == QFileInfo(centralSkill).canonicalFilePath()) return LinkState::Linked;
            return LinkState::Foreign;
        }
        return fi.exists() ? LinkState::Copy : LinkState::Missing;
    }
    // native / config: the agent reads the shared folder itself; a real folder of the same name in one of its own
    // folders is a stray duplicate
    for (const QString &d : a.extraDirs) {
        if (d == central) continue;
        const QFileInfo fi(d + QLatin1Char('/') + name);
        if (!platform::isLink(fi.filePath()) && fi.isDir()) return LinkState::Copy;
    }
    return LinkState::Native;
}

QVector<Skill> Store::scan() const {
    QVector<Skill> out;
    const QJsonObject lockSkills = loadLock(lock_).value(QStringLiteral("skills")).toObject();
    const QFileInfoList entries =
        QDir(central_).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks | QDir::Hidden, QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &e : entries) {
        if (isHiddenName(e.fileName())) continue;
        const QString md = e.filePath() + QStringLiteral("/SKILL.md");
        if (!QFileInfo::exists(md)) continue;
        Skill s;
        s.name = e.fileName();
        s.path = e.filePath();
        const auto fm = readFrontmatter(md);
        s.description = fm.value(QStringLiteral("description"));
        if (lockSkills.contains(s.name)) s.src = sourceFromJson(lockSkills.value(s.name).toObject());
        s.update = s.src.tracked() ? Skill::Unknown : Skill::Untracked;
        for (const AgentDef &a : agents_) s.agents.insert(a.id, stateFor(a, s.name, s.path, central_));
        out.push_back(s);
    }
    return out;
}

QVector<Stray> Store::strays() const {
    QVector<Stray> out;
    QSet<QString> seen;
    for (const AgentDef &a : agents_) {
        QStringList dirs = a.extraDirs;
        if (a.linked) dirs.prepend(a.dir);
        for (const QString &dir : dirs) {
            if (dir.isEmpty() || dir == central_) continue;
            const QFileInfoList entries =
                QDir(dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks | QDir::Hidden, QDir::Name);
            for (const QFileInfo &e : entries) {
                const QString n = e.fileName();
                if (isHiddenName(n) || a.ignore.contains(n) || platform::isLink(e.filePath())) continue;
                if (!QFileInfo::exists(e.filePath() + QStringLiteral("/SKILL.md"))) continue;
                if (seen.contains(e.canonicalFilePath())) continue;  // two agents may share a folder
                seen.insert(e.canonicalFilePath());
                Stray s;
                s.agentId = a.id;
                s.name = n;
                s.path = e.filePath();
                s.duplicate = QFileInfo::exists(central_ + QLatin1Char('/') + n);
                out.push_back(s);
            }
        }
    }
    return out;
}

// ---- adopt / relink -----------------------------------------------------------------------------------

// wait until nothing in `dir` changed for a few seconds (an installer may still be writing)
static void waitQuiet(const QString &dir) {
    for (int i = 0; i < 30; ++i) {
        QDateTime newest = QFileInfo(dir).lastModified();
        QDirIterator it(dir, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            const QDateTime t = it.fileInfo().lastModified();
            if (t > newest) newest = t;
        }
        if (newest.secsTo(QDateTime::currentDateTime()) >= 3) return;
        QThread::sleep(2);
    }
}

static void pruneBackups(const QString &backupRoot, const QString &name, int keep = 3) {
    QDir d(backupRoot + QLatin1Char('/') + name);
    const QStringList items = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (int i = 0; i < items.size() - keep; ++i) QDir(d.filePath(items[i])).removeRecursively();
}

int Store::adopt() {
    int changes = 0;
    QDir().mkpath(central_);
    for (const Stray &s : strays()) {
        const AgentDef *agent = nullptr;
        for (const AgentDef &a : agents_)
            if (a.id == s.agentId) agent = &a;
        if (!agent) continue;
        if (agent->linked && !platform::canMakeDirLinks()) {
            emit log(QStringLiteral("left %1 in %2: this account cannot create folder links here (Windows: turn on Developer Mode)")
                         .arg(s.name, agent->label));
            continue;
        }
        waitQuiet(s.path);
        const QString dest = central_ + QLatin1Char('/') + s.name;
        if (!QFileInfo::exists(dest)) {
            if (moveDir(s.path, dest)) {
                emit log(QStringLiteral("adopted %1 from %2 -> central").arg(s.name, agent->label));
                ++changes;
            } else {
                emit log(QStringLiteral("FAILED to move %1").arg(s.path));
            }
        } else if (dirDigest(s.path) == dirDigest(dest)) {
            const QString bk = backup_ + QStringLiteral("/_dupes/") + agent->id + QLatin1Char('-') + s.name + QLatin1Char('-') + stamp();
            if (moveDir(s.path, bk)) {
                emit log(QStringLiteral("removed duplicate %1 in %2 (backup: %3)").arg(s.name, agent->label, bk));
                ++changes;
            }
        } else {
            const QString bk = backup_ + QStringLiteral("/_conflicts/") + agent->id + QLatin1Char('-') + s.name + QLatin1Char('-') + stamp();
            if (moveDir(s.path, bk)) {
                emit log(QStringLiteral("CONFLICT %1 in %2 differs from central; kept central, moved yours to %3")
                             .arg(s.name, agent->label, bk));
                ++changes;
            }
        }
    }
    changes += relink();
    return changes;
}

int Store::relink() {
    int changes = 0;
    const QVector<Skill> skills = scan();
    QSet<QString> names;
    for (const Skill &s : skills) names.insert(s.name);

    for (const AgentDef &a : agents_) {
        if (!a.linked || a.dir.isEmpty()) continue;
        QDir().mkpath(a.dir);
        for (const Skill &s : skills) {
            const QString lp = a.dir + QLatin1Char('/') + s.name;
            const QFileInfo fi(lp);
            const bool isLink = platform::isLink(lp);
            if (isLink || fi.exists()) {
                if (isLink && !fi.exists()) {  // broken link with a live target name: repoint
                    platform::removeLink(lp);
                } else {
                    continue;
                }
            }
            const QString rel = QDir(a.dir).relativeFilePath(s.path);
            if (platform::makeDirLink(lp, rel)) {
                emit log(QStringLiteral("linked %1 -> %2").arg(lp, rel));
                ++changes;
            } else {
                emit log(QStringLiteral("FAILED to link %1").arg(lp));
            }
        }
        // dead links
        const QFileInfoList all = QDir(a.dir).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System | QDir::Hidden);
        for (const QFileInfo &e : all) {
            if (platform::isLink(e.filePath()) && !e.exists() && !isHiddenName(e.fileName()) && !a.ignore.contains(e.fileName()) &&
                platform::linkTarget(e.filePath()).startsWith(central_ + QLatin1Char('/'))) {  // only links Skill Hub could have made
                platform::removeLink(e.filePath());
                emit log(QStringLiteral("removed dead link %1").arg(e.filePath()));
                ++changes;
            }
        }
    }
    bool c = false;
    ensureProviderConfigs(&c);
    if (c) ++changes;
    return changes;
}

// ---- git helpers -------------------------------------------------------------------------------------------

QString Store::slugFromInput(const QString &in, QString *url) {
    QString s = in.trimmed();
    static const QRegularExpression gh(QStringLiteral(R"(^(?:https?://|git@)?(?:www\.)?github\.com[/:]([\w.-]+)/([\w.-]+?)(?:\.git)?(?:/.*)?$)"));
    static const QRegularExpression short_(QStringLiteral(R"(^([\w.-]+)/([\w.-]+)$)"));
    QRegularExpressionMatch m = gh.match(s);
    if (!m.hasMatch()) m = short_.match(s);
    if (!m.hasMatch()) return {};
    const QString slug = m.captured(1) + QLatin1Char('/') + m.captured(2);
    if (url) *url = QStringLiteral("https://github.com/") + slug + QStringLiteral(".git");
    return slug;
}

QString Store::dirOf(const QString &skillMdPath) {
    const int i = skillMdPath.lastIndexOf(QLatin1Char('/'));
    return i < 0 ? QString() : skillMdPath.left(i);
}

bool Store::ensureRepo(const QString &slug, const QString &url, QString *dirOut, QString *err) {
    QString key = slug;
    key.replace(QLatin1Char('/'), QStringLiteral("__"));
    const QString dir = cache_ + QStringLiteral("/repos/") + key;
    if (dirOut) *dirOut = dir;
    if (fetched_.contains(slug)) return true;
    GitResult r;
    if (!QFileInfo::exists(dir + QStringLiteral("/.git"))) {
        QDir().mkpath(cache_ + QStringLiteral("/repos"));
        QDir(dir).removeRecursively();
        emit log(QStringLiteral("cloning %1 ...").arg(slug));
        r = runGit({QStringLiteral("clone"), QStringLiteral("--depth"), QStringLiteral("1"), QStringLiteral("--quiet"),
                    QStringLiteral("--"), url, dir});
    } else {
        r = runGit({QStringLiteral("fetch"), QStringLiteral("--depth"), QStringLiteral("1"), QStringLiteral("--quiet"),
                    QStringLiteral("origin"), QStringLiteral("HEAD")},
                   dir);
        if (r.ok) r = runGit({QStringLiteral("checkout"), QStringLiteral("--quiet"), QStringLiteral("--force"), QStringLiteral("FETCH_HEAD")}, dir);
    }
    if (!r.ok) {
        if (err) *err = r.err.left(300);
        emit log(QStringLiteral("git failed for %1: %2").arg(slug, r.err.left(200)));
        return false;
    }
    fetched_.insert(slug);
    return true;
}

QString Store::treeHash(const QString &repo, const QString &dir) const {
    const QString spec = dir.isEmpty() ? QStringLiteral("HEAD^{tree}") : QStringLiteral("HEAD:") + dir;
    const GitResult r = runGit({QStringLiteral("rev-parse"), QStringLiteral("--verify"), QStringLiteral("--quiet"), spec}, repo);
    return r.ok ? r.out.trimmed() : QString();
}

QStringList Store::skillPathsInRepo(const QString &repo) const {
    const GitResult r = runGit({QStringLiteral("ls-tree"), QStringLiteral("-r"), QStringLiteral("--name-only"), QStringLiteral("HEAD")}, repo);
    QList<QPair<int, QString>> ranked;
    for (const QString &p : r.out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        if (p != QLatin1String("SKILL.md") && !p.endsWith(QStringLiteral("/SKILL.md"))) continue;
        const int rank = pathRank(p);
        if (rank != kNever) ranked.push_back({rank, p});
    }
    std::sort(ranked.begin(), ranked.end());
    QStringList out;
    for (const auto &pr : ranked) out << pr.second;
    return out;
}

static QMap<QString, QString> showFrontmatter(const QString &repo, const QString &path) {
    const GitResult r = runGit({QStringLiteral("show"), QStringLiteral("HEAD:") + path}, repo, 20000);
    return r.ok ? parseFrontmatter(r.out.left(64 * 1024)) : QMap<QString, QString>();
}

QString Store::findSkillPath(const QString &repo, const QString &name) const {
    for (const QString &p : skillPathsInRepo(repo)) {
        const QString nm = showFrontmatter(repo, p).value(QStringLiteral("name"));
        if (nm == name || (nm.isEmpty() && QFileInfo(dirOf(p)).fileName() == name)) return p;
    }
    return {};
}

QVector<Candidate> Store::discover(const QString &repoInput, QString *err) {
    QVector<Candidate> out;
    QString url;
    const QString slug = slugFromInput(repoInput, &url);
    if (slug.isEmpty()) {
        if (err) *err = QStringLiteral("expected owner/repo or a github.com URL");
        return out;
    }
    QString repo;
    if (!ensureRepo(slug, url, &repo, err)) return out;
    QSet<QString> seen;
    int budget = 400;
    for (const QString &p : skillPathsInRepo(repo)) {
        if (--budget < 0) break;
        const auto fm = showFrontmatter(repo, p);
        QString name = fm.value(QStringLiteral("name"));
        if (name.isEmpty()) name = QFileInfo(dirOf(p)).fileName();
        if (name.isEmpty() || seen.contains(name)) continue;
        // skill names become directory names: refuse anything path-like
        if (name.contains(QLatin1Char('/')) || name.startsWith(QLatin1Char('.')) || name.contains(QStringLiteral(".."))) continue;
        seen.insert(name);
        Candidate c;
        c.name = name;
        c.description = fm.value(QStringLiteral("description"));
        c.dir = dirOf(p);
        c.slug = slug;
        c.url = url;
        out.push_back(c);
    }
    if (out.isEmpty() && err) *err = QStringLiteral("no SKILL.md found in %1").arg(slug);
    return out;
}

// extract repo:dir into a staging folder, validate, swap into central keeping a backup
bool Store::stageAndSwap(const QString &name, const QString &repo, const QString &dir) {
    const QString stage = cache_ + QStringLiteral("/stage/") + name;
    QDir(stage).removeRecursively();
    QDir().mkpath(stage);
    const QString spec = dir.isEmpty() ? QStringLiteral("HEAD") : QStringLiteral("HEAD:") + dir;
    QString xerr;
    if (!extractTree(repo, spec, stage, &xerr)) {
        emit log(QStringLiteral("extract failed for %1: %2").arg(name, xerr));
        QDir(stage).removeRecursively();
        return false;
    }
    const auto fm = readFrontmatter(stage + QStringLiteral("/SKILL.md"));
    if (!QFileInfo::exists(stage + QStringLiteral("/SKILL.md")) || fm.value(QStringLiteral("description")).isEmpty()) {
        emit log(QStringLiteral("refusing %1: upstream folder has no valid SKILL.md").arg(name));
        QDir(stage).removeRecursively();
        return false;
    }
    const QString dest = central_ + QLatin1Char('/') + name;
    if (QFileInfo::exists(dest)) {
        if (!moveDir(dest, backup_ + QLatin1Char('/') + name + QLatin1Char('/') + stamp())) {
            emit log(QStringLiteral("cannot back up %1").arg(dest));
            QDir(stage).removeRecursively();
            return false;
        }
    }
    QDir().mkpath(central_);
    if (!moveDir(stage, dest)) {
        emit log(QStringLiteral("cannot place %1").arg(dest));
        return false;
    }
    pruneBackups(backup_, name);
    return true;
}

// ---- updates -----------------------------------------------------------------------------------------------

int Store::checkUpdates(QVector<Skill> &skills, const QStringList &only) {
    int available = 0;
    for (Skill &s : skills) {
        if (!only.isEmpty() && !only.contains(s.name)) continue;
        s.newHash.clear();
        s.newPath.clear();
        s.note.clear();
        if (!s.src.tracked()) {
            s.update = Skill::Untracked;
            continue;
        }
        QString repo, err;
        QString url = s.src.url.isEmpty() ? QStringLiteral("https://github.com/") + s.src.source + QStringLiteral(".git") : s.src.url;
        if (!ensureRepo(s.src.source, url, &repo, &err)) {
            s.update = Skill::Error;
            s.note = err;
            continue;
        }
        QString dir = dirOf(s.src.skillPath);
        QString h = s.src.skillPath.isEmpty() ? QString() : treeHash(repo, dir);
        const bool exists =
            !h.isEmpty() && runGit({QStringLiteral("cat-file"), QStringLiteral("-e"), QStringLiteral("HEAD:") + s.src.skillPath}, repo).ok;
        if (!exists) {  // upstream moved the folder: look it up by skill name
            const QString found = findSkillPath(repo, s.name);
            if (found.isEmpty()) {
                s.update = Skill::Error;
                s.note = QStringLiteral("not found upstream");
                continue;
            }
            s.newPath = found;
            dir = dirOf(found);
            h = treeHash(repo, dir);
        }
        s.newHash = h;
        if (h == s.src.hash && s.newPath.isEmpty()) {
            s.update = Skill::UpToDate;
        } else {
            s.update = Skill::Available;
            ++available;
        }
    }
    return available;
}

bool Store::applyUpdate(Skill &s) {
    if (!s.src.tracked()) return false;
    QString repo, err;
    const QString url = s.src.url.isEmpty() ? QStringLiteral("https://github.com/") + s.src.source + QStringLiteral(".git") : s.src.url;
    if (!ensureRepo(s.src.source, url, &repo, &err)) return false;
    const QString skillMd = s.newPath.isEmpty() ? s.src.skillPath : s.newPath;
    const QString dir = dirOf(skillMd);
    const QString hash = s.newHash.isEmpty() ? treeHash(repo, dir) : s.newHash;
    if (hash.isEmpty() || !stageAndSwap(s.name, repo, dir)) return false;

    QJsonObject root = loadLock(lock_);
    QJsonObject skills = root.value(QStringLiteral("skills")).toObject();
    Source src = s.src;
    src.url = url;
    src.skillPath = skillMd;
    src.hash = hash;
    if (src.installedAt.isEmpty()) src.installedAt = nowIso();
    src.updatedAt = nowIso();
    skills.insert(s.name, sourceToJson(src, skills.value(s.name).toObject()));
    root.insert(QStringLiteral("skills"), skills);
    saveLock(lock_, root);

    s.src = src;
    s.update = Skill::UpToDate;
    s.newHash.clear();
    s.newPath.clear();
    const auto fm = readFrontmatter(central_ + QLatin1Char('/') + s.name + QStringLiteral("/SKILL.md"));
    s.description = fm.value(QStringLiteral("description"));
    emit log(QStringLiteral("updated %1 -> %2").arg(s.name, hash.left(8)));
    relink();
    return true;
}

bool Store::install(const Candidate &c) {
    QString repo, err;
    if (!ensureRepo(c.slug, c.url, &repo, &err)) return false;
    const QString hash = treeHash(repo, c.dir);
    if (hash.isEmpty() || !stageAndSwap(c.name, repo, c.dir)) return false;
    QJsonObject root = loadLock(lock_);
    QJsonObject skills = root.value(QStringLiteral("skills")).toObject();
    Source src;
    src.source = c.slug;
    src.url = c.url;
    src.skillPath = c.dir.isEmpty() ? QStringLiteral("SKILL.md") : c.dir + QStringLiteral("/SKILL.md");
    src.hash = hash;
    src.installedAt = nowIso();
    src.updatedAt = src.installedAt;
    skills.insert(c.name, sourceToJson(src, skills.value(c.name).toObject()));
    root.insert(QStringLiteral("skills"), skills);
    saveLock(lock_, root);
    emit log(QStringLiteral("installed %1 from %2").arg(c.name, c.slug));
    relink();
    return true;
}

bool Store::removeSkill(const QString &name) {
    if (name.isEmpty() || name.contains(QLatin1Char('/')) || isHiddenName(name)) return false;
    const QString dest = central_ + QLatin1Char('/') + name;
    if (!QFileInfo::exists(dest)) return false;
    if (!moveDir(dest, backup_ + QStringLiteral("/_removed/") + name + QLatin1Char('-') + stamp())) return false;
    for (const AgentDef &a : agents_) {
        platform::removeLink(a.dir + QLatin1Char('/') + name);
    }
    QJsonObject root = loadLock(lock_);
    QJsonObject skills = root.value(QStringLiteral("skills")).toObject();
    skills.remove(name);
    root.insert(QStringLiteral("skills"), skills);
    saveLock(lock_, root);
    emit log(QStringLiteral("removed %1 (backup in %2/_removed)").arg(name, backup_));
    return true;
}

bool Store::track(const QString &name, const QString &repoInput, const QString &path, QString *err) {
    QString url;
    const QString slug = slugFromInput(repoInput, &url);
    if (slug.isEmpty()) {
        if (err) *err = QStringLiteral("expected owner/repo or a github.com URL");
        return false;
    }
    if (!QFileInfo::exists(central_ + QLatin1Char('/') + name + QStringLiteral("/SKILL.md"))) {
        if (err) *err = QStringLiteral("no such skill: %1").arg(name);
        return false;
    }
    QString repo;
    if (!ensureRepo(slug, url, &repo, err)) return false;
    QString skillMd = path;
    if (skillMd.isEmpty()) {
        for (const QString &p : skillPathsInRepo(repo)) {
            const QString nm = showFrontmatter(repo, p).value(QStringLiteral("name"));
            if (nm == name || (nm.isEmpty() && QFileInfo(dirOf(p)).fileName() == name)) {
                skillMd = p;
                break;
            }
        }
    } else if (!skillMd.endsWith(QStringLiteral("SKILL.md"))) {
        skillMd += QStringLiteral("/SKILL.md");
    }
    if (skillMd.isEmpty()) {
        if (err) *err = QStringLiteral("skill %1 not found in %2").arg(name, slug);
        return false;
    }
    QJsonObject root = loadLock(lock_);
    QJsonObject skills = root.value(QStringLiteral("skills")).toObject();
    Source src;
    src.source = slug;
    src.url = url;
    src.skillPath = skillMd;
    src.hash = QString();  // content of the local copy is unknown -> first update syncs it
    src.installedAt = nowIso();
    src.updatedAt = src.installedAt;
    skills.insert(name, sourceToJson(src, skills.value(name).toObject()));
    root.insert(QStringLiteral("skills"), skills);
    saveLock(lock_, root);
    emit log(QStringLiteral("tracking %1 <- %2 (%3)").arg(name, slug, skillMd));
    return true;
}

// ---- systemd user units (Linux only) ----------------------------------------------------------------------

#ifdef Q_OS_LINUX

static QString unitDir() { return platform::homeDir() + QStringLiteral("/.config/systemd/user"); }

static QString exeForUnits() {
    const QString installed = platform::homeDir() + QStringLiteral("/.local/bin/skillhub");
    return QFileInfo::exists(installed) ? QStringLiteral("%h/.local/bin/skillhub") : QCoreApplication::applicationFilePath();
}

QString Store::unitState(const QString &unit) const {
    QString out;
    runSimple(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("is-enabled"), unit}, &out, 8000);
    return out.isEmpty() ? QStringLiteral("not installed") : out;
}

static bool writeText(const QString &path, const QString &text) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(text.toUtf8());
    return f.commit();
}

bool Store::installUnits(bool timer, bool watcher, QString *err) {
    const QString exe = exeForUnits();
    const QString d = unitDir();
    bool ok = true;
    if (timer) {
        ok &= writeText(d + QStringLiteral("/skillhub-update.service"),
                        QStringLiteral("[Unit]\nDescription=Update shared agent skills\nAfter=network-online.target\n\n"
                                       "[Service]\nType=oneshot\nExecStart=%1 update --all --auto --notify\nNice=10\n").arg(exe));
        ok &= writeText(d + QStringLiteral("/skillhub-update.timer"),
                        QStringLiteral("[Unit]\nDescription=Daily agent skill update\n\n"
                                       "[Timer]\nOnCalendar=daily\nPersistent=true\nRandomizedDelaySec=30min\nOnStartupSec=10min\n\n"
                                       "[Install]\nWantedBy=timers.target\n"));
    }
    if (watcher) {
        QString paths;
        for (const QString &d : watchDirs()) paths += QStringLiteral("PathChanged=%1\n").arg(d);
        if (paths.isEmpty()) {
            if (err) *err = QStringLiteral("no coding agents are managed yet, so there is nothing to watch");
            return false;
        }
        ok &= writeText(d + QStringLiteral("/skillhub-adopt.service"),
                        QStringLiteral("[Unit]\nDescription=Pull stray agent skills into ~/.agents/skills\n"
                                       "StartLimitIntervalSec=0\n\n"
                                       "[Service]\nType=oneshot\nExecStart=%1 adopt --notify\n").arg(exe));
        ok &= writeText(d + QStringLiteral("/skillhub-adopt.path"),
                        QStringLiteral("[Unit]\nDescription=Watch agent skill dirs for stray skills\n\n"
                                       "[Path]\n%1\n"
                                       "[Install]\nWantedBy=default.target\n").arg(paths));
    }
    if (!ok) {
        if (err) *err = QStringLiteral("cannot write unit files in %1").arg(d);
        return false;
    }
    runSimple(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("daemon-reload")});
    if (timer) ok &= runSimple(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("enable"), QStringLiteral("--now"), QStringLiteral("skillhub-update.timer")});
    if (watcher) ok &= runSimple(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("enable"), QStringLiteral("--now"), QStringLiteral("skillhub-adopt.path")});
    if (!ok && err) *err = QStringLiteral("systemctl --user enable failed");
    return ok;
}

bool Store::removeUnits(bool timer, bool watcher, QString *err) {
    Q_UNUSED(err)
    const QString d = unitDir();
    if (timer) {
        runSimple(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("disable"), QStringLiteral("--now"), QStringLiteral("skillhub-update.timer")});
        QFile::remove(d + QStringLiteral("/skillhub-update.timer"));
        QFile::remove(d + QStringLiteral("/skillhub-update.service"));
    }
    if (watcher) {
        runSimple(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("disable"), QStringLiteral("--now"), QStringLiteral("skillhub-adopt.path")});
        QFile::remove(d + QStringLiteral("/skillhub-adopt.path"));
        QFile::remove(d + QStringLiteral("/skillhub-adopt.service"));
    }
    runSimple(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("daemon-reload")});
    return true;
}

#else

static const char kNoUnits[] = "daily updates and the stray watcher need systemd user units, which are only available on Linux";

QString Store::unitState(const QString &) const { return QStringLiteral("not available"); }

bool Store::installUnits(bool, bool, QString *err) {
    if (err) *err = QString::fromLatin1(kNoUnits);
    return false;
}

bool Store::removeUnits(bool, bool, QString *) { return true; }

#endif

// ---- uninstall -----------------------------------------------------------------------------------------------

bool Store::uninstall(bool dryRun, bool keepCache) {
    const QString home = platform::homeDir();
    const QString verb = dryRun ? QStringLiteral("would remove") : QStringLiteral("removed");
    bool ok = true;

#ifdef Q_OS_LINUX
    // 1. systemd user units
    QStringList units;
    for (const QString &n : {QStringLiteral("skillhub-update.timer"), QStringLiteral("skillhub-update.service"),
                             QStringLiteral("skillhub-adopt.path"), QStringLiteral("skillhub-adopt.service")})
        if (QFileInfo::exists(unitDir() + QLatin1Char('/') + n)) units << n;
    if (!units.isEmpty() && !dryRun) removeUnits(true, true, nullptr);
    for (const QString &n : units) {
        const QString f = unitDir() + QLatin1Char('/') + n;
        if (dryRun || !QFileInfo::exists(f)) emit log(verb + QStringLiteral(" unit ") + f);
        else { emit log(QStringLiteral("FAILED to remove ") + f); ok = false; }
    }

    // 2. installed files: only below $HOME (anything else belongs to a package manager / root)
    QSet<QString> prefixes{home + QStringLiteral("/.local")};
    const QFileInfo exe(QCoreApplication::applicationFilePath());
    if (exe.dir().dirName() == QLatin1String("bin")) {
        QDir p = exe.dir();
        p.cdUp();
        prefixes.insert(p.absolutePath());
    }
    for (const QString &prefix : prefixes) {
        QStringList files{prefix + QStringLiteral("/bin/skillhub"), prefix + QStringLiteral("/share/applications/skillhub.desktop"),
                          prefix + QStringLiteral("/share/icons/hicolor/scalable/apps/skillhub.svg")};
        for (int sz : {16, 24, 32, 48, 64, 128, 256, 512})
            files << QStringLiteral("%1/share/icons/hicolor/%2x%2/apps/skillhub.png").arg(prefix).arg(sz);
        bool any = false;
        for (const QString &f : files) any = any || QFileInfo(f).exists() || QFileInfo(f).isSymLink();
        if (!any) continue;
        if (!prefix.startsWith(home + QLatin1Char('/'))) {
            emit log(QStringLiteral("not touching %1: installed outside $HOME, use your package manager or `sudo make uninstall`").arg(prefix));
            continue;
        }
        for (const QString &f : files) {
            const QFileInfo fi(f);
            if (!fi.exists() && !fi.isSymLink()) continue;
            if (dryRun) { emit log(verb + QLatin1Char(' ') + f); continue; }
            if (QFile::remove(f)) emit log(verb + QLatin1Char(' ') + f);
            else { emit log(QStringLiteral("FAILED to remove ") + f); ok = false; }
        }
    }

#else
    Q_UNUSED(home)
    emit log(QStringLiteral("this system has no Skill Hub units or desktop files to remove; delete the application the way you installed it"));
#endif

    // 3. cache of cloned upstream repositories
    if (!keepCache && QFileInfo(cache_).fileName() == QLatin1String("skillhub") && QFileInfo::exists(cache_)) {
        if (dryRun) emit log(verb + QLatin1Char(' ') + cache_);
        else if (QDir(cache_).removeRecursively()) emit log(verb + QLatin1Char(' ') + cache_);
        else { emit log(QStringLiteral("FAILED to remove ") + cache_); ok = false; }
    }

    emit log(QStringLiteral("kept: %1, %2, agent links, %3, %4").arg(central_, lock_, backup_, providersFile()));
    return ok;
}

}  // namespace hub
