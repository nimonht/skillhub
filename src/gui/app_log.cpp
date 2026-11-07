#include "gui/app_log.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUrl>
#include <QVariantList>

#include <cstdio>
#include <cstdlib>

#ifndef SKILLHUB_VERSION
#define SKILLHUB_VERSION "dev"
#endif

namespace {

constexpr int kKeep = 5000;                 // lines in the viewer
constexpr qint64 kFileMax = 1024 * 1024;    // one file; with the four older ones about 5 MiB
constexpr int kFiles = 5;
constexpr qint64 kReadBack = 256 * 1024;    // how much of the last session the viewer starts with
const QString kWhenFormat = QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz");

AppLog *g_log = nullptr;
QtMessageHandler g_previous = nullptr;

// the home folder is ~ in what is written, as the backend already writes it
QString shortened(QString text) {
    static const QString home = QDir::homePath();
    static const QString native = QDir::toNativeSeparators(home);
    if (home.size() > 2) {
        text.replace(native, QStringLiteral("~"));
        if (native != home) text.replace(home, QStringLiteral("~"));
    }
    return text;
}

const char *levelOf(QtMsgType type) {
    switch (type) {
    case QtDebugMsg: return "debug";
    case QtInfoMsg: return "info";
    case QtWarningMsg: return "warn";
    default: return "error";
    }
}

void handler(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    AppLog::write(QStringLiteral("qt"), QString::fromLatin1(levelOf(type)), qFormatLogMessage(type, context, message));   // not logged again from inside the log
    if (g_previous) {
        g_previous(type, context, message);
    } else {
        std::fprintf(stderr, "%s\n", qPrintable(qFormatLogMessage(type, context, message)));
        std::fflush(stderr);
    }
    if (type == QtFatalMsg) std::abort();
}

}  // namespace

QString AppLog::directory() {
    QString dir = qEnvironmentVariable("SKILLHUB_CACHE");   // as the backend finds its cache
    if (!qEnvironmentVariableIsSet("SKILLHUB_CACHE"))
        dir = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/skillhub");
    return dir.isEmpty() ? QString() : dir + QStringLiteral("/logs");
}

int AppLog::levelRank(const QString &level) {
    return level == QLatin1String("error") ? 3 : level == QLatin1String("warn") ? 2 : level == QLatin1String("info") ? 1 : 0;
}

AppLog *AppLog::instance() { return g_log; }

void AppLog::install() {
    if (g_log) return;
    g_log = new AppLog;
    g_previous = qInstallMessageHandler(handler);
    write(QStringLiteral("gui"), QStringLiteral("info"),
          QStringLiteral("Skill Hub %1 started on %2, Qt %3").arg(QStringLiteral(SKILLHUB_VERSION), QSysInfo::prettyProductName(),
                                                                  QString::fromLatin1(qVersion())));
}

AppLog::AppLog(QObject *parent) : QAbstractListModel(parent) {
    const QString dir = directory();
    if (dir.isEmpty() || !QDir().mkpath(dir)) return;   // no folder: the viewer still works, from memory
    path_ = dir + QStringLiteral("/skillhub.log");
    readPrevious();
    openFile();
    if (size_ > kFileMax) rotate();
}

void AppLog::readPrevious() {
    QFile in(path_);
    if (!in.open(QIODevice::ReadOnly)) return;
    const bool partial = in.size() > kReadBack;
    if (partial) in.seek(in.size() - kReadBack);
    const QList<QByteArray> raw = in.readAll().split('\n');
    for (qsizetype i = partial ? 1 : 0; i < raw.size(); ++i) {   // the first line of a cut is not whole
        const QString text = QString::fromUtf8(raw[i]);
        if (text.startsWith(QLatin1String("    ")) && !lines_.isEmpty()) {
            lines_.last().text += QLatin1Char('\n') + text.mid(4);
            continue;
        }
        const int when = kWhenFormat.size();   // "<when> [level] [source] text"
        if (text.size() < when + 4 || text[when] != QLatin1Char(' ') || text[when + 1] != QLatin1Char('[')) continue;
        const int levelEnd = text.indexOf(QStringLiteral("] ["), when + 2), sourceEnd = levelEnd < 0 ? -1 : text.indexOf(QStringLiteral("] "), levelEnd + 3);
        const QDateTime at = QDateTime::fromString(text.left(when), kWhenFormat);
        if (levelEnd < 0 || sourceEnd < 0 || !at.isValid()) continue;
        lines_.push_back({at.toMSecsSinceEpoch(), text.mid(when + 2, levelEnd - when - 2), text.mid(levelEnd + 3, sourceEnd - levelEnd - 3),
                          text.mid(sourceEnd + 2)});
    }
    if (lines_.size() > kKeep) lines_.remove(0, lines_.size() - kKeep);
}

