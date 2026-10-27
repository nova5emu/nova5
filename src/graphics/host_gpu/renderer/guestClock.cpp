#include "common/gpuThreadSampler.h"
#include "common/frameTimeline.h"
#include "graphics/host_gpu/renderer/guestClock.h"

#include "common/submitProfile.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/sync.h"
#include "kernel/memory.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace Libs::Graphics {
using Common::SubmitProfile::Extra;

GuestClock::~GuestClock() {
	if (m_pool) m_context.GetGraphics().device.destroyQueryPool(m_pool);
}

bool GuestClock::Convert(uint64_t native, uint64_t native_anchor, uint64_t guest_anchor,
                         uint64_t mask, double period_ns, uint64_t& result) {
	if (!mask || !std::isfinite(period_ns) || period_ns <= 0) return false;
	const auto delta = (native - native_anchor) & mask;
	// A backwards sample (or an ambiguous half-wrap) must not become a huge time.
	if (delta > (mask >> 1)) return false;
	const auto scaled = std::floor(static_cast<long double>(delta) * period_ns / 10.0L);
	if (scaled < 0 || scaled >= 18446744073709551616.0L) return false;
	const auto ticks = static_cast<uint64_t>(scaled);
	if (ticks > UINT64_MAX - guest_anchor) return false;
	result = guest_anchor + ticks;
	return true;
}

bool GuestClock::Initialize() {
	if (m_pool) return true;
	if (m_unavailable) return false;
	m_unavailable = true;
	auto& graphics = m_context.GetGraphics();
	if (!graphics.calibrated_timestamps_enabled) {
		std::fprintf(stderr, "[GPU_TIMESTAMPS] unavailable=calibrated_timestamps\n");
		return false;
	}
	uint32_t count = 0;
	graphics.physical_device.getQueueFamilyProperties(&count, nullptr);
	std::vector<vk::QueueFamilyProperties> families(count);
	graphics.physical_device.getQueueFamilyProperties(&count, families.data());
	const auto bits = graphics.queue_family < count ? families[graphics.queue_family].timestampValidBits : 0u;
	m_mask = bits == 0 ? 0 : bits >= 64 ? UINT64_MAX : (uint64_t{1} << bits) - 1;
	m_period_ns = graphics.physical_device_properties.limits.timestampPeriod;
	if (!m_mask || !std::isfinite(m_period_ns) || m_period_ns <= 0) return false;

	// A calibrated DEVICE sample lies inside this CPU-clock bracket. Choose the
	// narrowest bracket, once per device lifetime; never anchor to callback time.
	// Warm ReadReferenceClock first: initial TSC-frequency discovery may sleep.
	(void)Sync::ReadReferenceClock();
	uint64_t best_error_ns = UINT64_MAX;
	vk::CalibratedTimestampInfoEXT info {};
	info.timeDomain = vk::TimeDomainEXT::eDevice;
	for (unsigned i = 0; i < 8; ++i) {
		uint64_t native = 0, deviation = 0;
		const auto before = Sync::ReadReferenceClock();
		const auto status = graphics.device.getCalibratedTimestampsEXT(1, &info, &native, &deviation);
		const auto after = Sync::ReadReferenceClock();
		if (status != vk::Result::eSuccess || after < before || after - before > 1000000) continue;
		const auto bracket_ns = (after - before) * 10;
		if (deviation > UINT64_MAX - bracket_ns) continue;
		const auto error = bracket_ns + deviation;
		if (error < best_error_ns) {
			best_error_ns = error;
			m_native_anchor = native & m_mask;
			m_guest_anchor = before + (after - before) / 2;
		}
	}
	if (best_error_ns == UINT64_MAX) {
		std::fprintf(stderr, "[GPU_TIMESTAMPS] unavailable=calibration\n");
		return false;
	}
	vk::QueryPoolCreateInfo create {};
	create.queryType = vk::QueryType::eTimestamp;
	create.queryCount = BlockCount * QueriesPerBlock;
	if (graphics.device.createQueryPool(&create, nullptr, &m_pool) != vk::Result::eSuccess) return false;
	m_unavailable = false;
	std::fprintf(stderr, "[GPU_TIMESTAMPS] ready=1 ref_hz=100000000 period_ns=%.9g calibration_bound_ns=%llu\n",
	             m_period_ns, static_cast<unsigned long long>(best_error_ns));
	return true;
}

