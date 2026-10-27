#pragma once

#include "graphics/host_gpu/vulkanCommon.h"
#include "common/readbackProfile.h"
#include <array>
#include <cstdint>

namespace Libs::Graphics {
class CommandScheduler;
class CommandBuffer;
struct GraphicContext;
struct ImageInfo;
void RecordIndirectImageWorkImpl(Common::ReadbackProfile::DrawWork::Op op, const ImageInfo& info,
                                 uint32_t flags, uint64_t bytes);
inline void RecordIndirectImageWork(Common::ReadbackProfile::DrawWork::Op op, const ImageInfo& info,
                                    uint32_t flags = 0, uint64_t bytes = 0) {
	if ((Common::ReadbackProfile::DrawWork::record || Common::ReadbackProfile::DrawWork::watched_count ||
	     Common::ReadbackProfile::DrawWork::IsImageTransfer(op)) &&
	    Common::ReadbackProfile::Enabled()) RecordIndirectImageWorkImpl(op, info, flags, bytes);
}

// Opt-in GPU elapsed-time sampling with Vulkan timestamps, active only while SubmitProfile records.
// One block of queries per command buffer of the owning scheduler: reset when the command buffer
// begins (outside rendering), a TOP->BOTTOM pair spanning the whole command buffer, and pairs around
// chosen operations. Results are read only after the block's tick completed, without WAIT_BIT,
// flushes or waits; a block that is not ready yet is simply read later. Intervals are GPU elapsed
// times between pipeline points: they may overlap and include dependencies, so they are neither
// exclusive costs nor a serial "work per frame".
class GpuTimeProfile {
public:
	enum class Category : uint8_t {
		Detile, TextureUpload, DepthDetile, DepthConvert, DepthUpload,
		IndirectMeshConvert, IndirectMeshPrepare, IndirectMeshEmit,
		IndirectPrechecks, IndirectDiscovery, Count
	};

	explicit GpuTimeProfile(GraphicContext& graphics);
	~GpuTimeProfile();
	GpuTimeProfile(const GpuTimeProfile&)            = delete;
	GpuTimeProfile& operator=(const GpuTimeProfile&) = delete;

	void SetOwner(CommandScheduler* owner) { m_owner = owner; }
	// Right after the scheduler began a new command buffer.
	void OnBegin(CommandScheduler& scheduler, const CommandBuffer& command);
	// Right before the scheduler ends the command buffer that will signal `tick`.
	void OnSubmit(CommandScheduler& scheduler, const CommandBuffer& command, uint64_t tick);

	struct Token {
		uint32_t query      = UINT32_MAX;
		uint64_t generation = 0; // Command buffer recording that wrote the begin timestamp.
	};
	Token Begin(CommandScheduler& scheduler, const CommandBuffer& command);
	void  End(CommandScheduler& scheduler, const CommandBuffer& command, Token token, Category category);

	// Shadow observations only: never publish a Vulkan clock into guest memory.
	// CPU reference values and native GPU timestamps have different epochs.
	static bool GuestClockRecording();
	struct GuestClockSample {
		uint64_t address = 0, cpu_reference = 0, submit = 0;
		uint32_t frame = 0, bytes = 0, control = 0, event = 0;
		int queue = 0;
		bool release = false;
	};
	void ObserveGuestClock(CommandScheduler& scheduler, GuestClockSample sample);

private:
	friend struct GpuTimeProfileTestAccess;
	static constexpr uint32_t GuestClockSlots = 256;
	static constexpr uint32_t GuestClockLimit = 8192;
	void InitializeGuestClock(const CommandBuffer& command);
	vk::QueryPool m_clock_pool;
	uint64_t m_clock_mask = 0, m_clock_window = UINT64_MAX, m_clock_sequence = 0;
	uint64_t m_clock_completed = 0, m_clock_failed = 0;
	uint32_t m_clock_attempts = 0, m_clock_capacity_drops = 0, m_clock_pending = 0;
	bool m_clock_unavailable = false, m_clock_limit_reported = false;
	double m_clock_period_ns = 0;
	static constexpr uint32_t Blocks        = 512;
	static constexpr uint32_t SlotsPerBlock = 32;
	struct Block {
		uint64_t tick       = 0; // Nonzero while results are pending.
		uint64_t generation = 0; // Recording that owns the block.
		uint64_t session    = 0; // Profile session (recording period) the block belongs to.
		uint32_t used       = 0;
		uint32_t ended      = 0; // Bit per pair after the first: closed in the same recording.
		bool timeline = false;
		uint64_t frame = 0, submission = 0;
		std::array<uint8_t, SlotsPerBlock / 2> categories {};
	};
	void Poll(CommandScheduler& scheduler);
	void CalibrateTimeline();
	bool TimelineTime(uint64_t native, uint64_t& host) const;
	bool m_timeline_calibration_tried = false, m_timeline_calibrated = false;
	bool m_timeline_native_unavailable = false;
	uint64_t m_timeline_native = 0, m_timeline_host = 0, m_timeline_frequency = 0;
	uint32_t m_timeline_pending = 0;

	GraphicContext&           m_graphics;
	CommandScheduler*         m_owner = nullptr;
	vk::QueryPool             m_pool;
	std::array<Block, Blocks> m_blocks {};
	int32_t                   m_active     = -1;
	uint32_t                  m_next       = 0;
	uint32_t                  m_begins     = 0;
	uint64_t                  m_generation = 0;
	uint64_t                  m_session    = 0;
	bool                      m_recording  = false;
	uint64_t                  m_valid_mask = 0; // 0 when the queue has no timestamp support.
	double                    m_period_ns  = 1.0;
};

// Times one operation recorded into the scheduler's current command buffer. If the command buffer
// changes before the scope ends (an intermediate flush), the pair is dropped and counted, never
// misattributed.
class GpuTimeScope {
public:
	GpuTimeScope(CommandScheduler& scheduler, GpuTimeProfile::Category category);
	~GpuTimeScope();
	GpuTimeScope(const GpuTimeScope&)            = delete;
	GpuTimeScope& operator=(const GpuTimeScope&) = delete;

private:
	CommandScheduler&        m_scheduler;
	GpuTimeProfile::Category m_category;
	GpuTimeProfile::Token    m_token;
};
} // namespace Libs::Graphics
