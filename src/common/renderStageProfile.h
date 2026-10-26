#pragma once

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <mutex>
#include <type_traits>

#include "common/submitProfile.h"

// Opt-in, bounded host-side profiling. Exclusive times subtract instrumented
// children on the SAME thread; they are not GPU execution times or CPU cycles.
// No per-event allocations, atomics or log writes on the enabled hot path.
// KYTY_RENDER_STAGE_PROFILE=1 enables the full-rate stages. Additionally set
// KYTY_RENDER_STAGE_DETAIL=1 for sampled program/image lookup diagnostics and
// [RENDER_STAGE_PROGRAMS]. The detail flag alone does not enable profiling.
namespace Common::RenderStageProfile {
enum class Stage {
	Draw, Compute, Materialize, ScalarTables, PrepareBindings, FindBuffers,
	RebindBuffers, RebindImages, CommitBindings, TextureUpload, GpuWait, Submit, BeginNext,
	DynamicState, DrawLock, DrawPreChecks, DrawVertexIndex, DrawPipeline, DrawTargets, DrawEmit,
	DrawMutex, DrawPrepareState, PrepareGraphicsBindings, ResetBindings,
	BindingsOverlap, BindingsImageHeap, BindingsTransitions, BindingsBuild, BindingsEmit,
	DrawHeapBegin, DrawPrograms, ProgramKeyLookup, DrawResolveTargets, TargetFindImage,
	PrepareImages, PrepareSamplers, PrepareShaderData,
	ImageDescBuild, ImageFind, ImageBind, ImageValidate, ImageDescObserve,
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
	"prepare_images", "prepare_samplers", "prepare_shader_data",
	"image_desc_build", "image_find", "image_bind", "image_validate", "image_desc_observe"
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

enum class ProgramStage {
	VertexPrepare, PixelPrepare, ModuleMaterialize, ModuleSelect,
	SrtNative, SrtCapture, MaterializeControl, MaterializeBuffers, MaterializeImages,
	MaterializeSamplers, MaterializeSpecialization, MaterializeDiagnostic,
	ImageLookupValidate, ImageLookupLock, ImageLookupCandidates, ImageLookupMatch,
	ImageLookupOverlap, ImageLookupFinalize, ImageLookupClear,
	RebindObtain, RebindInvalidate, RebindFlattenedUpload, RebindShaderUpload,
	RebindDescriptors, RebindSetup,
	VertexSizes, VertexRanges, VertexObtain, VertexSlots, IndexPrepare,
	// Non-tessellated PrepareProgram(VS), including its NGG/merged entry.
	// Attrib/reset/detect apply only to the ordinary vertex fetch path.
	VsMapLock, VsMapLookup, VsParams, VsInputReset, VsAttrib, VsDetectBuffers,
	// GetProgram plus binding the selected module (PS also collects its MRT mask).
	GraphicsPixelModule, GraphicsVertexModules, Count
};
inline constexpr std::array ProgramNames {
	"vertex_prepare", "pixel_prepare", "module_materialize", "module_select",
	"srt_native", "srt_capture", "materialize_control", "materialize_buffers", "materialize_images",
	"materialize_samplers", "materialize_specialization", "materialize_diagnostic",
	"image_lookup_validate", "image_lookup_lock", "image_lookup_candidates", "image_lookup_match",
	"image_lookup_overlap", "image_lookup_finalize", "image_lookup_clear",
	"rebind_obtain", "rebind_invalidate", "rebind_flattened_upload", "rebind_shader_upload",
	"rebind_descriptors", "rebind_setup",
	"vertex_sizes", "vertex_ranges", "vertex_obtain", "vertex_slots", "index_prepare",
	"vs_map_lock", "vs_map_lookup", "vs_params", "vs_input_reset", "vs_attrib", "vs_detect_buffers",
	"graphics_pixel_module", "graphics_vertex_modules"
};
static_assert(ProgramNames.size() == static_cast<size_t>(ProgramStage::Count));
struct ProgramTiming {
	uint64_t calls = 0, samples = 0, inclusive_ns = 0, exclusive_ns = 0, dropped = 0;
	uint64_t samples_with_children = 0, max_inclusive_ns = 0, max_children_ns = 0;
};
struct ImageLookups {
	// backing_checks/overlap_checks count calls to SameBacking/ResolveOverlap.
	// direct_matches is before live validation (and includes stencil matches).
	// alias_checks counts candidates visited in the alias-resolution pass.
	uint64_t calls = 0, candidates = 0, max_candidates = 0, backing_checks = 0;
	uint64_t direct_matches = 0, stencil_matches = 0, overlap_checks = 0;
	uint64_t alias_checks = 0, alias_resolves = 0, inserts = 0, format_rejects = 0, recreates = 0;
	// Unique candidates returned by the page query: 0, 1, 2, 3-4, 5-8, 9-16, 17+.
	std::array<uint64_t, 7> candidate_histogram {};
	void Observe(uint64_t count) {
		++calls;
		candidates += count;
		max_candidates = std::max(max_candidates, count);
		const auto bin = count <= 2 ? count : count <= 4 ? 3 : count <= 8 ? 4 : count <= 16 ? 5 : 6;
		++candidate_histogram[bin];
	}
};
struct ModuleSelections {
	uint64_t calls = 0, examined = 0, max_examined = 0, hits = 0, misses = 0;
	uint64_t repeat_previous = 0, history_resets = 0;
	// Candidates examined: 0, 1, 2, 3-4, 5-8, 9-16, 17+.
	std::array<uint64_t, 7> examined_histogram {};
};
// Diagnostic history only. Never consulted to select a shader module.
struct ModuleSelectionHistory {
	uint64_t module_count = UINT64_MAX, previous_index = UINT64_MAX, previous_identity = 0;
	void Observe(ModuleSelections& total, uint64_t count, uint64_t examined,
	             uint64_t index, uint64_t identity) {
		if (module_count != count) {
			total.history_resets += module_count != UINT64_MAX;
			module_count = count;
			previous_index = UINT64_MAX;
		}
		++total.calls;
		total.examined += examined;
		total.max_examined = std::max(total.max_examined, examined);
		const auto bin = examined <= 2 ? examined : examined <= 4 ? 3 :
		                 examined <= 8 ? 4 : examined <= 16 ? 5 : 6;
		++total.examined_histogram[bin];
		if (index != UINT64_MAX) {
			++total.hits;
			total.repeat_previous += previous_index == index && previous_identity == identity;
		} else {
			++total.misses;
		}
		previous_index = index;
		previous_identity = identity;
	}
};
struct ProgramSamplingSequence {
	uint32_t position = 0, slot = 0, random = 0x9e3779b9u;
	bool Next() {
		if (position == 0) {
			// Exactly one sample per 256 calls, avoiding fixed-phase VS/PS aliasing.
			random ^= random << 13;
			random ^= random >> 17;
			random ^= random << 5;
			slot = random & 255u;
		}
		const bool sample = position == slot;
		position = (position + 1u) & 255u;
		return sample;
	}
};
// Exact counts restricted to RebindBuffers; timings are independently sampled.
struct BufferRebinds {
	uint64_t calls = 0, descriptors = 0, null_descriptors = 0;
	uint64_t obtain_calls = 0, hint_hits = 0, searches = 0, stream_copies = 0;
	uint64_t invalidation_calls = 0, invalidation_matches = 0, invalidation_changes = 0;
	// changed_images counts GPU/buffer dirty-flag transitions; HeapChanged can
	// notify resident heaps even when those flags were already set.
	uint64_t matched_images = 0, changed_images = 0, heap_notifications = 0;
	uint64_t flattened_uploads = 0, flattened_bytes = 0, shader_uploads = 0, shader_bytes = 0;
	uint64_t stream_bytes = 0, stream_max_bytes = 0;
	// Payload bytes: <=64, <=256, <=1024, <=4096, <=16384, larger.
	std::array<uint64_t, 6> stream_size_histogram {};
	void ObserveStream(uint64_t bytes) {
		++stream_copies;
		stream_bytes += bytes;
		stream_max_bytes = std::max(stream_max_bytes, bytes);
		const auto bin = bytes <= 64 ? 0 : bytes <= 256 ? 1 : bytes <= 1024 ? 2 :
		                 bytes <= 4096 ? 3 : bytes <= 16384 ? 4 : 5;
		++stream_size_histogram[bin];
	}
};
// Exact counts for non-mesh draws reaching DrawVertexIndex, detail mode only.
// Divide slots/merged_ranges/obtain_calls by draws; timings sample whole loops,
// not individual slots. Index obtains are separate from vertex obtains.
struct VertexIndexPreparation {
	uint64_t draws = 0, slots = 0, nonempty_slots = 0, merged_ranges = 0, obtain_calls = 0;
	uint64_t requested_bytes = 0, acquired_bytes = 0, clipped_ranges = 0, slot_comparisons = 0;
	uint64_t index_calls = 0, index_empty = 0, index_obtain_calls = 0, index_stream_copies = 0, index_bytes = 0;
};
struct State {
	std::array<Totals, Names.size()> totals {};
	Plans plans;
	Frame* active = nullptr;
	uint64_t window_start = 0, thread = 0;
	std::array<ProgramTiming, ProgramNames.size()> program_timings {};
	ModuleSelections module_selections;
	ImageLookups image_lookups;
	BufferRebinds buffer_rebinds;
	VertexIndexPreparation vertex_index;
	// Sampling phase survives reports; resetting it would bias short windows.
	std::array<ProgramSamplingSequence, ProgramNames.size()> program_sequences {};
	uint64_t report_epoch = 0;
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
inline bool DetailedEnabled() {
	if (!Enabled()) return false;
	static const bool detailed = [] {
		const auto* value = std::getenv("KYTY_RENDER_STAGE_DETAIL");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
	}();
	return detailed;
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
inline void AddProgramSample(ProgramTiming& total, uint64_t elapsed, uint64_t children) {
	children = std::min(children, elapsed);
	++total.samples;
	total.inclusive_ns += elapsed;
	total.exclusive_ns += elapsed - children;
	total.samples_with_children += children != 0;
	total.max_inclusive_ns = std::max(total.max_inclusive_ns, elapsed);
	total.max_children_ns = std::max(total.max_children_ns, children);
}
// Restrict the breakdown to the existing image_find stage in PrepareBindings.
// Targets, heap and rebind callers must not inflate that stage's measurements.
inline bool IsImageBindingLookup(const Frame* frame) {
	while (frame != nullptr && frame->stage == Stage::Count) frame = frame->parent;
	return frame != nullptr && frame->stage == Stage::ImageFind;
}
inline void WriteProgramReport(std::FILE* out, const State& state, uint64_t now) {
	std::fprintf(out, "[RENDER_STAGE_PROGRAMS] {\"thread\":%llu,\"end_ns\":%llu,\"interval_ms\":%.3f,"
	    "\"tid\":%u,\"role\":\"%s\",\"sample_period\":256,\"sample_mode\":\"one_per_block_random_phase\",\"timings\":[",
	    static_cast<unsigned long long>(state.thread), static_cast<unsigned long long>(now),
	    (now - state.window_start) / 1e6, Common::SubmitProfile::Diagnostics::ThreadId(),
	    Common::SubmitProfile::Diagnostics::RoleNames[static_cast<size_t>(Common::SubmitProfile::Diagnostics::role)]);
	for (size_t i = 0; i < ProgramNames.size(); ++i) {
		const auto& t = state.program_timings[i];
		const double scale = t.samples != 0 ? static_cast<double>(t.calls) / t.samples : 0.0;
		std::fprintf(out, "%s{\"name\":\"%s\",\"calls\":%llu,\"samples\":%llu,\"dropped\":%llu,"
		    "\"sample_inclusive_ms\":%.6f,\"sample_exclusive_ms\":%.6f,"
		    "\"estimated_inclusive_ms\":%.3f,\"estimated_exclusive_ms\":%.3f,"
		    "\"samples_with_children\":%llu,\"sample_children_ms\":%.6f,"
		    "\"sample_max_inclusive_ms\":%.6f,\"sample_max_children_ms\":%.6f}",
		    i == 0 ? "" : ",", ProgramNames[i], static_cast<unsigned long long>(t.calls),
		    static_cast<unsigned long long>(t.samples), static_cast<unsigned long long>(t.dropped),
		    t.inclusive_ns / 1e6, t.exclusive_ns / 1e6,
		    t.inclusive_ns / 1e6 * scale, t.exclusive_ns / 1e6 * scale,
		    static_cast<unsigned long long>(t.samples_with_children),
		    (t.inclusive_ns - t.exclusive_ns) / 1e6, t.max_inclusive_ns / 1e6, t.max_children_ns / 1e6);
	}
	const auto& m = state.module_selections;
	std::fprintf(out, "],\"module_selection\":{\"calls\":%llu,\"examined\":%llu,\"max_examined\":%llu,"
	    "\"hits\":%llu,\"misses\":%llu,\"repeat_previous\":%llu,\"history_resets\":%llu,\"examined_histogram\":[",
	    static_cast<unsigned long long>(m.calls), static_cast<unsigned long long>(m.examined),
	    static_cast<unsigned long long>(m.max_examined), static_cast<unsigned long long>(m.hits),
	    static_cast<unsigned long long>(m.misses), static_cast<unsigned long long>(m.repeat_previous),
	    static_cast<unsigned long long>(m.history_resets));
	for (size_t i = 0; i < m.examined_histogram.size(); ++i)
		std::fprintf(out, "%s%llu", i == 0 ? "" : ",", static_cast<unsigned long long>(m.examined_histogram[i]));
	const auto& images = state.image_lookups;
	std::fprintf(out, "]},\"image_lookup\":{\"scope\":\"prepare_bindings\",\"calls\":%llu,"
	    "\"candidates\":%llu,\"max_candidates\":%llu,\"backing_checks\":%llu,"
	    "\"direct_matches\":%llu,\"stencil_matches\":%llu,\"overlap_checks\":%llu,"
	    "\"alias_checks\":%llu,\"alias_resolves\":%llu,\"inserts\":%llu,\"format_rejects\":%llu,"
	    "\"recreates\":%llu,\"candidate_histogram\":[",
	    static_cast<unsigned long long>(images.calls), static_cast<unsigned long long>(images.candidates),
	    static_cast<unsigned long long>(images.max_candidates), static_cast<unsigned long long>(images.backing_checks),
	    static_cast<unsigned long long>(images.direct_matches), static_cast<unsigned long long>(images.stencil_matches),
	    static_cast<unsigned long long>(images.overlap_checks), static_cast<unsigned long long>(images.alias_checks),
	    static_cast<unsigned long long>(images.alias_resolves), static_cast<unsigned long long>(images.inserts),
	    static_cast<unsigned long long>(images.format_rejects), static_cast<unsigned long long>(images.recreates));
	for (size_t i = 0; i < images.candidate_histogram.size(); ++i)
		std::fprintf(out, "%s%llu", i == 0 ? "" : ",", static_cast<unsigned long long>(images.candidate_histogram[i]));
	const auto& b = state.buffer_rebinds;
	std::fprintf(out, "]},\"buffer_rebinds\":{\"calls\":%llu,\"descriptors\":%llu,\"null_descriptors\":%llu,"
	    "\"obtain_calls\":%llu,\"hint_hits\":%llu,\"searches\":%llu,\"stream_copies\":%llu,"
	    "\"invalidation_calls\":%llu,\"invalidation_matches\":%llu,\"invalidation_changes\":%llu,"
	    "\"matched_images\":%llu,\"changed_images\":%llu,\"heap_notifications\":%llu,"
	    "\"flattened_uploads\":%llu,\"flattened_bytes\":%llu,\"shader_uploads\":%llu,\"shader_bytes\":%llu,"
	    "\"stream_bytes\":%llu,\"stream_max_bytes\":%llu,\"stream_size_histogram\":[",
	    static_cast<unsigned long long>(b.calls), static_cast<unsigned long long>(b.descriptors),
	    static_cast<unsigned long long>(b.null_descriptors), static_cast<unsigned long long>(b.obtain_calls),
	    static_cast<unsigned long long>(b.hint_hits), static_cast<unsigned long long>(b.searches),
	    static_cast<unsigned long long>(b.stream_copies), static_cast<unsigned long long>(b.invalidation_calls),
	    static_cast<unsigned long long>(b.invalidation_matches), static_cast<unsigned long long>(b.invalidation_changes),
	    static_cast<unsigned long long>(b.matched_images), static_cast<unsigned long long>(b.changed_images),
	    static_cast<unsigned long long>(b.heap_notifications),
	    static_cast<unsigned long long>(b.flattened_uploads), static_cast<unsigned long long>(b.flattened_bytes),
	    static_cast<unsigned long long>(b.shader_uploads), static_cast<unsigned long long>(b.shader_bytes),
	    static_cast<unsigned long long>(b.stream_bytes), static_cast<unsigned long long>(b.stream_max_bytes));
	for (size_t i = 0; i < b.stream_size_histogram.size(); ++i)
		std::fprintf(out, "%s%llu", i == 0 ? "" : ",", static_cast<unsigned long long>(b.stream_size_histogram[i]));
	const auto& v = state.vertex_index;
	std::fprintf(out, "]},\"vertex_index\":{\"draws\":%llu,\"slots\":%llu,\"nonempty_slots\":%llu,"
	    "\"merged_ranges\":%llu,\"obtain_calls\":%llu,\"requested_bytes\":%llu,\"acquired_bytes\":%llu,"
	    "\"clipped_ranges\":%llu,\"slot_comparisons\":%llu,\"index_calls\":%llu,\"index_empty\":%llu,"
	    "\"index_obtain_calls\":%llu,\"index_stream_copies\":%llu,\"index_bytes\":%llu}}\n",
	    static_cast<unsigned long long>(v.draws), static_cast<unsigned long long>(v.slots),
	    static_cast<unsigned long long>(v.nonempty_slots), static_cast<unsigned long long>(v.merged_ranges),
	    static_cast<unsigned long long>(v.obtain_calls), static_cast<unsigned long long>(v.requested_bytes),
	    static_cast<unsigned long long>(v.acquired_bytes), static_cast<unsigned long long>(v.clipped_ranges),
	    static_cast<unsigned long long>(v.slot_comparisons), static_cast<unsigned long long>(v.index_calls),
	    static_cast<unsigned long long>(v.index_empty), static_cast<unsigned long long>(v.index_obtain_calls),
	    static_cast<unsigned long long>(v.index_stream_copies), static_cast<unsigned long long>(v.index_bytes));
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
		if (DetailedEnabled()) WriteProgramReport(stderr, state, now);
		std::fflush(stderr);
	}
	state.totals = {};
	state.plans = {};
	state.program_timings = {};
	state.module_selections = {};
	state.image_lookups = {};
	state.buffer_rebinds = {};
	state.vertex_index = {};
	++state.report_epoch;
	state.window_start = now;
}
} // namespace Detail

class Scope {
public:
	explicit Scope(Stage stage, bool enabled = true): m_frame {stage, 0} {
		if (!enabled || !Detail::Enabled()) return;
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

// Select once outside a high-frequency loop. The false specialization has no
// frame, TLS access, environment check, clock read or destructor work.
class DisabledScope {
public:
	constexpr explicit DisabledScope(Stage) noexcept {}
};
template <bool Enabled>
using ScopeFor = std::conditional_t<Enabled, Scope, DisabledScope>;

// Independent samples: a temporary frame collects real child durations. Only
// those children (not the sampled interval) are forwarded to the full-rate parent,
// so existing exclusive totals remain unchanged. Also works without a parent.
// Sampled sub-stages are independent views, not additional full-rate children:
// do not add them to (or subtract them from) their enclosing sampled totals.
template <bool Profile>
class ProgramScope;
template <>
class ProgramScope<false> {
public:
	constexpr explicit ProgramScope(ProgramStage) noexcept {}
};
template <>
class ProgramScope<true> {
public:
	explicit ProgramScope(ProgramStage stage): m_index(static_cast<size_t>(stage)) {
		if (!Detail::DetailedEnabled()) return;
		auto& state = Detail::state;
		++state.program_timings[m_index].calls;
		if (!state.program_sequences[m_index].Next()) return;
		m_state = &state;
		m_epoch = state.report_epoch;
		m_frame.start = Detail::Now();
		if (state.window_start == 0) {
			state.window_start = m_frame.start;
			state.thread = Detail::next_thread.fetch_add(1, std::memory_order_relaxed);
		}
		Detail::Begin(state, m_frame);
	}
	~ProgramScope() {
		if (m_state == nullptr) return;
		auto& total = m_state->program_timings[m_index];
		const auto now = Detail::Now();
		if (m_epoch != m_state->report_epoch) ++total.dropped;
		else Detail::AddProgramSample(total, now - m_frame.start, m_frame.children);
		m_state->active = m_frame.parent;
		if (m_frame.parent != nullptr) m_frame.parent->children += m_frame.children;
		else Detail::Report(*m_state, now);
	}
	ProgramScope(const ProgramScope&) = delete;
	ProgramScope& operator=(const ProgramScope&) = delete;
private:
	size_t m_index;
	State* m_state = nullptr;
	Frame m_frame {Stage::Count, 0}; // Never passed to Detail::End or the stage totals.
	uint64_t m_epoch = 0;
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
