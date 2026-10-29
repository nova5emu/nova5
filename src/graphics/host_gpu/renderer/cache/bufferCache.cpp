#include "common/gpuThreadSampler.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/debugCounters.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/readbackProfile.h"
#include "common/renderStageProfile.h"
#include "common/submitProfile.h"
#include "graphics/guest_gpu/submitProfileRuntime.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "kernel/memory.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>
#include <xxhash.h>

namespace Libs::Graphics {

namespace {

constexpr uint64_t MiB           = 1024 * 1024;
constexpr uint64_t GdsBufferSize = 64 * 1024;

// Lifecycle operations are rare. Hot filters are timed only once per 1024
// calls, with local call counts flushed per frame (no atomic per lookup).
class ImportTimer {
public:
	explicit ImportTimer(Common::SubmitProfile::Extra ns, bool enabled = true)
	    : m_ns(ns), m_start(enabled && Common::SubmitProfile::Recording() ? Common::SubmitProfile::NowNs() : 0) {}
	~ImportTimer() { if (m_start) Common::SubmitProfile::AddExtra(m_ns, Common::SubmitProfile::NowNs() - m_start); }
private:
	Common::SubmitProfile::Extra m_ns;
	uint64_t m_start;
};

bool SampleImportWork(uint64_t& calls, Common::SubmitProfile::Extra samples) {
	if (!Common::SubmitProfile::Recording() || (++calls & 1023u) != 0) return false;
	Common::SubmitProfile::AddExtra(samples);
	return true;
}

std::pair<Buffer*, uint64_t> UploadMemory(GraphicContext& graphics, CommandScheduler& scheduler,
                                        StreamBuffer& staging, uint64_t size, uint64_t alignment,
                                        vk::BufferUsageFlags usage, const auto& write) {
	const auto [mapped, offset] = staging.Map(size, alignment);
	if (mapped != nullptr) {
		write(mapped, offset);
		staging.Commit();
		return {&staging, offset};
	}

	// Large transfers use the same one-time host allocation as ordinary buffer uploads.
	auto temporary = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::Upload, 0,
	                                         usage, size);
	write(temporary->Mapped().data(), 0);
	temporary->Flush(0, size);
	auto* buffer = temporary.get();
	scheduler.DeferOperation([owner = std::move(temporary)]() mutable { owner.reset(); });
	return {buffer, 0};
}

} // namespace

void BufferCache::WriteDataBuffer(Buffer& buffer, uint64_t address, const void* source,
                                  uint64_t size) {
	auto* bytes = static_cast<const uint8_t*>(source);
	while (size != 0) {
		const auto chunk  = std::min(size, m_staging_buffer.Size());
		// Copy waits for ring reuse and flushes the fresh host data before submission.
		const auto offset = m_staging_buffer.Copy(bytes, chunk, 4);
		const auto destination_offset = buffer.Offset(address);
		EXIT_IF(destination_offset > buffer.Size() || chunk > buffer.Size() - destination_offset);
		m_scheduler.EndRendering();
		const auto command = m_scheduler.Current().Commands();
		vk::BufferMemoryBarrier2 before {};
		before.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
		before.srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
		before.dstStageMask  = vk::PipelineStageFlagBits2::eTransfer;
		before.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
		before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.buffer        = buffer.Handle();
		before.offset        = destination_offset;
		before.size          = chunk;
		vk::DependencyInfo dependency {};
		dependency.dependencyFlags          = vk::DependencyFlagBits::eByRegion;
		dependency.bufferMemoryBarrierCount = 1;
		dependency.pBufferMemoryBarriers    = &before;
		Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::Barrier);
		command.pipelineBarrier2(dependency);
		const vk::BufferCopy copy {offset, destination_offset, chunk};
		Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::Copy);
		command.copyBuffer(m_staging_buffer.Handle(), buffer.Handle(), 1, &copy);
		auto after          = before;
		after.srcStageMask  = vk::PipelineStageFlagBits2::eTransfer;
		after.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
		after.dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
		after.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
		dependency.pBufferMemoryBarriers = &after;
		Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::Barrier);
		command.pipelineBarrier2(dependency);
		bytes += chunk;
		address += chunk;
		size -= chunk;
	}
}

void BufferCache::Register(BufferId id) {
	ChangeRegister<true>(id);
}

void BufferCache::Unregister(BufferId id) {
	if (m_coarsen_selected) ForgetCoarsening(m_slot_buffers[id].CpuAddress(), m_slot_buffers[id].Size());
	if (m_hot_gate) {
		const auto& buffer = m_slot_buffers[id];
		const auto it = m_hot_owners.find(buffer.CpuAddress());
		if (it != m_hot_owners.end()) {
			const bool was_hot = it->second.hot;
			RetireHot(it->first, it->second, Common::SubmitProfile::Extra::HotRetireMerge);
			if (was_hot) m_memory_tracker.UntrackMemory(it->first, it->second.size);
			m_memory_tracker.TakeHotFaults(it->first, it->second.size, false);
			m_hot_owners.erase(it);
		}
	}
	ChangeRegister<false>(id);
}

template <bool insert>
void BufferCache::ChangeRegister(BufferId id) {
	auto& buffer = m_slot_buffers[id];
	PageTable::PageRange pages {};
	EXIT_IF(!(GuestRange {buffer.CpuAddress(), buffer.Size()}.Valid()) ||
	        !PageTable::TryGetPageRange(buffer.CpuAddress(), buffer.Size(), pages));
	for (size_t page = pages.first; page < pages.last_exclusive; ++page) {
		if constexpr (insert) {
			m_page_table[page] = id;
		} else {
			m_page_table[page] = {};
		}
	}
	const auto size_pages = pages.last_exclusive - pages.first;
	const auto table_offset = PageIndex(buffer.CpuAddress()) * sizeof(vk::DeviceAddress);
	if constexpr (insert) {
		const auto [it, inserted] = m_buffers.emplace(buffer.CpuAddress(), id);
		(void)it;
		EXIT_IF(!inserted);
		m_total_used_memory += buffer.Size();
		buffer.lru_id = m_lru_cache.Insert(id, m_gc_tick);
		std::vector<vk::DeviceAddress> addresses;
		addresses.reserve(size_pages);
		for (uint64_t i = 0; i < size_pages; ++i) {
			addresses.push_back(buffer.BufferDeviceAddress() + (i << CACHING_PAGEBITS));
		}
		WriteDataBuffer(m_bda_pagetable_buffer, table_offset,
		                addresses.data(), addresses.size() * sizeof(vk::DeviceAddress));
		QueueBdaSync(buffer.CpuAddress(), buffer.Size());
	} else {
		const auto found = m_buffers.find(buffer.CpuAddress());
		EXIT_IF(found == m_buffers.end() || found->second != id);
		m_buffers.erase(found);
		EXIT_IF(buffer.Size() > m_total_used_memory);
		m_total_used_memory -= buffer.Size();
		m_lru_cache.Free(buffer.lru_id);
		m_bda_pagetable_buffer.Fill(table_offset,
		                            size_pages * sizeof(vk::DeviceAddress), 0);
		buffer.is_deleted = true;
	}
}

void BufferCache::TouchBuffer(const Buffer& buffer) {
	if (!buffer.is_deleted) {
		m_lru_cache.Touch(buffer.lru_id, m_gc_tick);
	}
}

void BufferCache::DeleteBuffer(BufferId id) {
	if (IsBufferInvalid(id)) {
		return;
	}
	Unregister(id);
	if (m_scheduler.Active()) {
		m_scheduler.DeferOperation([this, id] { m_slot_buffers.erase(id); });
	} else {
		m_slot_buffers.erase(id);
	}
}

template <bool async>
bool BufferCache::DownloadBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size) {
	ExcludeHotRange(vaddr, size, Common::SubmitProfile::Extra::HotRetireReadback);
	struct PendingDownload {
		std::atomic<uint32_t>& count;
		bool handed_off = false;
		explicit PendingDownload(std::atomic<uint32_t>& c): count(c) {
			count.fetch_add(1, std::memory_order_acq_rel);
		}
		~PendingDownload() {
			if (!handed_off) count.fetch_sub(1, std::memory_order_release);
		}
	} pending(m_unpublished_downloads);
	std::vector<vk::BufferCopy> copies;
	uint64_t                    total_size     = 0;
	const auto                  buffer_address = buffer.CpuAddress();
	m_memory_tracker.ForEachDownloadRange<false>(
	    vaddr, size, [&](uint64_t address, uint64_t bytes) noexcept {
		    m_memory_tracker.ValidateGpuDirtyPages(m_gpu_modified_ranges, address, bytes,
		                                           "buffer download");
		    m_gpu_modified_ranges.ForEachInRange(address, bytes, [&](uint64_t start, uint64_t end) {
			    if constexpr (!async) {
				    Common::SubmitProfile::Diagnostics::ReadFaults::Download(start, end);
				    Common::ReadbackProfile::RecordDownload(start, end);
			    }
			    copies.emplace_back(start - buffer_address, total_size, end - start);
			    // Keep packed ranges on separate cache lines, as in shadPS4.
			    total_size += Common::AlignUp(end - start, 64);
		    });
		    m_gpu_modified_ranges.Subtract(address, bytes);
		    ForgetColorMetadataChecks(address, bytes);
		    if (!m_clean_read_pages.empty()) {
			    for (auto page = address & ~uint64_t {4095}; page < address + bytes; page += 4096)
				    m_clean_read_pages.erase(page);
		    }
	    });
	if constexpr (!async) {
		// Payload bytes only: total_size also counts the 64-byte packing of each range.
		uint64_t copied = 0;
		for (const auto& copy: copies) copied += copy.size;
		Common::SubmitProfile::RecordReadbackBytes(0, copied);
	}
	if (copies.empty()) {
		return false;
	}

	Common::GpuThreadSampler::ImmediateScope boundary_immediate {Common::GpuThreadSampler::Boundary::Readback};
	Common::DebugCounters::Add(Common::DebugCounters::Counter::BufferDownloadBytes, total_size);

	auto [mapped, offset] = m_download_buffer.Map(total_size, 64);
	std::unique_ptr<Buffer> temporary;
	if (mapped == nullptr) {
		temporary = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::Download, 0,
		                                     vk::BufferUsageFlagBits::eTransferDst, total_size);
		mapped = temporary->Mapped().data();
	} else {
		m_download_buffer.Commit();
	}
	const auto& download = temporary ? *temporary : m_download_buffer;
	for (auto& copy: copies) {
		copy.dstOffset += offset;
	}

	auto& command = m_scheduler.Current();
	command.EndRendering();
	const auto              native = command.Commands();
	vk::BufferMemoryBarrier before {};
	before.srcAccessMask       = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	before.dstAccessMask       = vk::AccessFlagBits::eTransferRead;
	before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.buffer              = buffer.Handle();
	before.offset              = 0;
	before.size                = buffer.Size();
	Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::Barrier);
	native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                       vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 1, &before, 0,
	                       nullptr);
	Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::Copy);
	native.copyBuffer(buffer.Handle(), download.Handle(),
	                  static_cast<uint32_t>(copies.size()), copies.data());

	auto after          = before;
	after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	after.dstAccessMask = vk::AccessFlagBits::eHostRead;
	after.buffer        = download.Handle();
	after.offset        = offset;
	after.size          = total_size;
	Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::Barrier);
	native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                       vk::PipelineStageFlagBits::eAllCommands |
	                           vk::PipelineStageFlagBits::eHost,
	                       {}, 0, nullptr, 1, &after, 0, nullptr);
	auto publish = [this, mapped, offset, total_size, buffer_address,
	                copies = std::move(copies), owner = std::move(temporary)] {
		(owner ? *owner : m_download_buffer).Invalidate(offset, total_size);
		for (const auto& copy: copies) {
			Libs::LibKernel::Memory::WriteBacking(buffer_address + copy.srcOffset,
			                                      mapped + (copy.dstOffset - offset), copy.size);
		}
		m_unpublished_downloads.fetch_sub(1, std::memory_order_release);
	};
	pending.handed_off = true;
	if constexpr (async) {
		m_scheduler.DeferPriorityOperation(std::move(publish));
	} else {
		const auto tick = m_scheduler.CurrentTick();
		// The wait is attributed to the caller's site (ReadMemory carries it).
		m_scheduler.Wait(tick);
		{
			Common::SubmitProfile::WaitSiteScope site(Common::SubmitProfile::WaitSite::ReadbackPriorityOps);
			Common::SubmitProfile::WaitTimer     timer(Common::SubmitProfile::Recording() ? SubmitProfileRuntime::ThreadId() : 0u);
			m_scheduler.WaitPriorityOperations(tick);
		}
		publish();
	}
	return true;
}

// Keep the asynchronous entry available to the friend GPU test as well as GC,
// including builds where every GC call is inlined.
template bool BufferCache::DownloadBufferMemory<true>(Buffer&, uint64_t, uint64_t);

