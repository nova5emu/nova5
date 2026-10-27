#include "common/gpuThreadSampler.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/profiler.h"
#include "common/submitProfile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvBuilder.h"

#include <cstring>
#include <cstdlib>
#include <numeric>
#include <vk_mem_alloc.h>

namespace Libs::Graphics {

namespace {

constexpr size_t WATCHES_INITIAL_RESERVE = 0x4000;
constexpr size_t WATCHES_RESERVE_CHUNK   = 0x1000;

// Read once on the import/probe path, never while the experiment's gate is off.
// -1 preserves the existing cached preference; -2 rejects an invalid override.
int RequestedHostImportType() {
	static const int requested = [] {
		const auto* value = std::getenv("KYTY_HOST_IMPORT_TYPE");
		if (!value || !*value) return -1;
		if (std::strcmp(value, "2") == 0) return 2;
		if (std::strcmp(value, "3") == 0) return 3;
		std::fprintf(stderr, "[GUEST_HOST_IMPORT] INERT: invalid KYTY_HOST_IMPORT_TYPE (expected 2 or 3)\n");
		return -2;
	}();
	return requested;
}

[[nodiscard]] VmaAllocationCreateFlags AllocationFlags(MemoryUsage usage) {
	switch (usage) {
		case MemoryUsage::Upload:
		case MemoryUsage::Stream:
			return VMA_ALLOCATION_CREATE_MAPPED_BIT |
			       VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
		case MemoryUsage::Download:
			return VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
		case MemoryUsage::DeviceLocal: return {};
	}
	return {};
}

[[nodiscard]] VmaMemoryUsage AllocationUsage(MemoryUsage usage) {
	switch (usage) {
		case MemoryUsage::DeviceLocal:
		case MemoryUsage::Stream: return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
		case MemoryUsage::Upload:
		case MemoryUsage::Download: return VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
	}
	return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
}

[[nodiscard]] bool AlignUp(uint64_t value, uint64_t alignment, uint64_t& result) {
	if (alignment == 0) {
		result = value;
		return true;
	}
	const auto aligned = Common::AlignUp(value, alignment);
	if (aligned < value) {
		return false;
	}
	result = aligned;
	return true;
}

} // namespace

Buffer::Buffer(GraphicContext& graphics, CommandScheduler& scheduler, MemoryUsage usage,
               uint64_t cpu_address, vk::BufferUsageFlags flags, uint64_t size)
    : m_graphics(&graphics), m_scheduler(&scheduler), m_usage(usage), m_cpu_address(cpu_address),
      m_size(size) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(graphics.allocator == nullptr || size == 0);

	vk::BufferCreateInfo buffer_info {};
	buffer_info.size        = size;
	buffer_info.usage       = flags;

	const bool with_bda = bool(flags & vk::BufferUsageFlagBits::eShaderDeviceAddress);
	VmaAllocationCreateInfo allocation_info {};
	allocation_info.flags =
	    VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | AllocationFlags(usage);
	allocation_info.usage = AllocationUsage(usage);
	allocation_info.preferredFlags = usage == MemoryUsage::DeviceLocal
	                                     ? VkMemoryPropertyFlags {}
	                                     : VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

