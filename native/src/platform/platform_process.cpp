// SPDX-License-Identifier: AGPL-3.0-only

#include "platform_process.hpp"

namespace cuajone::platform {

ProcessStopPolicy processStopPolicy() noexcept {
    return detail::defaultProcessStopPolicy();
}

}  // namespace cuajone::platform