BufferCache::BufferCache(GraphicContext& graphics, CommandScheduler& scheduler,
                         PageManager& page_manager, TextureCache& texture_cache)
    : m_graphics(graphics), m_scheduler(scheduler), m_fault_manager(graphics, scheduler, *this),
      m_gds_buffer(graphics, scheduler, MemoryUsage::Stream, 0, AllFlags, GdsBufferSize),
      m_bda_pagetable_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags,
                             BDA_PAGETABLE_SIZE),
      m_memory_tracker(page_manager),
      m_staging_buffer(graphics, scheduler, MemoryUsage::Upload, 512 * MiB),
      m_stream_buffer(graphics, scheduler, MemoryUsage::Stream, 64 * MiB),
      m_download_buffer(graphics, scheduler, MemoryUsage::Download, 64 * MiB),
      m_device_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 128 * MiB),
      m_texture_cache(texture_cache) {
	if (Common::BufferUploadProfile::Enabled()) {
		m_buffer_upload_profile = std::make_unique<Common::BufferUploadProfile::State>();
		if (!m_buffer_upload_profile->Ready()) m_buffer_upload_profile.reset();
	}
	m_import_selected = Common::SubmitProfile::ExperimentSelected(Common::SubmitProfile::Experiment::GuestHostImport);
	const auto* guard = std::getenv("KYTY_HOST_IMPORT_GUARD");
	m_import_guard_enabled = !guard || std::strcmp(guard, "0") != 0;
	const auto* mode = std::getenv("KYTY_HOST_IMPORT_MODE");
	m_import_copy_mode = !mode || std::strcmp(mode, "direct") != 0;
	// Retired runtime experiment. Keep the implementation only for its internal
	// coherency regression tests; selecting its old name cannot unprotect memory.
	if (Common::SubmitProfile::ExperimentSelected(Common::SubmitProfile::Experiment::HotUnprotected))
		std::fprintf(stderr, "[HOT_UNPROTECTED] RETIRED: inert; use fault-coarsening for the new candidate\n");
	m_coarsen_selected = Common::SubmitProfile::ExperimentSelected(Common::SubmitProfile::Experiment::FaultCoarsening);
	if (m_coarsen_selected) {
		if (const auto* kb = std::getenv("KYTY_FAULT_COARSEN_KB")) {
			if (std::strcmp(kb, "64") == 0) m_coarsen_block = 64 * 1024;
			else if (std::strcmp(kb, "256") == 0) m_coarsen_block = 256 * 1024;
			else if (std::strcmp(kb, "1024") == 0) m_coarsen_block = 1024 * 1024;
			else std::fprintf(stderr, "[FAULT_COARSEN] invalid KYTY_FAULT_COARSEN_KB; using 64\n");
		}
		const auto* detail = std::getenv("KYTY_FAULT_COARSEN_PROFILE");
		m_coarsen_profile = detail && std::strcmp(detail, "1") == 0;
		m_coarsen_counters = m_coarsen_profile || Common::SubmitProfile::Armed();
		std::fprintf(stderr, "[FAULT_COARSEN] block_bytes=%llu history=3_of_12 busy_faults=256 owner_min=1048576 owner_max=67108864 reprobe_frames=120\n",
		    static_cast<unsigned long long>(m_coarsen_block));
	}
	if (m_hot_selected) {
		if (m_import_selected && !m_import_copy_mode) {
			std::fprintf(stderr, "[HOT_UNPROTECTED] INERT: direct host import is incompatible; use copy mode\n");
			m_hot_selected = false;
		} else {
			const auto* profile = std::getenv("KYTY_HOT_BUFFER_PROFILE");
			m_hot_frame_profile = profile && std::strcmp(profile, "1") == 0;
			if (const auto* value = std::getenv("KYTY_HOT_BUFFER_MAX_MIB")) {
				char* end = nullptr; const auto n = std::strtoull(value, &end, 10);
				if (end != value && !*end && n >= 16 && n <= 4096) m_hot_budget = n * MiB;
			}
			std::fprintf(stderr, "[HOT_UNPROTECTED] history=3_of_12 busy_faults=256 owner_min=1048576 owner_max=67108864 reprobe_frames=120 max_resync_bytes_per_frame=%llu\n",
			    static_cast<unsigned long long>(m_hot_budget));
		}
	}
	if (m_import_selected) {
		if (mode && std::strcmp(mode, "copy") != 0 && std::strcmp(mode, "direct") != 0)
			std::fprintf(stderr, "[GUEST_HOST_IMPORT] unknown mode=%s; using copy\n", mode);
		std::fprintf(stderr, "[GUEST_HOST_IMPORT] mode=%s guard=%s\n", m_import_copy_mode ? "copy" : "direct",
		    m_import_copy_mode ? "none_dma_lifetime_only" : (m_import_guard_enabled ? "range" : "off"));
	}
	m_import_armed = m_graphics.guest_host_import_enabled && m_import_selected;
	if (m_import_selected) std::fprintf(stderr, "[GUEST_HOST_IMPORT] extension=%u probe=%s alignment=%llu%s\n",
	    unsigned(m_import_armed), m_import_armed ? "deferred_until_B" : "unavailable",
	    static_cast<unsigned long long>(m_graphics.host_import_alignment), m_import_armed ? "" : " INERT: extension unavailable");
	if (m_import_armed) {
		std::lock_guard lock(Common::GuestHostImport::mutex);
		EXIT_IF(Common::GuestHostImport::owner != nullptr);
		Common::GuestHostImport::owner = this;
		Common::GuestHostImport::drain = [](void* cache) { static_cast<BufferCache*>(cache)->DrainHostImports(); };
		Common::GuestHostImport::armed.store(true);
	}
	std::memset(m_gds_buffer.Mapped().data(), 0, static_cast<size_t>(m_gds_buffer.Size()));
	m_gds_buffer.Flush(0, m_gds_buffer.Size());
	SetVulkanObjectNameF(m_graphics.device, m_bda_pagetable_buffer.Handle(),
	                     "BDA Page Table Buffer");
	m_scheduler.BeginCommand();
	for (auto* buffer: {&m_bda_pagetable_buffer, m_fault_manager.GetFaultBuffer()}) {
		buffer->Fill(0, buffer->Size(), 0);
	}
	const auto null_id =
	    m_slot_buffers.insert(m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0, AllFlags, 16);
	EXIT_IF(null_id != NULL_BUFFER_ID);
	SetVulkanObjectNameF(m_graphics.device, GetBuffer(null_id).Handle(), "Kyty.NullBuffer");
	if (!m_graphics.CanReportMemoryUsage()) {
		return;
	}
	constexpr int64_t GiB              = 1024ll * 1024 * 1024;
	constexpr int64_t target_threshold = 8 * GiB;
	const auto        budget =
	    static_cast<int64_t>(std::min<uint64_t>(m_graphics.GetTotalMemoryBudget(), INT64_MAX));
	const auto threshold = std::min(budget, target_threshold);
	const auto expected  = std::min(budget - 6 * threshold / 10, budget - GiB);
	const auto critical  = std::min(budget - 2 * threshold / 10, budget - GiB / 2);
	m_trigger_gc_memory  = static_cast<uint64_t>(std::max<int64_t>(expected, GiB));
	m_critical_gc_memory = static_cast<uint64_t>(std::max<int64_t>(critical, 2 * GiB));
}

BufferCache::~BufferCache() {
	for (auto& [base, owner]: m_hot_owners) {
		// The GPU/guest lanes have stopped. Release only our watcher contribution.
		if (owner.hot) {
			m_memory_tracker.SetHot(base, owner.size, false);
			m_memory_tracker.UntrackMemory(base, owner.size);
		}
	}
	if (m_hot_selected) { Common::SubmitProfile::hot_owner_count = 0; Common::SubmitProfile::hot_owner_bytes = 0; }
	EXIT_IF(!m_host_imports.empty()); // ShutdownGpu drains before the scheduler closes.
	if (m_import_selected) {
		std::lock_guard lock(Common::GuestHostImport::mutex);
		if (Common::GuestHostImport::owner == this) {
			Common::GuestHostImport::armed.store(false);
			Common::GuestHostImport::drain = nullptr; Common::GuestHostImport::owner = nullptr;
		}
	}
	if (!m_gpu_modified_ranges.Empty()) {
		EXIT("BufferCache: destroyed with pending GPU-modified ranges\n");
	}
	for (const auto& [vaddr, id]: m_buffers) {
		(void)vaddr;
		const auto& buffer = m_slot_buffers[id];
		if (m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size())) {
			EXIT("BufferCache: destroyed with GPU-modified buffer\n");
		}
	}
	m_buffers.clear();
}

void BufferCache::InvalidateMemory(uint64_t vaddr, uint64_t size, Common::BufferUploadProfile::Cause cause) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid memory-invalidation range\n");
	}
	uint64_t sync_address = vaddr, sync_size = size;
	m_memory_tracker.InvalidateRegion(vaddr, size,
	                                  [this, vaddr, size, cause] {
		                                  ReadMemory(vaddr, size, true,
		                                             Common::SubmitProfile::WaitSite::ReadbackCpuWriteInvalidate, cause);
	                                  }, cause, [this, cause](uint64_t address, uint64_t bytes) { InvalidateHostImport(address, bytes, cause); },
	    [&](uint64_t address, uint64_t bytes) {
		    // Expansion is allowed only for the single fault byte. Publish once,
		    // outside the tracker lock, with the full range now writable/dirty.
		    sync_address = address; sync_size = bytes;
	    }, !m_coarsen_selected || !HasUnpublishedDownloads());
	QueueBdaSync(sync_address, sync_size);
}

