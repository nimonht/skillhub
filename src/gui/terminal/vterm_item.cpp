#include "gui/terminal/vterm_item.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>

static constexpr int OSC_MAX = 4096;  // a title or a folder: whatever a program sends beyond this is noise

void PtyReader::run() {
    char buf[65536];
    for (;;) {
        room_.acquire();
        const long n = sh_pty_read(pty_, buf, sizeof buf);
        if (n <= 0) break;
        emit received(QByteArray(buf, int(n)));
    }
    emit ended();
}

VTermItem::VTermItem(QQuickItem *parent) : QQuickPaintedItem(parent) {
    setFlag(ItemAcceptsInputMethod, true);
    setFlag(ItemIsFocusScope, true);
    setActiveFocusOnTab(true);
    setAcceptedMouseButtons(Qt::LeftButton);
    font_ = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font_.setPixelSize(14);
    font_.setStyleHint(QFont::Monospace);
    vt_ = vterm_new(rows_, cols_);
    vterm_set_utf8(vt_, 1);
    vterm_output_set_callback(vt_, onOutput, this);
    screen_ = vterm_obtain_screen(vt_);
    state_ = vterm_obtain_state(vt_);
    static const VTermScreenCallbacks callbacks = {onDamage, nullptr, onMoveCursor, onProp, onBell, nullptr, onPushLine, onPopLine, nullptr};
    vterm_screen_set_callbacks(screen_, &callbacks, this);
    static const VTermStateFallbacks fallbacks = {nullptr, nullptr, onOsc, nullptr, nullptr, nullptr};
    vterm_screen_set_unrecognised_fallbacks(screen_, &fallbacks, this);
    vterm_screen_set_damage_merge(screen_, VTERM_DAMAGE_SCROLL);
    vterm_screen_enable_altscreen(screen_, 1);
    vterm_screen_reset(screen_, 1);
    applyColors();
    measure();
}

VTermItem::~VTermItem() {
    if (pty_) {
        sh_pty_hangup(pty_);
        if (reader_) {
            reader_->disconnect(this);
            reader_->consumed();  // it may be waiting for room, not on the terminal
            if (!reader_->wait(1000)) reader_->terminate();
            reader_->wait();
            delete reader_;
        }
        sh_pty_free(pty_);
    }
    vterm_free(vt_);
}

void VTermItem::measure() {
    const QFontMetricsF m(font_);
    cellW_ = m.horizontalAdvance(QLatin1Char('M'));
    cellH_ = qCeil(m.height());
    ascent_ = m.ascent();
    resizeGrid();
}

void VTermItem::resizeGrid() {
    const int cols = qMax(2, int(width() / cellW_)), rows = qMax(1, int(height() / cellH_));
    if (cols == cols_ && rows == rows_) return;
    cols_ = cols;
    rows_ = rows;
    vterm_set_size(vt_, rows_, cols_);
    vterm_screen_flush_damage(screen_);
    if (pty_) sh_pty_resize(pty_, cols_, rows_);
    update();
}

void VTermItem::geometryChange(const QRectF &now, const QRectF &before) {
    QQuickPaintedItem::geometryChange(now, before);
    resizeGrid();
}

void VTermItem::setFontFamily(const QString &family) {
    if (family.isEmpty() || family == font_.family()) return;
    font_.setFamily(family);
    measure();
    emit settingsChanged();
}

void VTermItem::setFontSize(int px) {
    if (px <= 0 || px == font_.pixelSize()) return;
    font_.setPixelSize(px);
    measure();
    emit settingsChanged();
}

void VTermItem::setForeground(const QColor &c) {
    foreground_ = c;
    applyColors();
    emit settingsChanged();
}

void VTermItem::setBackground(const QColor &c) {
    background_ = c;
    applyColors();
    emit settingsChanged();
}

QVariantList VTermItem::paletteList() const {
    QVariantList out;
    for (const QColor &c : palette_) out << c.name();
    return out;
}

void VTermItem::setPalette(const QVariantList &colors) {
    palette_.clear();
    for (const QVariant &v : colors) palette_ << QColor(v.toString());
    applyColors();
    emit settingsChanged();
}

void VTermItem::applyColors() {
    VTermColor fg, bg;
    vterm_color_rgb(&fg, foreground_.red(), foreground_.green(), foreground_.blue());
    vterm_color_rgb(&bg, background_.red(), background_.green(), background_.blue());
    vterm_state_set_default_colors(state_, &fg, &bg);
    for (int i = 0; i < 16 && i < palette_.size(); ++i) {
        VTermColor c;
        vterm_color_rgb(&c, palette_[i].red(), palette_[i].green(), palette_[i].blue());
        vterm_state_set_palette_color(state_, i, &c);
    }
    update();
}

