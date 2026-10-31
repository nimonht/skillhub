// A terminal on a Qt Quick item: libvterm keeps the screen, a PTY runs the shell, and this draws the cells.
#pragma once

#include <QColor>
#include <QFont>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QThread>
#include <QVector>

extern "C" {
#include <vterm.h>

#include "sh_pty.h"
}

class PtyReader : public QThread {
    Q_OBJECT
public:
    explicit PtyReader(sh_pty *pty) : pty_(pty) {}
signals:
    void received(const QByteArray &data);
    void ended();
protected:
    void run() override;
private:
    sh_pty *pty_;
};

class VTermItem : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QString program MEMBER program_ NOTIFY settingsChanged)
    Q_PROPERTY(QStringList arguments MEMBER arguments_ NOTIFY settingsChanged)
    Q_PROPERTY(QString workingDirectory MEMBER workingDirectory_ NOTIFY settingsChanged)
    Q_PROPERTY(QString fontFamily READ fontFamily WRITE setFontFamily NOTIFY settingsChanged)
    Q_PROPERTY(int fontSize READ fontSize WRITE setFontSize NOTIFY settingsChanged)
    Q_PROPERTY(QColor foreground READ foreground WRITE setForeground NOTIFY settingsChanged)
    Q_PROPERTY(QColor background READ background WRITE setBackground NOTIFY settingsChanged)
    Q_PROPERTY(QColor cursorColor MEMBER cursorColor_ NOTIFY settingsChanged)
    Q_PROPERTY(QColor selectionColor MEMBER selectionColor_ NOTIFY settingsChanged)
    Q_PROPERTY(QString cursorShape MEMBER cursorShape_ NOTIFY settingsChanged)   // block, bar or underline
    Q_PROPERTY(int scrollback MEMBER scrollbackMax_ NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList palette WRITE setPalette READ paletteList NOTIFY settingsChanged)  // 16 colours, empty: libvterm's own
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(QString currentDirectory READ currentDirectory NOTIFY titleChanged)
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(QString error READ error NOTIFY runningChanged)
public:
    explicit VTermItem(QQuickItem *parent = nullptr);
    ~VTermItem() override;

    Q_INVOKABLE void start();
    Q_INVOKABLE void feed(const QString &text);    // shown as if the shell had said it; the banner comes this way
    Q_INVOKABLE void type(const QString &text);    // as if typed
    Q_INVOKABLE void copy();
    void paste();
    Q_INVOKABLE QString screenText() const;        // what is on screen, for tests

    QString fontFamily() const { return font_.family(); }
    void setFontFamily(const QString &family);
    int fontSize() const { return font_.pixelSize(); }
    void setFontSize(int px);
    QColor foreground() const { return foreground_; }
    void setForeground(const QColor &c);
    QColor background() const { return background_; }
    void setBackground(const QColor &c);
    QVariantList paletteList() const;
    void setPalette(const QVariantList &colors);
    QString title() const { return title_; }
    QString currentDirectory() const { return cwd_; }
    bool running() const { return pty_ != nullptr; }
    QString error() const { return error_; }

    void paint(QPainter *painter) override;

signals:
    void settingsChanged();
    void titleChanged();
    void runningChanged();

protected:
    void geometryChange(const QRectF &now, const QRectF &before) override;
    void keyPressEvent(QKeyEvent *e) override;
    void inputMethodEvent(QInputMethodEvent *e) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void focusInEvent(QFocusEvent *e) override;
    void focusOutEvent(QFocusEvent *e) override;

private:
    struct Line { QVector<VTermScreenCell> cells; };
    static int onDamage(VTermRect rect, void *user);
    static int onMoveCursor(VTermPos pos, VTermPos old, int visible, void *user);
    static int onProp(VTermProp prop, VTermValue *val, void *user);
    static int onBell(void *user);
    static int onPushLine(int cols, const VTermScreenCell *cells, void *user);
    static int onPopLine(int cols, VTermScreenCell *cells, void *user);
    static int onOsc(int command, VTermStringFragment frag, void *user);
    static void onOutput(const char *s, size_t len, void *user);

    void measure();
    void resizeGrid();
    void received(const QByteArray &data);
    void ended();
    void send(const QByteArray &data);
    QPoint cellAt(const QPointF &p) const;
    QColor resolve(VTermColor c, bool fg) const;
    VTermScreenCell cellAt(int row, int col) const;
    bool selected(int row, int col) const;
    void applyColors();

    VTerm *vt_ = nullptr;
    VTermScreen *screen_ = nullptr;
    VTermState *state_ = nullptr;
    sh_pty *pty_ = nullptr;
    PtyReader *reader_ = nullptr;
    int cols_ = 80, rows_ = 24;
    qreal cellW_ = 8, cellH_ = 16, ascent_ = 12;
    QFont font_;
    QColor foreground_ = QColor(0xE8, 0xEC, 0xF1), background_ = QColor(0x10, 0x14, 0x1A), cursorColor_ = QColor(0x34, 0xE0, 0xA6), selectionColor_ = QColor(0x3A, 0x44, 0x52);
    QString cursorShape_ = QStringLiteral("block");
    QVector<QColor> palette_;
    QString program_, workingDirectory_, title_, cwd_, error_;
    QStringList arguments_;
    QByteArray osc_;
    int scrollbackMax_ = 10000, scrolled_ = 0;
    QVector<Line> scrollback_;
    VTermPos cursor_ = {0, 0};
    bool cursorVisible_ = true, focused_ = false, selecting_ = false, altScreen_ = false, flash_ = false;
    QPoint selStart_{-1, -1}, selEnd_{-1, -1};
};