void BufferCache::ReadMemory(uint64_t vaddr, uint64_t size, bool is_write,
                             Common::SubmitProfile::WaitSite site, Common::BufferUploadProfile::Cause cause) {
	Common::GuestCpuProfile::Stage readback(Common::GuestCpuProfile::Part::ReadbackWait);
	if (site == Common::SubmitProfile::WaitSite::ReadbackCpuWriteInvalidate) {
		Common::SubmitProfile::Diagnostics::WriteReadbackRequest();
	}
	if (!GuestGpu::IsGpuThread() && CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported buffer readback from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	const auto read = [this, vaddr, size, is_write, site, cause] {
		Common::SubmitProfile::WaitSiteScope profile_site(site);
		Common::SubmitProfile::RecordReadbackBytes(size, 0); // Every request, including early returns.
		if (is_write && !IsRegionRegistered(vaddr, size)) {
			return;
		}
		auto& buffer = m_slot_buffers[FindBuffer(vaddr, size)];
		if (site == Common::SubmitProfile::WaitSite::ReadbackCpuWriteInvalidate && Common::ReadbackProfile::Enabled()) {
			Common::ReadbackProfile::origin.owner_address = buffer.CpuAddress();
			Common::ReadbackProfile::origin.owner_bytes = buffer.Size();
		}

		GuestRange download {vaddr, size};
		using Experiment = Common::SubmitProfile::Experiment;
		using Extra = Common::SubmitProfile::Extra;
		if (size == 1 &&
		    ((is_write && cause == Common::BufferUploadProfile::Cause::CpuWriteFault &&
		      site == Common::SubmitProfile::WaitSite::ReadbackCpuWriteInvalidate &&
		      Common::SubmitProfile::ReadbackAheadEnabled()) ||
		     (!is_write && site == Common::SubmitProfile::WaitSite::ReadbackCpuReadFault &&
		      Common::SubmitProfile::ReadbackAheadEnabled()))) {
			download = FaultReadbackRange(buffer, vaddr, is_write);
		}
		const bool downloaded = DownloadBufferMemory<false>(buffer, download.address, download.size);
		if (downloaded) {
			m_memory_tracker.UnmarkRegionAsGpuModified(download.address, download.size);
			if (download.size > size) {
				Common::SubmitProfile::AddExtra(is_write ? Extra::WriteAheadBatches : Extra::ReadAheadBatches);
				Common::SubmitProfile::AddExtra(is_write ? Extra::WriteAheadPages : Extra::ReadAheadPages, download.size / TRACKER_PAGE_SIZE);
				Common::SubmitProfile::AddExtra(is_write ? Extra::WriteAheadExtraBytes : Extra::ReadAheadExtraBytes, download.size - TRACKER_PAGE_SIZE);
			}
		}
		if (is_write) {
			// Only the fault's page becomes CPU-dirty/writable. Prefetched pages
			// retain write tracking; a subsequent GPU writer protects them again.
			m_memory_tracker.MarkRegionAsCpuModified(vaddr, size,
			    downloaded ? Common::BufferUploadProfile::AfterDownload(cause) : cause);
			QueueBdaSync(vaddr, size);
		}
	};
	if (site == Common::SubmitProfile::WaitSite::ReadbackCpuWriteInvalidate && Common::ReadbackProfile::Enabled()) {
		const auto request = Common::ReadbackProfile::WriteRequestOrigin(
		    Common::ReadbackProfile::origin, vaddr, size, static_cast<uint32_t>(cause));
		m_scheduler.Context().GetGpu().SendCommandSync([read, request] {
			Common::ReadbackProfile::OriginScope attribution(request);
			read();
		});
	} else {
		m_scheduler.Context().GetGpu().SendCommandSync(read);
	}
}

bool BufferCache::ConsumeCleanReadBudget(uint64_t page) {
	// Bounded: when full, pages take normal recovery; nothing is evicted, so a
	// repeatedly read page can never regain its budget without being downloaded.
	constexpr size_t capacity = 4096;
	if (m_clean_read_pages.contains(page) || m_clean_read_pages.size() >= capacity) return false;
	m_clean_read_pages.insert(page);
	return true;
}

GuestRange BufferCache::FaultReadbackRange(const Buffer& buffer, uint64_t fault, bool write) {
	using Extra = Common::SubmitProfile::Extra;
	const auto limit = write ? Common::ReadbackAhead::WritePages() : Common::ReadbackAhead::ReadPages();
	const auto sparse = write ? Extra::WriteAheadRejectSparse : Extra::ReadAheadRejectSparse;
	const auto range = write ? Extra::WriteAheadRejectRange : Extra::ReadAheadRejectRange;
	Common::SubmitProfile::AddExtra(write ? Extra::WriteAheadAttempts : Extra::ReadAheadAttempts);
	const GuestRange original {fault, 1};
	const auto reject = [&](Extra reason) {
		Common::SubmitProfile::AddExtra(reason);
		return original;
	};
	if (!GuestGpu::IsGpuThread()) return reject(range);
	if (HasUnpublishedDownloads()) return reject(write ? Extra::WriteAheadRejectPending : Extra::ReadAheadRejectPending);
	// No import lifecycle changes or backing writes into live imported memory.
	if (Common::GuestHostImport::retained_bytes.load(std::memory_order_relaxed))
		return reject(write ? Extra::WriteAheadRejectImport : Extra::ReadAheadRejectImport);
	const auto first = fault & ~(TRACKER_PAGE_SIZE - 1);
	// Candidate: also accept pages with only some GPU-dirty bytes. The download still copies
	// just the GPU-modified intervals of each page (as the single-page path does), and pages
	// with CPU data pending upload stay excluded, so no CPU bytes are overwritten.
	const bool partial_pages = Common::SubmitProfile::ExperimentActive(
	    Common::SubmitProfile::Experiment::ReadbackAheadSparse);
	uint64_t bytes = 0;
	Extra stop = sparse;
	for (unsigned page = 0; page < limit; ++page) {
		const auto address = first + page * TRACKER_PAGE_SIZE;
		if (!GuestRange {address, TRACKER_PAGE_SIZE}.Valid() ||
		    !buffer.IsInBounds(address, TRACKER_PAGE_SIZE) ||
		    !m_scheduler.Context().IsMapped(address, TRACKER_PAGE_SIZE)) {
			stop = range;
			break;
		}
		// Full conservative dirty coverage, not just a bound resource or one
		// dirty byte in a page. Stop at holes, sparse labels and CPU-owned pages.
		if (!(partial_pages ? m_gpu_modified_ranges.Intersects(address, TRACKER_PAGE_SIZE)
		                    : m_gpu_modified_ranges.Contains(address, TRACKER_PAGE_SIZE)) ||
		    !m_memory_tracker.IsRegionGpuModified(address, TRACKER_PAGE_SIZE) ||
		    m_memory_tracker.IsRegionCpuModified(address, TRACKER_PAGE_SIZE)) {
			if (page == 0) Common::SubmitProfile::AddExtra(
			    write ? Extra::WriteAheadRejectFirstPage : Extra::ReadAheadRejectFirstPage);
			break;
		}
		bytes += TRACKER_PAGE_SIZE;
	}
	if (bytes < 2 * TRACKER_PAGE_SIZE) return reject(stop);
	if (bytes == limit * TRACKER_PAGE_SIZE)
		Common::SubmitProfile::AddExtra(write ? Extra::WriteAheadLimitHits : Extra::ReadAheadLimitHits);
	// No owner creation/merge, discarded GPU data or predicted CPU store widths.
	// DownloadBufferMemory uses the normal copy, timeline and publication ordering.
	return {first, bytes};
}

uint64_t BufferCache::ContiguousRegisteredSize(uint64_t vaddr, uint64_t limit) const {
	if (!GuestRange {vaddr, limit}.Valid()) return 0;
	auto it = m_buffers.upper_bound(vaddr);
	if (it != m_buffers.begin()) --it;
	uint64_t end = vaddr;
	const auto stop = vaddr + limit;
	for (; it != m_buffers.end() && end < stop; ++it) {
		const auto& buffer = m_slot_buffers[it->second];
		if (buffer.CpuAddress() > end) break;
		const auto buffer_end = buffer.CpuAddress() + buffer.Size();
		if (buffer_end > end) end = std::min(stop, buffer_end);
	}
	return end - vaddr;
}

bool BufferCache::TryReadSrtBatch(uint64_t fault, uint64_t address, uint64_t bytes) {
	if (!GuestGpu::IsGpuThread() || bytes < 8 || bytes > 4096 || (address & 3u) || (bytes & 3u) ||
	    !GuestRange {address, bytes}.Valid() || fault < address || fault - address > bytes - 4 ||
	    (fault & 3u) || HasUnpublishedDownloads() || !m_scheduler.Context().IsMapped(address, bytes)) return false;
	const auto first_page = address & ~uint64_t {4095};
	const auto last_page = (address + bytes - 1) & ~uint64_t {4095};
	if (last_page != first_page + 4096 || !HasGpuDirtyBytes(fault, 4) ||
	    !HasGpuDirtyBytes(first_page, 4096) || !HasGpuDirtyBytes(last_page, 4096)) return false;
	// Never create or merge cache buffers to widen a readback in a fault handler.
	const auto* owner = m_page_table.Find(address >> PageTable::kPageBits);
	if (!owner || !*owner || !m_slot_buffers[*owner].IsInBounds(address, bytes)) return false;
	ReadMemory(address, bytes, false, Common::SubmitProfile::WaitSite::ReadbackCpuReadFault);
	Common::SubmitProfile::AddExtra(Common::SubmitProfile::Extra::SrtBatchReadbacks);
	Common::SubmitProfile::AddExtra(Common::SubmitProfile::Extra::SrtBatchRequestedBytes, bytes);
	return true;
}

bool BufferCache::TouchRegisteredRange(uint64_t vaddr, uint64_t size) {
	if (!size || ContiguousRegisteredSize(vaddr, size) != size) return false;
	auto it = m_buffers.upper_bound(vaddr);
	if (it != m_buffers.begin()) --it;
	for (; it != m_buffers.end() && it->first < vaddr + size; ++it) {
		const auto& buffer = m_slot_buffers[it->second];
		if (buffer.CpuAddress() + buffer.Size() > vaddr) TouchBuffer(buffer);
	}
	return true;
}

BufferId BufferCache::FindBuffer(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0) {
		return NULL_BUFFER_ID;
	}
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid buffer discovery request\n");
	}
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner != nullptr && *owner) {
		auto& buffer = m_slot_buffers[*owner];
		if (buffer.IsInBounds(vaddr, size)) {
			return *owner;
		}
	}
	return CreateBuffer(vaddr, size);
}

BufferCache::OverlapResult BufferCache::ResolveOverlaps(uint64_t vaddr, uint64_t size) {
	static constexpr int      StreamLeapThreshold = 16;
	static constexpr uint64_t StreamLeapSize      = CACHING_PAGESIZE * 128;

	auto       begin      = vaddr;
	auto       end        = vaddr + size;
	const auto find_first = [&](uint64_t address) {
		auto first = m_buffers.lower_bound(address);
		if (first != m_buffers.begin()) {
			const auto  previous = std::prev(first);
			const auto& buffer   = m_slot_buffers[previous->second];
			if (buffer.CpuAddress() + buffer.Size() > address) {
				first = previous;
			}
		}
		return first;
	};
	auto first           = find_first(begin);
	auto last            = first;
	int  stream_score    = 0;
	bool has_stream_leap = false;
	for (; last != m_buffers.end() && last->first < end; ++last) {
		const auto& buffer        = m_slot_buffers[last->second];
		const auto  buffer_begin  = buffer.CpuAddress();
		const auto  buffer_end    = buffer_begin + buffer.Size();
		const bool  expands_left  = buffer_begin < begin;
		const bool  expands_right = buffer_end > end;
		begin                     = std::min(begin, buffer_begin);
		end                       = std::max(end, buffer_end);
		if (!has_stream_leap && (stream_score += buffer.StreamScore()) > StreamLeapThreshold) {
			has_stream_leap = true;
			// Reserve space in the incoming stream's direction of growth.
			// The old buffer extending left of the request predicts growth to the right, and vice versa.
			if (expands_left) {
				end += std::min(StreamLeapSize, (vaddr < LOWER_ADDRESS_SIZE ? LOWER_ADDRESS_SIZE
				                                       : LibKernel::Memory::kExtendedMemoryBase +
				                                             LibKernel::Memory::kExtendedMemorySize) - end);
			}
			if (expands_right) {
				const auto minimum = vaddr < LOWER_ADDRESS_SIZE
				                         ? CACHING_PAGESIZE * 2
				                         : LibKernel::Memory::kExtendedMemoryBase;
				if (begin > minimum) {
					begin -= std::min(StreamLeapSize, begin - minimum);
				}
				first = find_first(begin);
				begin = std::min(begin, first->first);
			}
		}
	}
	return {first, last, begin, end, has_stream_leap};
}

void BufferCache::JoinOverlap(BufferId new_id, BufferId overlap_id, bool accumulate_stream_score) {
	auto& new_buffer = m_slot_buffers[new_id];
	auto& overlap    = m_slot_buffers[overlap_id];
	if (accumulate_stream_score) {
		new_buffer.IncreaseStreamScore(overlap.StreamScore() + 1);
	}
	new_buffer.CopyFrom(m_scheduler.Current(), overlap, 0,
	                    overlap.CpuAddress() - new_buffer.CpuAddress(), overlap.Size());
	DeleteBuffer(overlap_id);
}

BufferId BufferCache::CreateBuffer(uint64_t vaddr, uint64_t size) {
	EXIT_IF(m_scheduler.Current().IsInvalid());
	const auto end = Common::AlignUp(vaddr + size, CACHING_PAGESIZE);
	vaddr = Common::AlignDown(vaddr, CACHING_PAGESIZE);
	size               = end - vaddr;
	const auto overlap = ResolveOverlaps(vaddr, size);
	if (m_import_armed) RetireHostImports(overlap.begin, overlap.end - overlap.begin, Common::SubmitProfile::Extra::HostImportFallbackMerge);

	const auto id = m_slot_buffers.insert(
	    m_graphics, m_scheduler, MemoryUsage::DeviceLocal, overlap.begin,
	    AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress, overlap.end - overlap.begin);
	const auto& buffer = m_slot_buffers[id];
	SetVulkanObjectNameF(m_graphics.device, buffer.Handle(),
	                     "Kyty.GameBuffer[guest=0x{:016x} size=0x{:x}]", overlap.begin,
	                     overlap.end - overlap.begin);
	for (auto it = overlap.first; it != overlap.last;) {
		const auto old_id = (it++)->second;
		JoinOverlap(id, old_id, !overlap.has_stream_leap);
	}
	Register(id);
	return id;
}

bool BufferCache::SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size, bool is_written,
                                    bool is_texel_buffer, Common::BufferUploadProfile::Use use) {
	if (m_hot_selected) CheckHotGate();
	if (is_written || use == Common::BufferUploadProfile::Use::Image)
		ExcludeHotRange(vaddr, size, is_written ? Common::SubmitProfile::Extra::HotRetireGpu : Common::SubmitProfile::Extra::HotRetireImage);
	const bool hot_use = HotUse(vaddr, size);
	if (m_import_selected && !m_import_armed && size >= 1024 * 1024 &&
	    use == Common::BufferUploadProfile::Use::RangeSync &&
	    Common::SubmitProfile::ExperimentActive(Common::SubmitProfile::Experiment::GuestHostImport) &&
	    buffer.import_observed_frame != m_import_frame && m_memory_tracker.IsRegionCpuModified(vaddr, size)) {
		buffer.import_observed_frame = m_import_frame;
		Common::SubmitProfile::AddExtra(Common::SubmitProfile::Extra::HostImportFallbackUnsupported);
		NoteImportCandidate(buffer, vaddr, size, Common::SubmitProfile::Extra::HostCandidateUnsupported);
	}
	if (m_import_armed && !m_import_retiring) {
		CheckHostImportGate(use == Common::BufferUploadProfile::Use::RangeSync);
		if (use == Common::BufferUploadProfile::Use::RangeSync && m_import_gate && TryHostImport(buffer, vaddr, size) &&
		    !m_import_copy_mode) return false;
		// Writers were excluded by ObtainBuffer before acquisition. This direct
		// image path also serves ObtainBufferForImage, which bypasses that wrapper.
		if (use == Common::BufferUploadProfile::Use::Image)
			ExcludeHostImport(vaddr, size, Common::SubmitProfile::Extra::HostImportFallbackImage, use);
	}
	std::vector<vk::BufferCopy> copies;
	uint64_t                    total_size = 0;
	vk::Buffer                  source;
	bool                        imported_copy = false;
	const auto add_copy = [&](uint64_t address, uint64_t bytes) noexcept {
		copies.emplace_back(total_size, buffer.Offset(address), bytes);
		total_size += bytes;
	};
	if (m_buffer_upload_profile && m_buffer_upload_profile->Ready()) {
		m_buffer_upload_profile->Sync(false);
		const Common::BufferUploadProfile::Request request {vaddr, size, buffer.CpuAddress(), buffer.Size(), use, is_written, is_texel_buffer};
		std::vector<Common::BufferUploadProfile::Causes> causes;
		m_memory_tracker.ForEachUploadRange(vaddr, size, is_written,
		    [&](uint64_t address, uint64_t bytes, const Common::BufferUploadProfile::Causes& why) noexcept {
			    add_copy(address, bytes); causes.push_back(why);
		    }, [&]() noexcept { source = UploadCopies(buffer, copies, total_size, &request, causes, &imported_copy); });
	} else {
		m_memory_tracker.ForEachUploadRange(vaddr, size, is_written, add_copy,
		    [&]() noexcept { source = UploadCopies(buffer, copies, total_size, nullptr, {}, &imported_copy); });
	}
	if (source) {
		if (imported_copy && copies.size() > 1) {
			// The tracker splits dirty runs at region boundaries. Merge only exact
			// adjacency in BOTH buffers, after choosing the source. Keep holes and
			// separate synchronization/use boundaries intact; A's staging is unchanged.
			size_t count = 0;
			for (const auto copy: copies) {
				if (count && copies[count - 1].srcOffset + copies[count - 1].size == copy.srcOffset &&
				    copies[count - 1].dstOffset + copies[count - 1].size == copy.dstOffset) {
					copies[count - 1].size += copy.size;
				} else {
					copies[count++] = copy;
				}
			}
			copies.resize(count);
		}
		if (hot_use) {
			m_hot_frame_bytes += total_size;
			++m_hot_frame_uses;
			Common::SubmitProfile::AddExtra(Common::SubmitProfile::Extra::HotResyncBytes, total_size);
			Common::SubmitProfile::AddExtra(Common::SubmitProfile::Extra::HotResyncUses);
			if (use == Common::BufferUploadProfile::Use::RangeSync)
				Common::SubmitProfile::AddExtra(Common::SubmitProfile::Extra::HotBdaBytes, total_size);
		}
		auto& command = m_scheduler.Current();
		command.EndRendering();
		const auto native = command.Commands();
		if (imported_copy) {
			// Host-coherent alias: no flush, but explicitly order prior host writes
			// before transfer reads. Later concurrent CPU writes are NOT snapshotted.
			vk::BufferMemoryBarrier host {};
			host.srcAccessMask = vk::AccessFlagBits::eHostWrite;
			host.dstAccessMask = vk::AccessFlagBits::eTransferRead;
			host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			host.buffer = source; host.offset = 0; host.size = buffer.Size();
			Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::Barrier);
			native.pipelineBarrier(vk::PipelineStageFlagBits::eHost, vk::PipelineStageFlagBits::eTransfer,
			    {}, 0, nullptr, 1, &host, 0, nullptr);
		}
		vk::BufferMemoryBarrier before {};
		before.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite |
		                       vk::AccessFlagBits::eTransferRead |
		                       vk::AccessFlagBits::eTransferWrite;
		before.dstAccessMask       = vk::AccessFlagBits::eTransferWrite;
		before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.buffer              = buffer.Handle();
		before.offset              = 0;
		before.size                = buffer.Size();
		Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::Barrier);
		native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
		                       vk::PipelineStageFlagBits::eTransfer,
		                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &before, 0, nullptr);
		Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::Copy);
		native.copyBuffer(source, buffer.Handle(), static_cast<uint32_t>(copies.size()),
		                  copies.data());
		if (imported_copy) {
			// Pin the source through THIS recording tick, including mapping/gate
			// retirement. These ranges never arm a CPU write guard in copy mode.
			std::lock_guard lock(Common::GuestHostImport::mutex);
			m_import_readers.Complete(m_scheduler.GetMasterSemaphore().KnownGpuTick());
			for (const auto& copy: copies)
				m_import_readers.Add(m_scheduler.CurrentTick(), buffer.CpuAddress() + copy.srcOffset, copy.size);
		}
		auto after          = before;
		after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		after.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
		Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::Barrier);
		native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
		                       vk::PipelineStageFlagBits::eAllCommands,
		                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &after, 0, nullptr);
	}
	if (is_texel_buffer && !is_written) {
		return SynchronizeBufferFromImage(buffer, vaddr, size);
	}
	return false;
}

