#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DRAWPREP_READSET_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DRAWPREP_READSET_H_

// Value certificates adapted from Jetsku 0adb8669; no coherence-log shortcut.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <span>
#include <type_traits>
#include <vector>

// The read set of one speculative draw preparation, and its certificate check.
//
// A preparation reads guest memory only through ReadMemory and a nonblocking backing probe.
// The probe need not prove GPU ownership: speculative bytes never reach a draw without the
// CP's clean-value check below. Unmapped, busy or oversized reads refuse the preparation;
// there is no guest-pointer fallback, GPU readback or cache mutation on a worker.
//
// At commit, on the GPU thread, Validate() re-reads every coalesced range with the coherent
// clean-backing read and compares it with the recorded bytes. Correctness argument: the
// preparation is a deterministic function of (register snapshot, shader map generation, the
// values returned by its reads, validated program-cache publication). If every range is clean for a
// backing read now and holds the recorded bytes, then the serial path run now would issue the same
// first read (same registers), get the same value, and by induction issue the same reads in the
// same order and compute the same outputs. Guest bytes that changed and changed back in between
// are indistinguishable from unchanged ones, which is exactly what the serial path would observe.
namespace Libs::Graphics::DrawPrep {

struct ReadRange { uint64_t begin = 0, end = 0; };

enum class ReadFailure : uint8_t {
	None,
	Unclean,      // not provably clean
	Backing,      // no backing translation
	Overflow,     // too many reads or bytes to certify
	Inconsistent, // two reads of the same byte returned different values
	Uncertified,  // the preparation needed a read or a decision outside the certificate
};

enum class ValidateResult : uint8_t {
	Ok,
	Unclean, // a range is not clean for a backing read now
	Changed, // a range is clean but its bytes differ from the recorded ones
};

class ReadSet {
public:
	static constexpr uint32_t MaxReads = 2048;
	static constexpr uint32_t MaxBytes = 64u * 1024u;

	void Reset() noexcept {
		m_reads.clear();
		m_bytes.clear();
		m_ranges.clear();
		m_range_offsets.clear();
		m_merged.clear();
		m_failure  = ReadFailure::None;
		m_finished = false;
	}

	// Records `size` bytes read at `address`. False (and a failure) when over the limits.
	bool Record(uint64_t address, const void* data, uint64_t size) {
		m_finished = false;
		if (m_failure != ReadFailure::None) {
			return false;
		}
		if (size == 0) {
			return true;
		}
		if (m_reads.size() >= MaxReads || size > MaxBytes - m_bytes.size() ||
		    address > UINT64_MAX - size) {
			Fail(ReadFailure::Overflow);
			return false;
		}
		const auto offset = static_cast<uint32_t>(m_bytes.size());
		m_bytes.resize(m_bytes.size() + size);
		std::memcpy(m_bytes.data() + offset, data, size);
		m_reads.push_back({address, static_cast<uint32_t>(size), offset});
		return true;
	}

	void Fail(ReadFailure failure) noexcept {
		if (m_failure == ReadFailure::None) {
			m_failure = failure;
		}
	}
	[[nodiscard]] ReadFailure Failure() const noexcept { return m_failure; }
	[[nodiscard]] bool        Failed() const noexcept { return m_failure != ReadFailure::None; }
	[[nodiscard]] size_t      ReadCount() const noexcept { return m_reads.size(); }
	[[nodiscard]] size_t      ByteCount() const noexcept { return m_bytes.size(); }

	// Coalesces recorded reads; touching reads merge only within a 4 KiB page.
	// Overlapping reads must agree on every shared byte, otherwise the preparation observed a
	// concurrent change and fails (Inconsistent).
	bool Finish() {
		if (m_failure != ReadFailure::None) {
			return false;
		}
		m_order.resize(m_reads.size());
		std::iota(m_order.begin(), m_order.end(), 0u);
		std::sort(m_order.begin(), m_order.end(), [this](uint32_t a, uint32_t b) {
			return m_reads[a].address < m_reads[b].address ||
			       (m_reads[a].address == m_reads[b].address && m_reads[a].size > m_reads[b].size);
		});
		m_ranges.clear();
		m_range_offsets.clear();
		m_merged.clear();
		for (const auto index: m_order) {
			const auto& read  = m_reads[index];
			const auto* bytes = m_bytes.data() + read.offset;
			const auto  end   = read.address + read.size;
			if (!m_ranges.empty() && (read.address < m_ranges.back().end ||
			    (read.address == m_ranges.back().end && (read.address & 4095u) != 0))) {
				auto&      range       = m_ranges.back();
				const auto base        = m_range_offsets.back();
				const auto overlap_end = std::min(end, range.end);
				if (overlap_end > read.address &&
				    std::memcmp(m_merged.data() + base + (read.address - range.begin), bytes,
				                overlap_end - read.address) != 0) {
					Fail(ReadFailure::Inconsistent);
					return false;
				}
				if (end > range.end) {
					const auto tail = end - range.end;
					m_merged.insert(m_merged.end(), bytes + (range.end - read.address),
					                bytes + (range.end - read.address) + tail);
					range.end = end;
				}
				continue;
			}
			m_ranges.push_back({read.address, end});
			m_range_offsets.push_back(static_cast<uint32_t>(m_merged.size()));
			m_merged.insert(m_merged.end(), bytes, bytes + read.size);
		}
		m_finished = true;
		return true;
	}

