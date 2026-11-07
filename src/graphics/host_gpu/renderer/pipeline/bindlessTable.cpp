// Adapted from mn3ema/KytyPS5-Frank (GPL-2.0), d6a807a4, 29bd4508 and c5a6961b.
#include "graphics/host_gpu/renderer/pipeline/bindlessTable.h"

#include <cstdio>
#include <mutex>
#include <set>
#include <string>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/regionDefinitions.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/shaderBindings.h"


#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <array>
#include <cstdlib>
#include <cstring>

namespace Libs::Graphics {

void BindlessTable::WatchHeap(Heap& heap) {
	heap.watched_entries  = heap.guest_entries;
	heap.watch_generation = m_watch_generation.load(std::memory_order_acquire);
	const auto table = heap.base + heap.table_offset;
	const auto begin = Common::AlignDown(table, TRACKER_PAGE_SIZE);
	const auto end   = Common::AlignUp(table + uint64_t {heap.guest_entries} * 32u, TRACKER_PAGE_SIZE);
	if (end <= begin) {
		return;
	}
	std::scoped_lock lock {m_watch_mutex};
	auto watch = std::ranges::find_if(m_watches, [&](const Watch& w) { return w.begin == begin; });
	if (watch == m_watches.end()) {
		watch        = m_watches.insert(m_watches.end(), Watch {});
		watch->begin = begin;
		watch->end   = begin;
	}
	if (end <= watch->end) {
		return;
	}
	// The heap grew (or is new): watch the pages it now covers.
	const auto first = static_cast<size_t>((watch->end - begin) / TRACKER_PAGE_SIZE);
	const auto pages = static_cast<size_t>((end - begin) / TRACKER_PAGE_SIZE);
	watch->watched.resize(pages, 0u);
	watch->written.resize(pages, 0u);
	watch->end = end;
	for (size_t page = first; page < pages; page++) {
		watch->watched[page] = 1;
	}
	m_page_manager.UpdatePageWatchers<true>(begin + first * TRACKER_PAGE_SIZE,
	                                        (pages - first) * TRACKER_PAGE_SIZE);
	if (begin < m_watch_low.load(std::memory_order_relaxed)) {
		m_watch_low.store(begin, std::memory_order_release);
	}
	if (end > m_watch_high.load(std::memory_order_relaxed)) {
		m_watch_high.store(end, std::memory_order_release);
	}
}

void BindlessTable::OnCpuWrite(uint64_t vaddr, uint64_t size) {
	// Called for every CPU write fault: most are nowhere near a heap and must not take the lock.
	if (vaddr + size <= m_watch_low.load(std::memory_order_acquire) ||
	    vaddr >= m_watch_high.load(std::memory_order_acquire)) {
		return;
	}
	std::scoped_lock lock {m_watch_mutex};
	for (auto& watch: m_watches) {
		const auto begin = std::max(watch.begin, Common::AlignDown(vaddr, TRACKER_PAGE_SIZE));
		const auto end   = std::min(watch.end, vaddr + size);
		for (auto address = begin; address < end; address += TRACKER_PAGE_SIZE) {
			const auto page = static_cast<size_t>((address - watch.begin) / TRACKER_PAGE_SIZE);
			if (watch.watched[page] == 0) {
				continue;
			}
			// The faulting write must go through: stop watching the page until the next sync.
			watch.watched[page] = 0;
			watch.written[page] = 1;
			m_page_manager.UpdatePageWatchers<false>(address, TRACKER_PAGE_SIZE);
			m_heap_writes.store(true, std::memory_order_release);
		}
	}
}

void BindlessTable::UnwatchRange(uint64_t vaddr, uint64_t size) {
	std::scoped_lock lock {m_watch_mutex};
	for (auto& watch: m_watches) {
		if (vaddr + size <= watch.begin || vaddr >= watch.end) {
			continue;
		}
		// Preserve invalidation after the watch itself is removed. The same VA may
		// subsequently name another allocation, as in Wolverine's former AMM collision.
		m_unmapped_ranges.emplace_back(watch.begin, watch.end);
		m_heap_writes.store(true, std::memory_order_release);
		// Part of the table is unmapped: drop the whole watch. WatchHeap builds it again the
		// next time a draw uses the heap, over what is mapped then.
		for (size_t page = 0; page < watch.watched.size(); page++) {
			if (watch.watched[page] != 0) {
				m_page_manager.UpdatePageWatchers<false>(watch.begin + page * TRACKER_PAGE_SIZE,
				                                         TRACKER_PAGE_SIZE);
			}
		}
		watch.watched.clear();
		watch.written.clear();
		watch.end = watch.begin;
		m_watch_generation.fetch_add(1, std::memory_order_acq_rel);
	}
}

void BindlessTable::TakeWrittenRanges(std::vector<std::pair<uint64_t, uint64_t>>& ranges) {
	ranges.clear();
	std::scoped_lock lock {m_watch_mutex};
	ranges.swap(m_unmapped_ranges);
	m_heap_writes.store(false, std::memory_order_release);
	for (auto& watch: m_watches) {
		for (size_t page = 0; page < watch.written.size(); page++) {
			if (watch.written[page] == 0) {
				continue;
			}
			const auto address  = watch.begin + page * TRACKER_PAGE_SIZE;
			watch.written[page] = 0;
			// Watched again before the caller reads the entries: a later write is not lost.
			if (watch.watched[page] == 0) {
				watch.watched[page] = 1;
				m_page_manager.UpdatePageWatchers<true>(address, TRACKER_PAGE_SIZE);
			}
			if (!ranges.empty() && ranges.back().second == address) {
				ranges.back().second += TRACKER_PAGE_SIZE;
			} else {
				ranges.emplace_back(address, address + TRACKER_PAGE_SIZE);
			}
		}
	}
}

BindlessTable::BindlessTable(GraphicContext& graphics, CommandScheduler& scheduler,
                             PageManager& page_manager)
    : m_graphics(graphics), m_scheduler(scheduler), m_page_manager(page_manager) {
	if (!graphics.bindless_enabled) {
		return;
	}
	m_images_per_array =
	    std::min(MaxImagesPerArray, graphics.bindless_max_sampled_images / ImageArrays);
	if (m_images_per_array == 0) {
		LOGF("Bindless table: the device allows no update-after-bind sampled images\n");
		return;
	}

	constexpr auto image_flags = vk::DescriptorBindingFlagBits::ePartiallyBound |
	                             vk::DescriptorBindingFlagBits::eUpdateAfterBind |
	                             vk::DescriptorBindingFlagBits::eUpdateUnusedWhilePending;
	// The buffers are written once, here, so they need no update-after-bind.
	constexpr auto buffer_flags = vk::DescriptorBindingFlags {};
	const std::array<vk::DescriptorSetLayoutBinding, 6> bindings {{
	    {Images2D, vk::DescriptorType::eSampledImage, m_images_per_array, vk::ShaderStageFlagBits::eAll,
	     nullptr},
	    {Images2DArray, vk::DescriptorType::eSampledImage, m_images_per_array,
	     vk::ShaderStageFlagBits::eAll, nullptr},
	    {ImagesCube, vk::DescriptorType::eSampledImage, m_images_per_array,
	     vk::ShaderStageFlagBits::eAll, nullptr},
	    {Images3D, vk::DescriptorType::eSampledImage, m_images_per_array, vk::ShaderStageFlagBits::eAll,
	     nullptr},
	    {Translation, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eAll, nullptr},
	    {Feedback, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eAll, nullptr},
	}};
	const std::array<vk::DescriptorBindingFlags, 6> binding_flags {
	    image_flags, image_flags, image_flags, image_flags, buffer_flags, buffer_flags};
	vk::DescriptorSetLayoutBindingFlagsCreateInfo flags_info {};
	flags_info.bindingCount  = static_cast<uint32_t>(binding_flags.size());
	flags_info.pBindingFlags = binding_flags.data();
	vk::DescriptorSetLayoutCreateInfo layout_info {};
	layout_info.pNext        = &flags_info;
	layout_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool;
	layout_info.bindingCount = static_cast<uint32_t>(bindings.size());
	layout_info.pBindings    = bindings.data();
	RequireVulkanSuccess(graphics.device.createDescriptorSetLayout(&layout_info, nullptr, &m_layout),
	                     "create bindless descriptor layout");

	const std::array<vk::DescriptorPoolSize, 2> sizes {{
	    {vk::DescriptorType::eSampledImage, m_images_per_array * ImageArrays},
	    {vk::DescriptorType::eStorageBuffer, 2},
	}};
	vk::DescriptorPoolCreateInfo pool_info {};
	pool_info.flags         = vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind;
	pool_info.maxSets       = 1;
	pool_info.poolSizeCount = static_cast<uint32_t>(sizes.size());
	pool_info.pPoolSizes    = sizes.data();
	RequireVulkanSuccess(graphics.device.createDescriptorPool(&pool_info, nullptr, &m_pool),
	                     "create bindless descriptor pool");

	vk::DescriptorSetAllocateInfo allocate_info {};
	allocate_info.descriptorPool     = m_pool;
	allocate_info.descriptorSetCount = 1;
	allocate_info.pSetLayouts        = &m_layout;
	RequireVulkanSuccess(graphics.device.allocateDescriptorSets(&allocate_info, &m_set),
	                     "allocate bindless descriptor set");

	// Updates and feedback clears are ordered on the GPU; only the completed
	// feedback snapshot is host-visible.
	m_translation = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::DeviceLocal, 0,
	                                         AllFlags, TranslationEntries * sizeof(uint32_t));
	m_feedback    = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::DeviceLocal, 0,
	                                         AllFlags, TranslationEntries * sizeof(uint32_t));
	m_feedback_snapshot = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::Download, 0,
	                                               AllFlags, TranslationEntries * sizeof(uint32_t));
	EXIT_IF(m_feedback_snapshot->Mapped().empty());
	// A region allocated after a snapshot was recorded reads as "no requests" until the next.
	std::fill(m_feedback_snapshot->Mapped().begin(), m_feedback_snapshot->Mapped().end(),
	          uint8_t {0});
	m_feedback_snapshot->Flush(0, m_feedback_snapshot->Size());
	SetVulkanObjectNameF(graphics.device, m_translation->Handle(), "Bindless Translation");
	SetVulkanObjectNameF(graphics.device, m_feedback->Handle(), "Bindless Feedback");

	const std::array<vk::DescriptorBufferInfo, 2> buffer_infos {{
	    {m_translation->Handle(), 0, VK_WHOLE_SIZE},
	    {m_feedback->Handle(), 0, VK_WHOLE_SIZE},
	}};
	std::array<vk::WriteDescriptorSet, 2> writes {};
	for (uint32_t i = 0; i < writes.size(); i++) {
		writes[i].dstSet          = m_set;
		writes[i].dstBinding      = Translation + i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType  = vk::DescriptorType::eStorageBuffer;
		writes[i].pBufferInfo     = &buffer_infos[i];
	}
	graphics.device.updateDescriptorSets(static_cast<uint32_t>(writes.size()), writes.data(), 0,
	                                     nullptr);
	CreatePlaceholders(scheduler);
	graphics.bindless_layout = m_layout;
	graphics.bindless_set    = m_set;
	LOGF("Bindless table: %u images per array, %u translation entries\n",
	     m_images_per_array, TranslationEntries);
}

