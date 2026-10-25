#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <mutex>

#include "common/submitProfile.h"

// Opt-in, bounded host-side profiling. Exclusive times subtract instrumented
// children on the SAME thread; they are not GPU execution times or CPU cycles.
// No per-event allocations, atomics or log writes on the enabled hot path.
namespace Common::RenderStageProfile {
enum class Stage {
	Draw, Compute, Materialize, ScalarTables, PrepareBindings, FindBuffers,
	RebindBuffers, RebindImages, CommitBindings, TextureUpload, GpuWait, Submit, BeginNext,
	DynamicState, DrawLock, DrawPreChecks, DrawVertexIndex, DrawPipeline, DrawTargets, DrawEmit,
	DrawMutex, DrawPrepareState, PrepareGraphicsBindings, ResetBindings,
	BindingsOverlap, BindingsImageHeap, BindingsTransitions, BindingsBuild, BindingsEmit,
	DrawHeapBegin, DrawPrograms, ProgramKeyLookup, DrawResolveTargets, TargetFindImage,
	PrepareImages, PrepareSamplers, PrepareShaderData,
	Count
};
inline constexpr std::array Names {
	"draw", "compute", "materialize", "scalar_tables", "prepare_bindings", "find_buffers",
	"rebind_buffers", "rebind_images", "commit_bindings", "texture_upload", "gpu_wait",
	"submit", "begin_next", "dynamic_state", "draw_lock", "draw_pre_checks", "draw_vertex_index",
	"draw_pipeline", "draw_targets", "draw_emit", "draw_mutex", "draw_prepare_state",
	"prepare_graphics_bindings", "reset_bindings", "bindings_overlap", "bindings_image_heap",
	"bindings_transitions", "bindings_build", "bindings_emit",
	"draw_heap_begin", "draw_programs", "program_key_lookup", "draw_resolve_targets", "target_find_image",
	"prepare_images", "prepare_samplers", "prepare_shader_data"
};
static_assert(Names.size() == static_cast<size_t>(Stage::Count));
struct Totals { uint64_t calls = 0, inclusive_ns = 0, exclusive_ns = 0; };
struct Plans {
	uint64_t calls = 0, reads = 0, blocks = 0, descriptors = 0;
	uint64_t with_control = 0, capture = 0, native_available = 0;
	// Plan categories by calls and walker time. Since d7856753 all valid plans use
	// category 0, including capture/control. The capture/with_control totals still distinguish them.
	// 0 native, 1 capture only + JIT-compatible shape, 2 capture only + other shape,
	// 3 control only, 4 capture and control, 5 neither but no native code.
	std::array<uint64_t, 6> category_calls {}, category_ns {};
};
struct Frame {
	Stage stage;
	uint64_t start, children = 0;
	Frame* parent = nullptr;
};
struct State {
	std::array<Totals, Names.size()> totals {};
	Plans plans;
	Frame* active = nullptr;
	uint64_t window_start = 0, thread = 0;
};
namespace Detail {
inline std::atomic<uint64_t> next_thread {1};
inline std::mutex output_mutex;
inline uint32_t output_lines = 0; // protected by output_mutex
inline thread_local State state;
inline bool Enabled() {
	static const bool enabled = [] {
		const auto* value = std::getenv("KYTY_RENDER_STAGE_PROFILE");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
	}();
	return enabled;
}
inline uint64_t Now() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	    std::chrono::steady_clock::now().time_since_epoch()).count());
}
inline void Begin(State& state, Frame& frame) {
	frame.parent = state.active;
	state.active = &frame;
}
inline void End(State& state, Frame& frame, uint64_t end) {
	const auto elapsed = end - frame.start;
	auto& total = state.totals[static_cast<size_t>(frame.stage)];
	++total.calls;
	total.inclusive_ns += elapsed;
	total.exclusive_ns += elapsed - frame.children;
	state.active = frame.parent;
	if (frame.parent != nullptr) frame.parent->children += elapsed;
}
inline void Report(State& state, uint64_t now) {
	if (now - state.window_start < 5'000'000'000ull) return;
	std::scoped_lock lock(output_mutex);
	// With KYTY_SUBMIT_PROFILE armed, only windows recorded while its gate is open are written,
	// so loading screens do not spend the line budget. Maximum 240 records per run.
	const bool gated_out = Common::SubmitProfile::Armed() && !Common::SubmitProfile::Recording();
	if (!gated_out && output_lines < 240) {
		++output_lines;
		std::fprintf(stderr, "[RENDER_STAGE] {\"thread\":%llu,\"end_ns\":%llu,\"interval_ms\":%.3f,\"tid\":%u,\"role\":\"%s\",\"stages\":[",
		    static_cast<unsigned long long>(state.thread), static_cast<unsigned long long>(now),
		    (now - state.window_start) / 1e6, Common::SubmitProfile::Diagnostics::ThreadId(),
		    Common::SubmitProfile::Diagnostics::RoleNames[static_cast<size_t>(Common::SubmitProfile::Diagnostics::role)]);
		bool comma = false;
		for (size_t i = 0; i < Names.size(); ++i) {
			const auto& t = state.totals[i];
			if (t.calls == 0) continue;
			std::fprintf(stderr, "%s{\"name\":\"%s\",\"calls\":%llu,\"inclusive_ms\":%.3f,\"exclusive_ms\":%.3f}",
			    comma ? "," : "", Names[i], static_cast<unsigned long long>(t.calls),
			    t.inclusive_ns / 1e6, t.exclusive_ns / 1e6);
			comma = true;
		}
		const auto& p = state.plans;
		std::fprintf(stderr, "],\"plans\":{\"calls\":%llu,\"reads\":%llu,\"blocks\":%llu,\"descriptors\":%llu,\"with_control\":%llu,\"capture\":%llu,\"native_available\":%llu}}\n",
		    static_cast<unsigned long long>(p.calls), static_cast<unsigned long long>(p.reads),
		    static_cast<unsigned long long>(p.blocks), static_cast<unsigned long long>(p.descriptors),
		    static_cast<unsigned long long>(p.with_control), static_cast<unsigned long long>(p.capture),
		    static_cast<unsigned long long>(p.native_available));
		const auto& c = p.category_calls;
		const auto& n = p.category_ns;
		std::fprintf(stderr, "[RENDER_STAGE_PLANS] {\"thread\":%llu,\"calls\":[%llu,%llu,%llu,%llu,%llu,%llu],"
		    "\"ms\":[%.3f,%.3f,%.3f,%.3f,%.3f,%.3f]}\n",
		    static_cast<unsigned long long>(state.thread),
		    static_cast<unsigned long long>(c[0]), static_cast<unsigned long long>(c[1]),
		    static_cast<unsigned long long>(c[2]), static_cast<unsigned long long>(c[3]),
		    static_cast<unsigned long long>(c[4]), static_cast<unsigned long long>(c[5]),
		    n[0] / 1e6, n[1] / 1e6, n[2] / 1e6, n[3] / 1e6, n[4] / 1e6, n[5] / 1e6);
		std::fflush(stderr);
	}
	state.totals = {};
	state.plans = {};
	state.window_start = now;
}
} // namespace Detail

