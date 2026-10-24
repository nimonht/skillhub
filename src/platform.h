#pragma once
// The few places where operating systems differ: directory links, copying, desktop notifications, terminals, automation.
#include <QString>

namespace hub::platform {

// Directory links are symlinks everywhere. On Windows a symlink needs Developer Mode or an administrator, so a junction
// (absolute target, no privilege needed) is used when a symlink is refused.
bool isLink(const QString &path);           // the entry itself is a link (never follows it)
QString linkTarget(const QString &path);    // absolute target of a link, "" when `path` is not one
bool makeDirLink(const QString &linkPath, const QString &target);  // target: relative to the link's folder, or absolute
bool removeLink(const QString &linkPath);   // removes the link only, never what it points to
bool canMakeDirLinks();                     // this user can create directory links here (probes on Windows)

bool copyTree(const QString &src, const QString &dst);  // plain recursive copy; links are skipped, never followed

QString gitProgram();                       // "git", or its full path where the desktop PATH is too short to find it
bool automationSupported();                 // daily update timer and stray watcher: systemd user units, Linux only
void notify(const QString &title, const QString &body);  // desktop notification, best effort
bool stdinIsTerminal();

}  // namespace hub::platform
