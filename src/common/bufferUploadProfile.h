#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>

namespace Common::BufferUploadProfile {

inline bool Enabled() {
	static const bool enabled = [] {
		const auto* value = std::getenv("KYTY_BUFFER_UPLOAD_PROFILE");
		return value && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

enum class Use : uint8_t { Unknown, Vertex, Index, Indirect, ShaderScalarRead,
	ShaderRead, ShaderFormattedRead, ShaderWrite, Image, Copy, Fill, RangeSync, Count };
inline constexpr std::array use_names {"unknown", "vertex", "index", "indirect", "shader_scalar_read",
	"shader_read", "shader_formatted_read", "shader_write", "image", "copy", "fill", "range_sync"};
// Last observed operation that marked the CPU page dirty. A write fault does NOT
// reveal how many bytes the guest subsequently wrote while the page was writable.
enum class Cause : uint8_t { Initial, Invalidate, CpuWriteFault, Untrack,
	AfterGpuDownload, CpuWriteFaultAfterGpuDownload, Count };
inline constexpr std::array cause_names {"initial", "invalidate", "cpu_write_fault", "untrack",
	"after_gpu_download", "cpu_write_fault_after_gpu_download"};
using Causes = std::array<uint64_t, static_cast<size_t>(Cause::Count)>;
inline Cause AfterDownload(Cause cause) {
	return cause == Cause::CpuWriteFault ? Cause::CpuWriteFaultAfterGpuDownload : Cause::AfterGpuDownload;
}

inline uint64_t Mix(uint64_t x) {
	x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
	x ^= x >> 27; x *= 0x94d049bb133111ebULL;
	return x ^ (x >> 31);
}
inline uint64_t Outside(uint64_t address, uint64_t bytes, uint64_t requested, uint64_t size) {
	const auto start = std::max(address, requested);
	const auto end = std::min(address + bytes, requested + size);
	return bytes - (end > start ? end - start : 0);
}

template <size_t Slots = 16384, size_t PageBytes = 4096>
class PageHistory {
public:
	struct Result { bool comparable = false, equal = false, evicted = false; uint64_t changed = 0; };
	Result Observe(uint64_t address, const uint8_t* data) {
		auto& slot = slots[Mix(address / PageBytes) % Slots];
		Result result;
		result.comparable = slot.valid && slot.address == address;
		result.evicted = slot.valid && !result.comparable;
		if (result.comparable) {
			result.equal = std::memcmp(slot.data.data(), data, PageBytes) == 0;
			if (!result.equal)
				for (size_t i = 0; i < PageBytes; ++i) result.changed += slot.data[i] != data[i];
		}
		std::memcpy(slot.data.data(), data, PageBytes);
		slot.address = address; slot.valid = true;
		return result;
	}
	void Reset() { for (auto& slot: slots) slot.valid = false; }
private:
	struct Slot { uint64_t address = 0; bool valid = false; std::array<uint8_t, PageBytes> data; };
	std::array<Slot, Slots> slots {};
};

template <size_t Slots = 16384>
class RangeHistory {
public:
	struct Result { bool comparable, hash_equal, evicted; };
	Result Observe(uint64_t address, uint64_t bytes, uint64_t hash) {
		auto& slot = slots[Mix(address ^ Mix(bytes)) % Slots];
		const bool comparable = slot.valid && slot.address == address && slot.bytes == bytes;
		const Result result {comparable, comparable && slot.hash == hash, slot.valid && !comparable};
		slot = {address, bytes, hash, true};
		return result; // Hash match is labelled as such; page snapshots provide exact equality.
	}
	void Reset() { for (auto& slot: slots) slot.valid = false; }
private:
	struct Slot { uint64_t address = 0, bytes = 0, hash = 0; bool valid = false; };
	std::array<Slot, Slots> slots {};
};

struct Counts {
	uint64_t uploads = 0, bytes = 0, outside_request_bytes = 0;
	uint64_t hash_comparable_bytes = 0, hash_equal_bytes = 0, range_evictions = 0;
	uint64_t page_comparable_bytes = 0, page_equal_bytes = 0, changed_bytes = 0, page_evictions = 0;
	Causes causes {};
	void Add(const Counts& other) {
#define UP_ADD(field) field += other.field
		UP_ADD(uploads); UP_ADD(bytes); UP_ADD(outside_request_bytes);
		UP_ADD(hash_comparable_bytes); UP_ADD(hash_equal_bytes); UP_ADD(range_evictions);
		UP_ADD(page_comparable_bytes); UP_ADD(page_equal_bytes); UP_ADD(changed_bytes); UP_ADD(page_evictions);
#undef UP_ADD
		for (size_t i = 0; i < causes.size(); ++i) causes[i] += other.causes[i];
	}
};

struct Request {
	uint64_t address = 0, size = 0, buffer_address = 0, buffer_size = 0;
	Use use = Use::Unknown;
	bool written = false, texel = false;
};

// GPU-thread owned. No locks, guest reads or report I/O in Observe. Storage is
// allocated once, before entering MemoryTracker's upload transaction.
class State {
public:
	static constexpr size_t ScratchBytes = 4 * 1024 * 1024;
	explicit State(std::FILE* output = nullptr): file(output) {
		if (file) return; // Ownership transferred by the diagnostic test harness.
		const auto* path = std::getenv("KYTY_BUFFER_UPLOAD_PROFILE_FILE");
		file = std::fopen(path && *path ? path : "buffer-upload-profile.jsonl", "w");
		if (!file) std::fprintf(stderr, "[BUFFER_UPLOAD_PROFILE] cannot open output file; capture disabled\n");
	}
	~State() { if (file) { Report(false); if (file) std::fclose(file); } }
	bool Ready() const { return file != nullptr; }
	uint8_t* Scratch() { return scratch.data(); }
	void Sync(bool mode) {
		if (started && persistent == mode) return;
		if (started) Report(false);
		ResetCounts(); pages.Reset(); ranges.Reset();
		persistent = mode; started = true; complete = false; ++segment;
	}
	void Frame(bool mode) {
		Sync(mode); Report(complete); ResetCounts(); complete = true; ++frame;
	}
	void Observe(const Request& request, uint64_t address, std::span<const uint8_t> data,
	             uint64_t hash, const Causes& causes) {
		Counts c;
		c.uploads = 1; c.bytes = data.size(); c.causes = causes;
		c.outside_request_bytes = Outside(address, data.size(), request.address, request.size);
		const auto previous = ranges.Observe(address, data.size(), hash);
		c.hash_comparable_bytes = previous.comparable ? data.size() : 0;
		c.hash_equal_bytes = previous.hash_equal ? data.size() : 0;
		c.range_evictions = previous.evicted;
		// Upload ranges come from the 4 KiB dirty-page tracker. Refuse partial
		// pages for exact comparison rather than read beyond the copied bytes.
		if (address % 4096 == 0 && data.size() % 4096 == 0) {
			for (size_t offset = 0; offset < data.size(); offset += 4096) {
				const auto p = pages.Observe(address + offset, data.data() + offset);
				c.page_comparable_bytes += p.comparable ? 4096 : 0;
				c.page_equal_bytes += p.equal ? 4096 : 0;
				c.changed_bytes += p.changed; c.page_evictions += p.evicted;
			}
		}
		total.Add(c); uses[static_cast<size_t>(request.use)].Add(c);
		const auto owner_slot = Mix(request.buffer_address ^ Mix(request.buffer_size)) % owners.size();
		bool owner_recorded = false;
		for (size_t probe = 0; probe < 32; ++probe) {
			auto& owner = owners[(owner_slot + probe) % owners.size()];
			if (!owner.valid) {
				owner.valid = true; owner.address = request.buffer_address; owner.size = request.buffer_size;
			}
			if (owner.address == request.buffer_address && owner.size == request.buffer_size) {
				owner.counts.Add(c); owner.use_mask |= 1ULL << static_cast<size_t>(request.use);
				owner_recorded = true; break;
			}
		}
		if (!owner_recorded) owner_dropped_bytes += data.size();
		size_t bucket = 0;
		while (bucket < 5 && data.size() > (4096ULL << (2 * bucket))) ++bucket;
		sizes[bucket].Add(c);
		const auto slot = Mix(address ^ Mix(data.size()) ^ Mix(request.buffer_address) ^
		                      Mix(request.buffer_size) ^ static_cast<uint64_t>(request.use)) % rows.size();
		for (size_t probe = 0; probe < 32; ++probe) {
			auto& row = rows[(slot + probe) % rows.size()];
			if (!row.valid) {
				row.valid = true; row.address = address; row.bytes = data.size(); row.request = request;
				row.min_request = row.max_request = request.size;
			}
			if (row.address == address && row.bytes == data.size() && row.request.use == request.use &&
			    row.request.buffer_address == request.buffer_address && row.request.buffer_size == request.buffer_size &&
			    row.request.written == request.written && row.request.texel == request.texel) {
				row.counts.Add(c); row.min_request = std::min(row.min_request, request.size);
				row.max_request = std::max(row.max_request, request.size); return;
			}
		}
		dropped_bytes += data.size(); ++dropped_ranges;
	}

private:
	struct Owner {
		bool valid = false;
		uint64_t address = 0, size = 0, use_mask = 0;
		Counts counts;
	};
	struct Row {
		bool valid = false;
		uint64_t address = 0, bytes = 0, min_request = 0, max_request = 0;
		Request request;
		Counts counts;
	};
	void WriteCounts(const Counts& c) {
		std::fputs("{", file);
#define UP_PRINT(field) std::fprintf(file, "\"" #field "\":%llu,", static_cast<unsigned long long>(c.field))
		UP_PRINT(uploads); UP_PRINT(bytes); UP_PRINT(outside_request_bytes);
		UP_PRINT(hash_comparable_bytes); UP_PRINT(hash_equal_bytes); UP_PRINT(range_evictions);
		UP_PRINT(page_comparable_bytes); UP_PRINT(page_equal_bytes); UP_PRINT(changed_bytes); UP_PRINT(page_evictions);
#undef UP_PRINT
		std::fputs("\"cause_bytes\":{", file);
		for (size_t i = 0; i < c.causes.size(); ++i)
			std::fprintf(file, "%s\"%s\":%llu", i ? "," : "", cause_names[i], static_cast<unsigned long long>(c.causes[i]));
		std::fputs("}}", file);
	}
	void Report(bool full) {
		if (!file || !started) return;
		const auto time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		    std::chrono::system_clock::now().time_since_epoch()).count();
		std::fprintf(file, "{\"frame\":%llu,\"segment\":%llu,\"persistent\":%u,\"complete\":%u,\"unix_ms\":%llu,\"total\":",
		    static_cast<unsigned long long>(frame), static_cast<unsigned long long>(segment), unsigned(persistent), unsigned(full),
		    static_cast<unsigned long long>(time_ms));
		WriteCounts(total);
		std::fputs(",\"uses\":{", file);
		for (size_t i = 0; i < uses.size(); ++i) {
			std::fprintf(file, "%s\"%s\":", i ? "," : "", use_names[i]); WriteCounts(uses[i]);
		}
		std::fputs("},\"copy_sizes\":[", file);
		for (size_t i = 0; i < sizes.size(); ++i) { if (i) std::fputc(',', file); WriteCounts(sizes[i]); }
		size_t used = 0;
		for (size_t i = 0; i < rows.size(); ++i) if (rows[i].valid) order[used++] = i;
		const auto shown = std::min<size_t>(32, used);
		std::partial_sort(order.begin(), order.begin() + shown, order.begin() + used,
		    [&](size_t a, size_t b) { return rows[a].counts.bytes > rows[b].counts.bytes; });
		uint64_t shown_bytes = 0;
		std::fputs("],\"top_ranges\":[", file);
		for (size_t i = 0; i < shown; ++i) {
			const auto& r = rows[order[i]]; shown_bytes += r.counts.bytes;
			std::fprintf(file, "%s{\"address\":\"0x%llx\",\"copy_size\":%llu,\"buffer_address\":\"0x%llx\","
			    "\"buffer_size\":%llu,\"first_request_address\":\"0x%llx\",\"min_request_size\":%llu,\"max_request_size\":%llu,"
			    "\"use\":\"%s\",\"written\":%u,\"texel\":%u,\"counts\":", i ? "," : "",
			    static_cast<unsigned long long>(r.address), static_cast<unsigned long long>(r.bytes),
			    static_cast<unsigned long long>(r.request.buffer_address), static_cast<unsigned long long>(r.request.buffer_size),
			    static_cast<unsigned long long>(r.request.address), static_cast<unsigned long long>(r.min_request),
			    static_cast<unsigned long long>(r.max_request), use_names[static_cast<size_t>(r.request.use)],
			    unsigned(r.request.written), unsigned(r.request.texel));
			WriteCounts(r.counts); std::fputc('}', file);
		}
		std::fprintf(file, "],\"unshown_bytes\":%llu,\"table_dropped_bytes\":%llu,\"table_dropped_ranges\":%llu,\"top_buffers\":[",
		    static_cast<unsigned long long>(total.bytes - shown_bytes), static_cast<unsigned long long>(dropped_bytes),
		    static_cast<unsigned long long>(dropped_ranges));
		used = 0;
		for (size_t i = 0; i < owners.size(); ++i) if (owners[i].valid) order[used++] = i;
		const auto owners_shown = std::min<size_t>(32, used);
		std::partial_sort(order.begin(), order.begin() + owners_shown, order.begin() + used,
		    [&](size_t a, size_t b) { return owners[a].counts.bytes > owners[b].counts.bytes; });
		uint64_t owner_shown_bytes = 0;
		for (size_t i = 0; i < owners_shown; ++i) {
			const auto& owner = owners[order[i]]; owner_shown_bytes += owner.counts.bytes;
			std::fprintf(file, "%s{\"buffer_address\":\"0x%llx\",\"buffer_size\":%llu,\"uses\":[", i ? "," : "",
			    static_cast<unsigned long long>(owner.address), static_cast<unsigned long long>(owner.size));
			bool comma = false;
			for (size_t use = 0; use < uses.size(); ++use) {
				if (!(owner.use_mask & (1ULL << use))) continue;
				std::fprintf(file, "%s\"%s\"", comma ? "," : "", use_names[use]); comma = true;
			}
			std::fputs("],\"counts\":", file); WriteCounts(owner.counts); std::fputc('}', file);
		}
		std::fprintf(file, "],\"unshown_buffer_bytes\":%llu,\"buffer_table_dropped_bytes\":%llu}\n",
		    static_cast<unsigned long long>(total.bytes - owner_shown_bytes), static_cast<unsigned long long>(owner_dropped_bytes));
		if (std::fflush(file) != 0 || std::ferror(file)) {
			std::fprintf(stderr, "[BUFFER_UPLOAD_PROFILE] output write failed; capture disabled\n");
			std::fclose(file); file = nullptr;
		}
	}
	void ResetCounts() {
		total = {}; uses = {}; sizes = {};
		for (auto& row: rows) { if (row.valid) row = {}; }
		for (auto& owner: owners) { if (owner.valid) owner = {}; }
		dropped_bytes = dropped_ranges = owner_dropped_bytes = 0;
	}
	std::FILE* file = nullptr;
	PageHistory<> pages;
	RangeHistory<> ranges;
	std::array<uint8_t, ScratchBytes> scratch;
	Counts total;
	std::array<Counts, static_cast<size_t>(Use::Count)> uses {};
	std::array<Counts, 6> sizes {};
	std::array<Row, 8192> rows {};
	std::array<Owner, 4096> owners {};
	std::array<size_t, 8192> order {};
	uint64_t frame = 0, segment = 0, dropped_bytes = 0, dropped_ranges = 0, owner_dropped_bytes = 0;
	bool persistent = false, started = false, complete = false;
};

} // namespace Common::BufferUploadProfile