void BindlessTable::CreatePlaceholders(CommandScheduler& /*scheduler*/) {
	struct Kind {
		vk::ImageType        type;
		vk::ImageViewType    view;
		uint32_t             layers;
		vk::ImageCreateFlags flags;
	};
	const std::array<Kind, ImageArrays> kinds {{
	    {vk::ImageType::e2D, vk::ImageViewType::e2D, 1, {}},
	    {vk::ImageType::e2D, vk::ImageViewType::e2DArray, 1, {}},
	    // Cube instructions use face coordinates and a 2D-array SPIR-V image in Nova5.
	    {vk::ImageType::e2D, vk::ImageViewType::e2DArray, 6, {}},
	    {vk::ImageType::e3D, vk::ImageViewType::e3D, 1, {}},
	}};
	for (uint32_t i = 0; i < Placeholders; i++) {
		vk::ImageCreateInfo info {};
		info.flags         = kinds[i % ImageArrays].flags;
		info.imageType     = kinds[i % ImageArrays].type;
		info.format        = vk::Format::eR8G8B8A8Unorm;
		info.extent        = vk::Extent3D {1, 1, 1};
		info.mipLevels     = 1;
		info.arrayLayers   = kinds[i % ImageArrays].layers;
		info.samples       = vk::SampleCountFlagBits::e1;
		info.tiling        = vk::ImageTiling::eOptimal;
		info.usage         = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
		info.initialLayout = vk::ImageLayout::eUndefined;
		EXIT_IF(!m_graphics.CreateImage(info, m_placeholders[i]));
		vk::ImageViewCreateInfo view {};
		view.image            = m_placeholders[i].image;
		view.viewType         = kinds[i % ImageArrays].view;
		view.format           = info.format;
		view.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, kinds[i % ImageArrays].layers};
		RequireVulkanSuccess(
		    m_graphics.device.createImageView(&view, nullptr, &m_placeholder_views[i]),
		    "create bindless placeholder view");
	}

	// One fenced submission, before any rendering: clears and a buffer fill are not allowed
	// inside the render passes draws record into.
	vk::CommandPoolCreateInfo pool_info {};
	pool_info.flags            = vk::CommandPoolCreateFlagBits::eTransient;
	pool_info.queueFamilyIndex = m_graphics.queue_family;
	vk::CommandPool pool       = nullptr;
	RequireVulkanSuccess(m_graphics.device.createCommandPool(&pool_info, nullptr, &pool),
	                     "create bindless init pool");
	vk::CommandBufferAllocateInfo allocate {};
	allocate.commandPool        = pool;
	allocate.level              = vk::CommandBufferLevel::ePrimary;
	allocate.commandBufferCount = 1;
	vk::CommandBuffer command   = nullptr;
	RequireVulkanSuccess(m_graphics.device.allocateCommandBuffers(&allocate, &command),
	                     "allocate bindless init command buffer");
	vk::CommandBufferBeginInfo begin {};
	begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
	RequireVulkanSuccess(command.begin(&begin), "begin bindless init command buffer");

	std::array<vk::ImageMemoryBarrier, Placeholders> to_transfer {};
	std::array<vk::ImageMemoryBarrier, Placeholders> to_read {};
	for (uint32_t i = 0; i < Placeholders; i++) {
		const vk::ImageSubresourceRange range {vk::ImageAspectFlagBits::eColor, 0, 1, 0,
		                                       kinds[i % ImageArrays].layers};
		to_transfer[i].srcAccessMask       = {};
		to_transfer[i].dstAccessMask       = vk::AccessFlagBits::eTransferWrite;
		to_transfer[i].oldLayout           = vk::ImageLayout::eUndefined;
		to_transfer[i].newLayout           = vk::ImageLayout::eTransferDstOptimal;
		to_transfer[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_transfer[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_transfer[i].image               = m_placeholders[i].image;
		to_transfer[i].subresourceRange    = range;
		to_read[i]                         = to_transfer[i];
		to_read[i].srcAccessMask           = vk::AccessFlagBits::eTransferWrite;
		to_read[i].dstAccessMask           = vk::AccessFlagBits::eShaderRead;
		to_read[i].oldLayout               = vk::ImageLayout::eTransferDstOptimal;
		to_read[i].newLayout               = vk::ImageLayout::eShaderReadOnlyOptimal;
	}
	command.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
	                        vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 0, nullptr,
	                        Placeholders, to_transfer.data());
	// Transparent black by default. KYTY_BINDLESS_DEBUG_COLORS=1: slot 0 grey (not resident), slot
	// 1 red (key outside its heap), slot 2 blue (pending), to see which case a surface is in.
	const bool debug_colors = std::getenv("KYTY_BINDLESS_DEBUG_COLORS") != nullptr;
	const auto color        = [debug_colors](float r, float g, float b) {
		return debug_colors ? vk::ClearColorValue {std::array<float, 4> {r, g, b, 1.0f}}
		                           : vk::ClearColorValue {std::array<float, 4> {0.0f, 0.0f, 0.0f, 0.0f}};
	};
	const std::array<vk::ClearColorValue, PlaceholderColors> colors {
	    color(0.5f, 0.5f, 0.5f), color(0.8f, 0.1f, 0.1f), color(0.1f, 0.2f, 0.8f)};
	for (uint32_t i = 0; i < Placeholders; i++) {
		command.clearColorImage(m_placeholders[i].image, vk::ImageLayout::eTransferDstOptimal,
		                        &colors[i / ImageArrays], 1, &to_transfer[i].subresourceRange);
	}
	// translation[0], the entry of keys outside their heap, is the red slot.
	command.fillBuffer(m_translation->Handle(), 4, VK_WHOLE_SIZE, 0u);
	command.fillBuffer(m_translation->Handle(), 0, 4, 1u);
	command.fillBuffer(m_feedback->Handle(), 0, VK_WHOLE_SIZE, 0u);
	vk::BufferMemoryBarrier translation_ready {};
	translation_ready.srcAccessMask       = vk::AccessFlagBits::eTransferWrite;
	translation_ready.dstAccessMask       = vk::AccessFlagBits::eShaderRead;
	translation_ready.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	translation_ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	translation_ready.buffer              = m_translation->Handle();
	translation_ready.offset              = 0;
	translation_ready.size                = VK_WHOLE_SIZE;
	auto feedback_ready                   = translation_ready;
	feedback_ready.dstAccessMask          = vk::AccessFlagBits::eShaderRead |
	                                        vk::AccessFlagBits::eShaderWrite |
	                                        vk::AccessFlagBits::eHostRead;
	feedback_ready.buffer                 = m_feedback->Handle();
	const std::array<vk::BufferMemoryBarrier, 2> buffers_ready {translation_ready, feedback_ready};
	command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                        vk::PipelineStageFlagBits::eAllCommands |
	                            vk::PipelineStageFlagBits::eHost,
	                        {}, 0, nullptr, static_cast<uint32_t>(buffers_ready.size()),
	                        buffers_ready.data(), Placeholders, to_read.data());
	RequireVulkanSuccess(command.end(), "end bindless init command buffer");

	vk::FenceCreateInfo fence_info {};
	vk::Fence           fence = nullptr;
	RequireVulkanSuccess(m_graphics.device.createFence(&fence_info, nullptr, &fence),
	                     "create bindless init fence");
	vk::SubmitInfo submit {};
	submit.commandBufferCount = 1;
	submit.pCommandBuffers    = &command;
	{
		Common::LockGuard lock(m_graphics.queue_mutex);
		RequireVulkanSuccess(m_graphics.queue.submit(1, &submit, fence), "submit bindless init");
	}
	RequireVulkanSuccess(m_graphics.device.waitForFences(1, &fence, VK_TRUE, UINT64_MAX),
	                     "wait for bindless init");
	m_graphics.device.destroyFence(fence, nullptr);
	m_graphics.device.destroyCommandPool(pool, nullptr);

	std::array<vk::DescriptorImageInfo, Placeholders> infos {};
	std::array<vk::WriteDescriptorSet, Placeholders> writes {};
	for (uint32_t i = 0; i < Placeholders; i++) {
		infos[i]                  = {nullptr, m_placeholder_views[i],
		                             vk::ImageLayout::eShaderReadOnlyOptimal};
		writes[i].dstSet          = m_set;
		writes[i].dstBinding      = Images2D + i % ImageArrays;
		writes[i].dstArrayElement = i / ImageArrays;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType  = vk::DescriptorType::eSampledImage;
		writes[i].pImageInfo      = &infos[i];
	}
	m_graphics.device.updateDescriptorSets(Placeholders, writes.data(), 0, nullptr);
}

