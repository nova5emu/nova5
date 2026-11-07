#include "graphics/host_gpu/renderer/drawPrep/bindingPlan.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace Libs::Graphics;
using namespace Libs::Graphics::ShaderRecompiler::IR;

static void Check(bool ok, const char* what) {
	if (!ok) { std::fprintf(stderr, "DrawPrepBindingPlanTests: %s\n", what); std::abort(); }
}

static DescriptorValue Buffer(uint64_t address, uint32_t stride, uint32_t records) {
	DescriptorValue value;
	value.dword_count = 4;
	value.dwords[0] = static_cast<uint32_t>(address);
	value.dwords[1] = static_cast<uint32_t>(address >> 32u) | (stride << 16u);
	value.dwords[2] = records;
	return value;
}

int main() {
	BindingLayout layout;
	layout.user_data_registers = {103, 100, 103}; // sparse order and a repeated register
	layout.memory_offset_dword = 3;
	layout.memory_offset_count = 4;
	layout.buffer_word_count = 4;
	layout.lod_stats_count = 2;
	ResourceSnapshot resources;
	resources.user_data = {0x12345678, 2, 3, 0xabcdef01};
	resources.buffers = {Buffer(0x123456789abc, 0x3fff, UINT32_MAX), Buffer(0x2000, 0, 13),
	                     Buffer(0, 16, 99), Buffer(0x3000, 16, 0)};
	DescriptorValue image;
	image.dword_count = 8;
	image.dwords[3] = 15u << 12u;
	image.dwords[5] = (1u << 25u) | (0xfffu << 8u);
	image.dwords[6] = 0xfe;
	resources.images = {image, image};
	resources.images[1].dwords[5] &= ~(1u << 25u);
	DrawPrep::StageBindingPlan plan;
	const auto build = [&] { return DrawPrep::BuildStageBindingPlan(layout, 100, 4, resources, plan); };
	Check(build() && plan.valid, "valid snapshot refused");
	Check(plan.user_data == std::vector<uint32_t>({0xabcdef01, 0x12345678, 0xabcdef01}), "user register order/base");
	Check(plan.ranges[0] == DrawPrep::StageBindingPlan::Range {0x123456789abc, 0x3ffeffffc001ull},
	      "48-bit address / wide stride product");
	Check(plan.ranges[1] == DrawPrep::StageBindingPlan::Range {0x2000, 13}, "zero stride means byte count");
	Check(plan.ranges[2] == DrawPrep::StageBindingPlan::Range {} &&
	      plan.ranges[3] == DrawPrep::StageBindingPlan::Range {}, "null address / empty range");
	Check(plan.lod_stats == std::vector<uint32_t>({0x80fffffeu, 0}), "LOD enable, base level, ID and warning bits");

	// The plan owns its outputs; source mutation cannot silently mutate a prepared plan.
	const auto old = plan;
	resources.user_data[3] = 55;
	resources.buffers[0] = Buffer(0x9000, 4, 7);
	resources.images[0].dwords[6] = 3;
	Check(plan.user_data == old.user_data && plan.ranges == old.ranges && plan.lod_stats == old.lod_stats,
	      "borrowed output storage");
	Check(build() && plan.user_data[0] == 55 && plan.ranges[0].address == 0x9000 &&
	      (plan.lod_stats[0] & 255u) == 3, "scratch reused stale values");

	resources.buffers[3].dword_count = 3; // refusal after several outputs were written
	Check(!build() && !plan.valid, "short V# retained a valid partial plan");
	resources.buffers[3].dword_count = 4;
	resources.images[1].dword_count = 7;
	Check(!build() && !plan.valid, "short T# accepted");
	resources.images[1].dword_count = 8;
	layout.user_data_registers[1] = 99;
	Check(!build() && !plan.valid, "register below base accepted");
	layout.user_data_registers[1] = 104;
	Check(!build() && !plan.valid, "register outside snapshot accepted");
	layout.user_data_registers[1] = 100;
	layout.memory_offset_dword = 2;
	Check(!build(), "user words overlap runtime offset fields");
	layout.memory_offset_dword = UINT32_MAX;
	Check(!build(), "overflowing layout accepted");
	layout.memory_offset_dword = 3;
	layout.lod_stats_count = 3;
	Check(!build(), "missing image accepted");
	layout.lod_stats_count = 2;
	Check(!DrawPrep::BuildStageBindingPlan(layout, 100, 5, resources, plan), "missing buffer accepted");
	Check(build(), "valid input failed after refusals");

	// Independent workers can share immutable inputs, but never their output scratch.
	const auto expected = plan;
	std::atomic_bool equal {true};
	std::vector<std::thread> workers;
	for (unsigned thread = 0; thread < 8; ++thread) workers.emplace_back([&] {
		DrawPrep::StageBindingPlan output;
		for (unsigned iteration = 0; iteration < 1000; ++iteration) {
			if (!DrawPrep::BuildStageBindingPlan(layout, 100, 4, resources, output) ||
			    output.ranges != expected.ranges || output.user_data != expected.user_data ||
			    output.lod_stats != expected.lod_stats) equal.store(false);
		}
	});
	for (auto& worker: workers) worker.join();
	Check(equal.load(), "concurrent pure preparation differs");
	Check(DrawPrep::BuildStageBindingPlan(BindingLayout {}, 0, 0, ResourceSnapshot {}, plan) &&
	      plan.valid && plan.ranges.empty() && plan.user_data.empty() && plan.lod_stats.empty(),
	      "empty stage retained previous outputs");
	plan.Reset();
	Check(!plan.valid, "reset did not invalidate");
	std::puts("DrawPrepBindingPlanTests: passed");
}
