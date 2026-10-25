#include "common/submitProfile.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace Common::SubmitProfile;
void Check(bool ok) { if (!ok) std::abort(); }

std::string Capture(bool timer, uint64_t now = 0) {
	std::FILE* file = std::tmpfile();
	Check(file != nullptr);
	if (now == 0) WriteReport(file, 5000.0, timer);
	else WriteReport(file, 5000.0, timer, now);
	std::rewind(file);
	std::string text;
	char buffer[4096];
	while (const auto size = std::fread(buffer, 1, sizeof(buffer), file)) text.append(buffer, size);
	std::fclose(file);
	return text;
}
bool Contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }
size_t Row(Reason reason, Origin origin) {
	return static_cast<size_t>(reason) * static_cast<size_t>(Origin::Count) + static_cast<size_t>(origin);
}

void TestReadFaultAttribution() {
	using namespace Diagnostics;
	Common::SubmitProfile::Reset();
	SetRole(Role::GuestGpu);
	// Same page, disjoint intervals: a decoded load really is false sharing.
	{
		ReadFaults::Scope fault(true, 0x1234, 0x1040);
		fault.SrtWalker(true);
		fault.ReadAccess(0x1040, 16);
		WaitSiteScope site(WaitSite::ReadbackCpuReadFault);
		ReadFaults::Download(0x1080, 0x10c0);
		ReadFaults::Download(0x1100, 0x1108);
		ReadFaults::Wait(37, WaitSite::ReadbackCpuReadFault);
		ReadFaults::Wait(11, WaitSite::ReadbackPriorityOps);
		ReadFaults::Wait(99, WaitSite::StreamWrap); // A different cause, excluded.
		fault.Handled();
	}
	// The fault byte is clean, but another byte of this same load is GPU-dirty.
	{
		ReadFaults::Scope fault(true, 0x1234, 0x1040);
		fault.ReadAccess(0x1040, 16);
		WaitSiteScope site(WaitSite::ReadbackCpuReadFault);
		ReadFaults::Download(0x1048, 0x1050);
		fault.Handled();
	}
	// Unknown widths and cross-page reads must not claim whole-load false sharing.
	for (bool crossing: {false, true}) {
		ReadFaults::Scope fault(true, 0x2345, 0x1040);
		if (crossing) fault.ReadAccess(0xff0, 128);
		WaitSiteScope site(WaitSite::ReadbackCpuReadFault);
		ReadFaults::Download(0x1080, 0x10c0);
		fault.Handled();
	}
	{
		ReadFaults::Scope outer(true, 0x3456, 0x1040);
		{
			ReadFaults::Scope nested(false, 0x4567, 0x1040);
			WaitSiteScope site(WaitSite::ReadbackCpuReadFault);
			ReadFaults::Download(0x1040, 0x1050);
			ReadFaults::Wait(1000, WaitSite::ReadbackCpuReadFault);
			nested.Handled();
		}
		Check(ReadFaults::active == &outer);
		outer.Handled();
	}
	Check(ReadFaults::active == nullptr);
	const auto& t = ReadFaults::table.total;
	Check(t.calls == 5 && t.false_shared == 1 && t.byte_clean == 4);
	Check(t.srt_walker_calls == 1);
	Check(t.bytes == 208 && t.ranges == 5 && t.no_download == 1);
	Check(t.wait_ns == 37 && t.priority_ns == 11 && t.decoded == 3 && t.unknown == 2);
	// Unhandled, guest/host, disabled and write faults never enter the histogram.
	{ ReadFaults::Scope ignored(true, 1, 0x1000); }
	for (auto role: {Role::Guest, Role::Host}) {
		SetRole(role);
		ReadFaults::Scope ignored(true, 1, 0x1000); ignored.Handled();
	}
	SetRole(Role::GuestGpu);
	SetRecording(false);
	{ ReadFaults::Scope ignored(true, 1, 0x1000); ignored.Handled(); }
	SetRecording(true);
	Check(t.calls == 5);
	const auto report = Capture(false);
	Check(Contains(report, "\"gpu_read_faults\":{") && Contains(report, "\"false_shared\":1"));
	Check(Contains(report, "\"srt_walker_calls\":1"));
	Check(Contains(report, "\"top_rips\":[{\"rip\":\"0x1234\""));
	Check(Contains(report, "\"page\":\"0x1000\"") && Contains(report, "\"read_size\":16"));
	Check(Contains(report, "\"start\":\"0x1048\",\"bytes\":8"));
	Check(ReadFaults::table.total.calls == 0);
	{
		ReadFaults::Scope crossing(true, 1, 0x1000);
		StartWindow(NowNs()); crossing.Handled();
	}
	Check(ReadFaults::dropped_boundary == 1 && ReadFaults::table.total.calls == 0);
	// A reporting thread cannot block a producer in an exception handler.
	{
		std::lock_guard lock(ReadFaults::mutex);
		std::thread producer([] {
			SetRole(Role::GuestGpu);
			ReadFaults::Scope fault(true, 1, 0x1000); fault.Handled();
		});
		producer.join();
	}
	Check(ReadFaults::dropped_busy == 1);
	Common::SubmitProfile::Reset();
	for (uint64_t i = 0; i < 300; ++i) {
		ReadFaults::Scope fault(true, 0x100 + i % 70, 0x1000 + i * 4096);
		fault.Handled();
	}
	Check(ReadFaults::table.total.calls == 300 && ReadFaults::table.rip_overflow != 0 &&
	      ReadFaults::table.page_overflow == 44);
	Common::SubmitProfile::Reset();
	Check(ReadFaults::table.total.calls == 0 && ReadFaults::table.page_overflow == 0);
	{
		ReadFaults::Scope fault(true, 0x1234, 0x1000);
		WaitSiteScope site(WaitSite::ReadbackCpuReadFault);
		{ WaitTimer timer(101); }
		fault.Handled();
	}
	const auto wait_site = static_cast<size_t>(WaitSite::ReadbackCpuReadFault);
	Check(Detail::window.waits[wait_site] == 1 && ReadFaults::table.total.calls == 1 &&
	      ReadFaults::table.total.wait_ns == Detail::window.wait_ns[wait_site]);
	Common::SubmitProfile::Reset();
	{
		ReadFaults::Scope served(true, 0x1234, 0x1000);
		served.ReadAccess(0x1000, 4); served.ServedClean(); served.Handled();
	}
	Check(ReadFaults::table.total.served == 1 && ReadFaults::table.total.false_shared == 1 &&
	      ReadFaults::table.total.wait_ns == 0 && ReadFaults::table.total.ranges == 0);
	Check(Contains(Capture(false), "\"false_shared_served\":1"));
	SetRole(Role::Host);
}

