#include "common/renderStageProfile.h"
#include <cstdlib>
#include <thread>

using namespace Common::RenderStageProfile;
void Check(bool ok) { if (!ok) std::abort(); }
int main() {
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
	// Deterministic output sample also checks reset and the bounded log budget.
	s.window_start = 1;
	s.thread = 999;
	Detail::Report(s, 5'000'000'001ull);
	Check(s.window_start == 5'000'000'001ull && s.totals[0].calls == 0);
	Detail::output_lines = 240;
	Detail::Report(s, 10'000'000'001ull);
	Check(Detail::output_lines == 240);
	std::puts("RenderStageProfileTests: all cases passed");
}
