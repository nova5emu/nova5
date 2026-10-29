#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/lruCache.h"
#include "common/slotVector.h"
#include "common/submitProfile.h"
#include "graphics/host_gpu/memoryTracker.h"
#include "graphics/host_gpu/rangeSet.h"
#include "graphics/host_gpu/renderer/cache/faultManager.h"
#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <unordered_set>
#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;
class CommandScheduler;
class TextureCache;

using BufferId = Common::SlotId;
inline constexpr BufferId NULL_BUFFER_ID {0};

class BufferCache {
public:
	static constexpr uint32_t CACHING_PAGEBITS  = 14;
	static constexpr uint64_t CACHING_PAGESIZE  = uint64_t {1} << CACHING_PAGEBITS;
	static constexpr uint64_t CACHING_NUMPAGES  = (LOWER_ADDRESS_SIZE + LibKernel::Memory::kExtendedMemorySize) >> CACHING_PAGEBITS;
	static constexpr uint64_t BDA_PAGETABLE_SIZE =
	    CACHING_NUMPAGES * sizeof(vk::DeviceAddress);

	static constexpr uint64_t PageIndex(uint64_t address) {
		return (address < LOWER_ADDRESS_SIZE
		            ? address
		            : address - LibKernel::Memory::kExtendedMemoryBase + LOWER_ADDRESS_SIZE) >>
		       CACHING_PAGEBITS;
	}
	static constexpr uint64_t GuestAddress(uint64_t offset) {
		return offset < LOWER_ADDRESS_SIZE
		           ? offset
		           : offset - LOWER_ADDRESS_SIZE + LibKernel::Memory::kExtendedMemoryBase;
	}

	BufferCache(GraphicContext& graphics, CommandScheduler& scheduler, PageManager& page_manager,
	            TextureCache& texture_cache);
	~BufferCache();
	KYTY_CLASS_NO_COPY(BufferCache);

