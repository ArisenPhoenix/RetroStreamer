#include "pad_on_screen_keyboard.hpp"

#include "client/sdl2_controller_backend.hpp"
#include "common/controller_state.hpp"

#include <QDateTime>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QShowEvent>
#include <QSizePolicy>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace archstreamer::gui {
namespace {

constexpr int kStickDeadzone = 12000;
constexpr int kInitialRepeatMs = 320;
constexpr int kRepeatMs = 110;

bool button_down(std::uint32_t buttons, ControllerButton bit) {
    return (buttons & bit) != 0;
}

bool button_pressed(std::uint32_t previous, std::uint32_t next, ControllerButton bit) {
    return !button_down(previous, bit) && button_down(next, bit);
}

int stick_axis_to_dir(std::int16_t value) {
    if (value <= -kStickDeadzone) {
        return -1;
    }
    if (value >= kStickDeadzone) {
        return 1;
    }
    return 0;
}

} // namespace

PadOnScreenKeyboard::PadOnScreenKeyboard(Options options, QWidget* parent)
    : QDialog(parent)
    , options_(std::move(options)) {
    setWindowTitle(options_.title);
    setModal(true);
    setWindowFlag(Qt::WindowStaysOnTopHint, true);
    resize(520, 460);

    if (options_.max_length < 1) {
        options_.max_length = 1;
    }
    text_ = options_.initial_text;
    if (text_.size() > options_.max_length) {
        text_.truncate(options_.max_length);
    }

    build_ui();
    rebuild_grid();
    refresh_text_field();
    set_cursor(0);

    pad_timer_ = new QTimer(this);
    pad_timer_->setInterval(16);
    connect(pad_timer_, &QTimer::timeout, this, &PadOnScreenKeyboard::poll_controllers);
}

PadOnScreenKeyboard::~PadOnScreenKeyboard() {
    release_pad_backend();
}

QString PadOnScreenKeyboard::text() const {
    return text_;
}

std::optional<QString> PadOnScreenKeyboard::prompt(QWidget* parent, Options options) {
    PadOnScreenKeyboard dialog(std::move(options), parent);
    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    return dialog.text();
}

void PadOnScreenKeyboard::build_ui() {
    auto* root = new QVBoxLayout(this);

    prompt_label_ = new QLabel(this);
    prompt_label_->setWordWrap(true);
    prompt_label_->setText(
        options_.prompt.isEmpty() ? QStringLiteral("Enter text") : options_.prompt);
    root->addWidget(prompt_label_);

    text_field_ = new QLineEdit(this);
    text_field_->setReadOnly(true);
    text_field_->setAlignment(Qt::AlignCenter);
    text_field_->setMinimumHeight(40);
    QFont text_font = text_field_->font();
    text_font.setPointSize(text_font.pointSize() + 4);
    text_field_->setFont(text_font);
    root->addWidget(text_field_);

    grid_host_ = new QWidget(this);
    root->addWidget(grid_host_, 1);

    hint_label_ = new QLabel(
        QStringLiteral("D-pad / stick: move · A: select · B: backspace · Start: done · Back: cancel"),
        this);
    hint_label_->setWordWrap(true);
    root->addWidget(hint_label_);

    auto* footer = new QHBoxLayout();
    auto* cancel = new QPushButton(QStringLiteral("Cancel"), this);
    auto* done = new QPushButton(QStringLiteral("Done"), this);
    done->setDefault(true);
    footer->addStretch(1);
    footer->addWidget(cancel);
    footer->addWidget(done);
    root->addLayout(footer);

    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(done, &QPushButton::clicked, this, &PadOnScreenKeyboard::accept_text);
}

