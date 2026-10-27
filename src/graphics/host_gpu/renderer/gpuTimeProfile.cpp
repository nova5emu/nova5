#include "common/gpuThreadSampler.h"
#include "graphics/host_gpu/renderer/gpuTimeProfile.h"

#include "common/assert.h"
#include "common/frameTimeline.h"
#include "common/timer.h"
#include "common/submitProfile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"
#include "graphics/host_gpu/renderer/sync.h"

#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace Libs::Graphics {

using Extra = Common::SubmitProfile::Extra;

void RecordIndirectImageWorkImpl(Common::ReadbackProfile::DrawWork::Op op, const ImageInfo& info,
                                 uint32_t flags, uint64_t bytes) {
	Common::ReadbackProfile::DrawWork::Record(op, info.data.address, info.data.size,
	    static_cast<uint32_t>(info.pixel_format), info.extent.width, info.extent.height,
	    info.TransferLayers(), flags, bytes);
}

GpuTimeProfile::GpuTimeProfile(GraphicContext& graphics): m_graphics(graphics) {}

GpuTimeProfile::~GpuTimeProfile() {
	if (m_pool) m_graphics.device.destroyQueryPool(m_pool);
	if (m_clock_pool) m_graphics.device.destroyQueryPool(m_clock_pool);
}

bool GpuTimeProfile::GuestClockRecording() {
	static const bool armed = [] {
		const auto* value = std::getenv("KYTY_GUEST_CLOCK_PROFILE");
		const bool enabled = value && std::string_view(value) == "1";
		if (enabled) std::fprintf(stderr, "[GUEST_CLOCK] armed shadow_only=1 per_window=256 total_limit=8192 ref_hz=100000000\n");
		return enabled;
	}();
	return armed && Common::SubmitProfile::Recording();
}

void GpuTimeProfile::ObserveGuestClock(CommandScheduler& scheduler, GuestClockSample sample) {
	if (&scheduler != m_owner || !GuestClockRecording() || m_clock_unavailable) return;
	const auto window = Common::SubmitProfile::Detail::generation.load(std::memory_order_relaxed);
	if (m_clock_window != window) {
		if (m_clock_attempts) std::fprintf(stderr,
		    "[GUEST_CLOCK_WINDOW] window=%llu attempts=%u capacity_drops=%u\n",
		    static_cast<unsigned long long>(m_clock_window), m_clock_attempts, m_clock_capacity_drops);
		m_clock_window = window; m_clock_attempts = m_clock_capacity_drops = 0;
	}
	if (++m_clock_attempts > GuestClockSlots) {
		if (m_clock_attempts == GuestClockSlots + 1) std::fprintf(stderr,
		    "[GUEST_CLOCK_LIMIT] window=%llu reason=window\n", static_cast<unsigned long long>(window));
		return;
	}
	if (m_clock_sequence == GuestClockLimit) {
		if (!m_clock_limit_reported) std::fprintf(stderr, "[GUEST_CLOCK_LIMIT] reason=process\n");
		m_clock_limit_reported = true;
		return;
	}
	// The gate may open in an already-recording command buffer. Wait for the
	// next natural BeginCommand to reset queries outside rendering; never split
	// a pass or submit just to arm this diagnostic.
	if (!m_clock_pool || m_clock_pending == GuestClockSlots) {
		if (++m_clock_capacity_drops == 1) std::fprintf(stderr,
		    "[GUEST_CLOCK_LIMIT] window=%llu reason=%s\n", static_cast<unsigned long long>(window),
		    m_clock_pool ? "pending" : "await_command_buffer");
		return;
	}
	const auto slot = static_cast<uint32_t>(m_clock_sequence);
	++m_clock_pending;
	const auto sequence = ++m_clock_sequence;
	const auto tick = scheduler.CurrentTick();
	const bool experiment = Common::SubmitProfile::ExperimentActive();
	// Every slot is used once. All resets were recorded at natural command-buffer
	// begin; no hostQueryReset feature, EndRendering, Flush or Wait is needed.
	const auto& command = scheduler.Current();
	Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::OtherNative);
	command.WriteTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, m_clock_pool, slot * 2);
	Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::OtherNative);
	command.WriteTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, m_clock_pool, slot * 2 + 1);
	scheduler.DeferOperation([this, sample, slot, sequence, tick, window, experiment] {
		struct Result { uint64_t value = 0, available = 0; };
		std::array<Result, 2> result {};
		const auto status = m_graphics.device.getQueryPoolResults(m_clock_pool, slot * 2, 2,
		    sizeof(result), result.data(), sizeof(Result),
		    vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
		const bool ready = status == vk::Result::eSuccess && result[0].available && result[1].available;
		if (ready) ++m_clock_completed; else ++m_clock_failed;
		std::fprintf(stderr,
		    "[GUEST_CLOCK] seq=%llu window=%llu experiment=%u source=%s queue=%d frame=%u submit=%llu tick=%llu "
		    "address=0x%llx bytes=%u control=0x%x event=0x%x cpu_ref=%llu cpu_callback=%llu "
		    "ready=%u status=%d gpu_top=%llu gpu_bottom=%llu mask=0x%llx period_ns=%.9g\n",
		    static_cast<unsigned long long>(sequence), static_cast<unsigned long long>(window), unsigned(experiment),
		    sample.release ? "release_mem" : "copy_data", sample.queue, sample.frame,
		    static_cast<unsigned long long>(sample.submit), static_cast<unsigned long long>(tick),
		    static_cast<unsigned long long>(sample.address), sample.bytes, sample.control, sample.event,
		    static_cast<unsigned long long>(sample.cpu_reference), static_cast<unsigned long long>(Sync::ReadReferenceClock()),
		    unsigned(ready), static_cast<int>(status), static_cast<unsigned long long>(result[0].value & m_clock_mask),
		    static_cast<unsigned long long>(result[1].value & m_clock_mask),
		    static_cast<unsigned long long>(m_clock_mask), m_clock_period_ns);
		// Normal scheduler callbacks are serialized with command recording.
		--m_clock_pending;
	});
}

