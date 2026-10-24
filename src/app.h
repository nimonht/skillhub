// Qt Quick front end plumbing: list models and one controller that wraps hub::Store.
// QML talks only to these classes; all file/git/systemd logic stays in Store.
#pragma once

#include <QAbstractListModel>
#include <QFutureWatcher>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QVariantList>
#include <QVariantMap>
#include <functional>

#include "store.h"

class SkillModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        DescriptionRole,
        SourceRole,
        StateRole,      // local | tracked | current | available | updated | error
        NoteRole,
        LinksRole,      // per agent: link | shared | missing | copy | foreign
        ProblemRole,
        RankRole
    };

    explicit SkillModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : skills_.size(); }
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setSkills(const QVector<hub::Skill> &skills, const QVector<hub::AgentDef> &agents, const QSet<QString> &justUpdated);
    const QVector<hub::Skill> &skills() const { return skills_; }
    const hub::Skill *find(const QString &name) const;
    static QString stateKey(const hub::Skill &s, bool justUpdated);
    static bool hasProblem(const hub::Skill &s);

private:
    QVector<hub::Skill> skills_;
    QVector<hub::AgentDef> agents_;
    QSet<QString> updated_;
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
    void apply();
    QString text_, mode_ = QStringLiteral("all"), key_ = QStringLiteral("name");
    bool asc_ = true;
};

class LogModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles { TimeRole = Qt::UserRole + 1, TextRole, LevelRole };
    explicit LogModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : lines_.size(); }
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void add(const QString &text);
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
    Q_PROPERTY(bool discovering READ discovering NOTIFY candidatesChanged)
    Q_PROPERTY(bool automationSupported READ automationSupported CONSTANT)  // daily timer + stray watcher exist on this system
    Q_PROPERTY(QString timerState READ timerState NOTIFY automationChanged)
    Q_PROPERTY(QString watcherState READ watcherState NOTIFY automationChanged)
    Q_PROPERTY(QString centralPath READ centralPath CONSTANT)
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
    int updateCount() const { return counts_[0]; }
    int localCount() const { return counts_[1]; }
    int problemCount() const { return counts_[2]; }
    int strayCount() const { return strays_; }
    int checkedCount() const { return counts_[3]; }
    bool wiringOk() const { return wiringOk_; }
    QVariantList agents() const { return agents_; }
    int columnCount() const { return columns_; }
    int portW() const { return portW_; }
    int hiddenCount() const { return hidden_; }
    QVariantMap overflow() const { return overflow_; }
    QVariantList providerList() const { return providerList_; }
    int knownCount() const { return providerList_.size(); }
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
    bool discovering() const { return discovering_; }
    bool automationSupported() const;
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

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void checkUpdates();
    Q_INVOKABLE void checkUpdatesQuietly();  // launch-time check: only speaks up when something needs attention
    Q_INVOKABLE void updateAll();
    Q_INVOKABLE void updateSkill(const QString &name);
    Q_INVOKABLE void adopt();
    Q_INVOKABLE void select(const QString &name);
    Q_INVOKABLE void discover(const QString &repo);
    Q_INVOKABLE void installChosen(const QStringList &names);
    Q_INVOKABLE void closeAdd();
    Q_INVOKABLE void dismiss();
    Q_INVOKABLE void removeSkill(const QString &name);
    Q_INVOKABLE void trackSkill(const QString &name, const QString &repo);
    Q_INVOKABLE void setTimer(bool on);
    Q_INVOKABLE void setWatcher(bool on);
    Q_INVOKABLE void rescanProviders();
    Q_INVOKABLE void setProviderManaged(const QString &id, bool on);
    Q_INVOKABLE QString addCustomProvider(const QString &label, const QString &dir, bool readsShared);
    Q_INVOKABLE void removeCustomProvider(const QString &id);
    Q_INVOKABLE QString exportTo(const QString &path, bool includeLocal);
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

private:
    using Progress = std::function<void(double)>;
    using Job = std::function<QVector<hub::Skill>(hub::Store &, QVector<hub::Skill>, const Progress &)>;
    void run(const QString &what, Job job, bool determinate = false, const QString &doneText = {});
    void finished();
    void applyScan(const QVector<hub::Skill> &verdicts);
    void rebuildDerived();
    void rebuildDetail();
    void refreshAutomation();
    void setBusy(bool busy, const QString &text = {}, double progress = -1);

    hub::Store store_;
    SkillModel model_;
    SkillFilter filter_;
    LogModel log_;
    struct DiscoverResult { QVector<hub::Candidate> items; QString error; };
    QFutureWatcher<QVector<hub::Skill>> jobWatcher_;
    QFutureWatcher<DiscoverResult> discoverWatcher_;
    QVector<hub::Candidate> pending_;
    QStringList jobUpdated_;
    QVector<hub::ImportItem> importPending_;
    QVariantList importItems_;
    QString importError_;
    int importOk_ = 0, importFailed_ = 0;
    QVector<hub::Skill> before_;
    QSet<QString> justUpdated_;
    QVariantList agents_, providerList_;
    QVariantMap overflow_;
    int columns_ = 0, portW_ = 72, hidden_ = 0;
    QVariantMap detail_;
    QVariantList candidates_;
    QString selected_, preview_, focus_, mode_ = QStringLiteral("skill");
    QString busyText_, doneText_, candidateRepo_, candidateError_, timer_ = QStringLiteral("off"), watch_ = QStringLiteral("off");
    QString themeMode_ = QStringLiteral("system");
    int counts_[4] = {0, 0, 0, 0};  // updates, local, problems, checked
    int strays_ = 0;
    double progress_ = -1;
    bool busy_ = false, wiringOk_ = true, discovering_ = false, reduce_ = false, forcedStill_ = false, noHover_ = false, loaded_ = false;
};
