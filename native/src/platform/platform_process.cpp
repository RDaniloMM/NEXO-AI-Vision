// SPDX-License-Identifier: AGPL-3.0-only

#include "platform_process.hpp"

namespace nexoai::platform {

ProcessStopPolicy processStopPolicy() noexcept {
    return detail::defaultProcessStopPolicy();
}

}  // namespace nexoai::platform