void VTermItem::start() {
    if (pty_ || program_.isEmpty()) return;
    QVector<QByteArray> args{program_.toUtf8()};
    for (const QString &a : arguments_) args << a.toUtf8();
    QVector<const char *> argv;
    for (const QByteArray &a : args) argv << a.constData();
    argv << nullptr;
    const char *const env[] = {"TERM=xterm-256color", "COLORTERM=truecolor", "SKILLHUB_TERMINAL=1", nullptr};
    char *err = nullptr;
    const QByteArray program = program_.toUtf8(), cwd = workingDirectory_.toUtf8();
    pty_ = sh_pty_spawn(program.constData(), argv.constData(), cwd.constData(), env, cols_, rows_, &err);
    if (!pty_) {
        error_ = QString::fromUtf8(err ? err : "");
        free(err);
        feed(error_ + QLatin1String("\r\n"));
        emit runningChanged();
        return;
    }
    reader_ = new PtyReader(pty_);
    connect(reader_, &PtyReader::received, this, &VTermItem::received, Qt::QueuedConnection);
    connect(reader_, &PtyReader::ended, this, &VTermItem::ended, Qt::QueuedConnection);
    reader_->start();
    emit runningChanged();
}

void VTermItem::received(const QByteArray &data) {
    vterm_input_write(vt_, data.constData(), size_t(data.size()));
    vterm_screen_flush_damage(screen_);
    if (reader_) reader_->consumed();
}

void VTermItem::ended() {
    reader_->wait();
    delete reader_;
    reader_ = nullptr;
    sh_pty_free(pty_);
    pty_ = nullptr;
    emit runningChanged();
}

void VTermItem::feed(const QString &text) {
    const QByteArray bytes = text.toUtf8();
    vterm_input_write(vt_, bytes.constData(), size_t(bytes.size()));
    vterm_screen_flush_damage(screen_);
}

void VTermItem::type(const QString &text) { send(text.toUtf8()); }

void VTermItem::send(const QByteArray &data) {
    if (pty_) sh_pty_write(pty_, data.constData(), size_t(data.size()));
}

void VTermItem::onOutput(const char *s, size_t len, void *user) {
    static_cast<VTermItem *>(user)->send(QByteArray(s, int(len)));
}

int VTermItem::onDamage(VTermRect rect, void *user) {
    auto *self = static_cast<VTermItem *>(user);
    self->update(QRectF(rect.start_col * self->cellW_, rect.start_row * self->cellH_, (rect.end_col - rect.start_col) * self->cellW_, (rect.end_row - rect.start_row) * self->cellH_).toAlignedRect());
    return 1;
}

int VTermItem::onMoveCursor(VTermPos pos, VTermPos old, int visible, void *user) {
    auto *self = static_cast<VTermItem *>(user);
    self->cursor_ = pos;
    self->cursorVisible_ = visible;
    self->scrolled_ = 0;
    self->update(QRectF(old.col * self->cellW_, old.row * self->cellH_, self->cellW_ * 2, self->cellH_).toAlignedRect());
    self->update(QRectF(pos.col * self->cellW_, pos.row * self->cellH_, self->cellW_ * 2, self->cellH_).toAlignedRect());
    return 1;
}

int VTermItem::onProp(VTermProp prop, VTermValue *val, void *user) {
    auto *self = static_cast<VTermItem *>(user);
    if (prop == VTERM_PROP_TITLE && val->string.initial) self->title_.clear();
    if (prop == VTERM_PROP_TITLE) {
        if (self->title_.size() < OSC_MAX) self->title_ += QString::fromUtf8(val->string.str, int(val->string.len));
        if (val->string.final) emit self->titleChanged();
    } else if (prop == VTERM_PROP_CURSORVISIBLE) {
        self->cursorVisible_ = val->boolean;
    } else if (prop == VTERM_PROP_ALTSCREEN) {
        self->altScreen_ = val->boolean;
    } else if (prop == VTERM_PROP_CURSORSHAPE) {
        self->cursorShape_ = val->number == VTERM_PROP_CURSORSHAPE_UNDERLINE ? QStringLiteral("underline") : val->number == VTERM_PROP_CURSORSHAPE_BAR_LEFT ? QStringLiteral("bar") : QStringLiteral("block");
    }
    return 1;
}

