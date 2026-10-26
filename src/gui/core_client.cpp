#include "core_client.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>
#include <tuple>
#include <utility>

#ifndef SKILLHUB_VERSION
#define SKILLHUB_VERSION "dev"
#endif

namespace {

// SKILLHUB_BACKEND, else the command line next to the window (inside the bundle on macOS)
QString backendProgram() {
    const QString forced = qEnvironmentVariable("SKILLHUB_BACKEND");
    if (!forced.isEmpty()) return forced;
#ifdef Q_OS_WIN
    const QString name = QStringLiteral("skillhubw.exe");  // the windowless build: no console flashes up
#else
    const QString name = QStringLiteral("skillhub");
#endif
    return QDir(QCoreApplication::applicationDirPath()).filePath(name);
}

// The backend's standard error goes to a file: unread, QProcess would keep it in memory for the backend's whole life,
// and after a crash it is the only clue. Kept small; only the latest runs matter.
QString backendLog() {
    QString dir = qEnvironmentVariable("SKILLHUB_CACHE");
    if (!qEnvironmentVariableIsSet("SKILLHUB_CACHE"))
        dir = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/skillhub");
    if (dir.isEmpty() || !QDir().mkpath(dir)) return {};
    const QString path = dir + QStringLiteral("/backend.log");
    if (QFileInfo(path).size() > 1024 * 1024) QFile::remove(path);
    return path;
}

// the rows of a snapshot page or a delta, built here so the interface thread gets them ready-made
SkillRows takeRows(QJsonObject &holder, const QString &key) {
    const QJsonArray arr = holder.value(key).toArray();
    holder.remove(key);
    if (arr.isEmpty()) return {};
    auto rows = QSharedPointer<QVector<SkillRow>>::create();
    rows->reserve(arr.size());
    for (const QJsonValue &v : arr) rows->push_back(SkillRow::fromJson(v.toObject()));
    return rows;
}

}  // namespace

// Lives on the worker thread: owns the process, cuts its output into lines and parses them there.
class BackendIo : public QObject {
    Q_OBJECT
public:
    void start(const QString &program, const QString &log) {
        delete proc_;
        buf_.clear();
        proc_ = new QProcess(this);
        proc_->setStandardErrorFile(log.isEmpty() ? QProcess::nullDevice() : log, QIODevice::Append);
        connect(proc_, &QProcess::readyReadStandardOutput, this, &BackendIo::read);
        connect(proc_, &QProcess::started, this, &BackendIo::started);
        connect(proc_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart) emit ended(QStringLiteral("cannot start %1: %2").arg(proc_->program(), proc_->errorString()));
        });
        connect(proc_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
            read();
            emit ended(status == QProcess::CrashExit ? QStringLiteral("the background service crashed")
                                                     : QStringLiteral("the background service exited (%1)").arg(code));
        });
        proc_->start(program, {QStringLiteral("serve"), QStringLiteral("--stdio")});
    }

    void send(const QByteArray &line) {
        if (proc_ && proc_->state() == QProcess::Running) proc_->write(line);
    }

    // at quit, after the shutdown request: closing its input lets the backend stop at a safe point and exit
    void stop(int waitMs) {
        if (!proc_ || proc_->state() == QProcess::NotRunning) return;
        proc_->waitForBytesWritten(1000);
        proc_->closeWriteChannel();
        if (!proc_->waitForFinished(waitMs)) proc_->kill();
    }

signals:
    void started();
    void message(const QJsonObject &msg, const SkillRows &rows);
    void ended(const QString &why);

private:
    void read() {
        buf_ += proc_->readAllStandardOutput();
        qsizetype from = 0, nl;
        while ((nl = buf_.indexOf('\n', from)) >= 0) {
            handle(QByteArray::fromRawData(buf_.constData() + from, nl - from));
            from = nl + 1;
        }
        buf_.remove(0, from);
    }

    void handle(const QByteArray &line) {
        QJsonParseError err;
        QJsonObject msg = QJsonDocument::fromJson(line, &err).object();
        if (err.error == QJsonParseError::IllegalUTF8String)  // a file name that is not UTF-8: shown with replacement marks
            msg = QJsonDocument::fromJson(QString::fromUtf8(line).toUtf8(), &err).object();
        if (err.error != QJsonParseError::NoError) return;
        SkillRows rows;
        if (msg.value(QStringLiteral("result")).toObject().contains(QStringLiteral("skills"))) {
            QJsonObject r = msg.value(QStringLiteral("result")).toObject();
            rows = takeRows(r, QStringLiteral("skills"));
            msg.insert(QStringLiteral("result"), r);
        } else if (msg.value(QStringLiteral("method")).toString() == QLatin1String("state.delta")) {
            QJsonObject p = msg.value(QStringLiteral("params")).toObject();
            rows = takeRows(p, QStringLiteral("upserts"));
            msg.insert(QStringLiteral("params"), p);
        }
        emit message(msg, rows);
    }

    QProcess *proc_ = nullptr;
    QByteArray buf_;
};

