#include "main_window.hpp"

#ifdef ARCHSTREAMER_HAS_HOST
#include "common/dlc_paths.hpp"
#include "host/catalog_ops.hpp"
#include "host/game_meta_edit_log.hpp"
#include "host/game_meta_store.hpp"
#endif

#include <QAbstractItemView>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <optional>
#include <cstdint>

namespace archstreamer::gui {

#ifdef ARCHSTREAMER_HAS_HOST

namespace {

QString qstr(const std::string& value) {
    return QString::fromStdString(value);
}

void setup_flat_table(QTableWidget* table) {
    table->setAlternatingRowColors(true);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSortingEnabled(true);
    table->setWordWrap(false);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setSectionsMovable(true);
    table->horizontalHeader()->setStretchLastSection(true);
    table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
}

bool row_matches_filter(const QStringList& cells, const QString& filter) {
    if (filter.isEmpty()) {
        return true;
    }
    return cells.join(QLatin1Char('\n')).toLower().contains(filter);
}

QTableWidgetItem* cell(const QString& text) {
    auto* item = new QTableWidgetItem(text);
    item->setToolTip(text);
    return item;
}

CatalogFsOptions catalog_fs_from_window(const MainWindow* window, bool apply_filesystem) {
    CatalogFsOptions fs;
    fs.apply_filesystem = apply_filesystem;
    if (window != nullptr) {
        fs.save_root = window->save_root_path_for_catalog();
        fs.art_root = window->art_root_path_for_catalog();
        fs.rom_root = window->rom_root_path_for_catalog();
        fs.dlc_root = window->dlc_root_path_for_catalog();
    }
    return fs;
}

void show_op_result(QWidget* parent, const CatalogOpResult& result) {
    QString detail = qstr(result.message);
    if (result.edit_id > 0) {
        detail += QStringLiteral("\nedit_id: %1").arg(result.edit_id);
    }
    if (!result.effects.empty()) {
        detail += QStringLiteral("\n\n");
        for (const auto& line : result.effects) {
            detail += QStringLiteral("• ") + qstr(line) + QStringLiteral("\n");
        }
    }
    if (!result.old_game_id.empty() || !result.new_game_id.empty()) {
        detail += QStringLiteral("\n");
        if (!result.old_game_id.empty()) {
            detail += QStringLiteral("old game_id: ") + qstr(result.old_game_id) + QStringLiteral("\n");
        }
        if (!result.new_game_id.empty() && result.new_game_id != result.old_game_id) {
            detail += QStringLiteral("new game_id: ") + qstr(result.new_game_id) + QStringLiteral("\n");
        }
    }
    if (result.ok) {
        QMessageBox::information(parent, "Catalog op", detail);
    } else {
        QMessageBox::warning(parent, "Catalog op failed", detail);
    }
}

std::optional<std::int64_t> selected_edit_id(QTableWidget* table) {
    if (table == nullptr) {
        return std::nullopt;
    }
    const auto rows = table->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        return std::nullopt;
    }
    const auto* item = table->item(rows.front().row(), 0);
    if (item == nullptr) {
        return std::nullopt;
    }
    bool ok = false;
    const auto id = item->text().toLongLong(&ok);
    if (!ok) {
        return std::nullopt;
    }
    return id;
}

std::optional<GameMetaRecord> selected_meta_row(QTableWidget* table) {
    if (table == nullptr) {
        return std::nullopt;
    }
    const auto rows = table->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        return std::nullopt;
    }
    const int row = rows.front().row();
    auto text = [&](int col) {
        const auto* item = table->item(row, col);
        return item != nullptr ? item->text().toStdString() : std::string{};
    };
    GameMetaRecord rec;
    rec.game_id = text(0);
    rec.system_key = text(1);
    rec.system_name = text(2);
    rec.display_name = text(3);
    rec.canonical_name = text(4);
    rec.core_name = text(5);
    rec.asset_key = text(6);
    rec.identity_key = text(7);
    rec.version = text(8);
    rec.language = text(9);
    rec.region = text(10);
    rec.content_stem = text(11);
    rec.updated_at = QString::fromStdString(text(12)).toLongLong();
    rec.source = text(13);
    if (rec.game_id.empty()) {
        return std::nullopt;
    }
    return rec;
}

