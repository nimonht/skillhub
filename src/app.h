// Qt Quick front end plumbing: list models and one controller. QML talks only to these classes; every file, git and
// systemd operation happens in the backend (`skillhub serve --stdio`, through CoreClient).
#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSortFilterProxyModel>
#include <QVariantList>
#include <QVariantMap>

#include "gui/core_client.h"
#include "gui/skill_row.h"

// The skills, keyed by name: snapshot pages are appended, deltas change rows in place (one ranged dataChanged for all
// of them) and insert or remove only what came or went. The model is never reset except for a fresh snapshot.
class SkillModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        DescriptionRole,
        SourceRole,
        StateRole,  // local | tracked | current | available | updated | error
        NoteRole,
        LinksRole,  // per agent: link | shared | missing | copy | foreign
        ProblemRole,
        RankRole
    };

    explicit SkillModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : int(rows_.size()); }
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void reset(const QVector<SkillRow> &rows);
    void append(const QVector<SkillRow> &rows);
    void upsert(const QVector<SkillRow> &rows);
    void remove(const QStringList &names);
    const SkillRow *find(const QString &name) const;
    const QVector<SkillRow> &rows() const { return rows_; }

private:
    void reindex();
    QVector<SkillRow> rows_;
    QHash<QString, int> index_;
};

// Search text, quick filter (all | updates | local | problems) and sort for the list.
class SkillFilter : public QSortFilterProxyModel {
    Q_OBJECT
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY changed)
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY changed)
    Q_PROPERTY(QString sortKey READ sortKey WRITE setSortKey NOTIFY changed)
    Q_PROPERTY(bool ascending READ ascending WRITE setAscending NOTIFY changed)
public:
    explicit SkillFilter(QObject *parent = nullptr);
    QString text() const { return text_; }
    QString mode() const { return mode_; }
    QString sortKey() const { return key_; }
    bool ascending() const { return asc_; }
    void setText(const QString &t);
    void setMode(const QString &m);
    void setSortKey(const QString &k);
    void setAscending(bool a);
    Q_INVOKABLE QString nameAt(int row) const;
    Q_INVOKABLE int rowOf(const QString &name) const;
signals:
    void changed();

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override;
    bool lessThan(const QModelIndex &l, const QModelIndex &r) const override;

private:
    const SkillRow &row(const QModelIndex &i) const;
    void apply();
    QString text_, mode_ = QStringLiteral("all"), key_ = QStringLiteral("name");
    bool asc_ = true;
};

class LogModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles { TimeRole = Qt::UserRole + 1, TextRole, LevelRole };
    explicit LogModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : int(lines_.size()); }
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void add(const QJsonArray &lines);  // event.log lines: {time, level, text}
    void addText(const QString &text, const QString &level = QStringLiteral("info"));
    Q_INVOKABLE void clear();

private:
    struct Line { QString time, text, level; };
    QVector<Line> lines_;
};