void GuestClock::OnBegin(CommandScheduler& scheduler, const CommandBuffer& command) {
	if (&scheduler != &m_context.GetCommandScheduler()) return;
	m_active = -1;
	// Native clocks are the default. Keep the pool armed even while the
	// no-gpu-timestamps gate is open: returning to native clocks mid-pass must
	// not require a reset or drop the first packet just to initialize the pool.
	if (!Initialize()) return;
	for (uint32_t i = 0; i < BlockCount; ++i) {
		const auto index = (m_next + i) % BlockCount;
		if (m_blocks[index].pending) continue;
		m_blocks[index].used = 0;
		m_blocks[index].samples.clear(); // Keep storage for the next use of this block.
		m_active = static_cast<int32_t>(index);
		m_next = (index + 1) % BlockCount;
		Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::OtherNative);
		command.ResetQueryPool(m_pool, index * QueriesPerBlock, QueriesPerBlock);
		break;
	}
}

void GuestClock::Cancel(uint64_t address, uint64_t size) {
	if (!size || m_latest.empty()) return;
	// Most immediate labels write one aligned dword/qword. Pending clocks are
	// always 8-byte aligned, so only one slot can overlap this common case.
	if (size <= 8 - (address & 7u)) {
		m_latest.erase(address & ~uint64_t{7});
		return;
	}
	for (auto it = m_latest.begin(); it != m_latest.end();) {
		const auto other = it->first;
		const bool overlap = address <= other ? other - address < size : address - other < 8;
		if (overlap) it = m_latest.erase(it); else ++it;
	}
}

bool GuestClock::Record(uint64_t address) {
	if (!Common::SubmitProfile::GpuTimestampsEnabled()) return false;
	if (m_active < 0 || m_blocks[m_active].used == QueriesPerBlock ||
	    (address & 7u) != 0 || !m_context.IsMapped(address, 8)) {
		Common::SubmitProfile::AddExtra(Extra::GpuTimestampsFallback);
		return false;
	}
	uint64_t previous = 0;
	// This address will be written through the backing alias by a completion,
	// which cannot fault or synchronously ask the GPU thread to retire readers.
	m_context.GetBufferCache().ExcludeHostImport(address, 8, Extra::HostImportFallbackGpu);
	if (!LibKernel::Memory::TryReadBacking(address, &previous, sizeof(previous))) {
		Common::SubmitProfile::AddExtra(Extra::GpuTimestampsFallback);
		return false;
	}
	const auto block = static_cast<uint32_t>(m_active);
	auto& state = m_blocks[block];
	const auto index = state.used++;
	const auto query = block * QueriesPerBlock + index;
	const auto serial = ++m_serial;
	m_latest[address] = serial;
	state.samples.push_back({address, serial, previous});
	++state.pending;
	++m_pending;
	auto& scheduler = m_context.GetCommandScheduler();
	Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::OtherNative);
	scheduler.Current().WriteTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, m_pool, query);
	// Extend only consecutive clock completions in this tick. An intervening
	// normal callback (e.g. fault processing) must retain its publication order.
	// Metadata is filled on the command lane before this tick can be submitted.
	if (index != 0 && state.operation_sequence == scheduler.DeferredOperationSequence()) {
		++state.samples[state.last_batch].batch_size;
	} else {
		state.last_batch = index;
		state.operation_sequence = scheduler.DeferOperation([this, block, index] { Complete(block, index); });
	}
	if (state.used == 1 && m_context.HasGpu()) {
		// One wake per buffer, queued while recording (not from Submit, which
		// also runs during scheduler shutdown). It waits for this native tick.
		// Otherwise normal completions could starve a guest polling an isolated
		// timestamp without sending another draw. Shutdown's Finish drains it
		// before GuestGpu is destroyed.
		scheduler.DeferPriorityOperation([gpu = &m_context.GetGpu()] { gpu->WakeClockCompletions(); });
	}
	Common::SubmitProfile::AddExtra(Extra::GpuTimestampsRecorded);
	return true;
}

