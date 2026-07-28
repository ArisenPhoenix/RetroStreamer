#pragma once

#include <QString>

class QComboBox;

namespace archstreamer::gui {

bool host_role_is_viewer(const QComboBox* combo);
QString host_role_text(const QComboBox* combo);
QString host_runner_program();
bool running_inside_flatpak();
QString resolve_native_host_runner(const QString& configured);

} // namespace archstreamer::gui