void GpuTimeProfile::InitializeGuestClock(const CommandBuffer& command) {
	if (!m_clock_pool && !m_clock_unavailable) {
		uint32_t count = 0;
		m_graphics.physical_device.getQueueFamilyProperties(&count, nullptr);
		std::vector<vk::QueueFamilyProperties> families(count);
		m_graphics.physical_device.getQueueFamilyProperties(&count, families.data());
		const auto bits = m_graphics.queue_family < count ? families[m_graphics.queue_family].timestampValidBits : 0u;
		m_clock_mask = bits == 0 ? 0 : bits >= 64 ? UINT64_MAX : (uint64_t{1} << bits) - 1;
		m_clock_period_ns = m_graphics.physical_device_properties.limits.timestampPeriod;
		vk::QueryPoolCreateInfo info {};
		info.queryType = vk::QueryType::eTimestamp; info.queryCount = GuestClockLimit * 2;
		if (!m_clock_mask || m_clock_period_ns <= 0 ||
		    m_graphics.device.createQueryPool(&info, nullptr, &m_clock_pool) != vk::Result::eSuccess) {
			m_clock_unavailable = true;
			std::fprintf(stderr, "[GUEST_CLOCK] unavailable=1\n");
			return;
		}
		Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::OtherNative);
		command.ResetQueryPool(m_clock_pool, 0, GuestClockLimit * 2);
	}
}

