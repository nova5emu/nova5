#include "graphics/host_gpu/renderer/drawPrep/drawPrep.h"

#include "common/assert.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/drawPrep/window.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/host_gpu/renderer/pipeline/shaderReadObserver.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/shaderCompiler.h"
#include "kernel/memory.h"
#include "live-counters.h"
#include "live-census.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <utility>

// Adapted from Jetsku 3b30c863 / 41c566f5 with 59e74d86's metadata and
// early-return corrections. Nova5 keeps its existing cache lock and eviction
// generation; certificates always compare values. No coherence-log shortcut.
namespace Libs::Graphics::DrawPrep {
namespace {
thread_local PreparedDraw* current = nullptr;
thread_local PreparedDraw* bindings = nullptr;

uint32_t EnvCount(const char* name, uint32_t fallback, uint32_t high) {
	const auto* value = std::getenv(name);
	if (!value) return fallback;
	return static_cast<uint32_t>(std::clamp(std::strtoul(value, nullptr, 10), 1ul, static_cast<unsigned long>(high)));
}
uint64_t Microseconds(std::chrono::steady_clock::time_point since) {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
	    std::chrono::steady_clock::now() - since).count());
}
void CountReadFailure(const ReadSet& reads) {
	LiveCounters::Add(LiveCounters::DrawPrepReadFailed);
	switch (reads.Failure()) {
		case ReadFailure::Backing: LiveCounters::Add(LiveCounters::DrawPrepReadBacking); break;
		case ReadFailure::Uncertified: LiveCounters::Add(LiveCounters::DrawPrepReadUncertified); break;
		case ReadFailure::Overflow: LiveCounters::Add(LiveCounters::DrawPrepReadOverflow); break;
		case ReadFailure::Inconsistent: LiveCounters::Add(LiveCounters::DrawPrepReadInconsistent); break;
		default: break;
	}
}
bool Eligible(const RegisterSnapshot& r, uint32_t count, uint32_t instances) {
	if (!count || !instances || !r.shaders.GetVs().es_regs.data_addr) return false;
	const auto& ctx = r.context;
	const auto& control = ctx.GetRenderControl();
	const auto& over = ctx.GetDepthRenderOverride();
	if (ctx.GetColorControl().mode > 1 || control.copy_depth_to_color || control.copy_stencil_to_color ||
	    control.depth_clear_enable || control.stencil_clear_enable ||
	    (ctx.GetColorControl().mode == 0 && ((over.force_z_dirty && over.force_z_valid) ||
	      (over.force_stencil_dirty && over.force_stencil_valid)))) return false;
	switch (r.user_config.GetPrimType()) {
		case Prospero::PrimitiveType::kPointList: case Prospero::PrimitiveType::kLineList:
		case Prospero::PrimitiveType::kLineStrip: case Prospero::PrimitiveType::kTriList:
		case Prospero::PrimitiveType::kTriStrip: case Prospero::PrimitiveType::kTriFan: return true;
		default: return false;
	}
}
bool SameSnapshot(const ShaderRecompiler::IR::ResourceSnapshot& a,
                  const ShaderRecompiler::IR::ResourceSnapshot& b) {
	return a.buffers == b.buffers && a.images == b.images && a.samplers == b.samplers &&
	       a.flattened_srt == b.flattened_srt && a.user_data == b.user_data && a.uniform_fill == b.uniform_fill;
}
template <typename Info> bool SameStage(const Info& a, const Info& b) {
	std::vector<uint32_t> ka, kb;
	BuildStageStaticKey(a, ka);
	BuildStageStaticKey(b, kb);
	return ka == kb && a.stage.program == b.stage.program && SameSnapshot(a.stage.resources, b.stage.resources);
}
bool SameVertex(const ShaderVertexInputInfo& a, const ShaderVertexInputInfo& b) {
	if (!SameStage(a, b) || a.resources_num != b.resources_num || a.buffers_num != b.buffers_num ||
	    a.fetch_external != b.fetch_external) return false;
	for (int i = 0; i < a.resources_num; ++i)
		if (std::memcmp(a.resources[i].fields, b.resources[i].fields, sizeof(a.resources[i].fields)) != 0) return false;
	for (int i = 0; i < a.buffers_num; ++i) {
		const auto& x = a.buffers[i];
		const auto& y = b.buffers[i];
		if (x.addr != y.addr || x.stride != y.stride || x.num_records != y.num_records ||
		    x.fetch_index != y.fetch_index || x.attr_num != y.attr_num) return false;
		for (int j = 0; j < x.attr_num; ++j)
			if (x.attr_indices[j] != y.attr_indices[j] || x.attr_offsets[j] != y.attr_offsets[j]) return false;
	}
	return true;
}
} // namespace