vk::Buffer BufferCache::UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
                                     uint64_t total_size, const Common::BufferUploadProfile::Request* request,
	                                     std::span<const Common::BufferUploadProfile::Causes> causes, bool* imported_copy) {
	if (copies.empty()) {
		return nullptr;
	}
	Common::DebugCounters::Add(Common::DebugCounters::Counter::BufferUploadBytes, total_size);
	using Extra = Common::SubmitProfile::Extra;
	if (m_import_copy_mode && m_import_gate && !m_import_retiring) {
		const auto it = m_host_imports.find(buffer.CpuAddress());
		if (it != m_host_imports.end() && !request && imported_copy) {
			// Source and destination cover the same owner; do not use packed staging offsets.
			for (auto& copy: copies) {
				EXIT_IF(copy.dstOffset > buffer.Size() || copy.size > buffer.Size() - copy.dstOffset);
				copy.srcOffset = copy.dstOffset;
			}
			*imported_copy = true;
			Common::SubmitProfile::AddExtra(Extra::HostImportDmaBytes, total_size);
			Common::SubmitProfile::AddExtra(Extra::HostImportBytesAvoided, total_size);
			return it->second.imported->Handle();
		}
		// The byte/hash diagnostic needs a CPU snapshot. Report that explicit
		// fallback instead of silently measuring a DMA path that still memcpy's.
		Common::SubmitProfile::AddExtra(request ? Extra::HostImportCopyFallbackProfile : Extra::HostImportCopyFallbackNoImport);
		Common::SubmitProfile::AddExtra(Extra::HostImportCopyFallbackBytes, total_size);
	}
	Common::SubmitProfile::AddExtra(Extra::HostImportStagingBytes, total_size);
	if (m_import_retiring) Common::SubmitProfile::AddExtra(Common::SubmitProfile::Extra::HostImportRestoreBytes, total_size);

	const auto source = UploadMemory(m_graphics, m_scheduler, m_staging_buffer, total_size, 4,
	    vk::BufferUsageFlagBits::eTransferSrc, [&](uint8_t* mapped, uint64_t base_offset) {
		size_t index = 0;
		for (auto& copy: copies) {
			const auto address = buffer.CpuAddress() + copy.dstOffset;
			// With the diagnostic on, take the SAME protected read into cached RAM
			// and upload those bytes. Never repeatedly read an uncached staging/BAR
			// mapping for hashes or byte diffs. No extra read of guest memory.
			static_assert(Common::BufferUploadProfile::State::ScratchBytes == TRACKER_REGION_SIZE);
			EXIT_IF(request && copy.size > Common::BufferUploadProfile::State::ScratchBytes);
			auto* destination = request ? m_buffer_upload_profile->Scratch() : mapped + copy.srcOffset;
			if (!LibKernel::Memory::CopySparseMemory(address, destination, copy.size)) {
				EXIT("BufferCache: failed to read sparse guest buffer memory\n");
			}
			if (request) {
				const std::span<const uint8_t> data {destination, static_cast<size_t>(copy.size)};
				m_buffer_upload_profile->Observe(*request, address, data, XXH3_64bits(data.data(), data.size()), causes[index]);
				std::memcpy(mapped + copy.srcOffset, data.data(), data.size());
			}
			++index;
			copy.srcOffset += base_offset;
		}
	});
	return source.first->Handle();
}

void BufferCache::SetHostImportBda(const Buffer& ordinary, vk::DeviceAddress base) {
	std::vector<vk::DeviceAddress> addresses(ordinary.Size() / CACHING_PAGESIZE);
	for (size_t page = 0; page < addresses.size(); ++page) addresses[page] = base + page * CACHING_PAGESIZE;
	WriteDataBuffer(m_bda_pagetable_buffer, PageIndex(ordinary.CpuAddress()) * sizeof(vk::DeviceAddress),
	    addresses.data(), addresses.size() * sizeof(vk::DeviceAddress));
}

void BufferCache::InvalidateHostImport(uint64_t address, uint64_t size, Common::BufferUploadProfile::Cause cause) {
	using Extra = Common::SubmitProfile::Extra;
	// Invalidation releases an entire tracker page, even for a one-byte store.
	const auto page = Common::AlignDown(address, TRACKER_PAGE_SIZE);
	const auto bytes = Common::AlignUp(address + size, TRACKER_PAGE_SIZE) - page;
	auto& master = m_scheduler.GetMasterSemaphore();
	const auto invalidate_if_safe = [&] {
		std::unique_lock lock(Common::GuestHostImport::mutex);
		m_import_readers.Complete(master.KnownGpuTick());
		if (m_import_guard_enabled && m_import_readers.Find(page, bytes).tick) return false;
		bool flush = false;
		m_memory_tracker.InvalidateRegion(address, size, [&] { flush = true; }, cause);
		m_import_cpu_dirty = true;
		lock.unlock();
		if (flush) ReadMemory(address, size, true, Common::SubmitProfile::WaitSite::ReadbackCpuWriteInvalidate, cause);
		QueueBdaSync(address, size);
		return true;
	};
	if (invalidate_if_safe()) {
		Common::SubmitProfile::AddExtra(m_import_guard_enabled ? Extra::HostImportGuardDisjoint : Extra::HostImportGuardBypassed);
		return;
	}
	// The cached completion can lag. Query only on an overlap, outside the
	// registry lock, before paying a synchronous GPU-lane round trip.
	master.Refresh();
	if (invalidate_if_safe()) {
		Common::SubmitProfile::AddExtra(Extra::HostImportGuardCompleted);
		return;
	}
	const bool profile = Common::SubmitProfile::Recording();
	const auto start = profile ? Common::SubmitProfile::NowNs() : 0;
	PendingRangeReads::Conflict waited;
	const auto guarded = [&, this] {
		// Re-evaluate on the serialized lane: additional overlapping draws may
		// have been emitted while this CPU fault was queued.
		waited = WaitHostImportReads(page, bytes);
		bool flush = false;
		{
			std::lock_guard lock(Common::GuestHostImport::mutex);
			// The default overload bypasses the import callback: this is already
			// the serialized GPU lane, and readers have completed.
			m_memory_tracker.InvalidateRegion(address, size, [&] { flush = true; }, cause);
			m_import_cpu_dirty = true;
		}
		if (flush) ReadMemory(address, size, true, Common::SubmitProfile::WaitSite::ReadbackCpuWriteInvalidate, cause);
		QueueBdaSync(address, size);
	};
	if (m_scheduler.Context().HasGpu()) m_scheduler.Context().GetGpu().SendCommandSync(guarded);
	else guarded();
	if (profile) {
		const auto elapsed = Common::SubmitProfile::NowNs() - start;
		Common::SubmitProfile::AddExtra(Extra::HostImportGuardLaneCalls);
		Common::SubmitProfile::AddExtra(Extra::HostImportGuardLaneNs, elapsed);
		if (waited.tick) {
			Common::SubmitProfile::AddExtra(Extra::HostImportGuardWaits);
			Common::SubmitProfile::AddExtra(Extra::HostImportGuardNs, elapsed);
			if (waited.known) Common::SubmitProfile::AddExtra(Extra::HostImportGuardRangeWaits);
			if (waited.unknown) Common::SubmitProfile::AddExtra(Extra::HostImportGuardUnknownWaits);
		}
	}
}

PendingRangeReads::Conflict BufferCache::WaitHostImportReads(uint64_t address, uint64_t size, bool retirement) {
	auto& master = m_scheduler.GetMasterSemaphore();
	PendingRangeReads::Conflict conflict;
	{
		std::lock_guard lock(Common::GuestHostImport::mutex);
		m_import_readers.Complete(master.KnownGpuTick());
		conflict = m_import_readers.Find(address, size);
	}
	if (!conflict.tick) return {};
	master.Refresh();
	if (!master.IsFree(conflict.tick)) {
		// Only a reference in the recording tick requires a flush. An older
		// reader waits its own tick and leaves unrelated recording work alone.
		ImportTimer timer(Common::SubmitProfile::Extra::HostImportRetireWaitNs, retirement);
		if (retirement) {
			Common::SubmitProfile::AddExtra(Common::SubmitProfile::Extra::HostImportRetireWaits);
			if (conflict.tick == m_scheduler.CurrentTick())
				Common::SubmitProfile::AddExtra(Common::SubmitProfile::Extra::HostImportRetireFlushes);
		}
		m_scheduler.Wait(conflict.tick);
	} else {
		conflict = {};
	}
	std::lock_guard lock(Common::GuestHostImport::mutex);
	m_import_readers.Complete(master.KnownGpuTick());
	return conflict;
}

void BufferCache::RetireHostImports(uint64_t address, uint64_t size, Common::SubmitProfile::Extra reason) {
	if (m_host_imports.empty()) return;
	for (auto it = m_host_imports.begin(); it != m_host_imports.end();) {
		if (!Common::GuestHostImport::Overlap(address, size, it->first, it->second.lease->span.size)) { ++it; continue; }
		ImportTimer timer(Common::SubmitProfile::Extra::HostImportRetireNs);
		Common::SubmitProfile::AddExtra(Common::SubmitProfile::Extra::HostImportRetirements);
		WaitHostImportReads(it->first, it->second.lease->span.size, true);
		auto entry = std::move(it->second);
		it = m_host_imports.erase(it);
		if (m_import_copy_mode) {
			// VRAM never stopped being authoritative. Keep outstanding CPU dirty
			// bits for its next ordinary sync; no restoration or BDA rewrite needed.
			Common::SubmitProfile::AddExtra(reason);
			continue;
		}
		auto& ordinary = m_slot_buffers[entry.ordinary];
		// The VRAM mirror was intentionally stale while BDA read the imported alias.
		// Restore it before publishing its address or permitting a GPU writer.
		m_import_retiring = true;
		// Only ranges whose uploads were consumed by the overlay are stale. CPU
		// writes not consumed yet remain dirty in the ordinary tracker already.
		entry.stale.ForEach([&](uint64_t first, uint64_t last) {
			m_memory_tracker.MarkRegionAsCpuModified(first, last - first);
		});
		(void)SynchronizeBuffer(ordinary, ordinary.CpuAddress(), ordinary.Size(), false, false);
		SetHostImportBda(ordinary, ordinary.BufferDeviceAddress());
		m_memory_tracker.SetHostImport(ordinary.CpuAddress(), ordinary.Size(), false);
		m_import_retiring = false;
		Common::SubmitProfile::AddExtra(reason);
		// Prepared descriptors still hold ordinary handles. Imported handles are
		// selected only at final emission, whose readers have now completed.
	}
	if (m_host_imports.empty()) {
		Common::GuestHostImport::pending_read_bytes = 0;
		Common::GuestHostImport::pending_known_bytes = 0;
		Common::GuestHostImport::pending_unknown_bytes = 0;
	}
}

void BufferCache::DrainHostImports() {
	if (!m_import_armed || Common::GuestHostImport::retained_bytes.load() == 0) return;
	const auto retire = [this] { RetireHostImports(0, UINT64_MAX, Common::SubmitProfile::Extra::HostImportRetireMapping); };
	if (m_scheduler.Context().HasGpu() && !m_scheduler.Context().GetGpu().IsStopping()) m_scheduler.Context().GetGpu().SendCommandSync(retire);
	else retire();
}

void BufferCache::StopHostImports() {
	if (!m_import_armed) return;
	const auto stop = [this] {
		m_import_stopping = true;
		CheckHostImportGate();
	};
	if (m_scheduler.Context().HasGpu()) m_scheduler.Context().GetGpu().SendCommandSync(stop);
	else stop();
}

