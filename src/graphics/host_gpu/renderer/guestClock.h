#pragma once

#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {
class CommandScheduler;
class CommandBuffer;
class RenderContext;

// Native EOP reference clocks by default (no-gpu-timestamps opts out).
// Stage one covers reference clocks only. Completion labels retain
// their existing semantics. All state, including publication, belongs to the
// GPU command lane (normal scheduler completions, not the priority thread).
class GuestClock {
public:
	explicit GuestClock(RenderContext& context): m_context(context) {}
	~GuestClock();
	GuestClock(const GuestClock&) = delete;
	GuestClock& operator=(const GuestClock&) = delete;
	void OnBegin(CommandScheduler& scheduler, const CommandBuffer& command);
	bool Record(uint64_t address);
	// Called before immediate PM4 writes even after native clocks are disabled.
	void Cancel(uint64_t address, uint64_t size);
	bool HasPending() const { return m_pending != 0; }

	// Convert a forward device-clock interval; subtract before scaling so a
	// large native epoch never loses precision in floating-point conversion.
	static bool Convert(uint64_t native, uint64_t native_anchor, uint64_t guest_anchor,
	                    uint64_t mask, double period_ns, uint64_t& result);

private:
	friend struct GuestClockTestAccess;
	static constexpr uint32_t BlockCount = 64, QueriesPerBlock = 1024;
	struct Sample {
		uint64_t address, serial, previous;
		uint32_t batch_size = 1;
	};
	struct Block {
		uint32_t used = 0, pending = 0, last_batch = 0;
		uint64_t operation_sequence = 0;
		std::vector<Sample> samples;
	};
	bool Initialize();
	void Complete(uint32_t block, uint32_t first);
	void Publish(const Sample& sample, uint64_t native, bool ready);
	RenderContext& m_context;
	vk::QueryPool m_pool;
	std::array<Block, BlockCount> m_blocks {};
	std::unordered_map<uint64_t, uint64_t> m_latest;
	uint64_t m_native_anchor = 0, m_guest_anchor = 0, m_mask = 0, m_serial = 0;
	double m_period_ns = 0;
	uint32_t m_next = 0, m_pending = 0;
	int32_t m_active = -1;
	bool m_unavailable = false;
};
} // namespace Libs::Graphics