Mode GetMode() {
	static const Mode mode = [] {
		const auto* value = std::getenv("KYTY_DRAW_PREP");
		if (!value || !*value || std::strcmp(value, "0") == 0 || std::strcmp(value, "off") == 0) return Mode::Off;
		if (std::strcmp(value, "probe") == 0) return Mode::Probe;
		if (std::strcmp(value, "inline") == 0) return Mode::Inline;
		if (std::strcmp(value, "window") == 0) return Mode::Window;
		if (std::strcmp(value, "parallel") == 0) return Mode::Parallel;
		EXIT("KYTY_DRAW_PREP must be off, probe, inline, window or parallel\n");
	}();
	return mode;
}
int VerifyMode() {
	static const int mode = [] {
		const auto* value = std::getenv("KYTY_DRAW_PREP_VERIFY");
		return !value || !*value || std::strcmp(value, "0") == 0 ? 0 : std::strcmp(value, "exit") == 0 ? 2 : 1;
	}();
	return mode;
}

void Prepare(PipelineCache& cache, const RegisterSnapshot& r, bool eligible, PreparedDraw& draw) {
	const auto started = std::chrono::steady_clock::now();
	struct Time {
		std::chrono::steady_clock::time_point started;
		~Time() { LiveCounters::Add(LiveCounters::DrawPrepPrepareUs, Microseconds(started)); }
	} time {started};
	draw.ok = false;
	draw.consumed = false;
	draw.programs = {};
	draw.mapping = {};
	draw.reads.Reset();
	draw.vertex_consumer = draw.pixel_consumer = nullptr;
	draw.vertex_bindings.Reset();
	draw.pixel_bindings.Reset();
	draw.binding_mode = kyty_local_draw_prep_bindings_mode.load(std::memory_order_relaxed);
	draw.shader_map_generation = ShaderMapGeneration();
	if (!eligible || !r.shaders.GetVs().es_regs.data_addr) {
		LiveCounters::Add(LiveCounters::DrawPrepIneligible); return;
	}
	const auto& ctx = r.context;
	const auto& sh = ctx.GetShaderRegisters();
	const auto& db = sh.db_shader_control;
	if (r.shaders.GetVs().gs_regs.rsrc2.user_sgpr > HW::UserSgprInfo::SGPRS_MAX ||
	    r.shaders.GetPs().ps_regs.rsrc2.user_sgpr > HW::UserSgprInfo::SGPRS_MAX ||
	    (sh.ps_in_control & 0x3fu) > 32 || (sh.ps_input_ena & ~0xffbfu) != 0 ||
	    sh.ps_input_ena != sh.ps_input_addr) {
		LiveCounters::Add(LiveCounters::DrawPrepIneligible); return;
	}
	draw.pixel_active = r.shaders.GetPs().ps_regs.data_addr != 0 &&
	    (((ctx.GetRenderTargetMask() & sh.m_cbShaderMask) != 0) || db.shader_kill_enable ||
	     db.shader_z_export_enable || db.shader_mask_export_enable || db.shader_dual_export_enable || db.shader_execute_on_noop);
	const auto& depth = ctx.GetDepthRenderTarget();
	bool has_target = depth.z_read_base_addr || depth.z_write_base_addr ||
	                  depth.stencil_read_base_addr || depth.stencil_write_base_addr;
	for (uint32_t slot = 0; slot < 8; ++slot)
		has_target |= ctx.GetRenderTarget(slot).base.addr != 0 &&
		              (slot == 0 || render_target_mask_slot(ctx.GetRenderTargetMask(), slot) != 0);
	if (!has_target && !draw.pixel_active) { LiveCounters::Add(LiveCounters::DrawPrepIneligible); return; }
	for (uint32_t slot = 0; slot < 8; ++slot) {
		const auto& target = ctx.GetRenderTarget(slot);
		if (target.base.addr && (slot == 0 || render_target_mask_slot(ctx.GetRenderTargetMask(), slot))) {
			const auto encoding = Prospero::ResolveRenderTargetFormat(target.info.format, target.info.channel_type);
			if (!encoding.IsValid() || !encoding.SupportsOrder(target.info.channel_order)) {
				LiveCounters::Add(LiveCounters::DrawPrepIneligible); return;
			}
			draw.mapping[slot] = TextureGetRenderTargetFormat(target.info.format, target.info.channel_type,
			                                                      target.info.channel_order).export_mapping;
		}
	}
	Recorder recorder {&draw.reads, LibKernel::Memory::TryProbeBackingToHost};
	RecordScope scope(recorder);
	if (!cache.PrepareGraphicsProgramsSpeculative(r.shaders.GetVs(), r.shaders.GetPs(), sh, ctx,
	    r.user_config, draw.mapping, draw.pixel_active, draw.vertex, draw.pixel, draw.programs, draw.publication,
	    draw.binding_mode ? &draw.vertex_bindings : nullptr, draw.binding_mode ? &draw.pixel_bindings : nullptr)) {
		if (draw.reads.Failed()) CountReadFailure(draw.reads);
		else LiveCounters::Add(LiveCounters::DrawPrepNotPublished);
		return;
	}
	if (!draw.reads.Finish()) { CountReadFailure(draw.reads); return; }
	draw.ok = true;
}

