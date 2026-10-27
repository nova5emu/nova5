#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_

#include "common/assert.h"
#include "common/alignment.h"
#include "common/bufferUploadProfile.h"
#include "common/submitProfile.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/regionDefinitions.h"

#include <atomic>
#include <mutex>
#include <memory>
#include <type_traits>
#include <utility>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__)
#include <xmmintrin.h>
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#undef MemoryBarrier
#elif defined(__APPLE__)
#include <pthread.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Libs::Graphics {

class TrackingSpinLock final {
public:
	explicit TrackingSpinLock(Common::SubmitProfile::Diagnostics::Metric metric =
	                              Common::SubmitProfile::Diagnostics::Metric::TrackingLock): m_metric(metric) {}
	void lock() noexcept {
		Common::SubmitProfile::Diagnostics::LockTimer timer(m_metric);
		const auto thread = CurrentThread();
		while (m_lock.test_and_set(std::memory_order_acquire)) {
			EXIT_NOT_IMPLEMENTED(m_owner.load(std::memory_order_relaxed) == thread);
#if defined(__x86_64__) || defined(_M_X64)
			_mm_pause();
#elif defined(_M_ARM64)
			__yield();
#elif defined(__aarch64__)
			asm volatile("yield");
#endif
		}
		m_owner.store(thread, std::memory_order_relaxed);
		timer.Acquired();
	}
	void unlock() noexcept {
		EXIT_NOT_IMPLEMENTED(m_owner.load(std::memory_order_relaxed) != CurrentThread());
		m_owner.store(0, std::memory_order_relaxed);
		m_lock.clear(std::memory_order_release);
	}

private:
	static uint32_t CurrentThread() noexcept {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		return GetCurrentThreadId();
#elif defined(__APPLE__)
		// mach thread port is a nonzero per-thread id (0 is the "no owner" sentinel).
		static thread_local const uint32_t tid =
		    static_cast<uint32_t>(pthread_mach_thread_np(pthread_self()));
		return tid;
#elif defined(__linux__)
		static thread_local const uint32_t tid = static_cast<uint32_t>(::syscall(SYS_gettid));
		return tid;
#else
		EXIT("region tracking thread identity is unsupported on this platform\n");
#endif
	}

	std::atomic_flag     m_lock = ATOMIC_FLAG_INIT;
	std::atomic_uint32_t m_owner {0};
	Common::SubmitProfile::Diagnostics::Metric m_metric;
};

static_assert(std::atomic_uint32_t::is_always_lock_free);

class RegionManager final {
public:
	RegionManager(PageManager& page_manager, uint64_t cpu_addr)
	    : m_page_manager(page_manager), m_cpu_addr(cpu_addr) {
		if (m_cpu_addr % TRACKER_REGION_SIZE != 0) {
			EXIT("invalid region tracking manager construction\n");
		}
		m_cpu_dirty.Fill();
		m_writable.Fill();
		m_readable.Fill();
		if (Common::BufferUploadProfile::Enabled()) {
			m_upload_causes = std::make_unique<UploadCauses>();
			m_upload_causes->fill(Common::BufferUploadProfile::Cause::Initial);
		}
	}

	KYTY_CLASS_NO_COPY(RegionManager);

