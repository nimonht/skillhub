// The window's connection to its backend, `skillhub serve --stdio` (docs/protocol.md). It starts the backend, starts it
// again after a crash, and turns JSON-RPC lines into calls and signals. The process lives on a worker thread, where every
// line is parsed and skill rows are built, so a large delta never stalls the interface.
#pragma once

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QTimer>
#include <functional>

#include "skill_row.h"

class QThread;
class BackendIo;

struct CoreReply {
    QJsonValue result;
    QJsonObject error;  // empty on success
    SkillRows rows;     // state.snapshot pages: the skills, already built
    bool ok() const { return error.isEmpty(); }
    int code() const { return error.value(QStringLiteral("code")).toInt(); }
    QString message() const { return error.value(QStringLiteral("message")).toString(); }
};

class CoreClient : public QObject {
    Q_OBJECT
public:
    using Reply = std::function<void(const CoreReply &)>;

    explicit CoreClient(QObject *parent = nullptr);
    ~CoreClient() override;

    void start();
    // Requests sent before the backend is ready wait for it. `reply` runs on the interface thread.
    void call(const QString &method, const QJsonObject &params = {}, Reply reply = {});
    // Asks the backend to stop at a safe point and waits up to 10 s for it: destroying the process would kill it mid-swap.
    void stop();
    bool isReady() const { return ready_; }
    QJsonObject init() const { return init_; }  // the initialize result: version, capabilities, paths

signals:
    void ready(bool restarted);
    void notification(const QString &method, const QJsonObject &params, const SkillRows &rows);
    void failed(const QString &why);  // it cannot run at all (missing, or another version)

private:
    void onStarted();
    void onMessage(const QJsonObject &msg, const SkillRows &rows);
    void onEnded(const QString &why);
    void send(qint64 id, const QString &method, const QJsonObject &params);

    QThread *thread_ = nullptr;
    BackendIo *io_ = nullptr;
    QHash<qint64, Reply> pending_;
    QVector<std::tuple<qint64, QString, QJsonObject>> waiting_;  // sent once the backend is ready
    qint64 nextId_ = 0;
    QJsonObject init_;
    bool ready_ = false, stopping_ = false, everReady_ = false, gaveUp_ = false;
    int restarts_ = 0;
    QTimer restart_;
};