class AppController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel *skills READ skillsView CONSTANT)
    Q_PROPERTY(SkillFilter *filter READ filter CONSTANT)
    Q_PROPERTY(QAbstractItemModel *log READ logModel CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString busyText READ busyText NOTIFY busyChanged)
    Q_PROPERTY(double progress READ progress NOTIFY busyChanged)
    Q_PROPERTY(int skillCount READ skillCount NOTIFY countsChanged)
    Q_PROPERTY(int updateCount READ updateCount NOTIFY countsChanged)
    Q_PROPERTY(int localCount READ localCount NOTIFY countsChanged)
    Q_PROPERTY(int problemCount READ problemCount NOTIFY countsChanged)
    Q_PROPERTY(int strayCount READ strayCount NOTIFY countsChanged)
    Q_PROPERTY(int checkedCount READ checkedCount NOTIFY countsChanged)
    Q_PROPERTY(bool wiringOk READ wiringOk NOTIFY countsChanged)
    Q_PROPERTY(QVariantList agents READ agents NOTIFY agentsChanged)            // the agents Skill Hub manages (detected or switched on)
    Q_PROPERTY(int columnCount READ columnCount NOTIFY agentsChanged)           // port columns in the list (agents shown + overflow)
    Q_PROPERTY(int portW READ portW NOTIFY agentsChanged)                       // width of one column
    Q_PROPERTY(int hiddenCount READ hiddenCount NOTIFY agentsChanged)           // agents folded into the overflow column
    Q_PROPERTY(QVariantMap overflow READ overflow NOTIFY agentsChanged)
    Q_PROPERTY(QVariantList providerList READ providerList NOTIFY agentsChanged)  // every known agent with its detection result
    Q_PROPERTY(int knownCount READ knownCount NOTIFY agentsChanged)
    Q_PROPERTY(QString focusAgent READ focusAgent WRITE setFocusAgent NOTIFY focusChanged)
    Q_PROPERTY(QVariantMap agentDetail READ agentDetail NOTIFY focusChanged)
    Q_PROPERTY(QString selected READ selected NOTIFY selectedChanged)
    Q_PROPERTY(QVariantMap detail READ detail NOTIFY selectedChanged)
    Q_PROPERTY(QString preview READ preview NOTIFY selectedChanged)
    Q_PROPERTY(QString detailMode READ detailMode WRITE setDetailMode NOTIFY detailModeChanged)  // skill | agent | add
    Q_PROPERTY(QVariantList candidates READ candidates NOTIFY candidatesChanged)
    Q_PROPERTY(QString candidateRepo READ candidateRepo NOTIFY candidatesChanged)
    Q_PROPERTY(QString candidateError READ candidateError NOTIFY candidatesChanged)
    Q_PROPERTY(QString candidateType READ candidateType NOTIFY candidatesChanged)  // github, gitlab, git, local
    Q_PROPERTY(QString candidateHost READ candidateHost NOTIFY candidatesChanged)
    Q_PROPERTY(bool discovering READ discovering NOTIFY candidatesChanged)
    Q_PROPERTY(bool reviewOpen READ reviewOpen NOTIFY reviewChanged)
    Q_PROPERTY(bool reviewLoading READ reviewLoading NOTIFY reviewChanged)
    Q_PROPERTY(QVariantList reviews READ reviews NOTIFY reviewChanged)
    Q_PROPERTY(QVariantList versions READ versions NOTIFY versionsChanged)  // of the selected skill, newest first
    Q_PROPERTY(bool onHold READ onHold NOTIFY versionsChanged)
    Q_PROPERTY(bool automationSupported READ automationSupported NOTIFY automationChanged)  // daily timer + stray watcher exist on this system
    Q_PROPERTY(QString timerState READ timerState NOTIFY automationChanged)
    Q_PROPERTY(QString watcherState READ watcherState NOTIFY automationChanged)
    Q_PROPERTY(QString centralPath READ centralPath NOTIFY countsChanged)
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(QString themeMode READ themeMode WRITE setThemeMode NOTIFY themeChanged)  // system | dark | light
    Q_PROPERTY(bool reduceMotion READ reduceMotion WRITE setReduceMotion NOTIFY themeChanged)  // the user's setting
    Q_PROPERTY(bool stillMotion READ stillMotion NOTIFY themeChanged)                          // setting or forced (tests)
    Q_PROPERTY(bool loaded READ loaded NOTIFY countsChanged)
    Q_PROPERTY(QVariantMap exportInfo READ exportInfo NOTIFY countsChanged)       // { tracked, local }
    Q_PROPERTY(QString defaultExportPath READ defaultExportPath CONSTANT)
    Q_PROPERTY(QString documentsFolder READ documentsFolder CONSTANT)
    Q_PROPERTY(QVariantList importItems READ importItems NOTIFY importChanged)
    Q_PROPERTY(QString importError READ importError NOTIFY importChanged)
    Q_PROPERTY(bool noHover READ noHover CONSTANT)   // screenshots: ignore the real pointer