	VmaAllocationInfo allocation_result {};
	VkBuffer          native_buffer = VK_NULL_HANDLE;
	const auto        result        = static_cast<vk::Result>(vmaCreateBuffer(
	    graphics.allocator, static_cast<const VkBufferCreateInfo*>(buffer_info), &allocation_info,
	    &native_buffer, &m_allocation, &allocation_result));
	if (result != vk::Result::eSuccess) {
		graphics.LogMemoryBudget();
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	m_buffer = native_buffer;
	if (with_bda) {
		vk::BufferDeviceAddressInfo address_info {};
		address_info.buffer = m_buffer;
		m_device_address    = graphics.device.getBufferAddress(address_info);
		EXIT_IF(m_device_address == 0);
	}

	VkMemoryPropertyFlags properties = 0;
	vmaGetAllocationMemoryProperties(graphics.allocator, m_allocation, &properties);
	m_coherent = (properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
	if (allocation_result.pMappedData != nullptr) {
		m_mapped = {static_cast<uint8_t*>(allocation_result.pMappedData),
		            static_cast<size_t>(size)};
	}
}

Buffer::~Buffer() {
	if (m_host_import) {
		if (m_buffer) m_graphics->device.destroyBuffer(m_buffer);
		if (m_import_memory) m_graphics->device.freeMemory(m_import_memory);
		return;
	}
	if (m_buffer != nullptr) {
		vmaDestroyBuffer(m_graphics->allocator, m_buffer, m_allocation);
	}
}

Buffer::Buffer(GraphicContext& graphics, CommandScheduler& scheduler, uint64_t address, uint64_t size)
    : m_host_import(true), m_graphics(&graphics), m_scheduler(&scheduler), m_usage(MemoryUsage::Upload),
      m_cpu_address(address), m_size(size), m_coherent(true) {}

std::unique_ptr<Buffer> Buffer::ImportHost(GraphicContext& graphics, CommandScheduler& scheduler,
    uint64_t address, void* pointer, uint64_t size, Common::SubmitProfile::Extra& failure) {
	using Extra = Common::SubmitProfile::Extra;
	failure = Extra::HostImportFallbackUnsupported;
	if (!graphics.guest_host_import_enabled || !VULKAN_HPP_DEFAULT_DISPATCHER.vkGetMemoryHostPointerPropertiesEXT) return {};
	failure = Extra::HostImportFallbackAlignment;
	const auto alignment = graphics.host_import_alignment;
	if (!pointer || !size || !alignment || (reinterpret_cast<uintptr_t>(pointer) % alignment) || size % alignment) return {};
	constexpr auto handle = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT;
	constexpr auto usage = vk::BufferUsageFlagBits::eShaderDeviceAddress | vk::BufferUsageFlagBits::eStorageBuffer |
	                       vk::BufferUsageFlagBits::eTransferSrc;
	vk::PhysicalDeviceExternalBufferInfo external_info {}; external_info.handleType = handle; external_info.usage = usage;
	vk::ExternalBufferProperties external {};
	graphics.physical_device.getExternalBufferProperties(&external_info, &external);
	failure = Extra::HostImportFallbackType;
	if (!(external.externalMemoryProperties.externalMemoryFeatures & vk::ExternalMemoryFeatureFlagBits::eImportable)) return {};
	vk::MemoryHostPointerPropertiesEXT host {};
	if (graphics.device.getMemoryHostPointerPropertiesEXT(handle, pointer, &host) != vk::Result::eSuccess) return {};
	graphics.host_import_type_bits = host.memoryTypeBits;
	auto result = std::unique_ptr<Buffer>(new Buffer(graphics, scheduler, address, size));
	vk::ExternalMemoryBufferCreateInfo external_create {}; external_create.handleTypes = handle;
	vk::BufferCreateInfo create {}; create.pNext = &external_create; create.size = size; create.usage = usage;
	failure = Extra::HostImportFallbackAllocation;
	if (graphics.device.createBuffer(&create, nullptr, &result->m_buffer) != vk::Result::eSuccess) return {};
	vk::MemoryRequirements requirements {};
	graphics.device.getBufferMemoryRequirements(result->m_buffer, &requirements);
	failure = Extra::HostImportFallbackAlignment;
	if (requirements.size != size) return {}; // Dedicated allocation, no padding outside the leased span.
	const auto bits = requirements.memoryTypeBits & host.memoryTypeBits;
	uint32_t type = UINT32_MAX;
	graphics.host_import_accepted_type_bits = 0;
	constexpr auto coherent = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
	for (uint32_t i = 0; i < graphics.physical_device_memory_properties.memoryTypeCount; ++i) {
		const auto flags = graphics.physical_device_memory_properties.memoryTypes[i].propertyFlags;
		if (!(bits & (1u << i)) || (flags & coherent) != coherent) continue;
		graphics.host_import_accepted_type_bits |= 1u << i;
		if (type == UINT32_MAX || (flags & vk::MemoryPropertyFlagBits::eHostCached)) type = i;
	}
	failure = Extra::HostImportFallbackType;
	const int requested = RequestedHostImportType();
	if (requested >= 0) {
		// Validate against BOTH the host pointer and this VkBuffer's requirements,
		// including HOST_VISIBLE|HOST_COHERENT. Never silently pick another type.
		type = (graphics.host_import_accepted_type_bits & (1u << requested)) ? static_cast<uint32_t>(requested) : UINT32_MAX;
	} else if (requested == -2) {
		type = UINT32_MAX;
	}
	if (type == UINT32_MAX) return {};
	vk::MemoryDedicatedAllocateInfo dedicated {}; dedicated.buffer = result->m_buffer;
	vk::ImportMemoryHostPointerInfoEXT import {}; import.pNext = &dedicated; import.handleType = handle; import.pHostPointer = pointer;
	vk::MemoryAllocateFlagsInfo flags {}; flags.pNext = &import; flags.flags = vk::MemoryAllocateFlagBits::eDeviceAddress;
	vk::MemoryAllocateInfo allocate {}; allocate.pNext = &flags; allocate.allocationSize = size; allocate.memoryTypeIndex = type;
	failure = Extra::HostImportFallbackAllocation;
	if (graphics.device.allocateMemory(&allocate, nullptr, &result->m_import_memory) != vk::Result::eSuccess ||
	    graphics.device.bindBufferMemory(result->m_buffer, result->m_import_memory, 0) != vk::Result::eSuccess) return {};
	vk::BufferDeviceAddressInfo bda {}; bda.buffer = result->m_buffer;
	result->m_device_address = graphics.device.getBufferAddress(bda);
	if (!result->m_device_address) return {};
	graphics.host_import_memory_type = type;
	result->m_mapped = {static_cast<uint8_t*>(pointer), static_cast<size_t>(size)};
	return result;
}

namespace {
// Runs at the first B boundary, on an independent command pool. Two CPU
// writes through the guest-like view must be seen by BDA reads of the RW alias.
bool ProbeImportedBda(GraphicContext& graphics, Buffer& imported, void* writer) {
	using namespace spv;
	ShaderRecompiler::Spirv::Builder b;
	b.RequireCapability(CapabilityShader);
	b.RequireCapability(CapabilityInt64);
	b.RequireCapability(CapabilityPhysicalStorageBufferAddresses);
	b.RequireExtension("SPV_KHR_physical_storage_buffer");
	b.AddMemoryModel(AddressingModelPhysicalStorageBuffer64, MemoryModelGLSL450);
	const auto u32 = b.Type(OpTypeInt, 32u, 0u), u64 = b.Type(OpTypeInt, 64u, 0u);
	const auto void_type = b.Type(OpTypeVoid), fn = b.Type(OpTypeFunction, void_type);
	const auto params = b.DecoratedType(OpTypeStruct,
	    {{OpDecorate, {DecorationBlock}}, {OpMemberDecorate, {0u, DecorationOffset, 0u}},
	     {OpMemberDecorate, {1u, DecorationOffset, 8u}}}, u64, u64);
	const auto push_ptr = b.Type(OpTypePointer, StorageClassPushConstant, u64);
	const auto phys_ptr = b.Type(OpTypePointer, StorageClassPhysicalStorageBuffer, u32);
	const auto push = b.DefineGlobalVariable(b.Type(OpTypePointer, StorageClassPushConstant, params), StorageClassPushConstant);
	const auto entry = b.AllocateId();
	b.AddEntryPoint(ExecutionModelGLCompute, entry, "main", {});
	b.AddExecutionMode(entry, ExecutionModeLocalSize, 1u, 1u, 1u);
	b.AddFunction(OpFunction, void_type, entry, FunctionControlMaskNone, fn);
	b.AddFunction(OpLabel, b.AllocateId());
	std::array<uint32_t, 2> pointers {};
	for (uint32_t i = 0; i < 2; ++i) {
		const auto member = b.AllocateId(), address = b.AllocateId();
		pointers[i] = b.AllocateId();
		b.AddFunction(OpAccessChain, push_ptr, member, push, b.Constant(OpConstant, u32, i));
		b.AddFunction(OpLoad, u64, address, member);
		b.AddFunction(OpConvertUToPtr, phys_ptr, pointers[i], address);
	}
	const auto value = b.AllocateId();
	b.AddFunction(OpLoad, u32, value, pointers[0], MemoryAccessAlignedMask, 4u);
	b.AddFunction(OpStore, pointers[1], value, MemoryAccessAlignedMask, 4u);
	b.AddFunction(OpReturn);
	b.AddFunction(OpFunctionEnd);
	struct Objects {
		vk::Device device;
		vk::ShaderModule module {}; vk::PipelineLayout layout {}; vk::Pipeline pipeline {};
		vk::CommandPool pool {}; vk::Fence fence {};
		~Objects() {
			if (fence) device.destroyFence(fence);
			if (pool) device.destroyCommandPool(pool);
			if (pipeline) device.destroyPipeline(pipeline);
			if (layout) device.destroyPipelineLayout(layout);
			if (module) device.destroyShaderModule(module);
		}
	} objects {graphics.device};
	const auto code = b.Build();
	vk::ShaderModuleCreateInfo shader {}; shader.codeSize = code.size() * sizeof(uint32_t); shader.pCode = code.data();
	if (graphics.device.createShaderModule(&shader, nullptr, &objects.module) != vk::Result::eSuccess) return false;
	const vk::PushConstantRange range {vk::ShaderStageFlagBits::eCompute, 0, 16};
	vk::PipelineLayoutCreateInfo layout {}; layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &range;
	if (graphics.device.createPipelineLayout(&layout, nullptr, &objects.layout) != vk::Result::eSuccess) return false;
	vk::ComputePipelineCreateInfo pipeline {}; pipeline.layout = objects.layout;
	pipeline.stage.stage = vk::ShaderStageFlagBits::eCompute; pipeline.stage.module = objects.module; pipeline.stage.pName = "main";
	if (graphics.device.createComputePipelines(nullptr, 1, &pipeline, nullptr, &objects.pipeline) != vk::Result::eSuccess) return false;
	vk::CommandPoolCreateInfo pool {}; pool.queueFamilyIndex = graphics.queue_family;
	pool.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
	if (graphics.device.createCommandPool(&pool, nullptr, &objects.pool) != vk::Result::eSuccess) return false;
	vk::CommandBufferAllocateInfo allocate {}; allocate.commandPool = objects.pool;
	allocate.level = vk::CommandBufferLevel::ePrimary; allocate.commandBufferCount = 1;
	vk::CommandBuffer command {};
	if (graphics.device.allocateCommandBuffers(&allocate, &command) != vk::Result::eSuccess) return false;
	vk::FenceCreateInfo fence {};
	if (graphics.device.createFence(&fence, nullptr, &objects.fence) != vk::Result::eSuccess) return false;
	for (const uint32_t pattern: {0x13579bdfu, 0xe2468ac0u}) {
		std::memcpy(writer, &pattern, sizeof(pattern));
		vk::CommandBufferBeginInfo begin {}; begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
		if (command.begin(&begin) != vk::Result::eSuccess) return false;
		const std::array<vk::DeviceAddress, 2> addresses {imported.BufferDeviceAddress(), imported.BufferDeviceAddress() + imported.Size() - 4};
		command.bindPipeline(vk::PipelineBindPoint::eCompute, objects.pipeline);
		command.pushConstants(objects.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(addresses), addresses.data());
		command.dispatch(1, 1, 1);
		vk::MemoryBarrier barrier {}; barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite; barrier.dstAccessMask = vk::AccessFlagBits::eHostRead;
		command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eHost, {}, 1, &barrier, 0, nullptr, 0, nullptr);
		if (command.end() != vk::Result::eSuccess) return false;
		vk::SubmitInfo submit {}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
		{
			Common::LockGuard queue_lock(graphics.queue_mutex);
			if (graphics.queue.submit(1, &submit, objects.fence) != vk::Result::eSuccess) return false;
		}
		if (graphics.device.waitForFences(1, &objects.fence, true, UINT64_MAX) != vk::Result::eSuccess) return false;
		uint32_t read = 0;
		std::memcpy(&read, imported.Mapped().data() + imported.Size() - 4, sizeof(read));
		if (read != pattern) return false;
		if (graphics.device.resetFences(1, &objects.fence) != vk::Result::eSuccess || command.reset({}) != vk::Result::eSuccess) return false;
	}
	return true;
}
} // namespace

void Buffer::ProbeHostImport(GraphicContext& graphics, CommandScheduler& scheduler) {
	if (!Common::SubmitProfile::ExperimentSelected(Common::SubmitProfile::Experiment::GuestHostImport)) return;
	bool accepted = false;
	Common::SubmitProfile::Extra failure = Common::SubmitProfile::Extra::HostImportFallbackUnsupported;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (graphics.guest_host_import_enabled && graphics.host_import_alignment) {
		failure = Common::SubmitProfile::Extra::HostImportFallbackAllocation;
		const auto size = Common::AlignUp(uint64_t {65536}, graphics.host_import_alignment);
		const auto mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT,
		    static_cast<DWORD>(size >> 32), static_cast<DWORD>(size), nullptr);
		if (mapping) {
			auto* pointer = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, size);
			if (pointer) {
				auto* writer = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, size);
				{
					auto imported = ImportHost(graphics, scheduler, 0, pointer, size, failure);
					if (imported) failure = Common::SubmitProfile::Extra::HostImportFallbackProbe;
					accepted = imported && writer && ProbeImportedBda(graphics, *imported, writer);
				}
				if (writer) UnmapViewOfFile(writer);
				UnmapViewOfFile(pointer);
			}
			CloseHandle(mapping);
		}
	}