BindlessTable::~BindlessTable() {
	UnwatchRange(0, UINT64_MAX);
	m_graphics.bindless_layout = nullptr;
	m_graphics.bindless_set = nullptr;
	for (uint32_t i = 0; i < Placeholders; i++) {
		if (m_placeholder_views[i] != nullptr) {
			m_graphics.device.destroyImageView(m_placeholder_views[i], nullptr);
		}
		if (m_placeholders[i].image != nullptr) {
			m_graphics.DeleteImage(m_placeholders[i]);
		}
	}
	m_translation.reset();
	m_feedback.reset();
	m_feedback_snapshot.reset();
	if (m_pool != nullptr) {
		m_graphics.device.destroyDescriptorPool(m_pool, nullptr);
	}
	if (m_layout != nullptr) {
		m_graphics.device.destroyDescriptorSetLayout(m_layout, nullptr);
	}
}

bool BindlessTable::AllocateRegion(Heap& heap, uint32_t entries) {
	// Headroom so a growing heap rarely moves; a move re-writes the keys resolved so far.
	const auto capacity = std::max(entries, std::min<uint32_t>(entries * 2u, 1u << 16u));
	if (capacity > TranslationEntries - m_next_region) {
		return false;
	}
	const auto old_region  = heap.region;
	const auto old_entries = heap.entries;
	heap.region            = m_next_region;
	heap.entries           = capacity;
	m_next_region += capacity;
	heap.slots.resize(capacity, 0u);
	heap.settled.resize(capacity, 0u);
	heap.images.resize(capacity);
	std::vector<uint32_t> translation(capacity, ShaderRecompiler::IR::BindlessPending);
	for (uint32_t key = 0; key < capacity; key++) {
		if (key < old_entries && heap.settled[key] != 0) {
			translation[key] = heap.slots[key];
		}
	}
	m_scheduler.EndRendering();
	auto command = m_scheduler.Current().Handle();
	// New regions are never reused. vkCmdUpdateBuffer copies at most 64 KiB per
	// call and avoids noncoherent host flushes touching a live adjacent region.
	for (uint32_t first = 0; first < capacity;) {
		const auto count = std::min(16384u, capacity - first);
		command.updateBuffer(m_translation->Handle(), uint64_t {heap.region + first} * 4,
		                     uint64_t {count} * 4, translation.data() + first);
		first += count;
	}
	vk::BufferMemoryBarrier ready {};
	ready.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	ready.dstAccessMask = vk::AccessFlagBits::eShaderRead;
	ready.srcQueueFamilyIndex = ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	ready.buffer = m_translation->Handle();
	ready.offset = uint64_t {heap.region} * 4;
	ready.size = uint64_t {capacity} * 4;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands,
	                        {}, 0, nullptr, 1, &ready, 0, nullptr);
	LOGF("Bindless heap region: base=0x%016" PRIx64 " binding=%u region=%u entries=%u (was %u at "
	     "%u)\n",
	     heap.base, heap.binding, heap.region, capacity, old_entries, old_region);
	return true;
}

