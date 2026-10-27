#include "common/gpuThreadSampler.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"

#include "common/assert.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/shader/shader.h"
#include "graphics/shader/shaderBindings.h"

#include <cstring>

namespace Libs::Graphics {

vk::ShaderStageFlagBits NativeShaderStage(ShaderType stage) {
	switch (stage) {
		case ShaderType::Local:
		case ShaderType::Vertex: return vk::ShaderStageFlagBits::eVertex;
		case ShaderType::Mesh: return vk::ShaderStageFlagBits::eMeshEXT;
		case ShaderType::TessellationControl: return vk::ShaderStageFlagBits::eTessellationControl;
		case ShaderType::TessellationEvaluation:
			return vk::ShaderStageFlagBits::eTessellationEvaluation;
		case ShaderType::Pixel: return vk::ShaderStageFlagBits::eFragment;
		case ShaderType::Compute: return vk::ShaderStageFlagBits::eCompute;
		default: EXIT("unknown native shader stage\n");
	}
}

vk::PipelineStageFlags ShaderPipelineStages(vk::ShaderStageFlags stages) {
	vk::PipelineStageFlags result = {};
	if (stages & vk::ShaderStageFlagBits::eVertex) {
		result |= vk::PipelineStageFlagBits::eVertexShader;
	}
	if (stages & vk::ShaderStageFlagBits::eMeshEXT) {
		result |= vk::PipelineStageFlagBits::eMeshShaderEXT;
	}
	if (stages & vk::ShaderStageFlagBits::eTessellationControl) {
		result |= vk::PipelineStageFlagBits::eTessellationControlShader;
	}
	if (stages & vk::ShaderStageFlagBits::eTessellationEvaluation) {
		result |= vk::PipelineStageFlagBits::eTessellationEvaluationShader;
	}
	if (stages & vk::ShaderStageFlagBits::eFragment) {
		result |= vk::PipelineStageFlagBits::eFragmentShader;
	}
	if (stages & vk::ShaderStageFlagBits::eCompute) {
		result |= vk::PipelineStageFlagBits::eComputeShader;
	}
	EXIT_IF(!result);
	return result;
}

vk::MemoryBarrier MakeShaderWriteDependency() {
	vk::MemoryBarrier barrier {};
	barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite |
	                        vk::AccessFlagBits::eVertexAttributeRead |
	                        vk::AccessFlagBits::eIndexRead | vk::AccessFlagBits::eUniformRead |
	                        vk::AccessFlagBits::eIndirectCommandRead |
	                        vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite |
	                        vk::AccessFlagBits::eColorAttachmentRead |
	                        vk::AccessFlagBits::eColorAttachmentWrite;
	return barrier;
}

vk::MemoryBarrier MakeShaderAccessDependency() {
	vk::MemoryBarrier barrier {};
	barrier.srcAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	return barrier;
}

vk::MemoryBarrier MakeShaderWriteHazardDependency() {
	vk::MemoryBarrier barrier {};
	barrier.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
	return barrier;
}

vk::BufferMemoryBarrier MakeGdsDependency(vk::Buffer buffer) {
	EXIT_IF(buffer == nullptr);

	vk::BufferMemoryBarrier barrier {};
	barrier.srcAccessMask = vk::AccessFlagBits::eHostWrite | vk::AccessFlagBits::eTransferWrite |
	                        vk::AccessFlagBits::eShaderWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer              = buffer;
	barrier.offset              = 0;
	barrier.size                = VK_WHOLE_SIZE;
	return barrier;
}

bool HasShaderBufferWrites(const ShaderStageRuntime& runtime) {
	EXIT_IF(!runtime);
	const auto& program   = runtime.module->info;
	const auto& resources = *runtime.resources;
	EXIT_IF(resources.buffers.size() != program.info.buffers.size());
	// Address writes reach memory outside the bound buffer descriptors.
	bool has_writes = program.has_address_writes;
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		if (!program.info.buffers[i].written) {
			continue;
		}
		const auto& value = resources.buffers[i];
		EXIT_IF(value.dword_count < 4);
		ShaderBufferResource descriptor;
		std::memcpy(descriptor.fields, value.dwords.data(), sizeof(descriptor.fields));
		// A zero stride means byte addressing. For either addressing mode a nonzero record
		// count is exactly the condition for a nonempty descriptor range.
		has_writes |= descriptor.Base48() != 0 && descriptor.NumRecords() != 0;
	}
	return has_writes;
}

void ShaderAccessBarrier(const CommandBuffer& vk_buffer, vk::PipelineStageFlags source_stages) {
	EXIT_IF(vk_buffer.IsInvalid() || !source_stages);
	const auto barrier = MakeShaderAccessDependency();
	Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::Barrier);
	vk_buffer.EmitMemoryBarrier(source_stages, vk::PipelineStageFlagBits::eAllCommands,
	                          barrier);
}

void ShaderWriteHazardBarrier(const CommandBuffer&   vk_buffer,
                              vk::PipelineStageFlags destination_stages) {
	EXIT_IF(vk_buffer.IsInvalid() || !destination_stages);
	const auto barrier = MakeShaderWriteHazardDependency();
	Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::Barrier);
	vk_buffer.EmitMemoryBarrier(vk::PipelineStageFlagBits::eAllCommands, destination_stages,
	                          barrier);
}

void ShaderWriteBarrier(const CommandBuffer& vk_buffer, vk::PipelineStageFlags source_stages) {
	EXIT_IF(vk_buffer.IsInvalid() || !source_stages);
	const auto barrier = MakeShaderWriteDependency();
	Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::Barrier);
	vk_buffer.EmitMemoryBarrier(source_stages,
	                          vk::PipelineStageFlagBits::eComputeShader |
	                              vk::PipelineStageFlagBits::eAllGraphics |
	                              vk::PipelineStageFlagBits::eTransfer,
	                          barrier);
}

} // namespace Libs::Graphics
