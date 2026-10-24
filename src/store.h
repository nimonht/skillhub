// skillhub core: one canonical skills dir (~/.agents/skills) shared by every coding agent.
// Reads/writes the same ~/.agents/.skill-lock.json (v3) that `npx skills` uses.
#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

namespace hub {

// A coding agent ("provider") that reads user-level skills.
struct AgentDef {
    QString id;
    QString label;
    QString mode = QStringLiteral("link");  // link: per-skill symlinks in dir | native: reads the shared folder itself | config: listed in a config file
    QString dir;                            // folder the agent reads user-level skills from (native: the shared folder)
    bool linked = true;                     // mode == link
    QStringList extraDirs;                  // other places it may keep skills (legacy folders): scanned for strays
    QStringList ignore;                     // entries owned by the agent itself (never touched)
    QStringList bins;                       // commands that prove it is installed
    QStringList detect;                     // folders/files that prove it is installed
    QString configFile;                     // config mode: file that must list the shared folder
    bool detected = false;
    QString detectedBy;                     // "command `claude`" / "folder ~/.claude" / "added by you"
    bool managed = false;                   // detected, unless the user decided otherwise
    bool custom = false;                    // added by the user
};
using Provider = AgentDef;

enum class LinkState { Native, Linked, Missing, Copy, Foreign };

struct Source {
    QString source;     // "owner/repo"
    QString url;        // clone url
    QString skillPath;  // path of SKILL.md inside the repo
    QString hash;       // git tree sha of the installed skill folder ("" = unknown)
    QString installedAt;
    QString updatedAt;
    bool tracked() const { return !source.isEmpty(); }
};

struct Skill {
    enum Update { Untracked, Unknown, UpToDate, Available, Error };

    QString name;
    QString description;
    QString path;
    Source src;
    QHash<QString, LinkState> agents;
    Update update = Untracked;
    QString newHash;
    QString newPath;  // SKILL.md path upstream when the skill moved
    QString note;
};

struct Stray {
    QString agentId;
    QString name;
    QString path;
    bool duplicate = false;  // central already holds a skill of that name
};

struct Candidate {
    QString name;
    QString description;
    QString dir;   // folder inside the repo
    QString slug;  // owner/repo
    QString url;
};

// A skill inside an export file (see transfer.cpp). Imported files are untrusted: everything is validated before use.
struct ImportFile {
    QString path;
    bool exec = false;
    QByteArray data;
};

struct ImportItem {
    QString name;
    QString description;
    bool local = false;                // true: the files are embedded; false: installed from a GitHub repository
    QString source, url, skillPath;    // repository items
    QVector<ImportFile> files;         // local items
    qint64 bytes = 0;
    bool exists = false;               // a skill of that name is already installed (it is skipped, never overwritten)
    QString problem;                   // non-empty: rejected, with the reason
};

class Store : public QObject {
    Q_OBJECT
public:
    explicit Store(QObject *parent = nullptr);

    QString central() const { return central_; }
    QString lockPath() const { return lock_; }
    QString backupDir() const { return backup_; }
    const QVector<AgentDef> &agents() const { return agents_; }        // providers Skill Hub manages (detected, or switched on)
    const QVector<AgentDef> &providers() const { return providers_; }  // every provider it knows, with the detection result

    // providers: detection, the user's overrides and custom entries (~/.config/skillhub/providers.json)
    void reloadProviders();
    QString providersFile() const;
    bool setProviderManaged(const QString &id, bool on, QString *err = nullptr);
    QString addCustomProvider(const QString &label, const QString &dir, const QString &mode, QString *err);
    bool removeCustomProvider(const QString &id);
    bool providerWired(const AgentDef &a) const;  // config mode: does its config file list the shared folder?
    bool wiringOk() const;
    bool ensureProviderConfigs(bool *changed = nullptr);
    QStringList watchDirs() const;                // folders the stray watcher should observe
    bool watcherStale() const;                    // the installed watcher unit watches different folders
    bool syncWatcher();

    QVector<Skill> scan() const;
    QVector<Stray> strays() const;

    int adopt();    // pull stray skills out of agent dirs into the central dir
    int relink();   // make every linked agent point at the central dir, drop dead links

    int checkUpdates(QVector<Skill> &skills, const QStringList &only = {});
    bool applyUpdate(Skill &skill);

    QVector<Candidate> discover(const QString &repoInput, QString *err);
    bool install(const Candidate &c);
    bool removeSkill(const QString &name);
    bool track(const QString &name, const QString &repoInput, const QString &path, QString *err);

    // moving skills between machines: one JSON file lists every skill (repository source, or the files of a local skill)
    bool writeExport(const QString &path, bool includeLocal, int *tracked, int *local, QStringList *skipped, QString *err);
    QVector<ImportItem> readExport(const QString &path, QString *err) const;  // validates, never writes
    bool importItem(const ImportItem &item, QString *err);

    // systemd user units (auto-update timer + stray watcher)
    QString unitState(const QString &unit) const;
    bool installUnits(bool timer, bool watcher, QString *err);
    bool removeUnits(bool timer, bool watcher, QString *err);

    // Remove what skillhub itself put on the machine: systemd units, installed files below $HOME, the git cache.
    // Never touches skills, the lock file, agent links or the backup folder.
    bool uninstall(bool dryRun, bool keepCache);

    static QString slugFromInput(const QString &in, QString *url);
    static QString dirOf(const QString &skillMdPath);

signals:
    void log(const QString &line);

private:
    bool ensureRepo(const QString &slug, const QString &url, QString *dir, QString *err);
    QString treeHash(const QString &repo, const QString &dir) const;
    QStringList skillPathsInRepo(const QString &repo) const;
    QString findSkillPath(const QString &repo, const QString &name) const;  // SKILL.md of the skill called `name`, wherever upstream keeps it
    bool stageAndSwap(const QString &name, const QString &repo, const QString &dir);

    QString central_;
    QString lock_;
    QString backup_;
    QString cache_;
    QVector<AgentDef> agents_;
    QVector<AgentDef> providers_;
    QSet<QString> fetched_;
};

}  // namespace hub
