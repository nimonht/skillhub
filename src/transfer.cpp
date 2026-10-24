// Export / import of the skill set. One JSON file:
//   { "format": "skillhub-export", "version": 1, "skills": [ { name, description, source, url, path, hash }          (GitHub skill)
//                                                           | { name, description, local: true, files: [ {path, exec, data(base64)} ] } ] }
// A GitHub skill is installed fresh from its repository (latest upstream); a local skill carries its files.
// The file may come from anywhere, so reading is strict: names and paths are validated, sizes are capped, nothing is
// executed, symlinks are never created, and existing skills are never overwritten.
#include "store.h"
#include "platform.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>

#ifndef SKILLHUB_VERSION
#define SKILLHUB_VERSION "dev"
#endif

namespace hub {

namespace {

constexpr qint64 kMaxFile = 2 * 1024 * 1024;      // one embedded file
constexpr qint64 kMaxSkill = 12 * 1024 * 1024;    // one local skill
constexpr qint64 kMaxExport = 48 * 1024 * 1024;   // the whole export file
constexpr int kMaxSkills = 600;
constexpr int kMaxFiles = 800;

bool validName(const QString &n) {
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,99}$"));
    return re.match(n).hasMatch() && !n.contains(QStringLiteral(".."));
}

// relative path made of plain segments: no "..", no absolute path, no backslash, nothing hidden in git/NUL tricks
bool validRel(const QString &p) {
    if (p.isEmpty() || p.size() > 300 || p.startsWith(QLatin1Char('/')) || p.contains(QLatin1Char('\\')) || p.contains(QChar(0))) return false;
    for (const QString &seg : p.split(QLatin1Char('/'))) {
        if (seg.isEmpty() || seg == QLatin1String(".") || seg == QLatin1String("..") || seg == QLatin1String(".git")) return false;
    }
    return true;
}

QString firstFrontmatter(const QByteArray &skillMd, const QString &key) {
    const QStringList lines = QString::fromUtf8(skillMd.left(16 * 1024)).replace(QStringLiteral("\r\n"), QStringLiteral("\n")).split(QLatin1Char('\n'));
    if (lines.isEmpty() || lines.first().trimmed() != QLatin1String("---")) return {};
    for (int i = 1; i < lines.size() && lines[i].trimmed() != QLatin1String("---"); ++i)
        if (lines[i].startsWith(key + QLatin1Char(':'))) {
            QString v = lines[i].mid(key.size() + 1).trimmed();
            if (v.size() >= 2 && (v.startsWith(QLatin1Char('"')) || v.startsWith(QLatin1Char('\''))) && v.endsWith(v[0])) v = v.mid(1, v.size() - 2);
            return v == QLatin1String(">") || v == QLatin1String(">-") || v == QLatin1String("|") ? QString() : v;
        }
    return {};
}

}  // namespace