bool Validate(PipelineCache& cache, PreparedDraw& draw, bool pixel_active,
              std::span<const Prospero::ColorComponentMapping, 8> mapping) {
	LiveCensus::Scope census(LiveCensus::DrawPrepValidate, 0);
	if (!draw.ok) return false;
	bool same = draw.pixel_active == pixel_active;
	for (size_t i = 0; i < mapping.size(); ++i) same &= draw.mapping[i].packed == mapping[i].packed;
	// Check publication before dereferencing any pointer into a permutation.
	if (!same || draw.shader_map_generation != ShaderMapGeneration() || !cache.SpeculativePublicationCurrent(draw.publication)) {
		LiveCounters::Add(LiveCounters::DrawPrepStale); return false;
	}
	thread_local std::vector<uint8_t> scratch;
	const auto result = draw.reads.Validate(LibKernel::Memory::TryReadGpuCleanBackingToHost, scratch);
	if (result != ValidateResult::Ok) {
		LiveCounters::Add(result == ValidateResult::Unclean ? LiveCounters::DrawPrepCertUnclean : LiveCounters::DrawPrepCertChanged);
		return false;
	}
	LiveCounters::Add(LiveCounters::DrawPrepAccepted);
	LiveCounters::Add(LiveCounters::DrawPrepCertBytes, draw.reads.ByteCount());
	// Existing read observers (native records/capture) must see all hoisted reads.
	for (const auto& range : draw.reads.Ranges()) ShaderReadObserver::ObserveRead(range.begin, range.end - range.begin);
	return true;
}

bool Verify(const PreparedDraw& prepared, const PipelineCache::GraphicsPrograms& programs,
            const ShaderVertexInputInfo& vertex, const ShaderPixelInputInfo& pixel) {
	LiveCounters::Add(LiveCounters::DrawPrepVerifyChecks);
	const bool same = prepared.programs.vertex.id == programs.vertex.id &&
	    prepared.programs.vertex.module == programs.vertex.module && SameVertex(prepared.vertex, vertex) &&
	    (!prepared.pixel_active || (prepared.programs.pixel.id == programs.pixel.id &&
	       prepared.programs.pixel.module == programs.pixel.module && SameStage(prepared.pixel, pixel)));
	if (!same) {
		LiveCounters::Add(LiveCounters::DrawPrepMismatches);
		std::printf("DrawPrepVerify: program/static-input/resource-snapshot mismatch\n");
		if (VerifyMode() == 2) EXIT("DrawPrepVerify mismatch\n");
	}
	return same;
}

PreparedDraw* TakePrepared() {
	auto* result = std::exchange(current, nullptr);
	if (result) result->consumed = true;
	return result;
}

void OfferBindingPlans(PreparedDraw& draw, const ShaderStageRuntime& vertex, const ShaderStageRuntime* pixel) {
	draw.vertex_consumer = &vertex;
	draw.pixel_consumer = pixel;
	bindings = &draw;
}
bool TakeBindingPlan(const ShaderStageRuntime& runtime, uint32_t mode, StageBindingPlan& destination) {
	if (!bindings || bindings->binding_mode != mode) return false;
	StageBindingPlan* source = nullptr;
	if (bindings->vertex_consumer == &runtime) {
		source = &bindings->vertex_bindings; bindings->vertex_consumer = nullptr;
	} else if (bindings->pixel_consumer == &runtime) {
		source = &bindings->pixel_bindings; bindings->pixel_consumer = nullptr;
	}
	if (!source || !source->valid) return false;
	std::swap(destination, *source);
	LiveCounters::Add(LiveCounters::DrawPrepBindingPlans);
	return true;
}

struct Engine::Impl {
	struct Slot {
		uint64_t submit_id = 0;
		bool indexed = false, eligible = false;
		DrawIndexArgs index;
		DrawAutoArgs automatic;
		RegisterSnapshot registers;
		PreparedDraw prepared;
	};
	RenderContext& renderer;
	Mode mode;
	Window<Slot> window;
	std::mutex mutex;
	std::condition_variable available, done;
	bool stop = false;
	bool committing = false;
	std::vector<std::thread> workers;

