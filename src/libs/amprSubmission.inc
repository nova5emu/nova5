// Included by libAmpr.cpp after ExecuteCommandBufferState.
// Adapted from Frank d6a807a4/c045f089 (GPL-2.0): independent in-order rings,
// snapshots at submit, and completion only after execution. No guest-thread waits in submit.

static int ExecuteAprCommandBuffer(uint64_t command_buffer, int32_t* execution_result,
                                   uint32_t* error_offset) {
	CommandBufferState state {};
	if (!TryGetCommandBufferState(command_buffer, &state)) return LibKernel::KERNEL_ERROR_EFAULT;
	return ExecuteCommandBufferState(command_buffer, state, false, execution_result, error_offset);
}

namespace {
struct PendingAmprSubmission {
	CommandBufferState state;
	uint64_t command_buffer = 0;
	uint32_t id = 0;
	uint64_t result = 0;
};

class WolverineAmprRing {
public:
	explicit WolverineAmprRing(bool amm, uint32_t priority): m_amm(amm), m_priority(priority) {
		std::thread([this] { Run(); }).detach();
	}
	void Enqueue(PendingAmprSubmission&& submission) {
		{
			std::scoped_lock lock(m_mutex);
			m_queue.push_back(std::move(submission));
		}
		m_wake.notify_one();
	}
	bool TryRunInline(const PendingAmprSubmission& submission) {
		{
			std::scoped_lock lock(m_mutex);
			if (m_running || !m_queue.empty()) return false;
			m_running = true;
		}
		Execute(submission);
		{
			std::scoped_lock lock(m_mutex);
			m_running = false;
		}
		m_wake.notify_one();
		return true;
	}
private:
	void Run() {
		for (;;) {
			PendingAmprSubmission submission;
			{
				std::unique_lock lock(m_mutex);
				m_wake.wait(lock, [this] { return !m_running && !m_queue.empty(); });
				submission = std::move(m_queue.front());
				m_queue.pop_front();
				m_running = true;
			}
			Execute(submission);
			{
				std::scoped_lock lock(m_mutex);
				m_running = false;
			}
		}
	}
	void Execute(const PendingAmprSubmission& submission) {
		int32_t result = OK;
		uint32_t offset = 0;
		const int status = ExecuteCommandBufferState(submission.command_buffer, submission.state,
		                                              m_amm, &result, &offset);
		if (status != OK && result == OK) result = status;
		if (result != OK || AprShared::TraceWolverineApr())
			std::fprintf(stderr, "[WOLVERINE][APR_COMPLETE] engine=%s priority=%u cb=0x%llx id=%u result=0x%x offset=0x%x\n",
			             m_amm ? "amm" : "apr", m_priority, (unsigned long long)submission.command_buffer,
			             submission.id, static_cast<uint32_t>(result), offset);
		if (submission.result != 0) {
			const int written = AprShared::WriteResult(reinterpret_cast<void*>(submission.result), result, offset);
			if (written != OK && result == OK) result = written;
		}
		// Publish completion last, after reads, maps, signals and the result buffer.
		if (submission.id != 0) AprShared::SetSubmissionResult(submission.id, result, offset);
	}
	bool m_amm;
	uint32_t m_priority;
	bool m_running = false;
	std::mutex m_mutex;
	std::condition_variable m_wake;
	std::deque<PendingAmprSubmission> m_queue;
};

static WolverineAmprRing& AmprRing(bool amm, uint32_t priority) {
	static std::mutex mutex;
	// Process-lifetime workers, like Frank. They never retain guest command-buffer pointers:
	// each queued batch owns its decoded records. Guest destinations live until completion.
	static std::map<std::pair<bool, uint32_t>, WolverineAmprRing*> rings;
	std::scoped_lock lock(mutex);
	auto& ring = rings[{amm, priority}];
	if (ring == nullptr) ring = new WolverineAmprRing(amm, priority);
	return *ring;
}
} // namespace

static int EnqueueCommandBuffer(uint64_t command_buffer, bool amm_engine, uint32_t priority,
                                uint32_t submission_id, uint64_t result_address) {
	PendingAmprSubmission submission;
	if (!TryGetCommandBufferState(command_buffer, &submission.state)) return LibKernel::KERNEL_ERROR_EFAULT;
	submission.command_buffer = command_buffer;
	submission.id = submission_id;
	submission.result = result_address;
	const auto& state = submission.state;
	if (AprShared::TraceWolverineApr())
		std::fprintf(stderr, "[WOLVERINE][APR_QUEUE] engine=%s priority=%u cb=0x%llx id=%u maps=%zu reads=%zu waits=%zu counters=%zu\n",
		             amm_engine ? "amm" : "apr", priority, (unsigned long long)command_buffer, submission_id,
		             state.amm_map_commands.size(), state.read_file_commands.size(),
		             state.wait_address_commands.size(), state.counter_commands.size());
	auto& ring = AmprRing(amm_engine, priority);
	// Preserve Frank's immediate map-only fast path without overtaking earlier work.
	if (amm_engine && state.wait_address_commands.empty() && state.counter_commands.empty() &&
	    state.read_file_commands.empty() && ring.TryRunInline(submission)) return OK;
	ring.Enqueue(std::move(submission));
	return OK;
}

// Shared by all six submit entry points, preserving their kernel/Posix return conventions.
static int SubmitWolverineCommandBuffer(uint64_t command_buffer, bool amm, uint32_t priority,
                                        void* result, uint32_t* out_id, bool track) {
	const auto id = track ? AprShared::AllocateSubmissionId(command_buffer, reinterpret_cast<uint64_t>(result)) : 0;
	const int status = EnqueueCommandBuffer(command_buffer, amm, priority, id, reinterpret_cast<uint64_t>(result));
	if (status != OK) {
		if (id != 0) {
			AprShared::SetSubmissionResult(id, status, 0);
			AprShared::CompleteSubmission(id, nullptr);
		}
		return status;
	}
	if (out_id != nullptr) *out_id = id;
	return OK;
}