public:
    explicit AppController(QObject *parent = nullptr);

    QAbstractItemModel *skillsView() { return &filter_; }
    SkillFilter *filter() { return &filter_; }
    QAbstractItemModel *logModel() { return &log_; }
    bool busy() const { return busy_; }
    QString busyText() const { return busyText_; }
    double progress() const { return progress_; }
    int skillCount() const { return model_.rowCount(); }
    int updateCount() const { return counts_.value(QStringLiteral("updates")).toInt(); }
    int localCount() const { return counts_.value(QStringLiteral("local")).toInt(); }
    int problemCount() const { return counts_.value(QStringLiteral("problems")).toInt(); }
    int strayCount() const { return counts_.value(QStringLiteral("strays")).toInt(); }
    int checkedCount() const { return counts_.value(QStringLiteral("checked")).toInt(); }
    bool wiringOk() const { return counts_.value(QStringLiteral("wiringOk")).toBool(true); }
    QVariantList agents() const { return agents_; }
    int columnCount() const { return columns_; }
    int portW() const { return portW_; }
    int hiddenCount() const { return hidden_; }
    QVariantMap overflow() const { return overflow_; }
    QVariantList providerList() const { return providerList_; }
    int knownCount() const { return int(providerList_.size()); }
    QString focusAgent() const { return focus_; }
    void setFocusAgent(const QString &id);
    QVariantMap agentDetail() const;
    QString selected() const { return selected_; }
    QVariantMap detail() const { return detail_; }
    QString preview() const { return preview_; }
    QString detailMode() const { return mode_; }
    void setDetailMode(const QString &m);
    QVariantList candidates() const { return candidates_; }
    QString candidateRepo() const { return candidateRepo_; }
    QString candidateError() const { return candidateError_; }
    QString candidateType() const { return candidateType_; }
    QString candidateHost() const { return candidateHost_; }
    bool discovering() const { return discovering_; }
    bool reviewOpen() const { return reviewOpen_; }
    bool reviewLoading() const { return reviewLoading_; }
    QVariantList reviews() const { return reviews_; }
    QVariantList versions() const { return versions_; }
    bool onHold() const { return onHold_; }
    bool automationSupported() const { return automationSupported_; }
    QString timerState() const { return timer_; }
    QString watcherState() const { return watch_; }
    QString centralPath() const;
    QString version() const;
    QString themeMode() const { return themeMode_; }
    void setThemeMode(const QString &m);
    bool reduceMotion() const { return reduce_; }
    bool stillMotion() const { return reduce_ || forcedStill_; }
    void setReduceMotion(bool on);
    bool loaded() const { return loaded_; }
    bool noHover() const { return noHover_; }
    QVariantMap exportInfo() const;
    QString defaultExportPath() const;
    QString documentsFolder() const;
    QVariantList importItems() const { return importItems_; }
    QString importError() const { return importError_; }
    bool idle() const { return loaded_ && !busy_; }  // the screenshot harness waits for this

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void checkUpdates();
    Q_INVOKABLE void checkUpdatesQuietly();  // launch-time check: only speaks up when something needs attention
    Q_INVOKABLE void updateAll();
    Q_INVOKABLE void reviewAll() { openReview({}); }
    Q_INVOKABLE void openReview(const QStringList &names);  // empty: every update there is
    Q_INVOKABLE void closeReview();
    Q_INVOKABLE void applyReview(const QStringList &names);
    Q_INVOKABLE void rollback(const QString &name, const QString &stamp);
    Q_INVOKABLE void undoLast();
    Q_INVOKABLE void setHold(const QString &name, bool on);
    Q_INVOKABLE void setAgentEnabled(const QStringList &names, const QString &agent, bool on);  // agent: an id, @shared, @all
    Q_INVOKABLE void updateSkill(const QString &name);
    Q_INVOKABLE void adopt();
    Q_INVOKABLE void select(const QString &name);
    Q_INVOKABLE void discover(const QString &repo);
    Q_INVOKABLE void installChosen(const QStringList &names, bool live);
    Q_INVOKABLE void closeAdd();
    Q_INVOKABLE void dismiss();
    Q_INVOKABLE void removeSkill(const QString &name);
    Q_INVOKABLE void trackSkill(const QString &name, const QString &repo);
    Q_INVOKABLE void setTimer(bool on);
    Q_INVOKABLE void setWatcher(bool on);
    Q_INVOKABLE void rescanProviders();
    Q_INVOKABLE void setProviderManaged(const QString &id, bool on);
    Q_INVOKABLE void addCustomProvider(const QString &label, const QString &dir, bool readsShared);  // answers with customProviderAdded
    Q_INVOKABLE void removeCustomProvider(const QString &id);
    Q_INVOKABLE void exportTo(const QString &path, bool includeLocal);
    Q_INVOKABLE void importPreview(const QString &path);
    Q_INVOKABLE void importInstall(const QStringList &names);
    Q_INVOKABLE void importClear();
    Q_INVOKABLE void openFolder();
    Q_INVOKABLE void copyText(const QString &text);
    Q_INVOKABLE QString renderMarkdown(const QString &markdown, bool dark) const;  // styled HTML for the SKILL.md preview