bool edit_game_meta_dialog(QWidget* parent, GameMetaRecord& row, bool* apply_fs) {
    QDialog dialog(parent);
    dialog.setWindowTitle("Edit game_meta");
    dialog.resize(560, 480);
    auto* form = new QFormLayout(&dialog);

    auto* system_key = new QLineEdit(qstr(row.system_key), &dialog);
    auto* system_name = new QLineEdit(qstr(row.system_name), &dialog);
    auto* display_name = new QLineEdit(qstr(row.display_name), &dialog);
    auto* canonical_name = new QLineEdit(qstr(row.canonical_name), &dialog);
    auto* core_name = new QLineEdit(qstr(row.core_name), &dialog);
    auto* version = new QLineEdit(qstr(row.version), &dialog);
    auto* language = new QLineEdit(qstr(row.language), &dialog);
    auto* region = new QLineEdit(qstr(row.region), &dialog);
    auto* content_stem = new QLineEdit(qstr(row.content_stem), &dialog);
    auto* source = new QLineEdit(qstr(row.source), &dialog);
    auto* preview = new QLabel(&dialog);
    preview->setWordWrap(true);
    preview->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto refresh_preview = [&] {
        GameMetaRecord draft = row;
        draft.system_key = system_key->text().toStdString();
        draft.system_name = system_name->text().toStdString();
        draft.display_name = display_name->text().toStdString();
        draft.canonical_name = canonical_name->text().toStdString();
        draft.core_name = core_name->text().toStdString();
        draft.version = version->text().toStdString();
        draft.language = language->text().toStdString();
        draft.region = region->text().toStdString();
        draft.content_stem = content_stem->text().toStdString();
        draft.source = source->text().toStdString();
        draft = recompute_game_meta_identity(std::move(draft));
        preview->setText(
            QStringLiteral("Derived preview:\ngame_id=%1\nasset_key=%2\nidentity_key=\n%3")
                .arg(qstr(draft.game_id), qstr(draft.asset_key), qstr(draft.identity_key)));
    };

    for (auto* edit : {system_key,
                       system_name,
                       display_name,
                       canonical_name,
                       core_name,
                       version,
                       language,
                       region,
                       content_stem,
                       source}) {
        QObject::connect(edit, &QLineEdit::textChanged, &dialog, refresh_preview);
    }

    form->addRow("system_key", system_key);
    form->addRow("system_name", system_name);
    form->addRow("display_name", display_name);
    form->addRow("canonical_name", canonical_name);
    form->addRow("core_name", core_name);
    form->addRow("version", version);
    form->addRow("language", language);
    form->addRow("region", region);
    form->addRow("content_stem", content_stem);
    form->addRow("source", source);
    form->addRow("Preview", preview);

    auto* fs_check = new QCheckBox(
        "Also rename on-disk saves (content_stem), Art/<asset_key>/, and "
        "DLC/<System>/<content_stem>/ when those change",
        &dialog);
    fs_check->setChecked(true);
    form->addRow("", fs_check);

    auto* note = new QLabel(
        "game_id / identity_key / asset_key are always recomputed from "
        "system_key + canonical_name + version + language + region. "
        "If game_id changes, the old row is removed and the old id is kept as a catalog_id alias.",
        &dialog);
    note->setWordWrap(true);
    form->addRow(note);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    refresh_preview();

    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }
    row.system_key = system_key->text().toStdString();
    row.system_name = system_name->text().toStdString();
    row.display_name = display_name->text().toStdString();
    row.canonical_name = canonical_name->text().toStdString();
    row.core_name = core_name->text().toStdString();
    row.version = version->text().toStdString();
    row.language = language->text().toStdString();
    row.region = region->text().toStdString();
    row.content_stem = content_stem->text().toStdString();
    row.source = source->text().toStdString();
    if (apply_fs != nullptr) {
        *apply_fs = fs_check->isChecked();
    }
    return true;
}

} // namespace