void BufferCache::CheckHostImportGate(bool allow_activation) {
	if (!m_import_armed || m_import_retiring) return;
	const bool active = !m_import_stopping && Common::SubmitProfile::ExperimentActive(Common::SubmitProfile::Experiment::GuestHostImport);
	if (active == m_import_gate) return;
	if (active && (!allow_activation || !m_scheduler.Active())) return;
	if (!active) {
		RetireHostImports(0, UINT64_MAX, Common::SubmitProfile::Extra::HostImportRetireGate);
	} else {
		// A recorded no exclusion history. Drain old descriptors/readers and all
		// privileged completion writes before learning B's live resource uses.
		ImportTimer activation_timer(Common::SubmitProfile::Extra::HostImportActivationNs);
		Common::SubmitProfile::AddExtra(Common::SubmitProfile::Extra::HostImportActivations);
		const auto tick = m_scheduler.CurrentTick();
		m_scheduler.Wait(tick);
		m_scheduler.WaitPriorityOperations(tick);
		if (!m_import_probed) {
			Buffer::ProbeHostImport(m_graphics, m_scheduler);
			m_import_probed = true;
			m_import_armed = m_graphics.guest_host_import_enabled;
			if (!m_import_armed) {
				std::lock_guard lock(Common::GuestHostImport::mutex);
				Common::GuestHostImport::armed.store(false);
				Common::GuestHostImport::owner = nullptr; Common::GuestHostImport::drain = nullptr;
				return;
			}
		}
		++m_import_epoch;
		m_import_trace_rows = 0;
	}
	m_import_gpu_excluded.Clear(); m_import_image_excluded.Clear();
	m_import_exclusion_count = 0;
	m_import_gate = active;
}

void BufferCache::ExcludeHostImportSlow(uint64_t address, uint64_t size, Common::SubmitProfile::Extra reason,
                                   Common::BufferUploadProfile::Use use) {
	ImportTimer timer(Common::SubmitProfile::Extra::HostImportExclusionNs,
	    SampleImportWork(m_import_exclusion_slow_samples, Common::SubmitProfile::Extra::HostImportExclusionSamples));
	auto& excluded = reason == Common::SubmitProfile::Extra::HostImportFallbackGpu ? m_import_gpu_excluded : m_import_image_excluded;
	// Exclusions persist until B->A. Do not erase/reallocate RangeSet nodes
	// for every repeated image/descriptor lookup, nor search imports for a range
	// that promotion already rejects.
	if (excluded.Contains(address, size)) return;
	excluded.Add(address, size);
	if (Common::SubmitProfile::Recording()) {
		m_import_exclusion_examples[m_import_exclusion_count++ % m_import_exclusion_examples.size()] = {address, size, reason, use};
	}
	RetireHostImports(address, size, reason);
}

void BufferCache::NoteImportCandidate(Buffer& buffer, uint64_t address, uint64_t size, Common::SubmitProfile::Extra reason) {
	if (!Common::SubmitProfile::Recording()) return;
	using Extra = Common::SubmitProfile::Extra;
	static_assert(static_cast<unsigned>(Extra::HostCandidateImported) - static_cast<unsigned>(Extra::HostCandidateOwnerSmall) < 32);
	const auto bit = uint32_t{1} << (static_cast<unsigned>(reason) - static_cast<unsigned>(Extra::HostCandidateOwnerSmall));
	if (buffer.import_diag_epoch != m_import_epoch) {
		buffer.import_diag_epoch = m_import_epoch;
		buffer.import_diag_mask = buffer.import_trace_mask = 0;
		buffer.import_diag_frame = m_import_frame;
	}
	if (buffer.import_diag_frame != m_import_frame) { buffer.import_diag_frame = m_import_frame; buffer.import_diag_mask = 0; }
	if (!(buffer.import_diag_mask & bit)) { buffer.import_diag_mask |= bit; Common::SubmitProfile::AddExtra(reason); }
	// Per-owner/per-reason, at most 128 rows per B phase. Early filters still have
	// counters but cannot crowd the trace out with small or clean ranges.
	if (reason <= Extra::HostCandidateCooldown || (buffer.import_trace_mask & bit) || m_import_trace_rows >= 128) return;
	buffer.import_trace_mask |= bit; ++m_import_trace_rows;
	ImportExclusion example {};
	for (uint64_t n = 0; n < std::min<uint64_t>(m_import_exclusion_count, m_import_exclusion_examples.size()); ++n) {
		const auto& entry = m_import_exclusion_examples[(m_import_exclusion_count - 1 - n) % m_import_exclusion_examples.size()];
		const bool matches = (reason == Extra::HostCandidateGpuHistory && entry.reason == Extra::HostImportFallbackGpu) ||
		    (reason == Extra::HostCandidateImageHistory && entry.reason == Extra::HostImportFallbackImage);
		if (matches && Common::GuestHostImport::Overlap(buffer.CpuAddress(), buffer.Size(), entry.address, entry.size)) { example = entry; break; }
	}
	uint64_t excluded_begin = 0, excluded_size = 0;
	const auto* excluded = reason == Extra::HostCandidateGpuHistory ? &m_import_gpu_excluded :
	    reason == Extra::HostCandidateImageHistory ? &m_import_image_excluded : nullptr;
	if (excluded) excluded->ForEachInRange(buffer.CpuAddress(), buffer.Size(), [&](uint64_t first, uint64_t last) {
		if (!excluded_size) { excluded_begin = first; excluded_size = last - first; }
	});
	std::fprintf(stderr, "[GUEST_HOST_IMPORT_CANDIDATE] frame=%llu owner=0x%llx owner_size=%llu request=0x%llx request_size=%llu reason=%s excluded=0x%llx excluded_size=%llu example=0x%llx example_size=%llu use=%s\n",
	    static_cast<unsigned long long>(m_import_frame), static_cast<unsigned long long>(buffer.CpuAddress()),
	    static_cast<unsigned long long>(buffer.Size()), static_cast<unsigned long long>(address), static_cast<unsigned long long>(size),
	    Common::SubmitProfile::ExtraNames[static_cast<size_t>(reason)], static_cast<unsigned long long>(excluded_begin),
	    static_cast<unsigned long long>(excluded_size), static_cast<unsigned long long>(example.address), static_cast<unsigned long long>(example.size),
	    Common::BufferUploadProfile::use_names[static_cast<size_t>(example.use)]);
}

bool BufferCache::TryHostImport(Buffer& buffer, uint64_t address, uint64_t size) {
	using Extra = Common::SubmitProfile::Extra;
	ImportTimer timer(Extra::HostImportCandidateNs, SampleImportWork(m_import_candidate_calls, Extra::HostImportCandidateSamples));
	const auto base = buffer.CpuAddress();
	if (!m_host_imports.contains(base)) {
		const auto filtered = [&](Extra cause) { NoteImportCandidate(buffer, address, size, cause); return false; };
		if (buffer.Size() < 1024 * 1024) return filtered(Extra::HostCandidateOwnerSmall);
		if (size < 1024 * 1024) return filtered(Extra::HostCandidateRequestSmall);
		if (buffer.import_retry_frame > m_import_frame) return filtered(Extra::HostCandidateCooldown);
		if (!m_memory_tracker.IsRegionCpuModified(address, size)) return filtered(Extra::HostCandidateClean);
		if (buffer.import_observed_frame != m_import_frame) { buffer.import_observed_frame = m_import_frame; ++buffer.import_observations; }
		if (buffer.import_observations < 3) return filtered(Extra::HostCandidateWarmup);
		buffer.import_retry_frame = m_import_frame + 60;
		const auto reject = [&](Extra cause, Extra candidate) { Common::SubmitProfile::AddExtra(cause); return filtered(candidate); };
		if (m_host_imports.size() >= 16) return reject(Extra::HostImportFallbackLimit, Extra::HostCandidateLimitCount);
		if (buffer.Size() > 64 * 1024 * 1024) return reject(Extra::HostImportFallbackLimit, Extra::HostCandidateLimitSize);
		if (Common::GuestHostImport::retained_bytes.load() + buffer.Size() > 128 * 1024 * 1024)
			return reject(Extra::HostImportFallbackLimit, Extra::HostCandidateLimitTotal);
		if (HasUnpublishedDownloads()) return reject(Extra::HostImportFallbackGpu, Extra::HostCandidateDownloads);
		if (m_import_gpu_excluded.Intersects(base, buffer.Size())) return reject(Extra::HostImportFallbackGpu, Extra::HostCandidateGpuHistory);
		if (m_memory_tracker.IsRegionGpuModified(base, buffer.Size())) return reject(Extra::HostImportFallbackGpu, Extra::HostCandidateGpuDirty);
		if (m_import_image_excluded.Intersects(base, buffer.Size())) return reject(Extra::HostImportFallbackImage, Extra::HostCandidateImageHistory);
		if (m_texture_cache.IsRegionRegistered(base, buffer.Size())) return reject(Extra::HostImportFallbackImage, Extra::HostCandidateImageLive);
		ImportTimer create_timer(Extra::HostImportCreateNs);
		Common::SubmitProfile::AddExtra(Extra::HostImportCreateAttempts);
		Common::GuestHostImport::Reject backing_reason {};
		auto lease = LibKernel::Memory::AcquireHostImport(base, buffer.Size(), &backing_reason);
		if (!lease) {
			using Reject = Common::GuestHostImport::Reject;
			const auto candidate = backing_reason == Reject::Busy ? Extra::HostCandidateBackingBusy :
			    backing_reason == Reject::Unmapped ? Extra::HostCandidateBackingUnmapped :
			    backing_reason == Reject::Discontiguous ? Extra::HostCandidateBackingDiscontiguous : Extra::HostCandidateBackingAlias;
			return reject(Extra::HostImportFallbackBacking, candidate);
		}
		Extra failure {};
		auto imported = Buffer::ImportHost(m_graphics, m_scheduler, base, lease->span.pointer, buffer.Size(), failure);
		if (!imported) return reject(failure, failure == Extra::HostImportFallbackAlignment ? Extra::HostCandidateAlignment :
		    failure == Extra::HostImportFallbackType ? Extra::HostCandidateType : Extra::HostCandidateAllocation);
		RangeSet stale;
		if (!m_import_copy_mode) {
			// Wait previous use of the old owner, then arm EVERY page before making
			// its external address visible. A CPU fault sees the lease from Acquire.
			m_scheduler.Wait(m_scheduler.CurrentTick());
			m_memory_tracker.SetHostImport(base, buffer.Size(), true);
			{
				std::lock_guard lock(Common::GuestHostImport::mutex);
				m_memory_tracker.ForEachUploadRange(base, buffer.Size(), false,
				    [&](uint64_t page, uint64_t bytes) noexcept {
					    stale.Add(page, bytes);
					    Common::SubmitProfile::AddExtra(Extra::HostImportBytesAvoided,
					        bytes - Common::BufferUploadProfile::Outside(page, bytes, address, size));
				    }, []() noexcept {});
			}
			SetHostImportBda(buffer, imported->BufferDeviceAddress());
		}
		const auto id = m_buffers.find(base)->second;
		m_host_imports.emplace(base, HostImportEntry {std::move(lease), std::move(imported), id, std::move(stale)});
		Common::SubmitProfile::AddExtra(buffer.ever_imported ? Extra::HostReimports : Extra::HostImports);
		buffer.ever_imported = true;
		NoteImportCandidate(buffer, address, size, Extra::HostCandidateImported);
	}
	if (m_import_copy_mode) return true;
	std::lock_guard lock(Common::GuestHostImport::mutex);
	auto& stale = m_host_imports.at(base).stale;
	m_memory_tracker.ForEachUploadRange(address, size, false,
	    [&](uint64_t page, uint64_t bytes) noexcept {
		    if (!stale.Contains(page, bytes)) stale.Add(page, bytes);
		    Common::SubmitProfile::AddExtra(Extra::HostImportBytesAvoided, bytes);
	    }, []() noexcept {});
	return true;
}

void BufferCache::AddHostImportRead(uint64_t address, uint64_t size) {
	if (m_import_copy_mode || m_host_imports.empty() || !size) return;
	EXIT_IF(size > UINT64_MAX - address);
	m_import_prepared_reads.emplace_back(address, size);
}

void BufferCache::PrepareHostImportReads(bool unknown_bda) {
	if (m_import_copy_mode || !m_import_armed || m_host_imports.empty()) { m_import_prepared_reads.clear(); return; }
	std::lock_guard lock(Common::GuestHostImport::mutex);
	// Close the race between draining the dirty queue and a CPU writer releasing
	// a page while there was no pending reader. No guest bytes are read here.
	if (m_import_cpu_dirty) {
		for (auto& [base, entry]: m_host_imports)
			m_memory_tracker.ForEachUploadRange(base, entry.lease->span.size, false,
			    [&](uint64_t page, uint64_t bytes) noexcept {
				    if (!entry.stale.Contains(page, bytes)) entry.stale.Add(page, bytes);
				    Common::SubmitProfile::AddExtra(Common::SubmitProfile::Extra::HostImportBytesAvoided, bytes);
			    },
			    []() noexcept {});
		m_import_cpu_dirty = false;
	}
	m_import_readers.Complete(m_scheduler.GetMasterSemaphore().KnownGpuTick());
	const auto tick = m_scheduler.CurrentTick();
	// Descriptor ranges and explicit guest BDA bounds are clipped to active
	// imports and expanded to fault pages. Dirty/upload ranges are NOT read bounds.
	for (const auto [address, size]: m_import_prepared_reads) {
		for (const auto& [base, entry]: m_host_imports) {
			const auto first = std::max(address, base);
			const auto last = std::min(address + size, base + entry.lease->span.size);
			if (first >= last) continue;
			const auto page = Common::AlignDown(first, TRACKER_PAGE_SIZE);
			m_import_readers.Add(tick, page, Common::AlignUp(last, TRACKER_PAGE_SIZE) - page);
		}
	}
	m_import_prepared_reads.clear();
	if (unknown_bda) {
		// Runtime pointer arithmetic / GPU-selected descriptors / BVH have no
		// proven host range. Keep this conservative cause separate from known reads.
		for (const auto& [base, entry]: m_host_imports) m_import_readers.Add(tick, base, entry.lease->span.size, true);
		Common::SubmitProfile::AddExtra(Common::SubmitProfile::Extra::HostImportUnknownBdaDraws);
	}
}

