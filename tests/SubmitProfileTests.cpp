#include "common/submitProfile.h"
#include "common/readbackProfile.h"
#include "common/bufferUploadProfile.h"
#include "common/frameTimeline.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <semaphore>
#include <vector>

using namespace Common::SubmitProfile;
void Check(bool ok) { if (!ok) std::abort(); }

void TestFrameTimelineClock() {
	using Common::FrameTimeline::ConvertClock;
	uint64_t host = 0;
	Check(ConvertClock(4, 250, 10000, 255, 1000000.0, 1000, host) && host == 10010);
	Check(ConvertClock(248, 250, 10000, 255, 1000000.0, 1000, host) && host == 9998);
	Check(ConvertClock(9, 8, UINT64_MAX - 1, UINT64_MAX, 1000000.0, 1000, host) && host == UINT64_MAX);
	Check(!ConvertClock(10, 8, UINT64_MAX - 1, UINT64_MAX, 1000000.0, 1000, host));
	Check(!ConvertClock(7, 8, 0, UINT64_MAX, 1000000.0, 1000, host));
	Check(!ConvertClock(128, 0, 10000, 255, 1000000.0, 1000, host));
	Check(!ConvertClock(4, 0, 10000, 0, 1000000.0, 1000, host));
	Check(!ConvertClock(4, 0, 10000, 255, -1.0, 1000, host));
	Check(!ConvertClock(4, 0, 10000, 255, 1.0, 0, host));
}

void TestHostImportLeases() {
	namespace Host = Common::GuestHostImport;
	Check(Host::live.empty() && !Host::owner);
	std::shared_ptr<Host::Lease> lease;
	Host::owner = &lease;
	Host::drain = [](void* value) { static_cast<std::shared_ptr<Host::Lease>*>(value)->reset(); };
	Host::armed = true;
	const auto backing = [](Host::Span& span) {
		span.pointer = reinterpret_cast<void*>(uintptr_t{0x10000}); span.physical_offset = 0; return true;
	};
	lease = Host::Acquire(0x20000, 4096, backing);
	Check(lease && Host::retained_bytes == 4096);
	Check(!Host::Acquire(0x20800, 4096, backing)); // Virtual overlap.
	Check(!Host::Acquire(0x30000, 4096, backing)); // Distinct VA, same physical memory.
	const auto previous = lease->span.generation;
	{
		Host::Mutation remap; // Must drain outside registry mutex (Lease destructor locks it).
		Check(!lease && Host::retained_bytes == 0 && Host::generation > previous);
		Check(!Host::Acquire(0x20000, 4096, backing));
		{ Host::Mutation nested; Check(Host::mutations == 2); }
		Check(Host::mutations == 1);
	}
	Check(!Host::Acquire(0x20000, 4096, [](Host::Span&) { return false; }));
	lease = Host::Acquire(0x20000, 4096, backing);
	Check(lease && lease->span.generation > previous);
	lease.reset();
	// Empty mapping transactions do not take the registry mutex in A.
	{ std::lock_guard lock(Host::mutex); Host::Mutation idle_mapping; Check(Host::mutations == 1); }
	// Publication racing a new mapping must not let it slip through the empty
	// fast path. The mutation waits for publication and then drains the lease.
	struct Race {
		std::shared_ptr<Host::Lease> lease;
		std::binary_semaphore queried{0}, resume{0}, published{0};
	} race;
	Host::owner = &race;
	Host::drain = [](void* value) {
		auto& state = *static_cast<Race*>(value);
		state.published.acquire(); state.lease.reset();
	};
	std::thread publisher([&] {
		race.lease = Host::Acquire(0x20000, 4096, [&](Host::Span& span) {
			race.queried.release(); race.resume.acquire(); return backing(span);
		});
		Check(bool(race.lease)); race.published.release();
	});
	race.queried.acquire();
	std::thread mapper([] { Host::Mutation mutation; });
	while (Host::mutations.load() == Host::Publishing) std::this_thread::yield();
	race.resume.release(); publisher.join(); mapper.join();
	Check(!race.lease);
	Host::drain = nullptr; Host::owner = nullptr; Host::armed = false;
	Check(Host::retained_bytes == 0 && Host::live.empty() && Host::mutations == 0);
}

