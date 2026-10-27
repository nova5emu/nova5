#include "common/gpuThreadSampler.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"

#include "common/assert.h"
#include "common/debugCounters.h"
#include "common/frameTimeline.h"
#include "common/logging/log.h"
#include "common/renderStageProfile.h"
#include "common/submitProfile.h"
#include "graphics/guest_gpu/submitProfileRuntime.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/vkRecorderBackend.h"

#include <algorithm>
#include <cstdio>
#include <optional>

namespace Libs::Graphics {

static thread_local CommandScheduler* g_deferred_callback_scheduler = nullptr;

namespace {

void ReportVulkanFatal(const char* what, vk::Result result, uint64_t tick, uint32_t debug_op,
                       uint64_t debug_submit, uint32_t arg0, uint32_t arg1, uint32_t arg2,
                       uint32_t arg3, uint64_t arg4) {
	LOGF("%s failed: %s (%d), tick=%" PRIu64 " debug_op=%u debug_submit=%" PRIu64
	     " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
	     what, vk::to_string(result).c_str(), static_cast<int>(result), tick, debug_op,
	     debug_submit, arg0, arg1, arg2, arg3, arg4);
	std::printf("%s failed: %s (%d), tick=%" PRIu64 " debug_op=%u debug_submit=%" PRIu64
	            " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
	            what, vk::to_string(result).c_str(), static_cast<int>(result), tick, debug_op,
	            debug_submit, arg0, arg1, arg2, arg3, arg4);
	std::fflush(stdout);
}

} // namespace

CommandScheduler::CommandPool::CommandPool(GraphicContext& graphics, MasterSemaphore& master)
    : m_graphics(graphics), m_master(master) {
	EXIT_IF(graphics.queue_family == static_cast<uint32_t>(-1));
	vk::CommandPoolCreateInfo create {};
	create.queueFamilyIndex = graphics.queue_family;
	create.flags            = vk::CommandPoolCreateFlagBits::eTransient |
	                          vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
	const auto result       = graphics.device.createCommandPool(&create, nullptr, &m_pool);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_pool == nullptr);
}

CommandScheduler::CommandPool::~CommandPool() {
	m_graphics.device.destroyCommandPool(m_pool, nullptr);
}

size_t CommandScheduler::CommandPool::Grow() {
	const auto first = m_ticks.size();
	m_ticks.resize(first + GrowStep);
	m_buffers.resize(first + GrowStep);

	vk::CommandBufferAllocateInfo allocate {};
	allocate.commandPool        = m_pool;
	allocate.level              = vk::CommandBufferLevel::ePrimary;
	allocate.commandBufferCount = static_cast<uint32_t>(GrowStep);
	EXIT_IF(m_graphics.device.allocateCommandBuffers(&allocate, m_buffers.data() + first) !=
	        vk::Result::eSuccess);
	return first;
}

vk::CommandBuffer CommandScheduler::CommandPool::Commit() {
	auto       gpu_tick = m_master.KnownGpuTick();
	const auto search   = [this, &gpu_tick](size_t begin, size_t end) -> std::optional<size_t> {
		for (size_t index = begin; index < end; ++index) {
			if (gpu_tick >= m_ticks[index]) {
				m_ticks[index] = m_master.CurrentTick();
				return index;
			}
		}
		return std::nullopt;
	};

	auto found = search(m_hint, m_ticks.size());
	if (!found) {
		m_master.Refresh();
		gpu_tick = m_master.KnownGpuTick();
		found    = search(m_hint, m_ticks.size());
	}
	if (!found) {
		found = search(0, m_hint);
	}
	if (!found) {
		found           = Grow();
		m_ticks[*found] = m_master.CurrentTick();
	}

	m_hint = (*found + 1) % m_ticks.size();
	return m_buffers[*found];
}

bool CommandScheduler::InDeferredOperation() noexcept {
	return g_deferred_callback_scheduler != nullptr;
}

CommandScheduler::CommandScheduler(RenderContext& context, GraphicContext& graphics)
    : m_master(graphics), m_context(context), m_graphics(graphics),
      m_command_pool(graphics, m_master), m_command(*this),
      m_priority_thread([this](std::stop_token stop) { PriorityOperationsThread(stop); }) {
	if (Common::SubmitProfile::ExperimentSelected(Common::SubmitProfile::Experiment::VkRecorder)) {
		static const bool announced = [] {
			std::fprintf(stderr, "[VK_RECORDER] phase=1 gameplay_supported=1 gate=command_boundary counters=cumulative\n");
			return true;
		}();
		(void)announced;
	}
}

