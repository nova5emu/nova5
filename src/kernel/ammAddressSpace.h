#pragma once

#include <cstdint>
#include <string_view>

namespace Libs::LibKernel::Memory {
// Frank c91fe80d separates AMM from ordinary user mappings at 8 TiB. The
// unified GPU trackers are still 40-bit: keep this title's 64 GiB aperture
// below that limit and below the default 512 GiB frame-generation ceiling.
struct AmmAddressLayout {
    uint64_t begin, end;
    bool isolated;
};
constexpr AmmAddressLayout LegacyAmmLayout {0x1000000000ull, 0x2000000000ull, false};
constexpr AmmAddressLayout WolverineAmmLayout {0x6000000000ull, 0x7000000000ull, true};

constexpr AmmAddressLayout SelectAmmAddressLayout(std::string_view title, const char* setting) {
    return title == "PPSA03671" && (setting == nullptr || std::string_view(setting) == "1")
               ? WolverineAmmLayout : LegacyAmmLayout;
}

// Selected once at memory initialization, after the title metadata is loaded.
AmmAddressLayout GetAmmAddressLayout();
} // namespace Libs::LibKernel::Memory