bool BufferCache::ResolveHostImportRead(uint64_t address, uint64_t size, vk::DescriptorBufferInfo& descriptor) {
	if (m_import_copy_mode || m_host_imports.empty() || !size) return false;
	auto it = m_host_imports.upper_bound(address);
	if (it == m_host_imports.begin()) return false;
	auto& entry = std::prev(it)->second;
	const auto& ordinary = m_slot_buffers[entry.ordinary];
	// A stream snapshot or a pre-merge owner keeps its original binding. The
	// imported and ordinary owners have identical bases, sizes and alignments;
	// the packed shader byte adjustment therefore remains unchanged.
	if (!ordinary.IsInBounds(address, size) || descriptor.buffer != ordinary.Handle()) return false;
	descriptor.buffer = entry.imported->Handle();
	EXIT_IF(descriptor.offset > ordinary.Size() || descriptor.range > ordinary.Size() - descriptor.offset);
	AddHostImportRead(ordinary.CpuAddress() + descriptor.offset, descriptor.range);
	Common::SubmitProfile::AddExtra(Common::SubmitProfile::Extra::HostImportDescriptorReads);
	return true;
}

void BufferCache::RetireHot(uint64_t base, HotOwner& owner, Common::SubmitProfile::Extra reason) {
	if (!owner.hot) return;
	owner.hot = false;
	--m_hot_active;
	m_hot_active_bytes -= owner.size;
	m_memory_tracker.SetHot(base, owner.size, false);
	QueueBdaSync(base, owner.size);
	owner.history = {};
	owner.retry_frame = m_hot_frame + 12;
	// A conservative unknown-BDA workload can demand the entire ring every
	// draw. Once its budget is exhausted, do not repeat that burst every probe.
	if (reason == Common::SubmitProfile::Extra::HotRetireBudget) {
		if (!m_hot_excluded.Contains(base, owner.size)) m_hot_excluded.Add(base, owner.size);
		m_memory_tracker.TakeHotFaults(base, owner.size, false);
		owner.observing = false;
	}
	Common::SubmitProfile::AddExtra(Common::SubmitProfile::Extra::HotReprotected);
	Common::SubmitProfile::AddExtra(reason);
	std::fprintf(stderr, "[HOT_UNPROTECTED] retire frame=%llu base=0x%llx size=%llu reason=%s frame_resync_bytes=%llu\n",
	    static_cast<unsigned long long>(m_hot_frame), static_cast<unsigned long long>(base),
	    static_cast<unsigned long long>(owner.size), Common::SubmitProfile::ExtraNames[static_cast<size_t>(reason)],
	    static_cast<unsigned long long>(m_hot_frame_bytes));
	Common::SubmitProfile::hot_owner_count = m_hot_active;
	Common::SubmitProfile::hot_owner_bytes = m_hot_active_bytes;
}

void BufferCache::ExcludeHotRange(uint64_t address, uint64_t size, Common::SubmitProfile::Extra reason) {
	if (!m_hot_gate || !size) return;
	const bool sample = (++m_hot_exclusion_calls & 1023u) == 0 && Common::SubmitProfile::Recording();
	if (sample) Common::SubmitProfile::AddExtra(Common::SubmitProfile::Extra::HotExclusionSamples);
	ImportTimer timer(Common::SubmitProfile::Extra::HotExclusionNs, sample);
	// Promotions reject every overlap with this set, so an already excluded
	// range cannot acquire a new hot owner until the gate has been reset.
	if (m_hot_excluded.Contains(address, size)) return;
	m_hot_excluded.Add(address, size);
	if (!m_hot_active) return;
	// Buffer owners are disjoint. Visit only potential overlaps, not the entire
	// history map for every texture lookup / GPU write notification.
	auto it = m_hot_owners.upper_bound(address);
	if (it != m_hot_owners.begin()) --it;
	const auto end = address + size; // RangeSet::Add validated overflow above.
	for (; it != m_hot_owners.end() && it->first < end; ++it) {
		++m_hot_exclusion_visits;
		if (Common::GuestHostImport::Overlap(address, size, it->first, it->second.size))
			RetireHot(it->first, it->second, reason);
	}
}

void BufferCache::StopHotBuffers() {
	if (!m_hot_selected) return;
	const auto stop = [this] { m_hot_stopping = true; CheckHotGate(); };
	if (m_scheduler.Context().HasGpu() && !m_scheduler.Context().GetGpu().IsStopping()) m_scheduler.Context().GetGpu().SendCommandSync(stop);
	else stop();
}

void BufferCache::CheckHotGate(bool allow_activation) {
	const bool active = m_hot_selected && !m_hot_stopping && Common::SubmitProfile::ExperimentActive(Common::SubmitProfile::Experiment::HotUnprotected);
	if (active == m_hot_gate || (active && (!allow_activation || !m_scheduler.Active()))) return;
	if (!active) {
		if (m_hot_frame_bytes || m_hot_frame_uses || m_hot_bda_calls || m_hot_exclusion_calls || m_hot_exclusion_visits)
			ReportHotFrame(false);
		for (auto& [base, owner]: m_hot_owners) {
			RetireHot(base, owner, Common::SubmitProfile::Extra::HotRetireGate);
			m_memory_tracker.TakeHotFaults(base, owner.size, false);
		}
		m_hot_owners.clear();
	} else {
		// Complete old writers and privileged readback callbacks before learning
		// this B interval. Promotion itself never waits for ordinary GPU readers:
		// shaders continue to use the VRAM snapshots, not guest memory.
		const auto tick = m_scheduler.CurrentTick();
		m_scheduler.Wait(tick);
		m_scheduler.WaitPriorityOperations(tick);
	}
	m_hot_excluded.Clear();
	m_hot_gate = active;
	m_hot_frame_bytes = 0;
	m_hot_frame_uses = m_hot_bda_calls = m_hot_bda_peak = 0;
	m_hot_exclusion_calls = m_hot_exclusion_visits = 0;
}

bool BufferCache::HotUse(uint64_t address, uint64_t size) {
	if (!m_hot_active) return false;
	auto it = m_hot_owners.upper_bound(address);
	if (it == m_hot_owners.begin()) return false;
	--it;
	auto& owner = it->second;
	if (!owner.hot || address - it->first >= owner.size || size > owner.size - (address - it->first)) return false;
	const auto upload_bytes = Common::AlignUp(address + size, TRACKER_PAGE_SIZE) - Common::AlignDown(address, TRACKER_PAGE_SIZE);
	if (HasUnpublishedDownloads() || m_hot_frame_bytes + upload_bytes > m_hot_budget) {
		RetireHot(it->first, owner, HasUnpublishedDownloads() ? Common::SubmitProfile::Extra::HotRetireReadback : Common::SubmitProfile::Extra::HotRetireBudget);
		return false;
	}
	owner.last_use = m_hot_frame;
	return true;
}

void BufferCache::SynchronizeHotRange(uint64_t address, uint64_t size) {
	if (m_hot_selected) CheckHotGate();
	if (!m_hot_active || !size) return;
	for (const auto& [base, owner]: m_hot_owners) {
		if (!owner.hot) continue;
		const auto begin = std::max(base, address);
		const auto end = std::min(base + owner.size, address + size);
		if (begin < end) SynchronizeBuffersInRange(begin, end - begin);
	}
}

void BufferCache::ReportHotFrame(bool complete) {
	using Extra = Common::SubmitProfile::Extra;
	if (complete) {
		Common::SubmitProfile::AddExtra(Extra::HotCompletedFrames);
		Common::SubmitProfile::AddExtra(Extra::HotFrameResyncBytes, m_hot_frame_bytes);
		Common::SubmitProfile::AddExtra(Extra::HotFrameResyncUses, m_hot_frame_uses);
	}
	Common::SubmitProfile::AddExtra(Extra::HotBdaCalls, m_hot_bda_calls);
	Common::SubmitProfile::AddExtra(Extra::HotExclusionCalls, m_hot_exclusion_calls);
	Common::SubmitProfile::AddExtra(Extra::HotExclusionVisits, m_hot_exclusion_visits);
	if (m_hot_frame_profile) {
		std::fprintf(stderr, "[HOT_UNPROTECTED_FRAME] frame=%llu complete=%u bytes=%llu uses=%llu bda_calls=%llu owners_end=%llu exclusion_calls=%llu exclusion_visits=%llu\n",
		    static_cast<unsigned long long>(m_hot_frame), unsigned(complete),
		    static_cast<unsigned long long>(m_hot_frame_bytes), static_cast<unsigned long long>(m_hot_frame_uses),
		    static_cast<unsigned long long>(m_hot_bda_calls), static_cast<unsigned long long>(m_hot_active),
		    static_cast<unsigned long long>(m_hot_exclusion_calls), static_cast<unsigned long long>(m_hot_exclusion_visits));
	}
	m_hot_bda_peak = std::max(m_hot_bda_peak, m_hot_bda_calls);
	m_hot_frame_bytes = m_hot_frame_uses = m_hot_bda_calls = 0;
	m_hot_exclusion_calls = m_hot_exclusion_visits = 0;
}

void BufferCache::HotFrame() {
	using Extra = Common::SubmitProfile::Extra;
	if (m_hot_gate) ReportHotFrame(true);
	++m_hot_frame;
	CheckHotGate(true);
	if (!m_hot_gate) return;
	ImportTimer maintenance(Extra::HotMaintenanceNs);
	for (const auto& [base, id]: m_buffers) {
		const auto bytes = m_slot_buffers[id].Size();
		if (bytes < MiB || bytes > 64 * MiB) continue;
		auto it = m_hot_owners.find(base);
		if (it == m_hot_owners.end()) {
			if (m_hot_excluded.Intersects(base, bytes)) continue;
			if (m_hot_owners.size() >= 128) continue;
			it = m_hot_owners.emplace(base, HotOwner {}).first;
			it->second.size = bytes;
			// Discard pre-observation counts when an owner was replaced.
			m_memory_tracker.TakeHotFaults(base, bytes, true);
			continue;
		}
		auto& owner = it->second;
		if (!owner.observing) continue;
		if (!owner.hot && m_hot_excluded.Intersects(base, bytes)) {
			m_memory_tracker.TakeHotFaults(base, bytes, false);
			owner.observing = false;
			owner.history = {};
			Common::SubmitProfile::AddExtra(Extra::HotRejectExcluded);
			continue;
		}
		const auto faults = m_memory_tracker.TakeHotFaults(base, bytes, true);
		Common::SubmitProfile::AddExtra(Extra::HotHistoryPages, bytes / TRACKER_PAGE_SIZE);
		Common::SubmitProfile::AddExtra(Extra::HotFaultsObserved, faults);
		const bool was_hot = owner.hot;
		if (was_hot) {
			Common::SubmitProfile::AddExtra(Extra::HotFaultsAvoidedEstimated, owner.estimate > faults ? owner.estimate - faults : 0);
			if (HasUnpublishedDownloads()) RetireHot(base, owner, Extra::HotRetireReadback);
			else if (m_hot_frame - owner.promoted_frame >= 120 || m_hot_frame - owner.last_use >= 12)
				RetireHot(base, owner, Extra::HotRetireAge);
			else if (!m_memory_tracker.CanUseHot(base, bytes)) RetireHot(base, owner, Extra::HotRetireImage);
			continue;
		}
		owner.history.Observe(faults);
		if (m_hot_frame < owner.retry_frame || !owner.history.Ready() || m_hot_active >= 16 || m_hot_active_bytes + bytes > 128 * MiB) continue;
		// Unknown BDA reads require a fresh snapshot at EVERY use. A frame/tick
		// stamp cannot prove that the guest has not written in between. Reject a
		// known unaffordable workload before removing any protection or copying.
		if (m_hot_active_bytes + bytes > m_hot_budget / std::max<uint64_t>(1, m_hot_bda_peak)) {
			m_hot_excluded.Add(base, bytes);
			m_memory_tracker.TakeHotFaults(base, bytes, false);
			owner.observing = false;
			Common::SubmitProfile::AddExtra(Extra::HotRejectBdaBudget);
			std::fprintf(stderr, "[HOT_UNPROTECTED] reject_bda_budget frame=%llu base=0x%llx size=%llu bda_peak_per_frame=%llu\n",
			    static_cast<unsigned long long>(m_hot_frame), static_cast<unsigned long long>(base),
			    static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(m_hot_bda_peak));
			continue;
		}
		if (m_memory_tracker.IsRegionGpuModified(base, bytes)) {
			Common::SubmitProfile::AddExtra(Extra::HotRejectGpu); continue;
		}
		if (HasUnpublishedDownloads()) { Common::SubmitProfile::AddExtra(Extra::HotRejectReadback); continue; }
		if (!m_scheduler.Context().IsMapped(base, bytes)) { Common::SubmitProfile::AddExtra(Extra::HotRejectMapping); continue; }
		if (m_texture_cache.IsRegionRegistered(base, bytes) || !m_memory_tracker.CanUseHot(base, bytes)) {
			Common::SubmitProfile::AddExtra(Extra::HotRejectImage); continue;
		}
		owner.estimate = owner.history.Mean();
		owner.hot = true; owner.promoted_frame = owner.last_use = m_hot_frame;
		m_memory_tracker.SetHot(base, bytes, true);
		QueueBdaSync(base, bytes);
		++m_hot_active; m_hot_active_bytes += bytes;
		Common::SubmitProfile::AddExtra(Extra::HotPromotions);
		std::fprintf(stderr, "[HOT_UNPROTECTED] promote frame=%llu base=0x%llx size=%llu observed_faults=%llu observed_frames=%zu estimated_faults_per_frame=%llu\n",
		    static_cast<unsigned long long>(m_hot_frame), static_cast<unsigned long long>(base), static_cast<unsigned long long>(bytes),
		    static_cast<unsigned long long>(owner.history.Total()), owner.history.frames, static_cast<unsigned long long>(owner.estimate));
	}
	Common::SubmitProfile::AddExtra(Extra::HotFrames);
	Common::SubmitProfile::AddExtra(Extra::HotOwnerFrames, m_hot_active);
	Common::SubmitProfile::hot_owner_count = m_hot_active;
	Common::SubmitProfile::hot_owner_bytes = m_hot_active_bytes;
}

