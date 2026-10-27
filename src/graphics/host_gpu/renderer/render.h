#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/submitProfile.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/bindingEmission.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/host_gpu/renderer/nativeCommands.h"

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Libs::Graphics {

namespace HW {
class Context;
class UserConfig;
class Shader;
} // namespace HW

struct GraphicContext;
struct DrawCallInfo;
struct DrawEmitInfo;
struct DrawIndexBufferSource;
class RenderContext;
class CommandScheduler;
struct DynamicState;
namespace VkRecording { class Backend; }
class Buffer;
struct RenderExecutorTestAccess;
struct IndirectCountClamp;

enum class CommandBufferDebugOp : uint32_t {
	DispatchDirect,
	DrawIndex,
	DrawIndexAuto,
	DispatchIndirect = 9,
	Unknown,
};

enum class DrawOffsetSource : uint8_t {
	DrawState,
	IndirectArgs,
};

// Nonzero address: the GPU reads count Vulkan indirect draw records, stride bytes apart.
struct DrawIndirectArgs {
	uint64_t address = 0;
	uint32_t count   = 0;
	uint32_t stride  = 0;
	// Immutable GPU snapshot at offset zero; count is its maximum, not a CPU read.
	vk::Buffer count_buffer = nullptr;
};

struct IndirectCountSnapshot {
	std::shared_ptr<Buffer> buffer;
	uint64_t tick = 0;
};
IndirectCountSnapshot SnapshotIndirectCount(RenderContext& context, uint64_t address, uint32_t max_count);
// Diagnostic snapshot: GPU-ordered, never reads guest arguments on the CPU.
// Returns null for oversized diagnostics; legacy callers must then use CPU fallback.
std::shared_ptr<Buffer> SnapshotIndirectIndexedArgs(RenderContext& context,
    const DrawIndirectArgs& args, uint32_t index_limit, bool legacy_clamp);

// 3 task-count dwords, 7 mesh parameters, status, then the 5 original draw dwords.
// Exposed for the differential GPU tests; the buffer lives through its submission.
std::shared_ptr<Buffer> ConvertIndirectMeshArgs(RenderContext& context, uint64_t address,
    const ShaderMeshInputInfo& mesh, uint32_t max_indices, uint64_t index_address,
    uint32_t index_bytes, uint64_t index_span, bool apply_offsets);

struct DrawIndexArgs {
	uint32_t         index_count                = 0;
	const void*      index_addr                 = nullptr;
	uint32_t         instance_count             = 0;
	uint32_t         index_type_and_size        = 0;
	int32_t          base_vertex                = 0;
	uint32_t         first_instance             = 0;
	DrawOffsetSource offset_source              = DrawOffsetSource::DrawState;
	uint32_t         render_target_slice_offset = 0;
	// With indirect records, index_count is INDEX_BUFFER_SIZE.
	DrawIndirectArgs indirect;
	// Number of valid elements at index_addr, independent of index_count.
	// AMD SI Programming Guide 2.5.4/2.5.10: indirect firstIndex consumes
	// INDEX_BUFFER_SIZE; CIK VGT_DMA_MAX_SIZE: subsequent indices read as zero.
	uint32_t valid_index_count = UINT32_MAX;
};

struct DrawAutoArgs {
	uint32_t         vertex_count               = 0;
	uint32_t         instance_count             = 0;
	uint32_t         first_vertex               = 0;
	uint32_t         first_instance             = 0;
	DrawOffsetSource offset_source              = DrawOffsetSource::DrawState;
	uint32_t         render_target_slice_offset = 0;
	DrawIndirectArgs indirect;
};

struct SubmitInfo {
	static constexpr uint32_t MaxSemaphores = 3;

	std::array<vk::Semaphore, MaxSemaphores>          wait_semaphores {};
	std::array<uint64_t, MaxSemaphores>               wait_ticks {};
	std::array<vk::PipelineStageFlags, MaxSemaphores> wait_stages {};
	std::array<vk::Semaphore, MaxSemaphores>          signal_semaphores {};
	std::array<uint64_t, MaxSemaphores>               signal_ticks {};
	uint32_t                                          num_wait_semaphores   = 0;
	uint32_t                                          num_signal_semaphores = 0;

	void AddWait(vk::Semaphore semaphore, uint64_t tick = 1,
	             vk::PipelineStageFlags stage = vk::PipelineStageFlagBits::eAllCommands) {
		EXIT_IF(semaphore == nullptr || num_wait_semaphores >= MaxSemaphores);
		wait_semaphores[num_wait_semaphores] = semaphore;
		wait_ticks[num_wait_semaphores]      = tick;
		wait_stages[num_wait_semaphores++]   = stage;
	}

	void AddSignal(vk::Semaphore semaphore, uint64_t tick = 1) {
		EXIT_IF(semaphore == nullptr || num_signal_semaphores >= MaxSemaphores);
		signal_semaphores[num_signal_semaphores] = semaphore;
		signal_ticks[num_signal_semaphores++]    = tick;
	}
};

class CommandBuffer {
public:
	~CommandBuffer() = default;

	KYTY_CLASS_NO_COPY(CommandBuffer);

	[[nodiscard]] bool IsInvalid() const;

	void SetDebugInfo(uint32_t op, uint64_t submit_id, uint32_t arg0 = 0, uint32_t arg1 = 0,
	                  uint32_t arg2 = 0, uint32_t arg3 = 0, uint64_t arg4 = 0);
	void BeginRendering(const RenderState& state) const;
	void EndRendering() const;
	void EmitBindings(const VkRecording::BindingEmission& emission) const;
	// Typed emission boundary for clock/profile queries. Callers retain this
	// facade, never a native handle across a preparation that can rotate it.
	void ResetQueryPool(vk::QueryPool pool, uint32_t first, uint32_t count) const;
	void WriteTimestamp(vk::PipelineStageFlagBits stage, vk::QueryPool pool, uint32_t query) const;
	void BindPipeline(vk::PipelineBindPoint point, vk::Pipeline pipeline) const;
	void PushConstants(vk::PipelineLayout layout, vk::ShaderStageFlags stages, uint32_t offset,
	                   uint32_t bytes, const void* data) const;
	void BindVertexBuffers(uint32_t first, uint32_t count, const vk::Buffer* buffers,
	                       const uint64_t* offsets, const uint64_t* sizes) const;
	void BindIndexBuffer(vk::Buffer buffer, uint64_t offset, uint64_t size, vk::IndexType type, bool bounded) const;
	void CommitDynamicState(DynamicState& state) const;
	void Barrier(const vk::DependencyInfo& info) const;
	void EmitMemoryBarrier(vk::PipelineStageFlags source, vk::PipelineStageFlags destination,
	                   const vk::MemoryBarrier& barrier) const;
	void Draw(uint32_t count, uint32_t instances, uint32_t first, uint32_t first_instance) const;
	void DrawIndexed(uint32_t count, uint32_t instances, uint32_t first, int32_t vertex, uint32_t first_instance) const;
	void DrawIndirect(vk::Buffer buffer, uint64_t offset, uint32_t count, uint32_t stride) const;
	void DrawIndexedIndirect(vk::Buffer buffer, uint64_t offset, uint32_t count, uint32_t stride) const;
	void DrawIndirectCount(vk::Buffer buffer, uint64_t offset, vk::Buffer counts, uint64_t count_offset, uint32_t count, uint32_t stride) const;
	void DrawIndexedIndirectCount(vk::Buffer buffer, uint64_t offset, vk::Buffer counts, uint64_t count_offset, uint32_t count, uint32_t stride) const;
	void DrawMeshTasks(uint32_t x, uint32_t y, uint32_t z) const;
	void DrawMeshTasksIndirect(vk::Buffer buffer, uint64_t offset, uint32_t count, uint32_t stride) const;
	void Dispatch(uint32_t x, uint32_t y, uint32_t z) const;
	void DispatchBase(uint32_t bx, uint32_t by, uint32_t bz, uint32_t x, uint32_t y, uint32_t z) const;
	void DispatchIndirect(vk::Buffer buffer, uint64_t offset) const;
	void BeginQuery(vk::QueryPool pool, uint32_t query, vk::QueryControlFlags flags) const;
	void EndQuery(vk::QueryPool pool, uint32_t query) const;

	[[nodiscard]] vk::CommandBuffer Handle() const;
	[[nodiscard]] NativeCommands Commands() const { return NativeCommands(*this); }
	[[nodiscard]] GraphicContext&   GetGraphics() const noexcept { return m_graphics; }
	[[nodiscard]] RenderContext&    GetContext() const noexcept { return m_context; }
	[[nodiscard]] HW::Context&      GetRegisters() const noexcept { return *m_registers; }
	[[nodiscard]] HW::UserConfig&   GetUserConfig() const noexcept { return *m_user_config; }
	[[nodiscard]] HW::Shader&       GetShaders() const noexcept { return *m_shaders; }

private:
	explicit CommandBuffer(CommandScheduler& scheduler);
	void Bind(HW::Context& registers, HW::UserConfig& user_config, HW::Shader& shaders) noexcept {
		m_registers   = &registers;
		m_user_config = &user_config;
		m_shaders     = &shaders;
	}

	void Begin();
	void End() const;

	RenderContext&      m_context;
	GraphicContext&     m_graphics;
	vk::CommandBuffer   m_buffer          = nullptr;
	// Non-null only while this facade represents an open worker command. No raw
	// handle may escape in that state, even after a recording drain.
	VkRecording::Backend* m_recorder = nullptr;
	uint32_t            m_debug_op        = 0;
	uint64_t            m_debug_submit_id = 0;
	uint32_t            m_debug_arg0      = 0;
	uint32_t            m_debug_arg1      = 0;
	uint32_t            m_debug_arg2      = 0;
	uint32_t            m_debug_arg3      = 0;
	uint64_t            m_debug_arg4      = 0;
	mutable RenderState m_render_state;
	mutable bool        m_rendering   = false;
	HW::Context*        m_registers   = nullptr;
	HW::UserConfig*     m_user_config = nullptr;
	HW::Shader*         m_shaders     = nullptr;

	friend class CommandScheduler;
	friend class NativeCommands;
	friend struct GuestClockTestAccess;
};

class RenderExecutor {
	friend IndirectCountSnapshot SnapshotIndirectCount(RenderContext&, uint64_t, uint32_t);
	friend std::shared_ptr<Buffer> SnapshotIndirectIndexedArgs(RenderContext&, const DrawIndirectArgs&, uint32_t, bool);
	friend std::shared_ptr<Buffer> ConvertIndirectMeshArgs(RenderContext&, uint64_t,
	    const ShaderMeshInputInfo&, uint32_t, uint64_t, uint32_t, uint64_t, bool);
public:
	explicit RenderExecutor(RenderContext& context): m_context(context) {}
	KYTY_CLASS_NO_COPY(RenderExecutor);

	void DispatchDirect(uint64_t submit_id, CommandBuffer& buffer, uint32_t thread_group_x,
	                    uint32_t thread_group_y, uint32_t thread_group_z, uint32_t mode, bool async_compute);
	void DispatchIndirect(uint64_t submit_id, CommandBuffer& buffer, uint64_t args_addr,
	                      uint32_t mode, bool async_compute);

	void PrepareBindings(const ShaderStageRuntime& runtime, PreparedBindings& prepared);
	void                           FindBuffers(std::span<PreparedBindings* const> stages);
	void                           RebindBuffers(PreparedBindings& bindings);
	template <bool Profile> void RebindBuffersImpl(PreparedBindings& bindings);
	void                           RebindImages(PreparedBindings& bindings);
	void CommitBindings(CommandBuffer& buffer, vk::PipelineBindPoint pipeline_bind_point,
	                    const PipelineCache::Pipeline&     pipeline,
	                    std::span<PreparedBindings* const> bindings);
	// Coherence, heap uploads, transitions and set allocation stay on the GPU
	// producer. This may rotate the command buffer. Consume the returned view
	// immediately; only BindingEmission::Enqueue may hand its copied bytes off.
	[[nodiscard]] VkRecording::BindingEmission PrepareBindingEmission(
	    CommandBuffer& buffer, vk::PipelineBindPoint point, const PipelineCache::Pipeline& pipeline,
	    std::span<PreparedBindings* const> bindings);
	[[nodiscard]] TextureBinding ResolveTexture(const ShaderRecompiler::IR::ImageResource&   resource,
	                                            const ShaderRecompiler::IR::DescriptorValue& value);

private:
	template <bool Profile>
	[[nodiscard]] TextureBinding ResolveTextureImpl(const ShaderRecompiler::IR::ImageResource& resource,
	                                               const ShaderRecompiler::IR::DescriptorValue& value,
	                                               uint64_t description_program, uint32_t description_resource);
	template <bool Profile>
	void PrepareImages(const ShaderStageRuntime& runtime, PreparedBindings& prepared);

	// False, with nothing drawn, when the draw's indirect records need CPU counts.
	bool DrawIndex(uint64_t submit_id, CommandBuffer& buffer, const DrawIndexArgs& args);
	bool DrawAuto(uint64_t submit_id, CommandBuffer& buffer, const DrawAutoArgs& args);

	struct GraphicsBindings {
		std::array<PreparedBindings, 3> vertex;
		std::optional<PreparedBindings> pixel;
	};

	void PrepareGraphicsBindings(std::span<PreparedBindings* const> stages,
	                             std::span<RenderColorInfo> colors);
	void ResolveRenderColorTarget(CommandBuffer& buffer, RenderColorInfo& target,
	                              uint32_t render_target_slice_offset, uint32_t render_target_slot,
	                              bool ignore_target_mask = false, bool exact_format = false);
	void ResolveRenderDepthTarget(CommandBuffer& buffer, RenderDepthInfo& target);
	[[nodiscard]] bool DepthStencilCopy(CommandBuffer& buffer);
	[[nodiscard]] bool CopyDepthToColor(CommandBuffer& buffer, const DrawCallInfo& draw,
	                                   const DrawEmitInfo& emit, DrawRenderState& state);
	[[nodiscard]] bool PrepareDrawRenderState(CommandBuffer& buffer,
	                                          const DrawCallInfo& draw,
	                                          uint32_t            render_target_slice_offset,
	                                          DrawRenderState& state);
	void ExecutePreparedDraw(uint64_t submit_id, CommandBuffer& buffer, const DrawCallInfo& draw,
	                         DrawRenderState& state, const DrawEmitInfo& emit,
	                         const DrawIndexBufferSource& index_source);
	[[nodiscard]] RenderState AcquireRenderTargets(CommandBuffer& buffer, RenderColorInfo* colors,
	                                               uint32_t color_count, RenderDepthInfo& depth,
	                                               vk::ImageAspectFlags& feedback_aspects,
	                                               std::span<PreparedBindings* const> stages = {});
	[[nodiscard]] bool        ResolveColorTargets(CommandBuffer& buffer,
	                                              uint32_t render_target_slice_offset);
	[[nodiscard]] bool        ConsumeMetadataColorOperation(CommandBuffer& buffer,
	                                                       uint32_t render_target_slice_offset);
	void                      BindImage(ImageId id, bool storage);
	void                      BindRenderTarget(ImageId id);
	void                      ResetBindings();
	[[nodiscard]] bool        TryConsumeComputeMetaClear(const ShaderComputeInputInfo& input,
	                                                     const CommandBuffer&          buffer);
	[[nodiscard]] bool TryConsumeComputeFill(const ShaderComputeInputInfo& input,
	                                        CommandBuffer& command, uint32_t group_x,
	                                        uint32_t group_y, uint32_t group_z, uint32_t mode);

	RenderContext&                        m_context;
	std::shared_ptr<IndirectCountClamp>    m_indirect_count_clamp;
	std::shared_ptr<struct IndirectMeshConvert> m_indirect_mesh_convert;
	GraphicsBindings                     m_graphics_bindings;
	PreparedBindings                     m_compute_bindings;
	std::vector<ImageId>                  m_bound_images;
	BindingOverlapWrites                 m_binding_overlap_writes;
	// Descriptor-derived descriptions only; bounded and owned by this GPU executor.
	std::unique_ptr<ImageDescriptionCache> m_image_descriptions;
	std::vector<vk::DescriptorImageInfo>  m_descriptor_images;
	std::vector<vk::WriteDescriptorSet>   m_descriptor_writes;
	std::array<vk::DescriptorBufferInfo, 2> m_address_descriptors;
	ShaderRecompiler::IR::PushData m_binding_push_data;

	friend class CommandProcessor;
	friend struct RenderExecutorTestAccess;
};

[[nodiscard]] bool ResolveComputeBufferFill(const ShaderComputeInputInfo& input, uint32_t group_x,
                                            uint32_t group_y, uint32_t group_z, uint32_t mode,
                                            ShaderBufferResource& descriptor,
                                            uint32_t& packed_clear, uint64_t& size);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_ */
