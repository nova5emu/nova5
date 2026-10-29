// Drives the real gate/timer runtime through A -> B -> A with trigger files and checks the
// reports it writes to stderr: one per window, timer state never mixed, no bogus intervals.
#include "common/submitProfile.h"
#include "common/readbackProfile.h"
#include "graphics/guest_gpu/submitProfileRuntime.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace Profile = Common::SubmitProfile;
namespace Runtime = Libs::Graphics::SubmitProfileRuntime;
namespace Readback = Common::ReadbackProfile;
void Check(bool ok) { if (!ok) std::abort(); }

void SetEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
	_putenv_s(name, value.c_str());
#else
	setenv(name, value.c_str(), 1);
#endif
}

void Touch(const std::filesystem::path& path) { std::ofstream(path) << "gate\n"; }

// Lets the runtime's 250 ms gate poll elapse, then ticks it.
void Poll() {
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	Runtime::Tick();
}

// Exercise the real TLS scopes in this TU and the runtime's flush in another TU.
// Every window is shorter than the former independent five-second report timer.
void RecordReadbacks(unsigned count, bool unknown_write = false) {
	Readback::RecordWriter({.address = 0x1000, .bytes = 16, .hash = count});
	for (unsigned i = 0; i < count; ++i) {
		{
			Readback::OriginScope origin({.kind = Readback::Kind::Fault, .address = 0x1000, .bytes = 4});
			Profile::WaitSiteScope site(Profile::WaitSite::ReadbackCpuReadFault);
			Readback::WaitScope wait(i + 1);
		}
		{
			Readback::OriginScope origin({.kind = unknown_write ? Readback::Kind::Unknown : Readback::Kind::WriteInvalidate,
			                              .address = 0x1000});
			Profile::WaitSiteScope site(Profile::WaitSite::ReadbackCpuWriteInvalidate);
			Readback::WaitScope wait(i + 1);
		}
	}
	Check(Readback::state.calls == count && Readback::WriteState() && Readback::WriteState()->calls == count);
}