CommandScheduler::~CommandScheduler() {
	Shutdown();
}

void CommandScheduler::DrainRecorder(Common::CommandRecording::Drain reason) {
	if (!m_recorder) return;
	if (reason == Common::CommandRecording::Drain::WaitTick) {
		using Site = Common::SubmitProfile::WaitSite;
		switch (Common::SubmitProfile::CurrentWaitSite()) {
		case Site::BufferReadback: case Site::ReadbackIndirectArgs: case Site::ReadbackColorMetadata:
		case Site::ReadbackCpuReadFault: case Site::ReadbackCpuWriteInvalidate: case Site::FaultDownload:
			reason = Common::CommandRecording::Drain::Readback; break;
		case Site::StreamWrap: reason = Common::CommandRecording::Drain::Upload; break;
		default: break;
		}
	}
	m_recorder->DrainRecording(reason);
}

void CommandScheduler::SetRecorderEnabled(bool enabled) {
	EXIT_NOT_IMPLEMENTED(InDeferredOperation());
	if (enabled == (m_recorder && m_recorder->Enabled())) return;
	const bool open = !m_command.IsInvalid();
	if (open) Submit();
	if (enabled && !m_recorder) {
		// Construct on the actual command producer, not the renderer's startup
		// thread: the transport binds its single producer at construction.
		m_recorder = std::make_unique<VkRecording::Backend>(m_graphics, m_master);
	}
	// B->A waits for recording/queue-submit, NOT GPU completion. Queue order and
	// the shared timeline protect native resources across both pools.
	m_recorder->SetEnabled(enabled);
	std::fprintf(stderr, "[VK_RECORDER_GATE] active=%u tick=%llu\n", enabled,
	    static_cast<unsigned long long>(CurrentTick()));
	if (!enabled) m_recorder->Report(stderr);
	if (open) BeginCommand();
}

void CommandScheduler::RecorderFrame() {
	if (!m_recorder || !m_recorder->Enabled()) return;
	m_recorder->Frame();
	if (++m_recorder_frames % 60 == 0) m_recorder->Report(stderr);
}

void CommandScheduler::StopRecorder() {
	m_recorder_shutdown = true;
	if (!m_recorder) return;
	SetRecorderEnabled(false);
	m_recorder->Stop();
	m_recorder->Report(stderr);
	m_recorder.reset(); // also destroys the transport on its producer thread
}

void CommandScheduler::Shutdown() {
	{
		std::unique_lock lock(m_operation_mutex);
		if (m_operation_state == OperationState::Closed) {
			return;
		}
		if (g_deferred_callback_scheduler == this) {
			EXIT_IF(m_operation_state == OperationState::Open);
			// A priority callback cannot join its own runner, while a normal callback can be
			// executing inside the shutdown owner's final PopPendingOperations. The owning
			// thread will finish shutdown after this callback returns.
			return;
		}
		if (m_operation_state == OperationState::Draining) {
			m_operation_available.wait(
			    lock, [this] { return m_operation_state == OperationState::Closed; });
			return;
		}
		m_operation_state = OperationState::Draining;
	}
	Common::SubmitProfile::ReasonScope   reason(Common::SubmitProfile::Reason::Shutdown);
	Common::SubmitProfile::WaitSiteScope site(Common::SubmitProfile::WaitSite::Shutdown);
	if (!m_command.IsInvalid()) {
		Submit();
	}
	DrainRecorder(Common::CommandRecording::Drain::Shutdown);
	m_master.Wait(CurrentTick() - 1);
	PopPendingOperations();
	DrainPriorityOperations();
	m_priority_thread.request_stop();
	m_operation_available.notify_all();
	if (m_priority_thread.joinable()) {
		m_priority_thread.join();
	}
	if (m_recorder) m_recorder->Stop();
	{
		std::lock_guard lock(m_operation_mutex);
		EXIT_IF(!m_pending_operations.empty() || !m_priority_operations.empty() ||
		        m_priority_active);
		m_operation_state = OperationState::Closed;
	}
	m_operation_available.notify_all();
}

void CommandScheduler::Begin(HW::Context& registers, HW::UserConfig& user_config,
                             HW::Shader& shaders) {
	{
		std::lock_guard lock(m_operation_mutex);
		EXIT_IF(m_operation_state != OperationState::Open);
	}
	m_command.Bind(registers, user_config, shaders);

	if (m_command.IsInvalid()) {
		BeginNext();
	}
}