void AppLog::openFile() {
    delete file_;
    file_ = new QFile(path_);
    if (!file_->open(QIODevice::WriteOnly | QIODevice::Append)) {
        delete file_;
        file_ = nullptr;
        size_ = 0;
        return;
    }
    size_ = file_->size();
}

// skillhub.log becomes .1, .1 becomes .2 and so on; the oldest goes. Where another copy of the window holds the file the rename
// fails: this one then goes on in the same file, and tries again after another megabyte.
void AppLog::rotate() {
    if (!file_) return;
    file_->close();
    QFile::remove(QStringLiteral("%1.%2").arg(path_).arg(kFiles - 1));
    for (int i = kFiles - 2; i >= 1; --i) QFile::rename(QStringLiteral("%1.%2").arg(path_).arg(i), QStringLiteral("%1.%2").arg(path_).arg(i + 1));
    const bool moved = QFile::rename(path_, path_ + QStringLiteral(".1"));
    openFile();
    if (!moved) size_ = 0;
}

QString AppLog::format(const Line &l) {
    QString text = l.text;
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    text.replace(QLatin1Char('\n'), QLatin1String("\n    "));
    return QDateTime::fromMSecsSinceEpoch(l.ts).toString(kWhenFormat) + QStringLiteral(" [") + l.level + QStringLiteral("] [") + l.source +
           QStringLiteral("] ") + text;
}

void AppLog::write(const QString &source, const QString &level, const QString &text, qint64 ts) {
    // Nothing the log does may log again: a warning raised while a line is written (the file, the rotation) would come back through the
    // message handler to the lock that is held. On this thread such a line is dropped; the handler still passes it on to stderr.
    static thread_local bool inside = false;
    if (!g_log || inside) return;
    inside = true;
    g_log->append({ts > 0 ? ts : QDateTime::currentMSecsSinceEpoch(), level, source, shortened(text)});
    inside = false;
}

void AppLog::append(Line l) {
    QMutexLocker lock(&mutex_);
    if (file_) {
        const QByteArray bytes = format(l).toUtf8() + '\n';
        file_->write(bytes);
        file_->flush();
        size_ += bytes.size();
        if (size_ > kFileMax) rotate();
    }
    pending_.push_back(std::move(l));
    if (!flushQueued_) {
        flushQueued_ = true;
        QMetaObject::invokeMethod(this, &AppLog::flush, Qt::QueuedConnection);
    }
}

void AppLog::flush() {
    QVector<Line> batch;
    {
        QMutexLocker lock(&mutex_);
        batch.swap(pending_);
        flushQueued_ = false;
    }
    if (batch.isEmpty()) return;
    if (batch.size() > kKeep) batch.remove(0, batch.size() - kKeep);
    const int drop = int(qMax<qsizetype>(0, lines_.size() + batch.size() - kKeep));
    if (drop > 0) {
        beginRemoveRows({}, 0, drop - 1);
        lines_.remove(0, drop);
        endRemoveRows();
    }
    beginInsertRows({}, int(lines_.size()), int(lines_.size() + batch.size() - 1));
    lines_ += batch;
    endInsertRows();
}

void AppLog::clearView() {
    beginResetModel();
    lines_.clear();
    endResetModel();
}

QString AppLog::tail(int count) const {
    QStringList out;
    for (qsizetype i = qMax<qsizetype>(0, lines_.size() - count); i < lines_.size(); ++i) out << format(lines_[i]);
    return out.join(QLatin1Char('\n'));
}