void GuestClock::Complete(uint32_t block, uint32_t first) {
	auto& state = m_blocks[block];
	const auto count = state.samples[first].batch_size;
	struct Result { uint64_t value, available; };
	std::array<Result, QueriesPerBlock> native;
	const auto status = m_context.GetGraphics().device.getQueryPoolResults(m_pool,
	    block * QueriesPerBlock + first, count, count * sizeof(Result), native.data(), sizeof(Result),
	    vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
	Common::SubmitProfile::AddExtra(Extra::GpuTimestampBatches);
	for (uint32_t i = 0; i < count; ++i) {
		// Do not inspect undefined output if the non-waiting query failed.
		const bool ready = status == vk::Result::eSuccess && native[i].available != 0;
		Publish(state.samples[first + i], ready ? native[i].value : 0, ready);
	}
	state.pending -= count;
	m_pending -= count;
}

void GuestClock::Publish(const Sample& sample, uint64_t native, bool ready) {
	const auto address = sample.address, serial = sample.serial, previous = sample.previous;
	const auto found = m_latest.find(address);
	if (found == m_latest.end() || found->second != serial) {
		// A later timestamp or immediate PM4 write superseded this slot. In
		// particular, do not overwrite the CPU path after disabling native clocks.
		Common::SubmitProfile::AddExtra(Extra::GpuTimestampsLate);
		return;
	}
	m_latest.erase(found);
	uint64_t value = 0;
	if (!ready || !Convert(native, m_native_anchor, m_guest_anchor, m_mask, m_period_ns, value)) {
		Common::SubmitProfile::AddExtra(Extra::GpuTimestampsFailed);
		return;
	}
	if (!m_context.IsMapped(address, 8)) {
		Common::SubmitProfile::AddExtra(Extra::GpuTimestampsLate);
		return;
	}
	// Stage one does not arbitrate with a GPU writer to the same tracked page.
	// Preserve its data rather than starting a readback from a completion or
	// falsely clearing GPU ownership. Clean resident buffers ARE invalidated.
	auto& cache = m_context.GetBufferCache();
	if (cache.IsRegionGpuModified(address, 8) || cache.HasUnpublishedDownloads() ||
	    m_context.GetTextureCache().IsRegionRegistered(address, 8)) {
		Common::SubmitProfile::AddExtra(Extra::GpuTimestampsConflict);
		return;
	}
	// No resident buffer means no stale GPU copy/BDA upload to invalidate.
	// Registration queues its own sync if the address becomes resident later.
	// Keep texture page watchers and descriptor-heap invalidation independent:
	// neither is implied by buffer residency. Unmapping is serialized on this lane.
	if (cache.IsRegionRegistered(address, 8)) {
		cache.InvalidateMemory(address, 8);
		Common::SubmitProfile::AddExtra(Extra::GpuTimestampBufferInvalidations);
	} else {
		Common::SubmitProfile::AddExtra(Extra::GpuTimestampBuffersAbsent);
	}
	m_context.GetTextureCache().InvalidateMemory(address, 8);
	m_context.GetImageHeap().InvalidateMemory(address, 8);
	// The CAS checks the old slot value under the backing lock; a separate read
	// would duplicate that lock and still need the CAS to prevent a racing write.
	const bool published = LibKernel::Memory::TryCompareExchangeBacking64(address, previous, value);
	if (published) Common::FrameTimeline::Current(Common::FrameTimeline::Kind::ClockPublish, 0, address, value);
	Common::SubmitProfile::AddExtra(published ? Extra::GpuTimestampsPublished : Extra::GpuTimestampsLate);
}
} // namespace Libs::Graphics