#endif
	const int requested = RequestedHostImportType();
	const char* requested_name = requested == -1 ? "default" : requested == 2 ? "2" : requested == 3 ? "3" : "invalid";
	const uint32_t selected_flags = graphics.host_import_memory_type < graphics.physical_device_memory_properties.memoryTypeCount ?
	    static_cast<uint32_t>(graphics.physical_device_memory_properties.memoryTypes[graphics.host_import_memory_type].propertyFlags) : 0;
	std::fprintf(stderr, "[GUEST_HOST_IMPORT] extension=%u support=%u host_pointer_type_bits=0x%x accepted_type_bits=0x%x selected_type=%u requested_type=%s selected_flags=0x%x alignment=%llu bda_alias_probe=%s%s\n",
	    unsigned(graphics.guest_host_import_enabled), unsigned(accepted), graphics.host_import_type_bits,
	    graphics.host_import_accepted_type_bits, graphics.host_import_memory_type, requested_name, selected_flags,
	    static_cast<unsigned long long>(graphics.host_import_alignment),
	    accepted ? "pass" : "fail", accepted ? "" : " INERT: compatibility probe unavailable/failed");
	if (!accepted) std::fprintf(stderr, "[GUEST_HOST_IMPORT] fallback=%s\n", Common::SubmitProfile::ExtraNames[static_cast<size_t>(failure)]);
	graphics.guest_host_import_enabled = accepted;
}

