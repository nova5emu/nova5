#ifndef NOVA5_GRAPHICS_VK_RECORDER_BACKEND_H
#define NOVA5_GRAPHICS_VK_RECORDER_BACKEND_H

#include "graphics/host_gpu/renderer/masterSemaphore.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/vkRecorderPackets.h"

namespace Libs::Graphics::VkRecording {

// Native backend connected to CommandScheduler's opt-in recording route.
// Gameplay uses the scheduler facade; its native handles never escape to the producer.
// Exactly one producer. Graphics and master must outlive this object. The caller
// reserves no ticks itself while a recorder command is open. Submit reserves a
// master tick on the producer; only the worker signals it, via the native queue.
// No command-buffer handle escapes: a drain does NOT transfer pool ownership.
class Backend final {
public:
	using Drain = Common::CommandRecording::Drain;
	struct Audit {
		std::thread::id owner;
		uint64_t buffers = 0, reuses = 0, submits = 0, last_tick = 0;
	};
	Backend(GraphicContext& graphics, MasterSemaphore& master);
	~Backend();
	Backend(const Backend&) = delete;
	Backend& operator=(const Backend&) = delete;
	void SetEnabled(bool enabled);
	bool Enabled() const { return m_transport.Enabled(); }
	void Begin();
	Encoder& Commands();
	uint64_t Submit(SubmitInfo info = {}, uint64_t frame = 0, uint64_t submission = 0, bool trace = false);
	void Publish() { m_transport.Publish(); }
	void DrainRecording(Drain reason) { m_transport.DrainUntilIdle(reason); }
	// Rare native island: the callable and its borrowed arguments stay on the
	// producer stack until this synchronous recording drain returns. The handle
	// is passed ONLY on the worker; no ownership transfer and no heap callable.
	void Call(void (*function)(void*, vk::CommandBuffer), void* data, Drain reason, int rendering = 0);
	// Unlike DrainRecording, these wait for native completion and release pins.
	void Wait(uint64_t tick);
	void Finish();
	void Stop();
	void Hold(std::shared_ptr<void> resource);
	// Feed only the master's observed native completion (e.g. after scheduler
	// PopPendingOperations); never a recorder queue ticket. Does not wait/refresh.
	void ReclaimCompleted() { m_pins.Complete(m_master.KnownGpuTick()); }
	size_t PendingPins() const { return m_pins.Pending(); }
	void Frame() { m_transport.Frame(); }
	auto Snapshot() const { return m_transport.Snapshot(); }
	void Report(std::FILE* output) const { m_transport.Report(output); }
	Audit Inspect(); // diagnostic/test only: drains before reading worker state
private:
	static constexpr uint16_t BeginOp = 0xff00, SubmitOp = 0xff01, ImmediateOp = 0xff02;
	struct Immediate { void (*function)(void*, vk::CommandBuffer); void* data; int rendering; };
	static constexpr size_t PayloadLimit = 65536;
	struct Submission { uint64_t tick; SubmitInfo info; uint64_t frame, submission; bool trace; };
	struct Slot { vk::CommandBuffer buffer; uint64_t tick = 0; };
	static void StartWorker(void*) noexcept;
	static void StopWorker(void*) noexcept;
	static void Consume(void*, uint16_t, std::span<const std::byte>) noexcept;
	void Execute(uint16_t, std::span<const std::byte>);
	void BeginNative(uint64_t tick);
	void SubmitNative(const Submission& submission);
	void BeginRenderingNative(const RenderState& state);
	void EndRenderingNative();
	void DescriptorsNative(Reader& reader, bool push);
	Writes ReadDescriptorWrites(Reader& reader);
	GraphicContext& m_graphics;
	MasterSemaphore& m_master;
	Common::CommandRecording::Queue m_transport;
	Encoder m_encoder;
	Common::CommandRecording::TickPins m_pins;
	bool m_open = false, m_stopped = false; // producer only
	uint64_t m_tick = 0, m_last_submitted = 0;
	// Everything below is worker-owned. Inspect() requires a recording drain.
	vk::CommandPool m_pool {};
	vk::CommandBuffer m_current {};
	std::vector<Slot> m_slots;
	size_t m_slot = 0;
	bool m_rendering = false;
	Audit m_audit;
	std::vector<vk::DescriptorSet> m_sets;
	std::vector<uint32_t> m_words;
	std::vector<vk::Buffer> m_buffers;
	std::vector<uint64_t> m_offsets;
	std::vector<uint64_t> m_sizes;
	std::vector<vk::MemoryBarrier2> m_memory_barriers;
	std::vector<vk::BufferMemoryBarrier2> m_buffer_barriers;
	std::vector<vk::ImageMemoryBarrier2> m_image_barriers;
	std::vector<vk::WriteDescriptorSet> m_writes;
	std::vector<vk::DescriptorBufferInfo> m_buffer_infos;
	std::vector<vk::DescriptorImageInfo> m_image_infos;
	std::vector<vk::BufferView> m_views;
};

} // namespace Libs::Graphics::VkRecording
#endif
