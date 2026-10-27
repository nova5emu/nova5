#include "graphics/host_gpu/renderer/vkRecorderBackend.h"

#include "common/assert.h"
#include "common/frameTimeline.h"
#include "graphics/host_gpu/graphicContext.h"

#include <bit>

namespace Libs::Graphics::VkRecording {
namespace {
template <typename T> T Read(Reader& reader) {
	T value {};
	EXIT_NOT_IMPLEMENTED(!reader.Read(value));
	return value;
}
template <typename T> T ReadValue(Reader& reader) {
	auto value = Read<T>(reader);
	EXIT_NOT_IMPLEMENTED(!reader.Empty());
	return value;
}
template <typename T> void ReadVector(Reader& reader, std::vector<T>& values, uint32_t count) {
	EXIT_NOT_IMPLEMENTED(count > 65536 / sizeof(T));
	values.resize(count);
	EXIT_NOT_IMPLEMENTED(!reader.ReadArray(std::span(values)));
}
template <typename T> const T* AppendArray(Reader& reader, std::vector<T>& values, uint32_t count) {
	const auto first = values.size();
	// StartWorker reserves the full bound: pointers in earlier writes stay valid.
	EXIT_NOT_IMPLEMENTED(count > values.capacity() - first);
	values.resize(first + count);
	EXIT_NOT_IMPLEMENTED(!reader.ReadArray(std::span(values).subspan(first)));
	return values.data() + first;
}
void Check(vk::Result result) { EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess); }
} // namespace

Backend::Backend(GraphicContext& graphics, MasterSemaphore& master)
    : m_graphics(graphics), m_master(master),
      m_transport(Consume, this, {}, StartWorker, StopWorker), m_encoder(m_transport) {}

Backend::~Backend() { Stop(); }

void Backend::SetEnabled(bool enabled) {
	EXIT_NOT_IMPLEMENTED(m_stopped);
	if (!enabled && m_open) Submit();
	EXIT_NOT_IMPLEMENTED(!m_transport.SetEnabled(enabled)); // drains submits before direct work
}

void Backend::Begin() {
	EXIT_NOT_IMPLEMENTED(!Enabled() || m_open || m_stopped);
	m_tick = m_master.CurrentTick();
	EXIT_NOT_IMPLEMENTED(!m_transport.AppendValue(BeginOp, m_tick));
	m_open = true;
}

Encoder& Backend::Commands() {
	EXIT_NOT_IMPLEMENTED(!m_open || !Enabled());
	return m_encoder;
}

uint64_t Backend::Submit(SubmitInfo info, uint64_t frame, uint64_t submission, bool trace) {
	EXIT_NOT_IMPLEMENTED(!m_open || !Enabled() || info.num_wait_semaphores > SubmitInfo::MaxSemaphores ||
	        info.num_signal_semaphores >= SubmitInfo::MaxSemaphores);
	EXIT_NOT_IMPLEMENTED(m_tick != m_master.CurrentTick());
	const auto tick = m_master.NextTick();
	info.AddSignal(m_master.Handle(), tick);
	EXIT_NOT_IMPLEMENTED(!m_transport.AppendValue(SubmitOp, Submission {tick, info, frame, submission, trace}));
	m_open = false;
	m_last_submitted = tick;
	m_transport.Publish(); // tick waiters must not depend on a later producer packet
	return tick;
}

void Backend::Hold(std::shared_ptr<void> resource) {
	EXIT_NOT_IMPLEMENTED(!m_open);
	m_pins.Hold(m_tick, std::move(resource));
}

void Backend::Call(void (*function)(void*, vk::CommandBuffer), void* data, Drain reason, int rendering) {
	EXIT_NOT_IMPLEMENTED(!m_open || !function);
	EXIT_NOT_IMPLEMENTED(!m_transport.AppendValue(ImmediateOp, Immediate {function, data, rendering}));
	DrainRecording(reason);
}

