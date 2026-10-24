// skillhub: GUI when started without a command, CLI otherwise (used by the systemd units).
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlFileSelector>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QIcon>
#include <QSurfaceFormat>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>

#include <cstdio>
#include <memory>

#include "app.h"
#include "platform.h"
#include "store.h"

#ifndef SKILLHUB_VERSION
#define SKILLHUB_VERSION "dev"
#endif

using namespace hub;

namespace {

QTextStream &out() {
    static QTextStream s(stdout);
    return s;
}
QTextStream &err() {
    static QTextStream s(stderr);
    return s;
}

QString linkChar(LinkState s) {
    switch (s) {
    case LinkState::Native: return QStringLiteral("shared");
    case LinkState::Linked: return QStringLiteral("link");
    case LinkState::Missing: return QStringLiteral("MISSING");
    case LinkState::Copy: return QStringLiteral("COPY");
    case LinkState::Foreign: return QStringLiteral("OTHER");
    }
    return {};
}

QString updateName(Skill::Update u) {
    switch (u) {
    case Skill::Untracked: return QStringLiteral("local");
    case Skill::Unknown: return QStringLiteral("tracked");
    case Skill::UpToDate: return QStringLiteral("up-to-date");
    case Skill::Available: return QStringLiteral("update-available");
    case Skill::Error: return QStringLiteral("error");
    }
    return {};
}

void usage() {
    out() << "skillhub " SKILLHUB_VERSION " - one skills directory for every coding agent\n\n"
             "  skillhub [gui]                  open the manager window\n"
             "  skillhub list [--json]          installed skills, source, per-agent link state\n"
             "  skillhub doctor [--fix]         report (and repair) strays, dead links, Antigravity config\n"
             "  skillhub adopt [--notify]       move stray skills from agent dirs into ~/.agents/skills\n"
             "  skillhub check                  fetch upstream, show available updates (exit 10 if any)\n"
             "  skillhub update [--all|NAME...] [--auto] [--notify]\n"
             "  skillhub add OWNER/REPO [--skill a,b | --all]\n"
             "  skillhub remove NAME\n"
             "  skillhub track NAME OWNER/REPO [PATH]   set the upstream of a local skill\n"
             "  skillhub autoupdate on|off|status       daily update timer (Linux: systemd user timer)\n"
             "  skillhub watch on|off|status            auto-adopt strays (Linux: systemd path unit)\n"
             "  skillhub export [FILE] [--no-local]    write every skill to one file (GitHub sources; local skills embedded)\n"
             "  skillhub import FILE [--yes] [--no-local] [--only a,b]   install the skills of such a file on this machine\n"
             "  skillhub providers [--all] [--json]     coding agents found on this machine and how each sees skills\n"
             "  skillhub providers enable|disable ID    manage (or stop managing) an agent\n"
             "  skillhub providers add LABEL DIR [--shared] | remove ID   your own agent (--shared: it reads the shared folder)\n"
             "  skillhub uninstall [--dry-run] [--yes] [--keep-cache]\n"
             "                                  remove units, installed files below $HOME and the cache (keeps your skills)\n";
    out().flush();
}

int runCli(const QStringList &a) {
    Store st;
    QObject::connect(&st, &Store::log, [](const QString &l) {
        out() << l << '\n';
        out().flush();
    });
    const QString cmd = a.value(0);
    const QStringList rest = a.mid(1);
    const bool wantNotify = rest.contains(QStringLiteral("--notify"));

    if (cmd == QLatin1String("list")) {
        const QVector<Skill> sk = st.scan();
        if (rest.contains(QStringLiteral("--json"))) {
            QJsonArray arr;
            for (const Skill &s : sk) {
                QJsonObject o;
                o.insert(QStringLiteral("name"), s.name);
                o.insert(QStringLiteral("source"), s.src.source);
                o.insert(QStringLiteral("hash"), s.src.hash);
                QJsonObject ag;
                for (const AgentDef &d : st.agents()) ag.insert(d.id, linkChar(s.agents.value(d.id)));
                o.insert(QStringLiteral("agents"), ag);
                arr.append(o);
            }
            out() << QJsonDocument(arr).toJson(QJsonDocument::Indented);
        } else {
            for (const Skill &s : sk) {
                out() << s.name.leftJustified(30) << (s.src.source.isEmpty() ? QStringLiteral("local") : s.src.source).leftJustified(32);
                for (const AgentDef &d : st.agents()) out() << d.id << '=' << linkChar(s.agents.value(d.id)).leftJustified(8) << ' ';
                out() << '\n';
            }
            out() << sk.size() << " skills in " << st.central() << '\n';
        }
        return 0;
    }

    if (cmd == QLatin1String("doctor")) {
        const bool fix = rest.contains(QStringLiteral("--fix"));
        int problems = 0;
        for (const Stray &s : st.strays()) {
            out() << "stray: " << s.name << " in " << s.agentId << (s.duplicate ? " (duplicate of central)" : "") << '\n';
            ++problems;
        }
        for (const Skill &s : st.scan())
            for (const AgentDef &d : st.agents()) {
                const LinkState ls = s.agents.value(d.id);
                if (ls == LinkState::Missing || ls == LinkState::Copy || ls == LinkState::Foreign) {
                    out() << "link: " << s.name << " / " << d.id << " = " << linkChar(ls) << '\n';
                    ++problems;
                }
            }
        for (const AgentDef &d : st.agents())
            if (!st.providerWired(d)) {
                out() << d.id << ": " << d.configFile << " does not list " << st.central() << '\n';
                ++problems;
            }
        out() << (problems ? "problems: " : "healthy") << (problems ? QString::number(problems) : QString()) << '\n';
        if (fix && problems) {
            const int n = st.adopt();
            out() << "fixed changes: " << n << '\n';
            return 0;
        }
        return problems ? 1 : 0;
    }

    if (cmd == QLatin1String("export")) {
        QString file;
        for (const QString &r : rest) if (!r.startsWith(QLatin1String("--"))) { file = r; break; }
        if (file.isEmpty()) file = QStringLiteral("skillhub-skills-") + QDate::currentDate().toString(QStringLiteral("yyyyMMdd")) + QStringLiteral(".json");
        int tracked = 0, local = 0;
        QStringList skipped;
        QString why;
        if (!st.writeExport(file, !rest.contains(QStringLiteral("--no-local")), &tracked, &local, &skipped, &why)) { err() << why << '\n'; err().flush(); return 1; }
        out() << "exported " << tracked + local << " skills (" << tracked << " from GitHub, " << local << " local) to " << file << '\n';
        for (const QString &k : skipped) out() << "skipped " << k << '\n';
        if (local > 0) out() << "note: the files of the local skills are embedded in that file; share it only with people you trust\n";
        return 0;
    }

    if (cmd == QLatin1String("import")) {
        QString file;
        for (const QString &r : rest) if (!r.startsWith(QLatin1String("--"))) { file = r; break; }
        if (file.isEmpty()) { usage(); return 2; }
        QString why;
        const QVector<ImportItem> items = st.readExport(file, &why);
        if (items.isEmpty()) { err() << (why.isEmpty() ? QStringLiteral("nothing to import in that file") : why) << '\n'; err().flush(); return 1; }
        const bool noLocal = rest.contains(QStringLiteral("--no-local"));
        QStringList only;
        const int oi = rest.indexOf(QStringLiteral("--only"));
        if (oi >= 0 && oi + 1 < rest.size()) only = rest[oi + 1].split(QLatin1Char(','), Qt::SkipEmptyParts);
        QVector<ImportItem> todo;
        for (const ImportItem &it : items) {
            QString what = it.local ? QStringLiteral("local, %1 files").arg(it.files.size()) : it.source;
            QString mark = QStringLiteral("+");
            if (!it.problem.isEmpty()) { mark = QStringLiteral("!"); what = QStringLiteral("rejected: ") + it.problem; }
            else if (it.exists) { mark = QStringLiteral("="); what += QStringLiteral(" (already installed, kept)"); }
            else if (it.local && noLocal) { mark = QStringLiteral("-"); what += QStringLiteral(" (skipped: --no-local)"); }
            else if (!only.isEmpty() && !only.contains(it.name)) { mark = QStringLiteral("-"); what += QStringLiteral(" (not selected)"); }
            else todo.push_back(it);
            out() << mark << ' ' << it.name.leftJustified(30) << what << '\n';
        }
        if (todo.isEmpty()) { out() << "nothing to install\n"; return 0; }
        out() << "\nSkills are instructions your agents follow. Install only files you trust. GitHub skills arrive at their latest upstream version.\n";
        if (!rest.contains(QStringLiteral("--yes"))) {
            if (!platform::stdinIsTerminal()) { err() << "pass --yes to install " << todo.size() << " skills without a prompt\n"; err().flush(); return 2; }
            out() << "Install " << todo.size() << " skills? [y/N] ";
            out().flush();
            QTextStream in(stdin);
            const QString a = in.readLine().trimmed().toLower();
            if (a != QLatin1String("y") && a != QLatin1String("yes")) { out() << "aborted\n"; return 1; }
        }
        int ok = 0, failed = 0;
        for (const ImportItem &it : todo) {
            QString e;
            if (st.importItem(it, &e)) ++ok; else ++failed;
        }
        st.relink();
        out() << ok << " installed, " << failed << " failed\n";
        return failed ? 1 : 0;
    }

    if (cmd == QLatin1String("providers")) {
        const QString what = rest.value(0);
        QString why;
        if (what == QLatin1String("enable") || what == QLatin1String("disable")) {
            if (rest.size() < 2) { usage(); return 2; }
            if (!st.setProviderManaged(rest[1], what == QLatin1String("enable"), &why)) { err() << why << '\n'; err().flush(); return 1; }
            out() << rest[1] << (what == QLatin1String("enable") ? " is now managed" : " is no longer managed") << '\n';
            return 0;
        }
        if (what == QLatin1String("add")) {
            if (rest.size() < 3) { usage(); return 2; }
            const QString id = st.addCustomProvider(rest[1], rest[2], rest.contains(QStringLiteral("--shared")) ? QStringLiteral("native") : QStringLiteral("link"), &why);
            if (id.isEmpty()) { err() << why << '\n'; err().flush(); return 1; }
            out() << "added " << id << '\n';
            return 0;
        }
        if (what == QLatin1String("remove")) {
            if (rest.size() < 2) { usage(); return 2; }
            return st.removeCustomProvider(rest[1]) ? 0 : 1;
        }
        const bool all = rest.contains(QStringLiteral("--all"));
        if (rest.contains(QStringLiteral("--json"))) {
            QJsonArray arr;
            for (const AgentDef &d : st.providers()) {
                if (!all && !d.managed) continue;
                QJsonObject o;
                o.insert(QStringLiteral("id"), d.id);
                o.insert(QStringLiteral("label"), d.label);
                o.insert(QStringLiteral("mode"), d.mode);
                o.insert(QStringLiteral("dir"), d.dir);
                o.insert(QStringLiteral("detected"), d.detected);
                o.insert(QStringLiteral("detectedBy"), d.detectedBy);
                o.insert(QStringLiteral("managed"), d.managed);
                o.insert(QStringLiteral("custom"), d.custom);
                arr.append(o);
            }
            out() << QJsonDocument(arr).toJson(QJsonDocument::Indented);
            return 0;
        }
        int shown = 0;
        for (const AgentDef &d : st.providers()) {
            if (!all && !d.managed && !d.detected) continue;
            ++shown;
            const QString state = d.managed ? (d.detected ? QStringLiteral("managed") : QStringLiteral("managed (forced)")) : (d.detected ? QStringLiteral("detected, off") : QStringLiteral("not found"));
            QString where = d.mode == QLatin1String("link") ? QStringLiteral("links in ") + d.dir : d.mode == QLatin1String("config") ? QStringLiteral("lists the shared folder in ") + d.configFile : QStringLiteral("reads the shared folder");
            where.replace(platform::homeDir(), QStringLiteral("~"));
            out() << d.id.leftJustified(18) << state.leftJustified(17) << (d.detectedBy.isEmpty() ? QStringLiteral("-") : d.detectedBy).leftJustified(34) << where << '\n';
        }
        out() << shown << (all ? " known providers, " : " providers shown, ") << st.agents().size() << " managed. `skillhub providers --all` lists every known agent.\n";
        return 0;
    }

    if (cmd == QLatin1String("adopt")) {
        const int n = st.adopt();
        out() << n << " change(s)\n";
        if (wantNotify && n) platform::notify(QStringLiteral("Skill Hub"), QStringLiteral("Tidied %1 stray skill change(s) into ~/.agents/skills").arg(n));
        return 0;
    }

    if (cmd == QLatin1String("check") || cmd == QLatin1String("update")) {
        QVector<Skill> sk = st.scan();
        QStringList names;
        bool all = false;
        for (const QString &r : rest) {
            if (r == QLatin1String("--all")) all = true;
            else if (!r.startsWith(QLatin1String("--"))) names << r;
        }
        const bool doUpdate = cmd == QLatin1String("update");
        if (doUpdate && names.isEmpty() && !all) {
            err() << "update: give skill names or --all\n";
            err().flush();
            return 2;
        }
        if (doUpdate && rest.contains(QStringLiteral("--auto"))) {  // housekeeping before the update run
            st.adopt();
        }
        const int avail = st.checkUpdates(sk, doUpdate && !all ? names : QStringList());
        int updated = 0, failed = 0;
        QStringList changed;
        for (Skill &s : sk) {
            if (!names.isEmpty() && !all && !names.contains(s.name)) continue;
            if (s.update == Skill::Available || s.update == Skill::Error)
                out() << s.name.leftJustified(30) << updateName(s.update) << (s.note.isEmpty() ? QString() : QStringLiteral(" (") + s.note + QLatin1Char(')')) << '\n';
            if (doUpdate && s.update == Skill::Available) {
                if (st.applyUpdate(s)) { ++updated; changed << s.name; }
                else ++failed;
            }
        }
        if (!doUpdate) {
            out() << avail << " update(s) available\n";
            return avail ? 10 : 0;
        }
        out() << updated << " updated, " << failed << " failed\n";
        if (wantNotify && (updated || failed))
            platform::notify(QStringLiteral("Skill Hub"),
                   QStringLiteral("%1 skill(s) updated%2: %3").arg(updated).arg(failed ? QStringLiteral(", %1 failed").arg(failed) : QString(), changed.join(QStringLiteral(", ")).left(160)));
        return failed ? 1 : 0;
    }

    if (cmd == QLatin1String("add")) {
        if (rest.isEmpty()) { usage(); return 2; }
        QString e;
        const QVector<Candidate> cands = st.discover(rest.first(), &e);
        if (cands.isEmpty()) { err() << e << '\n'; err().flush(); return 1; }
        QStringList want;
        bool all = rest.contains(QStringLiteral("--all"));
        const int si = rest.indexOf(QStringLiteral("--skill"));
        if (si >= 0 && si + 1 < rest.size()) want = rest[si + 1].split(QLatin1Char(','), Qt::SkipEmptyParts);
        if (!all && want.isEmpty()) {
            out() << "skills in " << rest.first() << ":\n";
            for (const Candidate &c : cands) out() << "  " << c.name.leftJustified(30) << c.description.left(80) << '\n';
            out() << "re-run with --skill NAME[,NAME] or --all\n";
            return 0;
        }
        int ok = 0;
        for (const Candidate &c : cands)
            if ((all || want.contains(c.name)) && st.install(c)) ++ok;
        out() << ok << " installed\n";
        return ok ? 0 : 1;
    }

    if (cmd == QLatin1String("remove")) {
        if (rest.isEmpty()) { usage(); return 2; }
        return st.removeSkill(rest.first()) ? 0 : 1;
    }

    if (cmd == QLatin1String("track")) {
        if (rest.size() < 2) { usage(); return 2; }
        QString e;
        if (!st.track(rest[0], rest[1], rest.value(2), &e)) { err() << e << '\n'; err().flush(); return 1; }
        return 0;
    }

    if (cmd == QLatin1String("uninstall")) {
        const bool dry = rest.contains(QStringLiteral("--dry-run"));
        const bool keepCache = rest.contains(QStringLiteral("--keep-cache"));
        if (!dry && !rest.contains(QStringLiteral("--yes"))) {
            out() << "This removes the skillhub units, installed files below $HOME and its cache. Your skills stay.\n"
                     "Preview with --dry-run. Continue? [y/N] ";
            out().flush();
            QTextStream in(stdin);
            const QString a = in.readLine().trimmed().toLower();
            if (a != QLatin1String("y") && a != QLatin1String("yes")) { out() << "aborted\n"; return 1; }
        }
        return st.uninstall(dry, keepCache) ? 0 : 1;
    }

    if (cmd == QLatin1String("autoupdate") || cmd == QLatin1String("watch")) {
        if (!platform::automationSupported()) {
            err() << "skillhub " << cmd << " is not available on this platform: it needs systemd user units (Linux only)\n";
            err().flush();
            return 1;
        }
        const bool timer = cmd == QLatin1String("autoupdate");
        const QString unit = timer ? QStringLiteral("skillhub-update.timer") : QStringLiteral("skillhub-adopt.path");
        const QString what = rest.value(0);
        QString e;
        if (what == QLatin1String("on")) {
            if (!st.installUnits(timer, !timer, &e)) { err() << e << '\n'; err().flush(); return 1; }
        } else if (what == QLatin1String("off")) {
            st.removeUnits(timer, !timer, &e);
        }
        out() << unit << ": " << st.unitState(unit) << '\n';
        return 0;
    }

    usage();
    return cmd.isEmpty() || cmd == QLatin1String("help") || cmd == QLatin1String("--help") ? 0 : 2;
}

}  // namespace

