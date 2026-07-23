#include "host_search_dialog.hpp"
#include "host_picker_widget.hpp"

#include <QDialogButtonBox>
#include <QHideEvent>
#include <QLabel>
#include <QShowEvent>
#include <QVBoxLayout>

namespace archstreamer::gui {

HostSearchDialog::HostSearchDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle("Select Host");
    resize(480, 420);

    auto* root = new QVBoxLayout(this);
    root->addWidget(new QLabel(
        "Hosts on the same subnet are preferred. Loopback is only via This PC on the Client tab.",
        this));

    picker_ = new HostPickerWidget(this);
    root->addWidget(picker_, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    root->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &HostSearchDialog::acceptSelection);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(picker_, &HostPickerWidget::hostActivated, this, &HostSearchDialog::acceptSelection);
}

std::optional<DiscoveredHost> HostSearchDialog::selectedHost() const {
    return picker_->selectedHost();
}

void HostSearchDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    picker_->setBrowsing(true);
}

void HostSearchDialog::hideEvent(QHideEvent* event) {
    picker_->setBrowsing(false);
    QDialog::hideEvent(event);
}

void HostSearchDialog::acceptSelection() {
    if (!picker_->hasSelection()) {
        return;
    }
    accept();
}

} // namespace archstreamer::gui