int VTermItem::onBell(void *user) {   // a flash, not a sound
    auto *self = static_cast<VTermItem *>(user);
    self->flash_ = true;
    self->update();
    QTimer::singleShot(90, self, [self] {
        self->flash_ = false;
        self->update();
    });
    return 1;
}

int VTermItem::onPushLine(int cols, const VTermScreenCell *cells, void *user) {
    auto *self = static_cast<VTermItem *>(user);
    Line line;
    line.cells = QVector<VTermScreenCell>(cells, cells + cols);
    self->scrollback_.append(line);
    if (self->scrollback_.size() > self->scrollbackMax_) self->scrollback_.removeFirst();
    return 1;
}

int VTermItem::onPopLine(int cols, VTermScreenCell *cells, void *user) {
    auto *self = static_cast<VTermItem *>(user);
    if (self->scrollback_.isEmpty()) return 0;
    const Line line = self->scrollback_.takeLast();
    for (int i = 0; i < cols; ++i) {
        if (i < line.cells.size()) {
            cells[i] = line.cells[i];
        } else {
            memset(&cells[i], 0, sizeof cells[i]);
            cells[i].width = 1;
            cells[i].fg = cells[0].fg;
            cells[i].bg = cells[0].bg;
        }
    }
    return 1;
}

// OSC 7: the shell says where it is, as file://host/path
int VTermItem::onOsc(int command, VTermStringFragment frag, void *user) {
    auto *self = static_cast<VTermItem *>(user);
    if (command != 7) return 0;
    if (frag.initial) self->osc_.clear();
    if (self->osc_.size() < OSC_MAX) self->osc_.append(frag.str, int(frag.len));
    if (frag.final) {
        self->cwd_ = QUrl(QString::fromUtf8(self->osc_)).toLocalFile();
        emit self->titleChanged();
    }
    return 1;
}

VTermScreenCell VTermItem::cellAt(int row, int col) const {
    VTermScreenCell cell;
    memset(&cell, 0, sizeof cell);
    cell.width = 1;
    const int shown = row - scrolled_;
    if (shown < 0) {
        const int index = scrollback_.size() + shown;
        if (index >= 0 && index < scrollback_.size() && col < scrollback_[index].cells.size()) return scrollback_[index].cells[col];
        vterm_color_rgb(&cell.fg, foreground_.red(), foreground_.green(), foreground_.blue());
        vterm_color_rgb(&cell.bg, background_.red(), background_.green(), background_.blue());
        return cell;
    }
    vterm_screen_get_cell(screen_, VTermPos{shown, col}, &cell);
    return cell;
}

QColor VTermItem::resolve(VTermColor c, bool fg) const {
    if (VTERM_COLOR_IS_DEFAULT_FG(&c)) return foreground_;
    if (VTERM_COLOR_IS_DEFAULT_BG(&c)) return background_;
    vterm_screen_convert_color_to_rgb(screen_, &c);
    Q_UNUSED(fg)
    return QColor(c.rgb.red, c.rgb.green, c.rgb.blue);
}

bool VTermItem::selected(int row, int col) const {
    if (selStart_.x() < 0) return false;
    QPoint a = selStart_, b = selEnd_;
    if (std::make_pair(a.y(), a.x()) > std::make_pair(b.y(), b.x())) std::swap(a, b);
    if (row < a.y() || row > b.y()) return false;
    if (row == a.y() && col < a.x()) return false;
    if (row == b.y() && col > b.x()) return false;
    return true;
}

