#include "graphics/shader/recompiler/CodegenOptions.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace Libs::Graphics::ShaderRecompiler {
namespace {

thread_local bool force_rt_stub = false;

CodegenOptions FromEnvironment() {
	CodegenOptions options;
	const auto enabled = [](const char* name) {
		const auto* value = std::getenv(name);
		return value == nullptr || std::strcmp(value, "0") != 0;
	};
	options.ps_append_live_election = enabled("KYTY_PS_APPEND_LIVE_ELECTION");
	options.ps_helper_atomics_skip  = enabled("KYTY_PS_HELPER_ATOMICS_SKIP");
	options.rt_software = enabled("KYTY_RT_SOFTWARE");
	options.rt_stub = enabled("KYTY_RT_STUB");
	options.rt_type6 = enabled("KYTY_RT_TYPE6");
	if (const auto* mode = std::getenv("KYTY_RT_SOFTWARE"); mode != nullptr && *mode != '\0') {
		options.rt_software_auto = std::strcmp(mode, "auto") == 0;
	}
	const auto number = [](const char* name, uint32_t fallback) {
		const auto* value = std::getenv(name);
		return value == nullptr || *value == '\0' ? fallback : static_cast<uint32_t>(std::strtoul(value, nullptr, 0));
	};
	options.rt_function = std::min(number("KYTY_RT_FUNCTION", 1), 2u);
	options.rt_node_budget = number("KYTY_RT_NODE_BUDGET", 8192);
	options.rt_node_stats = number("KYTY_RT_NODE_STATS", 0) != 0;
	if (const auto* mode = std::getenv("KYTY_PS_LIVE_EXEC"); mode != nullptr && mode[0] != '\0') {
		options.ps_live_exec = std::strcmp(mode, "all") == 0 ? PsLiveExec::All
		                       : std::strcmp(mode, "0") == 0 ? PsLiveExec::Off
		                                                     : PsLiveExec::AppendConsume;
	}
	return options;
}

CodegenOptions& Storage() {
	static auto options = FromEnvironment();
	return options;
}

} // namespace

const CodegenOptions& GetCodegenOptions() {
	return Storage();
}

void SetCodegenOptions(const CodegenOptions& options) {
	Storage() = options;
}

ScopedRtStub::ScopedRtStub(bool enabled): previous(force_rt_stub) {
	force_rt_stub = enabled;
}

ScopedRtStub::~ScopedRtStub() {
	force_rt_stub = previous;
}

bool SoftwareRtEnabled() {
	return Storage().rt_software && !force_rt_stub;
}

uint64_t RtCodegenFingerprint() {
	const auto& options = Storage();
	return (uint64_t {options.rt_node_budget} << 16u) | (uint64_t {options.rt_function} << 8u) |
	       (uint64_t {options.bindless_images} << 5u) |
	       (uint64_t {options.coherent_buffers} << 6u) |
	       (uint64_t {options.wolverine_loop_guard} << 7u) |
	       (uint64_t {options.rt_node_stats} << 4u) | (uint64_t {options.rt_type6} << 3u) |
	       (uint64_t {options.rt_stub} << 2u) | (uint64_t {options.rt_software_auto} << 1u) |
	       uint64_t {options.rt_software};
}

} // namespace Libs::Graphics::ShaderRecompiler