	[[nodiscard]] bool Finished() const noexcept { return m_finished; }
	// Sorted, non-overlapping; valid after a successful Finish().
	[[nodiscard]] std::span<const ReadRange> Ranges() const noexcept { return m_ranges; }
	[[nodiscard]] std::span<const uint8_t> RangeBytes(size_t index) const noexcept {
		const auto& range = m_ranges[index];
		return {m_merged.data() + m_range_offsets[index], range.end - range.begin};
	}

	// read(address, destination, size) -> bool: the coherent clean-backing read of the caller.
	template <typename Read>
	[[nodiscard]] ValidateResult Validate(Read&& read, std::vector<uint8_t>& scratch) const {
		static_assert(std::is_invocable_r_v<bool, Read&, uint64_t, void*, uint64_t>);
		if (!m_finished || Failed()) return ValidateResult::Unclean;
		for (size_t index = 0; index < m_ranges.size(); index++) {
			const auto bytes = RangeBytes(index);
			scratch.resize(bytes.size());
			if (!read(m_ranges[index].begin, scratch.data(), static_cast<uint64_t>(bytes.size()))) {
				return ValidateResult::Unclean;
			}
			if (std::memcmp(scratch.data(), bytes.data(), bytes.size()) != 0) {
				return ValidateResult::Changed;
			}
		}
		return ValidateResult::Ok;
	}

	// is_clean(address, size) -> bool: the clean verdict alone (no bytes compared).
	template <typename IsClean>
	[[nodiscard]] bool AllClean(IsClean&& is_clean) const {
		static_assert(std::is_invocable_r_v<bool, IsClean&, uint64_t, uint64_t>);
		for (const auto& range: m_ranges) {
			if (!is_clean(range.begin, range.end - range.begin)) {
				return false;
			}
		}
		return true;
	}

private:
	struct Read {
		uint64_t address = 0;
		uint32_t size    = 0;
		uint32_t offset  = 0;
	};

	std::vector<Read>             m_reads;
	std::vector<uint8_t>          m_bytes;
	std::vector<uint32_t>         m_order;
	std::vector<ReadRange> m_ranges;
	std::vector<uint32_t>         m_range_offsets;
	std::vector<uint8_t>          m_merged;
	ReadFailure                   m_failure  = ReadFailure::None;
	bool                          m_finished = false;
};

// The same probe-only reader on a worker and in inline mode. Only commit checks
// exact GPU ownership, on the CP thread that owns the resource caches.
struct Recorder {
	ReadSet* reads = nullptr;
	bool (*probe)(uint64_t, void*, uint64_t) = nullptr;
};

inline thread_local Recorder* t_recorder = nullptr;
// The serial oracle must not reuse a periodically refreshed shader hash while
// the speculative side certifies today's header bytes.
inline thread_local bool t_fresh_shader_hash = false;
class FreshHashScope {
public:
	explicit FreshHashScope(bool enabled): previous(t_fresh_shader_hash) { t_fresh_shader_hash = enabled || previous; }
	~FreshHashScope() { t_fresh_shader_hash = previous; }
	FreshHashScope(const FreshHashScope&) = delete;
	FreshHashScope& operator=(const FreshHashScope&) = delete;
private:
	bool previous;
};

[[nodiscard]] inline Recorder* ActiveRecorder() noexcept {
	return t_recorder;
}

// True while a draw preparation runs on this thread: guest reads must not fall back to the
// guest mapping (they fail the preparation instead).
[[nodiscard]] inline bool Speculative() noexcept {
	return t_recorder != nullptr;
}

// Marks the active preparation as failed (no-op outside a preparation).
inline void FailActive(ReadFailure failure) noexcept {
	if (t_recorder != nullptr) {
		t_recorder->reads->Fail(failure);
	}
}

[[nodiscard]] inline bool SpeculativeFailed() noexcept {
	return t_recorder != nullptr && t_recorder->reads->Failed();
}
// No guest-pointer fallback, cache access, synchronization or unbounded read.
inline bool ReadMemory(uint64_t address, void* data, uint64_t size) {
	auto* recorder = ActiveRecorder();
	if (!recorder || recorder->reads->Failed()) return false;
	if (!data || !size || size > ReadSet::MaxBytes || address > UINT64_MAX - size) {
		recorder->reads->Fail(ReadFailure::Overflow); return false;
	}
	if (!recorder->probe || !recorder->probe(address, data, size)) {
		recorder->reads->Fail(ReadFailure::Backing); return false;
	}
	return recorder->reads->Record(address, data, size);
}

class RecordScope {
public:
	explicit RecordScope(Recorder& recorder) noexcept: m_previous(t_recorder) {
		t_recorder = &recorder;
	}
	~RecordScope() { t_recorder = m_previous; }
	RecordScope(const RecordScope&)            = delete;
	RecordScope& operator=(const RecordScope&) = delete;

private:
	Recorder* m_previous;
};

} // namespace Libs::Graphics::DrawPrep

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DRAWPREP_READSET_H_
