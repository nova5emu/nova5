#include "common/stallDump.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>
#include <thread>
#include <vector>

namespace Common::StallDump {
namespace {
constexpr size_t      THREAD_LIMIT       = 512;
constexpr size_t      HISTORY_PER_THREAD = 16;
constexpr size_t      HISTORY_OUTPUT     = 64;
constexpr size_t      OBSERVATION_LIMIT  = 256;
constexpr uint64_t    RESERVED           = std::numeric_limits<uint64_t>::max();
constexpr size_t      MILESTONE_COUNT    = static_cast<size_t>(Milestone::Count);
constexpr const char* MILESTONE_NAMES[]  = {
    "startup",      "guest_printf",       "guest_stdout",
    "guest_stderr", "av_event_delivered", "av_event_no_callback",
    "av_demux_end"};
static_assert(std::size(MILESTONE_NAMES) == MILESTONE_COUNT && MILESTONE_COUNT < 256);

bool ResetsIdle(Milestone kind) {
	return kind != Milestone::Startup && kind != Milestone::AvEventNoCallback;
}
const char* ModeName(Mode mode) {
	return mode == Mode::Always ? "always" : mode == Mode::Idle ? "idle" : "off";
}
struct Config {
	Mode        mode;
	const char* setting;
	const char* activation;
	bool        guest_log;
};
const Config& Configuration() {
	static const Config config = [] {
		const auto* value = std::getenv("KYTY_STALL_DUMP");
		const auto* guest = std::getenv("KYTY_GUEST_LOG");
		const auto  mode  = ParseMode(value, guest);
		return Config {mode,
		               value == nullptr                    ? "unset"
		               : std::strcmp(value, "0") == 0      ? "0"
		               : std::strcmp(value, "1") == 0      ? "1"
		               : std::strcmp(value, "always") == 0 ? "always"
		                                                   : "invalid",
		               value != nullptr ? "KYTY_STALL_DUMP" : "KYTY_GUEST_LOG",
		               guest != nullptr && std::strcmp(guest, "1") == 0};
	}();
	return config;
}
static_assert(std::atomic<uint64_t>::is_always_lock_free &&
              std::atomic<uintptr_t>::is_always_lock_free &&
              std::atomic<const char*>::is_always_lock_free);

uint64_t Now() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

// All fields are atomic: even a rejected concurrent snapshot has no data races.
// No diagnostic mutex can be stranded by guest async cancellation or stack switching.
struct Record {
	std::atomic<uint64_t>    sequence {0};
	std::atomic<const char*> operation {nullptr};
	std::atomic<uintptr_t>   object {0};
	std::atomic<uint64_t>    first {0}, last {0}, detail {0}, repeats {0}, order {0};
	std::atomic<bool>        wait {false};
};
struct Sample {
	Frame    frame;
	uint64_t last = 0, detail = 0, repeats = 0, tid = 0, order = 0;
};
void Write(Record& r, const Frame& f, uint64_t last = 0, uint64_t detail = 0, uint64_t repeats = 1,
           uint64_t order = 0) {
	r.sequence.fetch_add(1);
	r.operation.store(f.operation);
	r.object.store(f.object);
	r.first.store(f.since_ms);
	r.wait.store(f.wait);
	r.last.store(last);
	r.detail.store(detail);
	r.repeats.store(repeats);
	r.order.store(order);
	r.sequence.fetch_add(1);
}
bool Read(const Record& r, Sample& s) {
	const auto before = r.sequence.load();
	if (before & 1) {
		return false;
	}
	s.frame   = {r.operation.load(), r.object.load(), r.first.load(), r.wait.load()};
	s.last    = r.last.load();
	s.detail  = r.detail.load();
	s.repeats = r.repeats.load();
	s.order   = r.order.load();
	return before == r.sequence.load();
}
struct Slot {
	std::atomic<uint64_t>                  generation {0}, tid {0};
	std::atomic<uintptr_t>                 guest {0};
	std::atomic<bool>                      primary {false};
	std::array<std::atomic<char>, 64>      name {};
	Record                                 current;
	std::array<Record, HISTORY_PER_THREAD> history;
	// Written only by this slot's registered thread; snapshot never reads these.
	size_t next = 0;
	Frame  frame;
};
struct State {
	std::array<Slot, THREAD_LIMIT> slots;
	std::atomic<uint64_t>          next_generation {1}, overflow {0};
	// Timestamp and reason are published together. Lower byte is Milestone.
	std::atomic<uint64_t> progress {Now() << 8};
	struct Counter {
		std::atomic<uint64_t> count {0}, last_ms {0};
	};
	std::array<Counter, MILESTONE_COUNT> milestones;
	struct Observation {
		std::atomic<const char*> operation {nullptr};
		std::atomic<uint64_t>    count {0}, nonzero_a {0}, last_nonzero_a {0}, last_ms {0};
		std::atomic<uint64_t>    object {0}, a {0}, b {0}, c {0}, tid {0};
	};
	std::array<Observation, OBSERVATION_LIMIT> observations;
	std::atomic<uint64_t>                      observations_dropped {0};
};
State& Get() {
	// Process lifetime: detached monitor/TLS cleanup must not race static destruction.
	static auto* state = new State;
	return *state;
}
thread_local Slot*    self            = nullptr;
thread_local uint64_t self_generation = 0;

void SetName(Slot& slot, const char* name) {
	for (size_t i = 0; i < slot.name.size(); ++i) {
		const char c = name != nullptr && i + 1 < slot.name.size() ? *name : '\0';
		slot.name[i].store(c);
		if (c != '\0') {
			++name;
		}
	}
}
void Remember(Slot& slot, const Frame& frame, uint64_t detail) {
	if (slot.next != 0) {
		auto&  previous = slot.history[(slot.next - 1) % HISTORY_PER_THREAD];
		Sample s;
		if (Read(previous, s) && s.frame.operation == frame.operation &&
		    s.frame.object == frame.object && s.detail == detail && s.frame.wait == frame.wait) {
			Write(previous, s.frame, frame.since_ms, detail, s.repeats + 1, s.order);
			return;
		}
	}
	const auto order = slot.next++;
	Write(slot.history[order % HISTORY_PER_THREAD], frame, frame.since_ms, detail, 1, order);
}
void StartMonitor() {
	static const bool started = [] {
		std::thread([] {
			const auto& config = Configuration();
			std::fprintf(stderr,
			             "[STALL_MONITOR] started mode=%s interval_ms=%u activation=%s setting=%s "
			             "guest_log=%d\n",
			             ModeName(config.mode), config.mode == Mode::Always ? 15000u : 10000u,
			             config.activation, config.setting, int(config.guest_log));
			std::fflush(stderr);
			uint64_t last_dump = Now();
			for (;;) {
				std::this_thread::sleep_for(std::chrono::seconds(1));
				const auto now = Now();
				if (DumpDue(now, Get().progress.load() >> 8, last_dump, config.mode)) {
					last_dump       = now;
					const auto dump = Snapshot();
					// Same stderr channel captured by run-astro-diagnostic.py, even with Silent.
					std::fwrite(dump.data(), 1, dump.size(), stderr);
					std::fflush(stderr);
				}
			}
		}).detach();
		return true;
	}();
	(void)started;
}
} // namespace

Mode ParseMode(const char* value, const char* guest) {
	if (value != nullptr) {
		if (std::strcmp(value, "always") == 0) {
			return Mode::Always;
		}
		return std::strcmp(value, "1") == 0 ? Mode::Idle : Mode::Off;
	}
	return guest != nullptr && std::strcmp(guest, "1") == 0 ? Mode::Idle : Mode::Off;
}
bool Enabled() {
	return Configuration().mode != Mode::Off;
}
void Initialize() {
	static const bool initialized = [] {
		const auto& config = Configuration();
		if (config.mode == Mode::Off) {
			if (std::strcmp(config.setting, "unset") != 0 || config.guest_log) {
				std::fprintf(
				    stderr,
				    "[STALL_MONITOR] disabled mode=off activation=%s setting=%s guest_log=%d\n",
				    config.activation, config.setting, int(config.guest_log));
				std::fflush(stderr);
			}
			return true;
		}
		Get(); // Initialize state on the host startup thread, before starting the monitor.
		StartMonitor();
		return true;
	}();
	(void)initialized;
}
bool DumpDue(uint64_t now, uint64_t progress, uint64_t previous, Mode mode) {
	if (mode == Mode::Off) {
		return false;
	}
	if (mode == Mode::Always) {
		return now >= previous && now - previous >= 15000;
	}
	return now >= progress && now - progress >= 10000 && now >= previous && now - previous >= 10000;
}
void RegisterThread(uintptr_t guest, uint64_t tid, const char* name, bool primary) {
	if (!Enabled() || self != nullptr) {
		return;
	}
	Initialize();
	auto& state = Get();
	for (auto& slot: state.slots) {
		uint64_t empty = 0;
		if (!slot.generation.compare_exchange_strong(empty, RESERVED)) {
			continue;
		}
		slot.tid.store(tid);
		slot.guest.store(guest);
		slot.primary.store(primary);
		SetName(slot, name);
		slot.next  = 0;
		slot.frame = {nullptr, 0, Now(), false};
		// An interrupted write can leave an odd sequence. Reset on reuse.
		slot.current.sequence.store(0);
		Write(slot.current, slot.frame);
		for (auto& r: slot.history) {
			r.sequence.store(0);
			Write(r, {});
		}
		self            = &slot;
		self_generation = state.next_generation.fetch_add(1);
		slot.generation.store(self_generation);
		return;
	}
	state.overflow.fetch_add(1);
}
void UnregisterThread() {
	if (self != nullptr) {
		self->generation.store(0);
		self            = nullptr;
		self_generation = 0;
	}
}
void RenameThread(uintptr_t guest, const char* name) {
	if (!Enabled()) {
		return;
	}
	for (auto& slot: Get().slots) {
		const auto generation = slot.generation.load();
		if (generation != 0 && generation != RESERVED && slot.guest.load() == guest) {
			SetName(slot, name);
			return;
		}
	}
}
void Progress(Milestone kind) {
	const auto index = static_cast<size_t>(kind);
	if (!Enabled() || index == 0 || index >= MILESTONE_COUNT) {
		return;
	}
	auto&      state   = Get();
	const auto now     = Now();
	auto&      counter = state.milestones[index];
	counter.count.fetch_add(1, std::memory_order_relaxed);
	auto last = counter.last_ms.load();
	while (last < now && !counter.last_ms.compare_exchange_weak(last, now)) {
	}
	if (ResetsIdle(kind)) {
		// A delayed writer must not move time backwards; ties can still change the reason.
		auto       previous = state.progress.load();
		const auto next     = (now << 8) | index;
		while ((previous >> 8) <= now && !state.progress.compare_exchange_weak(previous, next)) {
		}
	}
}
void Note(const char* operation, uintptr_t object, uint64_t detail) {
	if (Enabled() && self != nullptr) {
		Remember(*self, {operation, object, Now(), false}, detail);
	}
}
void Observe(const char* operation, uintptr_t object, uint64_t a, uint64_t b, uint64_t c) {
	if (!Enabled() || operation == nullptr) {
		return;
	}
	auto&      state = Get();
	const auto start = (reinterpret_cast<uintptr_t>(operation) >> 4) % OBSERVATION_LIMIT;
	for (size_t i = 0; i < OBSERVATION_LIMIT; ++i) {
		auto& record = state.observations[(start + i) % OBSERVATION_LIMIT];
		auto  key    = record.operation.load(std::memory_order_relaxed);
		if (key == nullptr) {
			record.operation.compare_exchange_strong(key, operation, std::memory_order_relaxed);
			if (key == nullptr) {
				key = operation;
			}
		}
		if (key != operation) {
			continue;
		}
		record.object.store(object, std::memory_order_relaxed);
		record.tid.store(self == nullptr ? 0 : self->tid.load(), std::memory_order_relaxed);
		record.a.store(a, std::memory_order_relaxed);
		record.b.store(b, std::memory_order_relaxed);
		record.c.store(c, std::memory_order_relaxed);
		record.last_ms.store(Now(), std::memory_order_relaxed);
		if (a != 0) {
			record.last_nonzero_a.store(a, std::memory_order_relaxed);
			record.nonzero_a.fetch_add(1, std::memory_order_relaxed);
		}
		record.count.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	state.observations_dropped.fetch_add(1, std::memory_order_relaxed);
}
Scope::Scope(const char* operation, uintptr_t object, bool wait) {
	if (!Enabled() || self == nullptr) {
		return;
	}
	slot        = self;
	generation  = self_generation;
	previous    = self->frame;
	self->frame = {operation, object, Now(), wait};
	Write(self->current, self->frame);
	Remember(*self, self->frame, 0);
}
Scope::~Scope() {
	if (slot == nullptr || slot != self || generation != self_generation) {
		return;
	}
	self->frame = previous;
	Write(self->current, previous);
}

std::string Snapshot() {
	if (!Enabled()) {
		return {};
	}
	const auto         now      = Now();
	auto&              state    = Get();
	const auto         stamp    = state.progress.load();
	const auto         progress = stamp >> 8;
	std::ostringstream out;
	out << "\n[STALL] t_ms=" << now << " mode=" << ModeName(Configuration().mode)
	    << " no_milestone_ms=" << (now >= progress ? now - progress : 0)
	    << " last_idle_reset=" << MILESTONE_NAMES[stamp & 255] << " last_idle_reset_ms=" << progress
	    << " overflow=" << state.overflow.load()
	    << " suspected_only=1 (always ignores idle time; polling/flips are not milestones)\n";
	for (size_t i = 1; i < MILESTONE_COUNT; ++i) {
		const auto& counter = state.milestones[i];
		out << "[STALL_MILESTONE] kind=" << MILESTONE_NAMES[i]
		    << " count_total=" << counter.count.load() << " last_ms=" << counter.last_ms.load()
		    << " resets_idle=" << int(ResetsIdle(static_cast<Milestone>(i))) << '\n';
	}
	std::vector<Sample> history;
	size_t              incomplete = 0;
	for (auto& slot: state.slots) {
		const auto generation = slot.generation.load();
		if (generation == RESERVED || slot.tid.load() == 0) {
			continue;
		}
		const auto  tid   = slot.tid.load();
		const auto  guest = slot.guest.load();
		std::string name;
		for (const auto& c: slot.name) {
			const auto ch = c.load();
			if (ch == '\0') {
				break;
			}
			name += ch < ' ' || ch == '"' ? '?' : ch;
		}
		Sample              current;
		const bool          stable = Read(slot.current, current);
		std::vector<Sample> recent;
		for (const auto& r: slot.history) {
			Sample s;
			if (Read(r, s) && s.frame.operation != nullptr) {
				s.tid = tid;
				recent.push_back(s);
			}
		}
		if (generation != slot.generation.load()) {
			++incomplete;
			continue;
		}
		// Preserve final worker milestones until this slot is reused, without reporting
		// exited threads as live/pending (e.g. AvPlayer demux/decoder completion).
		if (generation == 0) {
			history.insert(history.end(), recent.begin(), recent.end());
			continue;
		}
		out << "[STALL_THREAD] tid=" << tid << " guest=0x" << std::hex << guest << std::dec
		    << " generation=" << generation << " name=\"" << name << "\"";
		if (!stable) {
			out << " state=changing_or_interrupted\n";
		} else if (current.frame.operation == nullptr) {
			out << " state=outside_instrumented_HLE (guest/host code; not proof of running)\n";
		} else {
			const auto& f = current.frame;
			out << " state=" << (f.wait ? "wait_call_pending" : "HLE_pending")
			    << " op=" << f.operation << " object=0x" << std::hex << f.object << std::dec
			    << " since_ms=" << f.since_ms
			    << " age_ms=" << (now >= f.since_ms ? now - f.since_ms : 0) << '\n';
		}
		if (!recent.empty()) {
			const auto& last =
			    *std::max_element(recent.begin(), recent.end(),
			                      [](const auto& a, const auto& b) { return a.order < b.order; });
			out << "[STALL_LAST] tid=" << tid << " op=" << last.frame.operation << " object=0x"
			    << std::hex << last.frame.object << " detail=0x" << last.detail << std::dec
			    << " last_ms=" << last.last << " repeats=" << last.repeats << '\n';
		}
		if (slot.primary.load()) {
			std::stable_sort(recent.begin(), recent.end(),
			                 [](const auto& a, const auto& b) { return a.order < b.order; });
			for (const auto& s: recent) {
				out << "[STALL_MAIN] tid=" << tid << " op=" << s.frame.operation << " object=0x"
				    << std::hex << s.frame.object << " detail=0x" << s.detail << std::dec
				    << " first_ms=" << s.frame.since_ms << " last_ms=" << s.last
				    << " repeats=" << s.repeats << " order=" << s.order << '\n';
			}
		}
		history.insert(history.end(), recent.begin(), recent.end());
	}
	std::stable_sort(history.begin(), history.end(),
	                 [](const auto& a, const auto& b) { return a.last < b.last; });
	const auto first = history.size() > HISTORY_OUTPUT ? history.size() - HISTORY_OUTPUT : 0;
	for (size_t i = first; i < history.size(); ++i) {
		const auto& s = history[i];
		out << "[STALL_HLE] tid=" << s.tid << " op=" << s.frame.operation << " object=0x"
		    << std::hex << s.frame.object << " detail=0x" << s.detail << std::dec
		    << " first_ms=" << s.frame.since_ms << " last_ms=" << s.last << " repeats=" << s.repeats
		    << '\n';
	}
	for (const auto& r: state.observations) {
		const auto* operation = r.operation.load();
		if (operation == nullptr) {
			continue;
		}
		out << "[STALL_COUNTER] op=" << operation << " count_total=" << r.count.load()
		    << " nonzero_a_total=" << r.nonzero_a.load() << " last_ms=" << r.last_ms.load()
		    << " tid=" << r.tid.load()
		    << " object=0x" << std::hex << r.object.load() << " a=0x" << r.a.load() << " b=0x"
		    << r.b.load() << " c=0x" << r.c.load() << " last_nonzero_a=0x"
		    << r.last_nonzero_a.load() << std::dec << '\n';
	}
	out << "[STALL_END] history=" << history.size() - first
	    << " counters_dropped=" << state.observations_dropped.load()
	    << " concurrent_slots_skipped=" << incomplete
	    << " coverage=instrumented_calls_only history_per_thread=16 history_max=64\n";
	return out.str();
}
} // namespace Common::StallDump