vk::DeviceAddress Buffer::BufferDeviceAddress() const noexcept {
	EXIT_IF(m_device_address == 0);
	return m_device_address;
}

bool Buffer::IsInBounds(uint64_t address, uint64_t size) const noexcept {
	return address >= m_cpu_address && size <= Size() && address - m_cpu_address <= Size() - size;
}

void Buffer::Flush(uint64_t offset, uint64_t size) {
	EXIT_IF(m_mapped.empty() || offset > Size() || size > Size() - offset);
	if (!IsCoherent() && size != 0) {
		const auto result =
		    vmaFlushAllocation(m_graphics->allocator, m_allocation, offset, size);
		EXIT_NOT_IMPLEMENTED(static_cast<vk::Result>(result) != vk::Result::eSuccess);
	}
}

void Buffer::Invalidate(uint64_t offset, uint64_t size) {
	EXIT_IF(m_usage != MemoryUsage::Download || offset > Size() || size > Size() - offset);
	if (!IsCoherent() && size != 0) {
		const auto result =
		    vmaInvalidateAllocation(m_graphics->allocator, m_allocation, offset, size);
		EXIT_NOT_IMPLEMENTED(static_cast<vk::Result>(result) != vk::Result::eSuccess);
	}
}

vk::BufferMemoryBarrier Buffer::Barrier(uint64_t offset, uint64_t size, vk::AccessFlags source,
                                        vk::AccessFlags destination) const {
	if (Handle() == nullptr || size == 0 || offset > Size() || size > Size() - offset) {
		EXIT("Buffer: invalid DMA barrier, handle=%p offset=0x%016" PRIx64 " size=0x%016" PRIx64
		     " capacity=0x%016" PRIx64 "\n",
		     static_cast<const void*>(Handle()), offset, size, Size());
	}
	vk::BufferMemoryBarrier barrier {};
	barrier.srcAccessMask       = source;
	barrier.dstAccessMask       = destination;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer              = Handle();
	barrier.offset              = offset;
	barrier.size                = size;
	return barrier;
}

