#pragma once

#include "archstreamer/runtime_cadence/store.hpp"
#include "archstreamer/runtime_cadence/user_auth.hpp"
#include "archstreamer/runtime_cadence/instance_ops.hpp"

#if defined(ARCHSTREAMER_RUNTIME_CADENCE_DB)
#include "archstreamer/runtime_cadence/db_store.hpp"
#else
#include "archstreamer/runtime_cadence/file_store.hpp"
#endif

#include <memory>

namespace archstreamer::cadence {

#if defined(ARCHSTREAMER_RUNTIME_CADENCE_DB)
using ActiveStore = DbRuntimeStore;
#else
using ActiveStore = FileRuntimeStore;
#endif

/** Construct the build-selected store implementation. */
std::shared_ptr<RuntimeStore> make_runtime_store();

} // namespace archstreamer::cadence
