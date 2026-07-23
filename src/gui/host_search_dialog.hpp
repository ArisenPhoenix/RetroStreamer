#pragma once

#include "common/discovery.hpp"

#include <QDialog>
#include <optional>

class QHideEvent;
class QShowEvent;

namespace archstreamer::gui {

class HostPickerWidget;

class HostSearchDialog final : public QDialog {
    Q_OBJECT

public:
    explicit HostSearchDialog(QWidget* parent = nullptr);

    std::optional<DiscoveredHost> selectedHost() const;

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void acceptSelection();

    HostPickerWidget* picker_ = nullptr;
};

} // namespace archstreamer::gui
