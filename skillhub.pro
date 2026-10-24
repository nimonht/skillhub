QT += core gui qml quick quickcontrols2 concurrent
CONFIG += c++17 warn_on
TEMPLATE = app
TARGET = skillhub
VERSION = 1.0.0
DEFINES += SKILLHUB_VERSION=\\\"$$VERSION\\\"

SOURCES += src/main.cpp src/store.cpp src/providers.cpp src/transfer.cpp src/platform.cpp src/app.cpp
HEADERS += src/store.h src/platform.h src/app.h
RESOURCES += resources.qrc

win32 {
    RC_ICONS = branding/favicon.ico
    QMAKE_TARGET_PRODUCT = Skill Hub
    QMAKE_TARGET_DESCRIPTION = One skills folder for every coding agent
    msvc: QMAKE_CXXFLAGS += /utf-8
}

macx {
    ICON = data/skillhub.icns
    QMAKE_INFO_PLIST = data/macos/Info.plist
    QMAKE_APPLE_DEVICE_ARCHS = x86_64 arm64        # universal, when Qt itself is (the official packages are)
}

# Linux: qmake6 PREFIX=/usr && make && make install      (default: ~/.local)
unix:!macx {
    isEmpty(PREFIX): PREFIX = $$(HOME)/.local

    target.path = $$PREFIX/bin
    desktop.files = data/skillhub.desktop
    desktop.path = $$PREFIX/share/applications
    metainfo.files = data/io.github.nimonht.skillhub.metainfo.xml
    metainfo.path = $$PREFIX/share/metainfo
    INSTALLS += target desktop metainfo

    # One rule per icon file: `make uninstall` then deletes only these files, never the shared hicolor directory.
    ICON_SIZES = 16 24 32 48 64 128 256 512
    for(sz, ICON_SIZES) {
        eval(icon$${sz}.files = data/icons/hicolor/$${sz}x$${sz}/apps/skillhub.png)
        eval(icon$${sz}.path = $$PREFIX/share/icons/hicolor/$${sz}x$${sz}/apps)
        INSTALLS += icon$${sz}
    }
    icon_svg.files = data/icons/hicolor/scalable/apps/skillhub.svg
    icon_svg.path = $$PREFIX/share/icons/hicolor/scalable/apps
    INSTALLS += icon_svg
}
