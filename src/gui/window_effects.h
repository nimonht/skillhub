// What the window asks of the system so that the desktop shows through: native blur where it exists. Solid is always right.
#pragma once

#include <QString>

class QQuickWindow;

namespace window_effects {
bool canBeTranslucent();                                                     // false: no compositor, or the system says not to
void prepare();                                                              // before the first window exists
void apply(QQuickWindow *window, const QString &material, bool dark);        // solid, blur or glass
QString note(const QString &material);                                       // what this desktop does with it, in a sentence
}

#ifdef Q_OS_MACOS
void window_effects_apply_mac(QQuickWindow *window, int kind);               // 0 none, 1 blur, 2 glass
bool window_effects_reduce_transparency_mac();
#endif