void Buffer::CopyFrom(CommandBuffer& command, const Buffer& source, uint64_t source_offset,
                      uint64_t destination_offset, uint64_t size, vk::AccessFlags source_before,
                      vk::AccessFlags destination_before, vk::AccessFlags source_after,
                      vk::AccessFlags destination_after) {
	if (size == 0 || source_offset > source.Size() || size > source.Size() - source_offset ||
	    destination_offset > Size() || size > Size() - destination_offset) {
		EXIT("Buffer: invalid copy range\n");
	}
	if (source.Handle() == Handle() && source_offset < destination_offset + size &&
	    destination_offset < source_offset + size) {
		EXIT("Buffer: overlapping self-copy\n");
	}
	command.EndRendering();
	const vk::BufferMemoryBarrier before[] = {
	    source.Barrier(source_offset, size, source_before, vk::AccessFlagBits::eTransferRead),
	    Barrier(destination_offset, size, destination_before, vk::AccessFlagBits::eTransferWrite),
	};
	const auto host_access  = vk::AccessFlagBits::eHostRead | vk::AccessFlagBits::eHostWrite;
	auto       before_stage = vk::PipelineStageFlags {vk::PipelineStageFlagBits::eAllCommands};
	if (static_cast<bool>((source_before | destination_before) & host_access)) {
		before_stage |= vk::PipelineStageFlagBits::eHost;
	}
	const auto native = command.Commands();
	Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::Barrier);
	native.pipelineBarrier(before_stage, vk::PipelineStageFlagBits::eTransfer,
	                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 2, before, 0, nullptr);
	const vk::BufferCopy copy {source_offset, destination_offset, size};
	Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::Copy);
	native.copyBuffer(source.Handle(), Handle(), 1, &copy);
	const vk::BufferMemoryBarrier after[] = {
	    source.Barrier(source_offset, size, vk::AccessFlagBits::eTransferRead, source_after),
	    Barrier(destination_offset, size, vk::AccessFlagBits::eTransferWrite, destination_after),
	};
	auto after_stage = vk::PipelineStageFlags {vk::PipelineStageFlagBits::eAllCommands};
	if (static_cast<bool>((source_after | destination_after) & host_access)) {
		after_stage |= vk::PipelineStageFlagBits::eHost;
	}
	Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::Barrier);
	native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, after_stage,
	                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 2, after, 0, nullptr);
}