void Backend::Wait(uint64_t tick) {
	EXIT_NOT_IMPLEMENTED(tick > m_master.CurrentTick());
	if (tick == m_master.CurrentTick()) {
		EXIT_NOT_IMPLEMENTED(!m_open);
		Submit();
	}
	DrainRecording(Drain::WaitTick);
	m_master.Wait(tick);
	ReclaimCompleted();
}

void Backend::Finish() {
	if (m_open) Submit();
	DrainRecording(Drain::Finish);
	if (m_last_submitted) m_master.Wait(m_last_submitted);
	ReclaimCompleted();
}

void Backend::Stop() {
	if (m_stopped) return;
	if (m_open) Submit();
	// StopWorker waits for the last NATIVE tick before destroying the pool.
	m_transport.Stop();
	ReclaimCompleted();
	m_stopped = true;
}

Backend::Audit Backend::Inspect() {
	DrainRecording(Drain::Native);
	return m_audit;
}

void Backend::StartWorker(void* context) noexcept {
	auto& self = *static_cast<Backend*>(context);
	self.m_audit.owner = std::this_thread::get_id();
	vk::CommandPoolCreateInfo create {};
	create.queueFamilyIndex = self.m_graphics.queue_family;
	create.flags = vk::CommandPoolCreateFlagBits::eTransient | vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
	Check(self.m_graphics.device.createCommandPool(&create, nullptr, &self.m_pool));
	// Reusable aligned decode storage, allocated only when enabling the backend.
	self.m_sets.reserve(PayloadLimit / sizeof(vk::DescriptorSet));
	self.m_words.reserve(PayloadLimit / sizeof(uint32_t));
	self.m_buffers.reserve(PayloadLimit / sizeof(vk::Buffer));
	self.m_offsets.reserve(PayloadLimit / sizeof(uint64_t));
	self.m_sizes.reserve(PayloadLimit / sizeof(uint64_t));
	self.m_memory_barriers.reserve(PayloadLimit / sizeof(vk::MemoryBarrier2));
	self.m_buffer_barriers.reserve(PayloadLimit / sizeof(vk::BufferMemoryBarrier2));
	self.m_image_barriers.reserve(PayloadLimit / sizeof(vk::ImageMemoryBarrier2));
	self.m_writes.reserve(64);
	self.m_buffer_infos.reserve(PayloadLimit / sizeof(vk::DescriptorBufferInfo));
	self.m_image_infos.reserve(PayloadLimit / sizeof(vk::DescriptorImageInfo));
	self.m_views.reserve(PayloadLimit / sizeof(vk::BufferView));
}

void Backend::StopWorker(void* context) noexcept {
	auto& self = *static_cast<Backend*>(context);
	EXIT_NOT_IMPLEMENTED(self.m_audit.owner != std::this_thread::get_id() || self.m_current);
	if (self.m_audit.last_tick) self.m_master.Wait(self.m_audit.last_tick);
	self.m_graphics.device.destroyCommandPool(self.m_pool, nullptr);
	self.m_pool = nullptr;
}

void Backend::Consume(void* context, uint16_t opcode, std::span<const std::byte> payload) noexcept {
	auto& self = *static_cast<Backend*>(context);
	EXIT_NOT_IMPLEMENTED(self.m_audit.owner != std::this_thread::get_id());
	self.Execute(opcode, payload);
}