BindlessTable::Heap* BindlessTable::FindOrCreateHeap(
    uint64_t base, uint32_t table_offset, uint32_t binding, uint32_t entries,
    const ShaderRecompiler::IR::ImageResource& resource) {
	if (entries == 0) {
		return nullptr;
	}
	for (auto& heap: m_heaps) {
		if (heap.base == base && heap.table_offset == table_offset && heap.binding == binding) {
			if (entries > heap.entries && !AllocateRegion(heap, entries)) {
				return nullptr;
			}
			// After an unmap the same VA may hold a smaller allocation. Do not
			// rearm a watch over the former allocation's now-unmapped tail.
			heap.guest_entries = heap.watch_generation != m_watch_generation.load(std::memory_order_acquire)
			                         ? entries : std::max(heap.guest_entries, entries);
			return &heap;
		}
	}
	auto& heap         = m_heaps.emplace_back();
	heap.base          = base;
	heap.table_offset  = table_offset;
	heap.binding       = binding;
	heap.resource      = resource;
	// A heap serves multiple shaders; a layer bound proven for one sample must
	// not truncate the view used by another shader sampling the same key.
	heap.resource.max_array_index = ShaderRecompiler::IR::ImageResource::NoArrayIndexBound;
	heap.guest_entries = entries;
	if (!AllocateRegion(heap, std::max(entries, 1u << 14u))) {
		m_heaps.pop_back();
		return nullptr;
	}
	return &heap;
}