void BufferCache::ReportCoarsening(const FaultCoarsenStats& s) {
	using E = Common::SubmitProfile::Extra;
	Common::SubmitProfile::AddExtra(E::FcFaults, s.faults);
	Common::SubmitProfile::AddExtra(E::FcUniquePageFrames, s.unique);
	Common::SubmitProfile::AddExtra(E::FcRepeatPageFrames, s.repeat);
	Common::SubmitProfile::AddExtra(E::FcRefaultAfterUpload, s.refault_upload);
	Common::SubmitProfile::AddExtra(E::FcExpanded, s.expanded);
	Common::SubmitProfile::AddExtra(E::FcDirtyBytes, s.dirty_bytes);
	Common::SubmitProfile::AddExtra(E::FcNewDirtyBytes, s.new_dirty_bytes);
	Common::SubmitProfile::AddExtra(E::FcExtraDirtyUpperBytes, s.extra_dirty_upper);
	Common::SubmitProfile::AddExtra(E::FcUploadBytes, s.upload_bytes);
	Common::SubmitProfile::AddExtra(E::FcExtraUploadUpperBytes, s.extra_upload_upper);
	Common::SubmitProfile::AddExtra(E::FcRejectGpu, s.reject_gpu);
	Common::SubmitProfile::AddExtra(E::FcRejectImport, s.reject_import);
	Common::SubmitProfile::AddExtra(E::FcRejectWatcher, s.reject_watcher);
	Common::SubmitProfile::AddExtra(E::FcRejectReadback, s.reject_readback);
}

void BufferCache::ForgetCoarsening(uint64_t address, uint64_t size) {
	if (m_coarsen_owners.empty() || !size) return;
	auto it = m_coarsen_owners.upper_bound(address);
	if (it != m_coarsen_owners.begin()) --it;
	for (; it != m_coarsen_owners.end() && it->first < address + size;) {
		const auto base = it->first, bytes = it->second.size;
		if (base + bytes <= address) { ++it; continue; }
		m_memory_tracker.ConfigureFaultCoarsening(base, bytes, m_coarsen_block, false, m_coarsen_counters);
		m_memory_tracker.TakeHotFaults(base, bytes, false);
		if (m_coarsen_counters) ReportCoarsening(m_memory_tracker.TakeCoarsenStats(base, bytes));
		if (it->second.active) Common::SubmitProfile::AddExtra(Common::SubmitProfile::Extra::FcRetired);
		it = m_coarsen_owners.erase(it);
	}
}

void BufferCache::StopFaultCoarsening() {
	if (!m_coarsen_selected) return;
	const auto stop = [this] { m_coarsen_stopping = true; CoarsenFrame(); };
	if (m_scheduler.Context().HasGpu() && !m_scheduler.Context().GetGpu().IsStopping()) m_scheduler.Context().GetGpu().SendCommandSync(stop);
	else stop();
}

void BufferCache::CoarsenFrame() {
	using E = Common::SubmitProfile::Extra;
	++m_coarsen_frame;
	const bool active = !m_coarsen_stopping && Common::SubmitProfile::ExperimentActive(Common::SubmitProfile::Experiment::FaultCoarsening);
	if (!active) {
		if (m_coarsen_gate) ForgetCoarsening(0, UINT64_MAX);
		m_coarsen_gate = false;
		return;
	}
	m_coarsen_gate = true;
	FaultCoarsenStats stats;
	uint64_t owners = 0;
	for (const auto& [base, id]: m_buffers) {
		const auto bytes = m_slot_buffers[id].Size();
		if (bytes < MiB || bytes > 64 * MiB) continue;
		auto it = m_coarsen_owners.find(base);
		if (it == m_coarsen_owners.end()) {
			if (m_coarsen_owners.size() >= 128 || !m_scheduler.Context().IsMapped(base, bytes)) continue;
			it = m_coarsen_owners.emplace(base, CoarsenOwner{}).first;
			it->second.size = bytes;
			m_memory_tracker.ConfigureFaultCoarsening(base, bytes, m_coarsen_block, false, m_coarsen_counters);
			m_memory_tracker.TakeHotFaults(base, bytes, true);
			// Discard old diagnostics when an address is reused. No protection or
			// dirty state is changed by starting/stopping observation.
			if (m_coarsen_counters) stats += m_memory_tracker.TakeCoarsenStats(base, bytes, m_coarsen_frame);
			continue;
		}
		auto& owner = it->second;
		const auto faults = m_memory_tracker.TakeHotFaults(base, bytes, true);
		if (m_coarsen_counters) stats += m_memory_tracker.TakeCoarsenStats(base, bytes, m_coarsen_frame);
		if (m_coarsen_profile && (faults || owner.active))
			std::fprintf(stderr, "[FAULT_COARSEN_OWNER] frame=%llu base=0x%llx size=%llu active=%u faults=%llu\n",
			    static_cast<unsigned long long>(m_coarsen_frame), static_cast<unsigned long long>(base),
			    static_cast<unsigned long long>(bytes), unsigned(owner.active), static_cast<unsigned long long>(faults));
		if (owner.active) {
			if (m_coarsen_frame - owner.promoted_frame >= 120) {
				m_memory_tracker.ConfigureFaultCoarsening(base, bytes, m_coarsen_block, false, m_coarsen_counters);
				owner.active = false; owner.history = {};
				Common::SubmitProfile::AddExtra(E::FcRetired);
			} else ++owners;
			continue;
		}
		owner.history.Observe(faults);
		if (!owner.history.Ready()) continue;
		// Eligibility is only permission to TRY an expansion. The fault checks
		// current GPU ownership/import/watchers under the same lock as dirtying.
		m_memory_tracker.ConfigureFaultCoarsening(base, bytes, m_coarsen_block, true, m_coarsen_counters);
		owner.active = true; owner.promoted_frame = m_coarsen_frame;
		++owners;
		Common::SubmitProfile::AddExtra(E::FcPromotions);
		std::fprintf(stderr, "[FAULT_COARSEN] promote frame=%llu base=0x%llx size=%llu faults_window=%llu\n",
		    static_cast<unsigned long long>(m_coarsen_frame), static_cast<unsigned long long>(base),
		    static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(owner.history.Total()));
	}
	ReportCoarsening(stats);
	Common::SubmitProfile::AddExtra(E::FcFrames);
	Common::SubmitProfile::AddExtra(E::FcOwnerFrames, owners);
	if (m_coarsen_profile)
		std::fprintf(stderr, "[FAULT_COARSEN_FRAME] frame=%llu time_ns=%llu owners=%llu faults=%llu unique_pages=%llu repeat_pages=%llu refault_after_upload=%llu expanded=%llu dirty_bytes=%llu new_dirty_bytes=%llu upload_bytes=%llu extra_upload_upper=%llu\n",
		    static_cast<unsigned long long>(m_coarsen_frame), static_cast<unsigned long long>(Common::SubmitProfile::NowNs()), static_cast<unsigned long long>(owners),
		    static_cast<unsigned long long>(stats.faults), static_cast<unsigned long long>(stats.unique),
		    static_cast<unsigned long long>(stats.repeat), static_cast<unsigned long long>(stats.refault_upload),
		    static_cast<unsigned long long>(stats.expanded), static_cast<unsigned long long>(stats.dirty_bytes),
		    static_cast<unsigned long long>(stats.new_dirty_bytes), static_cast<unsigned long long>(stats.upload_bytes),
		    static_cast<unsigned long long>(stats.extra_upload_upper));
}

void BufferCache::BufferUploadProfileFrame() {
	if (m_coarsen_selected) CoarsenFrame();
	if (m_hot_selected) HotFrame();
	if (Common::GuestCpuProfile::FaultEnabled() && Common::GuestCpuProfile::AddressesEnabled()) {
		for (auto& snapshot: Common::GuestCpuProfile::buffer_owners) {
			const auto token = snapshot.requested.load(std::memory_order_acquire);
			if (!token || token == snapshot.published.load(std::memory_order_relaxed)) continue;
			snapshot.count = snapshot.dropped = 0;
			for (const auto& [base, id]: m_buffers) snapshot.Add(base, m_slot_buffers[id].Size());
			snapshot.captured_ticks = Common::GuestCpuProfile::Now();
			snapshot.published.store(token, std::memory_order_release);
		}
	}
	if (m_import_selected) { ++m_import_frame; CheckHostImportGate(true); }
	if (!m_host_imports.empty()) {
		std::lock_guard lock(Common::GuestHostImport::mutex);
		m_import_readers.Complete(m_scheduler.GetMasterSemaphore().KnownGpuTick());
		if (Common::SubmitProfile::Recording()) {
			const auto bytes = m_import_readers.PendingBytes();
			Common::GuestHostImport::pending_read_bytes = bytes.total;
			Common::GuestHostImport::pending_known_bytes = bytes.known;
			Common::GuestHostImport::pending_unknown_bytes = bytes.unknown;
		}
	}
	if (m_import_selected) {
		Common::SubmitProfile::AddExtra(Common::SubmitProfile::Extra::HostImportExclusionCalls, m_import_exclusion_calls - m_import_exclusion_reported);
		Common::SubmitProfile::AddExtra(Common::SubmitProfile::Extra::HostImportExclusionCacheHits, m_import_exclusion_hits - m_import_exclusion_hits_reported);
		Common::SubmitProfile::AddExtra(Common::SubmitProfile::Extra::HostImportCandidateCalls, m_import_candidate_calls - m_import_candidate_reported);
		m_import_exclusion_reported = m_import_exclusion_calls;
		m_import_exclusion_hits_reported = m_import_exclusion_hits;
		m_import_candidate_reported = m_import_candidate_calls;
	}
	// Legacy persistent/segment fields remain in the diagnostic schema; the
	// retired small-buffer-persistent experiment can no longer switch paths.
	if (m_buffer_upload_profile && m_buffer_upload_profile->Ready()) m_buffer_upload_profile->Frame(false);
}

template <bool ProfileRebind>
std::pair<Buffer*, uint64_t> BufferCache::ObtainBuffer(uint64_t vaddr, uint64_t size,
                                                       bool is_written, bool is_texel_buffer,
                                                       BufferId id, Common::BufferUploadProfile::Use use) {
	if constexpr (ProfileRebind) ++Common::RenderStageProfile::Detail::state.buffer_rebinds.obtain_calls;
	auto& command = m_scheduler.Current();
	if (command.IsInvalid() || !GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: buffer request requires a recording command buffer\n");
	}
	if (m_hot_selected) CheckHotGate();
	if (is_written) ExcludeHotRange(vaddr, size, Common::SubmitProfile::Extra::HotRetireGpu);
	if (m_import_armed) {
		CheckHostImportGate();
		if (is_written || use == Common::BufferUploadProfile::Use::Image)
			ExcludeHostImport(vaddr, size, is_written ? Common::SubmitProfile::Extra::HostImportFallbackGpu :
			    Common::SubmitProfile::Extra::HostImportFallbackImage, use);
	}
	if (!m_import_copy_mode && !is_written && !m_host_imports.empty()) {
		auto it = m_host_imports.upper_bound(vaddr);
		if (it != m_host_imports.begin()) {
			auto& entry = std::prev(it)->second;
			auto& ordinary = m_slot_buffers[entry.ordinary];
			if (ordinary.IsInBounds(vaddr, size)) {
				TouchBuffer(ordinary);
				using Use = Common::BufferUploadProfile::Use;
				if (use == Use::ShaderRead || use == Use::ShaderScalarRead || use == Use::ShaderFormattedRead) {
					// Placeholder until CommitBindings. A fallback before then restores
					// this mirror; otherwise final emission substitutes the import.
					return {&ordinary, ordinary.Offset(vaddr)};
				}
				// Vertex/index/indirect/copy clients can retain raw handles across
				// later acquisitions. Preserve snapshot semantics without retiring
				// the whole owner or exposing an import with an unpinned lifetime.
				Common::SubmitProfile::AddExtra(Common::SubmitProfile::Extra::HostImportReadSnapshotBytes, size);
				return UploadMemory(m_graphics, m_scheduler, m_stream_buffer, size,
				    std::max<uint64_t>(m_graphics.physical_device_properties.limits.minUniformBufferOffsetAlignment, 4),
				    ReadFlags | vk::BufferUsageFlagBits::eStorageBuffer, [&](uint8_t* mapped, uint64_t) {
					    if (!LibKernel::Memory::CopySparseMemory(vaddr, mapped, size)) EXIT("host import read snapshot failed\n");
				    });
			}
		}
	}

	if (!is_written && size <= CACHING_PAGESIZE &&
	    !m_memory_tracker.IsRegionGpuModified(vaddr, size) &&
	    m_memory_tracker.IsRegionCpuModified(vaddr, size)) {
		const auto alignment = std::max<uint64_t>(
		    m_graphics.physical_device_properties.limits.minUniformBufferOffsetAlignment, 1);
		auto [mapped, offset] = m_stream_buffer.Map(size, alignment, false);
		if (mapped != nullptr) {
			if (!LibKernel::Memory::CopySparseMemory(vaddr, mapped, size)) {
				EXIT("BufferCache: failed to read sparse guest buffer memory\n");
			}
			m_stream_buffer.Commit();
			if (HotUse(vaddr, size)) {
				m_hot_frame_bytes += size;
				++m_hot_frame_uses;
				Common::SubmitProfile::AddExtra(Common::SubmitProfile::Extra::HotResyncBytes, size);
				Common::SubmitProfile::AddExtra(Common::SubmitProfile::Extra::HotResyncUses);
			}
			if constexpr (ProfileRebind) Common::RenderStageProfile::Detail::state.buffer_rebinds.ObserveStream(size);
			if (m_coarsen_selected) Common::SubmitProfile::AddExtra(Common::SubmitProfile::Extra::FcStreamBytes, size);
			return {&m_stream_buffer, offset};
		}
	}

	if (IsBufferInvalid(id) || !m_slot_buffers[id].IsInBounds(vaddr, size)) {
		if constexpr (ProfileRebind) ++Common::RenderStageProfile::Detail::state.buffer_rebinds.searches;
		id = FindBuffer(vaddr, size);
	} else {
		if constexpr (ProfileRebind) ++Common::RenderStageProfile::Detail::state.buffer_rebinds.hint_hits;
	}
	auto& buffer = m_slot_buffers[id];
	TouchBuffer(buffer);
	(void)SynchronizeBuffer(buffer, vaddr, size, is_written, is_texel_buffer, use);
	if (is_written) {
		m_gpu_modified_ranges.Add(vaddr, size);
		ForgetColorMetadataChecks(vaddr, size);
	}
	return {&buffer, buffer.Offset(vaddr)};
}

