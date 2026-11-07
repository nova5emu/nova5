#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "local/shader-loop-guard.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include <spirv-tools/libspirv.hpp>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <unordered_map>

using namespace Libs::Graphics;
static void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "ShaderLoopGuardTests: %s\n", message); std::exit(1); }
}

#include "ShaderLoopGuardInterpreter.inc"

int main(int argc, char** argv) {
    Common::InitializeThreads(); Common::Subsystems subsystems;
    subsystems.Initialize<Config::Lifecycle>(); Config::ConfigOptions config;
    config.printf_direction = Config::OutputDirection::Silent; Config::Load(config);
    subsystems.Initialize<Log::Lifecycle>();
    // s0=0; while(s0<3) ++s0; end. Header PC=4. Carries a PHI through a backedge.
    const uint32_t code[] = {0xbe800380, 0xbf0a8300, 0xbf840002, 0x80008100, 0xbf82fffc, 0xbf810000};
    ShaderPixelInputInfo pixel{}; ShaderRecompiler::CompileOptions options;
    options.stage = ShaderType::Pixel; options.input_info.pixel = &pixel; options.shader_hash = 0x1234;
    auto translated = ShaderRecompiler::TranslateProgram(code, options);
    const auto& settings = ShaderLoopGuard::Settings();
    // The SCC candidate must affect the branch IR, and only for the selected hash.
    const bool expect_vote = settings.scc_vote && settings.Matches(options.shader_hash);
    bool has_vote = false;
    for (const auto* block: translated.program.blocks) for (const auto& inst: *block)
        has_vote |= inst.GetOpcode() == ShaderRecompiler::IR::ValueOpcode::AnyLane;
    Check(has_vote == expect_vote, "SCC vote selection mismatch");
    if (argc > 2 && std::string(argv[2]) == "dispatcher") translated.program.dispatcher_fallback = true;
    auto compiled = ShaderRecompiler::CompileProgram(std::move(translated), options, {}, 0);
    spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_3);
    validator.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t&, const char* m) { std::fprintf(stderr, "%s\n", m); });
    Check(validator.Validate(compiled.spirv), "guarded SPIR-V is invalid");
    if (argc > 1) { std::ofstream file(argv[1], std::ios::binary); file.write(reinterpret_cast<const char*>(compiled.spirv.data()), compiled.spirv.size()*4); }
    if (!expect_vote && !compiled.program.dispatcher_fallback) {
        const bool selected = settings.limit != 0 && settings.Matches(options.shader_hash) &&
                              (settings.pc == UINT32_MAX || settings.pc == 4);
        const auto visits = ExecuteLoopGuardFixture(compiled.spirv);
        Check(visits == (selected ? std::min(settings.limit + 1, 4u) : 0u), "guard did not bound header visits");
    }
    std::puts("ShaderLoopGuardTests: passed (host only)"); subsystems.Destroy();
}
