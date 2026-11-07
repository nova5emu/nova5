// Included by libAmpr.cpp after the command-record helpers.
// Counter encoding and comparisons: Frank d6a807a4, GPL-2.0.
// Only reachable through NOVA5_WOLVERINE_APR_ASYNC; other titles keep their stubs.

static bool WriteAmprSignal(uint64_t address, uint64_t value) {
	if (!AprShared::IsValidGuestRange(address, sizeof(value), true)) return false;
	if (AprShared::WolverineAprAsyncEnabled() && (address & 7u) == 0) {
		std::atomic_ref<uint64_t>(*reinterpret_cast<uint64_t*>(address)).store(value, std::memory_order_release);
		return true;
	}
	return AprShared::WriteGuest(address, value);
}

static int ExecuteCounterCommand(const CommandBufferState::CounterCommand& command,
                                 std::unique_lock<std::mutex>& execution_lock) {
	CounterAccess access {};
	if (!DecodeCounterAccess(command.index, command.access, &access))
		return LibKernel::KERNEL_ERROR_EINVAL;
	using Kind = CommandBufferState::CounterCommandKind;
	if (command.kind == Kind::Write) {
		std::scoped_lock lock(g_counter_mutex);
		const auto current = ReadCounterLocked(access);
		uint64_t next = command.value;
		switch (command.op) {
			case AMPR_WRITE_COUNTER_STORE: break;
			case AMPR_WRITE_COUNTER_ATOMIC_OR: next = current | command.value; break;
			case AMPR_WRITE_COUNTER_ATOMIC_AND_COMPLEMENT: next = current & ~command.value; break;
			case AMPR_WRITE_COUNTER_ATOMIC_XOR: next = current ^ command.value; break;
			case AMPR_WRITE_COUNTER_ATOMIC_ADD: next = current + command.value; break;
			default: return LibKernel::KERNEL_ERROR_EINVAL;
		}
		WriteCounterLocked(access, next & CounterValueMask(access.bytes));
		if (AprShared::TraceWolverineApr())
			std::fprintf(stderr, "[WOLVERINE][APR_COUNTER_WRITE] index=%u access=%u op=%u value=0x%llx now=0x%llx\n",
			             command.index, command.access, command.op, (unsigned long long)command.value,
			             (unsigned long long)ReadCounterLocked(access));
		g_counter_cv.notify_all();
		return OK;
	}
	if (command.kind == Kind::Wait) {
		execution_lock.unlock();
		bool met = false;
		uint64_t observed = 0;
		{
			std::unique_lock lock(g_counter_mutex);
			const auto satisfied = [&] {
				observed = ReadCounterLocked(access);
				const auto value = command.mask_op == AMPR_WAIT_ON_COUNTER_MASK_AND
				                       ? observed & command.mask : observed;
				return AmprWaitSatisfied(value, command.value, command.op, access.bytes);
			};
			met = g_counter_cv.wait_for(lock, std::chrono::seconds(10), satisfied);
		}
		execution_lock.lock();
		if (!met || AprShared::TraceWolverineApr())
			std::fprintf(stderr, "[WOLVERINE][APR_COUNTER_WAIT] index=%u access=%u compare=%u ref=0x%llx observed=0x%llx met=%u\n",
			             command.index, command.access, command.op, (unsigned long long)command.value,
			             (unsigned long long)observed, met ? 1u : 0u);
		// A timeout is an execution error, never permission to read an unmapped destination.
		return met ? OK : LibKernel::KERNEL_ERROR_ETIMEDOUT;
	}
	uint64_t value = 0;
	{
		std::scoped_lock lock(g_counter_mutex);
		value = ReadCounterLocked(access);
	}
	return WriteAmprSignal(command.address, value) ? OK : LibKernel::KERNEL_ERROR_EFAULT;
}

static int ExecuteWaitAddress(const CommandBufferState::WaitAddressCommand& command,
                              std::unique_lock<std::mutex>& execution_lock) {
	execution_lock.unlock();
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	uint64_t observed = 0;
	int result = OK;
	for (;;) {
		if (!AprShared::IsValidGuestRange(command.address, sizeof(observed))) {
			result = LibKernel::KERNEL_ERROR_EFAULT;
			break;
		}
		if ((command.address & 7u) == 0) {
			observed = std::atomic_ref<uint64_t>(*reinterpret_cast<uint64_t*>(command.address)).load(std::memory_order_acquire);
		} else {
			AprShared::ReadGuest(command.address, &observed);
		}
		if (AmprWaitSatisfied(observed, command.ref_value, command.compare, 8)) break;
		if (std::chrono::steady_clock::now() >= deadline) {
			result = LibKernel::KERNEL_ERROR_ETIMEDOUT;
			break;
		}
		std::this_thread::sleep_for(std::chrono::microseconds(50));
	}
	execution_lock.lock();
	if (result != OK || AprShared::TraceWolverineApr())
		std::fprintf(stderr, "[WOLVERINE][APR_ADDRESS_WAIT] address=0x%llx compare=%u ref=0x%llx observed=0x%llx result=0x%x\n",
		             (unsigned long long)command.address, command.compare, (unsigned long long)command.ref_value,
		             (unsigned long long)observed, static_cast<uint32_t>(result));
	return result;
}
