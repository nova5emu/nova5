#pragma once

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace Common {

// Opt-in file diagnostics only. Never changes a guest return value or reads its buffer.
class BankIoTrace {
public:
	static bool Enabled() {
		static const bool enabled = [] {
			const char* value = std::getenv("KYTY_BANK_IO_TRACE");
			const bool on = value != nullptr && std::strcmp(value, "1") == 0;
			if (on) {
				std::fprintf(stderr, "[BANK_IO_TRACE] started limit=131072 operations; offsets/sizes=-1 mean unknown\n");
				std::fflush(stderr);
			}
			return on;
		}();
		return enabled;
	}
	static bool Matches(std::string_view path) {
		if (path.size() < 5) { return false; }
		const auto suffix = path.substr(path.size() - 5);
		for (size_t i = 0; i < 5; ++i) {
			char ch = suffix[i];
			if (ch >= 'A' && ch <= 'Z') { ch += 'a' - 'A'; }
			if (ch != std::string_view(".bank")[i]) { return false; }
		}
		return true;
	}
	BankIoTrace(const char* operation, int fd, int64_t argument = 0, uint64_t count = 0):
	    m_operation(operation), m_fd(fd), m_argument(argument), m_count(count) {}
	void Start(std::string_view path, uint64_t tid) {
		if (!Enabled() || !Matches(path)) { return; }
		static std::atomic<uint64_t> sequence {0};
		const auto seq = sequence.fetch_add(1, std::memory_order_relaxed);
		if (seq >= 131072) {
			if (seq == 131072) {
				std::fprintf(stderr, "[BANK_IO_TRACE] limit reached; further operations omitted\n");
				std::fflush(stderr);
			}
			return;
		}
		m_id = seq + 1;
		m_tid = tid;
		m_path = Escape(path);
		m_start = Now();
		std::fprintf(stderr, "[BANK_IO] id=%" PRIu64 " phase=enter ms=%" PRIu64
		                     " tid=%" PRIu64 " op=%s fd=%d arg=%" PRId64 " count=%" PRIu64 " path=\"%s\"\n",
		             m_id, m_start, m_tid, m_operation, m_fd, m_argument, m_count, m_path.c_str());
		std::fflush(stderr);
	}
	bool Active() const { return m_id != 0; }
	void HostPath(std::string_view path) { if (Active()) { m_host = Escape(path); } }
	void Context(int64_t position, int64_t size) { m_position = position; m_size = size; }
	void Count(uint64_t count) { m_count = count; }
	template<class T> T Result(T result) {
		if (Active()) {
			const auto now = Now();
			std::fprintf(stderr, "[BANK_IO] id=%" PRIu64 " phase=exit ms=%" PRIu64 " elapsed_ms=%" PRIu64
			                     " tid=%" PRIu64 " op=%s fd=%d arg=%" PRId64 " count=%" PRIu64
			                     " pos=%" PRId64 " size=%" PRId64 " result=%" PRId64 " result_hex=0x%" PRIx64
			                     " path=\"%s\" host=\"%s\"\n",
			             m_id, now, now-m_start, m_tid, m_operation, m_fd, m_argument, m_count,
			             m_position, m_size, static_cast<int64_t>(result), static_cast<uint64_t>(result),
			             m_path.c_str(), m_host.c_str());
			std::fflush(stderr);
		}
		return result;
	}
private:
	static uint64_t Now() {
		return std::chrono::duration_cast<std::chrono::milliseconds>(
		    std::chrono::steady_clock::now().time_since_epoch()).count();
	}
	static std::string Escape(std::string_view value) {
		std::string out;
		for (char ch: value) {
			if (ch == '\n') { out += "\\n"; }
			else if (ch == '\r') { out += "\\r"; }
			else if (ch == '\t') { out += "\\t"; }
			else if (ch == '"' || ch == '\\') { out += '\\'; out += ch; }
			else { out += ch; }
		}
		return out;
	}
	const char* m_operation;
	int m_fd;
	int64_t m_argument;
	uint64_t m_count, m_id = 0, m_tid = 0, m_start = 0;
	int64_t m_position = -1, m_size = -1;
	std::string m_path, m_host;
};

} // namespace Common
