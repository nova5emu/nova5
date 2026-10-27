#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RANGESET_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RANGESET_H_

#include "common/assert.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <utility>

namespace Libs::Graphics {

class RangeSet final {
public:
	void Add(uint64_t address, uint64_t size) {
		const auto end = End(address, size);
		auto       it  = m_ranges.lower_bound(address);
		if (it != m_ranges.begin() && std::prev(it)->second >= address) {
			it = std::prev(it);
		}
		uint64_t begin = address;
		uint64_t last  = end;
		while (it != m_ranges.end() && it->first <= last) {
			begin = std::min(begin, it->first);
			last  = std::max(last, it->second);
			it    = m_ranges.erase(it);
		}
		m_ranges.emplace(begin, last);
	}

	void Subtract(uint64_t address, uint64_t size) {
		const auto end = End(address, size);
		auto       it  = m_ranges.lower_bound(address);
		if (it != m_ranges.begin() && std::prev(it)->second > address) {
			it = std::prev(it);
		}
		while (it != m_ranges.end() && it->first < end) {
			const auto begin = it->first;
			const auto last  = it->second;
			it               = m_ranges.erase(it);
			if (begin < address) {
				m_ranges.emplace(begin, address);
			}
			if (last > end) {
				m_ranges.emplace(end, last);
				break;
			}
		}
	}

	void Clear() { m_ranges.clear(); }

	template <typename Func>
	void ForEach(Func&& func) const {
		for (const auto& [begin, end]: m_ranges) {
			func(begin, end);
		}
	}

	[[nodiscard]] bool Intersects(uint64_t address, uint64_t size) const {
		const auto end = End(address, size);
		auto       it  = m_ranges.lower_bound(address);
		if (it != m_ranges.begin() && std::prev(it)->second > address) {
			return true;
		}
		return it != m_ranges.end() && it->first < end;
	}

	[[nodiscard]] bool Contains(uint64_t address, uint64_t size) const {
		const auto end = End(address, size);
		auto       it  = m_ranges.upper_bound(address);
		if (it == m_ranges.begin()) {
			return false;
		}
		--it;
		return it->first <= address && it->second >= end;
	}

	template <typename Func>
	void ForEachInRange(uint64_t address, uint64_t size, Func&& func) const {
		const auto end = End(address, size);
		auto       it  = m_ranges.upper_bound(address);
		if (it != m_ranges.begin()) {
			--it;
		}
		for (; it != m_ranges.end() && it->first < end; ++it) {
			const auto begin = std::max(address, it->first);
			const auto last  = std::min(end, it->second);
			if (begin < last) {
				func(begin, last);
			}
		}
	}

	[[nodiscard]] bool Empty() const { return m_ranges.empty(); }

private:
	static uint64_t End(uint64_t address, uint64_t size) {
		if (size == 0 || size > UINT64_MAX - address) {
			EXIT("invalid range-set address or size\n");
		}
		return address + size;
	}

	std::map<uint64_t, uint64_t> m_ranges;
};

// Exclusion history only grows until a gate transition clears it. Positive
// containment proofs therefore survive later additions, including coalescing.
// Keep exact byte coverage: a page/block approximation could hide a new alias.
// GPU-lane only; deliberately offers no Subtract operation.
class ExclusionRanges final {
public:
	[[nodiscard]] bool ContainsCached(uint64_t address, uint64_t size) const {
		const auto& entry = m_cache[Index(address)];
		return size != 0 && entry.address == address && size <= entry.size;
	}
	[[nodiscard]] bool Contains(uint64_t address, uint64_t size) {
		if (ContainsCached(address, size)) return true;
		if (!m_ranges.Contains(address, size)) return false;
		Remember(address, size);
		return true;
	}
	void Add(uint64_t address, uint64_t size) {
		m_ranges.Add(address, size);
		Remember(address, size);
	}
	void Clear() { m_ranges.Clear(); m_cache = {}; }
	[[nodiscard]] bool Empty() const { return m_ranges.Empty(); }
	[[nodiscard]] bool Intersects(uint64_t address, uint64_t size) const { return m_ranges.Intersects(address, size); }
	template <typename Func>
	void ForEachInRange(uint64_t address, uint64_t size, Func&& func) const {
		m_ranges.ForEachInRange(address, size, std::forward<Func>(func));
	}
private:
	static size_t Index(uint64_t address) {
		// Fold upper bits before multiplying: guest images commonly share large
		// power-of-two alignment. Collisions only cause an ordinary tree lookup.
		return (((address >> 6) ^ (address >> 23)) * UINT64_C(0x9e3779b97f4a7c15)) >> 54;
	}
	void Remember(uint64_t address, uint64_t size) { m_cache[Index(address)] = {address, size}; }
	struct Entry { uint64_t address = 0, size = 0; };
	std::array<Entry, 1024> m_cache {};
	RangeSet m_ranges;
};

// Read references of ordered GPU submissions, including the recording tick.
// Caller serializes publication, completion pruning and CPU write queries.
class PendingRangeReads final {
public:
	struct Conflict { uint64_t tick = 0; bool known = false, unknown = false; };
	struct Bytes { uint64_t known = 0, unknown = 0, total = 0; };
	void Add(uint64_t tick, uint64_t address, uint64_t size, bool unknown = false) {
		if (!size) return;
		EXIT_IF(!m_batches.empty() && tick < m_batches.back().tick);
		if (m_batches.empty() || m_batches.back().tick != tick) m_batches.emplace_back(tick);
		auto& ranges = unknown ? m_batches.back().unknown : m_batches.back().known;
		if (!ranges.Contains(address, size)) ranges.Add(address, size);
	}
	void Complete(uint64_t tick) {
		while (!m_batches.empty() && m_batches.front().tick <= tick) m_batches.pop_front();
	}
	[[nodiscard]] Conflict Find(uint64_t address, uint64_t size) const {
		Conflict result;
		for (const auto& batch: m_batches) {
			const bool known = batch.known.Intersects(address, size);
			const bool unknown = batch.unknown.Intersects(address, size);
			if (known || unknown) result.tick = batch.tick;
			result.known |= known; result.unknown |= unknown;
		}
		return result;
	}
	// Diagnostic only. Union across ticks: repeated reads do not inflate bytes.
	[[nodiscard]] Bytes PendingBytes() const {
		RangeSet known, unknown, total;
		for (const auto& batch: m_batches) {
			batch.known.ForEach([&](uint64_t first, uint64_t last) { known.Add(first, last - first); total.Add(first, last - first); });
			batch.unknown.ForEach([&](uint64_t first, uint64_t last) { unknown.Add(first, last - first); total.Add(first, last - first); });
		}
		Bytes bytes;
		known.ForEach([&](uint64_t first, uint64_t last) { bytes.known += last - first; });
		unknown.ForEach([&](uint64_t first, uint64_t last) { bytes.unknown += last - first; });
		total.ForEach([&](uint64_t first, uint64_t last) { bytes.total += last - first; });
		return bytes;
	}
private:
	struct Batch {
		explicit Batch(uint64_t tick): tick(tick) {}
		uint64_t tick;
		RangeSet known, unknown;
	};
	std::deque<Batch> m_batches;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RANGESET_H_