CoreClient::CoreClient(QObject *parent) : QObject(parent) {
    qRegisterMetaType<SkillRows>();
    thread_ = new QThread(this);
    io_ = new BackendIo;
    io_->moveToThread(thread_);
    connect(thread_, &QThread::finished, io_, &QObject::deleteLater);
    connect(io_, &BackendIo::started, this, &CoreClient::onStarted);
    connect(io_, &BackendIo::message, this, &CoreClient::onMessage);
    connect(io_, &BackendIo::ended, this, &CoreClient::onEnded);
    connect(qApp, &QCoreApplication::aboutToQuit, this, &CoreClient::stop);
    restart_.setSingleShot(true);
    connect(&restart_, &QTimer::timeout, this, &CoreClient::start);
    thread_->start();
}

CoreClient::~CoreClient() {
    stop();
    thread_->quit();
    thread_->wait();
}

void CoreClient::start() {
    const QString program = backendProgram(), log = backendLog();
    QMetaObject::invokeMethod(io_, [io = io_, program, log] { io->start(program, log); });
}

void CoreClient::send(qint64 id, const QString &method, const QJsonObject &params) {
    QJsonObject msg{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), id}, {QStringLiteral("method"), method}};
    if (!params.isEmpty()) msg.insert(QStringLiteral("params"), params);
    const QByteArray line = QJsonDocument(msg).toJson(QJsonDocument::Compact) + '\n';
    QMetaObject::invokeMethod(io_, [io = io_, line] { io->send(line); });
}

void CoreClient::call(const QString &method, const QJsonObject &params, Reply reply) {
    const qint64 id = ++nextId_;
    if (reply) pending_.insert(id, std::move(reply));
    if (ready_) send(id, method, params);
    else waiting_.push_back({id, method, params});
}

void CoreClient::onStarted() {
    const qint64 id = ++nextId_;
    pending_.insert(id, [this](const CoreReply &r) {
        if (!r.ok()) {
            gaveUp_ = true;
            emit failed(r.code() == -32002 ? tr("The background service belongs to another version of Skill Hub; reinstall Skill Hub.")
                                           : r.message());
            return;
        }
        init_ = r.result.toObject();
        ready_ = true;
        restarts_ = 0;
        const bool again = everReady_;
        everReady_ = true;
        for (const auto &[wid, method, params] : std::exchange(waiting_, {})) send(wid, method, params);
        emit ready(again);
    });
    send(id, QStringLiteral("initialize"),
         {{QStringLiteral("protocol"), 1}, {QStringLiteral("client"), QStringLiteral("skillhub-gui " SKILLHUB_VERSION)}});
}

void CoreClient::onMessage(const QJsonObject &msg, const SkillRows &rows) {
    const QString method = msg.value(QStringLiteral("method")).toString();
    if (!method.isEmpty()) {
        emit notification(method, msg.value(QStringLiteral("params")).toObject(), rows);
        return;
    }
    const Reply reply = pending_.take(msg.value(QStringLiteral("id")).toInteger());
    if (reply) reply(CoreReply{msg.value(QStringLiteral("result")), msg.value(QStringLiteral("error")).toObject(), rows});
}

void CoreClient::onEnded(const QString &why) {
    ready_ = false;
    // every request in flight fails with the reason; callers decide what to show
    for (const Reply &reply : std::exchange(pending_, {}))
        reply(CoreReply{{}, QJsonObject{{QStringLiteral("code"), -32000}, {QStringLiteral("message"), why}}, {}});
    if (stopping_ || gaveUp_) return;
    if (!everReady_ && restarts_ >= 3) {  // it never came up: it cannot run here
        gaveUp_ = true;
        emit failed(why);
        return;
    }
    static const int backoff[] = {500, 1000, 2000};  // then every 5 s
    restart_.start(restarts_ < 3 ? backoff[restarts_] : 5000);
    ++restarts_;
}

void CoreClient::stop() {
    if (stopping_) return;
    stopping_ = true;
    restart_.stop();
    if (ready_) send(++nextId_, QStringLiteral("shutdown"), {});
    QMetaObject::invokeMethod(io_, [io = io_] { io->stop(10000); }, Qt::BlockingQueuedConnection);
    ready_ = false;
}

#include "core_client.moc"
