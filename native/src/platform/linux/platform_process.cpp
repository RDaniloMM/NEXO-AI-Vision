// SPDX-License-Identifier: AGPL-3.0-only

#include "../platform_process.hpp"

namespace cuajone::platform::detail {

ProcessStopPolicy defaultProcessStopPolicy() noexcept {
    return {5000, 2000};
}

}  // namespace cuajone::platform::detail
