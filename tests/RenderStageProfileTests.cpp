#include "common/renderStageProfile.h"
#include <cstdlib>
#include <thread>
#include <string>

using namespace Common::RenderStageProfile;
void Check(bool ok) { if (!ok) std::abort(); }

void TestProgramDiagnostics() {
	static_assert(std::is_empty_v<ProgramScope<false>>);
	static_assert(std::is_trivially_destructible_v<ProgramScope<false>>);
	ProgramSamplingSequence sequence;
	uint32_t parity = 0;
	for (unsigned block = 0; block < 32; ++block) {
		unsigned samples = 0;
		for (unsigned i = 0; i < 256; ++i) {
			if (sequence.Next()) { ++samples; parity |= 1u << (i & 1u); }
		}
		Check(samples == 1);
	}
	Check(parity == 3); // An alternating VS/PS stream must not always sample one side.
	ModuleSelections counts;
	ModuleSelectionHistory history;
	history.Observe(counts, 3, 2, 1, 11); // first
	history.Observe(counts, 3, 2, 1, 11); // repeat
	history.Observe(counts, 3, 2, 1, 12); // replaced at the same index: not a repeat
	history.Observe(counts, 3, 3, UINT64_MAX, 0); // miss breaks the sequence
	history.Observe(counts, 3, 2, 1, 12);
	history.Observe(counts, 4, 2, 1, 12); // append invalidates diagnostic history
	history.Observe(counts, 4, 2, 1, 12); // repeat
	history.Observe(counts, 3, 2, 1, 13); // erase
	history.Observe(counts, 3, 1, 0, 13); // moved index
	history.Observe(counts, 0, 0, UINT64_MAX, 0); // clear
	Check(counts.calls == 10 && counts.hits == 8 && counts.misses == 2);
	Check(counts.examined == 18 && counts.max_examined == 3);
	Check(counts.repeat_previous == 2 && counts.history_resets == 3);
	Check(counts.examined_histogram == std::array<uint64_t, 7>{1, 1, 7, 1, 0, 0, 0});
	ModuleSelectionHistory another;
	ModuleSelections independent;
	another.Observe(independent, 3, 2, 1, 11);
	Check(independent.repeat_previous == 0);
	for (const auto n: {4u, 5u, 8u, 9u, 16u, 17u})
		another.Observe(independent, n, n, UINT64_MAX, 0);
	Check(independent.examined_histogram == std::array<uint64_t, 7>{0, 0, 1, 1, 2, 2, 1});

	ProgramTiming timing;
	Detail::AddProgramSample(timing, 100, 35);
	Check(timing.samples == 1 && timing.inclusive_ns == 100 && timing.exclusive_ns == 65);
	Check(timing.samples_with_children == 1 && timing.max_inclusive_ns == 100 && timing.max_children_ns == 35);
	Detail::AddProgramSample(timing, 10, 11);
	Check(timing.exclusive_ns == 65); // No unsigned underflow.
	Detail::AddProgramSample(timing, 20, 0);
	Check(timing.samples_with_children == 2 && timing.samples == 3 && timing.max_children_ns == 35);
	// A rare wait can dominate the sampled estimate; expose it instead of
	// presenting that estimate as an exact inclusive total.
	Detail::AddProgramSample(timing, 1'000'000, 900'000);
	Check(timing.max_inclusive_ns == 1'000'000 && timing.max_children_ns == 900'000 &&
	      timing.samples_with_children == 3);
	ImageLookups lookups;
	for (const auto count: {0u, 1u, 2u, 4u, 5u, 8u, 9u, 16u, 17u}) lookups.Observe(count);
	Check(lookups.calls == 9 && lookups.candidates == 62 && lookups.max_candidates == 17);
	Check(lookups.candidate_histogram == std::array<uint64_t, 7>{1, 1, 1, 1, 2, 2, 1});
	Frame binding {Stage::ImageFind, 0}, target {Stage::TargetFindImage, 0};
	Frame sampled {Stage::Count, 0, 0, &binding};
	Frame nested_sample {Stage::Count, 0, 0, &sampled};
	Check(Detail::IsImageBindingLookup(&binding) && Detail::IsImageBindingLookup(&nested_sample));
	Check(!Detail::IsImageBindingLookup(nullptr) && !Detail::IsImageBindingLookup(&target));
	sampled.parent = &target;
	Check(!Detail::IsImageBindingLookup(&nested_sample));
	State report;
	BufferRebinds stream_sizes;
	for (const auto bytes : {1u, 64u, 65u, 256u, 257u, 1024u, 1025u, 4096u, 4097u, 16384u, 16385u})
		stream_sizes.ObserveStream(bytes);
	Check(stream_sizes.stream_copies == 11 && stream_sizes.stream_bytes == 43654 &&
	      stream_sizes.stream_max_bytes == 16385 &&
	      stream_sizes.stream_size_histogram == std::array<uint64_t, 6>{2, 2, 2, 2, 2, 1});
	report.window_start = 1;
	report.module_selections = counts;
	report.image_lookups = lookups;
	report.buffer_rebinds = {.calls = 11, .descriptors = 9, .null_descriptors = 1,
	    .obtain_calls = 8, .hint_hits = 5, .searches = 2, .stream_copies = 1,
	    .invalidation_calls = 4, .invalidation_matches = 3, .invalidation_changes = 2,
	    .matched_images = 6, .changed_images = 5, .heap_notifications = 3,
	    .flattened_uploads = 7, .flattened_bytes = 112, .shader_uploads = 8, .shader_bytes = 256};
	report.program_timings[0].calls = 512;
	report.vertex_index = {.draws = 4, .slots = 12, .nonempty_slots = 10, .merged_ranges = 6,
	    .obtain_calls = 6, .requested_bytes = 1024, .acquired_bytes = 960, .clipped_ranges = 1,
	    .slot_comparisons = 14, .index_calls = 4, .index_empty = 1, .index_obtain_calls = 2,
	    .index_stream_copies = 1, .index_bytes = 128};
	Detail::AddProgramSample(report.program_timings[0], 1000, 500);
	Detail::AddProgramSample(report.program_timings[0], 1000, 500);
	std::FILE* file = std::tmpfile();
	Check(file != nullptr);
	Detail::WriteProgramReport(file, report, 5'000'000'001ull);
	std::rewind(file);
	std::string text;
	char bytes[1024];
	while (const auto size = std::fread(bytes, 1, sizeof(bytes), file)) text.append(bytes, size);
	std::fclose(file);
	Check(text.find("[RENDER_STAGE_PROGRAMS]") != std::string::npos);
	Check(text.find("\"sample_period\":256") != std::string::npos);
	Check(text.find("\"estimated_inclusive_ms\":0.512,\"estimated_exclusive_ms\":0.256") != std::string::npos);
	Check(text.find("\"repeat_previous\":2") != std::string::npos);
	Check(text.find("\"examined_histogram\":[1,1,7,1,0,0,0]") != std::string::npos);
	Check(text.find("\"samples_with_children\":2,\"sample_children_ms\":0.001000") != std::string::npos);
	Check(text.find("\"sample_max_inclusive_ms\":0.001000,\"sample_max_children_ms\":0.000500") != std::string::npos);
	Check(text.find("\"image_lookup\":{\"scope\":\"prepare_bindings\",\"calls\":9,\"candidates\":62") != std::string::npos);
	Check(text.find("\"candidate_histogram\":[1,1,1,1,2,2,1]") != std::string::npos);
	Check(text.find("\"buffer_rebinds\":{\"calls\":11,\"descriptors\":9,\"null_descriptors\":1") != std::string::npos);
	Check(text.find("\"obtain_calls\":8,\"hint_hits\":5,\"searches\":2,\"stream_copies\":1") != std::string::npos);
	Check(text.find("\"invalidation_calls\":4,\"invalidation_matches\":3,\"invalidation_changes\":2") != std::string::npos);
	Check(text.find("\"matched_images\":6,\"changed_images\":5,\"heap_notifications\":3") != std::string::npos);
	Check(text.find("\"flattened_uploads\":7,\"flattened_bytes\":112,\"shader_uploads\":8,\"shader_bytes\":256") != std::string::npos);
	Check(text.find("\"stream_bytes\":0,\"stream_max_bytes\":0,\"stream_size_histogram\":[0,0,0,0,0,0]") != std::string::npos);
	Check(text.find("\"vertex_index\":{\"draws\":4,\"slots\":12,\"nonempty_slots\":10,\"merged_ranges\":6,\"obtain_calls\":6") != std::string::npos);
	Check(text.find("\"requested_bytes\":1024,\"acquired_bytes\":960,\"clipped_ranges\":1,\"slot_comparisons\":14") != std::string::npos);
	Check(text.find("\"index_calls\":4,\"index_empty\":1,\"index_obtain_calls\":2,\"index_stream_copies\":1,\"index_bytes\":128}}") != std::string::npos);
	for (size_t i = static_cast<size_t>(ProgramStage::SrtNative); i < ProgramNames.size(); ++i)
		Check(text.find(std::string("\"name\":\"") + ProgramNames[i] + '"') != std::string::npos);

	auto& state = Detail::state;
	const auto index = static_cast<size_t>(ProgramStage::ModuleSelect);
	const auto before = state.program_sequences[index].position;
	if (!Detail::DetailedEnabled()) {
		auto* active = state.active;
		for (unsigned i = 0; i < 512; ++i) {
			ProgramScope<true> gated {ProgramStage::ModuleSelect};
		}
		Check(state.active == active && state.program_timings[index].calls == 0 &&
		      state.program_sequences[index].position == before);
	}
	for (unsigned i = 0; i < 1000; ++i) {
		[[maybe_unused]] ProgramScope<false> disabled {ProgramStage::ModuleSelect};
	}
	Check(state.program_sequences[index].position == before && state.program_timings[index].calls == 0);
	// Every materialization/image lookup sub-stage is erased in the disabled specialization.
	// When armed it samples once per block, without changing full-rate accounting.
	for (size_t i = static_cast<size_t>(ProgramStage::SrtNative); i < ProgramNames.size(); ++i) {
		const auto stage = static_cast<ProgramStage>(i);
		const auto position = state.program_sequences[i].position;
		const auto calls = state.program_timings[i].calls;
		for (unsigned n = 0; n < 512; ++n) {
			[[maybe_unused]] ProgramScope<false> disabled {stage};
		}
		Check(state.program_sequences[i].position == position && state.program_timings[i].calls == calls);
		if (!Detail::DetailedEnabled()) {
			auto* active = state.active;
			for (unsigned n = 0; n < 512; ++n) {
				ProgramScope<true> gated {stage};
			}
			Check(state.active == active && state.program_sequences[i].position == position &&
			      state.program_timings[i].calls == calls && state.program_timings[i].samples == 0);
		}
		if (Detail::DetailedEnabled()) {
			Scope parent {Stage::Materialize};
			auto* active = state.active;
			const auto children = active->children;
			const auto samples = state.program_timings[i].samples;
			for (unsigned n = 0; n < 512; ++n) {
				ProgramScope<true> measured {stage};
			}
			Check(state.program_timings[i].calls == calls + 512 &&
			      state.program_timings[i].samples == samples + 2);
			Check(state.active == active && active->children == children);
		}
	}
	if (Detail::DetailedEnabled()) {
		{
			Scope parent {Stage::DrawPrograms};
			auto* active = state.active;
			const auto children = active->children;
			for (unsigned i = 0; i < 1024; ++i) {
				{
					ProgramScope<true> sample {ProgramStage::ModuleSelect};
					Check(state.active == active || state.active->parent == active);
				}
				Check(state.active == active);
			}
			// Sampling must not steal elapsed time from the full-rate parent.
			Check(active->children == children);
		}
		Check(state.program_timings[index].calls == 1024 && state.program_timings[index].samples == 4);
		state.program_sequences[index].position = 42;
		state.program_sequences[index].slot = 42;
		{
			ProgramScope<true> crossing {ProgramStage::ModuleSelect};
			++state.report_epoch;
		}
		Check(state.program_timings[index].dropped == 1 && state.program_timings[index].samples == 4);
		Check(state.active == nullptr);
		// A sampled wrapper forwards only its fully instrumented children's time.
		state.program_sequences[index].position = 42;
		state.program_sequences[index].slot = 42;
		{
			Scope parent {Stage::DrawPrograms};
			auto* active = state.active;
			uint64_t child_ns = 0;
			{
				ProgramScope<true> sample {ProgramStage::ModuleSelect};
				const auto before_child = state.totals[static_cast<size_t>(Stage::Compute)].inclusive_ns;
				{ Scope child {Stage::Compute}; }
				child_ns = state.totals[static_cast<size_t>(Stage::Compute)].inclusive_ns - before_child;
				Check(state.active != active && active->children == 0);
			}
			Check(state.active == active && active->children == child_ns);
		}
	}
}

int main() {
	static_assert(std::is_empty_v<ScopeFor<false>>);
	static_assert(std::is_trivially_destructible_v<ScopeFor<false>>);
	static_assert(std::is_same_v<ScopeFor<true>, Scope>);
	// No state or clock machinery even when a real profiled parent is active.
	{
		Scope parent {Stage::PrepareImages};
		auto* active = Detail::state.active;
		for (int i = 0; i < 1000; ++i) {
			[[maybe_unused]] ScopeFor<false> build {Stage::ImageDescBuild};
			[[maybe_unused]] ScopeFor<false> validate {Stage::ImageValidate};
			Check(Detail::state.active == active);
		}
	}
	Check(Detail::state.totals[static_cast<size_t>(Stage::ImageDescBuild)].calls == 0);
	Check(Detail::state.totals[static_cast<size_t>(Stage::ImageValidate)].calls == 0);
	State s;
	Frame root {Stage::Draw, 100};
	Frame child {Stage::Materialize, 120};
	Frame grandchild {Stage::ScalarTables, 130};
	Detail::Begin(s, root);
	Detail::Begin(s, child);
	Detail::Begin(s, grandchild);
	Detail::End(s, grandchild, 150);
	Detail::End(s, child, 170);
	Detail::End(s, root, 200);
	Check(s.active == nullptr);
	Check(s.totals[0].inclusive_ns == 100 && s.totals[0].exclusive_ns == 50);
	Check(s.totals[2].inclusive_ns == 50 && s.totals[2].exclusive_ns == 30);
	Check(s.totals[3].inclusive_ns == 20 && s.totals[3].exclusive_ns == 20);
	uint64_t sum = 0;
	for (auto t : s.totals) sum += t.exclusive_ns;
	Check(sum == 100);
	// Recursive occurrences of a stage must not double-count exclusive time.
	State recursive;
	Frame a {Stage::Draw, 0}, b {Stage::Draw, 10};
	Detail::Begin(recursive, a); Detail::Begin(recursive, b);
	Detail::End(recursive, b, 30); Detail::End(recursive, a, 50);
	Check(recursive.totals[0].calls == 2 && recursive.totals[0].exclusive_ns == 50);
	Detail::state.plans.calls = 19;
	std::thread thread([] { Check(Detail::state.plans.calls == 0); Detail::state.plans.calls = 7; });
	thread.join();
	Check(Detail::state.plans.calls == 19);
	const bool enabled = Detail::Enabled();
	const auto* detail = std::getenv("KYTY_RENDER_STAGE_DETAIL");
	Check(Detail::DetailedEnabled() ==
	      (enabled && detail != nullptr && std::string_view(detail) == "1"));
	{
		Scope parent {Stage::Draw};
		Scope nested {Stage::Compute};
		RecordPlan(2, 3, 4, true, false);
	}
	Check(Detail::state.active == nullptr);
	Check(Detail::state.totals[0].calls == (enabled ? 1u : 0u));
	Check(Detail::state.totals[1].calls == (enabled ? 1u : 0u));
	Check(Detail::state.plans.calls == (enabled ? 20u : 19u));
	Check(Detail::state.plans.reads == (enabled ? 2u : 0u));
	// These sub-stages are restricted to PrepareBindings, not heap/rebind callers.
	{
		Scope disabled {Stage::ImageDescBuild, false};
		Check(Detail::state.active == nullptr);
	}
	Check(Detail::state.totals[static_cast<size_t>(Stage::ImageDescBuild)].calls == 0);
	State images;
	Frame prepare {Stage::PrepareImages, 0}, build {Stage::ImageDescBuild, 10};
	Frame validate {Stage::ImageValidate, 20}, find {Stage::ImageFind, 55};
	Frame observe {Stage::ImageDescObserve, 80}, bind {Stage::ImageBind, 90};
	Detail::Begin(images, prepare);
	Detail::Begin(images, build);
	Detail::Begin(images, validate); Detail::End(images, validate, 30);
	Detail::End(images, build, 50);
	Detail::Begin(images, find); Detail::End(images, find, 80);
	Detail::Begin(images, observe); Detail::End(images, observe, 90);
	Detail::Begin(images, bind); Detail::End(images, bind, 95);
	Detail::End(images, prepare, 100);
	Check(images.totals[static_cast<size_t>(Stage::ImageDescBuild)].exclusive_ns == 30);
	Check(images.totals[static_cast<size_t>(Stage::ImageFind)].exclusive_ns == 25);
	sum = 0;
	for (const auto& t: images.totals) sum += t.exclusive_ns;
	Check(sum == 100 && images.active == nullptr);
	TestProgramDiagnostics();
	// Deterministic output sample also checks reset and the bounded log budget.
	s.window_start = 1;
	s.thread = 999;
	s.module_selections.calls = 123;
	s.image_lookups.Observe(17);
	s.buffer_rebinds = {.calls = 1, .obtain_calls = 3, .invalidation_changes = 2,
	    .flattened_bytes = 128, .shader_bytes = 64};
	s.buffer_rebinds.ObserveStream(512);
	s.vertex_index = {.draws = 2, .slots = 5, .merged_ranges = 3, .obtain_calls = 3,
	    .index_calls = 2, .index_bytes = 80};
	s.program_timings[0].calls = 256;
	s.program_sequences[0].position = 77;
	Detail::Report(s, 5'000'000'001ull);
	Check(s.window_start == 5'000'000'001ull && s.totals[0].calls == 0);
	Check(s.module_selections.calls == 0 && s.program_timings[0].calls == 0);
	Check(s.image_lookups.calls == 0 && s.image_lookups.candidates == 0 &&
	      s.image_lookups.candidate_histogram == std::array<uint64_t, 7>{});
	Check(s.buffer_rebinds.calls == 0 && s.buffer_rebinds.obtain_calls == 0 &&
	      s.buffer_rebinds.invalidation_changes == 0 && s.buffer_rebinds.flattened_bytes == 0 &&
	      s.buffer_rebinds.shader_bytes == 0);
	Check(s.buffer_rebinds.stream_bytes == 0 && s.buffer_rebinds.stream_max_bytes == 0 &&
	      s.buffer_rebinds.stream_size_histogram == std::array<uint64_t, 6>{});
	Check(s.vertex_index.draws == 0 && s.vertex_index.slots == 0 && s.vertex_index.merged_ranges == 0 &&
	      s.vertex_index.obtain_calls == 0 && s.vertex_index.index_calls == 0 && s.vertex_index.index_bytes == 0);
	Check(s.program_sequences[0].position == 77 && s.report_epoch == 1);
	Detail::output_lines = 240;
	Detail::Report(s, 10'000'000'001ull);
	Check(Detail::output_lines == 240);
	std::puts("RenderStageProfileTests: all cases passed");
}
