// skillhub: GUI when started without a command, CLI otherwise (used by the systemd units).
#include <QSettings>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlFileSelector>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QElapsedTimer>
#include <QIcon>
#include <QSurfaceFormat>
#include <QTextStream>
#include <QTimer>

#include <cstdio>
#include <memory>

#include "app.h"
#include "gui/window_effects.h"

#ifndef SKILLHUB_VERSION
#define SKILLHUB_VERSION "dev"
#endif

namespace {

QTextStream &out() {
    static QTextStream s(stdout);
    return s;
}
QTextStream &err() {
    static QTextStream s(stderr);
    return s;
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
        //   [--mode skill|agent|add|providers|transfer|review] [--scope FOLDER] [--agent ID] [--filter all|updates|local|problems] [--search TEXT]
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
        {   // the scale has to be set before the application object exists
            const QVariant scale = QSettings(QStringLiteral("skillhub"), QStringLiteral("skillhub")).value(QStringLiteral("ui/scale"));
            if (scale.isValid() && scale.toDouble() > 0 && !qEnvironmentVariableIsSet("QT_SCALE_FACTOR")) qputenv("QT_SCALE_FACTOR", scale.toByteArray());
        }
        QGuiApplication app(argc, argv);
        app.setApplicationName(QStringLiteral("skillhub"));
        app.setOrganizationName(QStringLiteral("skillhub"));
        app.setDesktopFileName(QStringLiteral("skillhub"));
        QIcon icon;
        for (int sz : {16, 32, 48, 64, 128, 256}) icon.addFile(QStringLiteral(":/icons/skillhub-%1.png").arg(sz));
        app.setWindowIcon(icon);
        QQuickStyle::setStyle(QStringLiteral("Basic"));

        window_effects::prepare();
        AppController controller;
        if (!args.contains(QStringLiteral("--bench")))
            QTimer::singleShot(500, &controller, &AppController::checkUpdatesQuietly);  // verdicts without a click
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("app"), &controller);
        if (!shotFile.isEmpty()) {
            const QStringList sz = opt("--size").split(QLatin1Char('x'));
            if (sz.size() == 2) engine.rootContext()->setContextProperty(QStringLiteral("startSize"), QSize(sz[0].toInt(), sz[1].toInt()));
            engine.rootContext()->setContextProperty(QStringLiteral("startLayout"), opt("--layout"));
        }
#if QT_VERSION < QT_VERSION_CHECK(6, 7, 0)
        (new QQmlFileSelector(&engine, &engine))->setExtraSelectors({QStringLiteral("legacy")});  // qml/+legacy: no font features before Qt 6.7
#endif
        engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
        if (engine.rootObjects().isEmpty()) return 1;
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());

        // skillhub gui --bench: the longest the interface thread goes without running its event loop, first while the
        // first check's delta changes every row, then while everything available is updated (bench/bench_gui.py)
        if (args.contains(QStringLiteral("--bench"))) {
            auto *probe = new QTimer(&app);
            auto clock = std::make_shared<QElapsedTimer>();
            auto worst = std::make_shared<qint64>(0);
            auto phase = std::make_shared<int>(0);
            auto results = std::make_shared<QStringList>();
            auto quiet = std::make_shared<QElapsedTimer>();  // idle since: the delta after a job arrives a moment after it ends
            // Headless, Qt Quick lays out and draws on this same thread. The gaps include that; the longest layout pass
            // (after animating, before synchronizing) and the longest draw are reported on their own as well.
            auto frame = std::make_shared<QElapsedTimer>();
            auto worstPolish = std::make_shared<qint64>(0), worstDraw = std::make_shared<qint64>(0);
            QObject::connect(window, &QQuickWindow::afterAnimating, &app, [=] { frame->start(); }, Qt::DirectConnection);
            QObject::connect(window, &QQuickWindow::beforeSynchronizing, &app, [=] {
                if (frame->isValid()) *worstPolish = qMax(*worstPolish, frame->restart());
            }, Qt::DirectConnection);
            QObject::connect(window, &QQuickWindow::afterRendering, &app, [=] {
                if (frame->isValid()) *worstDraw = qMax(*worstDraw, frame->elapsed());
                frame->invalidate();
            }, Qt::DirectConnection);
            probe->setTimerType(Qt::PreciseTimer);
            auto worstBusy = std::make_shared<qint64>(0);  // gaps while the backend's job runs: the CPU is shared with it
            QObject::connect(probe, &QTimer::timeout, &app, [=, &controller, &app] {
                const qint64 gap = clock->restart();
                if (controller.busy()) *worstBusy = qMax(*worstBusy, gap);
                else *worst = qMax(*worst, gap);
                if (!controller.idle()) {
                    quiet->invalidate();
                    return;
                }
                if (!quiet->isValid()) quiet->start();
                if (*phase > 0 && quiet->elapsed() < 1500) return;
                quiet->invalidate();
                if (*phase == 1 || *phase == 2)
                    *results << QStringLiteral("%1 %2 ms (while the job ran %5 ms; layout up to %3 ms, drawing up to %4 ms)")
                                    .arg(*phase == 1 ? QStringLiteral("check") : QStringLiteral("update")).arg(*worst).arg(*worstPolish).arg(*worstDraw).arg(*worstBusy);
                *worstPolish = *worstDraw = *worstBusy = 0;
                if (*phase == 0) controller.checkUpdates();
                else if (*phase == 1) controller.updateAll();
                else {
                    out() << "max UI stall: " << results->join(QStringLiteral(", ")) << '\n';
                    out().flush();
                    app.quit();
                    return;
                }
                ++*phase;
                *worst = 0;
            });
            QTimer::singleShot(0, &app, [=] {
                clock->start();
                probe->start(2);
            });
        }

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
                if (*ticks < 4 || !controller.idle()) return;  // the backend's first snapshot is in and no job runs
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
                    if (!opt("--scope").isEmpty()) controller.setScope(opt("--scope"));
                    if (opt("--action") == QLatin1String("scopes")) emit controller.showScopeMenu();
                    if (opt("--mode") == QLatin1String("review")) controller.reviewAll();
                    if (opt("--mode") == QLatin1String("settings")) emit controller.showSettings(opt("--section").isEmpty() ? QStringLiteral("appearance") : opt("--section"));
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
    // the command line is `skillhub` (src/cli); this program is only the window
    err() << "skillhub-gui is Skill Hub's window: run it without arguments, or use the `skillhub` command line\n";
    err().flush();
    return 2;
}
