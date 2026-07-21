#include "client/client_app.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    try {
        auto config = archstreamer::ClientAppConfig{};
        if (argc > 1) {
            config.host = argv[1];
        }
        if (argc > 2) {
            config.control_port = static_cast<std::uint16_t>(std::stoul(argv[2]));
        }
        config.username = "catalog_probe";
        config.display_name = config.username;
        config.role = archstreamer::ClientParticipantRole::Viewer;
        config.filter.requested_players = 0;
        config.wants_video = false;
        config.wants_audio = false;

        const auto app = archstreamer::ClientApp{};
        const auto draft = app.begin_session(config);
        std::cout
            << "full=" << draft.pending_session.game_list.games.size()
            << " filtered=" << draft.filtered_catalog.games.size() << '\n';
        if (!draft.filtered_catalog.games.empty()) {
            const auto& game = draft.filtered_catalog.games.front();
            std::cout
                << "first=" << game.display_name
                << "\nid=" << game.id << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "client_catalog_probe: " << error.what() << '\n';
        return 1;
    }
}