void CommandScheduler::BeginRendering(const RenderState& state) {
	Current().BeginRendering(state);
}

void CommandScheduler::EndRendering() {
	if (Active() && !m_command.IsInvalid()) {
		Current().EndRendering();
	}
}

void CommandScheduler::Flush() {
	SubmitInfo submit;
	Flush(submit);
}

void CommandScheduler::Flush(SubmitInfo& submit) {
	Common::GpuThreadSampler::ImmediateScope boundary_immediate {Common::GpuThreadSampler::Boundary::Flush};
	Submit(submit);
	// Backend::Submit publishes the packet immediately. Keep preparing the next
	// command while the owner records/submits this one. Consumers of its results
	// wait on the native tick; switching to the direct pool drains in SetEnabled.
	BeginNext();
}

void CommandScheduler::FlushAndWait() {
	Common::GpuThreadSampler::ImmediateScope boundary_immediate {Common::GpuThreadSampler::Boundary::Flush};
	const auto tick = Submit();
	DrainRecorder(Common::CommandRecording::Drain::Finish);
	m_master.Wait(tick);
	BeginNext();
}

void CommandScheduler::Finish() {
	Common::GpuThreadSampler::ImmediateScope boundary_immediate {Common::GpuThreadSampler::Boundary::Flush};
	CheckActive();
	Common::SubmitProfile::ReasonScope   reason(Common::SubmitProfile::Reason::Finish);
	Common::SubmitProfile::WaitSiteScope site(Common::SubmitProfile::WaitSite::Finish);
	if (!m_command.IsInvalid()) {
		Submit();
	}
	DrainRecorder(Common::CommandRecording::Drain::Finish);
	m_master.Wait(CurrentTick() - 1);
	BeginNext();
	PopPendingOperations();
}

void CommandScheduler::Wait(uint64_t tick) {
	Common::GpuThreadSampler::ImmediateScope boundary_immediate {Common::GpuThreadSampler::Boundary::Wait};
	EXIT_IF(tick > CurrentTick());
	if (tick == CurrentTick()) {
		CheckActive();
		Common::SubmitProfile::ReasonScope reason(Common::SubmitProfile::Reason::WaitTick);
		// A stream-buffer wrap can wait while a draw is being prepared through a reference to
		// Current(). The wrapper stays stable while its pooled Vulkan buffer is retired. Deferred
		// resources are released only at the next GPU operation boundary.
		const auto submitted_tick = Submit();
		EXIT_IF(submitted_tick != tick);
		// Submit already published the work. Waiting for the native tick also
		// guarantees it was submitted, without a separate recording round trip.
		m_master.Wait(tick);
		BeginNext();
	} else {
		// An older tick was already published by Submit. In particular, stream
		// reuse must not publish/drain the CURRENT command merely to check an old
		// (often already completed) allocation. Only wait for the required tick.
		m_master.Wait(tick);
	}
}

void CommandScheduler::PopPendingOperations() {
	m_master.Refresh();
	for (;;) {
		PendingOperation operation;
		{
			std::lock_guard lock(m_operation_mutex);
			if (m_pending_operations.empty() ||
			    !m_master.IsFree(m_pending_operations.front().tick)) {
				return;
			}
			operation = std::move(m_pending_operations.front());
			m_pending_operations.pop();
		}
		WaitPriorityOperations(operation.tick);
		Common::FrameTimeline::ContextScope timeline(operation.timeline_frame, operation.timeline_submission);
		RunOperation(std::move(operation.callback));
	}
}

uint64_t CommandScheduler::DeferOperation(Common::UniqueFunction<void>&& operation) {
	return QueueOperation(std::move(operation), false);
}

void CommandScheduler::DeferPriorityOperation(Common::UniqueFunction<void>&& operation) {
	QueueOperation(std::move(operation), true);
}

uint64_t CommandScheduler::QueueOperation(Common::UniqueFunction<void>&& operation, bool priority) {
	CheckActive();
	EXIT_IF(!operation);
	std::unique_lock lock(m_operation_mutex);
	const auto sequence = priority ? 0 : m_operation_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
	if (m_operation_state == OperationState::Open) {
		auto& queue = priority ? m_priority_operations : m_pending_operations;
		const auto timeline = Common::FrameTimeline::Enabled() ? Common::FrameTimeline::context : Common::FrameTimeline::Context{};
		queue.push({std::move(operation), CurrentTick(), timeline.frame, timeline.submission});
		lock.unlock();
		if (priority) {
			m_operation_available.notify_one();
		}
		return sequence;
	}
	if (g_deferred_callback_scheduler != this) {
		m_operation_available.wait(lock,
		                           [this] { return m_operation_state == OperationState::Closed; });
	}
	lock.unlock();
	operation();
	return sequence;
}

