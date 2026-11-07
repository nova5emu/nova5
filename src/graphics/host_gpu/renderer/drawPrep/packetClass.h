#pragma once
#include "graphics/guest_gpu/pm4.h"
#include <cstdint>

namespace Libs::Graphics::DrawPrep {
// Conservative subset of Jetsku 59e74d86. In particular NOP extensions,
// register-indirect loads, indirect buffers, predication and all memory effects
// are fences, even if some could eventually be proved safe.
inline bool WindowSafePacket(uint32_t header) {
	if ((header & 1u) != 0 || (header >> 30u) != 3u) return false;
	switch ((header >> 8u) & 255u) {
		case Pm4::IT_SET_CONTEXT_REG: case Pm4::IT_SET_SH_REG:
		case Pm4::IT_SET_UCONFIG_REG: case Pm4::IT_SET_UCONFIG_REG_INDEX:
		case Pm4::IT_INDEX_TYPE: case Pm4::IT_INDEX_BASE: case Pm4::IT_INDEX_BUFFER_SIZE:
		case Pm4::IT_NUM_INSTANCES:
		case Pm4::IT_DRAW_INDEX_2: case Pm4::IT_DRAW_INDEX_OFFSET_2: case Pm4::IT_DRAW_INDEX_AUTO: return true;
		default: return false;
	}
}
} // namespace Libs::Graphics::DrawPrep
