// Included after the CFG harness helpers. These tests do not create a Vulkan device.
struct RestoreStabilityCodegen {
  ShaderRecompiler::CodegenOptions saved = ShaderRecompiler::GetCodegenOptions();
  ~RestoreStabilityCodegen() { ShaderRecompiler::SetCodegenOptions(saved); }
};

void TestWolverineCoherentBuffers() {
  using namespace ShaderRecompiler;
  RestoreStabilityCodegen restore;
  Check(Wolverine::CoherentBuffersEnabled("PPSA03671", nullptr) &&
        !Wolverine::CoherentBuffersEnabled("PPSA03671", "0") &&
        !Wolverine::CoherentBuffersEnabled("PPSA01342", "1") &&
        !Wolverine::CoherentBuffersEnabled("PPSA01405", nullptr) &&
        !Wolverine::CoherentBuffersEnabled("CUSA03173", "1"),
        "Wolverine coherent buffers title gate or opt-out failed");
  auto codegen = restore.saved;
  codegen.coherent_buffers = false;
  SetCodegenOptions(codegen);
  const auto old_key = RtCodegenFingerprint();
  codegen.coherent_buffers = true;
  SetCodegenOptions(codegen);
  Check(old_key != RtCodegenFingerprint(), "coherent buffers did not change the shader cache key");

  // GLC, DLC alone, both (the captured poll), and neither. Wide loads must
  // preserve the policy on every component.
  for (const uint32_t flags : {0u, 1u << 14, 1u << 15, 3u << 14}) {
    for (const bool enabled : {false, true}) {
      codegen.coherent_buffers = enabled;
      SetCodegenOptions(codegen);
      for (const uint32_t opcode : {0x0cu, 0x0eu}) {
        const uint32_t shader[] = {
          EncodeMubuf0(opcode) | flags, EncodeMubuf1(0, 0, 1),
          EncodeMubuf0(opcode == 0x0e ? 0x1e : 0x1c, 0, false), EncodeMubuf1(0, 12, 0),
          EncodeSopp(0x01),
        };
        auto options = MakeCompileOptions(ShaderType::Compute);
        auto result = RecompileForTest(shader, options);
        CheckSpirvBinaryValidates(result.spirv);
        const auto source = DisassembleSpirvBinary(result.spirv);
        const bool expected = enabled && flags != 0;
        Check(Common::ContainsStr(source, "Coherent") == expected,
              "buffer descriptor coherence did not follow GLC/DLC and title option");
        Check(Common::ContainsStr(source, "Volatile") == expected,
              "buffer load volatility did not follow GLC/DLC and title option");
        if (expected) {
          bool refused = false;
          try {
            RecoverShaderCompile(true, [&] { IR::EnterTableMode(result.program); });
          } catch (const ShaderCompileRejected &failure) {
            refused = failure.message.find("coherent descriptor path") != std::string::npos;
          }
          Check(refused, "table mode silently discarded cache-bypassing buffer semantics");
        }
      }
    }
  }
  // Actual polling load from 383103dcf737184e PC 0x618: preserve both bits.
  const uint32_t poll[] = {0xe030e000u, 0x8000100du};
  Decoder::Instruction decoded;
  Decoder::DecodeInstruction(poll, 0, decoded);
  Check(decoded.opcode == Decoder::Opcode::BUFFER_LOAD_DWORD && decoded.glc && decoded.dlc,
        "captured Wolverine polling load lost GLC/DLC");
  const uint32_t typed_dlc[] = {EncodeMtbuf0(0, 4, 4) | (1u << 15), EncodeMtbuf1(0, 0, 0, 1)};
  Decoder::DecodeInstruction(typed_dlc, 0, decoded);
  Check(decoded.family == Decoder::Family::MTBUF && decoded.dlc && !decoded.glc,
        "typed buffer decoder lost DLC or changed GLC");

  // GLC on an atomic still means return the old value; DLC alone must not
  // turn it into a returning atomic. Coherent publication covers both aliases.
  codegen.coherent_buffers = true;
  SetCodegenOptions(codegen);
  const uint32_t atomic64[] = {
    0xe1680018u | (1u << 14), 0x80000000u, // BUFFER_ATOMIC_OR_X2 GLC
    EncodeMubuf0(0x1d, 32, false), EncodeMubuf1(0, 0, 0), EncodeSopp(0x01),
  };
  auto atomic = RecompileForTest(atomic64, MakeCompileOptions(ShaderType::Compute));
  CheckSpirvBinaryValidates(atomic.spirv);
  Check(CountSourceOccurrences(DisassembleSpirvBinary(atomic.spirv), "Coherent") == 2,
        "coherent publication did not cover both u32/u64 buffer aliases");
  const uint32_t dlc_atomic[] = {
    EncodeMubuf0(0x32, 0, true, false) | (1u << 15), EncodeMubuf1(0, 0, 1), EncodeSopp(0x01),
  };
  Decoder::DecodeInstruction(dlc_atomic, 0, decoded);
  Check(!decoded.glc && decoded.dlc, "DLC changed the atomic return flag");
}

void TestWolverineRtLoopGuard() {
  using namespace ShaderRecompiler;
  RestoreStabilityCodegen restore;
  Check(Wolverine::LoopGuardEnabled("PPSA03671", nullptr) &&
        !Wolverine::LoopGuardEnabled("PPSA03671", "0") &&
        !Wolverine::LoopGuardEnabled("PPSA01342", "1") &&
        !Wolverine::LoopGuardEnabled("PPSA01405", nullptr) &&
        !Wolverine::LoopGuardEnabled("CUSA03173", "1"),
        "Wolverine loop guard title gate or opt-out failed");
  // s0=0; while(s0<4096) ++s0; end. With the selected RT hash, execution
  // must take its existing exit edge at decision 2048; other hashes finish the full loop.
  const uint32_t code[] = {0xbe800380u, 0xbf0aff00u, 4096u, 0xbf840002u,
                           0x80008100u, 0xbf82fffbu, 0xbf810000u};
  auto codegen = restore.saved;
  codegen.wolverine_loop_guard = false;
  SetCodegenOptions(codegen);
  const auto old_key = RtCodegenFingerprint();
  codegen.wolverine_loop_guard = true;
  SetCodegenOptions(codegen);
  Check(old_key != RtCodegenFingerprint(), "RT loop guard did not change the shader cache key");
  for (const bool enabled : {false, true}) {
    codegen.wolverine_loop_guard = enabled;
    SetCodegenOptions(codegen);
    for (const uint64_t hash : {0xdc76e1223a9bf673ULL, 0x383103dcf737184eULL}) {
      for (const bool dispatcher : {false, true}) {
        auto options = MakeCompileOptions(ShaderType::Compute);
        options.wave_size = 32;
        options.shader_hash = hash;
        auto translated = TranslateProgram(code, options);
        translated.program.dispatcher_fallback = dispatcher;
        auto compiled = CompileProgram(std::move(translated), options, {}, 0);
        CheckSpirvBinaryValidates(compiled.spirv);
        const bool expected = enabled && hash == 0xdc76e1223a9bf673ULL;
        Check(Common::ContainsStr(DisassembleSpirvBinary(compiled.spirv), "diagnostic_loop_budget") == expected,
              "RT loop guard selected the wrong shader or ignored its opt-out");
        if (!dispatcher)
          Check(ExecuteLoopGuardFixture(compiled.spirv) == (expected ? 2048u : 0u),
                "RT loop guard failed to terminate at its invocation budget");
      }
    }
  }
}