bool CommandScheduler::HasPendingPriorityOperations() {
	std::lock_guard lock(m_operation_mutex);
	return !m_priority_operations.empty() || m_priority_active;
}

void CommandScheduler::PriorityOperationsThread(std::stop_token stop) {
	Common::SubmitProfile::WaitSiteScope site(Common::SubmitProfile::WaitSite::PriorityCallbacks);
	while (!stop.stop_requested()) {
		PendingOperation operation;
		{
			std::unique_lock lock(m_operation_mutex);
			m_operation_available.wait(lock, [this, &stop] {
				return stop.stop_requested() || !m_priority_operations.empty();
			});
			if (stop.stop_requested()) {
				return;
			}
			operation = std::move(m_priority_operations.front());
			m_priority_operations.pop();
			m_priority_active      = true;
			m_priority_active_tick = operation.tick;
		}
		Common::FrameTimeline::ContextScope timeline(operation.timeline_frame, operation.timeline_submission);
		m_master.Wait(operation.tick);
		if (!stop.stop_requested()) {
			Common::FrameTimeline::Name("priority completions");
			Common::FrameTimeline::Current(Common::FrameTimeline::Kind::PriorityBegin, operation.tick);
			RunOperation(std::move(operation.callback));
			Common::FrameTimeline::Current(Common::FrameTimeline::Kind::PriorityEnd, operation.tick);
		}
		{
			std::lock_guard lock(m_operation_mutex);
			m_priority_active      = false;
			m_priority_active_tick = 0;
		}
		m_operation_available.notify_all();
	}
}

void CommandScheduler::DrainPriorityOperations() {
	EXIT_IF(g_deferred_callback_scheduler == this);
	std::unique_lock lock(m_operation_mutex);
	m_operation_available.wait(
	    lock, [this] { return m_priority_operations.empty() && !m_priority_active; });
}

void CommandScheduler::WaitPriorityOperations(uint64_t tick) {
	Common::FrameTimeline::WaitScope timeline(Common::FrameTimeline::Kind::PriorityDrainBegin,
	    Common::FrameTimeline::Kind::PriorityDrainEnd, tick);
	Common::GpuThreadSampler::ImmediateScope boundary_immediate {Common::GpuThreadSampler::Boundary::PriorityWait};
	EXIT_IF(g_deferred_callback_scheduler == this);
	std::unique_lock lock(m_operation_mutex);
	m_operation_available.wait(lock, [this, tick] {
		const bool active_before_or_at = m_priority_active && m_priority_active_tick <= tick;
		const bool queued_before_or_at =
		    !m_priority_operations.empty() && m_priority_operations.front().tick <= tick;
		return !active_before_or_at && !queued_before_or_at;
	});
}

void CommandScheduler::RunOperation(Common::UniqueFunction<void>&& operation) {
	auto* previous                = g_deferred_callback_scheduler;
	g_deferred_callback_scheduler = this;
	operation();
	g_deferred_callback_scheduler = previous;
}

bool CommandScheduler::IsFree(uint64_t tick) {
	if (m_master.IsFree(tick)) {
		return true;
	}
	m_master.Refresh();
	return m_master.IsFree(tick);
}

void CommandScheduler::CheckActive() const {
	EXIT_IF(!Active());
}

CommandBuffer& CommandScheduler::Current() {
	EXIT_IF(m_command.IsInvalid());
	return m_command;
}

CommandBuffer& CommandScheduler::BeginCommand() {
	EXIT_IF(!m_command.IsInvalid());
	// Only the guest command scheduler opts in. Present/UI schedulers remain
	// direct; their queue operations keep the existing semaphore dependencies.
	// BufferCache also calls this during RenderContext construction, before
	// its m_gpu member exists. Use the producer's TLS, never HasGpu() there.
	if (!m_recorder_shutdown && Common::SubmitProfile::ExperimentSelected(Common::SubmitProfile::Experiment::VkRecorder) &&
	    GuestGpu::IsGpuThread() && this == &m_context.GetCommandScheduler()) {
		SetRecorderEnabled(Common::SubmitProfile::ExperimentActive(Common::SubmitProfile::Experiment::VkRecorder));
	}
	if (m_recorder && m_recorder->Enabled()) {
		m_command.m_recorder = m_recorder.get();
	} else m_command.m_buffer = m_command_pool.Commit();
	m_command.Begin();
	m_context.GetGpuTimeProfile().OnBegin(*this, m_command);
	m_context.GetGuestClock().OnBegin(*this, m_command);
	m_dynamic_state.Invalidate();
	return m_command;
}