void VTermItem::paint(QPainter *p) {
    p->fillRect(boundingRect(), flash_ ? selectionColor_ : background_);
    p->setFont(font_);
    for (int row = 0; row < rows_; ++row) {
        const qreal y = row * cellH_;
        int col = 0;
        while (col < cols_) {
            VTermScreenCell cell = cellAt(row, col);
            const int span = qMax(1, int(cell.width));
            QColor fg = resolve(cell.fg, true), bg = resolve(cell.bg, false);
            if (cell.attrs.reverse) std::swap(fg, bg);
            if (selected(row, col)) bg = selectionColor_;
            const QRectF box(col * cellW_, y, span * cellW_, cellH_);
            if (bg != background_) p->fillRect(box, bg);
            if (cell.chars[0] == 0x2588 || cell.chars[0] == 0x2580 || cell.chars[0] == 0x2584) {   // blocks are drawn: fonts leave seams between them
                const qreal half = qRound(cellH_ / 2);
                p->fillRect(cell.chars[0] == 0x2588 ? box : cell.chars[0] == 0x2580 ? QRectF(box.x(), y, box.width(), half) : QRectF(box.x(), y + half, box.width(), cellH_ - half), fg);
            } else if (cell.chars[0] && cell.chars[0] != ' ') {
                QString text;
                for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; ++i) text += QString::fromUcs4(reinterpret_cast<const char32_t *>(&cell.chars[i]), 1);
                QFont f = font_;
                f.setBold(cell.attrs.bold);
                f.setItalic(cell.attrs.italic);
                f.setUnderline(cell.attrs.underline != 0);
                f.setStrikeOut(cell.attrs.strike);
                if (f != p->font()) p->setFont(f);
                p->setPen(fg);
                p->drawText(QPointF(box.x(), y + ascent_), text);
            }
            col += span;
        }
    }
    if (cursorVisible_ && scrolled_ == 0) {
        const QRectF box(cursor_.col * cellW_, cursor_.row * cellH_, cellW_, cellH_);
        if (cursorShape_ == QLatin1String("bar")) p->fillRect(QRectF(box.x(), box.y(), 2, box.height()), cursorColor_);
        else if (cursorShape_ == QLatin1String("underline")) p->fillRect(QRectF(box.x(), box.bottom() - 2, box.width(), 2), cursorColor_);
        else if (focused_) p->fillRect(box, cursorColor_);
        else { p->setPen(cursorColor_); p->drawRect(box.adjusted(0.5, 0.5, -0.5, -0.5)); }
    }
}

void VTermItem::keyPressEvent(QKeyEvent *e) {
    const auto mods = e->modifiers();
    if ((mods & Qt::ControlModifier) && (mods & Qt::ShiftModifier)) {
        if (e->key() == Qt::Key_C) { copy(); return; }
        if (e->key() == Qt::Key_V) { paste(); return; }
    }
    int mod = VTERM_MOD_NONE;
    if (mods & Qt::ShiftModifier) mod |= VTERM_MOD_SHIFT;
    if (mods & Qt::ControlModifier) mod |= VTERM_MOD_CTRL;
    if (mods & Qt::AltModifier) mod |= VTERM_MOD_ALT;
    const auto m = VTermModifier(mod);
    VTermKey key = VTERM_KEY_NONE;
    switch (e->key()) {
    case Qt::Key_Return: case Qt::Key_Enter: key = VTERM_KEY_ENTER; break;
    case Qt::Key_Tab: case Qt::Key_Backtab: key = VTERM_KEY_TAB; break;
    case Qt::Key_Backspace: key = VTERM_KEY_BACKSPACE; break;
    case Qt::Key_Escape: key = VTERM_KEY_ESCAPE; break;
    case Qt::Key_Up: key = VTERM_KEY_UP; break;
    case Qt::Key_Down: key = VTERM_KEY_DOWN; break;
    case Qt::Key_Left: key = VTERM_KEY_LEFT; break;
    case Qt::Key_Right: key = VTERM_KEY_RIGHT; break;
    case Qt::Key_Insert: key = VTERM_KEY_INS; break;
    case Qt::Key_Delete: key = VTERM_KEY_DEL; break;
    case Qt::Key_Home: key = VTERM_KEY_HOME; break;
    case Qt::Key_End: key = VTERM_KEY_END; break;
    case Qt::Key_PageUp: key = VTERM_KEY_PAGEUP; break;
    case Qt::Key_PageDown: key = VTERM_KEY_PAGEDOWN; break;
    default: break;
    }
    if (e->key() >= Qt::Key_F1 && e->key() <= Qt::Key_F12) key = VTermKey(VTERM_KEY_FUNCTION(e->key() - Qt::Key_F1 + 1));
    selStart_ = QPoint(-1, -1);
    scrolled_ = 0;
    if (key != VTERM_KEY_NONE) {
        vterm_keyboard_key(vt_, key, m);
    } else if ((mods & Qt::ControlModifier) && e->key() >= Qt::Key_A && e->key() <= Qt::Key_Z) {
        vterm_keyboard_unichar(vt_, uint32_t(e->key() - Qt::Key_A + 'a'), m);
    } else {
        for (const uint cp : e->text().toUcs4())
            if (cp >= 0x20 && cp != 0x7F) vterm_keyboard_unichar(vt_, cp, (mods & Qt::AltModifier) ? VTERM_MOD_ALT : VTERM_MOD_NONE);
    }
    update();
}