// Public path accessors for catalog ops (implemented via existing private helpers).
std::filesystem::path MainWindow::save_root_path_for_catalog() const {
    return save_root_path();
}

std::filesystem::path MainWindow::art_root_path_for_catalog() const {
    return art_root_path();
}

std::filesystem::path MainWindow::rom_root_path_for_catalog() const {
    if (host_rom_root_ != nullptr) {
        const auto text = host_rom_root_->text().trimmed();
        if (!text.isEmpty()) {
            return std::filesystem::path{text.toStdString()};
        }
    }
    return {};
}

std::filesystem::path MainWindow::dlc_root_path_for_catalog() const {
    const auto rom = rom_root_path_for_catalog();
    if (!rom.empty()) {
        return default_dlc_root_for(rom);
    }
    return resolve_dlc_root();
}

QWidget* MainWindow::build_catalog_tab() {
    auto* page = new QWidget(this);
    auto* root = new QVBoxLayout(page);

    auto* intro = new QLabel(
        "Flat 1:1 view of the local game_meta SQLite tables (identity, aliases, "
        "user_games, play_modes from Meta JSON, and edits history). Use the ops "
        "below to edit identity fields; game_id/asset_key are recomputed. "
        "Filesystem renames are optional.",
        page);
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto* path_row = new QHBoxLayout();
    catalog_db_path_ = new QLabel(page);
    catalog_db_path_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    catalog_status_ = new QLabel(page);
    catalog_refresh_ = new QPushButton("Refresh", page);
    path_row->addWidget(new QLabel("DB:", page));
    path_row->addWidget(catalog_db_path_, 1);
    path_row->addWidget(catalog_status_);
    path_row->addWidget(catalog_refresh_);
    root->addLayout(path_row);

    auto* filter_row = new QHBoxLayout();
    catalog_filter_ = new QLineEdit(page);
    catalog_filter_->setPlaceholderText("Filter any column…");
    filter_row->addWidget(new QLabel("Filter", page));
    filter_row->addWidget(catalog_filter_, 1);
    root->addLayout(filter_row);

    auto* ops = new QHBoxLayout();
    auto* edit_btn = new QPushButton("Edit game…", page);
    auto* normalize_btn = new QPushButton("Normalize names…", page);
    auto* add_alias_btn = new QPushButton("Add alias…", page);
    auto* del_alias_btn = new QPushButton("Delete alias", page);
    auto* del_game_btn = new QPushButton("Delete game…", page);
    auto* view_edit_btn = new QPushButton("View edit…", page);
    auto* rollback_btn = new QPushButton("Rollback edit…", page);
    ops->addWidget(edit_btn);
    ops->addWidget(normalize_btn);
    ops->addWidget(add_alias_btn);
    ops->addWidget(del_alias_btn);
    ops->addWidget(del_game_btn);
    ops->addWidget(view_edit_btn);
    ops->addWidget(rollback_btn);
    ops->addStretch(1);
    root->addLayout(ops);

    auto* tables = new QTabWidget(page);

    catalog_meta_table_ = new QTableWidget(page);
    catalog_meta_table_->setColumnCount(14);
    catalog_meta_table_->setHorizontalHeaderLabels({
        "game_id",
        "system_key",
        "system_name",
        "display_name",
        "canonical_name",
        "core_name",
        "asset_key",
        "identity_key",
        "version",
        "language",
        "region",
        "content_stem",
        "updated_at",
        "source",
    });
    setup_flat_table(catalog_meta_table_);
    tables->addTab(catalog_meta_table_, "game_meta");

    catalog_aliases_table_ = new QTableWidget(page);
    catalog_aliases_table_->setColumnCount(4);
    catalog_aliases_table_->setHorizontalHeaderLabels({
        "alias_kind",
        "alias_value",
        "system_key",
        "game_id",
    });
    setup_flat_table(catalog_aliases_table_);
    tables->addTab(catalog_aliases_table_, "game_aliases");

    catalog_user_games_table_ = new QTableWidget(page);
    catalog_user_games_table_->setColumnCount(4);
    catalog_user_games_table_->setHorizontalHeaderLabels({
        "username",
        "game_id",
        "system_key",
        "last_played_at",
    });
    setup_flat_table(catalog_user_games_table_);
    tables->addTab(catalog_user_games_table_, "user_games");

    catalog_edits_table_ = new QTableWidget(page);
    catalog_edits_table_->setColumnCount(8);
    catalog_edits_table_->setHorizontalHeaderLabels({
        "edit_id",
        "edited_at",
        "op",
        "display_name",
        "old_game_id",
        "new_game_id",
        "before_summary",
        "note",
    });
    setup_flat_table(catalog_edits_table_);
    tables->addTab(catalog_edits_table_, "edits");

    catalog_play_modes_table_ = new QTableWidget(page);
    catalog_play_modes_table_->setColumnCount(7);
    catalog_play_modes_table_->setHorizontalHeaderLabels({
        "game_id",
        "single",
        "multi",
        "min_players",
        "max_players",
        "updated_at",
        "source",
    });
    setup_flat_table(catalog_play_modes_table_);
    tables->addTab(catalog_play_modes_table_, "play_modes");

    root->addWidget(tables, 1);

    connect(catalog_refresh_, &QPushButton::clicked, this, [this] { refresh_catalog_browser(); });
    connect(catalog_filter_, &QLineEdit::textChanged, this, [this] { refresh_catalog_browser(); });

    connect(edit_btn, &QPushButton::clicked, this, [this] {
        auto selected = selected_meta_row(catalog_meta_table_);
        if (!selected) {
            QMessageBox::information(this, "Edit game", "Select a row in game_meta first.");
            return;
        }
        const auto old_id = selected->game_id;
        bool apply_fs = true;
        if (!edit_game_meta_dialog(this, *selected, &apply_fs)) {
            return;
        }
        GameMetaStore store;
        const auto result = apply_game_meta_edit(
            store, old_id, *selected, catalog_fs_from_window(this, apply_fs));
        show_op_result(this, result);
        refresh_catalog_browser();
    });

    connect(normalize_btn, &QPushButton::clicked, this, [this] {
        auto selected = selected_meta_row(catalog_meta_table_);
        if (!selected) {
            QMessageBox::information(this, "Normalize", "Select a row in game_meta first.");
            return;
        }
        const auto answer = QMessageBox::question(
            this,
            "Normalize names",
            QStringLiteral(
                "Strip region/language parentheticals and trailing “Version” from "
                "display_name / content_stem into region (and version when Rev…), "
                "re-derive canonical_name, recompute game_id/asset_key, and rename "
                "matching save stems + Art folders when those change?\n\n%1")
                .arg(qstr(selected->display_name)),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            return;
        }
        GameMetaStore store;
        const auto result = normalize_game_meta_names(
            store, selected->game_id, catalog_fs_from_window(this, true), true);
        show_op_result(this, result);
        refresh_catalog_browser();
    });

    connect(add_alias_btn, &QPushButton::clicked, this, [this] {
        auto selected = selected_meta_row(catalog_meta_table_);
        QString game_id = selected ? qstr(selected->game_id) : QString();
        if (game_id.isEmpty()) {
            bool ok = false;
            game_id = QInputDialog::getText(
                          this, "Add alias", "game_id:", QLineEdit::Normal, {}, &ok)
                          .trimmed();
            if (!ok || game_id.isEmpty()) {
                return;
            }
        }
        bool ok = false;
        const auto kind = QInputDialog::getText(
                              this,
                              "Add alias",
                              "alias_kind (content_stem, display_name, canonical, …):",
                              QLineEdit::Normal,
                              QStringLiteral("content_stem"),
                              &ok)
                              .trimmed();
        if (!ok || kind.isEmpty()) {
            return;
        }
        const auto value = QInputDialog::getText(
                               this, "Add alias", "alias_value:", QLineEdit::Normal, {}, &ok)
                               .trimmed();
        if (!ok || value.isEmpty()) {
            return;
        }
        const auto scope = QInputDialog::getText(
                               this,
                               "Add alias",
                               "system_key scope (empty = global):",
                               QLineEdit::Normal,
                               selected ? qstr(selected->system_key) : QString(),
                               &ok)
                               .trimmed();
        if (!ok) {
            return;
        }
        GameMetaStore store;
        const auto result = add_game_meta_alias(
            store,
            game_id.toStdString(),
            kind.toStdString(),
            value.toStdString(),
            scope.toStdString());
        show_op_result(this, result);
        refresh_catalog_browser();
    });

    connect(del_alias_btn, &QPushButton::clicked, this, [this] {
        if (catalog_aliases_table_ == nullptr) {
            return;
        }
        const auto rows = catalog_aliases_table_->selectionModel()->selectedRows();
        if (rows.isEmpty()) {
            QMessageBox::information(this, "Delete alias", "Select a row in game_aliases first.");
            return;
        }
        const int row = rows.front().row();
        auto text = [&](int col) {
            const auto* item = catalog_aliases_table_->item(row, col);
            return item != nullptr ? item->text() : QString();
        };
        const auto kind = text(0);
        const auto value = text(1);
        const auto scope = text(2);
        if (QMessageBox::question(
                this,
                "Delete alias",
                QStringLiteral("Delete alias %1 = %2 (scope %3)?")
                    .arg(kind, value, scope.isEmpty() ? QStringLiteral("(global)") : scope))
            != QMessageBox::Yes) {
            return;
        }
        GameMetaStore store;
        const auto result = delete_game_meta_alias(
            store, kind.toStdString(), value.toStdString(), scope.toStdString());
        show_op_result(this, result);
        refresh_catalog_browser();
    });

    connect(del_game_btn, &QPushButton::clicked, this, [this] {
        auto selected = selected_meta_row(catalog_meta_table_);
        if (!selected) {
            QMessageBox::information(this, "Delete game", "Select a row in game_meta first.");
            return;
        }
        if (QMessageBox::warning(
                this,
                "Delete game",
                QStringLiteral(
                    "Delete game_meta + aliases (+ user_games) for:\n%1\n\n"
                    "On-disk saves and Art folders are NOT deleted.")
                    .arg(qstr(selected->display_name)),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No)
            != QMessageBox::Yes) {
            return;
        }
        GameMetaStore store;
        const auto result = delete_game_meta_entry(store, selected->game_id, true);
        show_op_result(this, result);
        refresh_catalog_browser();
    });

    connect(view_edit_btn, &QPushButton::clicked, this, [this] {
        const auto id = selected_edit_id(catalog_edits_table_);
        if (!id) {
            QMessageBox::information(this, "View edit", "Select a row in the edits tab first.");
            return;
        }
        GameMetaEditLog log;
        auto entry = log.find_edit(*id);
        if (!entry) {
            QMessageBox::warning(this, "View edit", "edit_id not found.");
            return;
        }
        const auto before = game_meta_record_to_json(entry->before);
        const auto after = game_meta_record_to_json(entry->after);
        QString effects;
        for (const auto& line : entry->effects) {
            effects += QStringLiteral("• ") + qstr(line) + QStringLiteral("\n");
        }
        QMessageBox::information(
            this,
            QStringLiteral("Edit #%1").arg(*id),
            QStringLiteral("op: %1\nedited_at: %2\nnote: %3\n\n"
                           "BEFORE:\n%4\n\nAFTER:\n%5\n\nEffects:\n%6")
                .arg(qstr(entry->op))
                .arg(entry->edited_at)
                .arg(qstr(entry->note))
                .arg(qstr(before))
                .arg(qstr(after))
                .arg(effects));
    });

    connect(rollback_btn, &QPushButton::clicked, this, [this] {
        const auto id = selected_edit_id(catalog_edits_table_);
        if (!id) {
            QMessageBox::information(this, "Rollback", "Select a row in the edits tab first.");
            return;
        }
        GameMetaEditLog log;
        auto entry = log.find_edit(*id);
        if (!entry) {
            QMessageBox::warning(this, "Rollback", "edit_id not found.");
            return;
        }
        if (QMessageBox::question(
                this,
                "Rollback edit",
                QStringLiteral(
                    "Restore the BEFORE snapshot from edit #%1?\n\n"
                    "%2 → %3\n\n"
                    "This recomputes game_id/asset_key and can rename Art/saves/DLC.")
                    .arg(*id)
                    .arg(qstr(entry->after.display_name))
                    .arg(qstr(entry->before.display_name)),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No)
            != QMessageBox::Yes) {
            return;
        }
        GameMetaStore store;
        const auto result =
            rollback_game_meta_edit(store, *id, catalog_fs_from_window(this, true));
        show_op_result(this, result);
        refresh_catalog_browser();
    });

    refresh_catalog_browser();
    return page;
}