	[[nodiscard]] uint64_t GetCpuAddr() const { return m_cpu_addr; }
	void ObserveHotFault(uint64_t offset) {
		const auto page = offset / TRACKER_PAGE_SIZE;
		if (!m_hot || !m_hot->has_observed || !m_hot->observed.Get(page)) return;
		++m_hot->faults[page];
		if (m_coarsen && m_coarsen->capture) {
			auto& s = m_coarsen->stats;
			++s.faults;
			if (m_coarsen->seen_frame.Get(page)) ++s.repeat; else ++s.unique;
			if (m_coarsen->reprotected.Get(page)) ++s.refault_upload;
			m_coarsen->seen_frame.Set(page);
			m_coarsen->reprotected.Unset(page);
			m_coarsen->extra_dirty.Unset(page); // Actual write evidence for this page.
		}
	}
	uint64_t TakeHotFaults(uint64_t offset, uint64_t size, bool observe) {
		const auto [first, last] = GetPageRange(m_cpu_addr + offset, size);
		if (!m_hot && observe) m_hot = std::make_unique<HotState>();
		if (!m_hot) return 0;
		uint64_t count = 0;
		for (size_t i = first; i < last; ++i) count += std::exchange(m_hot->faults[i], 0);
		if (observe) m_hot->observed.SetRange(first, last); else m_hot->observed.UnsetRange(first, last);
		m_hot->has_observed = m_hot->observed.Any();
		return count;
	}
	void ConfigureFaultCoarsening(uint64_t offset, uint64_t size, uint64_t block_size, bool enabled, bool capture = true) {
		EXIT_IF(block_size != 64 * 1024 && block_size != 256 * 1024 && block_size != 1024 * 1024);
		if (!m_coarsen) m_coarsen = std::make_unique<CoarsenState>();
		m_coarsen->block_size = block_size;
		m_coarsen->capture = capture;
		const auto [first, last] = GetPageRange(m_cpu_addr + offset, size);
		m_coarsen->eligible.UnsetRange(first, last);
		if (enabled) {
			// Only whole aligned blocks wholly inside this owner/region. Edge
			// pages retain 4 KiB granularity; neighboring owners never merge.
			const auto begin = Common::AlignUp(offset, block_size);
			const auto end = Common::AlignDown(offset + size, block_size);
			if (begin < end) m_coarsen->eligible.SetRange(begin / TRACKER_PAGE_SIZE, end / TRACKER_PAGE_SIZE);
		}
	}
	FaultCoarsenStats TakeCoarsenStats(uint64_t epoch = 0) {
		if (!m_coarsen || !m_coarsen->capture) return {};
		if (epoch && m_coarsen->report_epoch == epoch) return {};
		m_coarsen->report_epoch = epoch;
		m_coarsen->seen_frame.Clear();
		m_coarsen->reprotected.Clear();
		return std::exchange(m_coarsen->stats, {});
	}
	bool CoarsenFault(uint64_t& offset, uint64_t& size, bool allow) {
		if (!m_coarsen || size != 1 || !m_coarsen->eligible.Get(offset / TRACKER_PAGE_SIZE) ||
		    !Common::SubmitProfile::ExperimentActive()) return false;
		auto& s = m_coarsen->stats;
		if (!allow) { if (m_coarsen->capture) ++s.reject_readback; return false; }
		const auto begin = Common::AlignDown(offset, m_coarsen->block_size);
		const auto bytes = m_coarsen->block_size;
		if (IsModified<DirtySource::Gpu>(begin, bytes)) { if (m_coarsen->capture) ++s.reject_gpu; return false; }
		if (HasHostImport(begin, bytes)) { if (m_coarsen->capture) ++s.reject_import; return false; }
		const auto first = begin / TRACKER_PAGE_SIZE, last = (begin + bytes) / TRACKER_PAGE_SIZE;
		if (m_page_manager.HasForeignWatchers(m_cpu_addr, first, last, m_writable)) {
			if (m_coarsen->capture) ++s.reject_watcher;
			return false;
		}
		const auto fault_page = offset / TRACKER_PAGE_SIZE;
		uint64_t extra = 0;
		for (auto i = first; m_coarsen->capture && i < last; ++i) {
			if (m_cpu_dirty.Get(i)) continue;
			if (i != fault_page) { m_coarsen->extra_dirty.Set(i); extra += TRACKER_PAGE_SIZE; }
		}
		if (m_coarsen->capture) { ++s.expanded; s.extra_dirty_upper += extra; }
		offset = begin; size = bytes;
		return true;
	}
	bool CanUseHot(uint64_t offset, uint64_t size) {
		const auto [first, last] = GetPageRange(m_cpu_addr + offset, size);
		return !IsModified<DirtySource::Gpu>(offset, size) && !HasHostImport(offset, size) &&
		    !m_page_manager.HasForeignWatchers(m_cpu_addr, first, last, m_writable);
	}
	void SetHot(uint64_t offset, uint64_t size, bool enabled) {
		const auto [first, last] = GetPageRange(m_cpu_addr + offset, size);
		if (enabled) {
			EXIT_IF(IsModified<DirtySource::Gpu>(offset, size) || HasHostImport(offset, size));
			if (!m_hot) m_hot = std::make_unique<HotState>();
			m_hot->pages.SetRange(first, last);
			m_hot->reprotect.UnsetRange(first, last);
			m_hot->has_pages = true; m_hot->has_reprotect = m_hot->reprotect.Any();
			m_cpu_dirty.SetRange(first, last);
			UpdateProtection<false, false>();
		} else if (m_hot) {
			// Keep a forced upload AND re-arm write watching. A fault after this
			// point returns that page to the ordinary dirty/writable state.
			const RegionBits retiring(m_hot->pages, first, last);
			m_cpu_dirty |= retiring;
			m_hot->reprotect |= retiring;
			m_hot->pages.UnsetRange(first, last);
			m_hot->has_pages = m_hot->pages.Any(); m_hot->has_reprotect = m_hot->reprotect.Any();
			UpdateProtection<true, false>();
		}
	}
	// Accessed under the existing region lock, including publication/removal.
	// A has no mask: faults neither acquire an import mutex nor search a range map.
	bool HasHostImport(uint64_t offset, uint64_t size) const {
		if (!m_host_import) return false;
		const auto [first, last] = GetPageRange(m_cpu_addr + offset, size);
		return m_host_import->FirstRangeFrom(first).first < last;
	}
	void SetHostImport(uint64_t offset, uint64_t size, bool enabled) {
		const auto [first, last] = GetPageRange(m_cpu_addr + offset, size);
		if (enabled) {
			if (!m_host_import) m_host_import = std::make_unique<RegionBits>();
			m_host_import->SetRange(first, last);
		} else if (m_host_import) {
			m_host_import->UnsetRange(first, last);
			if (m_host_import->None()) m_host_import.reset();
		}
	}
	template <DirtySource source>
	[[nodiscard]] bool IsModified(uint64_t offset, uint64_t size) const {
		const auto [start, end] = GetPageRange(m_cpu_addr + offset, size);
		return GetBits<source>().FirstRangeFrom(start).first < end;
	}

