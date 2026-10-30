// The font and colours of the terminals people already set up, so the built-in one looks like theirs.
#pragma once

#include <QVariantMap>

// `from`: kitty, ghostty or alacritty. {fontFamily, fontSize, foreground, background, palette: [16 colours]}; empty when there is
// no such config, or nothing in it.
QVariantMap importTerminalLook(const QString &from);
