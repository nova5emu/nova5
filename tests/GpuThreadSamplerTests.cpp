#include "common/gpuThreadSampler.h"

#include <string>
#include <thread>
#include <atomic>
#include <memory>

static void Check(bool value) { if (!value) std::abort(); }

static void BoundaryCounters() {
	using namespace Common::GpuThreadSampler;
	auto state = std::make_unique<BoundaryState>();
	boundary_state = state.get();
	{ PhaseScope draw(Phase::DrawEmit); }
	FrameBoundary();
	Check(state->published.load() == 0); // Closed gate.
	state->requested_window.store(1);
	FrameBoundary(); // Partial frame at the beginning of a window.
	for (unsigned i = 0; i < 10; ++i) {
		PhaseScope prepare(Phase::Prepare);
		PhaseScope draw(Phase::DrawEmit);
		NativeBoundary(Boundary::Barrier); // Part of emission, not a cut.
	}
	NativeBoundary(Boundary::Copy); // One real cut after 10 packets.
	NativeBoundary(Boundary::Barrier); // Another operation, no second cut.
	{ PhaseScope dispatch(Phase::DispatchEmit); }
	{
		ImmediateScope flush(Boundary::Flush);
		ImmediateScope submit(Boundary::Submit); // Nested, only first cuts.
	}
	{ PhaseScope draw(Phase::DrawEmit); }
	FrameBoundary(); // Artificial frame edge closes the tail separately.
	Check(state->published.load() == 2 && !state->frames[0].complete);
	const auto& row = state->frames[1];
	Check(row.complete && row.draws == 11 && row.dispatches == 1 && row.batches == 3);
	Check(row.batch_packets == 12 && row.longest == 10 && row.ge8 == 1 && row.ge32 == 0 && row.frame_tail == 1);
	Check(row.events[static_cast<size_t>(Boundary::Barrier)] == 1 && row.breaks[static_cast<size_t>(Boundary::Barrier)] == 0);
	Check(row.breaks[static_cast<size_t>(Boundary::Copy)] == 1 && row.breaks[static_cast<size_t>(Boundary::Flush)] == 1);
	Check(row.events[static_cast<size_t>(Boundary::Submit)] == 1 && row.breaks[static_cast<size_t>(Boundary::Submit)] == 0);
	{
		PhaseScope draw(Phase::DrawEmit);
		{ ImmediateScope wait(Boundary::Wait); Check(state->phase.load() == Phase::Wait); }
		Check(state->phase.load() == Phase::DrawEmit);
	}
	FrameBoundary();
	Check(state->frames[2].interruptions == 1);
	state->requested_window.store(0);
	FrameBoundary();
	state->requested_window.store(2);
	FrameBoundary();
	Check(state->frames[3].window == 2 && !state->frames[3].complete && state->frames[3].draws == 0);
	state->published.store(BoundaryState::Capacity);
	FrameBoundary();
	Check(state->dropped.load() == 1); // Never overwrite published storage.
	boundary_state = nullptr;
}

#if defined(_WIN32) && defined(_M_X64)
static std::wstring TemporaryFile() {
	wchar_t directory[MAX_PATH] {}, file[MAX_PATH] {};
	Check(GetTempPathW(MAX_PATH, directory) != 0);
	Check(GetTempFileNameW(directory, L"ngs", 0, file) != 0);
	return file;
}

static void CreateGate(const std::wstring& path) {
	const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	    nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	Check(file != INVALID_HANDLE_VALUE);
	CloseHandle(file);
}

static std::string ReadOutput(const std::wstring& path) {
	const auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	Check(file != INVALID_HANDLE_VALUE);
	char buffer[4096];
	DWORD bytes = 0;
	std::string contents;
	while (ReadFile(file, buffer, sizeof(buffer), &bytes, nullptr) && bytes) contents.append(buffer, bytes);
	CloseHandle(file);
	return contents;
}

static void WaitForReport(const std::wstring& path, const char* marker) {
	const auto deadline = GetTickCount64() + 15000;
	while (ReadOutput(path).find(marker) == std::string::npos && GetTickCount64() < deadline) Sleep(20);
	Check(ReadOutput(path).find(marker) != std::string::npos);
}

