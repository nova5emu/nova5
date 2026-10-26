#pragma once

#include "common/submitProfile.h"

#include <utility>

// Opt-in cold-read attribution, independent of render-stage clocks. No values,
// backing pointers or ownership decisions are cached here.
namespace Common::ReadbackProfile {
inline bool Armed() {
	static const bool enabled = [] {
		const auto* value = std::getenv("KYTY_READBACK_DETAIL");
		return value && std::string_view(value) == "1";
	}();
	return enabled && SubmitProfile::Armed();
}
inline bool Enabled() { return Armed() && SubmitProfile::Recording(); }
enum class Kind { Unknown, Fault, IndirectCount, IndirectRecords, Walker, IndirectInstances };
inline constexpr std::array KindNames {"unknown", "fault", "indirect_count", "indirect_records", "walker", "indirect_instances"};
inline bool Overlaps(uint64_t a, uint64_t n, uint64_t b, uint64_t m) {
	return n && m && (a <= b ? b - a < n : a - b < m);
}
struct Downloads {
	// Exact dirty intervals copied by BufferCache, not the broader bound-writer ranges.
	std::array<std::pair<uint64_t, uint64_t>, 4> ranges {};
	uint64_t count = 0, bytes = 0;
	bool fault_dirty = false, read_dirty = false, page_dirty = false;
	void Observe(uint64_t read, uint64_t width, uint64_t fault, bool has_fault,
	             uint64_t start, uint64_t end) {
		if (end <= start) return;
		if (count < ranges.size()) ranges[count] = {start, end - start};
		++count; bytes += end - start;
		fault_dirty |= has_fault && Overlaps(fault, 1, start, end - start);
		read_dirty |= Overlaps(read, width, start, end - start);
		page_dirty |= Overlaps((has_fault ? fault : read) & ~uint64_t{4095}, 4096, start, end - start);
	}
	bool FalseShared(uint64_t read, uint64_t width, uint64_t page) const {
		return width && read >= page && read - page < 4096 && width <= 4096 - (read - page) &&
		       page_dirty && !read_dirty;
	}
};
struct Origin {
	Kind kind = Kind::Unknown;
	uint32_t tid = 0, role = 0;
	uint64_t rip = 0, address = 0, bytes = 0, hash = 0;
	uint32_t stage = 0, jit_offset = 0, pc = 0, memory = UINT32_MAX, resource = UINT32_MAX;
	uint32_t uses = 0, flat_slot = UINT32_MAX, descriptor = UINT32_MAX;
	uint32_t memory_kind = 0, immediate = 0;
	bool planning_only = false;
	const char* opcode = "unknown";
	uint64_t fault_address = 0;
	Downloads downloads;
	uint64_t record_address = 0, count_address = 0;
	uint32_t record_index = 0, record_stride = 0, record_count = 0;
	uint32_t vertex_register = 0, instance_register = 0, index_register = 0;
	bool indexed = false;
};
// Preserve an explicit annotation only for the load it describes. An unrelated
// nested fault (e.g. shader preparation) must retain its own attribution.
inline Origin FaultOrigin(Origin reader, uint64_t rip, uint64_t fault) {
	const bool explicit_read = reader.kind == Kind::IndirectCount || reader.kind == Kind::IndirectRecords ||
	                           reader.kind == Kind::IndirectInstances;
	if (!explicit_read || !Overlaps(reader.address, reader.bytes, fault, 1)) reader = {};
	if (reader.kind == Kind::Unknown) reader.kind = Kind::Fault;
	reader.tid = SubmitProfile::Diagnostics::ThreadId();
	reader.role = static_cast<uint32_t>(SubmitProfile::Diagnostics::role);
	reader.rip = rip; reader.address = fault; reader.bytes = 0;
	reader.fault_address = fault; reader.downloads = {};
	return reader;
}
inline thread_local Origin origin;
class OriginScope {
public:
	explicit OriginScope(Origin value): previous(origin) { origin = value; }
	~OriginScope() { origin = previous; }
private:
	Origin previous;
};
// Only the selected guest load is wrapped, not the draws/dispatches it triggers.
template <typename F> auto Read(Kind kind, uint64_t address, uint64_t bytes, F&& read) {
	if (!Enabled()) return read();
	Origin value;
	value.kind = kind; value.address = address; value.bytes = bytes;
	value.tid = SubmitProfile::Diagnostics::ThreadId();
	value.role = static_cast<uint32_t>(SubmitProfile::Diagnostics::role);
	OriginScope scope(value);
	return read();
}
struct Writer {
	uint64_t address = 0, bytes = 0, hash = 0, module = 0, tick = 0, sequence = 0;
	uint32_t stage = 0, resource = 0;
	bool atomic = false;
};
struct Writers {
	// Recent bound shader buffer write ranges, NOT proof of an executed write.
	std::array<Writer, 2048> entries {};
	uint64_t sequence = 0;
	void Add(Writer value) {
		value.sequence = ++sequence;
		entries[(sequence - 1) % entries.size()] = value;
	}
	Writer Find(uint64_t address, uint64_t bytes) const {
		for (uint64_t age = 0; age < std::min<uint64_t>(sequence, entries.size()); ++age) {
			const auto& w = entries[(sequence - 1 - age) % entries.size()];
			if (Overlaps(address, bytes, w.address, w.bytes)) return w;
		}
		return {};
	}
};
struct Entry {
	Origin last;
	Writer writer;
	uint64_t calls = 0, ns = 0, tick = 0;
	uint64_t unknown_width = 0, dirty_fault_byte = 0, dirty_read = 0, false_shared = 0;
	bool local_fault_scope = false;
};
struct State {
	std::array<Entry, 64> entries {};
	Writers writers;
	uint64_t generation = UINT64_MAX, start = 0, calls = 0, ns = 0, overflow = 0, reports = 0;
};
inline thread_local State state;
inline void RecordDownload(uint64_t start, uint64_t end) {
	if (!Enabled() || origin.kind == Kind::Unknown ||
	    SubmitProfile::CurrentWaitSite() != SubmitProfile::WaitSite::ReadbackCpuReadFault) return;
	origin.downloads.Observe(origin.address, origin.bytes, origin.fault_address, origin.rip != 0, start, end);
}
inline bool Same(const Origin& a, const Origin& b) {
	return a.kind == b.kind && a.rip == b.rip && a.hash == b.hash && a.memory == b.memory &&
	       a.jit_offset == b.jit_offset && a.tid == b.tid && a.role == b.role;
}
inline void ResetWindow(State& s, uint64_t now, uint64_t generation) {
	s.entries = {}; s.calls = s.ns = s.overflow = 0; s.start = now; s.generation = generation;
}
inline void RecordWriter(Writer writer) {
	if (!Enabled() || !writer.bytes) return;
	const auto generation = SubmitProfile::Detail::generation.load(std::memory_order_relaxed);
	if (state.generation != generation) {
		ResetWindow(state, SubmitProfile::NowNs(), generation);
		state.writers = {};
	}
	state.writers.Add(writer);
}
inline void Add(State& s, Origin reader, uint64_t elapsed, uint64_t tick, bool local) {
	++s.calls; s.ns += elapsed;
	for (auto& e: s.entries) {
		if (!e.calls || (Same(e.last, reader) && e.local_fault_scope == local)) {
			e.last = reader; ++e.calls; e.ns += elapsed; e.tick = tick;
			e.local_fault_scope = local;
			e.writer = s.writers.Find(reader.address, reader.bytes ? reader.bytes : (reader.rip ? 1u : 0u));
			e.unknown_width += reader.bytes == 0;
			e.dirty_fault_byte += reader.downloads.fault_dirty;
			e.dirty_read += reader.downloads.read_dirty;
			e.false_shared += reader.downloads.FalseShared(reader.address, reader.bytes,
			    (reader.rip ? reader.fault_address : reader.address) & ~uint64_t{4095});
			return;
		}
	}
	++s.overflow;
}
inline void Report(State& s, uint64_t now) {
	if (now - s.start < 5'000'000'000ull) return;
	if (s.reports++ < 240) {
		const auto tid = SubmitProfile::Diagnostics::ThreadId();
		std::fprintf(stderr, "[READBACK_ORIGINS] {\"tid\":%u,\"end_ns\":%llu,\"interval_ms\":%.3f,"
		    "\"calls\":%llu,\"wait_ns\":%llu,\"overflow\":%llu,\"writer_sequence\":%llu,\"writer_capacity\":2048}\n",
		    tid, (unsigned long long)now, (now - s.start) / 1e6, (unsigned long long)s.calls,
		    (unsigned long long)s.ns, (unsigned long long)s.overflow, (unsigned long long)s.writers.sequence);
		std::sort(s.entries.begin(), s.entries.end(), [](const Entry& a, const Entry& b) { return a.ns > b.ns; });
		for (const auto& e: s.entries) {
			if (!e.calls) break;
			const auto& r = e.last;
			const auto& w = e.writer;
			std::fprintf(stderr, "[READBACK_ORIGIN] {\"tid\":%u,\"end_ns\":%llu,\"kind\":\"%s\","
			    "\"request_tid\":%u,\"request_role\":%u,\"local_fault_scope\":%s,\"calls\":%llu,\"wait_ns\":%llu,"
			    "\"rip\":\"0x%llx\",\"last_address\":\"0x%llx\",\"read_bytes\":%llu,\"reader_hash\":\"0x%llx\","
			    "\"stage\":%u,\"jit_offset\":%u,\"pc\":%u,\"ir\":\"%s\",\"memory\":%u,\"resource\":%u,"
			    "\"uses\":%u,\"flat_slot\":%u,\"descriptor\":%u,\"memory_kind\":%u,\"immediate\":%u,"
			    "\"planning_only\":%s,\"wait_tick\":%llu,"
			    "\"bound_writer\":{\"sequence\":%llu,\"tick\":%llu,\"hash\":\"0x%llx\",\"module\":\"0x%llx\","
			    "\"stage\":%u,\"resource\":%u,\"atomic\":%s,\"address\":\"0x%llx\",\"bytes\":%llu},",
			    tid, (unsigned long long)now, KindNames[static_cast<size_t>(r.kind)], r.tid, r.role,
			    e.local_fault_scope ? "true" : "false", (unsigned long long)e.calls, (unsigned long long)e.ns,
			    (unsigned long long)r.rip, (unsigned long long)r.address, (unsigned long long)r.bytes,
			    (unsigned long long)r.hash, r.stage, r.jit_offset, r.pc, r.opcode, r.memory, r.resource,
			    r.uses, r.flat_slot, r.descriptor, r.memory_kind, r.immediate, r.planning_only ? "true" : "false",
			    (unsigned long long)e.tick, (unsigned long long)w.sequence,
			    (unsigned long long)w.tick, (unsigned long long)w.hash, (unsigned long long)w.module,
			    w.stage, w.resource, w.atomic ? "true" : "false", (unsigned long long)w.address, (unsigned long long)w.bytes);
			std::fprintf(stderr, "\"unknown_width_waits\":%llu,\"dirty_fault_byte_waits\":%llu,"
			    "\"dirty_read_waits\":%llu,\"false_shared_waits\":%llu,\"last_fault_address\":\"0x%llx\","
			    "\"last_download_bytes\":%llu,\"last_download_range_count\":%llu,\"last_download_ranges\":[",
			    (unsigned long long)e.unknown_width, (unsigned long long)e.dirty_fault_byte,
			    (unsigned long long)e.dirty_read, (unsigned long long)e.false_shared,
			    (unsigned long long)r.fault_address, (unsigned long long)r.downloads.bytes,
			    (unsigned long long)r.downloads.count);
			for (size_t i = 0; i < std::min<uint64_t>(r.downloads.count, r.downloads.ranges.size()); ++i) {
				const auto [address, bytes] = r.downloads.ranges[i];
				std::fprintf(stderr, "%s{\"address\":\"0x%llx\",\"bytes\":%llu}", i ? "," : "",
				    (unsigned long long)address, (unsigned long long)bytes);
			}
			std::fprintf(stderr, "]");
			if (r.kind == Kind::IndirectRecords) {
				std::fprintf(stderr, ",\"indirect_record\":{\"address\":\"0x%llx\",\"fault_offset\":%llu,\"count_address\":\"0x%llx\","
				    "\"index\":%u,\"stride\":%u,\"count\":%u,\"indexed\":%s,"
				    "\"vertex_register\":%u,\"instance_register\":%u,\"index_register\":%u}",
				    (unsigned long long)r.record_address, (unsigned long long)(r.fault_address - r.record_address),
				    (unsigned long long)r.count_address,
				    r.record_index, r.record_stride, r.record_count, r.indexed ? "true" : "false",
				    r.vertex_register, r.instance_register, r.index_register);
			}
			std::fprintf(stderr, "}\n");
		}
	}
	ResetWindow(s, now, s.generation);
}
class WaitScope {
public:
	explicit WaitScope(uint64_t tick): m_tick(tick) {
		if (!Enabled() || SubmitProfile::Detail::current_wait_site != SubmitProfile::WaitSite::ReadbackCpuReadFault) return;
		m_start = SubmitProfile::NowNs();
		m_generation = SubmitProfile::Detail::generation.load(std::memory_order_relaxed);
		if (state.generation != m_generation) {
			ResetWindow(state, m_start, m_generation); state.writers = {};
		}
	}
	~WaitScope() {
		if (!m_start || !Enabled() || m_generation != SubmitProfile::Detail::generation.load(std::memory_order_relaxed)) return;
		const auto now = SubmitProfile::NowNs();
		Add(state, origin, now - m_start, m_tick, SubmitProfile::Diagnostics::ReadFaults::active != nullptr);
		Report(state, now);
	}
private:
	uint64_t m_tick, m_start = 0, m_generation = 0;
};
} // namespace Common::ReadbackProfile
