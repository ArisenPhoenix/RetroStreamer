#include "host_picker_widget.hpp"

#include <QAbstractItemView>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace archstreamer::gui {

namespace {

bool is_loopback_address(const std::string& address) {
    return address == "127.0.0.1" || address.rfind("127.", 0) == 0;
}

std::vector<DiscoveredHost> ranked_hosts(std::vector<DiscoveredHost> hosts) {
    const auto preferred = prefer_discovered_host(hosts);
    std::stable_sort(hosts.begin(), hosts.end(), [&](const DiscoveredHost& left, const DiscoveredHost& right) {
        const bool left_loop = is_loopback_address(left.address);
        const bool right_loop = is_loopback_address(right.address);
        if (left_loop != right_loop) {
            return !left_loop;
        }
        if (preferred.has_value()) {
            const bool left_pref =
                left.address == preferred->address &&
                left.control_port == preferred->control_port &&
                left.username == preferred->username;
            const bool right_pref =
                right.address == preferred->address &&
                right.control_port == preferred->control_port &&
                right.username == preferred->username;
            if (left_pref != right_pref) {
                return left_pref;
            }
        }
        return left.username < right.username;
    });
    return hosts;
}

} // namespace

HostPickerWidget::HostPickerWidget(QWidget* parent)
    : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    status_ = new QLabel("Discovery idle", this);
    list_ = new QListWidget(this);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setMinimumHeight(100);

    refresh_ = new QPushButton("Refresh Hosts", this);
    auto* actions = new QHBoxLayout();
    actions->addWidget(refresh_);
    actions->addStretch();

    root->addWidget(new QLabel("Available Hosts", this));
    root->addWidget(status_);
    root->addWidget(list_);
    root->addLayout(actions);

    timer_ = new QTimer(this);
    timer_->setInterval(1000);

    connect(timer_, &QTimer::timeout, this, &HostPickerWidget::pollDiscovery);
    connect(refresh_, &QPushButton::clicked, this, &HostPickerWidget::pollDiscovery);
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem*) {
        applySelection(true);
    });
    connect(list_, &QListWidget::itemSelectionChanged, this, [this] {
        applySelection(false);
    });
    connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) {
        applySelection(true);
        emit hostActivated();
    });

    setBrowsing(false);
}

void HostPickerWidget::setBrowsing(bool enabled) {
    if (enabled) {
        if (!browser_) {
            try {
                browser_ = std::make_unique<HostDiscoveryBrowser>();
                status_->setText("Listening for hosts on LAN...");
            } catch (const std::exception& error) {
                status_->setText(QString("Discovery unavailable: %1").arg(error.what()));
                return;
            }
        }
        timer_->start();
        pollDiscovery();
    } else {
        timer_->stop();
        browser_.reset();
        status_->setText("Discovery idle");
    }
}

bool HostPickerWidget::hasSelection() const {
    return selectedHost().has_value();
}

std::optional<DiscoveredHost> HostPickerWidget::selectedHost() const {
    if (list_->currentItem() == nullptr) {
        return std::nullopt;
    }
    DiscoveredHost host;
    host.username = list_->currentItem()->data(Qt::UserRole).toString().toStdString();
    host.address = list_->currentItem()->data(Qt::UserRole + 1).toString().toStdString();
    host.control_port = static_cast<std::uint16_t>(list_->currentItem()->data(Qt::UserRole + 2).toInt());
    host.input_port = static_cast<std::uint16_t>(list_->currentItem()->data(Qt::UserRole + 3).toInt());
    if (host.address.empty()) {
        return std::nullopt;
    }
    return host;
}

std::optional<DiscoveredHost> HostPickerWidget::preferredHost() const {
    if (!browser_) {
        return std::nullopt;
    }
    return prefer_discovered_host(browser_->hosts());
}

void HostPickerWidget::pollDiscovery() {
    if (!browser_) {
        return;
    }
    try {
        browser_->poll();
        browser_->expire_older_than(std::chrono::seconds(8));
        refreshUi();
    } catch (const std::exception& error) {
        status_->setText(QString("Discovery error: %1").arg(error.what()));
    }
}

void HostPickerWidget::refreshUi() {
    const auto previous = selectedHost();
    const auto hosts = ranked_hosts(browser_ ? browser_->hosts() : std::vector<DiscoveredHost>{});

    {
        const QSignalBlocker blocker(list_);
        list_->clear();
        QListWidgetItem* preferred_item = nullptr;
        const auto preferred = prefer_discovered_host(hosts);
        for (const auto& host : hosts) {
            auto* item = new QListWidgetItem(
                QString("%1  (%2:%3)")
                    .arg(QString::fromStdString(host.username))
                    .arg(QString::fromStdString(host.address))
                    .arg(host.control_port),
                list_);
            item->setData(Qt::UserRole, QString::fromStdString(host.username));
            item->setData(Qt::UserRole + 1, QString::fromStdString(host.address));
            item->setData(Qt::UserRole + 2, host.control_port);
            item->setData(Qt::UserRole + 3, host.input_port);
            if (previous.has_value() &&
                previous->username == host.username &&
                previous->address == host.address &&
                previous->control_port == host.control_port) {
                list_->setCurrentItem(item);
            }
            if (preferred.has_value() &&
                preferred->username == host.username &&
                preferred->address == host.address &&
                preferred->control_port == host.control_port) {
                preferred_item = item;
            }
        }
        if (list_->currentItem() == nullptr && preferred_item != nullptr) {
            list_->setCurrentItem(preferred_item);
        }
    }

    if (hosts.empty()) {
        status_->setText("No hosts found yet. On the host PC: Start Host with Advertise enabled.");
        last_emitted_.reset();
    } else {
        status_->setText(QString("%1 host(s) found — select one, then OK.")
            .arg(hosts.size()));
        applySelection(false);
    }
}

void HostPickerWidget::applySelection(bool force) {
    const auto host = selectedHost();
    if (!host.has_value()) {
        return;
    }
    if (!force &&
        last_emitted_.has_value() &&
        last_emitted_->address == host->address &&
        last_emitted_->control_port == host->control_port &&
        last_emitted_->input_port == host->input_port) {
        return;
    }
    last_emitted_ = *host;
    emit hostSelected(
        QString::fromStdString(host->address),
        host->control_port,
        host->input_port);
}

} // namespace archstreamer::gui