void MainWindow::refresh_catalog_browser() {
    if (catalog_meta_table_ == nullptr || catalog_aliases_table_ == nullptr
        || catalog_user_games_table_ == nullptr) {
        return;
    }

    GameMetaStore meta;
    GameMetaEditLog edits;
    catalog_db_path_->setText(
        QStringLiteral("%1\nedits: %2")
            .arg(QString::fromStdString(meta.path().string()),
                 QString::fromStdString(edits.path().string())));
    if (!meta.ready()) {
        catalog_meta_table_->setRowCount(0);
        catalog_aliases_table_->setRowCount(0);
        catalog_user_games_table_->setRowCount(0);
        if (catalog_edits_table_ != nullptr) {
            catalog_edits_table_->setRowCount(0);
        }
        if (catalog_play_modes_table_ != nullptr) {
            catalog_play_modes_table_->setRowCount(0);
        }
        catalog_status_->setText("DB not open");
        return;
    }

    const auto filter = catalog_filter_ != nullptr
        ? catalog_filter_->text().trimmed().toLower()
        : QString();

    const auto games = meta.list_games();
    const auto aliases = meta.list_aliases();
    const auto user_games = meta.list_user_games();
    const auto play_modes = meta.list_play_modes();
    const auto edit_rows = edits.ready() ? edits.list_edits() : std::vector<GameMetaEditRecord>{};

    catalog_meta_table_->setSortingEnabled(false);
    catalog_meta_table_->setRowCount(0);
    int meta_shown = 0;
    for (const auto& game : games) {
        const QStringList cells = {
            qstr(game.game_id),
            qstr(game.system_key),
            qstr(game.system_name),
            qstr(game.display_name),
            qstr(game.canonical_name),
            qstr(game.core_name),
            qstr(game.asset_key),
            qstr(game.identity_key),
            qstr(game.version),
            qstr(game.language),
            qstr(game.region),
            qstr(game.content_stem),
            QString::number(game.updated_at),
            qstr(game.source),
        };
        if (!row_matches_filter(cells, filter)) {
            continue;
        }
        const int row = catalog_meta_table_->rowCount();
        catalog_meta_table_->insertRow(row);
        for (int col = 0; col < cells.size(); ++col) {
            catalog_meta_table_->setItem(row, col, cell(cells[col]));
        }
        ++meta_shown;
    }
    catalog_meta_table_->setSortingEnabled(true);
    catalog_meta_table_->resizeColumnsToContents();

    catalog_aliases_table_->setSortingEnabled(false);
    catalog_aliases_table_->setRowCount(0);
    int alias_shown = 0;
    for (const auto& alias : aliases) {
        const QStringList cells = {
            qstr(alias.alias_kind),
            qstr(alias.alias_value),
            qstr(alias.system_key),
            qstr(alias.game_id),
        };
        if (!row_matches_filter(cells, filter)) {
            continue;
        }
        const int row = catalog_aliases_table_->rowCount();
        catalog_aliases_table_->insertRow(row);
        for (int col = 0; col < cells.size(); ++col) {
            catalog_aliases_table_->setItem(row, col, cell(cells[col]));
        }
        ++alias_shown;
    }
    catalog_aliases_table_->setSortingEnabled(true);
    catalog_aliases_table_->resizeColumnsToContents();

    catalog_user_games_table_->setSortingEnabled(false);
    catalog_user_games_table_->setRowCount(0);
    int user_shown = 0;
    for (const auto& played : user_games) {
        const QStringList cells = {
            qstr(played.username),
            qstr(played.game_id),
            qstr(played.system_key),
            QString::number(played.last_played_at),
        };
        if (!row_matches_filter(cells, filter)) {
            continue;
        }
        const int row = catalog_user_games_table_->rowCount();
        catalog_user_games_table_->insertRow(row);
        for (int col = 0; col < cells.size(); ++col) {
            catalog_user_games_table_->setItem(row, col, cell(cells[col]));
        }
        ++user_shown;
    }
    catalog_user_games_table_->setSortingEnabled(true);
    catalog_user_games_table_->resizeColumnsToContents();

    int edits_shown = 0;
    if (catalog_edits_table_ != nullptr) {
        catalog_edits_table_->setSortingEnabled(false);
        catalog_edits_table_->setRowCount(0);
        for (const auto& edit : edit_rows) {
            const QString before_summary = QStringLiteral("%1 | %2 | region=%3")
                                               .arg(qstr(edit.before.system_key),
                                                    qstr(edit.before.canonical_name),
                                                    qstr(edit.before.region));
            const QStringList cells = {
                QString::number(edit.edit_id),
                QString::number(edit.edited_at),
                qstr(edit.op),
                qstr(edit.after.display_name.empty() ? edit.before.display_name
                                                     : edit.after.display_name),
                qstr(edit.old_game_id),
                qstr(edit.new_game_id),
                before_summary,
                qstr(edit.note),
            };
            if (!row_matches_filter(cells, filter)) {
                continue;
            }
            const int row = catalog_edits_table_->rowCount();
            catalog_edits_table_->insertRow(row);
            for (int col = 0; col < cells.size(); ++col) {
                catalog_edits_table_->setItem(row, col, cell(cells[col]));
            }
            ++edits_shown;
        }
        catalog_edits_table_->setSortingEnabled(true);
        catalog_edits_table_->resizeColumnsToContents();
    }

    int modes_shown = 0;
    if (catalog_play_modes_table_ != nullptr) {
        catalog_play_modes_table_->setSortingEnabled(false);
        catalog_play_modes_table_->setRowCount(0);
        for (const auto& modes : play_modes) {
            const QStringList cells = {
                qstr(modes.game_id),
                modes.supports_singleplayer ? QStringLiteral("true") : QStringLiteral("false"),
                modes.supports_multiplayer ? QStringLiteral("true") : QStringLiteral("false"),
                QString::number(modes.min_players),
                QString::number(modes.max_players),
                QString::number(modes.updated_at),
                qstr(modes.source),
            };
            if (!row_matches_filter(cells, filter)) {
                continue;
            }
            const int row = catalog_play_modes_table_->rowCount();
            catalog_play_modes_table_->insertRow(row);
            for (int col = 0; col < cells.size(); ++col) {
                catalog_play_modes_table_->setItem(row, col, cell(cells[col]));
            }
            ++modes_shown;
        }
        catalog_play_modes_table_->setSortingEnabled(true);
        catalog_play_modes_table_->resizeColumnsToContents();
    }

    catalog_status_->setText(
        QStringLiteral(
            "game_meta %1/%2 · aliases %3/%4 · user_games %5/%6 · play_modes %7/%8 · edits %9/%10")
            .arg(meta_shown)
            .arg(static_cast<int>(games.size()))
            .arg(alias_shown)
            .arg(static_cast<int>(aliases.size()))
            .arg(user_shown)
            .arg(static_cast<int>(user_games.size()))
            .arg(modes_shown)
            .arg(static_cast<int>(play_modes.size()))
            .arg(edits_shown)
            .arg(static_cast<int>(edit_rows.size())));
}

#endif // ARCHSTREAMER_HAS_HOST

} // namespace archstreamer::gui