uint32_t BindlessTable::AllocateSlot(uint32_t binding) {
	if (binding >= ImageArrays) {
		return 0;
	}
	// Keys are settled again whenever the guest rewrites a heap entry or a texture is evicted,
	// and each settle took a new slot: the array would fill up over a long session.
	if (auto& free = m_free_slots[binding];
	    !free.empty() && m_scheduler.IsFree(free.front().second)) {
		const auto slot = free.front().first;
		free.pop_front();
		return slot;
	}
	if (m_next_slot[binding] >= m_images_per_array) {
		return 0;
	}
	return m_next_slot[binding]++;
}

void BindlessTable::ReleaseSlot(uint32_t binding, uint32_t slot) {
	static const bool reuse = [] {
		const char* value = std::getenv("KYTY_BINDLESS_SLOT_REUSE");
		return value == nullptr || value[0] != '0';
	}();
	if (reuse && binding < ImageArrays && slot >= PlaceholderColors) {
		m_free_slots[binding].emplace_back(slot, m_scheduler.CurrentTick());
	}
}

void BindlessTable::WriteSlot(uint32_t binding, uint32_t slot, vk::ImageView view,
                              vk::ImageLayout layout) {
	const vk::DescriptorImageInfo info {nullptr, view, layout};
	vk::WriteDescriptorSet        write {};
	write.dstSet          = m_set;
	write.dstBinding      = binding;
	write.dstArrayElement = slot;
	write.descriptorCount = 1;
	write.descriptorType  = vk::DescriptorType::eSampledImage;
	write.pImageInfo      = &info;
	m_graphics.device.updateDescriptorSets(1, &write, 0, nullptr);
	if (binding >= ImageArrays) {
		return;
	}
	auto& views = m_slot_views[binding];
	if (views.size() <= slot) {
		views.resize(slot + 1u);
	}
	if (const auto old = views[slot]; old != nullptr) {
		if (const auto found = m_view_slots.find(static_cast<VkImageView>(old));
		    found != m_view_slots.end()) {
			std::erase(found->second, std::pair {binding, slot});
			if (found->second.empty()) {
				m_view_slots.erase(found);
			}
		}
	}
	const bool placeholder =
	    std::ranges::find(m_placeholder_views, view) != m_placeholder_views.end();
	views[slot] = placeholder ? vk::ImageView {} : view;
	if (!placeholder && view != nullptr) {
		m_view_slots[static_cast<VkImageView>(view)].emplace_back(binding, slot);
	}
}