bool Store::writeExport(const QString &path, bool includeLocal, int *tracked, int *local, QStringList *skipped, QString *err) {
    int nTracked = 0, nLocal = 0;
    QJsonArray arr;
    for (const Skill &s : scan()) {
        QJsonObject o;
        o.insert(QStringLiteral("name"), s.name);
        o.insert(QStringLiteral("description"), s.description);
        if (s.src.tracked()) {
            o.insert(QStringLiteral("source"), s.src.source);
            o.insert(QStringLiteral("url"), s.src.url.isEmpty() ? QStringLiteral("https://github.com/") + s.src.source + QStringLiteral(".git") : s.src.url);
            o.insert(QStringLiteral("path"), s.src.skillPath);
            o.insert(QStringLiteral("hash"), s.src.hash);
            ++nTracked;
        } else if (includeLocal) {
            QJsonArray files;
            qint64 total = 0;
            int count = 0;
            bool tooBig = false;
            QDirIterator it(s.path, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                const QString rel = QDir(s.path).relativeFilePath(it.filePath());
                if (rel.startsWith(QStringLiteral(".git/")) || rel.contains(QStringLiteral("__pycache__/")) || rel.contains(QStringLiteral("node_modules/"))) continue;
                if (platform::isLink(it.filePath())) {
                    if (skipped) skipped->append(QStringLiteral("%1/%2 (symlink)").arg(s.name, rel));
                    continue;
                }
                QFile f(it.filePath());
                if (it.fileInfo().size() > kMaxFile || total + it.fileInfo().size() > kMaxSkill || ++count > kMaxFiles || !f.open(QIODevice::ReadOnly)) {
                    tooBig = true;
                    break;
                }
                const QByteArray data = f.readAll();
                total += data.size();
                QJsonObject fo;
                fo.insert(QStringLiteral("path"), rel);
#ifdef Q_OS_WIN
                const bool isExec = it.fileInfo().isExecutable() || data.startsWith("#!");  // Windows has no exec bit: scripts keep theirs
#else
                const bool isExec = it.fileInfo().isExecutable();
#endif
                fo.insert(QStringLiteral("exec"), isExec);
                fo.insert(QStringLiteral("data"), QString::fromLatin1(data.toBase64()));
                files.append(fo);
            }
            if (tooBig) {
                if (skipped) skipped->append(QStringLiteral("%1 (too large to embed)").arg(s.name));
                continue;
            }
            o.insert(QStringLiteral("local"), true);
            o.insert(QStringLiteral("files"), files);
            ++nLocal;
        } else {
            continue;
        }
        arr.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("format"), QStringLiteral("skillhub-export"));
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("createdAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    root.insert(QStringLiteral("createdBy"), QStringLiteral("skillhub ") + QStringLiteral(SKILLHUB_VERSION));
    root.insert(QStringLiteral("skills"), arr);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (err) *err = QStringLiteral("cannot write %1").arg(path);
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (err) *err = QStringLiteral("cannot write %1").arg(path);
        return false;
    }
    if (tracked) *tracked = nTracked;
    if (local) *local = nLocal;
    return true;
}

QVector<ImportItem> Store::readExport(const QString &path, QString *err) const {
    QVector<ImportItem> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = QStringLiteral("cannot open %1").arg(path);
        return out;
    }
    if (f.size() > kMaxExport) {
        if (err) *err = QStringLiteral("that file is larger than %1 MB, which no export of Skill Hub is").arg(kMaxExport / (1024 * 1024));
        return out;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    const QJsonObject root = doc.object();
    if (pe.error != QJsonParseError::NoError || root.value(QStringLiteral("format")).toString() != QLatin1String("skillhub-export")) {
        if (err) *err = QStringLiteral("this is not a Skill Hub export file");
        return out;
    }
    if (root.value(QStringLiteral("version")).toInt() != 1) {
        if (err) *err = QStringLiteral("this export was made by a newer Skill Hub (format version %1); update the app").arg(root.value(QStringLiteral("version")).toInt());
        return out;
    }
    const QJsonArray arr = root.value(QStringLiteral("skills")).toArray();
    if (arr.size() > kMaxSkills) {
        if (err) *err = QStringLiteral("too many skills in one file (%1)").arg(arr.size());
        return out;
    }
    QSet<QString> seen;
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        ImportItem it;
        it.name = o.value(QStringLiteral("name")).toString();
        it.description = o.value(QStringLiteral("description")).toString().left(600);
        it.local = o.value(QStringLiteral("local")).toBool();
        if (!validName(it.name)) {
            it.problem = QStringLiteral("not a valid skill name");
        } else if (seen.contains(it.name)) {
            it.problem = QStringLiteral("listed twice");
        } else if (!it.local) {
            QString url;
            const QString source = o.value(QStringLiteral("source")).toString();
            const QString slug = slugFromInput(source, &url);
            const QString p = o.value(QStringLiteral("path")).toString();
            if (slug.isEmpty() || slug != source) it.problem = QStringLiteral("source is not a GitHub repository (owner/repo)");
            else if (!validRel(p) || !(p == QLatin1String("SKILL.md") || p.endsWith(QLatin1String("/SKILL.md")))) it.problem = QStringLiteral("unsafe path inside the repository");
            else {
                it.source = slug;
                it.url = url;  // always rebuilt from the validated name; the file's own url is ignored
                it.skillPath = p;
            }
        } else {
            const QJsonArray files = o.value(QStringLiteral("files")).toArray();
            qint64 total = 0;
            bool hasSkillMd = false;
            if (files.isEmpty() || files.size() > kMaxFiles) it.problem = QStringLiteral("no files, or too many");
            for (const QJsonValue &fv : files) {
                if (!it.problem.isEmpty()) break;
                const QJsonObject fo = fv.toObject();
                ImportFile file;
                file.path = fo.value(QStringLiteral("path")).toString();
                file.exec = fo.value(QStringLiteral("exec")).toBool();
                if (!validRel(file.path)) { it.problem = QStringLiteral("unsafe file path \"%1\"").arg(file.path.left(60)); break; }
                file.data = QByteArray::fromBase64(fo.value(QStringLiteral("data")).toString().toLatin1());
                if (file.data.size() > kMaxFile) { it.problem = QStringLiteral("a file is larger than %1 MB").arg(kMaxFile / (1024 * 1024)); break; }
                total += file.data.size();
                if (total > kMaxSkill) { it.problem = QStringLiteral("larger than %1 MB").arg(kMaxSkill / (1024 * 1024)); break; }
                if (file.path == QLatin1String("SKILL.md")) hasSkillMd = true;
                it.files.push_back(file);
            }
            if (it.problem.isEmpty() && !hasSkillMd) it.problem = QStringLiteral("no SKILL.md at the top level");
            it.bytes = total;
            if (it.problem.isEmpty() && it.description.isEmpty())
                for (const ImportFile &file : it.files)
                    if (file.path == QLatin1String("SKILL.md")) it.description = firstFrontmatter(file.data, QStringLiteral("description"));
        }
        if (it.problem.isEmpty()) seen.insert(it.name);
        it.exists = validName(it.name) && QFileInfo::exists(central_ + QLatin1Char('/') + it.name);
        if (it.problem.isEmpty() || validName(it.name)) out.push_back(it);
        else {
            it.name = QStringLiteral("(invalid name)");
            out.push_back(it);
        }
    }
    return out;
}