class Scope {
public:
	explicit Scope(Stage stage): m_frame {stage, 0} {
		if (!Detail::Enabled()) return;
		m_state = &Detail::state;
		m_frame.start = Detail::Now();
		if (m_state->window_start == 0) {
			m_state->window_start = m_frame.start;
			m_state->thread = Detail::next_thread.fetch_add(1, std::memory_order_relaxed);
		}
		Detail::Begin(*m_state, m_frame);
	}
	~Scope() {
		if (m_state == nullptr) return;
		const auto now = Detail::Now();
		Detail::End(*m_state, m_frame, now);
		if (m_state->active == nullptr) Detail::Report(*m_state, now);
	}
	Scope(const Scope&) = delete;
	Scope& operator=(const Scope&) = delete;
private:
	State* m_state = nullptr;
	Frame m_frame;
};
inline void RecordPlan(uint64_t reads, uint64_t blocks, uint64_t descriptors,
                       bool capture, bool native_available) {
	if (!Detail::Enabled()) return;
	auto& p = Detail::state.plans;
	++p.calls;
	p.reads += reads;
	p.blocks += blocks;
	p.descriptors += descriptors;
	p.with_control += blocks != 0;
	p.capture += capture;
	p.native_available += native_available;
}
inline bool PlanProfileEnabled() {
	return Detail::Enabled();
}
inline void RecordPlanCategory(uint32_t category, uint64_t ns) {
	if (!Detail::Enabled() || category >= 6) return;
	auto& p = Detail::state.plans;
	++p.category_calls[category];
	p.category_ns[category] += ns;
}
} // namespace Common::RenderStageProfile