bool BindlessTable::ReleaseViews(std::span<const vk::ImageView> views) {
	bool held = false;
	for (const auto view: views) {
		const auto found = m_view_slots.find(static_cast<VkImageView>(view));
		if (found == m_view_slots.end()) {
			continue;
		}
		const auto slots = found->second;
		for (const auto& [binding, slot]: slots) {
			held = true;
			for (auto& heap: m_heaps) {
				for (uint32_t key = 0; key < heap.slots.size(); key++) {
					if (heap.slots[key] == 0 || SlotBinding(heap, heap.slots[key]) != binding ||
					    SlotIndex(heap.slots[key]) != slot) {
						continue;
					}
					if (const auto id = heap.images[key]; id) {
						if (const auto refs = m_image_refs.find(ImageKey(id)); refs != m_image_refs.end()) {
							std::erase(refs->second, std::pair<Heap*, uint32_t> {&heap, key});
							if (refs->second.empty()) {
								m_image_refs.erase(refs);
							}
						}
						std::erase(heap.resolved, id);
					}
					ReleaseSlot(binding, slot);
					heap.slots[key]   = 0;
					heap.settled[key] = 0;
					heap.images[key]  = {};
					SetTranslation(heap, key, ShaderRecompiler::IR::BindlessPending);
				}
			}
			WriteSlot(binding, slot, PlaceholderView(binding),
			          vk::ImageLayout::eShaderReadOnlyOptimal);
			static std::atomic<uint32_t> reported = 0;
			if (reported.fetch_add(1) < 16) {
				LOGF("Bindless: binding %u slot %u still held an image being destroyed; "
				     "repointed to the placeholder\n",
				     binding, slot);
			}
		}
	}
	return held;
}

