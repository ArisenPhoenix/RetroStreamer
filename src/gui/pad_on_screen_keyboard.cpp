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
constexpr std::uint16_t kTriggerPressThreshold = 16384; // ~25% of UINT16_MAX

bool button_down(std::uint32_t buttons, ControllerButton bit) {
    return (buttons & bit) != 0;
}

bool button_pressed(std::uint32_t previous, std::uint32_t next, ControllerButton bit) {
    return !button_down(previous, bit) && button_down(next, bit);
}

bool trigger_pressed(std::uint16_t previous, std::uint16_t next) {
    return previous < kTriggerPressThreshold && next >= kTriggerPressThreshold;
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
    // Non-modal so Settings can toggle it closed; prompt() still uses exec().
    setWindowModality(Qt::NonModal);
    setModal(false);
    setWindowFlag(Qt::WindowStaysOnTopHint, true);
    resize(780, 420);

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
        QStringLiteral(
            "D-pad / stick: move · A: select · □/X: DEL · R2 / Start: OK · Aa: case · Back: cancel"),
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
    columns_ = 14;

    const auto add_char = [&](QChar character) {
        Cell cell;
        cell.kind = CellKind::Character;
        cell.character = character;
        cell.label = QString(character);
        cells_.push_back(cell);
    };
    const auto add_special = [&](CellKind kind, const QString& label) {
        Cell cell;
        cell.kind = kind;
        cell.label = label;
        cells_.push_back(cell);
    };
    const auto add_skip = [&]() {
        Cell cell;
        cell.kind = CellKind::Skip;
        cells_.push_back(cell);
    };

    // Top: digits + Space / DEL / OK / Aa
    for (char digit = '0'; digit <= '9'; ++digit) {
        add_char(QChar(digit));
    }
    add_special(CellKind::Space, QStringLiteral("Space"));
    add_special(CellKind::Backspace, QStringLiteral("DEL"));
    add_special(CellKind::Done, QStringLiteral("OK"));
    add_special(CellKind::CaseToggle, QStringLiteral("Aa"));

    // Letters: 13 per row (A–M, N–Z), pad to 14 columns.
    for (char letter = 'A'; letter <= 'M'; ++letter) {
        add_char(QChar(letter));
    }
    add_skip();
    for (char letter = 'N'; letter <= 'Z'; ++letter) {
        add_char(QChar(letter));
    }
    add_skip();

    for (int index = 0; index < static_cast<int>(cells_.size()); ++index) {
        auto& cell = cells_[static_cast<std::size_t>(index)];
        if (cell.kind == CellKind::Skip) {
            auto* spacer = new QWidget(grid_host_);
            spacer->setMinimumSize(40, 44);
            spacer->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            grid->addWidget(spacer, index / columns_, index % columns_);
            continue;
        }

        auto* button = new QToolButton(grid_host_);
        button->setText(cell.label);
        button->setCheckable(true);
        button->setAutoRaise(false);
        button->setMinimumSize(40, 44);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        cell.button = button;
        grid->addWidget(button, index / columns_, index % columns_);
        connect(button, &QToolButton::clicked, this, [this, index]() {
            set_cursor(index);
            activate_cell();
        });
    }

    refresh_letter_labels();
}

void PadOnScreenKeyboard::refresh_text_field() {
    text_field_->setText(text_);
    text_field_->setCursorPosition(text_.size());
}

void PadOnScreenKeyboard::refresh_letter_labels() {
    for (auto& cell : cells_) {
        if (cell.kind != CellKind::Character || cell.button == nullptr) {
            continue;
        }
        if (cell.character.isLetter()) {
            const QChar shown = upper_case_ ? cell.character : cell.character.toLower();
            cell.label = QString(shown);
            cell.button->setText(cell.label);
        }
    }
    for (auto& cell : cells_) {
        if (cell.kind == CellKind::CaseToggle && cell.button != nullptr) {
            cell.button->setText(upper_case_ ? QStringLiteral("Aa") : QStringLiteral("aA"));
            cell.label = cell.button->text();
        }
    }
}