int main(int argc, char** argv) {
	const bool release_mem = argc > 1 && std::string_view(argv[1]) == "--experiment-release";
	const bool retired = argc > 1 && std::string_view(argv[1]) == "--experiment-retired";
	const bool read_clean = argc > 1 && std::string_view(argv[1]) == "--experiment-read-fault-clean";
	const bool attrib_clean = argc > 1 && std::string_view(argv[1]) == "--experiment-attrib-clean-read";
	const bool srt_clean = argc > 1 && std::string_view(argv[1]) == "--experiment-srt-clean-read";
	const auto* experiment = release_mem ? "release-mem" : retired ? "overlap-fast" : read_clean ? "read-fault-clean" :
	                         attrib_clean ? "attrib-clean-read" : srt_clean ? "srt-clean-read" : "";
#if defined(_WIN32)
	_putenv_s("KYTY_EXPERIMENT", experiment);
#else
	setenv("KYTY_EXPERIMENT", experiment, 1);
#endif
	Check(ParseExperiment("") == Experiment::None && ParseExperiment("typo") == Experiment::None);
	Check(ParseExperiment("release-mem") == Experiment::ReleaseMem);
	Check(ParseExperiment("read-fault-clean") == Experiment::ReadFaultClean);
	Check(ParseExperiment("attrib-clean-read") == Experiment::AttribCleanRead);
	Check(ParseExperiment("srt-clean-read") == Experiment::SrtCleanRead);
	// Old helpers may still pass the retired name. It must not select RELEASE_MEM.
	Check(ParseExperiment("overlap-fast") == Experiment::None);
	Check(std::string_view(ExperimentName()) == (release_mem ? "release-mem" : read_clean ? "read-fault-clean" :
	                                           attrib_clean ? "attrib-clean-read" : srt_clean ? "srt-clean-read" : "none"));
	SetExperiment(true);
	Check(ExperimentActive(Experiment::ReleaseMem) == release_mem);
	Check(ExperimentActive(Experiment::ReadFaultClean) == read_clean);
	Check(ExperimentActive(Experiment::AttribCleanRead) == attrib_clean);
	Check(ExperimentActive(Experiment::SrtCleanRead) == srt_clean);
	Check(!ExperimentActive(Experiment::None));
	SetExperiment(false);
	Check(!ExperimentActive(Experiment::ReleaseMem));
	Check(!ExperimentActive(Experiment::ReadFaultClean));
	Check(!ExperimentActive(Experiment::AttribCleanRead));
	Check(!ExperimentActive(Experiment::SrtCleanRead));
	const bool armed = Armed();
	// Disarmed builds never record and scopes leave thread-local state untouched.
	SetRecording(true);
	Check(Recording() == armed);
	if (!armed) {
		{
			ReasonScope reason {Reason::Flip};
			Check(Detail::current_reason == Reason::Other);
		}
		NoteDraw();
		RecordSubmit(1);
		RecordIdleWait(1, 20'000'001, true, 1);
		Check(Detail::pending_draws == 0);
		const auto text = Capture(false);
		Check(Contains(text, "\"submits\":[]") && Contains(text, "\"waits\":0"));
		std::puts("SubmitProfileTests: disarmed cases passed");
		return 0;
	}
	TestReadFaultAttribution();
	// Exact sampling period, shared fault/lock selection, TID roles, gate and
	// generation boundaries. No actual faults or sleeps are needed here.
	{
		using namespace Diagnostics;
		Common::SubmitProfile::Reset();
		SetRole(Role::Guest);
		sequences = {};
		for (unsigned i = 0; i < 256; ++i) {
			FaultScope fault(true);
			LockTimer lock(Metric::TrackingLock);
			lock.Acquired();
			fault.Handled();
		}
		auto* guest_row = GetRow();
		Check(guest_row->metrics[static_cast<size_t>(Metric::WriteFault)].calls == 256);
		Check(guest_row->metrics[static_cast<size_t>(Metric::WriteFault)].samples == 1);
		Check(guest_row->metrics[static_cast<size_t>(Metric::FaultTrackingLock)].calls == 256);
		Check(guest_row->metrics[static_cast<size_t>(Metric::FaultTrackingLock)].samples == 1);
		Check(!in_fault && !sample_fault);
		SetRole(Role::GuestGpu);
		sequences = {};
		for (unsigned i = 0; i < 256; ++i) { LockTimer lock(Metric::TextureLock); lock.Acquired(); }
		auto* gpu_row = GetRow();
		Check(gpu_row != guest_row && gpu_row->metrics[static_cast<size_t>(Metric::TextureLock)].samples == 1);
		WriteReadbackRequest();
		const auto diagnostics = Capture(false);
		Check(Contains(diagnostics, "\"role\":\"guest\"") && Contains(diagnostics, "\"role\":\"guest_gpu\""));
		Check(Contains(diagnostics, "\"site\":\"write_readback_request\",\"calls\":1"));
		SetRecording(false);
		{ FaultScope fault(true); LockTimer lock(Metric::TrackingLock); lock.Acquired(); fault.Handled(); }
		Check(gpu_row->metrics[static_cast<size_t>(Metric::WriteFault)].calls == 0);
		SetRecording(true);
		sequences[static_cast<size_t>(Metric::TrackingLock)] = 255;
		LockTimer crossing(Metric::TrackingLock);
		StartWindow(NowNs());
		crossing.Acquired();
		Check(gpu_row->metrics[static_cast<size_t>(Metric::TrackingLock)].dropped == 1);
		Check(gpu_row->metrics[static_cast<size_t>(Metric::TrackingLock)].samples == 0);
		SetRole(Role::Host);
	}
	Common::SubmitProfile::Reset(1000);
	auto& w = Detail::window;

	// Draws and dispatches go to the submit that carries them, by reason and PM4 origin.
	{
		OriginScope origin {Origin::GfxPm4};
		ReasonScope gfx {Reason::GfxSuspended};
		NoteDraw();
		NoteDraw();
		NoteDispatch();
		RecordSubmit(11);
		RecordSubmit(11); // Nothing recorded since the previous submit.
		{
			ReasonScope tick {Reason::WaitTick}; // E.g. a stream wrap inside a PM4 packet.
			NoteDraw();
			RecordSubmit(11);
		}
	}
	{
		ReasonScope flip {Reason::Flip};
		RecordSubmit(12);
	}
	Check(Detail::current_reason == Reason::Other && Detail::current_origin == Origin::Outside);
	const auto suspended = Row(Reason::GfxSuspended, Origin::GfxPm4);
	Check(w.submits[suspended] == 2 && w.draws[suspended] == 2 && w.dispatches[suspended] == 1);
	Check(w.submits_without_draw_or_dispatch[suspended] == 1);
	Check(w.submits[Row(Reason::WaitTick, Origin::GfxPm4)] == 1);
	Check(w.draws[Row(Reason::WaitTick, Origin::GfxPm4)] == 1);
	Check(w.submits_without_draw_or_dispatch[Row(Reason::Flip, Origin::Outside)] == 1);

	// Wait sites nest and restore; the timer attributes to the innermost scope.
	{
		WaitSiteScope outer {WaitSite::Finish};
		{
			WaitSiteScope inner {WaitSite::StreamWrap};
			WaitTimer timer {21};
		}
		Check(Detail::current_wait_site == WaitSite::Finish);
	}
	Check(w.waits[static_cast<size_t>(WaitSite::StreamWrap)] == 1);
	Check(w.waits[static_cast<size_t>(WaitSite::Finish)] == 0);
	{
		// Readback bytes follow the site carried by the caller; nothing copied still counts.
		WaitSiteScope site {WaitSite::ReadbackIndirectArgs};
		RecordReadbackBytes(16, 64);
		RecordReadbackBytes(16, 0);
		Check(CurrentWaitSite() == WaitSite::ReadbackIndirectArgs);
	}
	Check(w.readback_requested_bytes[static_cast<size_t>(WaitSite::ReadbackIndirectArgs)] == 32);
	Check(w.readback_copied_bytes[static_cast<size_t>(WaitSite::ReadbackIndirectArgs)] == 64);

	// Blocks: attributed once, cleared at the start of each processing, never guessed.
	BeginProcessing();
	NoteWaitRegMem(1, reinterpret_cast<const void*>(0x1000), 2, 3, 0xffffffff, 3, 32);
	RecordBlocked(1);
	RecordBlocked(0); // No cause noted since: unknown.
	NoteBlock(Block::WaitFlip);
	BeginProcessing(); // That stream completed; its cause must not leak to the next one.
	RecordBlocked(0);
	BeginProcessing();
	NoteBlock(Block::WaitFlip);
	RecordBlocked(0);
	Check(w.blocks[static_cast<size_t>(Block::WaitRegMem) * QueueKinds + 1] == 1);
	Check(w.blocks[static_cast<size_t>(Block::Unknown) * QueueKinds + 0] == 2);
	Check(w.blocks[static_cast<size_t>(Block::WaitFlip) * QueueKinds + 0] == 1);
	Check(w.wait_reg_mem_functions[3] == 1 && w.sample_count == 1);

	// Idle histogram boundaries: 0.4, 1.0, 15.6 and 40 ms (buckets <0.5, <1.5, <17, >=34).
	RecordIdleWait(2000, 2000 + 400'000, false, 31);
	RecordIdleWait(2000, 2000 + 1'000'000, true, 31);
	RecordIdleWait(2000, 2000 + 15'600'000, true, 31);
	RecordIdleWait(2000, 2000 + 40'000'000, true, 31);
	Check(w.idle_histogram[0] == 1 && w.idle_histogram[1] == 1 && w.idle_histogram[4] == 1);
	Check(w.idle_histogram[6] == 1 && w.idle_waits == 4 && w.idle_timeouts == 3);
	Check(w.idle_max_ns == 40'000'000);
	RecordReevaluation(1100, 1600);
	RecordReevaluation(0, 1600); // Never blocked: ignored.
	Check(w.reevaluations == 1 && w.reevaluation_ns == 500);

	// One JSON line with every section; the report starts a new window.
	const auto text = Capture(true, 5000);
	Check(text.rfind("[SUBMIT_PROFILE] {", 0) == 0 && text.back() == '\n');
	Check(Contains(text, "\"timer_1ms\":true"));
	Check(Contains(text, "{\"reason\":\"gfx_suspended\",\"origin\":\"gfx_pm4\",\"count\":2,"
	                     "\"draws\":2,\"dispatches\":1,\"without_draw_or_dispatch\":1,\"tid\":11,"
	                     "\"multi_thread\":false}"));
	Check(Contains(text, "{\"reason\":\"wait_tick\",\"origin\":\"gfx_pm4\",\"count\":1,"));
	Check(Contains(text, "{\"site\":\"stream_wrap\",\"count\":1,"));
	Check(Contains(text, "{\"site\":\"readback_indirect_args\",\"count\":0,\"ms\":0.000,\"max_ms\":0.000,"
	                     "\"tid\":0,\"multi_thread\":false,\"bytes_requested\":32,\"bytes_copied\":64}"));
	Check(Contains(text, "{\"block\":\"unknown\",\"queue\":\"gfx\",\"count\":2}"));
	Check(Contains(text, "\"histogram\":[1,1,0,0,1,0,1]"));
	Check(Contains(text, "\"address\":\"0x1000\",\"value\":\"0x2\""));
	Check(Contains(text, "\"dropped_cross_window\":{\"waits\":0,\"idle_waits\":0,\"reevaluations\":0}"));
	Check(w.submits[suspended] == 0 && w.idle_waits == 0 && w.sample_count == 0);
	const auto empty = Capture(false, 6000);
	Check(Contains(empty, "\"submits\":[]") && Contains(empty, "\"wait_reg_mem_samples\":[]"));

	// Durations that started before the current window are dropped, not mixed in.
	{
		WaitTimer old_wait {41};
		Reset(); // E.g. the timer gate toggled while this wait was blocked.
	}
	RecordIdleWait(10, NowNs(), true, 31);  // Started before the window.
	RecordReevaluation(10, NowNs());
	Check(w.waits[0] == 0 && w.idle_waits == 0 && w.reevaluations == 0);
	Check(w.dropped_waits == 1 && w.dropped_idle_waits == 1 && w.dropped_reevaluations == 1);

	// Pending per-thread state does not survive a new window (off -> on with work pending).
	Reset();
	NoteDraw();
	NoteBlock(Block::Rewind);
	SetRecording(false);
	NoteDraw(); // Inert while off.
	SetRecording(true);
	Reset();
	RecordSubmit(13);
	RecordBlocked(0);
	Check(w.draws[Row(Reason::Other, Origin::Outside)] == 0);
	Check(w.submits_without_draw_or_dispatch[Row(Reason::Other, Origin::Outside)] == 1);
	Check(w.blocks[static_cast<size_t>(Block::Unknown) * QueueKinds + 0] == 1);

	// Concurrent producers: totals are exact and multi-thread rows are flagged.
	Reset();
	std::vector<std::thread> threads;
	for (uint32_t t = 0; t < 4; ++t) {
		threads.emplace_back([t] {
			ReasonScope reason {Reason::GfxComplete};
			for (int i = 0; i < 1000; ++i) {
				NoteDraw();
				RecordSubmit(100 + t);
			}
		});
	}
	for (auto& thread: threads) thread.join();
	const auto complete = Row(Reason::GfxComplete, Origin::Outside);
	Check(w.submits[complete] == 4000 && w.draws[complete] == 4000);
	const auto concurrent = Capture(false);
	Check(Contains(concurrent, "\"count\":4000,\"draws\":4000,\"dispatches\":0,"
	                           "\"without_draw_or_dispatch\":0"));
	Check(Contains(concurrent, "\"multi_thread\":true"));

	// Reports racing with producers: every submit appears in exactly one report.
	Reset();
	std::atomic<bool> producing {true};
	std::vector<std::thread> racers;
	for (uint32_t t = 0; t < 4; ++t) {
		racers.emplace_back([t] {
			ReasonScope reason {Reason::Finish};
			for (int i = 0; i < 20000; ++i) {
				RecordSubmit(200 + t);
				Diagnostics::WriteReadbackRequest();
			}
		});
	}
	std::string reports;
	std::thread reporter([&] {
		while (producing.load()) reports += Capture(false);
	});
	for (auto& thread: racers) thread.join();
	producing = false;
	reporter.join();
	reports += Capture(false);
	uint64_t seen = 0;
	const std::string key = "\"reason\":\"finish\",\"origin\":\"outside_pm4\",\"count\":";
	for (auto at = reports.find(key); at != std::string::npos; at = reports.find(key, at + 1)) {
		seen += std::strtoull(reports.c_str() + at + key.size(), nullptr, 10);
	}
	Check(seen == 80000);
	seen = 0;
	const std::string diagnostic_key = "\"site\":\"write_readback_request\",\"calls\":";
	for (auto at = reports.find(diagnostic_key); at != std::string::npos;
	     at = reports.find(diagnostic_key, at + 1)) {
		seen += std::strtoull(reports.c_str() + at + diagnostic_key.size(), nullptr, 10);
	}
	Check(seen == 80000); // Stable per-TID rows do not lose events during report exchanges.

	// Recording off: hooks are inert.
	SetRecording(false);
	NoteDraw();
	RecordSubmit(1);
	RecordIdleWait(NowNs(), NowNs() + 1, true, 1);
	Check(w.submits[0] == 0 && w.idle_waits == 0);
	std::puts("SubmitProfileTests: all cases passed");
}
