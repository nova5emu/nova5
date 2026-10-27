#include "common/commandRecorder.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <exception>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#endif

namespace Common::CommandRecording {
namespace {
uint64_t Now() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	    std::chrono::steady_clock::now().time_since_epoch()).count());
}
bool ThreadCpu(uint64_t& ns) {
#if defined(_WIN32)
	FILETIME creation {}, exit {}, kernel {}, user {};
	if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) return false;
	const auto ticks = [](FILETIME value) { return (uint64_t{value.dwHighDateTime} << 32) | value.dwLowDateTime; };
	ns = (ticks(kernel) + ticks(user)) * 100;
	return true;
#elif defined(CLOCK_THREAD_CPUTIME_ID)
	timespec value {};
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0) return false;
	ns = static_cast<uint64_t>(value.tv_sec) * 1000000000ull + static_cast<uint64_t>(value.tv_nsec);
	return true;
#else
	(void)ns;
	return false;
#endif
}
} // namespace

Queue::Queue(Consume consume, void* context, Options options, Lifecycle start, Lifecycle stop)
    : m_consume(consume), m_context(context), m_options(options), m_start(start), m_stop(stop),
      m_producer(std::this_thread::get_id()) {
	if (!consume || options.block_count < 2 || options.block_bytes < 16 ||
	    options.block_bytes > UINT32_MAX || options.block_bytes % 8) std::terminate();
	m_stats.timing = options.measure;
}
Queue::~Queue() { Stop(); }
void Queue::CheckProducer() const { assert(std::this_thread::get_id() == m_producer); }

bool Queue::SetEnabled(bool enabled) {
	CheckProducer();
	if (m_stopped) return false;
	if (m_enabled == enabled) return true;
	if (!enabled) {
		DrainUntilIdle(Drain::Gate);
		m_enabled = false;
		return true;
	}
	if (!m_thread.joinable()) {
		m_blocks.resize(m_options.block_count);
		m_ready.resize(m_options.block_count);
		m_free.reserve(m_options.block_count);
		for (size_t i = 0; i < m_blocks.size(); ++i) {
			m_blocks[i].data.resize(m_options.block_bytes);
			m_free.push_back(i);
		}
		m_stats.storage_bytes = m_options.block_count * m_options.block_bytes;
		m_thread = std::thread([this] { Run(); });
		std::unique_lock lock(m_mutex);
		m_done.wait(lock, [this] { return m_started; });
	}
	m_enabled = true;
	return true;
}

void Queue::Acquire() {
	if (m_build != None) return;
	std::unique_lock lock(m_mutex);
	if (m_free.empty()) {
		++m_stats.requests[static_cast<size_t>(Drain::Capacity)];
		++m_stats.drains[static_cast<size_t>(Drain::Capacity)];
		const auto start = m_options.measure ? Now() : 0;
		m_done.wait(lock, [this] { return !m_free.empty(); });
		if (m_options.measure) {
			const auto elapsed = Now() - start;
			m_stats.wait_ns += elapsed;
			m_stats.wait_by_cause_ns[static_cast<size_t>(Drain::Capacity)] += elapsed;
		}
	}
	m_build = m_free.back(); m_free.pop_back();
	m_blocks[m_build].used = 0;
	m_blocks[m_build].packets = 0;
}

bool Queue::Append(uint16_t opcode, std::span<const std::byte> payload) {
	return AppendParts(opcode, std::span(&payload, 1));
}
bool Queue::AppendParts(uint16_t opcode, std::span<const std::span<const std::byte>> parts) {
	CheckProducer();
	if (!m_enabled) return false;
	size_t bytes = 0;
	for (const auto part: parts) {
		if (part.size() > m_options.block_bytes - sizeof(Header) - bytes) { ++m_stats.oversized; return false; }
		bytes += part.size();
	}
	const auto padded = (sizeof(Header) + bytes + 7) & ~size_t{7};
	if (m_build != None && padded > m_options.block_bytes - m_blocks[m_build].used) Publish();
	Acquire();
	auto& block = m_blocks[m_build];
	const Header header {opcode, 0, static_cast<uint32_t>(bytes)};
	auto* destination = block.data.data() + block.used;
	std::memcpy(destination, &header, sizeof(header)); destination += sizeof(header);
	for (const auto part: parts) {
		if (!part.empty()) std::memcpy(destination, part.data(), part.size());
		destination += part.size();
	}
	block.used += padded; ++block.packets;
	++m_stats.packets; m_stats.bytes += bytes;
	return true;
}

void Queue::Publish() {
	CheckProducer();
	if (m_build == None) return;
	auto& block = m_blocks[m_build];
	block.ticket = ++m_published;
	++m_stats.batches;
	m_stats.max_packets = std::max(m_stats.max_packets, block.packets);
	{
		std::lock_guard lock(m_mutex);
		assert(m_count < m_ready.size());
		m_ready[m_tail] = m_build;
		m_tail = (m_tail + 1) % m_ready.size(); ++m_count;
		m_build = None;
	}
	m_work.notify_one();
}

