#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>
#include <optional>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class QHideEvent;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QShowEvent;
class QTimer;
class QToolButton;

struct ControllerState;

namespace archstreamer {
class Sdl2ControllerBackend;
}

namespace archstreamer::gui {

/** Pad-navigable on-screen keyboard for host Software Keyboard / text prompts. */
class PadOnScreenKeyboard final : public QDialog {
    Q_OBJECT

public:
    struct Options {
        QString title = QStringLiteral("Software Keyboard");
        QString prompt;
        QString initial_text;
        int max_length = 12;
        /** Prefer these SDL device ids; empty opens the first available pad. */
        QStringList preferred_controller_ids;
        /**
         * Optional external pad poller (e.g. session-owned backend).
         * When set, the dialog does not open its own SDL controllers.
         */
        std::function<std::optional<ControllerState>()> poll_pad;
    };

    explicit PadOnScreenKeyboard(Options options, QWidget* parent = nullptr);
    ~PadOnScreenKeyboard() override;

    QString text() const;

    /** Modal helper: Accepted → text, Rejected → nullopt. */
    static std::optional<QString> prompt(QWidget* parent, Options options);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    enum class CellKind : std::uint8_t {
        Character,
        Space,
        Backspace,
        Done,
        CaseToggle,
        Skip,
    };

    struct Cell {
        CellKind kind = CellKind::Character;
        QString label;
        QChar character; // uppercase A–Z / digit when kind == Character
        QToolButton* button = nullptr;
    };

    void build_ui();
    void rebuild_grid();
    void refresh_text_field();
    void refresh_letter_labels();
    void toggle_case();
    void set_cursor(int index);
    void move_cursor(int dx, int dy);
    void activate_cell();
    void insert_character(QChar character);
    void backspace();
    void accept_text();
    void poll_controllers();
    void apply_pad_edges(
        std::uint32_t previous_buttons,
        std::uint32_t next_buttons,
        std::uint16_t previous_l2,
        std::uint16_t next_l2,
        std::uint16_t previous_r2,
        std::uint16_t next_r2);
    void ensure_pad_backend();
    void release_pad_backend();

    Options options_;
    QLabel* prompt_label_ = nullptr;
    QLineEdit* text_field_ = nullptr;
    QLabel* hint_label_ = nullptr;
    QWidget* grid_host_ = nullptr;
    QTimer* pad_timer_ = nullptr;

    QString text_;
    std::vector<Cell> cells_;
    int columns_ = 14;
    int cursor_ = 0;
    bool upper_case_ = true;

    std::unique_ptr<archstreamer::Sdl2ControllerBackend> pad_backend_;
    std::uint32_t last_buttons_ = 0;
    std::uint16_t last_l2_ = 0;
    std::uint16_t last_r2_ = 0;
    qint64 next_repeat_ms_ = 0;
    int held_nav_dx_ = 0;
    int held_nav_dy_ = 0;
};

} // namespace archstreamer::gui
