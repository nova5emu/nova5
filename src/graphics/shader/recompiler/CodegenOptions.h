#ifndef EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_CODEGENOPTIONS_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_CODEGENOPTIONS_H_

#include <cstdint>

namespace Libs::Graphics::ShaderRecompiler {

enum class PsLiveExec : uint8_t { Off, AppendConsume, All };

constexpr uint32_t RtNodeBudgetGdsFromEnd = 2;
constexpr uint32_t RtNodeStatsGdsFromEnd = 3;
constexpr uint32_t RtNodeStatsBins = 16;

struct CodegenOptions {
	// Enabled by the device only for Wolverine, after descriptor-indexing feature checks.
	bool bindless_images = false;
	// Title-gated by device initialization, before prefetch/compilation starts.
	bool coherent_buffers = false;
	bool wolverine_loop_guard = false;
	// KYTY_PS_APPEND_LIVE_ELECTION=0 restores election from EXEC including helpers.
	bool ps_append_live_election = true;
	// KYTY_PS_LIVE_EXEC=0|1|all: start with the pixel valid mask for append/consume shaders or all PS.
	PsLiveExec ps_live_exec = PsLiveExec::AppendConsume;
	// KYTY_PS_HELPER_ATOMICS_SKIP=0 restores helper participation in emulated CAS loops.
	bool ps_helper_atomics_skip = true;
	// KYTY_RT_SOFTWARE=auto|1|0: auto retries unsupported BVH shaders with the miss stub.
	bool rt_software = true;
	bool rt_software_auto = true;
	bool rt_stub = true; // KYTY_RT_STUB: miss fallback when software RT is unavailable
	bool rt_type6 = true;
	uint32_t rt_function = 1; // KYTY_RT_FUNCTION=0 inline, 1 shared function, 2 DontInline
	uint32_t rt_node_budget = 8192; // KYTY_RT_NODE_BUDGET=0 disables the traversal limit
	bool rt_node_stats = false;
};

[[nodiscard]] const CodegenOptions& GetCodegenOptions();
// Test hook; call only while no shader is being compiled.
void SetCodegenOptions(const CodegenOptions& options);

// A fallback belongs to one translation, not to other prefetch threads.
class ScopedRtStub {
public:
	explicit ScopedRtStub(bool enabled);
	~ScopedRtStub();
	ScopedRtStub(const ScopedRtStub&) = delete;
	ScopedRtStub& operator=(const ScopedRtStub&) = delete;
private:
	bool previous;
};
[[nodiscard]] bool SoftwareRtEnabled();
[[nodiscard]] uint64_t RtCodegenFingerprint();

} // namespace Libs::Graphics::ShaderRecompiler

#endif /* EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_CODEGENOPTIONS_H_ */