void Buffer::Fill(uint64_t offset, uint64_t size, uint32_t value) {
	if (((offset | size) & 3u) != 0) {
		EXIT("Buffer: fill range must be dword aligned\n");
	}
	auto& command = Scheduler().Current();
	command.EndRendering();
	const auto before =
	    Barrier(offset, size, vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
	            vk::AccessFlagBits::eTransferWrite);
	const auto native = command.Commands();
	Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::Barrier);
	native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                       vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
	                       0, nullptr, 1, &before, 0, nullptr);
	Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::Copy);
	native.fillBuffer(Handle(), offset, size, value);
	const auto after = Barrier(offset, size, vk::AccessFlagBits::eTransferWrite,
	                           vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite);
	Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::Barrier);
	native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                       vk::PipelineStageFlagBits::eAllCommands,
	                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &after, 0, nullptr);
}

StreamBuffer::StreamBuffer(GraphicContext& graphics, CommandScheduler& scheduler, MemoryUsage usage,
                           uint64_t size)
    : Buffer(graphics, scheduler, usage, 0, AllFlags, size),
      m_current_watches(WATCHES_INITIAL_RESERVE), m_previous_watches(WATCHES_INITIAL_RESERVE) {}

bool StreamBuffer::NormalizeReservation(bool coherent, uint64_t atom, uint64_t& size,
                                        uint64_t& alignment) {
	if (coherent) {
		return true;
	}
	if (!AlignUp(size, atom, size)) {
		return false;
	}
	const auto divisor = std::gcd(alignment, atom);
	if (alignment != 0 && alignment / divisor > UINT64_MAX / atom) {
		return false;
	}
	alignment = alignment == 0 ? atom : alignment / divisor * atom;
	return true;
}

