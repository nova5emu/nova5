#pragma once

#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/drawPrep/readSet.h"
#include "graphics/host_gpu/renderer/drawPrep/packetClass.h"
#include "graphics/host_gpu/renderer/drawPrep/bindingPlan.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/render.h"

#include <memory>

namespace Libs::Graphics::DrawPrep {

// Separate experiments, all off unless explicitly selected. Probe always renders
// with the serial result; Inline exercises consumption; Window adds ordered
// deferral without threads; Parallel runs the same preparation on workers.
enum class Mode { Off, Probe, Inline, Window, Parallel };
Mode GetMode();
int VerifyMode(); // 0 off, 1 compare + serial result, 2 stop on mismatch

struct RegisterSnapshot {
	HW::Context context;
	HW::UserConfig user_config;
	HW::Shader shaders;
};

struct PreparedDraw {
	bool ok = false;
	bool pixel_active = false;
	bool consumed = false;
	std::array<Prospero::ColorComponentMapping, 8> mapping {};
	PipelineCache::GraphicsPrograms programs;
	ShaderVertexInputInfo vertex;
	ShaderPixelInputInfo pixel;
	ReadSet reads;
	uint64_t shader_map_generation = 0;
	uint64_t publication = 0;
	uint32_t binding_mode = 0;
	StageBindingPlan vertex_bindings, pixel_bindings;
	const ShaderStageRuntime* vertex_consumer = nullptr;
	const ShaderStageRuntime* pixel_consumer = nullptr;
};

// All memory goes through a bounded probe, including AGC metadata. Only the CP
// may use the output, after checking mapping/activity, map generation and bytes.
void Prepare(PipelineCache& cache, const RegisterSnapshot& registers, bool eligible, PreparedDraw& draw);
bool Validate(PipelineCache& cache, PreparedDraw& draw, bool pixel_active,
              std::span<const Prospero::ColorComponentMapping, 8> mapping);
bool Verify(const PreparedDraw& prepared, const PipelineCache::GraphicsPrograms& programs,
            const ShaderVertexInputInfo& vertex, const ShaderPixelInputInfo& pixel);

// One-shot offer to RefreshShaders within a committed draw; nested draws cannot
// consume it again. Scoped installation is private to Engine.
PreparedDraw* TakePrepared();
void OfferBindingPlans(PreparedDraw& draw, const ShaderStageRuntime& vertex, const ShaderStageRuntime* pixel);
bool TakeBindingPlan(const ShaderStageRuntime& runtime, uint32_t mode, StageBindingPlan& destination);

// Conservative whitelist: only immediate register/index state and direct draws
// may stay in the window. Every other packet drains before its handler.

class Engine {
public:
	explicit Engine(RenderContext& renderer);
	~Engine();
	Engine(const Engine&) = delete;
	Engine& operator=(const Engine&) = delete;
	bool Submit(uint64_t submit_id, const DrawIndexArgs* index, const DrawAutoArgs* automatic,
	            const HW::Context& context, const HW::UserConfig& user_config, const HW::Shader& shaders);
	void Drain();
	bool Pending() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace Libs::Graphics::DrawPrep