int main(int argc, char **argv) {
    QStringList args;
    for (int i = 1; i < argc; ++i) args << QString::fromLocal8Bit(argv[i]);

    if (!args.isEmpty() && (args.first() == QLatin1String("--version") || args.first() == QLatin1String("version"))) {
        out() << "skillhub " << SKILLHUB_VERSION << '\n';
        out().flush();
        return 0;
    }

    if (args.isEmpty() || args.first() == QLatin1String("gui")) {
        // headless self-test: skillhub gui --shot out.png [--theme dark|light] [--size WxH] [--select NAME]
        //   [--mode skill|agent|add|providers|transfer] [--agent ID] [--filter all|updates|local|problems] [--search TEXT]
        //   [--discover OWNER/REPO] [--console] [--animate] [--action update-all|check|adopt [--busy]] [--settle TICKS] [--import FILE]
        const auto opt = [&](const char *name) {
            const int i = args.indexOf(QString::fromLatin1(name));
            return i >= 0 && i + 1 < args.size() ? args.at(i + 1) : QString();
        };
        const QString shotFile = opt("--shot");
        if (!shotFile.isEmpty()) {
            if (!opt("--theme").isEmpty()) qputenv("SKILLHUB_THEME", opt("--theme").toUtf8());
            if (!args.contains(QStringLiteral("--animate"))) qputenv("SKILLHUB_REDUCE_MOTION", "1");
            qputenv("SKILLHUB_NO_HOVER", "1");
        }
#if QT_VERSION < QT_VERSION_CHECK(6, 6, 0)
        {   // no curve renderer for shapes before Qt 6.6: smooth their edges by multisampling instead
            QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
            fmt.setSamples(4);
            QSurfaceFormat::setDefaultFormat(fmt);
        }
#endif
        QGuiApplication app(argc, argv);
        app.setApplicationName(QStringLiteral("skillhub"));
        app.setOrganizationName(QStringLiteral("skillhub"));
        app.setDesktopFileName(QStringLiteral("skillhub"));
        QIcon icon;
        for (int sz : {16, 32, 48, 64, 128, 256}) icon.addFile(QStringLiteral(":/icons/skillhub-%1.png").arg(sz));
        app.setWindowIcon(icon);
        QQuickStyle::setStyle(QStringLiteral("Basic"));

        AppController controller;
        QTimer::singleShot(500, &controller, &AppController::checkUpdatesQuietly);  // verdicts without a click
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("app"), &controller);
        if (!shotFile.isEmpty()) {
            const QStringList sz = opt("--size").split(QLatin1Char('x'));
            if (sz.size() == 2) engine.rootContext()->setContextProperty(QStringLiteral("startSize"), QSize(sz[0].toInt(), sz[1].toInt()));
        }
#if QT_VERSION < QT_VERSION_CHECK(6, 7, 0)
        (new QQmlFileSelector(&engine, &engine))->setExtraSelectors({QStringLiteral("legacy")});  // qml/+legacy: no font features before Qt 6.7
#endif
        engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
        if (engine.rootObjects().isEmpty()) return 1;
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());

        if (!shotFile.isEmpty() && window) {
            if (args.contains(QStringLiteral("--console"))) window->setProperty("consoleOpen", true);
            auto *tick = new QTimer(&app);
            auto ticks = std::make_shared<int>(0);
            auto armed = std::make_shared<int>(-1);
            QObject::connect(tick, &QTimer::timeout, &app, [&, tick, ticks, armed, shotFile, opt] {
                ++*ticks;
                const bool busyShot = !opt("--action").isEmpty() && args.contains(QStringLiteral("--busy"));
                if (*armed >= 0 && busyShot) {  // capture while the action is running
                    if (*ticks - *armed < 3) return;
                    window->grabWindow().save(shotFile);
                    tick->stop();
                    app.quit();
                    return;
                }
                if (*ticks < 4 || controller.busy()) return;
                if (*armed < 0) {  // apply the requested view once the window is idle
                    controller.filter()->setText(opt("--search"));
                    if (!opt("--filter").isEmpty()) controller.filter()->setMode(opt("--filter"));
                    if (!opt("--select").isEmpty()) controller.select(opt("--select"));
                    if (!opt("--agent").isEmpty()) controller.setFocusAgent(opt("--agent"));
                    if (opt("--mode") == QLatin1String("add") || opt("--mode") == QLatin1String("providers") || opt("--mode") == QLatin1String("transfer"))
                        controller.setDetailMode(opt("--mode"));
                    if (!opt("--import").isEmpty()) controller.importPreview(opt("--import"));
                    if (!opt("--discover").isEmpty()) {
                        controller.setDetailMode(QStringLiteral("add"));
                        controller.discover(opt("--discover"));
                    }
                    if (opt("--action") == QLatin1String("update-all")) controller.updateAll();
                    if (opt("--action") == QLatin1String("check")) controller.checkUpdates();
                    if (opt("--action") == QLatin1String("adopt")) controller.adopt();
                    *armed = *ticks;
                    return;
                }
                if (*ticks - *armed < (opt("--settle").isEmpty() ? 4 : opt("--settle").toInt())) return;  // let layout, transitions and toasts settle
                window->grabWindow().save(shotFile);
                tick->stop();
                app.quit();
            });
            tick->start(300);
        }
        return app.exec();
    }
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("skillhub"));
    return runCli(args);
}