std::pair<uint8_t*, uint64_t> StreamBuffer::Map(uint64_t size, uint64_t alignment,
                                                bool allow_wait) {
	if (Mapped().empty()) {
		return {nullptr, 0};
	}
	uint64_t   mapped_size = size;
	const auto atom        = Graphics().physical_device_properties.limits.nonCoherentAtomSize;
	if (!NormalizeReservation(IsCoherent(), atom, mapped_size, alignment)) {
		return {nullptr, 0};
	}
	if (mapped_size > Size()) {
		return {nullptr, 0};
	}

	uint64_t aligned_offset = 0;
	if (!AlignUp(m_offset, alignment, aligned_offset)) {
		return {nullptr, 0};
	}

	const bool wrap = aligned_offset > Size() - mapped_size;
	if (wrap) {
		aligned_offset = 0;
	}

	auto wait_cursor = wrap ? size_t {0} : m_wait_cursor;
	auto wait_bound  = wrap ? uint64_t {0} : m_wait_bound;
	auto invalidation_mark =
	    wrap ? std::optional<size_t> {m_current_watch_cursor} : m_invalidation_mark;
	auto& pending_watches = wrap ? m_current_watches : m_previous_watches;
	if (!WaitPendingOperations(pending_watches, invalidation_mark, aligned_offset + mapped_size,
	                           allow_wait, wait_cursor, wait_bound)) {
		return {nullptr, 0};
	}

	if (wrap) {
		m_invalidation_mark    = invalidation_mark;
		m_current_watch_cursor = 0;
		std::swap(m_previous_watches, m_current_watches);
	}
	m_wait_cursor = wait_cursor;
	m_wait_bound  = wait_bound;
	m_offset      = aligned_offset;
	m_mapped_size = mapped_size;
	return {Mapped().data() + m_offset, m_offset};
}

void StreamBuffer::Commit() {
	if (Usage() != MemoryUsage::Download && m_mapped_size != 0) {
		Flush(m_offset, m_mapped_size);
	}

	m_offset += m_mapped_size;
	const auto tick = Scheduler().CurrentTick();
	if (m_current_watch_cursor != 0 && m_current_watches[m_current_watch_cursor - 1].tick == tick) {
		m_current_watches[m_current_watch_cursor - 1].upper_bound = m_offset;
		return;
	}
	if (m_current_watch_cursor + 1 >= m_current_watches.size()) {
		m_current_watches.resize(m_current_watches.size() + WATCHES_RESERVE_CHUNK);
	}
	auto& watch       = m_current_watches[m_current_watch_cursor++];
	watch.upper_bound = m_offset;
	watch.tick        = tick;
}

uint64_t StreamBuffer::Copy(const void* source, uint64_t size, uint64_t alignment) {
	EXIT_IF(source == nullptr);
	const auto [data, offset] = Map(size, alignment);
	EXIT_IF(data == nullptr);
	std::memcpy(data, source, static_cast<size_t>(size));
	Commit();
	return offset;
}

bool StreamBuffer::WaitPendingOperations(const std::vector<Watch>& watches,
                                         std::optional<size_t>     invalidation_mark,
                                         uint64_t requested_upper_bound, bool allow_wait,
                                         size_t& wait_cursor, uint64_t& wait_bound) {
	if (!invalidation_mark.has_value()) {
		return true;
	}
	while (requested_upper_bound > wait_bound && wait_cursor < *invalidation_mark) {
		const auto& watch = watches[wait_cursor];
		if (!Scheduler().IsFree(watch.tick) && !allow_wait) {
			return false;
		}
		{
			Common::SubmitProfile::WaitSiteScope site(Common::SubmitProfile::WaitSite::StreamWrap);
			Scheduler().Wait(watch.tick);
		}
		if (Usage() == MemoryUsage::Download) {
			Scheduler().WaitPriorityOperations(watch.tick);
		}
		wait_bound = watch.upper_bound;
		++wait_cursor;
	}
	return true;
}

} // namespace Libs::Graphics
