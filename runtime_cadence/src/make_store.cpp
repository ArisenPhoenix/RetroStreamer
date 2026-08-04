#include "archstreamer/runtime_cadence/cadence.hpp"

namespace archstreamer::cadence {

std::shared_ptr<RuntimeStore> make_runtime_store() {
    return std::make_shared<ActiveStore>();
}

} // namespace archstreamer::cadence
