#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORYTRACKER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORYTRACKER_H_

#include "common/assert.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/rangeSet.h"
#include "graphics/host_gpu/regionManager.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

namespace Libs::Graphics {

class MemoryTracker final {
public:
	explicit MemoryTracker(PageManager& page_manager);
	~MemoryTracker();

	KYTY_CLASS_NO_COPY(MemoryTracker);

	[[nodiscard]] bool IsRegionCpuModified(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionGpuModified(uint64_t vaddr, uint64_t size);
	void               MarkRegionAsCpuModified(uint64_t vaddr, uint64_t size,
	    Common::BufferUploadProfile::Cause cause = Common::BufferUploadProfile::Cause::Invalidate);
	void               MarkRegionAsGpuModified(uint64_t vaddr, uint64_t size);
	void               UnmarkRegionAsGpuModified(uint64_t vaddr, uint64_t size);
	void               UntrackMemory(uint64_t vaddr, uint64_t size);
	void ConfigureFaultCoarsening(uint64_t vaddr, uint64_t size, uint64_t block_size, bool enabled, bool capture = true) {
		CheckNotInUploadCallback();
		Iterate<true>(vaddr, size, [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
			std::scoped_lock lock(manager->lock); manager->ConfigureFaultCoarsening(offset, bytes, block_size, enabled, capture);
		});
	}
	FaultCoarsenStats TakeCoarsenStats(uint64_t vaddr, uint64_t size, uint64_t epoch = 0) {
		CheckNotInUploadCallback(); FaultCoarsenStats stats;
		Iterate<false>(vaddr, size, [&](RegionManager* manager, uint64_t, uint64_t) {
			std::scoped_lock lock(manager->lock); stats += manager->TakeCoarsenStats(epoch);
		});
		return stats;
	}
	uint64_t TakeHotFaults(uint64_t vaddr, uint64_t size, bool observe) {
		CheckNotInUploadCallback(); uint64_t count = 0;
		Iterate<true>(vaddr, size, [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
			std::scoped_lock lock(manager->lock); count += manager->TakeHotFaults(offset, bytes, observe);
		});
		return count;
	}
	bool CanUseHot(uint64_t vaddr, uint64_t size) {
		CheckNotInUploadCallback();
		return !Iterate<true>(vaddr, size, [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
			std::scoped_lock lock(manager->lock); return !manager->CanUseHot(offset, bytes);
		});
	}
	void SetHot(uint64_t vaddr, uint64_t size, bool enabled) {
		CheckNotInUploadCallback();
		Iterate<true>(vaddr, size, [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
			std::scoped_lock lock(manager->lock); manager->SetHot(offset, bytes, enabled);
		});
	}
	void SetHostImport(uint64_t vaddr, uint64_t size, bool enabled) {
		CheckNotInUploadCallback();
		Iterate<true>(vaddr, size, [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
			std::scoped_lock lock(manager->lock);
			manager->SetHostImport(offset, bytes, enabled);
		});
	}
	// Removes protection from a range and flushes GPU-owned data when required.
	template <typename Flush, typename Imported = std::nullptr_t, typename Expanded = std::nullptr_t>
	void InvalidateRegion(uint64_t vaddr, uint64_t size, Flush&& on_flush,
	                      Common::BufferUploadProfile::Cause cause = Common::BufferUploadProfile::Cause::Invalidate,
	                      Imported on_import = nullptr, Expanded on_expanded = nullptr, bool allow_coarsen = true) noexcept {
		static_assert(std::is_invocable_v<Flush&>);
		CheckNotInUploadCallback();

		Iterate<false>(vaddr, size, [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
			[[maybe_unused]] bool imported = false;
			[[maybe_unused]] bool expanded = false;
			const bool should_flush = [&] {
				// Perform both the GPU modification check and CPU state change with the lock in
				// case the GPU thread is racing to mark the page modified. If a flush is needed,
				// on_flush performs the CPU state change.
				std::scoped_lock lock(manager->lock);
				if (cause == Common::BufferUploadProfile::Cause::CpuWriteFault) manager->ObserveHotFault(offset);
				if constexpr (!std::is_same_v<Expanded, std::nullptr_t>) {
					if (cause == Common::BufferUploadProfile::Cause::CpuWriteFault && size == 1)
						expanded = manager->CoarsenFault(offset, bytes, allow_coarsen);
				}
				if constexpr (!std::is_same_v<Imported, std::nullptr_t>) {
					if (manager->HasHostImport(offset, bytes)) { imported = true; return false; }
				}
				if (manager->IsModified<DirtySource::Gpu>(offset, bytes)) {
					return true;
				}
				manager->ChangeState<DirtySource::Cpu, true>(manager->GetCpuAddr() + offset, bytes, cause);
				return false;
			}();
			// Publication must include EVERY newly unprotected page. Never acquire
			// the BDA queue lock (or wait for the GPU) while holding the region lock.
			if constexpr (!std::is_same_v<Expanded, std::nullptr_t>) {
				if (expanded) on_expanded(manager->GetCpuAddr() + offset, bytes);
			}
			if constexpr (!std::is_same_v<Imported, std::nullptr_t>) {
				// Never wait/send to GuestGpu under the region lock. Its guard route
				// performs the CPU state change after readers have completed.
				if (imported) on_import(manager->GetCpuAddr() + offset, bytes);
			}
			if (should_flush) {
				on_flush();
			}
		});
	}
#if KYTY_BUILD == KYTY_BUILD_DEBUG
	void ValidateGpuDirtyPages(const RangeSet& dirty, uint64_t vaddr, uint64_t size,
	                           const char* operation) const noexcept;
	void ValidateGpuDirtyOwnership(const RangeSet& dirty, uint64_t vaddr, uint64_t size,
	                               const char* operation);
#else
	void ValidateGpuDirtyPages(const RangeSet&, uint64_t, uint64_t, const char*) const noexcept {}
	void ValidateGpuDirtyOwnership(const RangeSet&, uint64_t, uint64_t, const char*) {}
#endif

	template <bool clear, typename Func>
	void ForEachDownloadRange(uint64_t vaddr, uint64_t size, Func&& func) {
		static_assert(std::is_nothrow_invocable_v<Func&, uint64_t, uint64_t>);
		CheckNotInUploadCallback();
		Iterate<false>(vaddr, size, [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
			std::scoped_lock lock(manager->lock);
			const auto       address = manager->GetCpuAddr() + offset;
			manager->template ForEachModifiedRange<DirtySource::Gpu, false>(address, bytes, func);
			if constexpr (clear) {
				manager->template ChangeState<DirtySource::Gpu, false>(address, bytes);
			}
		});
	}

	template <typename RangeFunc, typename UploadFunc>
	void ForEachUploadRange(uint64_t vaddr, uint64_t size, bool is_written, RangeFunc&& range_func,
	                        UploadFunc&& upload_func) {
		static_assert(std::is_nothrow_invocable_v<RangeFunc&, uint64_t, uint64_t> ||
		              std::is_nothrow_invocable_v<RangeFunc&, uint64_t, uint64_t, const Common::BufferUploadProfile::Causes&>);
		static_assert(std::is_nothrow_invocable_v<UploadFunc&>);
		CheckNotInUploadCallback();
		const auto* previous_upload_owner = std::exchange(s_upload_owner, this);
		Iterate<true>(vaddr, size, [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
			manager->lock.lock();
			manager->ForEachModifiedRange<DirtySource::Cpu, true>(manager->GetCpuAddr() + offset,
			                                                      bytes, range_func);
			if (!is_written) {
				manager->lock.unlock();
			}
		});
		upload_func();
		if (is_written) {
			Iterate<false>(vaddr, size,
			               [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
				               manager->template ChangeState<DirtySource::Gpu, true>(
				                   manager->GetCpuAddr() + offset, bytes);
				               manager->lock.unlock();
			               });
		}
		s_upload_owner = previous_upload_owner;
	}

private:
	static constexpr size_t REGION_COUNT = TRACKER_ADDRESS_SIZE / TRACKER_REGION_SIZE;
	inline static thread_local const MemoryTracker* s_upload_owner = nullptr;

	void CheckNotInUploadCallback() const noexcept {
		if (s_upload_owner == this) {
			EXIT("memory tracker re-entered from upload callback\n");
		}
	}

	template <bool create, typename Func>
	bool Iterate(uint64_t vaddr, uint64_t size, Func&& func) {
		ValidateRange(vaddr, size);
		using Result = std::invoke_result_t<Func, RegionManager*, uint64_t, uint64_t>;
		constexpr bool returns_bool = std::is_same_v<Result, bool>;
		uint64_t       remaining    = size;
		uint64_t       index        = vaddr / TRACKER_REGION_SIZE;
		uint64_t       offset       = vaddr % TRACKER_REGION_SIZE;
		while (remaining != 0) {
			Common::GuestCpuProfile::Stage lookup(Common::GuestCpuProfile::Part::TrackerLookup);
			const auto bytes   = std::min(TRACKER_REGION_SIZE - offset, remaining);
			auto*      manager = m_regions[index].load(std::memory_order_acquire);
			if (manager == nullptr && create) {
				manager = GetOrCreateRegion(index);
			}
			lookup.End();
			if (manager != nullptr) {
				if constexpr (returns_bool) {
					if (func(manager, offset, bytes)) {
						return true;
					}
				} else {
					func(manager, offset, bytes);
				}
			}
			remaining -= bytes;
			offset = 0;
			index++;
		}
		return false;
	}

	static void    ValidateRange(uint64_t vaddr, uint64_t size);
	RegionManager* GetOrCreateRegion(uint64_t index);

	std::unique_ptr<std::atomic<RegionManager*>[]> m_regions;
	std::vector<std::unique_ptr<RegionManager>>    m_region_storage;
	std::mutex                                     m_region_mutex;
	PageManager&                                   m_page_manager;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORYTRACKER_H_