void GpuTimeProfile::OnBegin(CommandScheduler& scheduler, const CommandBuffer& command) {
	// Only the owning scheduler may touch the profile state (the present scheduler also calls in).
	if (&scheduler != m_owner) return;
	if (GuestClockRecording()) InitializeGuestClock(command);
	m_active = -1;
	++m_generation;
	const bool timeline = Common::FrameTimeline::Recording();
	if (timeline && m_timeline_native_unavailable && !Common::SubmitProfile::Recording()) return;
	const bool recording = Common::SubmitProfile::Recording() || timeline;
	if (recording && !m_recording) ++m_session; // Results from earlier sessions are discarded.
	m_recording = recording;
	if (!recording) {
		// Drain captured queries at natural command-buffer boundaries; no waits
		// or extra submits to make the diagnostic finish.
		if (m_timeline_pending && (++m_begins & 31u) == 0) Poll(scheduler);
		return;
	}
	if (!m_pool) {
		uint32_t count = 0;
		m_graphics.physical_device.getQueueFamilyProperties(&count, nullptr);
		std::vector<vk::QueueFamilyProperties> families(count);
		m_graphics.physical_device.getQueueFamilyProperties(&count, families.data());
		const auto bits = m_graphics.queue_family < count ? families[m_graphics.queue_family].timestampValidBits : 0u;
		m_valid_mask = bits == 0 ? 0 : bits >= 64 ? ~uint64_t {0} : (uint64_t {1} << bits) - 1;
		m_period_ns  = m_graphics.physical_device_properties.limits.timestampPeriod;
		if (m_valid_mask == 0) {
			Common::FrameTimeline::Current(Common::FrameTimeline::Kind::NativeUnavailable, 0, 1);
			m_timeline_native_unavailable = true;
			return;
		}
		vk::QueryPoolCreateInfo info {};
		info.queryType  = vk::QueryType::eTimestamp;
		info.queryCount = Blocks * SlotsPerBlock;
		const auto result = m_graphics.device.createQueryPool(&info, nullptr, &m_pool);
		if (result != vk::Result::eSuccess && timeline && !Common::SubmitProfile::Recording()) {
			m_timeline_native_unavailable = true;
			Common::FrameTimeline::Current(Common::FrameTimeline::Kind::NativeUnavailable, 0, 6, static_cast<uint64_t>(result));
			return;
		}
		EXIT_IF(result != vk::Result::eSuccess);
	}
	if (m_valid_mask == 0) return;
	if (timeline && !m_timeline_calibration_tried) CalibrateTimeline();
	if ((++m_begins & 31u) == 0) Poll(scheduler);
	for (uint32_t probe = 0; probe < Blocks; ++probe) {
		const auto index = (m_next + probe) % Blocks;
		if (m_blocks[index].tick != 0) continue;
		m_next   = (index + 1) % Blocks;
		m_active = static_cast<int32_t>(index);
		break;
	}
	if (m_active < 0) {
		Common::FrameTimeline::Current(Common::FrameTimeline::Kind::NativeUnavailable, scheduler.CurrentTick(), 2);
		Common::SubmitProfile::AddExtra(Extra::GpuBlocksSkipped);
		return;
	}
	auto& block      = m_blocks[m_active];
	block.used       = 2;
	block.ended      = 0;
	block.generation = m_generation;
	block.session    = m_session;
	block.timeline   = timeline;
	block.frame = block.submission = 0;
	const auto first = static_cast<uint32_t>(m_active) * SlotsPerBlock;
	Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::OtherNative);
	command.ResetQueryPool(m_pool, first, SlotsPerBlock);
	Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::OtherNative);
	command.WriteTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, m_pool, first);
}

GpuTimeProfile::Token GpuTimeProfile::Begin(CommandScheduler& scheduler, const CommandBuffer& command) {
	if (&scheduler != m_owner || m_active < 0) return {};
	auto& block = m_blocks[m_active];
	if (block.used + 2 > SlotsPerBlock) {
		Common::SubmitProfile::AddExtra(Extra::GpuPairsDroppedCapacity);
		return {};
	}
	const auto slot = block.used;
	block.used += 2;
	const auto query = static_cast<uint32_t>(m_active) * SlotsPerBlock + slot;
	Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::OtherNative);
	command.WriteTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, m_pool, query);
	return {query, m_generation};
}

void GpuTimeProfile::End(CommandScheduler& scheduler, const CommandBuffer& command, Token token,
                         Category category) {
	if (&scheduler != m_owner || token.query == UINT32_MAX) return;
	if (token.generation != m_generation || m_active != static_cast<int32_t>(token.query / SlotsPerBlock)) {
		Common::SubmitProfile::AddExtra(Extra::GpuPairsDroppedFlush);
		return;
	}
	auto&      block = m_blocks[token.query / SlotsPerBlock];
	const auto pair  = (token.query % SlotsPerBlock) / 2;
	block.categories[pair] = static_cast<uint8_t>(category);
	block.ended |= 1u << pair;
	Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::OtherNative);
	command.WriteTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, m_pool, token.query + 1);
}