void Queue::DrainUntilIdle(Drain reason) {
	CheckProducer();
	if (reason >= Drain::Count) std::terminate();
	++m_stats.requests[static_cast<size_t>(reason)];
	Publish();
	if (m_last_drain == m_published) return;
	++m_stats.drains[static_cast<size_t>(reason)];
	std::unique_lock lock(m_mutex);
	if (m_completed < m_published) {
		const auto start = m_options.measure ? Now() : 0;
		m_done.wait(lock, [this] { return m_completed == m_published; });
		if (m_options.measure) {
			const auto elapsed = Now() - start;
			m_stats.wait_ns += elapsed;
			m_stats.wait_by_cause_ns[static_cast<size_t>(reason)] += elapsed;
		}
	}
	m_last_drain = m_published;
}

void Queue::Run() noexcept {
#if defined(_WIN32)
	SetThreadDescription(GetCurrentThread(), L"Vulkan recorder");
#endif
	uint64_t cpu_start = 0;
	const bool cpu_supported = m_options.measure && ThreadCpu(cpu_start);
	m_cpu_available.store(cpu_supported, std::memory_order_relaxed);
	if (m_start) m_start(m_context);
	{
		std::lock_guard lock(m_mutex);
		m_started = true;
	}
	m_done.notify_all();
	for (;;) {
		size_t index;
		{
			std::unique_lock lock(m_mutex);
			m_work.wait(lock, [this] { return m_exit || m_count != 0; });
			if (m_count == 0) break;
			index = m_ready[m_head]; m_head = (m_head + 1) % m_ready.size(); --m_count;
		}
		const auto start = m_options.measure ? Now() : 0;
		const auto& block = m_blocks[index];
		for (size_t offset = 0; offset < block.used;) {
			Header header {};
			std::memcpy(&header, block.data.data() + offset, sizeof(header));
			m_consume(m_context, header.opcode, {block.data.data() + offset + sizeof(Header), header.bytes});
			offset += (sizeof(Header) + header.bytes + 7) & ~size_t{7};
		}
		if (m_options.measure) m_wall_ns.fetch_add(Now() - start, std::memory_order_relaxed);
		uint64_t cpu = 0;
		if (cpu_supported && ThreadCpu(cpu)) m_cpu_ns.store(cpu - cpu_start, std::memory_order_relaxed);
		{
			std::lock_guard lock(m_mutex);
			m_completed = block.ticket;
			m_free.push_back(index);
		}
		m_done.notify_all();
	}
	if (m_stop) m_stop(m_context);
}

void Queue::Stop() {
	CheckProducer();
	if (m_stopped) return;
	if (m_thread.joinable()) {
		DrainUntilIdle(Drain::Shutdown);
		{
			std::lock_guard lock(m_mutex); m_exit = true;
		}
		m_work.notify_one();
		m_thread.join();
	}
	m_enabled = false; m_stopped = true;
}
Queue::Stats Queue::Snapshot() const {
	CheckProducer();
	auto result = m_stats;
	std::lock_guard lock(m_mutex);
	result.completed_batches = m_completed;
	result.worker_cpu_ns = m_cpu_ns.load(std::memory_order_relaxed);
	result.worker_wall_ns = m_wall_ns.load(std::memory_order_relaxed);
	result.cpu_available = m_cpu_available.load(std::memory_order_relaxed);
	return result;
}
void Queue::Report(std::FILE* output) const {
	const auto s = Snapshot();
	std::fprintf(output, "[VK_RECORDER] frames=%llu batches=%llu packets=%llu batches_per_frame=%.3f packets_per_batch=%.3f max_packets=%llu wait_ms=%.3f worker_cpu_ms=%.3f worker_wall_ms=%.3f timing=%u cpu_available=%u oversized=%llu storage_bytes=%llu",
	    static_cast<unsigned long long>(s.frames), static_cast<unsigned long long>(s.batches), static_cast<unsigned long long>(s.packets),
	    s.frames ? double(s.batches) / s.frames : 0, s.batches ? double(s.packets) / s.batches : 0,
	    static_cast<unsigned long long>(s.max_packets), s.wait_ns / 1e6, s.worker_cpu_ns / 1e6, s.worker_wall_ns / 1e6,
	    unsigned(s.timing), unsigned(s.cpu_available), static_cast<unsigned long long>(s.oversized), static_cast<unsigned long long>(s.storage_bytes));
	for (size_t i = 0; i < DrainNames.size(); ++i)
		std::fprintf(output, " drain_%s=%llu request_%s=%llu wait_%s_ms=%.3f", DrainNames[i], static_cast<unsigned long long>(s.drains[i]),
		    DrainNames[i], static_cast<unsigned long long>(s.requests[i]), DrainNames[i], s.wait_by_cause_ns[i] / 1e6);
	std::fputc('\n', output);
}

TickPins::~TickPins() { if (!m_pins.empty()) std::terminate(); }
void TickPins::Hold(uint64_t tick, std::shared_ptr<void> resource) {
	if (!resource || tick <= m_completed || (!m_pins.empty() && tick < m_pins.back().tick)) std::terminate();
	m_pins.push_back({tick, std::move(resource)});
}
void TickPins::Complete(uint64_t native_completed_tick) {
	if (native_completed_tick < m_completed) std::terminate();
	m_completed = native_completed_tick;
	const auto end = std::find_if(m_pins.begin(), m_pins.end(), [&](const auto& pin) { return pin.tick > native_completed_tick; });
	m_pins.erase(m_pins.begin(), end);
}
} // namespace Common::CommandRecording