void BindlessTable::SetTranslation(const Heap& heap, uint32_t key, uint32_t slot) {
	// Existing entries can still be read by in-flight shaders. Unlike Frank's
	// mapped host store, an ordered GPU update preserves the older draws' mapping.
	m_scheduler.EndRendering();
	auto command = m_scheduler.Current().Handle();
	vk::BufferMemoryBarrier barrier {};
	barrier.srcAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eTransferWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
	barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer = m_translation->Handle();
	barrier.offset = uint64_t {heap.region + key} * sizeof(uint32_t);
	barrier.size = sizeof(uint32_t);
	command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer,
	                        {}, 0, nullptr, 1, &barrier, 0, nullptr);
	command.updateBuffer(m_translation->Handle(), barrier.offset, sizeof(slot), &slot);
	barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands,
	                        {}, 0, nullptr, 1, &barrier, 0, nullptr);
}

void BindlessTable::RecordFeedbackSnapshot(CommandScheduler& scheduler) {
	// Regions are allocated contiguously after word 0, so one copy covers every heap.
	const vk::DeviceSize size = vk::DeviceSize {m_next_region} *
	                            sizeof(uint32_t);
	scheduler.EndRendering();
	auto                    command = scheduler.Current().Handle();
	vk::BufferMemoryBarrier flags_written {};
	flags_written.srcAccessMask       = vk::AccessFlagBits::eShaderWrite;
	flags_written.dstAccessMask       = vk::AccessFlagBits::eTransferRead;
	flags_written.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	flags_written.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	flags_written.buffer              = m_feedback->Handle();
	flags_written.offset              = 0;
	flags_written.size                = size;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                        vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 1,
	                        &flags_written, 0, nullptr);
	const vk::BufferCopy copy {0, 0, size};
	command.copyBuffer(m_feedback->Handle(), m_feedback_snapshot->Handle(), 1, &copy);
	// Clear in the same command stream. A host clear after reading the snapshot
	// races the requests of later submissions.
	auto clear = flags_written;
	clear.srcAccessMask = vk::AccessFlagBits::eTransferRead;
	clear.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
	                        {}, 0, nullptr, 1, &clear, 0, nullptr);
	command.fillBuffer(m_feedback->Handle(), 0, size, 0);
	clear.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	clear.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands,
	                        {}, 0, nullptr, 1, &clear, 0, nullptr);
	auto snapshot_written          = flags_written;
	snapshot_written.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	snapshot_written.dstAccessMask = vk::AccessFlagBits::eHostRead;
	snapshot_written.buffer        = m_feedback_snapshot->Handle();
	command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                        vk::PipelineStageFlagBits::eHost, {}, 0, nullptr, 1,
	                        &snapshot_written, 0, nullptr);
	m_snapshot_tick = scheduler.CurrentTick();
}

