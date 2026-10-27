#include "graphics/host_gpu/renderer/dynamicState.h"

#include "graphics/host_gpu/graphicContext.h"

namespace Libs::Graphics {

template <bool Emit> void DynamicState::CommitImpl(const GraphicContext& graphics, vk::CommandBuffer cmdbuf) {
	if (dirty_state.viewports) {
		dirty_state.viewports = false;
		if constexpr (Emit) cmdbuf.setViewportWithCount(num_viewports, viewports.data());
	}
	if (dirty_state.scissors) {
		dirty_state.scissors = false;
		if constexpr (Emit) cmdbuf.setScissorWithCount(num_scissors, scissors.data());
	}
	if (dirty_state.depth_test_enabled) {
		dirty_state.depth_test_enabled = false;
		if constexpr (Emit) cmdbuf.setDepthTestEnable(depth_test_enabled);
	}
	if (dirty_state.depth_write_enabled) {
		dirty_state.depth_write_enabled = false;
		// Note that this must be set in a command buffer even if depth test is disabled.
		if constexpr (Emit) cmdbuf.setDepthWriteEnable(depth_write_enabled);
	}
	if (depth_test_enabled && dirty_state.depth_compare_op) {
		dirty_state.depth_compare_op = false;
		if constexpr (Emit) cmdbuf.setDepthCompareOp(depth_compare_op);
	}
	if (dirty_state.depth_bounds_test_enabled) {
		dirty_state.depth_bounds_test_enabled = false;
#if !defined(__APPLE__)
		if constexpr (Emit) cmdbuf.setDepthBoundsTestEnable(depth_bounds_test_enabled);
#endif
	}
	if (depth_bounds_test_enabled && dirty_state.depth_bounds) {
		dirty_state.depth_bounds = false;
#if !defined(__APPLE__)
		if constexpr (Emit) cmdbuf.setDepthBounds(depth_bounds_min, depth_bounds_max);
#endif
	}
	if (dirty_state.depth_bias_enabled) {
		dirty_state.depth_bias_enabled = false;
		if constexpr (Emit) cmdbuf.setDepthBiasEnable(depth_bias_enabled);
	}
	if (depth_bias_enabled && dirty_state.depth_bias) {
		dirty_state.depth_bias = false;
		if constexpr (Emit) cmdbuf.setDepthBias(depth_bias_constant, depth_bias_clamp, depth_bias_slope);
	}
	if (dirty_state.stencil_test_enabled) {
		dirty_state.stencil_test_enabled = false;
		if constexpr (Emit) cmdbuf.setStencilTestEnable(stencil_test_enabled);
	}
	if (stencil_test_enabled) {
		if (dirty_state.stencil_front_ops && dirty_state.stencil_back_ops &&
		    stencil_front_ops == stencil_back_ops) {
			dirty_state.stencil_front_ops = false;
			dirty_state.stencil_back_ops  = false;
			if constexpr (Emit) cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eFrontAndBack, stencil_front_ops.fail_op,
			                    stencil_front_ops.pass_op, stencil_front_ops.depth_fail_op,
			                    stencil_front_ops.compare_op);
		} else {
			if (dirty_state.stencil_front_ops) {
				dirty_state.stencil_front_ops = false;
				if constexpr (Emit) cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eFront, stencil_front_ops.fail_op,
				                    stencil_front_ops.pass_op, stencil_front_ops.depth_fail_op,
				                    stencil_front_ops.compare_op);
			}
			if (dirty_state.stencil_back_ops) {
				dirty_state.stencil_back_ops = false;
				if constexpr (Emit) cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eBack, stencil_back_ops.fail_op,
				                    stencil_back_ops.pass_op, stencil_back_ops.depth_fail_op,
				                    stencil_back_ops.compare_op);
			}
		}
		if (dirty_state.stencil_front_reference && dirty_state.stencil_back_reference &&
		    stencil_front_reference == stencil_back_reference) {
			dirty_state.stencil_front_reference = false;
			dirty_state.stencil_back_reference  = false;
			if constexpr (Emit) cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack,
			                           stencil_front_reference);
		} else {
			if (dirty_state.stencil_front_reference) {
				dirty_state.stencil_front_reference = false;
				if constexpr (Emit) cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFront,
				                           stencil_front_reference);
			}
			if (dirty_state.stencil_back_reference) {
				dirty_state.stencil_back_reference = false;
				if constexpr (Emit) cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eBack, stencil_back_reference);
			}
		}
		if (dirty_state.stencil_front_write_mask && dirty_state.stencil_back_write_mask &&
		    stencil_front_write_mask == stencil_back_write_mask) {
			dirty_state.stencil_front_write_mask = false;
			dirty_state.stencil_back_write_mask  = false;
			if constexpr (Emit) cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack,
			                           stencil_front_write_mask);
		} else {
			if (dirty_state.stencil_front_write_mask) {
				dirty_state.stencil_front_write_mask = false;
				if constexpr (Emit) cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFront,
				                           stencil_front_write_mask);
			}
			if (dirty_state.stencil_back_write_mask) {
				dirty_state.stencil_back_write_mask = false;
				if constexpr (Emit) cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eBack, stencil_back_write_mask);
			}
		}
		if (dirty_state.stencil_front_compare_mask && dirty_state.stencil_back_compare_mask &&
		    stencil_front_compare_mask == stencil_back_compare_mask) {
			dirty_state.stencil_front_compare_mask = false;
			dirty_state.stencil_back_compare_mask  = false;
			if constexpr (Emit) cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFrontAndBack,
			                             stencil_front_compare_mask);
		} else {
			if (dirty_state.stencil_front_compare_mask) {
				dirty_state.stencil_front_compare_mask = false;
				if constexpr (Emit) cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFront,
				                             stencil_front_compare_mask);
			}
			if (dirty_state.stencil_back_compare_mask) {
				dirty_state.stencil_back_compare_mask = false;
				if constexpr (Emit) cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eBack,
				                             stencil_back_compare_mask);
			}
		}
	}
	if (dirty_state.blend_constants) {
		dirty_state.blend_constants = false;
		if constexpr (Emit) cmdbuf.setBlendConstants(blend_constants.data());
	}
	if (dirty_state.color_write_enables) {
		dirty_state.color_write_enables = false;
#if !defined(__APPLE__)
		if (num_color_write_enables != 0) {
			if constexpr (Emit) cmdbuf.setColorWriteEnableEXT(num_color_write_enables, color_write_enables.data());
		}
#endif
	}
	if (dirty_state.line_width) {
		dirty_state.line_width = false;
		if constexpr (Emit) cmdbuf.setLineWidth(line_width);
	}
	if (dirty_state.feedback_loop_aspects && graphics.attachment_feedback_loop_enabled) {
		dirty_state.feedback_loop_aspects = false;
		if constexpr (Emit) cmdbuf.setAttachmentFeedbackLoopEnableEXT(feedback_loop_aspects);
	}
}

void DynamicState::Commit(const GraphicContext& graphics, vk::CommandBuffer cmdbuf) {
	CommitImpl<true>(graphics, cmdbuf);
}
void DynamicState::Acknowledge(const GraphicContext& graphics) {
	CommitImpl<false>(graphics, {});
}

} // namespace Libs::Graphics
