#include "platform.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#include <cstdio>

#ifdef Q_OS_WIN
#include <windows.h>
#include <io.h>
#include <filesystem>
#else
#include <unistd.h>
#endif

namespace hub::platform {

QString homeDir() {
    const QString forced = qEnvironmentVariable("SKILLHUB_HOME");
    return forced.isEmpty() ? QDir::homePath() : QDir::cleanPath(forced);
}

#ifdef Q_OS_WIN

namespace {

std::wstring native(const QString &p) { return QDir::toNativeSeparators(p).toStdWString(); }

bool makeJunction(const QString &linkPath, const QString &absTarget) {
    if (!QFileInfo(absTarget).isDir()) return false;
    QProcess p;
    p.setProgram(qEnvironmentVariable("ComSpec", QStringLiteral("cmd.exe")));
    p.setNativeArguments(QStringLiteral("/d /c mklink /J \"%1\" \"%2\"")
                             .arg(QDir::toNativeSeparators(linkPath), QDir::toNativeSeparators(absTarget)));
    p.start();
    if (!p.waitForFinished(15000)) {
        p.kill();
        return false;
    }
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

}  // namespace

bool isLink(const QString &path) {
    const DWORD a = GetFileAttributesW(native(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

QString linkTarget(const QString &path) {
    if (!isLink(path)) return {};
    const QFileInfo fi(path);
    const QString t = fi.symLinkTarget();
    return t.isEmpty() ? fi.canonicalFilePath() : t;
}

bool makeDirLink(const QString &linkPath, const QString &target) {
    if (!qEnvironmentVariableIsSet("SKILLHUB_NO_SYMLINKS")) {  // set by tests: behave like an account that may not create symlinks
        std::error_code ec;
        std::filesystem::create_directory_symlink(std::filesystem::path(native(target)), std::filesystem::path(native(linkPath)), ec);
        if (!ec) return true;
    }
    const QString abs = QFileInfo(target).isAbsolute() ? target : QDir::cleanPath(QFileInfo(linkPath).absolutePath() + QLatin1Char('/') + target);
    return makeJunction(linkPath, abs);
}

bool removeLink(const QString &linkPath) {
    if (!isLink(linkPath)) return false;
    return RemoveDirectoryW(native(linkPath).c_str()) || DeleteFileW(native(linkPath).c_str());
}

bool canMakeDirLinks() {
    static const bool ok = [] {
        const QString dir = QDir::tempPath() + QStringLiteral("/skillhub-probe-") + QString::number(GetCurrentProcessId());
        QDir().mkpath(dir + QStringLiteral("/t"));
        const bool made = makeDirLink(dir + QStringLiteral("/l"), QStringLiteral("t"));
        if (made) removeLink(dir + QStringLiteral("/l"));
        QDir(dir).removeRecursively();
        return made;
    }();
    return ok;
}

bool stdinIsTerminal() { return _isatty(_fileno(stdin)) != 0; }

#else  // POSIX

bool isLink(const QString &path) { return QFileInfo(path).isSymLink(); }

QString linkTarget(const QString &path) {
    const QFileInfo fi(path);
    return fi.isSymLink() ? fi.symLinkTarget() : QString();
}

bool makeDirLink(const QString &linkPath, const QString &target) {
    return ::symlink(QFile::encodeName(target).constData(), QFile::encodeName(linkPath).constData()) == 0;
}

bool removeLink(const QString &linkPath) { return isLink(linkPath) && QFile::remove(linkPath); }

bool canMakeDirLinks() { return true; }

bool stdinIsTerminal() { return isatty(0) != 0; }

#endif

bool copyTree(const QString &src, const QString &dst) {
    if (isLink(src)) return false;
    if (!QDir().mkpath(dst)) return false;
    QDirIterator it(src, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    while (it.hasNext()) {
        it.next();
        const QFileInfo fi = it.fileInfo();
        if (isLink(fi.filePath())) continue;
        const QString to = dst + QLatin1Char('/') + fi.fileName();
        if (fi.isDir()) {
            if (!copyTree(fi.filePath(), to)) return false;
        } else if (!QFile::copy(fi.filePath(), to)) {
            return false;
        }
    }
    return true;
}

QString gitProgram() {
    static const QString git = [] {
        QStringList dirs = qEnvironmentVariable("PATH").split(QDir::listSeparator(), Qt::SkipEmptyParts);
#if defined(Q_OS_MACOS)
        dirs << QStringLiteral("/opt/homebrew/bin") << QStringLiteral("/usr/local/bin") << QStringLiteral("/usr/bin");  // apps started from the Dock have a short PATH
#elif defined(Q_OS_WIN)
        for (const char *v : {"ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA"})
            dirs << qEnvironmentVariable(v) + QStringLiteral("/Git/cmd") << qEnvironmentVariable(v) + QStringLiteral("/Programs/Git/cmd");
#endif
        const QString found = QStandardPaths::findExecutable(QStringLiteral("git"), dirs);
        return found.isEmpty() ? QStringLiteral("git") : found;
    }();
    return git;
}

bool automationSupported() {
#ifdef Q_OS_LINUX
    return true;
#else
    return false;
#endif
}

void notify(const QString &title, const QString &body) {
#if defined(Q_OS_LINUX)
    if (!QStandardPaths::findExecutable(QStringLiteral("notify-send")).isEmpty())
        QProcess::startDetached(QStringLiteral("notify-send"),
                                {QStringLiteral("-a"), QStringLiteral("Skill Hub"), QStringLiteral("-i"),
                                 QStringLiteral("system-software-update"), title, body});
#elif defined(Q_OS_MACOS)
    auto esc = [](QString s) {
        s.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        s.replace(QLatin1Char('"'), QStringLiteral("\\\""));
        return s;
    };
    QProcess::startDetached(QStringLiteral("osascript"),
                            {QStringLiteral("-e"), QStringLiteral("display notification \"%1\" with title \"%2\"").arg(esc(body), esc(title))});
#else
    Q_UNUSED(title)
    Q_UNUSED(body)
#endif
}

}  // namespace hub::platform