void TestBufferUploadProfile() {
	using namespace Common::BufferUploadProfile;
	PageHistory<1, 16> pages;
	std::array<uint8_t, 16> bytes {};
	Check(!pages.Observe(0x1000, bytes.data()).comparable);
	Check(pages.Observe(0x1000, bytes.data()).equal);
	bytes[7] = 1;
	const auto changed = pages.Observe(0x1000, bytes.data());
	Check(changed.comparable && !changed.equal && changed.changed == 1);
	Check(pages.Observe(0x2000, bytes.data()).evicted);
	Check(!pages.Observe(0x1000, bytes.data()).comparable);
	pages.Reset();
	Check(!pages.Observe(0x1000, bytes.data()).comparable);
	RangeHistory<1> ranges;
	Check(!ranges.Observe(0x1000, 4096, 7).comparable);
	Check(ranges.Observe(0x1000, 4096, 7).hash_equal);
	Check(!ranges.Observe(0x1000, 8192, 7).comparable); // Size is part of the key.
	Check(ranges.Observe(0x2000, 8192, 7).evicted);
	ranges.Reset();
	Check(!ranges.Observe(0x2000, 8192, 7).comparable);
	Check(Outside(0x1000, 8192, 0x1ffc, 8) == 8184);
	Check(Outside(0x1000, 4096, 0x3000, 4) == 4096);
	Check(Outside(0x1000, 4096, 0x800, 8192) == 0);
	Check(AfterDownload(Cause::CpuWriteFault) == Cause::CpuWriteFaultAfterGpuDownload);
	Check(AfterDownload(Cause::Invalidate) == Cause::AfterGpuDownload);

	// Exercise the real report and frame/segment resets without Vulkan or guest reads.
	auto* file = std::tmpfile();
	Check(file != nullptr);
	auto state = std::make_unique<Common::BufferUploadProfile::State>(file);
	std::array<uint8_t, 4096> payload {};
	Causes causes {};
	causes[static_cast<size_t>(Cause::CpuWriteFault)] = payload.size();
	const Request request {0x1008, 8, 0x1000, 16384, Use::Index, false, false};
	state->Sync(false);
	state->Observe(request, 0x1000, payload, 7, causes);
	state->Frame(false); // Partial initial frame; history survives the flip.
	state->Observe(request, 0x1000, payload, 7, causes);
	state->Frame(false);
	payload[4] = 1;
	state->Observe(request, 0x1000, payload, 7, causes); // Forced hash collision; exact pages differ.
	state->Frame(false);
	state->Sync(true);
	state->Observe(request, 0x1000, payload, 7, causes);
	state->Frame(true); // New segment must not reuse old comparison history.
	std::rewind(file);
	std::string reports;
	std::array<char, 8192> chunk {};
	while (const auto n = std::fread(chunk.data(), 1, chunk.size(), file)) reports.append(chunk.data(), n);
	Check(reports.find("\"frame\":1,\"segment\":1,\"persistent\":0,\"complete\":1") != std::string::npos);
	Check(reports.find("\"page_comparable_bytes\":4096,\"page_equal_bytes\":4096,\"changed_bytes\":0") != std::string::npos);
	Check(reports.find("\"page_comparable_bytes\":4096,\"page_equal_bytes\":0,\"changed_bytes\":1") != std::string::npos);
	Check(reports.find("\"outside_request_bytes\":4088") != std::string::npos);
	const auto new_segment = reports.find("\"segment\":2");
	Check(new_segment != std::string::npos && reports.find("\"hash_comparable_bytes\":0", new_segment) != std::string::npos);
	Check(reports.find("\"use\":\"index\"") != std::string::npos);
	Check(reports.find("\"top_buffers\":[{\"buffer_address\":\"0x1000\",\"buffer_size\":16384,\"uses\":[\"index\"]") != std::string::npos);
	Check(reports.find("\"cpu_write_fault\":4096") != std::string::npos);
	std::fseek(file, 0, SEEK_END); // Update stream: switch from reading back to report writes.
}

