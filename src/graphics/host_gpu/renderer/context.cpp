#include "common/gpuThreadSampler.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/profiler.h"
#include "common/readbackProfile.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/vkRecorderBackend.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <bit>
#include <cstring>
namespace Libs::Graphics {

CommandBuffer::CommandBuffer(CommandScheduler& scheduler)
    : m_context(scheduler.Context()), m_graphics(scheduler.Graphics()) {}

bool CommandBuffer::IsInvalid() const {
	return m_buffer == nullptr && m_recorder == nullptr;
}

vk::CommandBuffer CommandBuffer::Handle() const {
	// Draining cannot make an escaped handle safe: only the worker owns this
	// pool, including between packets. Fail in release builds too.
	EXIT_NOT_IMPLEMENTED(m_recorder != nullptr);
	EXIT_IF(IsInvalid());
	return m_buffer;
}

void CommandBuffer::ResetQueryPool(vk::QueryPool pool, uint32_t first, uint32_t count) const {
	if (m_recorder) {
		EXIT_NOT_IMPLEMENTED(!m_recorder->Commands().Value(VkRecording::Op::ResetQueries, VkRecording::QueryReset {pool, first, count}));
		return;
	}
	Handle().resetQueryPool(pool, first, count);
}

void CommandBuffer::EmitBindings(const VkRecording::BindingEmission& emission) const {
	if (m_recorder) {
		if (!emission.Enqueue(m_recorder->Commands())) {
			Commands().Synchronous([&](vk::CommandBuffer native) { emission.Emit(m_graphics.device, native); });
		}
		return;
	}
	emission.Emit(m_graphics.device, Handle());
}

void CommandBuffer::WriteTimestamp(vk::PipelineStageFlagBits stage, vk::QueryPool pool, uint32_t query) const {
	if (m_recorder) {
		EXIT_NOT_IMPLEMENTED(!m_recorder->Commands().Value(VkRecording::Op::Timestamp, VkRecording::Timestamp {stage, pool, query}));
		return;
	}
	Handle().writeTimestamp(stage, pool, query);
}

void CommandBuffer::Begin() {
	EXIT_IF(m_rendering || IsInvalid());
	if (m_recorder) { m_recorder->Begin(); return; }
	auto buffer = Handle();

	vk::CommandBufferBeginInfo begin_info {};
	begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;

	Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::OtherNative);
	auto result = buffer.begin(&begin_info);

	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
}

void CommandBuffer::End() const {
	EndRendering();
	if (m_recorder) return; // Backend::Submit ends the native buffer on its owner.
	auto buffer = Handle();

	Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::OtherNative);
	auto result = buffer.end();

	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
}

void CommandBuffer::SetDebugInfo(uint32_t op, uint64_t submit_id, uint32_t arg0, uint32_t arg1,
                                 uint32_t arg2, uint32_t arg3, uint64_t arg4) {
	m_debug_op        = op;
	m_debug_submit_id = submit_id;
	m_debug_arg0      = arg0;
	m_debug_arg1      = arg1;
	m_debug_arg2      = arg2;
	m_debug_arg3      = arg3;
	m_debug_arg4      = arg4;
}

void CommandBuffer::BeginRendering(const RenderState& state) const {
	if (m_rendering && m_render_state == state) {
		return;
	}
	EXIT_IF(state.width == 0 || state.height == 0 || state.num_layers == 0 ||
	        state.num_color_attachments > RENDER_COLOR_ATTACHMENTS_MAX);
	EndRendering();

	const auto& depth_stencil = state.depth_stencil_attachment;
	if (Common::ReadbackProfile::DrawWork::record) {
		uint32_t clears = 0;
		for (uint32_t i = 0; i < state.num_color_attachments; ++i)
			if (state.color_attachments[i].is_clear) clears |= 1u << i;
		const uint32_t flags = clears | (uint32_t(depth_stencil.has_depth) << 8) |
		    (uint32_t(depth_stencil.has_stencil) << 9) | (uint32_t(depth_stencil.depth_clear) << 10) |
		    (uint32_t(depth_stencil.stencil_clear) << 11) | (state.num_color_attachments << 16);
		Common::ReadbackProfile::DrawWork::Record(Common::ReadbackProfile::DrawWork::Op::BeginPass,
		    0, 0, 0, state.width, state.height, state.num_layers, flags);
	}
	Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::OtherNative);
	if (m_recorder) {
		EXIT_NOT_IMPLEMENTED(!m_recorder->Commands().BeginRendering(state));
	} else {
		std::array<vk::RenderingAttachmentInfo, RENDER_COLOR_ATTACHMENTS_MAX> colors {};
		for (uint32_t i = 0; i < state.num_color_attachments; i++) {
			const auto& attachment = state.color_attachments[i];
			colors[i].imageView    = attachment.image_view;
			colors[i].imageLayout  = attachment.image_layout;
			colors[i].loadOp =
			    attachment.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
			colors[i].storeOp                 = vk::AttachmentStoreOp::eStore;
			colors[i].clearValue.color.uint32 = attachment.clear_value;
		}

		vk::RenderingAttachmentInfo depth {};
		depth.imageView   = depth_stencil.image_view;
		depth.imageLayout = depth_stencil.image_layout;
		depth.loadOp =
		    depth_stencil.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
		depth.storeOp                       = vk::AttachmentStoreOp::eStore;
		depth.clearValue.depthStencil.depth = std::bit_cast<float>(depth_stencil.clear_value[0]);

		vk::RenderingAttachmentInfo stencil {};
		stencil.imageView   = depth_stencil.image_view;
		stencil.imageLayout = depth_stencil.image_layout;
		stencil.loadOp =
		    depth_stencil.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
		stencil.storeOp                         = vk::AttachmentStoreOp::eStore;
		stencil.clearValue.depthStencil.stencil = depth_stencil.clear_value[1];

		vk::RenderingInfo rendering {};
		rendering.renderArea.extent    = {state.width, state.height};
		rendering.layerCount           = state.num_layers;
		rendering.colorAttachmentCount = state.num_color_attachments;
		rendering.pColorAttachments    = colors.data();
		rendering.pDepthAttachment     = depth_stencil.has_depth ? &depth : nullptr;
		rendering.pStencilAttachment   = depth_stencil.has_stencil ? &stencil : nullptr;
		Handle().beginRendering(rendering);
	}
	// The open pass holds the cleared contents, so a later clear begins a new pass.
	m_render_state = state;
	for (auto& color: m_render_state.color_attachments) {
		color.is_clear = false;
	}
	m_render_state.depth_stencil_attachment.depth_clear   = false;
	m_render_state.depth_stencil_attachment.stencil_clear = false;
	m_rendering                                           = true;
}

void CommandBuffer::EndRendering() const {
	if (!m_rendering) {
		return;
	}
	Common::ReadbackProfile::DrawWork::Record(Common::ReadbackProfile::DrawWork::Op::EndPass,
	    0, 0, 0, m_render_state.width, m_render_state.height, m_render_state.num_layers,
	    m_render_state.num_color_attachments << 16);
	Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::OtherNative);
	if (m_recorder) {
		EXIT_NOT_IMPLEMENTED(!m_recorder->Commands().EndRendering());
	} else Handle().endRendering();
	m_rendering    = false;
	m_render_state = {};
}

} // namespace Libs::Graphics
