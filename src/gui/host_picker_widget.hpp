#pragma once

#include "common/discovery.hpp"

#include <QString>
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
    void setSeedHosts(std::vector<std::string> hosts);
    bool hasSelection() const;
    std::optional<DiscoveredHost> selectedHost() const;

    /** Best LAN candidate (same-subnet preferred); never loopback. */
    std::optional<DiscoveredHost> preferredHost() const;

signals:
    void hostSelected(const QString& address, int control_port, int input_port);
    void hostActivated();

private:
    void refreshUi();
    void pollDiscovery();
    void applySelection(bool force = false);

    std::unique_ptr<HostDiscoveryBrowser> browser_;
    std::vector<std::string> seed_hosts_;
    QTimer* timer_ = nullptr;
    QListWidget* list_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* refresh_ = nullptr;
    std::optional<DiscoveredHost> last_emitted_;
    /** Fingerprint of last painted host rows; skip clear/rebuild when unchanged. */
    QString last_list_fingerprint_;
};

} // namespace archstreamer::gui
