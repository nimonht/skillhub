import QtQuick

// Text with tabular numerals (counts and times line up). Needs font features, which exist from Qt 6.7;
// older Qt versions get qml/+legacy/NumText.qml instead (selected in main.cpp).
Text {
    font.features: { "tnum": 1 }
}