static void CheckReports(const std::string& text) {
	size_t windows = 0;
	size_t all_samples = 0, module_samples = 0, non_exe_samples = 0, caller_samples = 0;
	bool waiting_seen = false, exe_seen = false;
	size_t cursor = 0;
	while ((cursor = text.find("[GPU_SAMPLER] exe=", cursor)) != std::string::npos) {
		size_t exe = 0, wait = 0, system = 0, external = 0, total = 0;
		Check(std::sscanf(text.c_str() + cursor,
		    "[GPU_SAMPLER] exe=%zu wait_rip=%zu system_other=%zu external=%zu denominator=%zu",
		    &exe, &wait, &system, &external, &total) == 5);
		Check(total > 0 && total == exe + wait + system + external);
		all_samples += total;
		non_exe_samples += wait + system + external;
		waiting_seen |= wait > 0;
		exe_seen |= exe > 0;
		++windows; ++cursor;
	}
	Check(windows == 2 && waiting_seen && exe_seen);
	Check(text.find("reason=trigger_closed") != std::string::npos);
	Check(text.find("reason=shutdown") != std::string::npos);
	Check(text.find("top200_plus_0.1pct=self") != std::string::npos);
	Check(text.find("top200_plus_0.1pct=inclusive") != std::string::npos);
	Check(text.find("resume_error=0") != std::string::npos);
	Check(text.find("schema=3 grouping=module_pdata_entry") != std::string::npos);
	Check(text.find("module_pc module_id=") != std::string::npos);
	Check(text.find("identity=pdata") != std::string::npos);
	cursor = 0;
	while ((cursor = text.find("[GPU_SAMPLER] module id=", cursor)) != std::string::npos) {
		const auto field = text.find(" self=", cursor);
		Check(field < text.find('\n', cursor));
		size_t self = 0;
		Check(std::sscanf(text.c_str() + field, " self=%zu", &self) == 1);
		module_samples += self;
		++cursor;
	}
	Check(module_samples == all_samples);
	cursor = 0;
	while ((cursor = text.find("[GPU_SAMPLER] module_caller ", cursor)) != std::string::npos) {
		const auto field = text.find(" samples=", cursor);
		Check(field < text.find('\n', cursor));
		size_t samples = 0;
		Check(std::sscanf(text.c_str() + field, " samples=%zu", &samples) == 1);
		caller_samples += samples; ++cursor;
	}
	Check(caller_samples == non_exe_samples); // Includes unknown callers, no top-N loss.
}