QHash<int, QByteArray> AppLog::roleNames() const {
    return {{TsRole, "ts"}, {TimeRole, "time"}, {WhenRole, "when"}, {LevelRole, "level"}, {SourceRole, "source"}, {TextRole, "text"}};
}

QVariant AppLog::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= lines_.size()) return {};
    const Line &l = lines_[index.row()];
    switch (role) {
    case TsRole: return l.ts;
    case TimeRole: return QDateTime::fromMSecsSinceEpoch(l.ts).toString(QStringLiteral("HH:mm:ss"));
    case WhenRole: return QDateTime::fromMSecsSinceEpoch(l.ts).toString(kWhenFormat);
    case LevelRole: return l.level;
    case SourceRole: return l.source;
    case TextRole: return l.text;
    default: return {};
    }
}

// ---- LogFilter ---------------------------------------------------------------------------------------------------------

LogFilter::LogFilter(QObject *parent) : QSortFilterProxyModel(parent) {
    setDynamicSortFilter(true);
    connect(this, &QAbstractProxyModel::sourceModelChanged, this, [this] {   // the total is the source's, whatever the filter shows of it
        if (!sourceModel()) return;
        connect(sourceModel(), &QAbstractItemModel::rowsInserted, this, &LogFilter::totalChanged);
        connect(sourceModel(), &QAbstractItemModel::rowsRemoved, this, &LogFilter::totalChanged);
        connect(sourceModel(), &QAbstractItemModel::modelReset, this, &LogFilter::totalChanged);
        emit totalChanged();
    });
}

void LogFilter::setLevel(const QString &v) {
    if (v == level_) return;
    level_ = v;
    invalidate();
    emit changed();
}

void LogFilter::setSide(const QString &v) {
    if (v == side_) return;
    side_ = v;
    invalidate();
    emit changed();
}

void LogFilter::setText(const QString &v) {
    if (v == text_) return;
    text_ = v;
    invalidate();
    emit changed();
}

bool LogFilter::filterAcceptsRow(int row, const QModelIndex &parent) const {
    const auto *log = qobject_cast<AppLog *>(sourceModel());
    if (!log || row >= log->rowCount(parent)) return false;
    const AppLog::Line &l = log->line(row);
    if (level_ != QLatin1String("all") && AppLog::levelRank(l.level) < AppLog::levelRank(level_)) return false;
    const bool service = l.source == QLatin1String("backend") || l.source == QLatin1String("stderr");
    if (side_ == QLatin1String("window") && service) return false;
    if (side_ == QLatin1String("service") && !service) return false;
    return text_.isEmpty() || l.text.contains(text_, Qt::CaseInsensitive) || l.source.contains(text_, Qt::CaseInsensitive);
}

QString LogFilter::joined(const QVariantList &rows) const {
    const auto *log = qobject_cast<AppLog *>(sourceModel());
    if (!log) return {};
    QList<int> picked;
    if (rows.isEmpty()) {
        for (int r = 0; r < rowCount(); ++r) picked << mapToSource(index(r, 0)).row();
    } else {
        for (const QVariant &v : rows) {
            const QModelIndex src = mapToSource(index(v.toInt(), 0));
            if (src.isValid()) picked << src.row();
        }
        std::sort(picked.begin(), picked.end());
    }
    QStringList out;
    for (const int r : picked) out << AppLog::format(log->line(r));
    return out.join(QLatin1Char('\n'));
}

void LogFilter::copy(const QVariantList &rows) const {
    if (QClipboard *clipboard = QGuiApplication::clipboard()) clipboard->setText(joined(rows));
}

bool LogFilter::saveTo(const QString &path) const {
    QFile out(path.startsWith(QLatin1String("file:")) ? QUrl(path).toLocalFile() : path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    out.write(joined({}).toUtf8() + '\n');
    return true;
}

QString LogFilter::filePath() const {
    const auto *log = qobject_cast<AppLog *>(sourceModel());
    return log ? log->filePath() : QString();
}

void LogFilter::openFolder() const {
    const QString dir = AppLog::directory();
    if (!dir.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void LogFilter::clearView() {
    if (auto *log = qobject_cast<AppLog *>(sourceModel())) log->clearView();
}