void Backend::BeginNative(uint64_t tick) {
	EXIT_NOT_IMPLEMENTED(m_current || tick <= m_audit.last_tick);
	m_master.Refresh();
	const auto completed = m_master.KnownGpuTick();
	m_slot = 0;
	while (m_slot < m_slots.size() && m_slots[m_slot].tick > completed) ++m_slot;
	if (m_slot == m_slots.size()) {
		std::array<vk::CommandBuffer, 4> added {};
		vk::CommandBufferAllocateInfo allocate {};
		allocate.commandPool = m_pool;
		allocate.level = vk::CommandBufferLevel::ePrimary;
		allocate.commandBufferCount = static_cast<uint32_t>(added.size());
		Check(m_graphics.device.allocateCommandBuffers(&allocate, added.data()));
		for (auto buffer: added) m_slots.push_back({buffer, 0});
		m_audit.buffers = m_slots.size();
	}
	auto& slot = m_slots[m_slot];
	if (slot.tick) {
		Check(slot.buffer.reset({}));
		++m_audit.reuses;
	}
	slot.tick = tick;
	m_current = slot.buffer;
	vk::CommandBufferBeginInfo begin {};
	begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
	Check(m_current.begin(&begin));
}

void Backend::SubmitNative(const Submission& submission) {
	EXIT_NOT_IMPLEMENTED(!m_current || submission.tick != m_slots[m_slot].tick);
	EndRenderingNative();
	Check(m_current.end());
	const auto& info = submission.info;
	EXIT_NOT_IMPLEMENTED(info.num_wait_semaphores > SubmitInfo::MaxSemaphores ||
	        info.num_signal_semaphores > SubmitInfo::MaxSemaphores);
	vk::TimelineSemaphoreSubmitInfo timeline {};
	timeline.waitSemaphoreValueCount = info.num_wait_semaphores;
	timeline.pWaitSemaphoreValues = info.wait_ticks.data();
	timeline.signalSemaphoreValueCount = info.num_signal_semaphores;
	timeline.pSignalSemaphoreValues = info.signal_ticks.data();
	vk::SubmitInfo submit {};
	submit.pNext = &timeline;
	submit.waitSemaphoreCount = info.num_wait_semaphores;
	submit.pWaitSemaphores = info.wait_semaphores.data();
	submit.pWaitDstStageMask = info.wait_stages.data();
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &m_current;
	submit.signalSemaphoreCount = info.num_signal_semaphores;
	submit.pSignalSemaphores = info.signal_semaphores.data();
	{
		Common::LockGuard lock(m_graphics.queue_mutex);
		if (submission.trace) Common::FrameTimeline::Event(Common::FrameTimeline::Kind::NativeSubmit,
		    submission.frame, submission.submission, submission.tick);
		Check(m_graphics.queue.submit(1, &submit, nullptr));
	}
	m_current = nullptr;
	++m_audit.submits;
	m_audit.last_tick = submission.tick;
}

void Backend::BeginRenderingNative(const RenderState& state) {
	EXIT_NOT_IMPLEMENTED(m_rendering || !state.width || !state.height || !state.num_layers ||
	        state.num_color_attachments > RENDER_COLOR_ATTACHMENTS_MAX);
	// Same attachment translation as CommandBuffer::BeginRendering. Pass reuse,
	// guest ownership and clear consumption remain the producer's responsibility.
	std::array<vk::RenderingAttachmentInfo, RENDER_COLOR_ATTACHMENTS_MAX> colors {};
	for (uint32_t i = 0; i < state.num_color_attachments; ++i) {
		const auto& a = state.color_attachments[i];
		colors[i].imageView = a.image_view;
		colors[i].imageLayout = a.image_layout;
		colors[i].loadOp = a.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
		colors[i].storeOp = vk::AttachmentStoreOp::eStore;
		colors[i].clearValue.color.uint32 = a.clear_value;
	}
	const auto& a = state.depth_stencil_attachment;
	vk::RenderingAttachmentInfo depth {}, stencil {};
	depth.imageView = stencil.imageView = a.image_view;
	depth.imageLayout = stencil.imageLayout = a.image_layout;
	depth.storeOp = stencil.storeOp = vk::AttachmentStoreOp::eStore;
	depth.loadOp = a.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
	stencil.loadOp = a.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
	depth.clearValue.depthStencil.depth = std::bit_cast<float>(a.clear_value[0]);
	stencil.clearValue.depthStencil.stencil = a.clear_value[1];
	vk::RenderingInfo info {};
	info.renderArea.extent = {state.width, state.height};
	info.layerCount = state.num_layers;
	info.colorAttachmentCount = state.num_color_attachments;
	info.pColorAttachments = colors.data();
	info.pDepthAttachment = a.has_depth ? &depth : nullptr;
	info.pStencilAttachment = a.has_stencil ? &stencil : nullptr;
	m_current.beginRendering(&info);
	m_rendering = true;
}

