// The window's log: what Qt prints (warnings, QML errors), what the window says, what the backend reports on its event stream
// and on its standard error. Every line goes to a rotating file, <cache>/logs/skillhub.log, and to a model the log viewer shows.
// The file is the record that outlives a crash, and what a bug report should hold.
#pragma once

#include <QAbstractListModel>
#include <QMutex>
#include <QSortFilterProxyModel>
#include <QString>
#include <QVector>

class QFile;

class AppLog : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles { TsRole = Qt::UserRole + 1, TimeRole, WhenRole, LevelRole, SourceRole, TextRole };
    struct Line {
        qint64 ts = 0;  // milliseconds since 1970
        QString level;  // debug, info, warn, error
        QString source; // qt, gui, backend, stderr
        QString text;
    };

    // First thing in main(), before the application object: from then on Qt's messages are kept, and still go where they went.
    static void install();
    static AppLog *instance();
    // Any thread. `ts` 0 is now. The text is kept as it is, apart from the home folder, which is written as ~.
    static void write(const QString &source, const QString &level, const QString &text, qint64 ts = 0);
    static QString directory();
    static int levelRank(const QString &level);  // debug 0 ... error 3

    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : int(lines_.size()); }
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    const Line &line(int row) const { return lines_[row]; }
    QString filePath() const { return path_; }
    // "2026-11-07 14:03:11.123 [warn] [qt] text", longer texts continue on lines that start with four spaces: the file's format
    static QString format(const Line &l);
    QString tail(int count) const;  // the last `count` lines, formatted
    void clearView();               // the viewer's list only: the file stays

private slots:
    void flush();

private:
    explicit AppLog(QObject *parent = nullptr);
    void append(Line l);
    void openFile();
    void rotate();
    void readPrevious();

    QVector<Line> lines_;    // the interface thread's copy, at most kKeep lines
    QVector<Line> pending_;  // written from any thread, not yet in the model
    QMutex mutex_;           // pending_, the file
    QString path_;
    QFile *file_ = nullptr;
    qint64 size_ = 0;
    bool flushQueued_ = false;
};

// What the viewer shows: a least level, a side (the window or the background service) and a text.
class LogFilter : public QSortFilterProxyModel {
    Q_OBJECT
    Q_PROPERTY(QString level READ level WRITE setLevel NOTIFY changed)    // all, info, warn, error: this level and worse
    Q_PROPERTY(QString side READ side WRITE setSide NOTIFY changed)       // all, window (gui, qt), service (backend, stderr)
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY changed)
    Q_PROPERTY(int total READ total NOTIFY totalChanged)                  // lines in the list before filtering
public:
    explicit LogFilter(QObject *parent = nullptr);
    QString level() const { return level_; }
    QString side() const { return side_; }
    QString text() const { return text_; }
    int total() const { return sourceModel() ? sourceModel()->rowCount() : 0; }
    void setLevel(const QString &v);
    void setSide(const QString &v);
    void setText(const QString &v);

    // rows are this model's rows; a QML selection passes them
    Q_INVOKABLE QString joined(const QVariantList &rows) const;
    Q_INVOKABLE void copy(const QVariantList &rows) const;       // to the clipboard; no rows: every line shown
    Q_INVOKABLE bool saveTo(const QString &path) const;          // every line shown
    Q_INVOKABLE void openFolder() const;
    Q_INVOKABLE QString filePath() const;
    Q_INVOKABLE void clearView();

signals:
    void changed();
    void totalChanged();

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override;

private:
    QString level_ = QStringLiteral("all"), side_ = QStringLiteral("all"), text_;
};
