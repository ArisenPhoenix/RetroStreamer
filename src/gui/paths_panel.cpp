#include "paths_panel.hpp"

#include <QDir>
#include <QLineEdit>

namespace archstreamer::gui {

QString path_field_text(const QLineEdit* field) {
    return field == nullptr ? QString{} : field->text().trimmed();
}

QString expand_user_path(QString path) {
    path = path.trimmed();
    if (path == QLatin1String("~")) {
        return QDir::homePath();
    }
    if (path.startsWith(QLatin1String("~/"))) {
        return QDir::homePath() + path.mid(1);
    }
    return path;
}

std::filesystem::path path_field_value(const QLineEdit* field) {
    const auto text = expand_user_path(path_field_text(field));
    return text.isEmpty() ? std::filesystem::path{} : std::filesystem::path{text.toStdString()};
}

} // namespace archstreamer::gui