template std::pair<Buffer*, uint64_t> BufferCache::ObtainBuffer<false>(uint64_t, uint64_t, bool, bool, BufferId, Common::BufferUploadProfile::Use);
template std::pair<Buffer*, uint64_t> BufferCache::ObtainBuffer<true>(uint64_t, uint64_t, bool, bool, BufferId, Common::BufferUploadProfile::Use);

std::pair<Buffer*, uint64_t> BufferCache::ObtainBufferForImage(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid image source\n");
	}
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner != nullptr && *owner) {
		auto& buffer = m_slot_buffers[*owner];
		if (buffer.IsInBounds(vaddr, size)) {
			TouchBuffer(buffer);
			(void)SynchronizeBuffer(buffer, vaddr, size, false, false, Common::BufferUploadProfile::Use::Image);
			return {&buffer, buffer.Offset(vaddr)};
		}
	}
	if (IsRegionGpuModified(vaddr, size)) {
		return ObtainBuffer(vaddr, size, false, false, {}, Common::BufferUploadProfile::Use::Image);
	}

	return UploadMemory(m_graphics, m_scheduler, m_staging_buffer, size, 16,
	    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eStorageBuffer,
	    [&](uint8_t* mapped, uint64_t) {
		if (!Libs::LibKernel::Memory::TryReadSparseBacking(vaddr, mapped, size)) {
			EXIT("BufferCache: failed to read mapped guest image backing\n");
		}
	});
}

void BufferCache::FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds) {
	if ((vaddr & 3u) != 0 || size == 0 || (size & 3u) != 0 || size > UINT64_MAX - vaddr) {
		EXIT("BufferCache: fill range must be dword aligned\n");
	}
	if (is_gds) {
		if (vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - vaddr) {
			EXIT("BufferCache: GDS fill range is out of bounds\n");
		}
		m_gds_buffer.Fill(vaddr, size, value);
		return;
	}
	if (vaddr == 0) {
		EXIT("BufferCache: invalid fill memory address\n");
	}
	(void)m_texture_cache.ClearMeta(vaddr);
	if (!IsRegionGpuModified(vaddr, size)) {
		// Access the guest mapping so write faults invalidate cached buffers and images.
		auto* destination = reinterpret_cast<uint32_t*>(vaddr);
		std::fill(destination, destination + size / sizeof(uint32_t), value);
		return;
	}

	m_texture_cache.InvalidateMemoryFromGPU(vaddr, size);
	auto [dst, dst_offset] = ObtainBuffer(vaddr, size, true, true, {}, Common::BufferUploadProfile::Use::Fill);
	dst->Fill(dst_offset, size, value);
}

void BufferCache::CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds,
                             bool src_gds) {
	const bool dst_memory = !dst_gds;
	const bool src_memory = !src_gds;
	if ((dst_memory && dst_vaddr == 0) || (src_memory && src_vaddr == 0) || size == 0 ||
	    ((dst_gds || src_gds) && ((dst_vaddr | src_vaddr | size) & 3u) != 0) ||
	    size > UINT64_MAX - dst_vaddr || size > UINT64_MAX - src_vaddr || (dst_gds && src_gds) ||
	    (dst_gds && (dst_vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - dst_vaddr)) ||
	    (src_gds && (src_vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - src_vaddr))) {
		EXIT("BufferCache: invalid copy range, src=0x%016" PRIx64 " dst=0x%016" PRIx64
		     " size=0x%016" PRIx64 " src_gds=%d dst_gds=%d\n",
		     src_vaddr, dst_vaddr, size, static_cast<int>(src_gds), static_cast<int>(dst_gds));
	}
	if (src_memory && dst_memory && !IsRegionGpuModified(dst_vaddr, size) &&
	    !IsRegionGpuModified(src_vaddr, size) && !m_texture_cache.FindImageFromRange(src_vaddr, size)) {
		std::memcpy(reinterpret_cast<void*>(dst_vaddr), reinterpret_cast<const void*>(src_vaddr),
		            size);
		return;
	}

	auto& command = m_scheduler.Current();
	if (dst_memory) {
		m_texture_cache.InvalidateMemoryFromGPU(dst_vaddr, size);
	}
	const auto src_id      = src_memory ? FindBuffer(src_vaddr, size) : BufferId {};
	const auto dst_id      = dst_memory ? FindBuffer(dst_vaddr, size) : BufferId {};
	auto [src, src_offset] = src_memory ? ObtainBuffer(src_vaddr, size, false, true, src_id, Common::BufferUploadProfile::Use::Copy)
	                                    : std::pair {&m_gds_buffer, src_vaddr};
	auto [dst, dst_offset] = dst_memory ? ObtainBuffer(dst_vaddr, size, true, true, dst_id, Common::BufferUploadProfile::Use::Copy)
	                                    : std::pair {&m_gds_buffer, dst_vaddr};
	dst->CopyFrom(command, *src, src_offset, dst_offset, size);
}

bool BufferCache::IsRegionRegistered(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid registered-region query\n");
	}
	// Cached buffers are ordered and non-overlapping. The last buffer beginning before the query
	// end is therefore the only possible intersection.
	const auto candidate = m_buffers.lower_bound(vaddr + size);
	if (candidate == m_buffers.begin()) {
		return false;
	}
	const auto& [address, id] = *std::prev(candidate);
	return address + m_slot_buffers[id].Size() > vaddr;
}

void BufferCache::MarkColorMetadataChecked(uint64_t vaddr, uint64_t size, uint32_t keys) {
	for (uint32_t slot = 0; slot < m_checked_color_metadata.size(); slot++) {
		if ((keys & (1u << slot)) != 0) {
			m_checked_color_metadata[slot].Add(vaddr, size);
		}
	}
}

bool BufferCache::IsColorMetadataChecked(uint64_t vaddr, uint64_t size, uint32_t keys) const {
	for (uint32_t slot = 0; slot < m_checked_color_metadata.size(); slot++) {
		if ((keys & (1u << slot)) != 0 && !m_checked_color_metadata[slot].Contains(vaddr, size)) {
			return false;
		}
	}
	return true;
}

void BufferCache::ForgetColorMetadataChecks(uint64_t vaddr, uint64_t size) {
	for (auto& checked: m_checked_color_metadata) {
		checked.Subtract(vaddr, size);
	}
}

bool BufferCache::IsRegionGpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionGpuModified(vaddr, size);
}

bool BufferCache::HasGpuDirtyBytes(uint64_t vaddr, uint64_t size) {
	return m_gpu_modified_ranges.Intersects(vaddr, size);
}

bool BufferCache::IsRegionCpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionCpuModified(vaddr, size);
}

void BufferCache::RunGarbageCollector() {
	const auto tick = m_gc_tick++;
	if (m_graphics.CanReportMemoryUsage()) {
		m_total_used_memory = m_graphics.GetDeviceMemoryUsage();
	}
	if (m_total_used_memory < m_trigger_gc_memory) {
		return;
	}

	const bool     aggressive = m_total_used_memory >= m_critical_gc_memory;
	const uint64_t age        = std::min<uint64_t>(aggressive ? 80 : 160, tick);
	const size_t   limit      = aggressive ? 64 : 32;

	std::vector<BufferId> dirty_buffers;
	size_t                retire_count = 0;
	m_lru_cache.ForEachItemBelow(tick - age, [&](BufferId id) {
		auto& buffer = m_slot_buffers[id];
		EXIT_IF(buffer.is_deleted);
		m_memory_tracker.ValidateGpuDirtyOwnership(m_gpu_modified_ranges, buffer.CpuAddress(),
		                                           buffer.Size(), "garbage collection");
		const bool dirty = m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size());
		if (dirty && !aggressive) {
			return false;
		}
		// Crossing the global budget is not an eviction of every imported ring.
		// Retire only the owner that the LRU actually selected for eviction.
		if (m_import_armed) RetireHostImports(buffer.CpuAddress(), buffer.Size(), Common::SubmitProfile::Extra::HostImportFallbackLimit);
		if (dirty) {
			EXIT_NOT_IMPLEMENTED(!DownloadBufferMemory<true>(buffer, buffer.CpuAddress(), buffer.Size()));
			dirty_buffers.push_back(id);
		} else {
			m_memory_tracker.UntrackMemory(buffer.CpuAddress(), buffer.Size());
			DeleteBuffer(id);
			Common::DebugCounters::Add(Common::DebugCounters::Counter::BufferEvictions);
		}
		return ++retire_count == limit;
	});
	if (dirty_buffers.empty()) {
		return;
	}

	// Publish all queued downloads before releasing their tracked pages and owners.
	const auto completion_tick = m_scheduler.CurrentTick();
	{
		Common::SubmitProfile::WaitSiteScope site(Common::SubmitProfile::WaitSite::BufferGc);
		m_scheduler.Wait(completion_tick);
	}
	m_scheduler.WaitPriorityOperations(completion_tick);
	for (const auto id: dirty_buffers) {
		auto& buffer = m_slot_buffers[id];
		m_memory_tracker.UnmarkRegionAsGpuModified(buffer.CpuAddress(), buffer.Size());
		if (m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size()) ||
		    m_gpu_modified_ranges.Intersects(buffer.CpuAddress(), buffer.Size())) {
			EXIT("BufferCache: garbage collection retained GPU ownership\n");
		}
		m_memory_tracker.UntrackMemory(buffer.CpuAddress(), buffer.Size());
		Unregister(id);
		m_slot_buffers.erase(id);
	}
}

void BufferCache::ProcessFaultBuffer() {
	m_fault_manager.ProcessFaultBuffer();
}

void BufferCache::QueueBdaSync(uint64_t vaddr, uint64_t size) {
	const auto begin = Common::AlignDown(vaddr, TRACKER_PAGE_SIZE);
	const auto end   = Common::AlignUp(vaddr + size, TRACKER_PAGE_SIZE);
	Common::GuestCpuProfile::Stage acquire(Common::GuestCpuProfile::Part::BdaLock);
	std::lock_guard lock(m_bda_sync_mutex);
	acquire.End();
	m_bda_sync.Add(begin, end - begin);
}

RangeSet BufferCache::TakeBdaSync() {
	if (m_hot_selected) CheckHotGate();
	if (m_hot_gate) ++m_hot_bda_calls;
	const auto hot_bytes = m_hot_active_bytes;
	if (hot_bytes && (HasUnpublishedDownloads() || m_hot_frame_bytes + hot_bytes > m_hot_budget)) {
		for (auto& [base, owner]: m_hot_owners)
			RetireHot(base, owner, HasUnpublishedDownloads() ? Common::SubmitProfile::Extra::HotRetireReadback : Common::SubmitProfile::Extra::HotRetireBudget);
	}
	std::lock_guard lock(m_bda_sync_mutex);
	auto ranges = std::exchange(m_bda_sync, {});
	// Unknown BDA pointers can reach any resident owner. Do not substitute dirty
	// upload ranges or last frame's reads for proven shader access bounds.
	if (m_hot_active)
		for (const auto& [base, owner]: m_hot_owners) if (owner.hot) ranges.Add(base, owner.size);
	return ranges;
}

void BufferCache::SynchronizeBuffersInRange(uint64_t vaddr, uint64_t size) {
	const auto end = vaddr + size;
	auto       it  = m_buffers.upper_bound(vaddr);
	if (it != m_buffers.begin()) {
		--it;
	}
	for (; it != m_buffers.end() && it->first < end; ++it) {
		auto&      buffer = m_slot_buffers[it->second];
		const auto start  = std::max(buffer.CpuAddress(), vaddr);
		const auto finish = std::min(buffer.CpuAddress() + buffer.Size(), end);
		if (start < finish) {
			(void)SynchronizeBuffer(buffer, start, finish - start, false, false, Common::BufferUploadProfile::Use::RangeSync);
		}
	}
}

} // namespace Libs::Graphics