	explicit Impl(RenderContext& r): renderer(r), mode(GetMode()), window(EnvCount("KYTY_DRAW_PREP_WINDOW", 32, 256)) {
		if (mode == Mode::Parallel) {
			const auto count = EnvCount("KYTY_DRAW_PREP_WORKERS", 2, 16);
			for (uint32_t i = 0; i < count; ++i) workers.emplace_back([this, i] {
				char name[32]; std::snprintf(name, sizeof(name), "DrawPrep#%u", i + 1);
				Profiler::SetThreadName(name);
				for (;;) {
					uint64_t seq = 0;
					if (auto* slot = window.TryClaim(seq)) {
						Prepare(renderer.GetPipelineCache(), slot->registers, slot->eligible, slot->prepared);
						LiveCounters::Add(LiveCounters::DrawPrepWorker);
						{ std::lock_guard lock(mutex); window.Complete(seq); }
						done.notify_one();
						continue;
					}
					std::unique_lock lock(mutex);
					available.wait(lock, [this] { return stop || window.HasClaimable(); });
					if (stop) return;
				}
			});
		}
		std::printf("DrawPrep: mode=%d window=%u workers=%zu cert=value verify=%d (cache lock retained)\n",
		            static_cast<int>(mode), window.Capacity(), workers.size(), VerifyMode());
	}
	~Impl() {
		{ std::lock_guard lock(mutex); stop = true; }
		available.notify_all();
		for (auto& worker : workers) worker.join();
	}
	void CommitHead() {
		EXIT_IF(committing || window.Empty());
		auto& slot = window.HeadPayload();
		if (window.TryClaimHead()) {
			Prepare(renderer.GetPipelineCache(), slot.registers, slot.eligible, slot.prepared);
			LiveCounters::Add(LiveCounters::DrawPrepSelf);
		} else if (!window.HeadDone()) {
			LiveCensus::Scope census(LiveCensus::DrawPrepWait, 0);
			const auto started = std::chrono::steady_clock::now();
			std::unique_lock lock(mutex);
			done.wait(lock, [this] { return window.HeadDone(); });
			LiveCounters::Add(LiveCounters::DrawPrepWaitUs, Microseconds(started));
		}
		// Workers hold no VM lock while waiting for CP services: their only
		// guest reader is nonblocking. No nested service/reordering is needed.
		committing = true;
		auto& scheduler = renderer.GetCommandScheduler();
		const auto previous = scheduler.BindRegisters(slot.registers.context, slot.registers.user_config, slot.registers.shaders);
		auto* previous_prepared = current;
		auto* previous_bindings = bindings;
		bindings = nullptr;
		current = &slot.prepared;
		if (slot.indexed) renderer.GetRenderExecutor().DrawIndex(slot.submit_id, scheduler.Current(), slot.index);
		else renderer.GetRenderExecutor().DrawAuto(slot.submit_id, scheduler.Current(), slot.automatic);
		if (!slot.prepared.consumed) LiveCounters::Add(LiveCounters::DrawPrepUnused);
		current = previous_prepared;
		bindings = previous_bindings;
#ifdef KYTY_LOCAL_VULKAN_RECORDING
		// The packet observer has already advanced beyond this snapshot. A later
		// native indirect must rebuild its state key from its own registers.
		renderer.GetRenderExecutor().NativeXprForgetState();
#endif
		scheduler.RestoreRegisters(previous);
		window.Retire();
		scheduler.CompleteDraws(1);
		committing = false;
	}
};

Engine::Engine(RenderContext& renderer): m_impl(std::make_unique<Impl>(renderer)) {}
Engine::~Engine() = default;
bool Engine::Pending() const { return !m_impl->window.Empty(); }
bool Engine::Submit(uint64_t submit_id, const DrawIndexArgs* index, const DrawAutoArgs* automatic,
                    const HW::Context& context, const HW::UserConfig& user_config, const HW::Shader& shaders) {
	auto& s = *m_impl;
	if (s.mode == Mode::Off) return false;
	EXIT_IF((index == nullptr) == (automatic == nullptr) || s.committing);
	if (s.window.Full()) s.CommitHead();
	auto& slot = s.window.Reserve();
	slot.submit_id = submit_id;
	slot.indexed = index != nullptr;
	if (index) slot.index = *index; else slot.automatic = *automatic;
	slot.registers.context = context;
	slot.registers.user_config = user_config;
	slot.registers.shaders = shaders;
	slot.eligible = Eligible(slot.registers, index ? index->index_count : automatic->vertex_count,
	                         index ? index->instance_count : automatic->instance_count);
	LiveCounters::Add(LiveCounters::DrawPrepSubmitted);
	{ std::lock_guard lock(s.mutex); s.window.Publish(); }
	s.available.notify_one();
	if (s.mode == Mode::Probe || s.mode == Mode::Inline) s.CommitHead();
	return true;
}
void Engine::Drain() {
	auto& s = *m_impl;
	if (s.committing || s.window.Empty()) return;
	LiveCounters::Add(LiveCounters::DrawPrepDrains);
	while (!s.window.Empty()) s.CommitHead();
}

} // namespace Libs::Graphics::DrawPrep
