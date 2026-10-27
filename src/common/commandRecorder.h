#ifndef NOVA5_COMMON_COMMAND_RECORDER_H
#define NOVA5_COMMON_COMMAND_RECORDER_H

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <type_traits>
#include <vector>

namespace Common::CommandRecording {

enum class Drain : uint8_t { Native, Upload, Readback, Submit, Finish, WaitTick, Gate, Shutdown, Capacity, Count };
inline constexpr std::array DrainNames {"native", "upload", "readback", "submit", "finish", "wait_tick", "gate", "shutdown", "capacity"};
static_assert(DrainNames.size() == static_cast<size_t>(Drain::Count));

// One producer, one consumer. Packet data is copied; pointer-bearing SDK structs
// must first be flattened by the encoder. No per-command allocation or callable.
// Consumer/lifecycle callbacks may NOT call the producer API or throw.
class Queue final {
public:
	using Consume = void (*)(void*, uint16_t, std::span<const std::byte>) noexcept;
	using Lifecycle = void (*)(void*) noexcept;
	struct Options { size_t block_bytes = 65536, block_count = 8; bool measure = true; };
	struct Stats {
		uint64_t frames = 0, batches = 0, packets = 0, bytes = 0, max_packets = 0;
		uint64_t oversized = 0, wait_ns = 0, worker_cpu_ns = 0, worker_wall_ns = 0;
		uint64_t completed_batches = 0, storage_bytes = 0;
		bool timing = false, cpu_available = false;
		std::array<uint64_t, static_cast<size_t>(Drain::Count)> requests {}, drains {};
		std::array<uint64_t, static_cast<size_t>(Drain::Count)> wait_by_cause_ns {};
	};
	Queue(Consume consume, void* context, Options options, Lifecycle start = nullptr, Lifecycle stop = nullptr);
	~Queue();
	Queue(const Queue&) = delete;
	Queue& operator=(const Queue&) = delete;
	// Disabled construction allocates no blocks and starts no thread. A->B starts
	// once; B->A drains before returning and parks the worker. Stop is final.
	bool SetEnabled(bool enabled);
	bool Enabled() const { return m_enabled; }
	bool Append(uint16_t opcode, std::span<const std::byte> payload);
	bool AppendParts(uint16_t opcode, std::span<const std::span<const std::byte>> parts);
	template <typename T> bool AppendValue(uint16_t opcode, const T& value) {
		static_assert(std::is_trivially_copyable_v<T>);
		return Append(opcode, std::as_bytes(std::span(&value, 1)));
	}
	// Publish without waiting. A full block is published automatically.
	void Publish();
	// Recording completion only. This is NEVER evidence of native GPU completion.
	void DrainUntilIdle(Drain reason);
	void Frame() { ++m_stats.frames; }
	void Stop();
	Stats Snapshot() const;
	void Report(std::FILE* output) const; // cumulative; difference snapshots for windows
private:
	struct Header { uint16_t opcode, reserved; uint32_t bytes; };
	static_assert(sizeof(Header) == 8);
	struct Block { std::vector<std::byte> data; size_t used = 0; uint64_t packets = 0, ticket = 0; };
	static constexpr size_t None = SIZE_MAX;
	void Acquire();
	void Run() noexcept;
	void CheckProducer() const;
	Consume m_consume;
	void* m_context;
	Options m_options;
	Lifecycle m_start, m_stop;
	std::thread::id m_producer;
	std::thread m_thread;
	std::vector<Block> m_blocks;
	std::vector<size_t> m_free, m_ready;
	size_t m_build = None, m_head = 0, m_tail = 0, m_count = 0;
	uint64_t m_published = 0, m_completed = 0, m_last_drain = 0;
	bool m_enabled = false, m_started = false, m_exit = false, m_stopped = false;
	mutable std::mutex m_mutex;
	std::condition_variable m_work, m_done;
	Stats m_stats;
	std::atomic<uint64_t> m_cpu_ns {0}, m_wall_ns {0};
	std::atomic<bool> m_cpu_available {false};
};

// Optional producer-side pins for resources not already owned by the scheduler.
// Complete must be fed the NATIVE completed tick, never a queue ticket or Drain.
// Destruction with outstanding pins is a programming error, not an early free.
class TickPins final {
public:
	TickPins() = default;
	TickPins(const TickPins&) = delete;
	TickPins& operator=(const TickPins&) = delete;
	~TickPins();
	void Hold(uint64_t tick, std::shared_ptr<void> resource);
	void Complete(uint64_t native_completed_tick);
	size_t Pending() const { return m_pins.size(); }
private:
	struct Pin { uint64_t tick; std::shared_ptr<void> resource; };
	std::vector<Pin> m_pins;
	uint64_t m_completed = 0;
};

} // namespace Common::CommandRecording
#endif
