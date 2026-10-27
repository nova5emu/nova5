#pragma once

#include <cstdint>
#include <string>

// Opt-in observations, not a scheduler/deadlock detector. Never reads guest memory.
namespace Common::StallDump {

enum class Mode { Off, Idle, Always };
enum class Milestone {
	Startup,
	GuestPrintf,
	GuestStdout,
	GuestStderr,
	AvEventDelivered,
	AvEventNoCallback,
	AvDemuxEnd,
	Count
};

Mode ParseMode(const char* override_value, const char* guest_log);
bool Enabled();
void Initialize(); // Explicit host startup; also used by RegisterThread as a fallback.
void RegisterThread(uintptr_t guest_thread, uint64_t host_tid, const char* name,
                    bool primary = false);
void UnregisterThread(); // Call from the host cleanup path, including cancellation/explicit exit.
void RenameThread(uintptr_t guest_thread, const char* name);
void Progress(Milestone kind); // Counts observations; AvEventNoCallback does not reset idle time.
void Note(const char* operation, uintptr_t object = 0, uint64_t detail = 0);
// Bounded process counters, also usable from unregistered host threads. Static names only.
// Counts are exact; latest fields are approximate concurrent observations, not a tuple.
void Observe(const char* operation, uintptr_t object = 0, uint64_t a = 0, uint64_t b = 0,
             uint64_t c = 0);
template <typename T>
T Result(const char* operation, T result, uintptr_t object = 0, uint64_t b = 0, uint64_t c = 0) {
	Observe(operation, object, static_cast<uint32_t>(result), b, c);
	return result;
}

struct Frame {
	const char* operation = nullptr; // Static storage only, e.g. __func__ or a literal.
	uintptr_t   object    = 0;
	uint64_t    since_ms  = 0;
	bool        wait      = false;
};

class Scope {
public:
	explicit Scope(const char* operation, uintptr_t object = 0, bool wait = false);
	~Scope();
	Scope(const Scope&)            = delete;
	Scope& operator=(const Scope&) = delete;

private:
	void*    slot       = nullptr;
	uint64_t generation = 0;
	Frame    previous;
};

// Also used by host tests. Snapshot does not take emulator/guest locks or suspend threads.
std::string Snapshot();
bool        DumpDue(uint64_t now_ms, uint64_t progress_ms, uint64_t previous_dump_ms,
                    Mode mode = Mode::Idle);

} // namespace Common::StallDump
