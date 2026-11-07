// A real VTermItem in a window, with real key and wheel events: the terminal keeps the keyboard and scrolls.
// Run: ctest -R terminal   (QT_QPA_PLATFORM=offscreen is set by the test)
#include <QQmlEngine>
#include <QtQuickTest/quicktest.h>

#include "gui/terminal/vterm_item.h"

class Setup : public QObject {
    Q_OBJECT
public slots:
    void qmlEngineAvailable(QQmlEngine *) { qmlRegisterType<VTermItem>("SkillHub.Terminal", 1, 0, "VTermItem"); }
};

QUICK_TEST_MAIN_WITH_SETUP(terminal, Setup)

#include "term_quicktest.moc"