	template <DirtySource source, bool enable>
	void ChangeState(uint64_t vaddr, uint64_t size,
	                 Common::BufferUploadProfile::Cause cause = Common::BufferUploadProfile::Cause::Invalidate) {
		const auto [start, end] = GetPageRange(vaddr, size);
		if constexpr (source == DirtySource::Cpu && enable) {
			if (m_coarsen && m_coarsen->capture && m_hot && m_hot->has_observed && cause == Common::BufferUploadProfile::Cause::CpuWriteFault) {
				const RegionBits observed(m_hot->observed, start, end);
				m_coarsen->stats.dirty_bytes += observed.Count() * TRACKER_PAGE_SIZE;
				m_coarsen->stats.new_dirty_bytes += (observed & ~m_cpu_dirty).Count() * TRACKER_PAGE_SIZE;
			}
			if (m_coarsen && m_coarsen->capture && cause != Common::BufferUploadProfile::Cause::CpuWriteFault)
				m_coarsen->extra_dirty.UnsetRange(start, end);
			if (m_hot && m_hot->has_reprotect) {
				m_hot->reprotect.UnsetRange(start, end); m_hot->has_reprotect = m_hot->reprotect.Any();
			}
			if (RegionBits(m_gpu_dirty, start, end).Any()) {
				EXIT("CPU dirty state conflicts with GPU dirty state\n");
			}
			if (m_upload_causes) std::fill(m_upload_causes->begin() + start, m_upload_causes->begin() + end, cause);
		}
		if constexpr (source == DirtySource::Gpu && enable) {
			EXIT_IF(m_hot && m_hot->has_pages && RegionBits(m_hot->pages, start, end).Any());
			if (RegionBits(m_cpu_dirty, start, end).Any()) {
				EXIT("GPU dirty state conflicts with CPU dirty state\n");
			}
		}
		auto& bits = GetBits<source>();
		if constexpr (enable) {
			bits.SetRange(start, end);
		} else {
			bits.UnsetRange(start, end);
		}
		if constexpr (source == DirtySource::Cpu) {
			if (m_hot && m_hot->has_pages) m_cpu_dirty |= m_hot->pages;
			UpdateProtection<!enable, false>();
		} else {
			UpdateProtection<enable, true>();
		}
	}

