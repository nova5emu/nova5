#include "common/audioSignal.h"
#include "common/stallDump.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace Common::StallDump;
void Check(bool ok, const char* message) {
	if (!ok) {
		std::fprintf(stderr, "StallDumpTests: %s\n", message);
		std::abort();
	}
}
void Env(const char* key, const char* value) {
#ifdef _WIN32
	_putenv_s(key, value);
#else
	setenv(key, value, 1);
#endif
}
size_t Count(const std::string& text, const std::string& token) {
	size_t count = 0, pos = 0;
	while ((pos = text.find(token, pos)) != std::string::npos) {
		++count;
		pos += token.size();
	}
	return count;
}
} // namespace

int main(int argc, char** argv) {
	const int16_t pcm[] {0, 0, 0, 0};
	const auto    silence = Common::MeasureAudioSignal(pcm, 4, false);
	Check(silence.scanned == 4 && silence.nonzero == 0 && silence.peak == 0 && silence.rms == 0,
	      "signed PCM silence");
	const int16_t full[] {INT16_MIN, 0, INT16_MAX};
	const auto    signal = Common::MeasureAudioSignal(full, 3, false);
	Check(signal.nonzero == 2 && signal.peak == 1 && signal.rms > 0.8, "signed PCM amplitude");
	const float floats[] {0.0f, -0.0f, -0.5f, 0.5f, std::numeric_limits<float>::quiet_NaN()};
	const auto  floating = Common::MeasureAudioSignal(floats, 5, true);
	Check(floating.nonzero == 2 && floating.nonfinite == 1 && floating.peak == 0.5 &&
	          std::abs(floating.rms - std::sqrt(0.125)) < 1e-9,
	      "float signed zero, nonfinite and normalized RMS");
	std::vector<int16_t> bounded(65537, 0);
	bounded.back()    = 1;
	const auto capped = Common::MeasureAudioSignal(bounded.data(), bounded.size(), false);
	Check(capped.scanned == 65536 && capped.nonzero == 0, "audio scan bound is explicit");
	const std::string mode = argc > 1 ? argv[1] : "enabled";
	Env("KYTY_GUEST_LOG", mode == "always" ? "0" : "1");
	Env("KYTY_STALL_DUMP", mode == "disabled" ? "0" : mode == "always" ? "always" : "1");
	if (mode == "guest") {
#ifdef _WIN32
		_putenv_s("KYTY_STALL_DUMP", "");
#else
		unsetenv("KYTY_STALL_DUMP");
#endif
	}
	Check(!DumpDue(9999, 0, 0), "ten-second threshold");
	Check(DumpDue(10000, 0, 0), "first dump due");
	Check(!DumpDue(19999, 0, 10000), "rate limited after first dump");
	Check(!DumpDue(20000, 15000, 10000), "milestone postpones dump");
	Check(DumpDue(25000, 15000, 10000), "resumes after milestone silence");
	Check(!DumpDue(9000, 10000, 0), "future observation does not underflow");
	Check(ParseMode(nullptr, nullptr) == Mode::Off, "default off");
	Check(ParseMode(nullptr, "1") == Mode::Idle, "guest-log fallback");
	Check(ParseMode("0", "1") == Mode::Off, "zero override");
	Check(ParseMode("invalid", "1") == Mode::Off, "invalid override is not silently enabled");
	Check(ParseMode("always", nullptr) == Mode::Always, "always independent of guest-log");
	Check(!DumpDue(14999, 0, 0, Mode::Always), "always waits fifteen seconds");
	Check(DumpDue(15000, 15000, 0, Mode::Always), "always ignores current progress");
	Check(!DumpDue(29999, 0, 15000, Mode::Always), "always interval after previous dump");
	Check(DumpDue(30000, 30001, 15000, Mode::Always), "always ignores concurrent newer milestone");
	Check(!DumpDue(60000, 0, 0, Mode::Off), "off never dumps");
	Initialize();
	Initialize(); // Idempotent; CTest also checks the started marker in monitor tests.
	if (mode == "always") {
		// No RegisterThread call: startup must work even before any guest thread exists.
		// Continual real progress would starve the old idle-only monitor indefinitely.
		const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(32200);
		while (std::chrono::steady_clock::now() < end) {
			Progress(Milestone::GuestStdout);
			Observe("host-progress", 0x5678, 1, 2, 3);
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		return 0; // CTest requires a periodic dump, not a manual snapshot.
	}
	RegisterThread(0x1234, 42, "main-test");
	if (mode == "disabled") {
		Scope scope("disabled-wait", 1, true);
		Note("disabled-note");
		Observe("disabled-counter", 123, 4, 5, 6);
		Check(Result("disabled-result", -7) == -7, "disabled tracing preserves result");
		Progress(Milestone::GuestPrintf);
		Check(!Enabled() && Snapshot().empty(), "explicit zero overrides guest-log");
		UnregisterThread();
		return 0;
	}
	Check(Enabled(), "environment enables diagnostics");
	if (mode == "watchdog" || mode == "no_callback") {
		Scope waiting("watchdog-wait", 0x123, true);
		Progress(Milestone::GuestPrintf);
		const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(11200);
		while (std::chrono::steady_clock::now() < end) {
			if (mode == "no_callback") {
				Progress(Milestone::AvEventNoCallback);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		UnregisterThread();
		return 0; // CTest requires the monitor's stderr output, not a manual Snapshot().
	}
	Check(Snapshot().find("name=\"main-test\"") != std::string::npos, "thread metadata");
	Progress(Milestone::GuestPrintf);
	for (int i = 0; i < 100; ++i) {
		Progress(Milestone::AvEventNoCallback);
	}
	const auto milestones = Snapshot();
	Check(milestones.find("last_idle_reset=guest_printf ") != std::string::npos,
	      "undelivered AvPlayer events do not reset idle time");
	Check(milestones.find("kind=av_event_no_callback count_total=100 ") != std::string::npos,
	      "count undelivered events for diagnosis");
	Check(milestones.find("kind=guest_printf count_total=1 ") != std::string::npos,
	      "count the source resetting idle time");
	for (int i = 0; i < 3; ++i) {
		Progress(Milestone::AvEventDelivered);
	}
	Check(Snapshot().find("last_idle_reset=av_event_delivered ") != std::string::npos,
	      "delivered callbacks still reset idle time");
	{
		Scope outer("outer-HLE", 0x456);
		{
			Scope      inner("condition-wait", 0x789, true);
			const auto dump = Snapshot();
			Check(dump.find("state=wait_call_pending op=condition-wait object=0x789") !=
			          std::string::npos,
			      "wait operation and object");
			Check(dump.find("since_ms=") != std::string::npos &&
			          dump.find("age_ms=") != std::string::npos,
			      "wait timing");
		}
		Check(Snapshot().find("state=HLE_pending op=outer-HLE") != std::string::npos,
		      "nested scope restores outer");
	}
	Check(Snapshot().find("state=outside_instrumented_HLE") != std::string::npos,
	      "return clears pending HLE");
	for (int i = 0; i < 100; ++i) {
		Note("repeat", 8, 7);
	}
	Check(Snapshot().find("repeats=100") != std::string::npos,
	      "coalesce consecutive identical polls");
	for (int i = 0; i < 100; ++i) {
		Note("ring", i);
	}
	Check(Count(Snapshot(), "[STALL_HLE]") == 16, "bounded history wraps");
	Check(Snapshot().find("[STALL_LAST] tid=42 op=ring object=0x63") != std::string::npos,
	      "last call follows ring order even when timestamps tie");
	RenameThread(0x1234, "renamed");
	Check(Snapshot().find("name=\"renamed\"") != std::string::npos, "thread rename");

	// Model the cleanup path bypassing scope destructors (guest exit/cancellation).
	auto* abandoned = new Scope("abandoned-wait", 0xa, true);
	UnregisterThread();
	Check(Snapshot().find("[STALL_THREAD]") == std::string::npos, "exited thread removed");
	RegisterThread(0x1234, 43, "reused");
	{
		Scope current("new-generation", 0xb, true);
		delete abandoned;
		Check(Snapshot().find("state=wait_call_pending op=new-generation") != std::string::npos,
		      "stale scope cannot overwrite reused slot");
	}
	UnregisterThread();
	for (int i = 0; i < 600; ++i) {
		RegisterThread(0x1234, 44, "short-lived");
		Note("short-call");
		UnregisterThread();
	}
	Check(Snapshot().find("overflow=0") != std::string::npos, "slots reused after cleanup");

	std::atomic<bool> ready {false}, finish {false};
	std::thread       worker([&] {
        RegisterThread(0xcafe, 99, "worker");
        ready = true;
        while (!finish.load()) {
            Scope scope("worker-wait", 0xbeef, true);
            Note("worker-note");
        }
        UnregisterThread();
    });
	while (!ready.load()) {
		std::this_thread::yield();
	}
	for (int i = 0; i < 200; ++i) {
		const auto dump = Snapshot();
		Check(Count(dump, "[STALL_THREAD]") == 1, "concurrent snapshots preserve live thread");
		Check(Count(dump, "[STALL_HLE]") <= 64, "global history bound");
	}
	finish = true;
	worker.join();
	Check(Count(Snapshot(), "[STALL_THREAD]") == 0, "worker cleanup");
	Check(Snapshot().find("worker-note") != std::string::npos,
	      "retain final history until slot reuse");

	// Busy workers must not evict the primary thread from its dedicated
	// output.
	RegisterThread(0xface, 500, "primary", true);
	Observe("primary-counter", 0xface);
	for (int i = 0; i < 16; ++i) {
		Note("primary-poll", i);
	}
	RenameThread(0xface, "renamed-primary");
	std::this_thread::sleep_for(std::chrono::milliseconds(3));
	std::atomic<int> ready_count {0};
	finish = false;
	std::vector<std::thread> noisy;
	for (int i = 0; i < 5; ++i) {
		noisy.emplace_back([&, i] {
			RegisterThread(0x1000 + i, 600 + i, "noisy");
			for (int n = 0; n < 16; ++n) {
				Note("noisy-call", n);
			}
			++ready_count;
			while (!finish.load()) {
				std::this_thread::yield();
			}
			UnregisterThread();
		});
	}
	while (ready_count.load() != 5) {
		std::this_thread::yield();
	}
	const auto noisy_dump = Snapshot();
	Check(noisy_dump.find("tid=500 object=0xface") != std::string::npos,
	      "counter identifies guest caller");
	Check(Count(noisy_dump, "[STALL_MAIN] tid=500") == 16,
	      "primary history survives worker traffic and rename");
	Check(noisy_dump.find("[STALL_HLE] tid=500 ") == std::string::npos,
	      "regression reproduces global ring eviction");
	Check(Count(noisy_dump, "[STALL_HLE]") == 64, "global ring stays bounded");
	finish = true;
	for (auto& t: noisy) {
		t.join();
	}
	UnregisterThread();
	Check(Count(Snapshot(), "[STALL_MAIN]") == 0, "exited primary no longer reported as live");

	// Process counters must include unregistered render/audio host threads
	// and retain errors.
	Check(Result("service-result", -7, 0x123, 4, 5) == -7, "negative result preserved");
	Check(Result("service-result", 0, 0x123, 4, 5) == 0, "success result preserved");
	noisy.clear();
	for (int i = 0; i < 8; ++i) {
		noisy.emplace_back([] {
			for (int n = 0; n < 1000; ++n) {
				Observe("host-counter", 0xabc, 1, 2, 3);
			}
		});
	}
	for (auto& t: noisy) {
		t.join();
	}
	const auto counter_dump = Snapshot();
	Check(counter_dump.find("tid=0 object=0xabc") != std::string::npos,
	      "unregistered host caller is explicit");
	Check(counter_dump.find("op=host-counter count_total=8000 nonzero_a_total=8000") !=
	          std::string::npos,
	      "concurrent first registration and exact counts without guest "
	      "registration");
	Check(counter_dump.find("object=0xabc a=0x1 b=0x2 c=0x3") != std::string::npos,
	      "counter details");
	Check(counter_dump.find("op=service-result count_total=2 nonzero_a_total=1") !=
	              std::string::npos &&
	          counter_dump.find("last_nonzero_a=0xfffffff9") != std::string::npos,
	      "retain an earlier error after later success");
	// Fixed capacity, static names, no unbounded logging/allocation on
	// producer threads.
	static char labels[300][32];
	for (int i = 0; i < 300; ++i) {
		std::snprintf(labels[i], sizeof(labels[i]), "capacity-%d", i);
		Observe(labels[i]);
	}
	const auto full_dump = Snapshot();
	Check(Count(full_dump, "[STALL_COUNTER]") == 256, "bounded counter table");
	Check(full_dump.find("counters_dropped=0") == std::string::npos, "counter overflow is visible");
	std::puts("StallDumpTests passed");
}
