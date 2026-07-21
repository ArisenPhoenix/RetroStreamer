#pragma once

#include "common/discovery.hpp"

#include <QWidget>
#include <memory>
#include <optional>

class QLabel;
class QListWidget;
class QPushButton;
class QTimer;

namespace archstreamer::gui {

class HostPickerWidget final : public QWidget {
    Q_OBJECT

public:
    explicit HostPickerWidget(QWidget* parent = nullptr);

    void setBrowsing(bool enabled);
    bool hasSelection() const;
    std::optional<DiscoveredHost> selectedHost() const;

signals:
    void hostSelected(const QString& address, int control_port, int input_port);

private:
    void refreshUi();
    void pollDiscovery();
    void applySelection();

    std::unique_ptr<HostDiscoveryBrowser> browser_;
    QTimer* timer_ = nullptr;
    QListWidget* list_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* refresh_ = nullptr;
    std::optional<DiscoveredHost> last_emitted_;
};

} // namespace archstreamer::gui