uint64_t CommandScheduler::Submit(SubmitInfo submit) {
	Common::GpuThreadSampler::ImmediateScope boundary_immediate {Common::GpuThreadSampler::Boundary::Submit};
	// Includes ending the command buffer, the queue mutex and the Vulkan call.
	Common::RenderStageProfile::Scope stage_profile {Common::RenderStageProfile::Stage::Submit};
	EXIT_IF(m_command.IsInvalid());
	EXIT_IF(submit.num_wait_semaphores > SubmitInfo::MaxSemaphores ||
	        submit.num_signal_semaphores >= SubmitInfo::MaxSemaphores);

	// Put the diagnostic BOTTOM timestamp after the existing final rendering
	// end (including attachment stores). End() then sees rendering already closed;
	// this does not add a pass break, flush, barrier or submit.
	if (Common::FrameTimeline::Recording()) m_command.EndRendering();
	m_context.GetGpuTimeProfile().OnSubmit(*this, m_command, CurrentTick());
	m_command.End();
	if (m_command.m_recorder) {
		const bool trace = this == &m_context.GetCommandScheduler() && Common::FrameTimeline::Recording();
		const auto timeline = trace ? Common::FrameTimeline::context : Common::FrameTimeline::Context {};
		const auto tick = m_recorder->Submit(submit, timeline.frame, timeline.submission, trace);
		m_command.m_recorder = nullptr;
		Common::DebugCounters::Add(Common::DebugCounters::Counter::QueueSubmits);
		if (Common::SubmitProfile::Recording()) Common::SubmitProfile::RecordSubmit(SubmitProfileRuntime::ThreadId());
		return tick;
	}
	const auto buffer   = m_command.m_buffer;
	auto&      graphics = m_graphics;
	EXIT_IF(graphics.queue == nullptr);

	vk::Result result;
	uint64_t   tick;
	{
		Common::LockGuard lock(graphics.queue_mutex);
		tick = m_master.NextTick();
		submit.AddSignal(m_master.Handle(), tick);

		vk::TimelineSemaphoreSubmitInfo timeline_info {};
		timeline_info.waitSemaphoreValueCount   = submit.num_wait_semaphores;
		timeline_info.pWaitSemaphoreValues      = submit.wait_ticks.data();
		timeline_info.signalSemaphoreValueCount = submit.num_signal_semaphores;
		timeline_info.pSignalSemaphoreValues    = submit.signal_ticks.data();

		vk::SubmitInfo submit_info {};
		submit_info.pNext                = &timeline_info;
		submit_info.waitSemaphoreCount   = submit.num_wait_semaphores;
		submit_info.pWaitSemaphores      = submit.wait_semaphores.data();
		submit_info.pWaitDstStageMask    = submit.wait_stages.data();
		submit_info.commandBufferCount   = 1;
		submit_info.pCommandBuffers      = &buffer;
		submit_info.signalSemaphoreCount = submit.num_signal_semaphores;
		submit_info.pSignalSemaphores    = submit.signal_semaphores.data();

		if (this == &m_context.GetCommandScheduler()) {
			Common::FrameTimeline::Current(Common::FrameTimeline::Kind::NativeSubmit, tick);
		}
		result = graphics.queue.submit(1, &submit_info, nullptr);
	}
	Common::DebugCounters::Add(Common::DebugCounters::Counter::QueueSubmits);
	if (Common::SubmitProfile::Recording()) {
		Common::SubmitProfile::RecordSubmit(SubmitProfileRuntime::ThreadId());
	}

	if (result != vk::Result::eSuccess) {
		ReportVulkanFatal("vkQueueSubmit", result, tick, m_command.m_debug_op,
		                  m_command.m_debug_submit_id, m_command.m_debug_arg0,
		                  m_command.m_debug_arg1, m_command.m_debug_arg2, m_command.m_debug_arg3,
		                  m_command.m_debug_arg4);
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	m_command.m_buffer = nullptr;
	return tick;
}

void CommandScheduler::BeginNext() {
	Common::RenderStageProfile::Scope stage_profile {Common::RenderStageProfile::Stage::BeginNext};
	CheckActive();
	BeginCommand();
}

} // namespace Libs::Graphics