void TestIndirectDrawWork() {
	using namespace Common::ReadbackProfile::DrawWork;
	Totals summary;
	Key key {0x1000, 0x2000, 4096, 37, 64, 16, 1, 0, Phase::Discovery, Op::Upload};
	summary.Add(key, 4096); summary.Add(key, 4096);
	Check(summary.used == 1 && summary.entries[0].calls == 2 && summary.entries[0].bytes == 8192);
	auto changed = key; changed.phase = Phase::Execute; summary.Add(changed, 4096);
	changed = key; changed.record++; summary.Add(changed, 4096);
	changed = key; changed.format++; summary.Add(changed, 4096);
	Check(summary.used == 4);
	for (uint64_t i = 0; i < 128; ++i) { changed = key; changed.address += 0x10000 + i; summary.Add(changed, 0); }
	Check(summary.used == summary.entries.size() && summary.dropped == 4);
	summary.Add(key, 4); // Existing keys keep counting even at capacity.
	Check(summary.entries[0].calls == 3 && summary.entries[0].bytes == 8196);
	const auto previous_record = record; const auto previous_phase = phase;
	record = 0x1234; phase = Phase::Execute;
	{
		Scope disabled(0);
		Check(record == 0);
		const auto before = totals.used;
		Record(Op::Upload, 0, 0, 0, 0, 0, 0);
		Check(totals.used == before);
	}
	Check(record == 0x1234 && phase == Phase::Execute);
	record = previous_record; phase = previous_phase;
}

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
	TestFrameTimelineClock();
	Check(ParseExperiment("hot-unprotected") == Experiment::HotUnprotected);
	Check(static_cast<uint32_t>(ParseExperiment("hot-unprotected,guest-host-import")) ==
	    (static_cast<uint32_t>(Experiment::HotUnprotected) | static_cast<uint32_t>(Experiment::GuestHostImport)));
	Check(ParseExperiment("hot-unprotected-extra") == Experiment::None);
	Check(ParseExperiment("fault-coarsening") == Experiment::FaultCoarsening);
	Check(static_cast<uint32_t>(ParseExperiment("fault-coarsening,guest-host-import")) ==
	    (static_cast<uint32_t>(Experiment::FaultCoarsening) | static_cast<uint32_t>(Experiment::GuestHostImport)));
	Check(ParseExperiment("fault-coarsening-extra") == Experiment::None);
	if (argc > 1 && std::string_view(argv[1]) == "--guest-cpu-profile") {
#if defined(_WIN32)
		namespace Cpu = Common::GuestCpuProfile;
		Check(Cpu::FaultEnabled() && Cpu::DetailPeriod() == 1 && Cpu::AddressesEnabled());
		// Exact per-page counting, repeated writes, bounded overflow without
		// evicting old counts, and separate producers touching the same page.
		Cpu::FaultPages<1> pages, second_thread;
		pages.Add(0); pages.Add(4095); pages.Add(4096); pages.Add(65536); pages.Add(0);
		second_thread.Add(0);
		uint64_t first = 0, next = 0, other = 0;
		pages.ForEach([&](uint64_t page, uint64_t n) {
			if (page == 0) first += n; else if (page == 4096) next += n; else other += n;
		});
		Check(first == 3 && next == 1 && other == 0 && pages.dropped == 1);
		second_thread.ForEach([&](uint64_t page, uint64_t n) { Check(page == 0); first += n; });
		Check(first == 4);
		Check(Cpu::ContainsFaultPage(0x1000, 8192, 0x1000) && Cpu::ContainsFaultPage(0x1000, 8192, 0x2000));
		Check(!Cpu::ContainsFaultPage(0x1000, 8192, 0) && !Cpu::ContainsFaultPage(0x1000, 8192, 0x3000));
		Check(!Cpu::ContainsFaultPage(0x1001, 8192, 0x1000) && !Cpu::ContainsFaultPage(0x1000, 4095, 0x1000));
		Cpu::Register("guest-test", 0x10000, 0x20000);
		Check(Cpu::self && Cpu::self->ready && Cpu::self->alive && Cpu::self->creation &&
		    Cpu::self->guest_high == 0x30000 && Cpu::self->host_high > Cpu::self->host_low);
		auto* row = Cpu::self;
		Check(row->pages != nullptr);
		Cpu::Totals before {}, after {};
		Check(Cpu::Read(*row, before));
		{ Cpu::Fault disabled(true); disabled.Handled(); }
		Check(Cpu::Read(*row, after) && after.calls == 0);
		Cpu::recording = true;
		{
			Cpu::Fault fault(true, 0x51e432123);
			Cpu::Stage buffer(Cpu::Part::Buffer);
			{ Cpu::Stage protect(Cpu::Part::VirtualProtect); }
			{ Cpu::Stage lock(Cpu::Part::TrackingLock); lock.End(); lock.End(); }
			fault.Handled();
		}
		Check(Cpu::Read(*row, after));
		const auto delta = Cpu::Difference(after, before);
		uint64_t exclusive = 0; for (auto ticks: delta.parts) exclusive += ticks;
		Check(delta.calls == 1 && delta.detail_calls == 1 && exclusive == delta.ticks && delta.ticks == delta.detail_ticks);
		Check(delta.events[static_cast<size_t>(Cpu::Part::VirtualProtect)] == 1 &&
		    delta.events[static_cast<size_t>(Cpu::Part::TrackingLock)] == 1);
		{ Cpu::Fault read(false); read.Handled(); }
		{ Cpu::Fault unhandled(true); }
		{ Cpu::Fault boundary(true); boundary.Handled(); ++Cpu::epoch; }
		Cpu::recording = false;
		Check(Cpu::Read(*row, after) && after.calls == 1 && after.unhandled == 1 && after.dropped == 1 && !Cpu::current);
		uint64_t address_calls = 0;
		row->pages->ForEach([&](uint64_t page, uint64_t n) { Check(page == 0x51e432000); address_calls += n; });
		Check(address_calls == 1 && row->pages->dropped == 0);
		// A second window retains lifetime counters; subtract the baseline.
		Cpu::recording = true;
		{ Cpu::Fault fault(true, 0x51e432fff); fault.Handled(); }
		Cpu::recording = false;
		uint64_t second_window = 0;
		row->pages->ForEach([&](uint64_t page, uint64_t n) { Check(page == 0x51e432000); second_window += n; });
		Check(second_window - address_calls == 1);
		Cpu::Unregister(); Check(!row->alive && !Cpu::self);
		std::puts("Guest CPU profile: identity, stages, gates, page counts, overflow and window deltas passed");
		return 0;
#else
		return 77;
#endif
	}
	if (argc > 1 && std::string_view(argv[1]) == "--guest-host-import") {
#if defined(_WIN32)
		_putenv_s("KYTY_EXPERIMENT", "guest-host-import");
#else
		setenv("KYTY_EXPERIMENT", "guest-host-import", 1);
#endif
		Check(ParseExperiment("guest-host-import") == Experiment::GuestHostImport);
		Check(ParseExperiment("guest-host-import-extra") == Experiment::None);
		Check(ExperimentSelected(Experiment::GuestHostImport));
		SetExperiment(false); Check(!ExperimentActive(Experiment::GuestHostImport));
		SetExperiment(true); Check(ExperimentActive(Experiment::GuestHostImport));
		Check(std::string_view(ExperimentName()) == "guest-host-import");
		TestHostImportLeases();
		SetRecording(true); Check(Recording());
		AddExtra(Extra::HostImports, 2); AddExtra(Extra::HostReimports, 3);
		AddExtra(Extra::HostImportBytesAvoided, 4096); AddExtra(Extra::HostImportGuardWaits, 1);
		AddExtra(Extra::HostImportGuardNs, 2500000); AddExtra(Extra::HostImportFallbackBacking, 7);
		AddExtra(Extra::HostCandidateResource, 4); AddExtra(Extra::HostCandidateBackingDiscontiguous, 2);
		AddExtra(Extra::HostImportRetirements, 1); AddExtra(Extra::HostImportRestoreBytes, 4096);
		AddExtra(Extra::HostImportRetireNs, 12000); AddExtra(Extra::HostImportCreateAttempts, 2);
		AddExtra(Extra::HostImportExclusionCalls, 2048); AddExtra(Extra::HostImportExclusionSamples, 2);
		AddExtra(Extra::HostImportExclusionNs, 500); AddExtra(Extra::HostImportDescriptorReads, 3);
		AddExtra(Extra::HostImportExclusionCacheHits, 2000);
		AddExtra(Extra::HostImportRetireWaits, 2); AddExtra(Extra::HostImportRetireWaitNs, 3000000);
		AddExtra(Extra::HostImportRetireFlushes, 1);
		AddExtra(Extra::HostImportActivations, 1); AddExtra(Extra::HostImportActivationNs, 9000000);
		AddExtra(Extra::HostImportGuardRangeWaits, 1); AddExtra(Extra::HostImportGuardUnknownWaits, 2);
		AddExtra(Extra::HostImportGuardBypassed, 4); AddExtra(Extra::HostImportGuardLaneNs, 1000);
		AddExtra(Extra::HostImportDmaBytes, 8192); AddExtra(Extra::HostImportStagingBytes, 4096);
		AddExtra(Extra::HostImportCopyFallbackBytes, 1024);
		AddExtra(Extra::HostImportCopyFallbackNoImport, 2); AddExtra(Extra::HostImportCopyFallbackProfile, 1);
		Common::GuestHostImport::pending_read_bytes = 8192;
		Common::GuestHostImport::pending_known_bytes = 4096;
		Common::GuestHostImport::pending_unknown_bytes = 8192;
		Common::GuestHostImport::retained_bytes = 8192;
		const auto report = Capture(true);
		Check(Contains(report, "\"host_imports\":2") && Contains(report, "\"host_reimports\":3"));
		Check(Contains(report, "\"host_import_bytes_avoided\":4096") && Contains(report, "\"host_import_fallback_backing\":7"));
		Check(Contains(report, "\"host_import_guard_waits\":1") && Contains(report, "\"host_import_guard_ms\":2.500"));
		Check(Contains(report, "\"host_import_retained_bytes\":8192"));
		Check(Contains(report, "\"host_candidate_resource\":4") && Contains(report, "\"host_candidate_backing_discontiguous\":2"));
		Check(Contains(report, "\"host_import_retirements\":1") && Contains(report, "\"host_import_restore_bytes\":4096"));
		Check(Contains(report, "\"host_import_retire_ns\":12000") && Contains(report, "\"host_import_create_attempts\":2"));
		Check(Contains(report, "\"host_import_exclusion_calls\":2048") && Contains(report, "\"host_import_exclusion_samples\":2"));
		Check(Contains(report, "\"host_import_exclusion_ns\":500") && Contains(report, "\"host_import_descriptor_reads\":3"));
		Check(Contains(report, "\"host_import_exclusion_cache_hits\":2000"));
		Check(Contains(report, "\"host_import_retire_waits\":2") && Contains(report, "\"host_import_retire_wait_ns\":3000000"));
		Check(Contains(report, "\"host_import_retire_flushes\":1"));
		Check(Contains(report, "\"host_import_activations\":1") && Contains(report, "\"host_import_activation_ns\":9000000"));
		Check(Contains(report, "\"host_import_guard_range_waits\":1") && Contains(report, "\"host_import_guard_unknown_waits\":2"));
		Check(Contains(report, "\"host_import_guard_bypassed\":4") && Contains(report, "\"host_import_guard_lane_ns\":1000"));
		Check(Contains(report, "\"host_import_pending_read_bytes\":8192") && Contains(report, "\"host_import_pending_known_bytes\":4096"));
		Check(Contains(report, "\"host_import_pending_unknown_bytes\":8192"));
		Check(Contains(report, "\"host_import_dma_bytes\":8192") && Contains(report, "\"host_import_staging_bytes\":4096"));
		Check(Contains(report, "\"host_import_copy_fallback_bytes\":1024"));
		Check(Contains(report, "\"host_import_copy_fallback_no_import\":2") && Contains(report, "\"host_import_copy_fallback_profile\":1"));
		const auto reset = Capture(true);
		Check(Contains(reset, "\"host_imports\":0") && Contains(reset, "\"host_import_retained_bytes\":8192"));
		Common::GuestHostImport::retained_bytes = 0;
		Common::GuestHostImport::pending_read_bytes = Common::GuestHostImport::pending_known_bytes = Common::GuestHostImport::pending_unknown_bytes = 0;
		std::puts("SubmitProfileTests: host import gate, leases and counters passed");
		return 0;
	}
	if (argc > 1 && std::string_view(argv[1]) == "--outside-image-transfers") {
		namespace Work = Common::ReadbackProfile::DrawWork;
		SetRecording(true);
		Check(Common::ReadbackProfile::Enabled());
		{
			Work::Scope draw(0x1000);
			Work::Record(Work::Op::TargetBefore, 0x2000, 4096, 37, 64, 16, 1);
		}
		const auto before = Work::totals.used;
		// Disjoint images used by other draws were invisible in the old diagnostic.
		Work::Record(Work::Op::Detile, 0x9000, 4096, 37, 64, 16, 1, 2);
		Work::Record(Work::Op::Upload, 0x9000, 4096, 37, 64, 16, 1, 0, 4096);
		Check(Work::totals.used == before + 2);
		const auto& transfer = Work::totals.entries[before + 1];
		Check(transfer.key.phase == Work::Phase::OutsideDraw && transfer.key.record == 0 &&
		      transfer.bytes == 4096 && Work::totals.entries[before].key.flags == 2);
		// An overlap keeps both views, explicitly distinguished by phase/record.
		Work::Record(Work::Op::Upload, 0x2000, 4096, 37, 64, 16, 1, 0, 4096);
		Check(Work::totals.used == before + 4 &&
		      Work::totals.entries[before + 3].key.phase == Work::Phase::LaterOverlap &&
		      Work::totals.entries[before + 3].key.record == 0x1000);
		SetRecording(false);
		Work::Record(Work::Op::Upload, 0xa000, 4096, 37, 64, 16, 1, 0, 4096);
		Check(Work::totals.used == before + 4);
		std::puts("SubmitProfileTests: outside-draw transfer attribution passed");
		return 0;
	}
	TestIndirectDrawWork();
	if (argc > 1 && std::string_view(argv[1]) == "--buffer-upload-profile") {
		TestBufferUploadProfile();
		std::puts("SubmitProfileTests: buffer upload diagnostic passed");
		return 0;
	}
	const bool release_mem = argc > 1 && std::string_view(argv[1]) == "--experiment-release";
	const bool walker_control = argc > 1 && std::string_view(argv[1]) == "--experiment-walker-control";
	const bool indirect_count = argc > 1 && std::string_view(argv[1]) == "--experiment-indirect-count";
	const bool combined = argc > 1 && std::string_view(argv[1]) == "--experiment-combined";
	const bool retired = argc > 1 && std::string_view(argv[1]) == "--experiment-retired";
	const bool menu = argc > 1 && std::string_view(argv[1]) == "--experiment-menu";
	const bool no_gpu_timestamps = argc > 1 && std::string_view(argv[1]) == "--experiment-no-gpu-timestamps";
	const auto* experiment = no_gpu_timestamps ? "gpu-timestamps,no-gpu-timestamps" :
	    menu ? "no-walker-control-gpu,legacy-index-clamp,no-indirect-mesh-gpu" :
	    combined ? " indirect-count-gpu,release-mem, walker-control-gpu,indirect-mesh-gpu,retired,, " :
	    release_mem ? "release-mem" : walker_control ? "walker-control-gpu" : indirect_count ? "indirect-count-gpu" :
	    retired ? (argc > 2 ? argv[2] : "overlap-fast") : "";