void PadOnScreenKeyboard::toggle_case() {
    upper_case_ = !upper_case_;
    refresh_letter_labels();
}

void PadOnScreenKeyboard::set_cursor(int index) {
    if (cells_.empty()) {
        cursor_ = 0;
        return;
    }
    index = std::clamp(index, 0, static_cast<int>(cells_.size()) - 1);
    if (cells_[static_cast<std::size_t>(index)].kind == CellKind::Skip) {
        // Prefer previous real cell.
        while (index > 0 && cells_[static_cast<std::size_t>(index)].kind == CellKind::Skip) {
            --index;
        }
        if (cells_[static_cast<std::size_t>(index)].kind == CellKind::Skip) {
            while (index + 1 < static_cast<int>(cells_.size()) &&
                   cells_[static_cast<std::size_t>(index)].kind == CellKind::Skip) {
                ++index;
            }
        }
    }
    cursor_ = index;
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

    for (int attempt = 0; attempt < count; ++attempt) {
        row = (row + dy % rows + rows) % rows;
        col = (col + dx % columns_ + columns_) % columns_;
        int next = row * columns_ + col;
        if (next >= count) {
            next = count - 1;
        }
        if (cells_[static_cast<std::size_t>(next)].kind != CellKind::Skip) {
            set_cursor(next);
            return;
        }
        // Landed on skip: keep stepping in the same direction.
        if (dx == 0 && dy == 0) {
            break;
        }
    }
}

void PadOnScreenKeyboard::activate_cell() {
    if (cells_.empty()) {
        return;
    }
    const auto& cell = cells_[static_cast<std::size_t>(cursor_)];
    switch (cell.kind) {
    case CellKind::Character: {
        QChar character = cell.character;
        if (character.isLetter() && !upper_case_) {
            character = character.toLower();
        }
        insert_character(character);
        break;
    }
    case CellKind::Space:
        insert_character(QChar(' '));
        break;
    case CellKind::Backspace:
        backspace();
        break;
    case CellKind::Done:
        accept_text();
        break;
    case CellKind::CaseToggle:
        toggle_case();
        break;
    case CellKind::Skip:
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
    const auto r2 = state->right_trigger;
    apply_pad_edges(last_buttons_, buttons, last_r2_, r2);

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
    last_r2_ = r2;
}

void PadOnScreenKeyboard::apply_pad_edges(
    std::uint32_t previous_buttons,
    std::uint32_t next_buttons,
    std::uint16_t previous_r2,
    std::uint16_t next_r2) {
    if (button_pressed(previous_buttons, next_buttons, ButtonA)) {
        activate_cell();
    }
    // Square (SDL X) → DEL
    if (button_pressed(previous_buttons, next_buttons, ButtonX)) {
        backspace();
    }
    if (button_pressed(previous_buttons, next_buttons, ButtonB)) {
        backspace();
    }
    if (button_pressed(previous_buttons, next_buttons, ButtonY)) {
        insert_character(QChar(' '));
    }
    if (button_pressed(previous_buttons, next_buttons, ButtonStart) ||
        trigger_pressed(previous_r2, next_r2)) {
        accept_text();
    }
    if (button_pressed(previous_buttons, next_buttons, ButtonBack)) {
        reject();
    }
}

void PadOnScreenKeyboard::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    last_buttons_ = 0;
    last_r2_ = 0;
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
        activate_cell();
        break;
    case Qt::Key_Space:
        // Space key inserts a space; grid Space cell still works via Enter when selected.
        insert_character(QChar(' '));
        break;
    case Qt::Key_Backspace:
        backspace();
        break;
    case Qt::Key_Escape:
        reject();
        break;
    case Qt::Key_CapsLock:
        toggle_case();
        break;
    default: {
        const QString text = event->text();
        if (text.size() == 1) {
            QChar character = text.at(0);
            if (character.isLetter()) {
                character = upper_case_ ? character.toUpper() : character.toLower();
                insert_character(character);
                break;
            }
            if (character.isDigit() || character == QChar(' ')) {
                insert_character(character);
                break;
            }
        }
        QDialog::keyPressEvent(event);
        break;
    }
    }
}

} // namespace archstreamer::gui