bool Store::importItem(const ImportItem &it, QString *err) {
    auto fail = [&](const QString &m) { if (err) *err = m; emit log(QStringLiteral("FAILED to import %1: %2").arg(it.name, m)); return false; };
    if (!it.problem.isEmpty() || !validName(it.name)) return fail(it.problem.isEmpty() ? QStringLiteral("invalid skill") : it.problem);
    if (QFileInfo::exists(central_ + QLatin1Char('/') + it.name)) return fail(QStringLiteral("already installed (not overwritten)"));

    if (!it.local) {
        QString url;
        const QString slug = slugFromInput(it.source, &url);
        if (slug.isEmpty() || slug != it.source || !validRel(it.skillPath)) return fail(QStringLiteral("unsafe source"));
        QString repo, e;
        if (!ensureRepo(slug, url, &repo, &e)) return fail(e.isEmpty() ? QStringLiteral("cannot reach %1").arg(slug) : e);
        QString dir = dirOf(it.skillPath);
        if (treeHash(repo, dir).isEmpty()) {  // upstream moved the folder: find it by name
            const QString found = findSkillPath(repo, it.name);
            if (found.isEmpty()) return fail(QStringLiteral("not found in %1 any more").arg(slug));
            dir = dirOf(found);
        }
        Candidate c;
        c.name = it.name;
        c.description = it.description;
        c.dir = dir;
        c.slug = slug;
        c.url = url;
        return install(c) || fail(QStringLiteral("could not install from %1").arg(slug));
    }

    // local skill: write into a staging folder first, then move it into place
    const QString stage = cache_ + QStringLiteral("/stage/import-") + it.name;
    QDir(stage).removeRecursively();
    for (const ImportFile &f : it.files) {
        const QString dest = stage + QLatin1Char('/') + f.path;
        QDir().mkpath(QFileInfo(dest).absolutePath());
        QFile out(dest);
        if (!out.open(QIODevice::WriteOnly) || out.write(f.data) != f.data.size()) {
            QDir(stage).removeRecursively();
            return fail(QStringLiteral("cannot write %1").arg(f.path));
        }
        out.close();
        QFile::setPermissions(dest, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther |
                                        (f.exec ? (QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther) : QFileDevice::Permissions()));
    }
    QDir().mkpath(central_);
    if (!QDir().rename(stage, central_ + QLatin1Char('/') + it.name)) {
        QDir(stage).removeRecursively();
        return fail(QStringLiteral("cannot place the skill in the shared folder"));
    }
    emit log(QStringLiteral("imported local skill %1 (%2 files)").arg(it.name).arg(it.files.size()));
    return true;
}

}  // namespace hub