// Exercise real suspension of a cooperative test lane, never the renderer:
// closed gate, CPU work, kernel wait, reporting while alive, rearm, active exit.
static void Lifecycle(bool enabled, bool flush_before_exit) {
	const auto gate = TemporaryFile();
	const auto output = TemporaryFile();
	Check(DeleteFileW(gate.c_str()) != FALSE);
	Check(_wputenv_s(L"KYTY_GPU_THREAD_SAMPLER_GATE", gate.c_str()) == 0);
	Check(_wputenv_s(L"KYTY_GPU_THREAD_SAMPLER_FILE", output.c_str()) == 0);
	std::atomic<unsigned> phase {0};
	std::atomic<bool> ready {false};
	std::atomic<uint64_t> work {0};
	std::thread lane([&] {
		Common::GpuThreadSampler::Session session;
		ready.store(true);
		while (phase.load() != 3) {
			if (phase.load() == 1) {
				{
					Common::GpuThreadSampler::PhaseScope emission(Common::GpuThreadSampler::Phase::DrawEmit);
					work.fetch_add(1, std::memory_order_relaxed);
				}
				if (work.load(std::memory_order_relaxed) % 16384 == 0) Common::GpuThreadSampler::FrameBoundary();
			} else {
				Common::GpuThreadSampler::ImmediateScope wait(Common::GpuThreadSampler::Boundary::TimelineWait);
				Sleep(5);
			}
		}
	});
	while (!ready.load()) Sleep(1);
	Sleep(150);
	Check(ReadOutput(output).empty()); // Never sample without the gate.
	CreateGate(gate);
	phase.store(1);
	Sleep(400);
	phase.store(2);
	Sleep(400);
	Check(DeleteFileW(gate.c_str()) != FALSE);
	if (enabled) WaitForReport(output, "end_window=1");
	else { Sleep(150); Check(ReadOutput(output).empty()); }
	CreateGate(gate);
	Sleep(400);
	if (flush_before_exit) {
		// WindowRun's quick_exit path must flush without waiting for lane exit.
		Common::GpuThreadSampler::StopAndFlush();
		if (enabled) Check(ReadOutput(output).find("end_window=2") != std::string::npos);
	}
	phase.store(3); // Destructor must resume/join/report with an active window.
	lane.join();
	Check(work.load() > 0);
	if (enabled) {
		const auto report = ReadOutput(output);
		CheckReports(report);
		if (flush_before_exit) {
			Check(report.find("boundary_enabled=1") != std::string::npos);
			const auto draw = report.find("phase name=draw_emit samples=");
			Check(draw != std::string::npos);
			size_t samples = 0;
			Check(std::sscanf(report.c_str() + draw, "phase name=draw_emit samples=%zu", &samples) == 1 && samples > 0);
			Check(report.find("boundary_frame frame=") != std::string::npos);
			Check(report.find("phase_module phase=wait") != std::string::npos);
		}
	}
	else Check(ReadOutput(output).empty());
	Check(DeleteFileW(gate.c_str()) != FALSE);
	Check(DeleteFileW(output.c_str()) != FALSE);
	Check(_wputenv_s(L"KYTY_GPU_THREAD_SAMPLER_GATE", L"") == 0);
	Check(_wputenv_s(L"KYTY_GPU_THREAD_SAMPLER_FILE", L"") == 0);
}
#endif

int main() {
	BoundaryCounters();
	const auto* setting = std::getenv("KYTY_GPU_THREAD_SAMPLER");
	const bool enabled = setting != nullptr && std::strcmp(setting, "1") == 0;
	auto* output = std::tmpfile();
	Check(output != nullptr);
	std::thread lane([&] {
		const auto announced = Common::GpuThreadSampler::AnnounceCurrentThread(output);
#if defined(_WIN32)
		Check(announced == enabled);
		std::rewind(output);
		char text[512] {};
		const auto length = std::fread(text, 1, sizeof(text) - 1, output);
		if (!enabled) { Check(length == 0); return; }
		unsigned long process = 0, thread = 0;
		unsigned long long process_start = 0, thread_start = 0;
		Check(std::sscanf(text,
		    "[GPU_THREAD_SAMPLER] {\"schema\":1,\"backend\":\"inprocess\",\"pid\":%lu,\"tid\":%lu,"
		    "\"process_start_filetime\":\"%llu\",\"thread_start_filetime\":\"%llu\"}",
		    &process, &thread, &process_start, &thread_start) == 4);
		Check(process == GetCurrentProcessId() && thread == GetCurrentThreadId());
		FILETIME created {}, exited {}, kernel {}, user {};
		Check(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user));
		Check(process_start == ((uint64_t{created.dwHighDateTime} << 32u) | created.dwLowDateTime));
		Check(GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user));
		Check(thread_start == ((uint64_t{created.dwHighDateTime} << 32u) | created.dwLowDateTime));
#else
		Check(!announced);
#endif
	});
	lane.join();
	std::fclose(output);
#if defined(_WIN32) && defined(_M_X64)
	Check(Common::GpuThreadSampler::CheckAggregationForTests());
	Check(_wputenv_s(L"KYTY_GPU_THREAD_SAMPLER_BOUNDARY", L"") == 0);
	Lifecycle(enabled, false);
	if (enabled) {
		Check(_wputenv_s(L"KYTY_GPU_THREAD_SAMPLER_BOUNDARY", L"1") == 0);
		Lifecycle(true, true);
		Check(_wputenv_s(L"KYTY_GPU_THREAD_SAMPLER_BOUNDARY", L"") == 0);
	}
#endif
	std::puts("GpuThreadSamplerTests: identity, gate and lifecycle passed");
}
