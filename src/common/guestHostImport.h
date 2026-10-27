#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

// One guest address space. Lock order: registry -> address space -> backing ->
// tracker. Never call the GPU command lane or wait for a tick under this mutex.
namespace Common::GuestHostImport {
enum class Reject { None, Busy, VirtualOverlap, PhysicalOverlap, Unmapped, Discontiguous, GuestAlias };
struct Span {
	uint64_t address = 0, size = 0, physical_offset = 0, generation = 0;
	void* pointer = nullptr;
	Reject reject = Reject::Unmapped;
};
inline std::mutex mutex;
inline constexpr unsigned Publishing = 1u << 31;
inline std::atomic<unsigned> mutations {0};
inline std::atomic<uint64_t> generation {1};
inline std::vector<const Span*> live;
inline std::atomic<uint64_t> retained_bytes {0};
// Last profiled frame's union of pending page ranges, not cumulative byte counts.
inline std::atomic<uint64_t> pending_read_bytes {0}, pending_known_bytes {0}, pending_unknown_bytes {0};
inline std::atomic<bool> armed {false};
inline void* owner = nullptr;
inline void (*drain)(void*) = nullptr;
inline bool Overlap(uint64_t a, uint64_t n, uint64_t b, uint64_t m) {
	return n && m && (a <= b ? b - a < n : a - b < m);
}
inline bool IntersectsLocked(uint64_t address, uint64_t size) {
	return std::any_of(live.begin(), live.end(), [&](const Span* s) { return Overlap(address, size, s->address, s->size); });
}
class Lease {
public:
	explicit Lease(Span span): span(span) {}
	~Lease() {
		std::lock_guard lock(mutex);
		const auto found = std::find(live.begin(), live.end(), &span);
		if (found != live.end()) { live.erase(found); retained_bytes.fetch_sub(span.size); }
	}
	Lease(const Lease&) = delete;
	Lease& operator=(const Lease&) = delete;
	Span span;
};

// query executes under the registry lock, so a map/unmap cannot slip between
// resolving the backing and publishing the lease. It must not enter the GPU.
template <typename Query>
std::shared_ptr<Lease> Acquire(uint64_t address, uint64_t size, Query&& query, Reject* reason = nullptr) {
	std::lock_guard lock(mutex);
	const auto reject = [&](Reject why) -> std::shared_ptr<Lease> { if (reason) *reason = why; return {}; };
	if (!size || UINT64_MAX - address < size) return reject(Reject::Unmapped);
	if (!drain) return reject(Reject::Busy);
	if (IntersectsLocked(address, size)) return reject(Reject::VirtualOverlap);
	unsigned idle = 0;
	if (!mutations.compare_exchange_strong(idle, Publishing)) return reject(Reject::Busy);
	struct PublishingScope { ~PublishingScope() { mutations.fetch_sub(Publishing); } } publishing;
	Span span {address, size, 0, generation.load(), nullptr};
	if (!query(span)) return reject(span.reject);
	for (const auto* other: live)
		if (Overlap(span.physical_offset, size, other->physical_offset, other->size)) return reject(Reject::PhysicalOverlap);
	auto lease = std::make_shared<Lease>(span);
	live.push_back(&lease->span);
	retained_bytes.fetch_add(size);
	if (reason) *reason = Reject::None;
	return lease;
}

// Mapping/physical-reuse transactions block new imports BEFORE asking the GPU
// to retire old ones. The drain runs with no registry/backing lock held.
class Mutation {
public:
	Mutation() {
		if (!armed.load(std::memory_order_relaxed)) return;
		const auto previous = mutations.fetch_add(1);
		generation.fetch_add(1); active = true;
		// No leases and no concurrent publication: mapping takes no import lock
		// in A. Acquire's CAS cannot pass until this transaction has completed.
		if (!(previous & Publishing) && retained_bytes.load() == 0) return;
		void (*callback)(void*) = nullptr; void* context = nullptr;
		{
			std::lock_guard lock(mutex);
			if (!live.empty()) { callback = drain; context = owner; }
		}
		if (callback) callback(context);
	}
	~Mutation() { if (active) mutations.fetch_sub(1); }
	Mutation(const Mutation&) = delete;
	Mutation& operator=(const Mutation&) = delete;
private:
	bool active = false;
};
} // namespace Common::GuestHostImport