void Backend::EndRenderingNative() {
	if (m_rendering) m_current.endRendering();
	m_rendering = false;
}

Writes Backend::ReadDescriptorWrites(Reader& reader) {
	const auto header = Read<Writes>(reader);
	EXIT_NOT_IMPLEMENTED(header.count > 64);
	m_writes.resize(header.count);
	m_buffer_infos.clear(); m_image_infos.clear(); m_views.clear();
	for (auto& write: m_writes) {
		const auto item = Read<Write>(reader);
		EXIT_NOT_IMPLEMENTED(!item.count);
		write = vk::WriteDescriptorSet {};
		write.dstSet = item.set;
		write.dstBinding = item.binding;
		write.dstArrayElement = item.element;
		write.descriptorCount = item.count;
		write.descriptorType = item.type;
		switch (item.type) {
		case vk::DescriptorType::eUniformBuffer: case vk::DescriptorType::eStorageBuffer:
		case vk::DescriptorType::eUniformBufferDynamic: case vk::DescriptorType::eStorageBufferDynamic:
			write.pBufferInfo = AppendArray(reader, m_buffer_infos, item.count); break;
		case vk::DescriptorType::eSampler: case vk::DescriptorType::eCombinedImageSampler:
		case vk::DescriptorType::eSampledImage: case vk::DescriptorType::eStorageImage: case vk::DescriptorType::eInputAttachment:
			write.pImageInfo = AppendArray(reader, m_image_infos, item.count); break;
		case vk::DescriptorType::eUniformTexelBuffer: case vk::DescriptorType::eStorageTexelBuffer:
			write.pTexelBufferView = AppendArray(reader, m_views, item.count); break;
		default: EXIT_NOT_IMPLEMENTED(true);
		}
	}
	EXIT_NOT_IMPLEMENTED(!reader.Empty());
	return header;
}

void Backend::DescriptorsNative(Reader& reader, bool push) {
	const auto header = ReadDescriptorWrites(reader);
	if (push) {
		EXIT_NOT_IMPLEMENTED(!VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPushDescriptorSetKHR);
		m_current.pushDescriptorSetKHR(header.point, header.layout, header.set, header.count, m_writes.data());
	} else {
		// Must target fresh/tick-safe sets, just like DescriptorHeap today. Merely
		// ordering a host update after a draw does NOT snapshot the old descriptor.
		m_graphics.device.updateDescriptorSets(header.count, m_writes.data(), 0, nullptr);
	}
}