void PadOnScreenKeyboard::rebuild_grid() {
    if (auto* old = grid_host_->layout()) {
        QLayoutItem* item = nullptr;
        while ((item = old->takeAt(0)) != nullptr) {
            if (QWidget* widget = item->widget()) {
                widget->deleteLater();
            }
            delete item;
        }
        delete old;
    }
    cells_.clear();

    auto* grid = new QGridLayout(grid_host_);
    grid->setSpacing(6);
    columns_ = 7;

    const auto add_char = [&](QChar character) {
        Cell cell;
        cell.kind = CellKind::Character;
        cell.character = character;
        cell.label = QString(character);
        cells_.push_back(cell);
    };

    for (char letter = 'A'; letter <= 'Z'; ++letter) {
        add_char(QChar(letter));
    }
    for (char digit = '0'; digit <= '9'; ++digit) {
        add_char(QChar(digit));
    }

    {
        Cell space;
        space.kind = CellKind::Space;
        space.label = QStringLiteral("Space");
        cells_.push_back(space);
    }
    {
        Cell backspace;
        backspace.kind = CellKind::Backspace;
        backspace.label = QStringLiteral("Del");
        cells_.push_back(backspace);
    }
    {
        Cell done;
        done.kind = CellKind::Done;
        done.label = QStringLiteral("OK");
        cells_.push_back(done);
    }

    for (int index = 0; index < static_cast<int>(cells_.size()); ++index) {
        auto& cell = cells_[static_cast<std::size_t>(index)];
        auto* button = new QToolButton(grid_host_);
        button->setText(cell.label);
        button->setCheckable(true);
        button->setAutoRaise(false);
        button->setMinimumSize(56, 44);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        cell.button = button;
        const int row = index / columns_;
        const int col = index % columns_;
        grid->addWidget(button, row, col);
        connect(button, &QToolButton::clicked, this, [this, index]() {
            set_cursor(index);
            activate_cell();
        });
    }
}

void PadOnScreenKeyboard::refresh_text_field() {
    text_field_->setText(text_);
    text_field_->setCursorPosition(text_.size());
}

void PadOnScreenKeyboard::set_cursor(int index) {
    if (cells_.empty()) {
        cursor_ = 0;
        return;
    }
    cursor_ = std::clamp(index, 0, static_cast<int>(cells_.size()) - 1);
    for (int i = 0; i < static_cast<int>(cells_.size()); ++i) {
        if (cells_[static_cast<std::size_t>(i)].button != nullptr) {
            cells_[static_cast<std::size_t>(i)].button->setChecked(i == cursor_);
        }
    }
    if (cells_[static_cast<std::size_t>(cursor_)].button != nullptr) {
        cells_[static_cast<std::size_t>(cursor_)].button->setFocus(Qt::OtherFocusReason);
    }
}

void PadOnScreenKeyboard::move_cursor(int dx, int dy) {
    if (cells_.empty() || (dx == 0 && dy == 0)) {
        return;
    }
    const int count = static_cast<int>(cells_.size());
    const int rows = (count + columns_ - 1) / columns_;
    int row = cursor_ / columns_;
    int col = cursor_ % columns_;
    row = (row + dy % rows + rows) % rows;
    col = (col + dx % columns_ + columns_) % columns_;
    int next = row * columns_ + col;
    if (next >= count) {
        next = count - 1;
    }
    set_cursor(next);
}

void PadOnScreenKeyboard::activate_cell() {
    if (cells_.empty()) {
        return;
    }
    const auto& cell = cells_[static_cast<std::size_t>(cursor_)];
    switch (cell.kind) {
    case CellKind::Character:
        insert_character(cell.character);
        break;
    case CellKind::Space:
        insert_character(QChar(' '));
        break;
    case CellKind::Backspace:
        backspace();
        break;
    case CellKind::Done:
        accept_text();
        break;
    }
}

void PadOnScreenKeyboard::insert_character(QChar character) {
    if (text_.size() >= options_.max_length) {
        return;
    }
    text_.append(character);
    refresh_text_field();
}

void PadOnScreenKeyboard::backspace() {
    if (text_.isEmpty()) {
        return;
    }
    text_.chop(1);
    refresh_text_field();
}

void PadOnScreenKeyboard::accept_text() {
    accept();
}

