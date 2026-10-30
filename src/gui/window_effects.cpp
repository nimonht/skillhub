#include "gui/window_effects.h"

#include <QGuiApplication>
#include <QOperatingSystemVersion>
#include <QQuickWindow>
#include <QSettings>

#ifdef Q_OS_WIN
#include <dwmapi.h>
#include <windows.h>
#endif
#ifdef SKILLHUB_HAVE_XCB
#include <xcb/xcb.h>
#endif

namespace window_effects {

#ifdef Q_OS_WIN
static bool windows_11() { return QOperatingSystemVersion::current() >= QOperatingSystemVersion(QOperatingSystemVersion::Windows, 10, 0, 22621); }
#endif

static bool x11_compositor() {
#ifdef SKILLHUB_HAVE_XCB
    auto *x11 = qApp->nativeInterface<QNativeInterface::QX11Application>();
    if (!x11 || !x11->connection()) return false;
    xcb_connection_t *c = x11->connection();
    auto reply = xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, 12, "_NET_WM_CM_S0"), nullptr);
    if (!reply) return false;
    auto owner = xcb_get_selection_owner_reply(c, xcb_get_selection_owner(c, reply->atom), nullptr);
    const bool have = owner && owner->owner != XCB_NONE;
    free(owner);
    free(reply);
    return have;
#else
    return false;
#endif
}

bool canBeTranslucent() {
    if (qEnvironmentVariableIsSet("SKILLHUB_FORCE_TRANSLUCENT")) return true;  // tests, on a platform with no compositor
#if defined(Q_OS_WIN)
    QSettings personalize(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"), QSettings::NativeFormat);
    return windows_11() && personalize.value(QStringLiteral("EnableTransparency"), 1).toInt() != 0;
#elif defined(Q_OS_MACOS)
    return !window_effects_reduce_transparency_mac();
#else
    if (QGuiApplication::platformName() == QLatin1String("xcb")) return x11_compositor();
    return QGuiApplication::platformName() != QLatin1String("offscreen");
#endif
}

void prepare() {
    if (canBeTranslucent()) QQuickWindow::setDefaultAlphaBuffer(true);
}

void apply(QQuickWindow *window, const QString &material, bool dark) {
    // a real window only: on the offscreen platform of the tests winId() is no handle at all
#if defined(Q_OS_WIN)
    if (QGuiApplication::platformName() != QLatin1String("windows")) return;
    const auto hwnd = reinterpret_cast<HWND>(window->winId());
    const BOOL immersive = dark;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &immersive, sizeof immersive);
    if (!windows_11()) return;
    const bool see_through = material != QLatin1String("solid");
    const int backdrop = see_through ? 3 /* DWMSBT_TRANSIENTWINDOW */ : 1 /* DWMSBT_NONE */;
    DwmSetWindowAttribute(hwnd, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */, &backdrop, sizeof backdrop);
    const MARGINS margins = see_through ? MARGINS{-1, -1, -1, -1} : MARGINS{0, 0, 0, 0};
    DwmExtendFrameIntoClientArea(hwnd, &margins);
#elif defined(Q_OS_MACOS)
    (void)dark;
    if (QGuiApplication::platformName() != QLatin1String("cocoa")) return;
    window_effects_apply_mac(window, material == QLatin1String("glass") ? 2 : material == QLatin1String("blur") ? 1 : 0);
#else
    (void)window;
    (void)material;
    (void)dark;  // the compositor blurs a translucent window when it is set up to
#endif
}

QString note(const QString &material) {
    if (material == QLatin1String("solid")) return QStringLiteral("Solid looks the same everywhere.");
#if defined(Q_OS_WIN)
    return windows_11() ? QStringLiteral("Windows 11 draws the blur.") : QStringLiteral("Blur needs Windows 11, so this stays solid here.");
#elif defined(Q_OS_MACOS)
    return material == QLatin1String("glass") ? QStringLiteral("macOS 26 draws Liquid Glass; older versions draw the blur.") : QStringLiteral("macOS draws the blur.");
#else
    if (QGuiApplication::platformName() == QLatin1String("xcb") && !x11_compositor()) return QStringLiteral("This desktop runs no compositor, so the window stays solid.");
    const QString desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP").toLower();
    if (desktop.contains(QLatin1String("hyprland"))) return QStringLiteral("Hyprland blurs what is behind the window when decoration:blur is on, which it is by default.");
    if (desktop.contains(QLatin1String("gnome"))) return QStringLiteral("GNOME does not blur behind windows, so the window is translucent without blur.");
    if (desktop.contains(QLatin1String("kde"))) return QStringLiteral("KDE blurs only windows that ask through its own protocol, so here the window is translucent without blur.");
    return QStringLiteral("Your compositor decides: with blur set up it blurs what is behind the window, otherwise the window is just translucent.");
#endif
}

}  // namespace window_effects