#if defined(_WIN32)
	_putenv_s("KYTY_EXPERIMENT", experiment);
#else
	setenv("KYTY_EXPERIMENT", experiment, 1);
#endif
	Check(ParseExperiment("") == Experiment::None && ParseExperiment("typo") == Experiment::None);
	Check(ParseExperiment("release-mem") == Experiment::ReleaseMem);
	Check(ParseExperiment("image-resolve-reuse") == Experiment::None);
	Check(ParseExperiment("host-cached-stream") == Experiment::None);
	Check(ParseExperiment("small-buffer-persistent") == Experiment::None);
	Check(ParseExperiment("small-buffer-persistent-extra") == Experiment::None);
	Check(ParseExperiment("host-cached-stream,release-mem") == Experiment::ReleaseMem);
	Check(ParseExperiment("image-resolve-reuse,release-mem") == Experiment::ReleaseMem);
	Check(ParseExperiment("legacy-index-clamp") == Experiment::LegacyIndexClamp);
	Check(ParseExperiment("no-indirect-mesh-gpu") == Experiment::NoIndirectMeshGpu);
	Check(ParseExperiment("no-walker-control-gpu") == Experiment::NoWalkerControlGpu);
	Check(ParseExperiment("gpu-timestamps") == Experiment::None);
	Check(ParseExperiment("no-gpu-timestamps") == Experiment::NoGpuTimestamps);
	Check(ParseExperiment("gpu-timestamps,no-gpu-timestamps") == Experiment::NoGpuTimestamps);
	Check(ParseExperiment("no-gpu-timestamps-extra") == Experiment::None);
	Check(ParseExperiment("indirect-mesh-lazy-mark") == Experiment::None);
	Check(static_cast<uint32_t>(ParseExperiment(" no-walker-control-gpu,legacy-index-clamp,no-indirect-mesh-gpu,,legacy-index-clamp ")) == 28);
	Check(ParseExperiment("legacy-index-clamp-extra,no-walker-control-gpu-extra") == Experiment::None);
	Check(ParseExperiment("vs-attrib-reuse") == Experiment::None);
	Check(static_cast<uint32_t>(ParseExperiment("vs-attrib-reuse,release-mem,indirect-count-gpu")) == 3);
	Check(ParseExperiment("vs-attrib-reuse-extra") == Experiment::None);
	Check(ParseExperiment("clean-read-early-reject") == Experiment::None);
	Check(static_cast<uint32_t>(ParseExperiment("clean-read-early-reject,release-mem,indirect-count-gpu")) == 3);
	Check(ParseExperiment("clean-read-early-reject-extra") == Experiment::None);
	Check(ParseExperiment("overlap-intervals") == Experiment::None);
	Check(static_cast<uint32_t>(ParseExperiment("overlap-intervals,release-mem,indirect-count-gpu")) == 3);
	Check(ParseExperiment("overlap-intervals-extra") == Experiment::None);
	Check(ParseExperiment("image-binding-inplace") == Experiment::None);
	Check(static_cast<uint32_t>(ParseExperiment("image-binding-inplace,release-mem,indirect-count-gpu")) == 3);
	Check(ParseExperiment("image-binding-inplace-extra") == Experiment::None);
	Check(ParseExperiment("walker-control-gpu") == Experiment::None);
	Check(ParseExperiment("indirect-count-gpu") == Experiment::IndirectCountGpu);
	Check(ParseExperiment("indirect-mesh-gpu") == Experiment::None);
	Check(ParseExperiment("walker-control-gpu,indirect-mesh-gpu") == Experiment::None);
	Check(ParseExperiment("indirect-mesh-gpu,indirect-count-gpu,walker-control-gpu") == Experiment::IndirectCountGpu);
	Check(ParseExperiment("walker-control-gpu,indirect-count-gpu") == Experiment::IndirectCountGpu);
	Check(ParseExperiment(" indirect-count-gpu,walker-control-gpu,walker-control-gpu,,typo ") == Experiment::IndirectCountGpu);
	Check(static_cast<uint32_t>(ParseExperiment("walker-control-gpu,release-mem,indirect-count-gpu")) == 3);
	Check(ParseExperiment("walker-control-gpu-extra,not-release-mem,, ") == Experiment::None);
	// Old helpers may keep these names. None may activate RELEASE_MEM or
	// disable the promoted clean reads when experiment.trigger is toggled.
	for (const auto name: {"overlap-intervals", "obtain-region-generation", "walker-control-gpu", "indirect-mesh-gpu", "buffer-dirty-query", "overlap-fast", "read-fault-clean", "attrib-clean-read", "srt-clean-read", "image-desc-reuse", "image-find-hint"})
		Check(ParseExperiment(name) == Experiment::None);
	Check(ParseExperiment("obtain-region-generation,indirect-count-gpu") == Experiment::IndirectCountGpu);
	Check(std::string_view(ExperimentName()) == (no_gpu_timestamps ? "no-gpu-timestamps" : menu ? "legacy-index-clamp,no-indirect-mesh-gpu,no-walker-control-gpu" : combined ? "release-mem,indirect-count-gpu" :
	    release_mem || indirect_count ? experiment : "none"));
	SetExperiment(true);
	Check(GpuTimestampsEnabled() == !no_gpu_timestamps);
	Check(ExperimentActive(Experiment::ReleaseMem) == (release_mem || combined));
	Check(ExperimentActive(Experiment::IndirectCountGpu) == (indirect_count || combined));
	Check(!ExperimentActive(Experiment::None));
	for (const auto diagnostic : {Experiment::LegacyIndexClamp, Experiment::NoIndirectMeshGpu, Experiment::NoWalkerControlGpu})
		Check(ExperimentActive(diagnostic) == menu);
	SetExperiment(false);
	Check(GpuTimestampsEnabled()); // Default, independent of profiling and the old opt-in name.
	for (const auto diagnostic : {Experiment::LegacyIndexClamp, Experiment::NoIndirectMeshGpu, Experiment::NoWalkerControlGpu})
		Check(!ExperimentActive(diagnostic));
	Check(!ExperimentActive(Experiment::ReleaseMem));
	Check(!ExperimentActive(Experiment::IndirectCountGpu));
	Check(ExperimentSelected(Experiment::IndirectCountGpu) == (indirect_count || combined));
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
	AddExtra(Extra::ImageDescReuseHits);
	Check(Detail::window.extra[static_cast<size_t>(Extra::ImageDescReuseHits)] == 0);
	SetRecording(true);
	AddExtra(Extra::ImageDescReuseHits, 7);
	AddExtra(Extra::ImageDescReuseMisses, 2);
	AddExtra(Extra::ImageDescHistoryCapacityResets);
	const auto image_report = Capture(false);
	Check(Contains(image_report, "\"image_desc_reuse_hits\":7") &&
	      Contains(image_report, "\"image_desc_reuse_misses\":2") &&
	      Contains(image_report, "\"image_desc_history_capacity_resets\":1"));
	Reset();
	// Rejection diagnostics must be available even when the experiment never
	// enters its attempt/fallback branch (e.g. a packet without a count address).
	SetRecording(false);
	AddExtra(Extra::IndirectNoCount);
	Check(Detail::window.extra[static_cast<size_t>(Extra::IndirectNoCount)] == 0);
	SetRecording(true);
	AddExtra(Extra::IndirectPackets, 3);
	AddExtra(Extra::IndirectNoCount, 2);
	AddExtra(Extra::IndirectPatchInstance, 2);
	AddExtra(Extra::IndirectCountAttempts);
	AddExtra(Extra::IndirectArgsUnmapped);
	AddExtra(Extra::IndirectCountGpuFallbacks);
	const auto indirect_report = Capture(false);
	Check(Contains(indirect_report, "\"indirect_packets\":3") &&
	      Contains(indirect_report, "\"indirect_no_count\":2") &&
	      Contains(indirect_report, "\"indirect_patch_instance\":2") &&
	      Contains(indirect_report, "\"indirect_count_attempts\":1") &&
	      Contains(indirect_report, "\"indirect_args_unmapped\":1") &&
	      Contains(indirect_report, "\"indirect_count_gpu_fallbacks\":1"));
	Reset();
	SetRecording(false);
	RecordIndirectIndices(9, 3, 6, 1, 6);
	Check(Detail::window.extra[static_cast<size_t>(Extra::IndirectIndexSamples)] == 0);
	SetRecording(true);
	RecordIndirectIndices(9, 3, 6, 1, 6);
	RecordIndirectIndices(100, UINT32_MAX, 1, 0, 1);
	const auto index_report = Capture(false);
	Check(Contains(index_report, "\"indirect_index_samples\":2") &&
	      Contains(index_report, "\"indirect_index_requested\":109") &&
	      Contains(index_report, "\"indirect_index_available\":3") &&
	      Contains(index_report, "\"indirect_index_valid\":3") &&
	      Contains(index_report, "\"indirect_index_issued\":7") &&
	      Contains(index_report, "\"indirect_index_zero_instances\":1"));
	Reset();
	std::puts("SubmitProfileTests: all cases passed");
}