void PadOnScreenKeyboard::ensure_pad_backend() {
    if (options_.poll_pad || pad_backend_) {
        return;
    }
    try {
        pad_backend_ = std::make_unique<Sdl2ControllerBackend>();
        std::vector<std::string> ids;
        for (const auto& id : options_.preferred_controller_ids) {
            if (!id.trimmed().isEmpty()) {
                ids.push_back(id.toStdString());
            }
        }
        if (ids.empty()) {
            const auto devices = pad_backend_->list_devices();
            if (!devices.empty()) {
                ids.push_back(devices.front().id);
            }
        }
        if (!ids.empty()) {
            pad_backend_->open_selected(ids);
        }
    } catch (...) {
        pad_backend_.reset();
    }
}

void PadOnScreenKeyboard::release_pad_backend() {
    pad_backend_.reset();
}

void PadOnScreenKeyboard::poll_controllers() {
    std::optional<ControllerState> state;
    if (options_.poll_pad) {
        state = options_.poll_pad();
    } else if (pad_backend_) {
        state = pad_backend_->poll(0);
    }
    if (!state.has_value()) {
        return;
    }

    const auto buttons = state->buttons;
    apply_pad_edges(last_buttons_, buttons);

    int dx = 0;
    int dy = 0;
    if (button_down(buttons, ButtonDpadLeft)) {
        dx = -1;
    } else if (button_down(buttons, ButtonDpadRight)) {
        dx = 1;
    }
    if (button_down(buttons, ButtonDpadUp)) {
        dy = -1;
    } else if (button_down(buttons, ButtonDpadDown)) {
        dy = 1;
    }
    if (dx == 0) {
        dx = stick_axis_to_dir(state->left_x);
    }
    if (dy == 0) {
        dy = stick_axis_to_dir(state->left_y);
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (dx != 0 || dy != 0) {
        const bool direction_changed = dx != held_nav_dx_ || dy != held_nav_dy_;
        if (direction_changed || now >= next_repeat_ms_) {
            move_cursor(dx, dy);
            next_repeat_ms_ = now + (direction_changed ? kInitialRepeatMs : kRepeatMs);
            held_nav_dx_ = dx;
            held_nav_dy_ = dy;
        }
    } else {
        held_nav_dx_ = 0;
        held_nav_dy_ = 0;
        next_repeat_ms_ = 0;
    }

    last_buttons_ = buttons;
}

void PadOnScreenKeyboard::apply_pad_edges(std::uint32_t previous, std::uint32_t next) {
    if (button_pressed(previous, next, ButtonA) || button_pressed(previous, next, ButtonX)) {
        activate_cell();
    }
    if (button_pressed(previous, next, ButtonB)) {
        backspace();
    }
    if (button_pressed(previous, next, ButtonY)) {
        insert_character(QChar(' '));
    }
    if (button_pressed(previous, next, ButtonStart)) {
        accept_text();
    }
    if (button_pressed(previous, next, ButtonBack)) {
        reject();
    }
}

void PadOnScreenKeyboard::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    last_buttons_ = 0;
    held_nav_dx_ = 0;
    held_nav_dy_ = 0;
    next_repeat_ms_ = 0;
    ensure_pad_backend();
    if (pad_timer_ != nullptr) {
        pad_timer_->start();
    }
}

void PadOnScreenKeyboard::hideEvent(QHideEvent* event) {
    if (pad_timer_ != nullptr) {
        pad_timer_->stop();
    }
    release_pad_backend();
    QDialog::hideEvent(event);
}

void PadOnScreenKeyboard::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
    case Qt::Key_Left:
        move_cursor(-1, 0);
        break;
    case Qt::Key_Right:
        move_cursor(1, 0);
        break;
    case Qt::Key_Up:
        move_cursor(0, -1);
        break;
    case Qt::Key_Down:
        move_cursor(0, 1);
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        activate_cell();
        break;
    case Qt::Key_Backspace:
        backspace();
        break;
    case Qt::Key_Escape:
        reject();
        break;
    default: {
        const QString text = event->text();
        if (text.size() == 1) {
            const QChar character = text.at(0).toUpper();
            if (character.isLetterOrNumber() || character == QChar(' ')) {
                insert_character(character == QChar(' ') ? QChar(' ') : character);
                break;
            }
        }
        QDialog::keyPressEvent(event);
        break;
    }
    }
}

} // namespace archstreamer::gui