	void                   InvalidateMemory(uint64_t vaddr, uint64_t size,
	    Common::BufferUploadProfile::Cause cause = Common::BufferUploadProfile::Cause::Invalidate);
	// site attributes the synchronous wait in SubmitProfile; it is carried into the GPU thread.
	void                   ReadMemory(uint64_t vaddr, uint64_t size, bool is_write = false,
	                                  Common::SubmitProfile::WaitSite site = Common::SubmitProfile::WaitSite::BufferReadback,
	                                  Common::BufferUploadProfile::Cause cause = Common::BufferUploadProfile::Cause::Invalidate);
	// SRT fault recovery: one existing owner, mapped span, two dirty pages;
	// otherwise no side effects. Caller proved every spanned page is needed.
	bool TryReadSrtBatch(uint64_t fault, uint64_t address, uint64_t bytes);
	[[nodiscard]] Buffer&  GetBuffer(BufferId id) { return m_slot_buffers[id]; }
	[[nodiscard]] BufferId FindBuffer(uint64_t vaddr, uint64_t size);
	// GPU-thread only. Full contiguous coverage, not IsRegionRegistered's intersection.
	[[nodiscard]] uint64_t ContiguousRegisteredSize(uint64_t vaddr, uint64_t limit) const;
	// Keep existing BDA owners alive; PrepareBda still drains CPU dirty ranges.
	[[nodiscard]] bool TouchRegisteredRange(uint64_t vaddr, uint64_t size);
	template <bool ProfileRebind = false>
	[[nodiscard]] std::pair<Buffer*, uint64_t> ObtainBuffer(uint64_t vaddr, uint64_t size,
	                                                        bool     is_written,
	                                                        bool     is_texel_buffer = false,
	                                                        BufferId id              = {},
	                                                        Common::BufferUploadProfile::Use use = Common::BufferUploadProfile::Use::Unknown);
	[[nodiscard]] StreamBuffer&                GetUtilityBuffer(MemoryUsage usage) noexcept {
		switch (usage) {
			case MemoryUsage::Upload: return m_staging_buffer;
			case MemoryUsage::Stream: return m_stream_buffer;
			case MemoryUsage::Download: return m_download_buffer;
			case MemoryUsage::DeviceLocal: return m_device_buffer;
		}
		EXIT("BufferCache: invalid utility-buffer usage\n");
	}
	[[nodiscard]] const Buffer* GetGdsBuffer() const noexcept { return &m_gds_buffer; }
	[[nodiscard]] Buffer* GetBdaPageTableBuffer() noexcept { return &m_bda_pagetable_buffer; }
	[[nodiscard]] Buffer* GetFaultBuffer() noexcept { return m_fault_manager.GetFaultBuffer(); }
	[[nodiscard]] std::pair<Buffer*, uint64_t> ObtainBufferForImage(uint64_t vaddr, uint64_t size);
	void FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds);
	void CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds,
	                bool src_gds);
	// Cache-index and exact dirty-range queries require GPU-thread serialization.
	[[nodiscard]] bool IsRegionRegistered(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool HasGpuDirtyBytes(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool HasUnpublishedDownloads() const {
		return m_unpublished_downloads.load(std::memory_order_acquire) != 0;
	}
	// guest-clean-reads: one clean guest load per continuous GPU-dirty episode of a page.
	// A page's budget is renewed only when a download removes its GPU-dirty bytes.
	[[nodiscard]] bool ConsumeCleanReadBudget(uint64_t page);
	[[nodiscard]] bool IsRegionCpuModified(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionGpuModified(uint64_t vaddr, uint64_t size);
	// Color metadata the GPU found free of the clear keys in `keys` (bits of code >> 5) stays so
	// until a GPU write or download.
	void               MarkColorMetadataChecked(uint64_t vaddr, uint64_t size, uint32_t keys);
	[[nodiscard]] bool IsColorMetadataChecked(uint64_t vaddr, uint64_t size, uint32_t keys) const;
	void               ProcessFaultBuffer();
	void               SynchronizeBuffersInRange(uint64_t vaddr, uint64_t size);
	// Queue after the range's state change becomes visible, or a drain could consume it first.
	void                   QueueBdaSync(uint64_t vaddr, uint64_t size);
	[[nodiscard]] RangeSet TakeBdaSync();
	void               RunGarbageCollector();
	void               BufferUploadProfileFrame();
	// GPU lane, before publishing a GPU writer/image or removing a guest mapping.
	void ExcludeHotRange(uint64_t address, uint64_t size, Common::SubmitProfile::Extra reason);
	void StopHotBuffers();
	void StopFaultCoarsening();
	void ForgetCoarsening(uint64_t address, uint64_t size);
	void SynchronizeHotRange(uint64_t address, uint64_t size);
	// Direct mode only: known guest BDA ranges and resolved descriptors are staged
	// on the GPU lane, then protected/published at final emission for CurrentTick().
	// Copy mode never exposes imported buffers to shaders.
	void AddHostImportRead(uint64_t address, uint64_t size);
	void PrepareHostImportReads(bool unknown_bda = true);
	[[nodiscard]] bool HasHostImports() const { return !m_import_copy_mode && !m_host_imports.empty(); }
	// Final descriptor emission only: preparation keeps ordinary handles so a
	// fallback/merge cannot leave a dangling imported VkBuffer in prepared state.
	bool ResolveHostImportRead(uint64_t address, uint64_t size, vk::DescriptorBufferInfo& descriptor);
	void DrainHostImports(); // May be called by a guest mapping transaction, with no mapping locks held.
	void StopHostImports();
	void ExcludeHostImport(uint64_t address, uint64_t size, Common::SubmitProfile::Extra reason,
	    Common::BufferUploadProfile::Use use = Common::BufferUploadProfile::Use::Unknown) {
		using Extra = Common::SubmitProfile::Extra;
		if (m_hot_gate && (reason == Extra::HostImportFallbackGpu || reason == Extra::HostImportFallbackImage))
			ExcludeHotRange(address, size, reason == Extra::HostImportFallbackGpu ? Extra::HotRetireGpu : Extra::HotRetireImage);
		if (!m_import_gate || !size || m_import_retiring || reason == Extra::HostImportFallbackResource) return;
		EXIT_IF(reason != Extra::HostImportFallbackGpu && reason != Extra::HostImportFallbackImage);
		++m_import_exclusion_calls; // GPU-local; flushed once per frame, no per-lookup atomic/QPC.
		auto& excluded = reason == Extra::HostImportFallbackGpu ? m_import_gpu_excluded : m_import_image_excluded;
		if (excluded.ContainsCached(address, size)) { ++m_import_exclusion_hits; return; }
		ExcludeHostImportSlow(address, size, reason, use);
	}

private:
	friend struct BufferCacheTestAccess;
	// Dirty intervals are removed when a download is scheduled, before its
	// priority callback publishes the bytes. The backing is not safe in between.
	std::atomic<uint32_t> m_unpublished_downloads {0};

	bool IsBufferInvalid(BufferId id) const { return !id || m_slot_buffers[id].is_deleted; }

	using BufferMap = std::map<uint64_t, BufferId>;
	struct OverlapResult {
		BufferMap::iterator first;
		BufferMap::iterator last;
		uint64_t            begin;
		uint64_t            end;
		bool                has_stream_leap;
	};

	using PageTable = MultiLevelPageTable<BufferId, CACHING_PAGEBITS, 44, 20>;
	static_assert(CACHING_PAGESIZE == (uint64_t {1} << PageTable::kPageBits));
	void WriteDataBuffer(Buffer& buffer, uint64_t address, const void* source, uint64_t size);
	void TouchBuffer(const Buffer& buffer);
	[[nodiscard]] OverlapResult ResolveOverlaps(uint64_t vaddr, uint64_t size);
	void JoinOverlap(BufferId new_id, BufferId overlap_id, bool accumulate_stream_score);
	[[nodiscard]] BufferId CreateBuffer(uint64_t vaddr, uint64_t size);
	void                   Register(BufferId id);
	void Unregister(BufferId id);
	template <bool insert>
	void ChangeRegister(BufferId id);
	void DeleteBuffer(BufferId id);
	[[nodiscard]] bool SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size,
	                                     bool is_written, bool is_texel_buffer,
	                                     Common::BufferUploadProfile::Use use = Common::BufferUploadProfile::Use::Unknown);
	[[nodiscard]] vk::Buffer UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
	                                      uint64_t total_size,
	                                      const Common::BufferUploadProfile::Request* request = nullptr,
	                                      std::span<const Common::BufferUploadProfile::Causes> causes = {},
	                                      bool* imported_copy = nullptr);
	[[nodiscard]] bool SynchronizeBufferFromImage(Buffer& buffer, uint64_t vaddr, uint64_t size);
	// Synchronous downloads publish before returning; asynchronous callers wait before reuse.
	template <bool async>
	[[nodiscard]] bool DownloadBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size);
	// GPU lane only. Returns the original one-byte request unless at least two
	// consecutive full dirty pages can be published from this existing owner.
	[[nodiscard]] GuestRange FaultReadbackRange(const Buffer& buffer, uint64_t fault, bool write);
	void               ForgetColorMetadataChecks(uint64_t vaddr, uint64_t size);
	std::unique_ptr<Common::BufferUploadProfile::State> m_buffer_upload_profile;
	struct CoarsenOwner {
		uint64_t size = 0, promoted_frame = 0;
		bool active = false;
		HotBufferHistory history;
	};
	std::map<uint64_t, CoarsenOwner> m_coarsen_owners;
	bool m_coarsen_selected = false, m_coarsen_gate = false, m_coarsen_stopping = false;
	bool m_coarsen_profile = false, m_coarsen_counters = false;
	uint64_t m_coarsen_frame = 0, m_coarsen_block = 64 * 1024;
	void CoarsenFrame();
	void ReportCoarsening(const FaultCoarsenStats& stats);
	struct HotOwner {
		uint64_t size = 0, promoted_frame = 0, last_use = 0, retry_frame = 0, estimate = 0;
		bool hot = false, observing = true;
		HotBufferHistory history;
	};
	std::map<uint64_t, HotOwner> m_hot_owners;
	RangeSet m_hot_excluded;
	bool m_hot_selected = false, m_hot_gate = false, m_hot_stopping = false;
	uint64_t m_hot_frame = 0, m_hot_frame_bytes = 0, m_hot_budget = 256 * 1024 * 1024;
	uint64_t m_hot_active = 0, m_hot_active_bytes = 0;
	uint64_t m_hot_frame_uses = 0, m_hot_bda_calls = 0, m_hot_bda_peak = 0;
	uint64_t m_hot_exclusion_calls = 0, m_hot_exclusion_visits = 0;
	bool m_hot_frame_profile = false;
	void CheckHotGate(bool allow_activation = false);
	void ReportHotFrame(bool complete);
	void HotFrame();
	void RetireHot(uint64_t base, HotOwner& owner, Common::SubmitProfile::Extra reason);
	bool HotUse(uint64_t address, uint64_t size);
	struct HostImportEntry {
		std::shared_ptr<Common::GuestHostImport::Lease> lease;
		std::unique_ptr<Buffer> imported; // Destroy Vulkan memory before releasing its backing lease.
		BufferId ordinary;
		RangeSet stale; // CPU upload ranges consumed without updating the VRAM mirror.
	};
	std::map<uint64_t, HostImportEntry> m_host_imports;
	ExclusionRanges m_import_gpu_excluded, m_import_image_excluded;
	bool m_import_selected = false, m_import_armed = false, m_import_gate = false;
	bool m_import_retiring = false, m_import_stopping = false;
	bool m_import_probed = false;
	struct ImportExclusion { uint64_t address = 0, size = 0; Common::SubmitProfile::Extra reason {}; Common::BufferUploadProfile::Use use {}; };
	std::array<ImportExclusion, 256> m_import_exclusion_examples {};
	uint64_t m_import_exclusion_count = 0, m_import_epoch = 0;
	unsigned m_import_trace_rows = 0;
	uint64_t m_import_exclusion_calls = 0, m_import_candidate_calls = 0;
	uint64_t m_import_exclusion_reported = 0, m_import_candidate_reported = 0;
	uint64_t m_import_exclusion_hits = 0, m_import_exclusion_hits_reported = 0, m_import_exclusion_slow_samples = 0;
	void ExcludeHostImportSlow(uint64_t address, uint64_t size, Common::SubmitProfile::Extra reason,
	                          Common::BufferUploadProfile::Use use);
	void NoteImportCandidate(Buffer& buffer, uint64_t address, uint64_t size, Common::SubmitProfile::Extra reason);
	PendingRangeReads m_import_readers; // registry mutex, including CPU faults
	std::vector<std::pair<uint64_t, uint64_t>> m_import_prepared_reads; // GPU lane only
	bool m_import_cpu_dirty = false; // registry mutex
	bool m_import_guard_enabled = true; // startup diagnostic policy, never disables retirement waits
	bool m_import_copy_mode = true; // DMA source only; descriptors/BDA and CPU dirty tracking stay ordinary
	uint64_t m_import_frame = 0;
	void CheckHostImportGate(bool allow_activation = false);
	void RetireHostImports(uint64_t address, uint64_t size, Common::SubmitProfile::Extra reason);
	PendingRangeReads::Conflict WaitHostImportReads(uint64_t address, uint64_t size, bool retirement = false);
	void InvalidateHostImport(uint64_t address, uint64_t size, Common::BufferUploadProfile::Cause cause);
	void SetHostImportBda(const Buffer& ordinary, vk::DeviceAddress base);
	bool TryHostImport(Buffer& buffer, uint64_t address, uint64_t size);

	GraphicContext&                                   m_graphics;
	CommandScheduler&                                 m_scheduler;
	FaultManager                                      m_fault_manager;
	Buffer                                            m_gds_buffer;
	Buffer                                            m_bda_pagetable_buffer;
	Common::SlotVector<Buffer>                        m_slot_buffers;
	Common::LeastRecentlyUsedCache<BufferId, uint64_t> m_lru_cache;
	BufferMap                                         m_buffers;
	PageTable                                         m_page_table;
	RangeSet                                          m_gpu_modified_ranges;
	// One per TileManager::ColorClearSlots key slot.
	std::array<RangeSet, 8>                           m_checked_color_metadata;
	MemoryTracker                                     m_memory_tracker;
	std::mutex                                        m_bda_sync_mutex;
	RangeSet                                          m_bda_sync;
	StreamBuffer                                      m_staging_buffer;
	StreamBuffer                                      m_stream_buffer;
	StreamBuffer                                      m_download_buffer;
	StreamBuffer                                      m_device_buffer;
	TextureCache&                                     m_texture_cache;
	uint64_t                                          m_total_used_memory  = 0;
	uint64_t m_trigger_gc_memory  = 1ull * 1024 * 1024 * 1024;
	uint64_t m_critical_gc_memory = 2ull * 1024 * 1024 * 1024;
	uint64_t m_gc_tick            = 0;
	std::unordered_set<uint64_t> m_clean_read_pages; // GPU thread only.
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