	template <DirtySource source, bool clear, typename Func>
	void ForEachModifiedRange(uint64_t vaddr, uint64_t size, Func&& func) {
		const auto [start, end] = GetPageRange(vaddr, size);
		auto&      bits         = GetBits<source>();
		if (bits.FirstRangeFrom(start).first >= end) {
			return;
		}
		RegionBits mask(bits, start, end);
		if constexpr (clear) {
			if constexpr (source == DirtySource::Cpu) {
				if (m_coarsen && m_coarsen->capture && m_hot && m_hot->has_observed) {
					const auto observed = mask & m_hot->observed;
					m_coarsen->stats.upload_bytes += observed.Count() * TRACKER_PAGE_SIZE;
					m_coarsen->stats.extra_upload_upper += (observed & m_coarsen->extra_dirty).Count() * TRACKER_PAGE_SIZE;
					m_coarsen->reprotected |= observed & m_coarsen->seen_frame;
				}
				if (m_coarsen && m_coarsen->capture) m_coarsen->extra_dirty.UnsetRange(start, end);
			}
			bits.UnsetRange(start, end);
			if constexpr (source == DirtySource::Cpu) {
				if (m_hot) {
					if (m_hot->has_pages) bits |= m_hot->pages;
					if (m_hot->has_reprotect) {
						m_hot->reprotect.UnsetRange(start, end); m_hot->has_reprotect = m_hot->reprotect.Any();
					}
				}
				UpdateProtection<true, false>();
			} else {
				UpdateProtection<false, true>();
			}
		}
		for (const auto [first, last]: mask) {
			if constexpr (source == DirtySource::Cpu &&
			              std::is_invocable_v<Func&, uint64_t, uint64_t, const Common::BufferUploadProfile::Causes&>) {
				Common::BufferUploadProfile::Causes causes {};
				if (m_upload_causes)
					for (size_t page = first; page < last; ++page)
						causes[static_cast<size_t>((*m_upload_causes)[page])] += TRACKER_PAGE_SIZE;
				func(m_cpu_addr + first * TRACKER_PAGE_SIZE, (last - first) * TRACKER_PAGE_SIZE, causes);
			} else {
				func(m_cpu_addr + first * TRACKER_PAGE_SIZE, (last - first) * TRACKER_PAGE_SIZE);
			}
		}
	}

	TrackingSpinLock lock;

private:
	template <bool track, bool is_read>
	void UpdateProtection() {
		const auto protection = is_read ? ~m_gpu_dirty : (m_hot && m_hot->has_reprotect ? m_cpu_dirty & ~m_hot->reprotect : m_cpu_dirty);
		auto&      previous   = is_read ? m_readable : m_writable;
		auto       mask       = protection ^ previous;
		if (mask.None()) {
			return;
		}
		previous = protection;
		m_page_manager.UpdatePageWatchersForRegion<track, is_read>(m_cpu_addr, mask);
	}

	template <DirtySource source>
	RegionBits& GetBits() {
		if constexpr (source == DirtySource::Cpu) {
			return m_cpu_dirty;
		} else {
			return m_gpu_dirty;
		}
	}

	template <DirtySource source>
	const RegionBits& GetBits() const {
		if constexpr (source == DirtySource::Cpu) {
			return m_cpu_dirty;
		} else {
			return m_gpu_dirty;
		}
	}

	[[nodiscard]] std::pair<size_t, size_t> GetPageRange(uint64_t vaddr, uint64_t size) const {
		if (size == 0 || vaddr < m_cpu_addr || vaddr >= m_cpu_addr + TRACKER_REGION_SIZE ||
		    size > m_cpu_addr + TRACKER_REGION_SIZE - vaddr) {
			EXIT("range lies outside its tracking region\n");
		}
		const auto offset = vaddr - m_cpu_addr;
		return {static_cast<size_t>(offset / TRACKER_PAGE_SIZE),
		        static_cast<size_t>((offset + size + TRACKER_PAGE_SIZE - 1) / TRACKER_PAGE_SIZE)};
	}

	PageManager& m_page_manager;
	uint64_t     m_cpu_addr = 0;
	RegionBits   m_cpu_dirty;
	RegionBits   m_gpu_dirty;
	RegionBits   m_writable;
	RegionBits   m_readable;
	std::unique_ptr<RegionBits> m_host_import;
	struct HotState {
		RegionBits pages, reprotect, observed;
		bool has_pages = false, has_reprotect = false, has_observed = false;
		std::array<uint64_t, TRACKER_REGION_PAGES> faults {};
	};
	std::unique_ptr<HotState> m_hot;
	struct CoarsenState {
		bool capture = true;
		uint64_t block_size = 64 * 1024;
		uint64_t report_epoch = 0;
		RegionBits eligible, extra_dirty, seen_frame, reprotected;
		FaultCoarsenStats stats;
	};
	std::unique_ptr<CoarsenState> m_coarsen;
	using UploadCauses = std::array<Common::BufferUploadProfile::Cause, TRACKER_REGION_SIZE / TRACKER_PAGE_SIZE>;
	std::unique_ptr<UploadCauses> m_upload_causes;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_