signals:
    void busyChanged();
    void countsChanged();
    void agentsChanged();
    void focusChanged();
    void selectedChanged();
    void detailModeChanged();
    void candidatesChanged();
    void importChanged();
    void automationChanged();
    void themeChanged();
    void toast(const QString &level, const QString &text);
    void undoOffered(const QString &text);
    void reviewChanged();
    void versionsChanged();
    void customProviderAdded(const QString &error);  // "" when it worked

private:
    void onReady(bool restarted);
    void onNotification(const QString &method, const QJsonObject &params, const SkillRows &rows);
    void loadSnapshot();
    void loadPage(int page, qint64 rev);
    void applyState(const QJsonObject &o);
    void applyDelta(const QJsonObject &params, const SkillRows &rows);
    void startJob(const QString &method, const QJsonObject &params, const QString &busyText, const QString &kind, bool determinate);
    void jobDone(const QJsonObject &done);
    void rebuildDetail();
    void readPreview(const QString &name);
    void loadVersions();
    void offerUndo(const QString &method, const QStringList &names, const QString &text);
    void refreshAutomation();
    void loadSettings();
    void saveSetting(const QString &key, const QJsonValue &value);
    void setBusy(bool busy, const QString &text = {}, double progress = -1);
    QString tilde(const QString &path) const;

    CoreClient core_;
    SkillModel model_;
    SkillFilter filter_;
    LogModel log_;
    QJsonObject counts_;
    QVariantList agents_, providerList_;
    QVariantMap overflow_;
    int columns_ = 0, portW_ = 72, hidden_ = 0;
    qint64 rev_ = 0;
    bool loading_ = false, stale_ = false;
    QString job_, jobKind_, jobName_;  // the running job: its id, kind, and the skill it is about
    QVariantMap detail_;
    QVariantList candidates_, reviews_, versions_;
    QString undoMethod_;       // skills.rollback or skills.restore, for what just happened
    QStringList undoNames_;
    bool reviewOpen_ = false, reviewLoading_ = false, onHold_ = false;
    QVariantList importItems_;
    QString importError_, importPath_;
    QString selected_, preview_, focus_, mode_ = QStringLiteral("skill");
    QString busyText_, candidateRepo_, candidateError_, candidateType_, candidateHost_, timer_ = QStringLiteral("off"), watch_ = QStringLiteral("off");
    QString themeMode_ = QStringLiteral("system");
    double progress_ = -1;
    bool busy_ = false, discovering_ = false, reduce_ = false, forcedStill_ = false, noHover_ = false, loaded_ = false;
    bool automationSupported_ = false, themeForced_ = false;
};