void Backend::Execute(uint16_t opcode, std::span<const std::byte> payload) {
	Reader reader(payload);
	if (opcode == BeginOp) { BeginNative(ReadValue<uint64_t>(reader)); return; }
	if (opcode == SubmitOp) { SubmitNative(ReadValue<Submission>(reader)); return; }
	EXIT_NOT_IMPLEMENTED(!m_current);
	if (opcode == ImmediateOp) {
		const auto p = ReadValue<Immediate>(reader);
		EXIT_NOT_IMPLEMENTED(!p.function || (p.rendering == 1 && m_rendering));
		p.function(p.data, m_current);
		if (p.rendering) m_rendering = p.rendering > 0;
		return;
	}
	switch (static_cast<Op>(opcode)) {
	case Op::ConditionalBegin: {
		const auto p = ReadValue<Conditional>(reader);
		vk::ConditionalRenderingBeginInfoEXT info {}; info.buffer = p.buffer; info.offset = p.offset; info.flags = p.flags;
		m_current.beginConditionalRenderingEXT(&info); break;
	}
	case Op::ConditionalEnd: EXIT_NOT_IMPLEMENTED(!reader.Empty()); m_current.endConditionalRenderingEXT(); break;
	case Op::ClearAttachments: {
		const auto p = Read<Attachments>(reader);
		std::array<vk::ClearAttachment, 16> attachments;
		std::array<vk::ClearRect, 256> rectangles;
		EXIT_NOT_IMPLEMENTED(p.count > attachments.size() || p.rectangles > rectangles.size() ||
		    !reader.ReadArray(std::span(attachments).first(p.count)) ||
		    !reader.ReadArray(std::span(rectangles).first(p.rectangles)) || !reader.Empty());
		m_current.clearAttachments(p.count, attachments.data(), p.rectangles, rectangles.data()); break;
	}
	case Op::CopyBuffer: {
		const auto p = Read<Transfer>(reader);
		std::array<vk::BufferCopy, 512> data;
		EXIT_NOT_IMPLEMENTED(p.count > data.size() || !reader.ReadArray(std::span(data).first(p.count)) || !reader.Empty());
		m_current.copyBuffer(p.source_buffer, p.destination_buffer, p.count, data.data()); break;
	}
	case Op::BufferToImage: {
		const auto p = Read<Transfer>(reader);
		std::array<vk::BufferImageCopy, 512> data;
		EXIT_NOT_IMPLEMENTED(p.count > data.size() || !reader.ReadArray(std::span(data).first(p.count)) || !reader.Empty());
		m_current.copyBufferToImage(p.source_buffer, p.destination_image, p.destination_layout, p.count, data.data()); break;
	}
	case Op::ImageToBuffer: {
		const auto p = Read<Transfer>(reader);
		std::array<vk::BufferImageCopy, 512> data;
		EXIT_NOT_IMPLEMENTED(p.count > data.size() || !reader.ReadArray(std::span(data).first(p.count)) || !reader.Empty());
		m_current.copyImageToBuffer(p.source_image, p.source_layout, p.destination_buffer, p.count, data.data()); break;
	}
	case Op::CopyImage: {
		const auto p = Read<Transfer>(reader);
		std::array<vk::ImageCopy, 512> data;
		EXIT_NOT_IMPLEMENTED(p.count > data.size() || !reader.ReadArray(std::span(data).first(p.count)) || !reader.Empty());
		m_current.copyImage(p.source_image, p.source_layout, p.destination_image, p.destination_layout, p.count, data.data()); break;
	}
	case Op::ResolveImage: {
		const auto p = Read<Transfer>(reader);
		std::array<vk::ImageResolve, 512> data;
		EXIT_NOT_IMPLEMENTED(p.count > data.size() || !reader.ReadArray(std::span(data).first(p.count)) || !reader.Empty());
		m_current.resolveImage(p.source_image, p.source_layout, p.destination_image, p.destination_layout, p.count, data.data()); break;
	}
	case Op::BlitImage: {
		const auto p = Read<Transfer>(reader);
		std::array<vk::ImageBlit, 512> data;
		EXIT_NOT_IMPLEMENTED(p.count > data.size() || !reader.ReadArray(std::span(data).first(p.count)) || !reader.Empty());
		m_current.blitImage(p.source_image, p.source_layout, p.destination_image, p.destination_layout, p.count, data.data(), p.filter); break;
	}
	case Op::ClearColor: {
		const auto p = Read<Transfer>(reader);
		std::array<vk::ImageSubresourceRange, 512> data;
		EXIT_NOT_IMPLEMENTED(p.count > data.size() || !reader.ReadArray(std::span(data).first(p.count)) || !reader.Empty());
		m_current.clearColorImage(p.destination_image, p.destination_layout, &p.clear.color, p.count, data.data()); break;
	}
	case Op::ClearDepth: {
		const auto p = Read<Transfer>(reader);
		std::array<vk::ImageSubresourceRange, 512> data;
		EXIT_NOT_IMPLEMENTED(p.count > data.size() || !reader.ReadArray(std::span(data).first(p.count)) || !reader.Empty());
		m_current.clearDepthStencilImage(p.destination_image, p.destination_layout, &p.clear.depthStencil, p.count, data.data()); break;
	}
	case Op::Viewport: {
		const auto p = Read<Transfer>(reader);
		std::array<vk::Viewport, 512> data;
		EXIT_NOT_IMPLEMENTED(p.count > data.size() || !reader.ReadArray(std::span(data).first(p.count)) || !reader.Empty());
		m_current.setViewport(p.first, p.count, data.data()); break;
	}
	case Op::Scissor: {
		const auto p = Read<Transfer>(reader);
		std::array<vk::Rect2D, 512> data;
		EXIT_NOT_IMPLEMENTED(p.count > data.size() || !reader.ReadArray(std::span(data).first(p.count)) || !reader.Empty());
		m_current.setScissor(p.first, p.count, data.data()); break;
	}
	case Op::LegacyBarrier: {
		const auto p = Read<LegacyBarrier>(reader);
		std::array<vk::MemoryBarrier, 256> memory;
		std::array<vk::BufferMemoryBarrier, 256> buffers;
		std::array<vk::ImageMemoryBarrier, 256> images;
		EXIT_NOT_IMPLEMENTED(p.counts.memory > memory.size() || p.counts.buffers > buffers.size() || p.counts.images > images.size());
		EXIT_NOT_IMPLEMENTED(!reader.ReadArray(std::span(memory).first(p.counts.memory)) ||
		    !reader.ReadArray(std::span(buffers).first(p.counts.buffers)) ||
		    !reader.ReadArray(std::span(images).first(p.counts.images)) || !reader.Empty());
		m_current.pipelineBarrier(p.source, p.destination, p.counts.flags, p.counts.memory, memory.data(),
		    p.counts.buffers, buffers.data(), p.counts.images, images.data()); break;
	}
	case Op::NativeRendering: {
		auto p = Read<Rendering>(reader);
		std::array<vk::RenderingAttachmentInfo, 8> colors;
		vk::RenderingAttachmentInfo depth {}, stencil {};
		EXIT_NOT_IMPLEMENTED(m_rendering || p.info.colorAttachmentCount > colors.size());
		EXIT_NOT_IMPLEMENTED(!reader.ReadArray(std::span(colors).first(p.info.colorAttachmentCount)) ||
		    (p.depth && !reader.Read(depth)) || (p.stencil && !reader.Read(stencil)) || !reader.Empty());
		p.info.pColorAttachments = colors.data(); p.info.pDepthAttachment = p.depth ? &depth : nullptr;
		p.info.pStencilAttachment = p.stencil ? &stencil : nullptr;
		m_current.beginRendering(&p.info); m_rendering = true; break;
	}
	case Op::FillBuffer: {
		const auto p = ReadValue<Transfer>(reader);
		m_current.fillBuffer(p.destination_buffer, p.offset, p.size, p.value); break;
	}
	case Op::ResetQueries: {
		const auto p = ReadValue<QueryReset>(reader);
		EXIT_NOT_IMPLEMENTED(m_rendering || !p.pool || !p.count);
		m_current.resetQueryPool(p.pool, p.first, p.count);
		break;
	}
	case Op::Timestamp: {
		const auto p = ReadValue<Timestamp>(reader);
		m_current.writeTimestamp(p.stage, p.pool, p.query);
		break;
	}
	case Op::Bindings: {
		const auto state = Read<BindingState>(reader);
		EXIT_NOT_IMPLEMENTED(state.constant_bytes % 4 || state.constant_offset % 4);
		ReadVector(reader, m_words, state.constant_bytes / 4);
		const auto descriptors = ReadDescriptorWrites(reader);
		EXIT_NOT_IMPLEMENTED(descriptors.layout != state.layout || descriptors.point != state.point || descriptors.set != 0);
		const BindingEmission emission {state, std::as_bytes(std::span(m_words)), m_writes};
		emission.Emit(m_graphics.device, m_current);
		break;
	}
	case Op::BindPipeline: {
		const auto p = ReadValue<Pipeline>(reader);
		m_current.bindPipeline(p.point, p.pipeline); break;
	}
	case Op::BindDescriptors: {
		const auto p = Read<DescriptorSets>(reader);
		ReadVector(reader, m_sets, p.sets); ReadVector(reader, m_words, p.offsets);
		EXIT_NOT_IMPLEMENTED(!reader.Empty());
		m_current.bindDescriptorSets(p.point, p.layout, p.first, p.sets, m_sets.data(), p.offsets, m_words.data()); break;
	}
	case Op::PushConstants: {
		const auto p = Read<Constants>(reader);
		EXIT_NOT_IMPLEMENTED(p.bytes % 4 || p.offset % 4);
		ReadVector(reader, m_words, p.bytes / 4);
		EXIT_NOT_IMPLEMENTED(!reader.Empty());
		m_current.pushConstants(p.layout, p.stages, p.offset, p.bytes, m_words.data()); break;
	}
	case Op::PushDescriptors: DescriptorsNative(reader, true); break;
	case Op::UpdateDescriptors: DescriptorsNative(reader, false); break;
	case Op::BeginRendering: BeginRenderingNative(ReadValue<RenderState>(reader)); break;
	case Op::EndRendering: EXIT_NOT_IMPLEMENTED(!reader.Empty()); EndRenderingNative(); break;
	case Op::DynamicState: {
		auto p = ReadValue<DynamicState>(reader);
		EXIT_NOT_IMPLEMENTED(p.num_viewports > p.viewports.size() || p.num_scissors > p.scissors.size() ||
		        p.num_color_write_enables > p.color_write_enables.size());
		p.Commit(m_graphics, m_current); break;
	}
	case Op::Barrier2: {
		const auto p = Read<Barrier>(reader);
		ReadVector(reader, m_memory_barriers, p.memory);
		ReadVector(reader, m_buffer_barriers, p.buffers);
		ReadVector(reader, m_image_barriers, p.images);
		EXIT_NOT_IMPLEMENTED(!reader.Empty());
		for (const auto& b: m_memory_barriers) EXIT_NOT_IMPLEMENTED(b.pNext);
		for (const auto& b: m_buffer_barriers) EXIT_NOT_IMPLEMENTED(b.pNext);
		for (const auto& b: m_image_barriers) EXIT_NOT_IMPLEMENTED(b.pNext);
		vk::DependencyInfo info {};
		info.dependencyFlags = p.flags;
		info.memoryBarrierCount = p.memory; info.pMemoryBarriers = m_memory_barriers.data();
		info.bufferMemoryBarrierCount = p.buffers; info.pBufferMemoryBarriers = m_buffer_barriers.data();
		info.imageMemoryBarrierCount = p.images; info.pImageMemoryBarriers = m_image_barriers.data();
		m_current.pipelineBarrier2(&info); break;
	}
	case Op::Draw: {
		const auto p = ReadValue<Draw>(reader);
		m_current.draw(p.count, p.instances, p.first, p.first_instance); break;
	}
	case Op::DrawIndexed: {
		const auto p = ReadValue<Draw>(reader);
		m_current.drawIndexed(p.count, p.instances, p.first, p.vertex_offset, p.first_instance); break;
	}
	case Op::DrawIndirect: case Op::DrawIndexedIndirect:
	case Op::DrawIndirectCount: case Op::DrawIndexedIndirectCount:
	case Op::DrawMeshIndirect: case Op::DispatchIndirect: {
		const auto p = ReadValue<Indirect>(reader);
		switch (static_cast<Op>(opcode)) {
		case Op::DrawIndirect: m_current.drawIndirect(p.buffer, p.offset, p.count, p.stride); break;
		case Op::DrawIndexedIndirect: m_current.drawIndexedIndirect(p.buffer, p.offset, p.count, p.stride); break;
		case Op::DrawIndirectCount:
			EXIT_NOT_IMPLEMENTED(!m_graphics.draw_indirect_count_enabled);
			m_current.drawIndirectCount(p.buffer, p.offset, p.count_buffer, p.count_offset, p.count, p.stride); break;
		case Op::DrawIndexedIndirectCount:
			EXIT_NOT_IMPLEMENTED(!m_graphics.draw_indirect_count_enabled);
			m_current.drawIndexedIndirectCount(p.buffer, p.offset, p.count_buffer, p.count_offset, p.count, p.stride); break;
		case Op::DrawMeshIndirect:
			EXIT_NOT_IMPLEMENTED(!m_graphics.mesh_shader_enabled);
			m_current.drawMeshTasksIndirectEXT(p.buffer, p.offset, p.count, p.stride); break;
		case Op::DispatchIndirect: m_current.dispatchIndirect(p.buffer, p.offset); break;
		default: EXIT_NOT_IMPLEMENTED(true);
		}
		break;
	}
	case Op::Dispatch: case Op::DrawMesh: {
		const auto p = ReadValue<Dispatch>(reader);
		if (static_cast<Op>(opcode) == Op::Dispatch) m_current.dispatch(p.x, p.y, p.z);
		else { EXIT_NOT_IMPLEMENTED(!m_graphics.mesh_shader_enabled); m_current.drawMeshTasksEXT(p.x, p.y, p.z); }
		break;
	}
	case Op::BindIndex: {
		const auto p = ReadValue<Index>(reader);
		if (p.bounded) {
			EXIT_NOT_IMPLEMENTED(!m_graphics.bounded_index_buffer_enabled);
			m_current.bindIndexBuffer2KHR(p.buffer, p.offset, p.size, p.type);
		} else m_current.bindIndexBuffer(p.buffer, p.offset, p.type);
		break;
	}
	case Op::BindVertices: {
		const auto p = Read<Vertices>(reader);
		ReadVector(reader, m_buffers, p.count); ReadVector(reader, m_offsets, p.count);
		EXIT_NOT_IMPLEMENTED(!reader.Empty());
		m_current.bindVertexBuffers(p.first, p.count, m_buffers.data(), m_offsets.data()); break;
	}
	case Op::BindVerticesSized: {
		const auto p = Read<Vertices>(reader);
		ReadVector(reader, m_buffers, p.count); ReadVector(reader, m_offsets, p.count);
		ReadVector(reader, m_sizes, p.count);
		EXIT_NOT_IMPLEMENTED(!reader.Empty());
		m_current.bindVertexBuffers2(p.first, p.count, m_buffers.data(), m_offsets.data(), m_sizes.data(), nullptr);
		break;
	}
	case Op::DispatchBase: {
		const auto p = ReadValue<DispatchOffset>(reader);
		m_current.dispatchBase(p.base.x, p.base.y, p.base.z, p.groups.x, p.groups.y, p.groups.z); break;
	}
	case Op::BeginQuery: case Op::EndQuery: {
		const auto p = ReadValue<Query>(reader);
		if (static_cast<Op>(opcode) == Op::BeginQuery) m_current.beginQuery(p.pool, p.query, p.flags);
		else m_current.endQuery(p.pool, p.query);
		break;
	}
	default: EXIT_NOT_IMPLEMENTED(true); // internal protocol violation: never silently drop a command
	}
}

} // namespace Libs::Graphics::VkRecording