int main(int argc, char** argv) {
	const bool experiment_only = argc == 2 && std::string(argv[1]) == "--experiment-only";
	const auto dir = std::filesystem::temp_directory_path() /
	                 ("kyty-submit-profile-" + std::to_string(Profile::NowNs()));
	std::filesystem::create_directories(dir);
	const auto profile_gate = dir / "submit-profile.trigger";
	const auto timer_gate   = dir / "timer-1ms.trigger";
	const auto experiment_gate = dir / "experiment.trigger";
	const auto log          = dir / "stderr.log";
	// Configure before the first call: the runtime caches the environment.
	SetEnv("KYTY_SUBMIT_PROFILE", experiment_only ? "0" : "1");
	SetEnv("KYTY_READBACK_DETAIL", "1");
	SetEnv("KYTY_SUBMIT_PROFILE_GATE", profile_gate.string());
	SetEnv("KYTY_SUBMIT_PROFILE_FILE", ""); // Ignore a profiling log inherited from the shell.
	if (experiment_only) {
#if defined(_WIN32)
		_putenv_s("KYTY_TIMER_PERIOD_GATE", "");
#else
		unsetenv("KYTY_TIMER_PERIOD_GATE");
#endif
	} else SetEnv("KYTY_TIMER_PERIOD_GATE", timer_gate.string());
	SetEnv("KYTY_EXPERIMENT_GATE", experiment_gate.string());
	SetEnv("KYTY_EXPERIMENT", experiment_only ? "indirect-count-gpu" : "");
	Check(std::freopen(log.string().c_str(), "w", stderr) != nullptr);
	if (experiment_only) {
		Check(!Profile::Armed());
		Runtime::Tick();
		Check(!Profile::ExperimentActive(Profile::Experiment::IndirectCountGpu));
		Touch(experiment_gate);
		Poll();
		Check(Profile::ExperimentActive(Profile::Experiment::IndirectCountGpu));
		Check(!Profile::ExperimentActive(Profile::Experiment::NoGpuTimestamps));
		Check(std::string(Profile::ExperimentName()) == "indirect-count-gpu");
		Check(!Profile::Recording());
		std::filesystem::remove(experiment_gate);
		Poll();
		Check(!Profile::ExperimentActive());
		Runtime::Shutdown();
		std::fflush(stderr);
		std::ifstream input(log);
		std::stringstream contents;
		contents << input.rdbuf();
		Check(contents.str() == "[SUBMIT_PROFILE_EVENT] experiment on\n"
		                        "[SUBMIT_PROFILE_EVENT] experiment off\n");
		input.close();
		std::error_code ignored;
		std::filesystem::remove_all(dir, ignored);
		std::printf("SubmitProfileRuntimeTests: unprofiled experiment gate passed\n");
		return 0;
	}

	Runtime::Tick();
	Check(!Profile::Recording()); // Gate closed: nothing recorded yet.
	Readback::FlushCurrentThread(Profile::NowNs());
	Check(!Readback::WriteState()); // Flushing never allocates the write histogram.
	Profile::RecordSubmit(99);

	Touch(profile_gate);
	Poll();
	Check(Profile::Recording());
	{
		Profile::ReasonScope reason {Profile::Reason::GfxComplete};
		Profile::RecordSubmit(1); // Window A.
	}
	RecordReadbacks(1);
	Touch(timer_gate);
	Poll(); // A closes with timer off; B opens with timer on.
	Check(Readback::state.calls == 0 && Readback::WriteState()->calls == 0);
	{
		Profile::ReasonScope reason {Profile::Reason::GfxComplete};
		Profile::RecordSubmit(2);
		Profile::RecordSubmit(2); // Window B.
	}
	RecordReadbacks(2);
	std::filesystem::remove(timer_gate);
	Poll(); // B closes with timer on; A' opens with timer off.
	{
		Profile::ReasonScope reason {Profile::Reason::GfxComplete};
		Profile::RecordSubmit(3);
		Profile::RecordSubmit(3);
		Profile::RecordSubmit(3); // Window A'.
	}
	RecordReadbacks(3, true); // Unknown origin must still report waits, without invented ranges.
	std::filesystem::remove(profile_gate);
	Poll(); // A' closes; recording stops.
	Check(!Profile::Recording());
	Check(Readback::state.calls == 0 && Readback::WriteState()->calls == 0);
	Runtime::Tick(); // Nothing further is reported while closed.
	Touch(experiment_gate);
	Poll();
	Check(Profile::ExperimentActive());
	Check(!Profile::ExperimentActive(Profile::Experiment::ReleaseMem));
	std::filesystem::remove(experiment_gate);
	Poll();
	Check(!Profile::ExperimentActive());
	Touch(profile_gate);
	Poll(); // Reopening must not leak the preceding generation.
	Check(Profile::Recording());
	{
		Profile::ReasonScope reason {Profile::Reason::GfxComplete};
		for (unsigned i = 0; i < 4; ++i) Profile::RecordSubmit(4);
	}
	RecordReadbacks(4);
	Runtime::Shutdown();
	Check(!Profile::Recording() && Readback::state.calls == 0 && Readback::WriteState()->calls == 0);
	Runtime::Shutdown(); // No duplicate or empty reports.
	std::fflush(stderr);

	std::ifstream input(log);
	std::vector<std::string> reports;
	std::vector<std::string> read_reports, write_reports, write_origins;
	std::string events;
	for (std::string line; std::getline(input, line);) {
		if (line.rfind("[SUBMIT_PROFILE] {", 0) == 0) reports.push_back(line);
		if (line.rfind("[READBACK_ORIGINS] {", 0) == 0) read_reports.push_back(line);
		if (line.rfind("[WRITEBACK_ORIGINS] {", 0) == 0) write_reports.push_back(line);
		if (line.rfind("[WRITEBACK_ORIGIN] {", 0) == 0) write_origins.push_back(line);
		if (line.rfind("[SUBMIT_PROFILE_EVENT] ", 0) == 0) events += line.substr(23) + ";";
	}
	Check(events == "recording on;timer_1ms on;timer_1ms off;recording off;experiment on;experiment off;recording on;");
	Check(reports.size() == 4 && read_reports.size() == 4 && write_reports.size() == 4 && write_origins.size() == 4);
	Check(write_origins[2].find("\"kind\":\"unknown\"") != std::string::npos);
	const char* timers[] = {"\"timer_1ms\":false", "\"timer_1ms\":true", "\"timer_1ms\":false", "\"timer_1ms\":false"};
	const char* counts[] = {"\"count\":1,", "\"count\":2,", "\"count\":3,", "\"count\":4,"};
	const char* tids[]   = {"\"tid\":1,", "\"tid\":2,", "\"tid\":3,", "\"tid\":4,"};
	for (size_t i = 0; i < reports.size(); ++i) {
		const auto& r = reports[i];
		Check(r.find(timers[i]) != std::string::npos);
		Check(r.find(counts[i]) != std::string::npos && r.find(tids[i]) != std::string::npos);
		Check(r.find("\"tid\":99") == std::string::npos); // Gate-closed work never reported.
		const auto interval = std::strtod(r.c_str() + r.find("\"interval_ms\":") + 14, nullptr);
		Check(interval > 0.0 && interval < 5000.0); // No underflowed or stale windows.
		for (const auto* detail : {&read_reports[i], &write_reports[i]}) {
			Check(detail->find("\"calls\":" + std::to_string(i + 1) + ",") != std::string::npos);
			const auto detail_interval = std::strtod(detail->c_str() + detail->find("\"interval_ms\":") + 14, nullptr);
			Check(detail_interval >= 0.0 && detail_interval < 5000.0);
		}
	}
	std::error_code ignored;
	std::filesystem::remove_all(dir, ignored);
	std::printf("SubmitProfileRuntimeTests: all cases passed\n");
}
