// skillhub-gui: the window. The command line is src/cli, and the window's backend is src/serve.
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlFileSelector>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QIcon>
#include <QSurfaceFormat>
#include <QTextStream>
#include <QTimer>

#include <cstdio>
#include <memory>

#include "app.h"
#include "gui/terminal/vterm_item.h"
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
#ifdef Q_OS_WIN
        qputenv("PATH", (QDir::toNativeSeparators(QCoreApplication::applicationDirPath()) + QLatin1Char(';') + qEnvironmentVariable("PATH")).toLocal8Bit());  // shells started from here find skillhub-cli
#endif
        app.setApplicationName(QStringLiteral("skillhub"));
        app.setOrganizationName(QStringLiteral("skillhub"));
        app.setDesktopFileName(QStringLiteral("skillhub"));
        QIcon icon;
        for (int sz : {16, 32, 48, 64, 128, 256}) icon.addFile(QStringLiteral(":/icons/skillhub-%1.png").arg(sz));
        app.setWindowIcon(icon);
        QQuickStyle::setStyle(QStringLiteral("Basic"));

        qmlRegisterType<VTermItem>("SkillHub.Terminal", 1, 0, "VTermItem");
        window_effects::prepare();
        AppController controller;
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
        if (shotFile.isEmpty() && controller.tourNeeded()) QTimer::singleShot(400, &controller, [&controller] { emit controller.startTour(0, 0); });

        if (!shotFile.isEmpty() && window) {
            if (args.contains(QStringLiteral("--console"))) window->setProperty("consoleOpen", true);
            auto *tick = new QTimer(&app);
            auto ticks = std::make_shared<int>(0);
            auto armed = std::make_shared<int>(-1);
            auto typed = std::make_shared<bool>(false);
            QObject::connect(tick, &QTimer::timeout, &app, [&, tick, ticks, armed, typed, shotFile, opt] {
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
                    if (opt("--mode") == QLatin1String("onboarding")) emit controller.startTour(opt("--step").toInt(), opt("--coach").toInt());
                    if (opt("--mode") == QLatin1String("terminal")) emit controller.showTerminal();
                    if (opt("--mode") == QLatin1String("settings")) emit controller.showSettings(opt("--section").isEmpty() ? QStringLiteral("appearance") : opt("--section"));
                    if (opt("--action") == QLatin1String("update-all")) controller.updateAll();
                    if (opt("--action") == QLatin1String("check")) controller.checkUpdates();
                    if (opt("--action") == QLatin1String("adopt")) controller.adopt();
                    *armed = *ticks;
                    return;
                }
                if (*ticks - *armed < (opt("--settle").isEmpty() ? 4 : opt("--settle").toInt())) return;  // let layout, transitions and toasts settle
                if (opt("--mode") == QLatin1String("terminal") && !*typed) {   // a real round trip through the shell, then a look at the screen
                    *typed = true;
                    QMetaObject::invokeMethod(window, "typeInTerminal", Q_ARG(QVariant, opt("--terminal-run") + QStringLiteral("\r")));
                    *armed = *ticks;
                    return;
                }
                if (!opt("--terminal-dump").isEmpty()) {
                    QVariant text;
                    QMetaObject::invokeMethod(window, "terminalScreen", Q_RETURN_ARG(QVariant, text));
                    QFile dump(opt("--terminal-dump"));
                    if (dump.open(QIODevice::WriteOnly)) dump.write(text.toString().toUtf8());
                }
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
