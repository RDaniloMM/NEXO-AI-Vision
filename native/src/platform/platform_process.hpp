// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

namespace cuajone::platform {

struct ProcessStopPolicy {
    int graceful_timeout_ms{};
    int force_timeout_ms{};
};

ProcessStopPolicy processStopPolicy() noexcept;

namespace detail {
ProcessStopPolicy defaultProcessStopPolicy() noexcept;
}

}  // namespace cuajone::platform