bool BindlessTable::SnapshotReady(CommandScheduler& scheduler) {
	return m_snapshot_tick != 0 && scheduler.IsFree(m_snapshot_tick);
}

void BindlessTable::TakeRequests(const Heap& heap, std::vector<uint32_t>& keys) {
	const auto* snapshot = reinterpret_cast<const uint32_t*>(m_feedback_snapshot->Mapped().data());
	m_feedback_snapshot->Invalidate(heap.region * sizeof(uint32_t),
	                                heap.entries * sizeof(uint32_t));
	for (uint32_t key = 0; key < heap.entries; key++) {
		if (snapshot[heap.region + key] == 1u) {
			keys.push_back(key);
		}
	}
}

void BindlessTable::AddImageReference(ImageId id, Heap& heap, uint32_t key) {
	m_image_refs[ImageKey(id)].emplace_back(&heap, key);
}

bool BindlessTable::ReleaseKey(Heap& heap, uint32_t key) {
	const auto id      = heap.images[key];
	bool       no_refs = false;
	if (id) {
		const auto found = m_image_refs.find(ImageKey(id));
		if (found != m_image_refs.end()) {
			std::erase(found->second, std::pair<Heap*, uint32_t> {&heap, key});
			const bool in_heap = std::any_of(found->second.begin(), found->second.end(),
			                                 [&](const auto& ref) { return ref.first == &heap; });
			if (!in_heap) {
				std::erase(heap.resolved, id);
			}
			if (found->second.empty()) {
				m_image_refs.erase(found);
				no_refs = true;
			}
		}
	}
	if (heap.slots[key] != 0) {
		ReleaseSlot(SlotBinding(heap, heap.slots[key]), SlotIndex(heap.slots[key]));
	}
	heap.slots[key]   = 0;
	heap.settled[key] = 0;
	heap.images[key]  = {};
	SetTranslation(heap, key, ShaderRecompiler::IR::BindlessPending);
	return no_refs;
}

void BindlessTable::OnImageUnregistered(ImageId id) {
	const auto found = m_image_refs.find(ImageKey(id));
	if (found == m_image_refs.end()) {
		return;
	}
	for (const auto& [heap, key]: found->second) {
		if (heap->slots[key] != 0) {
			// Keep the old descriptor/view until its retire tick. The ordered
			// translation update makes later draws request the key again.
			const auto binding = SlotBinding(*heap, heap->slots[key]);
			const auto slot    = SlotIndex(heap->slots[key]);
			ReleaseSlot(binding, slot);
		}
		heap->slots[key]   = 0;
		heap->settled[key] = 0;
		heap->images[key]  = {};
		SetTranslation(*heap, key, ShaderRecompiler::IR::BindlessPending);
		std::erase(heap->resolved, id);
		m_unregistered_keys++;
	}
	m_image_refs.erase(found);
}

} // namespace Libs::Graphics
