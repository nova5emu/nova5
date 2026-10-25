#include "graphics/host_gpu/renderer/gpuTimeProfile.h"

#include "common/assert.h"
#include "common/submitProfile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/renderContext.h"

#include <vector>

namespace Libs::Graphics {

using Extra = Common::SubmitProfile::Extra;

GpuTimeProfile::GpuTimeProfile(GraphicContext& graphics): m_graphics(graphics) {}

GpuTimeProfile::~GpuTimeProfile() {
	if (m_pool) m_graphics.device.destroyQueryPool(m_pool);
}

void GpuTimeProfile::OnBegin(CommandScheduler& scheduler, vk::CommandBuffer command) {
	// Only the owning scheduler may touch the profile state (the present scheduler also calls in).
	if (&scheduler != m_owner) return;
	m_active = -1;
	++m_generation;
	const bool recording = Common::SubmitProfile::Recording();
	if (recording && !m_recording) ++m_session; // Results from earlier sessions are discarded.
	m_recording = recording;
	if (!recording) return;
	if (!m_pool) {
		uint32_t count = 0;
		m_graphics.physical_device.getQueueFamilyProperties(&count, nullptr);
		std::vector<vk::QueueFamilyProperties> families(count);
		m_graphics.physical_device.getQueueFamilyProperties(&count, families.data());
		const auto bits = m_graphics.queue_family < count ? families[m_graphics.queue_family].timestampValidBits : 0u;
		m_valid_mask = bits == 0 ? 0 : bits >= 64 ? ~uint64_t {0} : (uint64_t {1} << bits) - 1;
		m_period_ns  = m_graphics.physical_device_properties.limits.timestampPeriod;
		if (m_valid_mask == 0) return;
		vk::QueryPoolCreateInfo info {};
		info.queryType  = vk::QueryType::eTimestamp;
		info.queryCount = Blocks * SlotsPerBlock;
		EXIT_IF(m_graphics.device.createQueryPool(&info, nullptr, &m_pool) != vk::Result::eSuccess);
	}
	if (m_valid_mask == 0) return;
	if ((++m_begins & 31u) == 0) Poll(scheduler);
	for (uint32_t probe = 0; probe < Blocks; ++probe) {
		const auto index = (m_next + probe) % Blocks;
		if (m_blocks[index].tick != 0) continue;
		m_next   = (index + 1) % Blocks;
		m_active = static_cast<int32_t>(index);
		break;
	}
	if (m_active < 0) {
		Common::SubmitProfile::AddExtra(Extra::GpuBlocksSkipped);
		return;
	}
	auto& block      = m_blocks[m_active];
	block.used       = 2;
	block.ended      = 0;
	block.generation = m_generation;
	block.session    = m_session;
	const auto first = static_cast<uint32_t>(m_active) * SlotsPerBlock;
	command.resetQueryPool(m_pool, first, SlotsPerBlock);
	command.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, m_pool, first);
}

GpuTimeProfile::Token GpuTimeProfile::Begin(CommandScheduler& scheduler, vk::CommandBuffer command) {
	if (&scheduler != m_owner || m_active < 0) return {};
	auto& block = m_blocks[m_active];
	if (block.used + 2 > SlotsPerBlock) {
		Common::SubmitProfile::AddExtra(Extra::GpuPairsDroppedCapacity);
		return {};
	}
	const auto slot = block.used;
	block.used += 2;
	const auto query = static_cast<uint32_t>(m_active) * SlotsPerBlock + slot;
	command.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, m_pool, query);
	return {query, m_generation};
}

void GpuTimeProfile::End(CommandScheduler& scheduler, vk::CommandBuffer command, Token token,
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
	command.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, m_pool, token.query + 1);
}

void GpuTimeProfile::OnSubmit(CommandScheduler& scheduler, vk::CommandBuffer command, uint64_t tick) {
	if (&scheduler != m_owner || m_active < 0) return;
	const auto first = static_cast<uint32_t>(m_active) * SlotsPerBlock;
	command.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, m_pool, first + 1);
	m_blocks[m_active].tick = tick;
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

GpuTimeScope::GpuTimeScope(CommandScheduler& scheduler, GpuTimeProfile::Category category)
    : m_scheduler(scheduler), m_category(category) {
	if (Common::SubmitProfile::Recording()) {
		m_token = scheduler.Context().GetGpuTimeProfile().Begin(scheduler, scheduler.Current().Handle());
	}
}

GpuTimeScope::~GpuTimeScope() {
	if (m_token.query == UINT32_MAX) return;
	m_scheduler.Context().GetGpuTimeProfile().End(m_scheduler, m_scheduler.Current().Handle(), m_token,
	                                              m_category);
}

} // namespace Libs::Graphics