void VTermItem::inputMethodEvent(QInputMethodEvent *e) {
    if (!e->commitString().isEmpty()) send(e->commitString().toUtf8());
    e->accept();
}

QVariant VTermItem::inputMethodQuery(Qt::InputMethodQuery query) const {
    if (query == Qt::ImCursorRectangle) return QRectF(cursor_.col * cellW_, cursor_.row * cellH_, cellW_, cellH_);
    if (query == Qt::ImEnabled) return true;
    if (query == Qt::ImFont) return font_;
    return QQuickPaintedItem::inputMethodQuery(query);
}

QPoint VTermItem::cellAt(const QPointF &p) const { return QPoint(qBound(0, int(p.x() / cellW_), cols_ - 1), qBound(0, int(p.y() / cellH_), rows_ - 1)); }

void VTermItem::mousePressEvent(QMouseEvent *e) {
    forceActiveFocus();
    selStart_ = selEnd_ = cellAt(e->position());
    selecting_ = true;
    if (e->modifiers() & Qt::ControlModifier) {   // a link in the text opens outside
        QString line;
        for (int c = 0; c < cols_; ++c) {
            const VTermScreenCell cell = cellAt(selStart_.y(), c);
            line += cell.chars[0] ? QString::fromUcs4(reinterpret_cast<const char32_t *>(&cell.chars[0]), 1) : QStringLiteral(" ");
        }
        static const QRegularExpression url(QStringLiteral(R"(https?://[^\s"'<>]+)"));
        for (auto it = url.globalMatch(line); it.hasNext();) {
            const auto m = it.next();
            if (selStart_.x() >= m.capturedStart() && selStart_.x() < m.capturedEnd()) QDesktopServices::openUrl(QUrl(m.captured()));
        }
    }
    update();
}

void VTermItem::mouseMoveEvent(QMouseEvent *e) {
    if (!selecting_) return;
    selEnd_ = cellAt(e->position());
    update();
}

void VTermItem::mouseReleaseEvent(QMouseEvent *) {
    selecting_ = false;
    if (selStart_ == selEnd_) selStart_ = QPoint(-1, -1);
    update();
}

void VTermItem::wheelEvent(QWheelEvent *e) {
    const int lines = e->angleDelta().y() / 40;
    scrolled_ = qBound(0, scrolled_ + lines, int(scrollback_.size()));
    update();
}

void VTermItem::focusInEvent(QFocusEvent *e) {
    QQuickPaintedItem::focusInEvent(e);
    focused_ = true;
    update();
}

void VTermItem::focusOutEvent(QFocusEvent *e) {
    QQuickPaintedItem::focusOutEvent(e);
    focused_ = false;
    update();
}

void VTermItem::copy() {
    if (selStart_.x() < 0) return;
    QPoint a = selStart_, b = selEnd_;
    if (std::make_pair(a.y(), a.x()) > std::make_pair(b.y(), b.x())) std::swap(a, b);
    QString text;
    for (int row = a.y(); row <= b.y(); ++row) {
        QString line;
        for (int col = row == a.y() ? a.x() : 0; col <= (row == b.y() ? b.x() : cols_ - 1); ++col) {
            const VTermScreenCell cell = cellAt(row, col);
            line += cell.chars[0] ? QString::fromUcs4(reinterpret_cast<const char32_t *>(&cell.chars[0]), 1) : QStringLiteral(" ");
        }
        while (line.endsWith(QLatin1Char(' '))) line.chop(1);
        text += line;
        if (row != b.y()) text += QLatin1Char('\n');
    }
    QGuiApplication::clipboard()->setText(text);
}

void VTermItem::paste() {
    QByteArray text = QGuiApplication::clipboard()->text().toUtf8();
    text.replace("\x1b", "");  // copied text must not be able to end the paste early and run what follows
    vterm_keyboard_start_paste(vt_);
    send(text);
    vterm_keyboard_end_paste(vt_);
}

QString VTermItem::screenText() const {
    QString out;
    for (int row = 0; row < rows_; ++row) {
        QString line;
        for (int col = 0; col < cols_; ++col) {
            const VTermScreenCell cell = cellAt(row, col);
            line += cell.chars[0] ? QString::fromUcs4(reinterpret_cast<const char32_t *>(&cell.chars[0]), 1) : QStringLiteral(" ");
        }
        while (line.endsWith(QLatin1Char(' '))) line.chop(1);
        out += line + QLatin1Char('\n');
    }
    return out;
}