void GpuTimeProfile::OnSubmit(CommandScheduler& scheduler, const CommandBuffer& command, uint64_t tick) {
	if (&scheduler != m_owner || m_active < 0) return;
	const auto first = static_cast<uint32_t>(m_active) * SlotsPerBlock;
	Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::OtherNative);
	command.WriteTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, m_pool, first + 1);
	m_blocks[m_active].tick = tick;
	if (m_blocks[m_active].timeline) {
		m_blocks[m_active].frame = Common::FrameTimeline::context.frame;
		m_blocks[m_active].submission = Common::FrameTimeline::context.submission;
		++m_timeline_pending;
	}
	m_active = -1;
}

void GpuTimeProfile::Poll(CommandScheduler& scheduler) {
	struct Result { uint64_t value = 0, available = 0; };
	std::array<Result, SlotsPerBlock> results {};
	static constexpr std::array<std::pair<Extra, Extra>, static_cast<size_t>(Category::Count)> CategoryCounters {{
	    {Extra::GpuDetiles, Extra::GpuDetileNs},
	    {Extra::GpuUploads, Extra::GpuUploadNs},
	    {Extra::GpuDepthDetiles, Extra::GpuDepthDetileNs},
	    {Extra::GpuDepthConverts, Extra::GpuDepthConvertNs},
	    {Extra::GpuDepthUploads, Extra::GpuDepthUploadNs},
	    {Extra::GpuIndirectMeshConverts, Extra::GpuIndirectMeshConvertNs},
	    {Extra::GpuIndirectMeshPrepares, Extra::GpuIndirectMeshPrepareNs},
	    {Extra::GpuIndirectMeshEmits, Extra::GpuIndirectMeshEmitNs},
	    {Extra::GpuIndirectPrechecks, Extra::GpuIndirectPrecheckNs},
	    {Extra::GpuIndirectDiscoveries, Extra::GpuIndirectDiscoveryNs},
	}};
	for (uint32_t index = 0; index < Blocks; ++index) {
		auto& block = m_blocks[index];
		if (block.tick == 0 || !scheduler.GetMasterSemaphore().IsFree(block.tick)) continue;
		const auto status = m_graphics.device.getQueryPoolResults(
		    m_pool, index * SlotsPerBlock, block.used, block.used * sizeof(Result), results.data(),
		    sizeof(Result), vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
		// NOT_READY is expected when an abandoned pair left its end slot unwritten.
		bool ready = status == vk::Result::eSuccess || status == vk::Result::eNotReady;
		for (uint32_t slot = 0; ready && slot < block.used; ++slot) {
			const bool needed = slot < 2 || (block.ended >> (slot / 2) & 1u) != 0;
			ready = !needed || results[slot].available != 0;
		}
		if (!ready) {
			Common::SubmitProfile::AddExtra(Extra::GpuResultsNotReady);
			continue; // Retried on a later poll; the block stays reserved.
		}
		if (block.timeline) {
			uint64_t begin = 0, end = 0;
			if (TimelineTime(results[0].value, begin) && TimelineTime(results[1].value, end) && end >= begin) {
				using namespace Common::FrameTimeline;
				Event(Kind::NativeBegin, block.frame, block.submission, block.tick, results[0].value, 0, 0, 0, begin);
				Event(Kind::NativeEnd, block.frame, block.submission, block.tick, results[1].value, 0, 0, 0, end);
			} else {
				Common::FrameTimeline::Event(Common::FrameTimeline::Kind::NativeUnavailable, block.frame, block.submission, block.tick, 3);
			}
			--m_timeline_pending;
			block.timeline = false;
		}
		block.tick = 0;
		if (block.session != m_session) continue; // Recorded in an earlier profile session.
		const auto elapsed = [&](uint32_t begin) {
			// Modular difference within the valid timestamp bits.
			const auto ticks = (results[begin + 1].value - results[begin].value) & m_valid_mask;
			return static_cast<uint64_t>(static_cast<double>(ticks) * m_period_ns);
		};
		Common::SubmitProfile::AddExtra(Extra::GpuCommandBuffers);
		Common::SubmitProfile::AddExtra(Extra::GpuCommandBufferNs, elapsed(0));
		for (uint32_t slot = 2; slot + 1 < block.used; slot += 2) {
			if ((block.ended >> (slot / 2) & 1u) == 0) continue;
			const auto category = block.categories[slot / 2];
			if (category >= CategoryCounters.size()) continue;
			Common::SubmitProfile::AddExtra(CategoryCounters[category].first);
			Common::SubmitProfile::AddExtra(CategoryCounters[category].second, elapsed(slot));
		}
	}
}

void GpuTimeProfile::CalibrateTimeline() {
	m_timeline_calibration_tried = true;
	if (!m_graphics.calibrated_timestamps_enabled || !std::isfinite(m_period_ns) || m_period_ns <= 0) {
		Common::FrameTimeline::Current(Common::FrameTimeline::Kind::NativeUnavailable, 0, 4);
		return;
	}
	m_timeline_frequency = Common::Timer::QueryPerformanceFrequency();
	uint64_t best = UINT64_MAX;
	vk::CalibratedTimestampInfoEXT info {};
	info.timeDomain = vk::TimeDomainEXT::eDevice;
	// Same bracket method as GuestClock, but using QPC directly for ALL CPU
	// events. The guest reference clock uses TSC, so its epoch is not reused.
	for (unsigned i = 0; i < 8; ++i) {
		uint64_t native = 0, deviation = 0;
		const auto before = Common::FrameTimeline::Now();
		const auto status = m_graphics.device.getCalibratedTimestampsEXT(1, &info, &native, &deviation);
		const auto after = Common::FrameTimeline::Now();
		if (status != vk::Result::eSuccess || after < before) continue;
		const auto bracket = static_cast<uint64_t>((after - before) * 1.0e9L / m_timeline_frequency);
		if (deviation > UINT64_MAX - bracket || bracket + deviation >= best) continue;
		best = bracket + deviation;
		m_timeline_native = native & m_valid_mask;
		m_timeline_host = before + (after - before) / 2;
	}
	m_timeline_calibrated = best != UINT64_MAX;
	using namespace Common::FrameTimeline;
	if (m_timeline_calibrated) {
		Event(Kind::Calibration, 0, 0, 0, m_timeline_native, m_valid_mask, best,
		      static_cast<uint64_t>(m_period_ns * 1000.0), m_timeline_host);
	} else Current(Kind::NativeUnavailable, 0, 5);
}

bool GpuTimeProfile::TimelineTime(uint64_t native, uint64_t& host) const {
	if (!m_timeline_calibrated) return false;
	const auto elapsed = Common::FrameTimeline::Now() - m_timeline_host;
	const long double half_wrap = static_cast<long double>(m_valid_mask >> 1) * m_period_ns * m_timeline_frequency / 1.0e9L;
	if (elapsed >= half_wrap) return false; // Do not guess a wrap epoch on narrow counters.
	return Common::FrameTimeline::ConvertClock(native, m_timeline_native, m_timeline_host,
	    m_valid_mask, m_period_ns, m_timeline_frequency, host);
}

GpuTimeScope::GpuTimeScope(CommandScheduler& scheduler, GpuTimeProfile::Category category)
    : m_scheduler(scheduler), m_category(category) {
	if (Common::SubmitProfile::Recording()) {
		m_token = scheduler.Context().GetGpuTimeProfile().Begin(scheduler, scheduler.Current());
	}
}

GpuTimeScope::~GpuTimeScope() {
	if (m_token.query == UINT32_MAX) return;
	m_scheduler.Context().GetGpuTimeProfile().End(m_scheduler, m_scheduler.Current(), m_token,
	                                              m_category);
}

} // namespace Libs::Graphics
