#include "graphics/shader/recompiler/ir/Serialize.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ir/passes/SrtDescriptorProbe.h"
#include "SrtWalkerTestMemory.h"
#include "common/readbackProfile.h"

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>

namespace {

void Check(bool value, const char *text) {
  if (!value) {
    std::fprintf(stderr, "ResourceMaterializationTests: failed: %s\n", text);
    std::abort();
  }
}

// Every extracted plan below is the one a restarted game loads from the shader cache: exactly
// its bytes load, and the loaded plan saves back to them.
Libs::Graphics::ShaderRecompiler::IR::ResourcePlan
Extract(const Libs::Graphics::ShaderRecompiler::IR::Program &program) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  const auto bytes = SaveResourcePlan(ExtractResourcePlan(program));
  auto extended = bytes;
  extended.push_back(0u);
  ResourcePlan loaded, truncated, padded;
  Check(LoadResourcePlan(bytes, loaded) && SaveResourcePlan(loaded) == bytes &&
            !LoadResourcePlan(std::span(bytes).first(bytes.size() - 1u), truncated) &&
            !LoadResourcePlan(extended, padded),
        "resource plan did not round-trip through its exact cache bytes");
  return loaded;
}

bool RejectSpecializationRead(void *userdata, uint64_t, std::span<uint32_t>) {
  ++*static_cast<uint32_t *>(userdata);
  return false;
}

Libs::Graphics::ShaderRecompiler::IR::Block &
AddValueBlock(Libs::Graphics::ShaderRecompiler::IR::Program &program) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto block = std::make_unique<Block>();
  auto *result = block.get();
  program.blocks.push_back(result);
  result->id = static_cast<uint32_t>(program.blocks.size() - 1u);
  program.block_storage.push_back(std::move(block));
  return *result;
}

// Lists one extra source per block and walks the plan: each source's activity is its block's reachability.
std::vector<uint8_t> ReachedBlocks(Libs::Graphics::ShaderRecompiler::IR::ResourcePlan &plan,
                                   const Libs::Graphics::ShaderRecompiler::IR::SrtRuntime &runtime) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  const auto first = static_cast<uint32_t>(plan.descriptor_sources.size());
  for (uint32_t block = 0; block < plan.control_flow.size(); ++block) {
    plan.descriptor_sources.emplace_back();
    plan.control_flow[block].sources.push_back(first + block);
  }
  CompileSrtWalker(plan);
  std::vector<uint32_t> flat;
  Check(WalkSrt(plan, runtime, flat, nullptr), "reachability walk failed");
  std::vector<uint8_t> reached;
  for (uint32_t block = 0; block < plan.control_flow.size(); ++block)
    reached.push_back(WalkedActive(plan, first + block));
  return reached;
}

Libs::Graphics::ShaderRecompiler::IR::Program SrtProgram(uint64_t address) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &value_block = AddValueBlock(program);

  MemoryInfo memory;
  memory.kind = ResourceKind::ScalarAddress;
  memory.planning_only = true;
  program.memory_info.push_back(memory);
  const auto low = Value(static_cast<uint32_t>(address));
  const auto high = Value(static_cast<uint32_t>(address >> 32u));
  auto &handle =
      value_block.AppendNewInst(ValueOpcode::GetAddressResource, {low, high});
  auto &raw = value_block.AppendNewInst(
      ValueOpcode::LoadAddressU32,
      {Value(&handle), Value(0u), Value(0u), Value(true)});
  raw.SetFlags(MemoryFlags{.index = 0, .pc = 0x40});
  program.srt_reads.push_back({Value(&raw), 0});

  auto &srt = value_block.AppendNewInst(ValueOpcode::GetSrtResource);
  auto &flat = value_block.AppendNewInst(ValueOpcode::ReadConst,
                                         {Value(&srt), Value(0u)});
  DescriptorSource source;
  source.dwords[0] = Value(&flat);
  source.dwords[1] = Value(0u);
  source.dword_count = 2;
  program.descriptor_sources.push_back(source);
  return program;
}

void TestSrtDescriptorProbe() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  using namespace SrtDescriptorProbe;
  Budget budget;
  Check(!budget.Claim(Targets[0], 0, false, true) &&
            !budget.Claim(Targets[0], 0, true, false) &&
            !budget.Claim(123, 0, true, true),
        "descriptor probe consumed a disabled/non-target sample");
  Check(budget.Claim(Targets[0], 0, true, true) == 1 &&
            !budget.Claim(Targets[0], 0, true, true) &&
            budget.Claim(Targets[1], 0, true, true) == 1,
        "descriptor probe did not separate targets or coalesce a profile window");
  for (uint32_t i = 1; i < MaxSamples; ++i)
    Check(budget.Claim(Targets[0], i, true, true) == i + 1,
          "descriptor probe lost a later window");
  Check(!budget.Claim(Targets[0], 99, true, true), "descriptor probe exceeded its process budget");

  // Address 1 must remain opaque metadata. Reporting cannot evaluate even one
  // guest load; this plan deliberately points to inaccessible guest memory.
  auto program = SrtProgram(1);
  program.shader_hash = Targets[0];
  auto plan = Extract(program);
  ResourceSnapshot snapshot;
  snapshot.flattened_srt = {0x12345678};
  const auto before = SaveResourcePlan(plan);
  const auto report = Report(plan, snapshot, 7, 1);
  Check(report.find("\"hash\":\"0xfbb1e69b2c2c0abd\"") != std::string::npos &&
            report.find("LoadAddressU32") != std::string::npos &&
            report.find("\"planning_only\":true") != std::string::npos &&
            report.find("305419896") != std::string::npos &&
            report.find("\"truncated\":false") != std::string::npos &&
            SaveResourcePlan(plan) == before && snapshot.flattened_srt[0] == 0x12345678,
        "descriptor probe changed its inputs or lost the mandatory read graph");

  // Cyclic SSA and omitted nodes must not cause recursive traversal, mutation,
  // or an apparently complete dependency graph.
  ResourcePlan cycle;
  auto& phi = cycle.value_storage.emplace_back(ValueOpcode::Phi);
  phi.SetFlags(Type::U32);
  phi.AddPhiOperand(nullptr, Value(&phi));
  const auto cyclic_report = Report(cycle, {}, 1, 1);
  Check(cyclic_report.find("\"node\":0") != std::string::npos &&
            cyclic_report.find("\"truncated\":false") != std::string::npos,
        "descriptor probe failed to retain a cyclic reference");
  while (cycle.value_storage.size() <= MaxNodes) cycle.value_storage.emplace_back(ValueOpcode::UndefU32);
  cycle.srt_reads.push_back({Value(&cycle.value_storage.back()), 0});
  snapshot.flattened_srt.resize(MaxEntries + 1);
  const auto capped_report = Report(cycle, snapshot, 2, 2);
  Check(capped_report.find("\"node\":null,\"unlisted\":true") != std::string::npos &&
            capped_report.find("\"truncated\":true") != std::string::npos &&
            capped_report.size() < 512 * 1024,
        "descriptor probe hid truncation or exceeded bounded output");
  std::puts("SrtDescriptorProbe: all cases passed");
}

Libs::Graphics::ShaderRecompiler::IR::ResourcePlan UnbasedFlatPlan() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  AddValueBlock(program);
  program.info.uses_dma = true;
  return Extract(program);
}

Libs::Graphics::ShaderRecompiler::IR::ResourcePlan UserDataBufferPlan() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &value_block = AddValueBlock(program);

  auto &user_data = value_block.AppendNewInst(
      ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(0))});
  DescriptorSource source;
  source.dwords[0] = Value(&user_data);
  source.dwords[1] = Value(0u);
  source.dwords[2] = Value(0u);
  source.dwords[3] = Value(0u);
  source.dword_count = 4;
  program.descriptor_sources.push_back(source);
  program.info.buffers.push_back({.source = 0});
  return Extract(program);
}

Libs::Graphics::ShaderRecompiler::IR::Program MixedSamplerProgram() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &block = AddValueBlock(program);

  const auto AddSource = [&program](uint32_t dword_count) {
    DescriptorSource source;
    source.dword_count = dword_count;
    for (uint32_t i = 0; i < dword_count; i++) {
      source.dwords[i] = Value(0u);
    }
    program.descriptor_sources.push_back(source);
    return static_cast<uint32_t>(program.descriptor_sources.size() - 1u);
  };

  const auto image0 = AddSource(8);
  const auto image1 = AddSource(8);
  const auto sampler0 = AddSource(4);
  const auto sampler1 = AddSource(4);
  program.descriptor_sources[image1].dwords[0] = Value(1u);
  program.descriptor_sources[image1].dwords[1] = Value(static_cast<uint32_t>(
      Libs::Graphics::Prospero::BufferFormat::k11_11_10UInt) << 20u);
  program.descriptor_sources[image1].dwords[3] = Value(static_cast<uint32_t>(
      Libs::Graphics::Prospero::ImageType::kColor2D) << 28u);
  for (uint32_t index = 0; index < 2; ++index) {
    auto &value = block.AppendNewInst(
        ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(index))});
    program.descriptor_sources[sampler0 + index].dwords[0] = Value(&value);
  }
  program.info.images.push_back(
      {.source = image0,
       .resource_class = ImageResourceClass::Sampled,
       .numeric_class = Libs::Graphics::Prospero::TextureNumericClass::Float,
       .dimension =
           Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension::Dim2D});
  program.info.images.push_back(
      {.source = image1,
       .resource_class = ImageResourceClass::Sampled,
       .numeric_class = Libs::Graphics::Prospero::TextureNumericClass::Float,
       .dimension =
           Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension::Dim2D});
  program.info.samplers.push_back({.source = sampler0});
  program.info.samplers.push_back({.source = sampler1});
  program.info.sampled_pairs.push_back({.image = 0, .sampler = 0});
  program.info.sampled_pairs.push_back({.image = 0, .sampler = 1});
  program.info.sampled_pairs.push_back({.image = 1, .sampler = 1});
  return program;
}

void TestWalkerLoadsBypassSpecializationReader() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  const uint32_t dword = 0x12345678;
  auto plan = Extract(SrtProgram(reinterpret_cast<uint64_t>(&dword)));
  uint32_t specialization_reads = 0;
  const SrtRuntime runtime{.userdata = &specialization_reads,
                           .read_specialization_memory =
                               RejectSpecializationRead};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization),
        "mapped SRT stage materialization failed");
  Check(specialization_reads == 0,
        "walker load used the specialization reader");
  Check(snapshot.flattened_srt.size() == 1 &&
            snapshot.flattened_srt[0] == dword,
        "walker did not load the mapped SRT DWORD");
}

void TestIdentityResolutionPreservesTerminalValue() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  const std::array terminals{Value{}, Value(true), Value(uint32_t{0xdeadbeef}),
      Value(uint64_t{0x123456789abcdef0}),
      Value::F32(std::bit_cast<float>(uint32_t{0x7fc01234})), Value(ScalarReg(2))};
  for (const auto terminal : terminals) {
    Program program;
    auto &block = AddValueBlock(program);
    auto value = terminal;
    Check(value.Resolve() == terminal, "unwrapped terminal value changed");
    for (uint32_t depth = 0; depth < 4096; ++depth) {
      value = Value(&block.AppendNewInst(ValueOpcode::Identity, {value}));
    }
    auto *original = value.TryInstruction();
    Check(value.Resolve() == terminal && value.TryInstruction() == original &&
              original->GetOpcode() == ValueOpcode::Identity,
          "identity resolution changed terminal bits or mutated the original graph");
  }
  Program program;
  auto &block = AddValueBlock(program);
  auto &terminal = block.AppendNewInst(ValueOpcode::GetUserData, {Value(ScalarReg(0))});
  auto value = Value(&terminal);
  for (uint32_t depth = 0; depth < 4096; ++depth)
    value = Value(&block.AppendNewInst(ValueOpcode::Identity, {value}));
  Check(value.Resolve().TryInstruction() == &terminal,
        "identity resolution lost the terminal instruction");
}

void TestNativeNestedSrtRefresh() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  std::array<uint32_t, 3> first{0u, 0x12345678u, 0xabcdef01u};
  std::array<uint32_t, 3> second{0u, 0x87654321u, 0x10fedcbau};
  const auto pointer_words = [](const void *pointer) {
    const auto address = reinterpret_cast<uint64_t>(pointer);
    return std::array<uint32_t, 2>{static_cast<uint32_t>(address) | 3u,
                                  static_cast<uint32_t>(address >> 32u) | 0xabcd0000u};
  };
  auto table = pointer_words(first.data());
  const auto user_data = pointer_words(table.data());
  auto program = SrtProgram(0u);
  program.user_data_base = 4;
  program.user_data_count = 2;
  auto &block = *program.blocks[0];
  const auto emit = [&](ValueOpcode opcode, std::initializer_list<Value> args) {
    return Value(&block.AppendNewInst(opcode, args));
  };
  const auto low = emit(ValueOpcode::GetUserData, {Value(ScalarReg(4))});
  const auto high = emit(ValueOpcode::GetUserData, {Value(ScalarReg(5))});
  const auto root = emit(ValueOpcode::GetAddressResource, {low, high});
  const auto read = [&](Value address, uint32_t offset) {
    const auto value = emit(ValueOpcode::LoadAddressU32,
                            {address, Value(offset), Value(0u), Value(true)});
    value.Instruction()->SetFlags(MemoryFlags{.index = 0});
    program.srt_reads.push_back(
        {value, static_cast<uint32_t>(program.srt_reads.size())});
    return value;
  };
  const auto pointer_low = read(root, 0u);
  const auto pointer_high = read(root, 4u);
  auto *leaf = program.srt_reads[0].value.Instruction();
  leaf->Arg(0).Instruction()->SetArg(0, pointer_low);
  leaf->Arg(0).Instruction()->SetArg(1, pointer_high);
  leaf->SetArg(1, Value(7u));
  read(leaf->Arg(0), 11u);
  auto plan = Extract(program);
  Check(plan.srt_code != nullptr, "nested user-data tree did not compile an x86 walker");
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  const SrtRuntime runtime{.user_data = user_data};
  for (const auto *data : {&first, &second}) {
    table = pointer_words(data->data());
    Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
              snapshot.flattened_srt ==
                  std::vector<uint32_t>{(*data)[1], table[0], table[1], (*data)[2]},
          "native SRT refresh lost pointer masking, alignment, or current values");
  }
  // Identical SGPRs, root/leaf pointers and specialization do not imply identical
  // materialized data. A CPU store can change a table without moving it.
  const auto previous_specialization = specialization;
  second[1] ^= 1u;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            snapshot.flattened_srt ==
                std::vector<uint32_t>{second[1], table[0], table[1], second[2]} &&
            specialization == previous_specialization,
        "same-pointer table DWORD update must refresh data even with unchanged specialization");
  {
    // Unmapped tables and the null pointer they yield read as zero.
    FakeGuestMemory unmapped;
    const std::array<uint32_t, 2> unmapped_table{0x1000u, 0u};
    Check(MaterializeResources(plan, {.user_data = unmapped_table}, snapshot, specialization) &&
              snapshot.flattened_srt == std::vector<uint32_t>(4, 0u),
          "faulting walker loads of a table and a null pointer did not read zero");
  }
  Check(!MaterializeResources(plan, {.user_data = std::span(user_data).first(1)},
                              snapshot, specialization),
        "native SRT walk accepted truncated user data");
  const std::array<uint32_t, 2> fake_user_data{0x1003u, 0xabcd0000u};
  uint32_t reads = 0;
  FakeGuestMemory fake(+[](void *data, uint64_t address, std::span<uint32_t> words) {
        ++*static_cast<uint32_t *>(data);
        switch (address) {
          case 0x1000u: words[0] = 0x2003u; break;
          case 0x1004u: words[0] = 0xabcd0000u; break;
          case 0x2004u: words[0] = 0x1234u; break;
          case 0x2008u: words[0] = 0x5678u; break;
          default: return false;
        }
        return true;
      }, &reads);
  Check(MaterializeResources(plan, {.user_data = fake_user_data}, snapshot, specialization) && reads == 4 &&
            snapshot.flattened_srt ==
                std::vector<uint32_t>{0x1234u, 0x2003u, 0xabcd0000u, 0x5678u},
        "native SRT walk repeated nested pointer reads");
  leaf->SetArg(0, root);
  leaf->SetArg(1, Value(0u));
  program.srt_reads.resize(1);
  for (const int32_t offset : {-1, 7}) {
    program.memory_info[0].offset = static_cast<uint32_t>(offset);
    auto bounded = Extract(program);
    auto base = pointer_words(second.data() + 1);
    Check(bounded.srt_code != nullptr &&
              MaterializeResources(bounded, {.user_data = base}, snapshot, specialization) &&
              snapshot.flattened_srt[0] == second[offset < 0 ? 0 : 2],
          "native SRT applied a signed immediate offset incorrectly");
    base = offset < 0 ? std::array<uint32_t, 2>{0u, 0u}
                      : std::array<uint32_t, 2>{0xfffffffcu, 0xffffu};
    Check(!MaterializeResources(bounded, {.user_data = base}, snapshot, specialization),
          "native SRT accepted address underflow or 48-bit overflow");
  }
}

void TestIntegerRuntimeValueFollowsSrtReads() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan = SrtProgram(0x10000);
  const auto root = plan.descriptor_sources.front().dwords[0];
  Check(ValidateRuntimeValue(plan, root, RuntimeValueType::Integer),
        "integer SRT read was rejected");

  Block values;
  auto &comparison = values.AppendNewInst(ValueOpcode::FPOrdLessThanEqual32,
                                          {Value::F32(1.f), Value::F32(0.f)});
  auto &selection = values.AppendNewInst(
      ValueOpcode::SelectU32, {Value(&comparison), Value(1u), Value(0u)});
  plan.srt_reads[0].value = Value(&selection);
  Check(ValidateRuntimeValue(plan, root),
        "ordinary SRT validation rejected a floating-point dependency");
  Check(!ValidateRuntimeValue(plan, root, RuntimeValueType::Integer),
        "integer SRT validation missed a hidden floating-point dependency");

  auto &first =
      values.AppendNewInst(ValueOpcode::ReadFirstLane, {root, Value(true)});
  Check(!ValidateRuntimeValue(plan, Value(&first), RuntimeValueType::Integer),
        "read-first-lane lost integer-only SRT validation");

  auto &active = values.AppendNewInst(ValueOpcode::ReadFirstLane,
                                      {Value(&selection), Value(&comparison)});
  Check(!ValidateRuntimeValue(plan, Value(&active), RuntimeValueType::Integer),
        "floating-point execution mask was accepted as integer-only");

  auto &lane = values.AppendNewInst(
      ValueOpcode::GetBuiltin,
      {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)),
       Value(0u)});
  auto &mask =
      values.AppendNewInst(ValueOpcode::INotEqual32, {Value(&lane), Value(0u)});
  selection.SetArg(0, Value(&mask));
  active.SetArg(1, Value(&mask));
  Check(ValidateRuntimeValue(plan, Value(&active), RuntimeValueType::Integer),
        "nonuniform integer execution mask was rejected");
  auto &float_value =
      values.AppendNewInst(ValueOpcode::BitCastU32F32, {Value::F32(1.f)});
  selection.SetArg(2, Value(&float_value));
  Check(!ValidateRuntimeValue(plan, Value(&active), RuntimeValueType::Integer),
        "floating-point inactive arm was accepted as integer-only");

  plan.srt_reads[0].value = Value(&first);
  Check(!ValidateRuntimeValue(plan, root, RuntimeValueType::Integer),
        "cyclic SRT read-first-lane dependency was accepted");
}

void TestSrtAliasesShareReads() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto program = SrtProgram(0x1000u);
  auto &block = *program.blocks[0];
  const auto append_read = [&](uint32_t address) {
    auto &handle = block.AppendNewInst(ValueOpcode::GetAddressResource,
                                       {Value(address), Value(0u)});
    auto &read = block.AppendNewInst(ValueOpcode::LoadAddressU32,
        {Value(&handle), Value(0u), Value(0u), Value(true)});
    // Different reads deliberately share MemoryInfo but have different guest PCs.
    read.SetFlags(MemoryFlags{.index = 0, .pc = address});
    const auto slot = static_cast<uint32_t>(program.srt_reads.size());
    program.srt_reads.push_back({Value(&read), slot});
    auto &srt = block.AppendNewInst(ValueOpcode::GetSrtResource);
    return Value(&block.AppendNewInst(ValueOpcode::ReadConst,
                                      {Value(&srt), Value(slot)}));
  };
  const auto ordinary = append_read(0x2000u);
  const auto pointer = append_read(0x3000u);
  program.srt_reads[0].value.Instruction()->Arg(0).Instruction()->SetArg(0, pointer);
  auto &mask = block.AppendNewInst(ValueOpcode::INotEqual32, {ordinary, Value(0u)});
  auto &first = block.AppendNewInst(ValueOpcode::ReadFirstLane,
                                    {Value(9u), Value(&mask)});
  program.descriptor_sources.push_back({.dwords = {Value(&first), Value(0u)},
                                       .dword_count = 2});
  DescriptorSource indirect;
  indirect.indirect_descriptor.emplace(DescriptorSource::IndirectDescriptor{}).sources = {0, 1};
  program.descriptor_sources.push_back(indirect);
  auto plan = Extract(program);
  for (const auto &inst : plan.value_storage) {
    Check(inst.GetOpcode() != ValueOpcode::ReadConst &&
              inst.GetOpcode() != ValueOpcode::GetSrtResource,
          "retained plan still contains flattened-read aliases");
  }
  Check(plan.descriptor_sources[0].dwords[0] == plan.srt_reads[0].value,
        "alias normalization lost read identity");
  struct Reads { uint32_t count = 0; bool dirty = false; } reads;
  FakeGuestMemory fake(+[](void *data, uint64_t address, std::span<uint32_t> words) {
        auto &reads = *static_cast<Reads *>(data);
        ++reads.count;
        if (address == 0x1000u) words[0] = 0x1111u;
        else if (address == 0x2000u) words[0] = 0x2222u;
        else if (address == 0x3000u && !reads.dirty) words[0] = 0x1000u;
        else return false;
        return true;
      }, &reads);
  std::vector<uint32_t> flat;
  Check(WalkSrt(plan, {}, flat, nullptr) && flat == std::vector<uint32_t>{0x1111u, 0x2222u, 0x1000u} &&
            reads.count == 3,
        "normalized aliases changed nested reads or repeated a shared read");
  // An unreadable pointer reads as zero, and so does the null table it yields.
  reads = {.dirty = true};
  Check(WalkSrt(plan, {}, flat, nullptr) && flat == std::vector<uint32_t>{0u, 0x2222u, 0u} &&
            reads.count == 3,
        "unreadable pointer did not read as zero");
}

void TestUniformVectorDescriptorRead() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = program.resource_tracking_complete = true;
  auto &block = AddValueBlock(program);
  auto &handle = block.AppendNewInst(ValueOpcode::GetBufferResource,
      {Value(0x1000u), Value(4u << 16u), Value(1u), Value(0x16204u)});
  program.memory_info.push_back({.kind = ResourceKind::Buffer});
  auto &count = block.AppendNewInst(ValueOpcode::LoadBufferU32,
      {Value(&handle), Value(0u), Value(0u), Value(0u), Value(true)});
  count.SetFlags(MemoryFlags{.index = 0});
  // The captured indirect kernel shares one read across sibling scalar lane reads,
  // enclosed by a different EXEC mask. Its resource plan must share that read too.
  auto &lane = block.AppendNewInst(ValueOpcode::GetBuiltin,
      {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)), Value(0u)});
  auto &active = block.AppendNewInst(ValueOpcode::ULessThan32, {Value(&lane), Value(32u)});
  auto &first = block.AppendNewInst(ValueOpcode::ReadFirstLane, {Value(&count), Value(true)});
  auto &next = block.AppendNewInst(ValueOpcode::IAdd32, {Value(&count), Value(1u)});
  auto &second = block.AppendNewInst(ValueOpcode::ReadFirstLane, {Value(&next), Value(true)});
  auto &sum = block.AppendNewInst(ValueOpcode::IAdd32, {Value(&first), Value(&second)});
  auto &is_small = block.AppendNewInst(ValueOpcode::ULessThanEqual32, {Value(&count), Value(72u)});
  auto &selected = block.AppendNewInst(ValueOpcode::SelectU32, {Value(&is_small), Value(&sum), Value(&count)});
  auto &masked = block.AppendNewInst(ValueOpcode::SelectU32, {Value(&active), Value(&selected), Value(0u)});
  auto &records = block.AppendNewInst(ValueOpcode::ReadFirstLane, {Value(&masked), Value(&active)});
  DescriptorSource source;
  source.dword_count = 4;
  source.dwords = {Value(0x2000u), Value(4u << 16u), Value(&records), Value(0x16204u)};
  program.descriptor_sources.push_back(source);
  program.info.buffers.push_back({.source = 0, .written = true});
  Check(ValidateRuntimeValue(program, Value(&count)), "uniform DWORD count was rejected");
  struct Reads { uint32_t value = 72; uint32_t count = 0; bool readable = true; } reads;
  FakeGuestMemory fake([](void *data, uint64_t address, std::span<uint32_t> words) {
        auto &reads = *static_cast<Reads *>(data);
        ++reads.count;
        if (!reads.readable || address != 0x1000u || words.size() != 1) return false;
        words[0] = reads.value;
        return true;
      }, &reads);
  const SrtRuntime runtime;
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  for (const bool written : {false, true}) {
    program.info.buffers[0].written = written;
    auto plan = Extract(program);
    Check(plan.control_flow.empty() && plan.capture_specialization_reads,
          "vector descriptor read depended on incidental control-flow capture");
    reads = {};
    Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
              snapshot.buffers[0].dwords[2] == 145 && reads.count == 1 &&
              snapshot.specialization_reads ==
                  std::vector<std::pair<uint64_t, uint64_t>>{{0x1000u, 4u}},
          "vector descriptor input was not read and captured exactly once");
    // An unreadable input reads as zero, so the count selects 0 + 1.
    reads = {.readable = false};
    Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
              snapshot.buffers[0].dwords[2] == 1 && reads.count == 1,
          "unreadable vector descriptor input did not read as zero");
  }
  // A read guarded by its EXEC mask depends on that mask, so it cannot reuse the unmasked read.
  auto &guarded = block.AppendNewInst(ValueOpcode::LoadBufferU32,
      {Value(&handle), Value(0u), Value(0u), Value(0u), Value(&active)});
  guarded.SetFlags(MemoryFlags{.index = 0});
  auto &guarded_lane = block.AppendNewInst(ValueOpcode::ReadFirstLane, {Value(&guarded), Value(&active)});
  program.descriptor_sources.push_back({.dwords = {Value(&guarded_lane)}, .dword_count = 1});
  {
    auto plan = Extract(program);
    uint32_t value = 0;
    reads = {};
    Check(EvaluateSrtValue(plan, runtime, plan.descriptor_sources.back().dwords[0], value) &&
              value == 72u && reads.count == 1,
          "EXEC-guarded vector read lost its mask");
  }
  count.SetArg(4, Value(false));
  auto &inactive = block.AppendNewInst(ValueOpcode::ReadFirstLane, {Value(&count), Value(false)});
  program.descriptor_sources.push_back({.dwords = {Value(&inactive)}, .dword_count = 1});
  reads.count = 0;
  uint32_t result = 99;
  {
    auto plan = Extract(program);
    Check(EvaluateSrtValue(plan, runtime, plan.descriptor_sources.back().dwords[0], result) &&
              result == 0 && reads.count == 0,
          "literal false EXEC read vector memory");
  }
  count.SetArg(4, Value(true));
  handle.SetArg(3, Value(0x204u));
  program.descriptor_sources.back().dwords[0] = Value(&count);
  {
    auto plan = Extract(program);
    Check(EvaluateSrtValue(plan, runtime, plan.descriptor_sources.back().dwords[0], result) &&
              result == 0 && reads.count == 0,
          "invalid vector buffer format read memory");
  }
  handle.SetArg(3, Value(0x16204u));
  count.SetArg(1, Value(&lane));
  Check(!ValidateRuntimeValue(program, Value(&count)),
        "varying vector address was treated as a uniform descriptor read");
}

void TestExactReciprocalDescriptorArithmetic() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  auto &block = AddValueBlock(program);
  for (const float divisor : {64.f, 128.f, 256.f, 512.f,
                              std::numeric_limits<float>::min(),
                              std::bit_cast<float>(253u << 23u)}) {
    auto &reciprocal = block.AppendNewInst(ValueOpcode::FPRecipIFlag32, {Value::F32(divisor)});
    uint32_t result = 0;
    Check(EvaluateSrtValue(program, {}, Value(&reciprocal), result) &&
              result == std::bit_cast<uint32_t>(1.f / divisor),
          "power-of-two reciprocal was not exact");
  }
  for (const float divisor : {0.f, 3.f, -64.f, std::numeric_limits<float>::infinity(),
                              std::numeric_limits<float>::denorm_min(),
                              std::bit_cast<float>(254u << 23u)}) {
    auto &reciprocal = block.AppendNewInst(ValueOpcode::FPRecipIFlag32, {Value::F32(divisor)});
    uint32_t result = 0;
    Check(!EvaluateSrtValue(program, {}, Value(&reciprocal), result),
          "unsupported reciprocal rounding or exceptional input was accepted");
  }
}

void TestUnbasedFlatCacheHitMaterializes() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan = UnbasedFlatPlan();
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, {}, snapshot, specialization),
        "unbased FLAT stage materialization failed");
  Check(snapshot.buffers.empty() && snapshot.images.empty(),
        "unbased FLAT plan produced unexpected descriptors");
}

void TestWrittenDescriptorPredicateReads(bool memory_condition) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.user_data_count = 1;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &block = AddValueBlock(program);
  program.memory_info.push_back({.kind = ResourceKind::ScalarAddress});
  auto &handle = block.AppendNewInst(ValueOpcode::GetAddressResource,
                                     {Value(0x1000u), Value(0u)});
  auto &offset = block.AppendNewInst(ValueOpcode::GetUserData,
                                     {Value(static_cast<ScalarReg>(0))});
  auto &read = block.AppendNewInst(ValueOpcode::LoadAddressU32,
      {Value(&handle), Value(&offset), Value(0u), Value(false)});
  read.SetFlags(MemoryFlags{.index = 0});
  DescriptorSource source;
  source.dwords = {Value(&read), Value(0u), Value(4u), Value(0u)};
  source.dword_count = 4;
  program.descriptor_sources.push_back(source);
  program.info.buffers.push_back({.source = 0, .written = true});
  // Only memory-derived host decisions capture their descriptor reads.
  auto &condition = block.AppendNewInst(ValueOpcode::IEqual32,
      {memory_condition ? Value(&read) : Value(&offset),
       Value(memory_condition ? 0x8000u : 4u)});
  auto &store_block = AddValueBlock(program);
  AddValueBlock(program);
  program.blocks[0]->condition = Value(&condition);
  program.blocks[0]->terminator.kind =
      Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind::ConditionalBranch;
  program.blocks[0]->terminator.true_block = program.blocks[1];
  program.blocks[0]->terminator.false_block = program.blocks[2];
  program.blocks[1]->id = 1;
  program.blocks[1]->terminator.kind =
      Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind::Return;
  program.blocks[2]->id = 2;
  program.blocks[2]->terminator.kind =
      Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind::Return;
  program.memory_info.push_back({.kind = ResourceKind::Buffer, .resource = 0});
  auto &output = store_block.AppendNewInst(ValueOpcode::GetBufferResource,
      {source.dwords[0], source.dwords[1], source.dwords[2], source.dwords[3]});
  store_block.AppendNewInst(ValueOpcode::StoreBufferU32,
      {Value(&output), Value(0u), Value(0u), Value(0u), Value(1u), Value(true)})
      .SetFlags(MemoryFlags{.index = 1});
  auto plan = Extract(program);
  Check(plan.capture_specialization_reads == memory_condition && !plan.control_flow.empty(),
        "conditional writable descriptor used the wrong memory dependency policy");
  struct Reads { uint32_t count = 0; bool readable = false; } reads;
  const std::array<uint32_t, 1> user_data{4u};
  FakeGuestMemory fake(+[](void *data, uint64_t address, std::span<uint32_t> words) {
        auto &reads = *static_cast<Reads *>(data);
        ++reads.count;
        if (!reads.readable || address != 0x1004u) return false;
        words[0] = 0x8000u;
        return true;
      }, &reads);
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  for (const bool readable : {false, true}) {
    reads = {.readable = readable};
    // The predicate and the descriptor share one read. An unreadable one is zero, which
    // makes a memory predicate skip the store block and leave its descriptor inactive.
    Check(MaterializeResources(plan, {.user_data = user_data}, snapshot, specialization) &&
              reads.count == 1,
          "writable descriptor repeated or skipped its shared read");
    Check(snapshot.buffers[0].dwords[0] == (readable ? 0x8000u : 0u) &&
              snapshot.specialization_reads == (memory_condition
                  ? std::vector<std::pair<uint64_t, uint64_t>>{{0x1004u, 4u}}
                  : std::vector<std::pair<uint64_t, uint64_t>>{}),
          "writable descriptor lost its address or captured an immutable predicate");
  }
}

void TestWritableBufferControlProjection(bool adaptive = true) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  using Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind;
  for (bool scalar : {false, true}) for (bool written : {false, true}) for (bool descriptor_dependency : {false, true}) {
    Program program;
    program.shader_hash = 0x12345678;
    program.stage = Libs::Graphics::ShaderType::Compute;
    program.srt_plan_complete = program.resource_tracking_complete = true;
    auto &entry = AddValueBlock(program);
    auto &yes = AddValueBlock(program);
    auto &no = AddValueBlock(program);
    program.memory_info.push_back({.kind = scalar ? ResourceKind::ScalarBuffer : ResourceKind::Buffer, .resource = 0});
    auto &handle = entry.AppendNewInst(ValueOpcode::GetBufferResource,
        {Value(0x1000u), Value(4u << 16u), Value(4u), Value(0x16204u)});
    auto &read = scalar
        ? entry.AppendNewInst(ValueOpcode::ReadConstBuffer, {Value(&handle), Value(0u)})
        : entry.AppendNewInst(ValueOpcode::LoadBufferU32,
            {Value(&handle), Value(0u), Value(0u), Value(0u), Value(true)});
    read.SetFlags(MemoryFlags{.index = 0, .pc = 0x24});
    auto &varying = entry.AppendNewInst(ValueOpcode::UndefU32);
    auto &condition = entry.AppendNewInst(ValueOpcode::ULessThan32,
        {Value(&varying), Value(&read)});
    entry.condition = Value(&condition);
    entry.terminator.kind = TerminatorKind::ConditionalBranch;
    entry.terminator.true_block = &yes;
    entry.terminator.false_block = &no;
    yes.terminator.kind = no.terminator.kind = TerminatorKind::Return;
    for (uint32_t i = 0; i < 3; ++i) {
      DescriptorSource source;
      source.dwords = {Value(0x1000u + i * 0x1000u), Value(4u << 16u), Value(4u), Value(0x16204u)};
      source.dword_count = 4;
      program.descriptor_sources.push_back(source);
      program.info.buffers.push_back({.source = i, .read = true, .written = i != 0 || written});
      if (i == 0) continue;
      auto &block = i == 1 ? yes : no;
      auto &out = block.AppendNewInst(ValueOpcode::GetBufferResource,
          {source.dwords[0], source.dwords[1], source.dwords[2], source.dwords[3]});
      program.memory_info.push_back({.kind = ResourceKind::Buffer, .resource = i});
      block.AppendNewInst(ValueOpcode::StoreBufferU32,
          {Value(&out), Value(0u), Value(0u), Value(0u), Value(1u), Value(true)})
          .SetFlags(MemoryFlags{.index = i});
    }
    if (descriptor_dependency) {
      // The same read also selects an actual descriptor: suppressing control
      // projection must NOT suppress this mandatory descriptor dependency.
      program.descriptor_sources[2].dwords[0] = Value(&read);
    }
    Check(ValidateRuntimeValue(program, Value(&read), RuntimeValueType::Integer) &&
              ValidateRuntimeValue(program, Value(&read), RuntimeValueType::ReadOnlyControlInteger) == !written,
          "writable buffer control classification changed descriptor evaluation");
    auto plan = Extract(program); // Includes save/load round trip.
    struct ReadState {
      uint32_t calls = 0, attributed = 0;
      bool written, descriptor;
      uint32_t word = 0;
      bool train = false;
    };
    ReadState read_state{0, 0, written, descriptor_dependency};
    FakeGuestMemory fake(+[](void *data, uint64_t address, std::span<uint32_t> words) {
      ++static_cast<ReadState *>(data)->calls;
      Check(address == 0x1000u && words.size() == 1, "unexpected control read");
      words[0] = static_cast<ReadState *>(data)->word;
      return true;
    }, &read_state, +[](void* data, uint64_t rip, uint64_t address) {
      auto& state = *static_cast<ReadState*>(data);
      if (state.train) {
        Check(NoteSrtControlReadback(rip) == (!state.written && !state.descriptor),
              "adaptive control suppressed a mandatory descriptor dependency");
      }
      if (!Common::ReadbackProfile::Armed()) return;
      Common::ReadbackProfile::Origin reader;
      Check(DescribeSrtWalkerRead(rip, reader) && reader.hash == 0x12345678 &&
                reader.memory == 0 && reader.resource == 0 && address == 0x1000 &&
                (std::string_view(reader.opcode) == "LoadBufferU32" || std::string_view(reader.opcode) == "ReadConstBuffer"),
            "JIT fault attribution lost its reader or memory instruction");
      Check((reader.uses & 1u) == (state.written ? 0u : 1u) &&
                (reader.uses & 4u) == (state.descriptor ? 4u : 0u),
            "JIT attribution confused optional control and descriptor dependency");
      Check(!DescribeSrtWalkerRead(rip + 1, reader), "non-load RIP accepted by attribution");
      ++state.attributed;
    });
    ResourceSnapshot snapshot;
    ResourceSpecialization specialization;
    Check(MaterializeResources(plan, {}, snapshot, specialization), "RW control plan failed");
    Check(WalkedActive(plan, 1) == written && WalkedActive(plan, 2),
          "RW predicate pruned a GPU branch or read-only pruning was lost");
    Check(read_state.calls == ((!written || descriptor_dependency) ? 1u : 0u),
          "optional RW control read survived or mandatory descriptor read was hidden");
    // Scalar descriptor reads alone do not enable specialization capture (see
    // TestWrittenDescriptorPredicateReads). A surviving memory-derived control
    // predicate does; LoadBufferU32 also enables capture when cloned as a descriptor.
    const bool captured = !written || (!scalar && descriptor_dependency);
    Check(snapshot.specialization_reads == (captured
              ? std::vector<std::pair<uint64_t, uint64_t>>{{0x1000u, 4u}}
              : std::vector<std::pair<uint64_t, uint64_t>>{}),
          "RW control projection changed the specialization capture policy");
    Check(!descriptor_dependency || snapshot.buffers[2].dwords[0] == read_state.word,
          "mandatory descriptor did not consume the actual scalar/vector read");
    Check(!Common::ReadbackProfile::Armed() || read_state.attributed == read_state.calls,
          "JIT fault was not attributed after shader plan serialization");
    Check(read.GetOpcode() == (scalar ? ValueOpcode::ReadConstBuffer : ValueOpcode::LoadBufferU32) && read.Parent() == &entry &&
              condition.Arg(1) == Value(&read), "extraction modified the dynamic GPU load");
    if (adaptive) {
      using namespace Common::SubmitProfile;
      auto reference = Extract(program); // Fresh, untrained CPU pruning reference.
      SetExperiment(false);
      read_state.train = true; // Simulated tracked readback; first walk still reads normally.
      Check(MaterializeResources(plan, {}, snapshot, specialization), "adaptive training failed");
      read_state.train = false;
      for (const bool gate : {false, true, false}) {
        SetExperiment(gate); // Promotion is independent of experiment.trigger.
        for (const uint32_t word : {0u, 1u, 0u}) {
          read_state.word = word;
          read_state.calls = 0;
          Check(MaterializeResources(plan, {}, snapshot, specialization), "adaptive refresh failed");
          const bool suppressed = !written && !descriptor_dependency && !ExperimentActive(Experiment::NoWalkerControlGpu);
          Check(WalkedActive(plan, 1) == (written || suppressed || word != 0) && WalkedActive(plan, 2),
                "adaptive control pruned a dynamic GPU branch or depended on the experiment gate");
          const bool required_read = !suppressed && (!written || descriptor_dependency);
          Check(read_state.calls == uint32_t{required_read}, "adaptive read count differs from dependency contract");
          Check(!descriptor_dependency || snapshot.buffers[2].dwords[0] == word,
                "adaptive control suppressed a changing mandatory descriptor");
          // For either value, every resource used by the original GPU condition is
          // still available. The unchanged GPU instruction decides the branch.
          const auto gpu_branch = word != 0 ? 1u : 2u;
          Check(WalkedActive(plan, gpu_branch), "changed GPU condition selected a pruned resource");
          ResourceSnapshot cpu_snapshot;
          ResourceSpecialization cpu_specialization;
          Check(MaterializeResources(reference, {}, cpu_snapshot, cpu_specialization) &&
                    WalkedActive(reference, gpu_branch) &&
                    cpu_snapshot.buffers[gpu_branch] == snapshot.buffers[gpu_branch],
                "promoted control changed the resource used by the CPU reference branch");
        }
      }
      SetExperiment(false);
      const auto saved = SaveResourcePlan(plan);
      ResourcePlan reloaded;
      Check(LoadResourcePlan(saved, reloaded) &&
                std::ranges::all_of(reloaded.walker_control_disabled, [](uint8_t v) { return v == 0; }) &&
                !reloaded.walker_control_rejected && reloaded.walker_control_history.learned_sites == 0 &&
                reloaded.walker_control_history.retry_remaining == 0,
            "adaptive history persisted in the shader cache");
      if (!written && !descriptor_dependency) {
        // The pruned arm has a descriptor that is initially unknown, but becomes
        // materializable when runtime user data changes. Do NOT recompile or
        // reload the plan when repairing it: that would mask a permanent blacklist.
        auto& available = plan.value_storage.emplace_back(ValueOpcode::GetUserData);
        available.SetArg(0, Value(ScalarReg(0)));
        auto& condition = plan.value_storage.emplace_back(ValueOpcode::INotEqual32);
        condition.SetArg(0, Value(&available)); condition.SetArg(1, Value(0u));
        auto& missing = plan.value_storage.emplace_back(ValueOpcode::UndefU32);
        auto& selected = plan.value_storage.emplace_back(ValueOpcode::SelectU32);
        selected.SetArg(0, Value(&condition)); selected.SetArg(1, Value(0x2000u));
        selected.SetArg(2, Value(&missing));
        plan.descriptor_sources[1].dwords[0] = Value(&selected);
        plan.user_data_count = 1;
        CompileSrtWalker(plan);
        const auto* jit = plan.srt_code.get();
        std::array<uint32_t, 1> data {0};
        const SrtRuntime runtime {.user_data = data};
        read_state.word = 0;
        SetExperiment(false);
        read_state.train = true;
        const bool was_recording = Recording();
        SetRecording(false); // Simulate learning during load, before measurements.
        Check(MaterializeResources(plan, runtime, snapshot, specialization), "fallback training failed");
        Check(MaterializeResources(plan, runtime, snapshot, specialization) && plan.walker_control_rejected &&
                  !WalkedActive(plan, 1) && !snapshot.specialization_reads.empty(),
              "unsupported newly active descriptor did not fall back to original pruning");
        Check(plan.walker_control_history.learned_sites == 1,
              "fallback forgot the learned site or counted it twice");
        const auto count = [](Extra extra) {
          return Common::SubmitProfile::Detail::window.extra[static_cast<size_t>(extra)].load();
        };
        Reset(); SetRecording(true);
        data[0] = 1; // Same JIT and addresses; now the expanded descriptor is valid.
        constexpr auto interval = ResourcePlan::WalkerControlHistory::RetryInterval;
        for (uint32_t call = 0; call < interval; ++call) {
          // The fake readback observer continues to learn during fallback.
          Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
                    plan.walker_control_rejected && !WalkedActive(plan, 1) &&
                    plan.walker_control_history.learned_sites == 1,
                "cooldown stopped using original pruning or lost readback learning");
        }
        if (Recording()) {
          Check(count(Extra::WalkerControlPlans) == 1 && count(Extra::WalkerControlLearnedSites) == 1 &&
                    count(Extra::WalkerControlFallbackPlans) == 1 && count(Extra::WalkerControlPromotions) == 0 &&
                    count(Extra::WalkerControlRetries) == 0 && count(Extra::WalkerControlWalks) == 0,
                "fallback state learned before profiling was missing or counted per call");
        }
        Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
                  !plan.walker_control_rejected && WalkedActive(plan, 1) && WalkedActive(plan, 2) &&
                  snapshot.buffers[1].dwords[0] == 0x2000u && snapshot.specialization_reads.empty() &&
                  plan.srt_code.get() == jit,
              "transient descriptor failure permanently disabled GPU control");
        if (Recording()) Check(count(Extra::WalkerControlRetries) == 1 &&
                                  count(Extra::WalkerControlRecoveries) == 1 &&
                                  count(Extra::WalkerControlWalks) == 1,
                              "successful control retry not reported");
        // State appears again in the next window without another learning event.
        Reset();
        Check(MaterializeResources(plan, runtime, snapshot, specialization), "next window failed");
        if (Recording()) Check(count(Extra::WalkerControlPlans) == 1 &&
                                  count(Extra::WalkerControlLearnedSites) == 1 &&
                                  count(Extra::WalkerControlFallbackPlans) == 0,
                              "learned state disappeared after the profile window reset");
        // A still-invalid descriptor must keep its safe fallback through retries.
        data[0] = 0;
        Check(MaterializeResources(plan, runtime, snapshot, specialization) && plan.walker_control_rejected,
              "second transient failure did not fall back");
        for (uint32_t call = 0; call <= interval; ++call)
          Check(MaterializeResources(plan, runtime, snapshot, specialization) && plan.walker_control_rejected &&
                    !WalkedActive(plan, 1) && !snapshot.specialization_reads.empty(),
                "failed retry exposed an invalid resource or suppressed original reads");
        Check(plan.walker_control_history.retry_remaining == interval,
              "failed retry did not rearm its bounded cooldown");
        data[0] = 1;
        for (uint32_t call = 0; call <= interval; ++call)
          Check(MaterializeResources(plan, runtime, snapshot, specialization), "second recovery failed");
        Check(!plan.walker_control_rejected && plan.srt_code.get() == jit, "second recovery stayed blacklisted");
        auto cpu_reference = Extract(program);
        for (auto word : {0u, 1u, 0u}) {
          read_state.word = word;
          read_state.train = false;
          ResourceSnapshot expected;
          ResourceSpecialization expected_specialization;
          Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
                    MaterializeResources(cpu_reference, {}, expected, expected_specialization),
                "recovered dynamic branch failed");
          const auto branch = word ? 1u : 2u;
          Check(WalkedActive(plan, 1) && WalkedActive(plan, 2) &&
                    snapshot.buffers[branch] == expected.buffers[branch],
                "recovered promotion changed the descriptor used by a changing GPU branch");
        }
        SetRecording(was_recording);
        SetExperiment(false);
      }
    }
  }
}

void TestFailedMaterializationRejectsStage() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan = UserDataBufferPlan();
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(!MaterializeResources(plan, {}, snapshot, specialization),
        "missing runtime user data did not reject the cached stage");
}

void TestSharedControlPredicates() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  using Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind;
  constexpr uint32_t depth = 8;
  for (const auto operation : {ValueOpcode::LogicalAnd, ValueOpcode::LogicalOr}) {
    Program program;
    program.user_data_count = 1;
    program.srt_plan_complete = program.resource_tracking_complete = true;
    auto &block = AddValueBlock(program);
    const auto emit = [&](ValueOpcode opcode, std::initializer_list<Value> args) {
      return Value(&block.AppendNewInst(opcode, args));
    };
    const auto user = emit(ValueOpcode::GetUserData, {Value(ScalarReg(0))});
    const auto uniform = emit(ValueOpcode::INotEqual32, {user, Value(0u)});
    const auto lane = emit(ValueOpcode::GetBuiltin,
        {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)), Value(0u)});
    auto varying = emit(ValueOpcode::INotEqual32, {lane, Value(0u)});
    // Both failed and partially uniform subgraphs share their operands exponentially.
    for (uint32_t i = 0; i < depth; ++i)
      varying = emit(ValueOpcode::LogicalOr, {varying, varying});
    auto condition = emit(operation, {varying, uniform});
    for (uint32_t i = 0; i < depth; ++i)
      condition = emit(ValueOpcode::LogicalOr, {condition, condition});
    condition = emit(ValueOpcode::LogicalNot, {condition});
    condition = emit(ValueOpcode::ConditionRef, {condition});
    for (uint32_t i = 0; i < 3; ++i) AddValueBlock(program);
    for (uint32_t i = 0; i < 2; ++i) {
      program.blocks[i]->condition = condition;
      program.blocks[i]->terminator.kind = TerminatorKind::ConditionalBranch;
      program.blocks[i]->terminator.true_block = program.blocks[i == 0 ? 1 : 3];
      program.blocks[i]->terminator.false_block = program.blocks[2];
    }
    auto plan = Extract(program);
    Check(plan.value_storage.size() <= 2u * depth + 12u &&
              plan.control_flow.size() == 4 &&
              plan.control_flow[0].condition == plan.control_flow[1].condition &&
              !plan.capture_specialization_reads,
          "shared predicates expanded into trees or retained varying dependencies");
    for (const uint32_t flag : {0u, 1u}) {
      const std::array<uint32_t, 1> user_data{flag};
      uint32_t result = 99;
      const bool conjunction = operation == ValueOpcode::LogicalAnd;
      const bool known = conjunction ? flag == 0 : flag != 0;
      Check(EvaluateSrtValue(plan, {.user_data = user_data}, plan.control_flow[0].condition, result) == known &&
                (!known || result == uint32_t{conjunction}),
            "shared predicate changed its uniform factor or cached runtime values");
      Check(ReachedBlocks(plan, {.user_data = user_data}) == (known
                    ? (conjunction ? std::vector<uint8_t>{1, 1, 0, 1}
                                   : std::vector<uint8_t>{1, 0, 1, 0})
                    : std::vector<uint8_t>{1, 1, 1, 1}),
            "shared predicate skipped an unknown branch or followed a rejected branch");
    }
  }
}

void TestNestedExecPredicatesReuseReads() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  using Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind;
  constexpr uint32_t depth = 12;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = program.resource_tracking_complete = true;
  for (uint32_t i = 0; i < 3; ++i) AddValueBlock(program);
  auto &entry = *program.blocks[0];
  auto &use = *program.blocks[1];
  auto &exit = *program.blocks[2];
  const auto emit = [&](ValueOpcode opcode, std::initializer_list<Value> args) {
    return Value(&entry.AppendNewInst(opcode, args));
  };
  const auto lane = emit(ValueOpcode::GetBuiltin,
      {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)), Value(0u)});
  program.memory_info.push_back({.kind = ResourceKind::ScalarAddress, .planning_only = true});
  const auto address = emit(ValueOpcode::GetAddressResource, {Value(0x1000u), Value(0u)});
  const auto count = emit(ValueOpcode::LoadAddressU32,
      {address, Value(0u), Value(0u), Value(true)});
  count.Instruction()->SetFlags(MemoryFlags{.index = 0});
  auto condition = emit(ValueOpcode::LogicalNot,
      {emit(ValueOpcode::ULessThanEqual32, {count, lane})});
  // A compact shared DAG must not become a tree of fresh EXEC evaluations.
  for (uint32_t i = 0; i < depth; ++i) {
    const auto mask = emit(ValueOpcode::INotEqual32, {lane, Value(i)});
    condition = emit(ValueOpcode::LogicalAnd,
        {mask, emit(ValueOpcode::LogicalOr, {condition, condition})});
  }
  entry.condition = condition;
  entry.terminator.kind = TerminatorKind::ConditionalBranch;
  entry.terminator.true_block = &use;
  entry.terminator.false_block = &exit;
  entry.AddBranch(&use);
  entry.AddBranch(&exit);
  use.terminator.kind = TerminatorKind::Branch;
  use.terminator.true_block = &exit;
  use.AddBranch(&exit);
  DescriptorSource source;
  source.dwords = {Value(0x8000u), Value(1u << 16u), Value(16u), Value(3u << 28u)};
  source.dword_count = 4;
  program.descriptor_sources.push_back(source);
  program.info.buffers.push_back({.source = 0, .read = true});
  program.memory_info.push_back({.kind = ResourceKind::Buffer, .resource = 0});
  const auto buffer = Value(&use.AppendNewInst(ValueOpcode::GetBufferResource,
      {source.dwords[0], source.dwords[1], source.dwords[2], source.dwords[3]}));
  use.AppendNewInst(ValueOpcode::LoadBufferU32,
      {buffer, Value(0u), Value(0u), Value(0u), Value(true)})
      .SetFlags(MemoryFlags{.index = 1});
  auto plan = Extract(program);
  struct Reads { uint32_t value = 0; bool readable = true; uint32_t count = 0; } reads;
  FakeGuestMemory fake(+[](void *data, uint64_t address, std::span<uint32_t> words) {
        auto &reads = *static_cast<Reads *>(data);
        ++reads.count;
        if (!reads.readable || address != 0x1000u || words.size() != 1) return false;
        words[0] = reads.value;
        return true;
      }, &reads);
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  for (const auto state : {Reads{64u, true}, Reads{0u, true}, Reads{64u, false}, Reads{64u, true}}) {
    reads = state;
    Check(MaterializeResources(plan, {}, snapshot, specialization),
          "nested EXEC predicates rejected a valid resource snapshot");
    // An unreadable count reads as zero.
    const bool inactive = !reads.readable || reads.value == 0;
    Check(snapshot.buffers[0].dwords[0] == (inactive ? 0u : 0x8000u),
          "nested EXEC predicates lost an unknown branch or retained stale runtime facts");
    Check(reads.count == 1 && snapshot.specialization_reads ==
              std::vector<std::pair<uint64_t, uint64_t>>{{0x1000u, 4u}},
          "EXEC contexts repeated a mask-independent scalar read");
  }
}

void TestFiniteImageRefreshReusesScalarReads() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &block = AddValueBlock(program);
  program.memory_info.push_back({.kind = ResourceKind::ScalarAddress});
  auto &handle = block.AppendNewInst(ValueOpcode::GetAddressResource,
                                     {Value(0x1000u), Value(0u)});
  auto &srt = block.AppendNewInst(ValueOpcode::GetSrtResource);
  for (uint32_t index = 0; index < 3; ++index) {
    auto &read = block.AppendNewInst(ValueOpcode::LoadAddressU32,
        {Value(&handle), Value(index * 4u), Value(0u), Value(true)});
    read.SetFlags(MemoryFlags{.index = 0});
    program.srt_reads.push_back({Value(&read), index});
    auto &flat = block.AppendNewInst(ValueOpcode::ReadConst,
                                     {Value(&srt), Value(index)});
    DescriptorSource source;
    source.dword_count = 8;
    source.dwords.fill(Value(0u));
    source.dwords[0] = Value(&flat);
    source.dwords[1] = Value(static_cast<uint32_t>(
        Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float) << 20u);
    source.dwords[3] = Value(Libs::Graphics::DstSel(4, 5, 6, 7) |
        (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u));
    program.descriptor_sources.push_back(source);
  }
  DescriptorSource root;
  root.dword_count = 8;
  root.dwords.fill(Value(0u));
  root.indirect_descriptor.emplace(DescriptorSource::IndirectDescriptor{}).sources = {0, 1, 2, 1};
  program.descriptor_sources.push_back(root);
  program.info.images.push_back({
      .source = 3,
      .resource_class = ImageResourceClass::Sampled,
      .numeric_class = Libs::Graphics::Prospero::TextureNumericClass::Float,
      .dimension = Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension::Dim2D});
  auto plan = Extract(program);
  struct Reads {
    std::array<uint32_t, 3> words{0x100u, 0x200u, 0x200u};
    std::array<uint32_t, 3> counts{};
  } reads;
  FakeGuestMemory fake(+[](void *data, uint64_t address, std::span<uint32_t> words) {
        if (words.size() != 1 || address < 0x1000u || address >= 0x100cu ||
            (address & 3u) != 0) return false;
        auto &reads = *static_cast<Reads *>(data);
        const auto index = (address - 0x1000u) / 4u;
        ++reads.counts[index];
        words[0] = reads.words[index];
        return true;
      }, &reads);
  const SrtRuntime runtime;
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  const auto capacities = [&] {
    return std::array{snapshot.images.capacity(), snapshot.flattened_srt.capacity(),
                      snapshot.specialization_reads.capacity(), specialization.images.capacity()};
  };
  const auto check_mapping = [&](std::array<uint32_t, 4> ordinals) {
    const auto offset = specialization.images[0].indirect_mapping_offset;
    Check(snapshot.images.size() == 2 && snapshot.flattened_srt[offset] == 4,
          "finite image candidates were not deduplicated");
    for (uint32_t key = 0; key < ordinals.size(); ++key) {
      Check(snapshot.flattened_srt[offset + 1u + key * 2u] == key &&
                snapshot.flattened_srt[offset + 2u + key * 2u] == ordinals[key],
            "finite image selector mapping is stale or incorrect");
    }
  };
  Check(MaterializeResources(plan, runtime, snapshot, specialization),
        "finite image materialization failed");
  Check(reads.counts == std::array<uint32_t, 3>{1, 1, 1} &&
            snapshot.specialization_reads.size() == 3,
        "finite image candidates repeated or skipped scalar reads");
  check_mapping({0, 1, 1, 1});
  const auto warm_capacities = capacities();
  reads.words = {0x300u, 0x300u, 0x400u};
  Check(MaterializeResources(plan, runtime, snapshot, specialization),
        "finite image refresh failed after descriptor changes");
  Check(reads.counts == std::array<uint32_t, 3>{2, 2, 2} &&
            snapshot.specialization_reads.size() == 3 &&
            snapshot.images[0].dwords[0] == 0x300u && snapshot.images[1].dwords[0] == 0x400u,
        "finite image refresh retained old scalar values or repeated reads");
  check_mapping({0, 0, 1, 0});
  Check(capacities() == warm_capacities,
        "finite image refresh grew reusable resource storage after warmup");
}

void TestEmptyTraversalSkipsUnmappedDescriptor(bool independent_mask, bool counted_loop = false) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  using Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = program.resource_tracking_complete = true;
  for (uint32_t i = 0; i < 6; ++i) AddValueBlock(program);
  auto &entry = *program.blocks[0];
  auto &loop = *program.blocks[1];
  auto &body = *program.blocks[2];
  auto &merge = *program.blocks[3];
  auto &use = *program.blocks[4];
  auto &exit = *program.blocks[5];
  const auto emit = [](Block &block, ValueOpcode opcode, std::initializer_list<Value> args) {
    return Value(&block.AppendNewInst(opcode, args));
  };
  const auto branch = [](Block &block, Block &target, Value condition = {}, Block *other = nullptr) {
    block.terminator.kind = other ? TerminatorKind::ConditionalBranch : TerminatorKind::Branch;
    block.terminator.true_block = &target;
    block.terminator.false_block = other;
    block.condition = condition;
    block.AddBranch(&target);
    if (other) block.AddBranch(other);
  };
  Value lane, active, initial;
  if (!counted_loop) {
    lane = emit(entry, ValueOpcode::GetBuiltin,
        {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)), Value(0u)});
    active = emit(entry, ValueOpcode::INotEqual32, {lane, Value(0u)});
    const auto initial_mask = independent_mask
        ? emit(entry, ValueOpcode::IEqual32, {lane, Value(0u)}) : active;
    initial = emit(entry, ValueOpcode::SelectU32,
        {initial_mask, Value(UINT32_MAX), Value(0u)});
  }
  program.memory_info.push_back({.kind = ResourceKind::ScalarAddress, .planning_only = true});
  const auto address = emit(entry, ValueOpcode::GetAddressResource, {Value(0x1000u), Value(0u)});
  const auto count = emit(entry, ValueOpcode::LoadAddressU32,
      {address, Value(0u), Value(0u), Value(true)});
  count.Instruction()->SetFlags(MemoryFlags{.index = 0});
  if (counted_loop) {
    branch(entry, loop);
  } else {
    const auto rejected = emit(entry, ValueOpcode::ULessThanEqual32, {count, lane});
    const auto accepted = emit(entry, ValueOpcode::LogicalNot, {rejected});
    branch(entry, loop, emit(entry, ValueOpcode::LogicalAnd, {active, accepted}), &merge);
  }

  // A nonempty traversal contains a varying loop-carried value. Its cycle must
  // remain unknown; only the proven empty traversal can select the bypass arm.
  auto &carried = loop.AppendNewInst(ValueOpcode::Phi);
  carried.SetFlags(Type::U32);
  const auto next = emit(body, ValueOpcode::IAdd32, {Value(&carried), Value(1u)});
  carried.AddPhiOperand(&entry, counted_loop ? Value(0u) : lane);
  carried.AddPhiOperand(&body, next);
  const auto condition = counted_loop
      ? emit(loop, ValueOpcode::ULessThan32, {Value(&carried), count})
      : emit(loop, ValueOpcode::INotEqual32, {Value(&carried), Value(UINT32_MAX)});
  branch(loop, counted_loop ? use : body, condition, &merge);
  loop.terminator.loop_header = true;
  loop.terminator.continue_block = &body;
  loop.terminator.merge_block = &merge;
  branch(body, loop);
  if (counted_loop) {
    branch(merge, exit);
    branch(use, body);
  } else {
    auto &selected = merge.AppendNewInst(ValueOpcode::Phi);
    selected.SetFlags(Type::U32);
    selected.AddPhiOperand(&entry, initial);
    selected.AddPhiOperand(&loop, Value(&carried));
    const auto valid = emit(merge, ValueOpcode::INotEqual32, {Value(&selected), Value(UINT32_MAX)});
    branch(merge, use, emit(merge, ValueOpcode::LogicalAnd, {active, valid}), &exit);
    branch(use, exit);
  }
  DescriptorSource source;
  const auto descriptor_address = emit(use, ValueOpcode::GetAddressResource,
      {Value(0x2000u), Value(0u)});
  for (uint32_t i = 0; i < 4; ++i) {
    const auto index = static_cast<uint32_t>(program.memory_info.size());
    program.memory_info.push_back({.kind = ResourceKind::ScalarAddress, .planning_only = true});
    source.dwords[i] = emit(use, ValueOpcode::LoadAddressU32,
        {descriptor_address, Value(i * 4u), Value(0u), Value(true)});
    source.dwords[i].Instruction()->SetFlags(MemoryFlags{.index = index});
  }
  source.dword_count = 4;
  program.descriptor_sources.push_back(source);
  program.info.buffers.push_back({.source = 0, .read = true});
  const auto buffer_index = static_cast<uint32_t>(program.memory_info.size());
  program.memory_info.push_back({.kind = ResourceKind::Buffer, .resource = 0});
  const auto buffer = emit(use, ValueOpcode::GetBufferResource,
      {source.dwords[0], source.dwords[1], source.dwords[2], source.dwords[3]});
  emit(use, ValueOpcode::LoadBufferU32, {buffer, Value(0u), Value(0u), Value(0u), Value(true)})
      .Instruction()->SetFlags(MemoryFlags{.index = buffer_index});
  auto plan = Extract(program);
  struct Reads { uint32_t count = 0; bool readable = true; uint32_t descriptors = 0; } reads;
  FakeGuestMemory fake(+[](void *data, uint64_t address, std::span<uint32_t> words) {
        constexpr std::array<uint32_t, 4> descriptor{0x8000u, 1u << 16u, 16u,
                                                   (3u << 28u) | 0x4dfacu};
        auto &reads = *static_cast<Reads *>(data);
        if (address == 0x1000u && reads.readable) {
          words[0] = reads.count;
          return true;
        }
        if (address < 0x2000u || address >= 0x2010u || address % 4u || words.size() != 1)
          return false;
        ++reads.descriptors;
        words[0] = descriptor[(address - 0x2000u) / 4u];
        return true;
      }, &reads);
  const SrtRuntime runtime;
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  for (const auto state : {Reads{0u, true}, Reads{4u, true}, Reads{4u, false}, Reads{0u, true}}) {
    reads = state;
    if (!counted_loop) {
      uint32_t condition = 99;
      Check(!EvaluateSrtValue(plan, runtime, plan.control_flow[3].condition, condition),
            "direct merge evaluation used missing or previous runtime reachability");
    }
    Check(MaterializeResources(plan, runtime, snapshot, specialization),
          "partial traversal proof rejected a valid resource snapshot");
    // An unreadable count reads as zero.
    const bool inactive = !independent_mask && (!reads.readable || reads.count == 0u);
    Check(snapshot.buffers[0].dwords[0] == (inactive ? 0u : 0x8000u),
          "empty traversal kept its descriptor or an uncertain traversal lost it");
    Check(specialization.buffers[0].zero_stride_oob == inactive,
          "inactive traversal did not expose dead vector reads to specialization");
    Check(reads.descriptors == (inactive ? 0u : 4u),
          "inactive traversal read its descriptor or an uncertain traversal skipped it");
    if (inactive) {
      Check(!snapshot.specialization_reads.empty(), "empty traversal lost its captured scalar read");
      for (const auto &read : snapshot.specialization_reads)
        Check(read == std::pair<uint64_t, uint64_t>{0x1000u, 4u},
              "empty traversal proof captured an unrelated memory range");
    }
  }
}

void TestCyclicControlPredicatesRemainUnknown() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  using Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = program.resource_tracking_complete = true;
  for (uint32_t i = 0; i < 4; ++i) AddValueBlock(program);
  auto &entry = *program.blocks[0];
  auto &left = *program.blocks[1];
  auto &right = *program.blocks[2];
  auto &exit = *program.blocks[3];
  const auto lane = Value(&entry.AppendNewInst(ValueOpcode::GetBuiltin,
      {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)), Value(0u)}));
  const auto u = Value(&entry.AppendNewInst(ValueOpcode::INotEqual32, {lane, Value(0u)}));
  const auto v = Value(&entry.AppendNewInst(ValueOpcode::INotEqual32, {lane, Value(1u)}));
  auto &p = left.AppendNewInst(ValueOpcode::Phi);
  auto &q = right.AppendNewInst(ValueOpcode::Phi);
  p.SetFlags(Type::U1);
  q.SetFlags(Type::U1);
  const auto a = Value(&left.AppendNewInst(ValueOpcode::LogicalAnd, {u, Value(&p)}));
  const auto b = Value(&right.AppendNewInst(ValueOpcode::LogicalAnd, {v, Value(&q)}));
  p.AddPhiOperand(&entry, Value(true));
  p.AddPhiOperand(&right, b);
  q.AddPhiOperand(&entry, Value(true));
  q.AddPhiOperand(&left, a);
  const auto branch = [](Block &block, Value condition, Block &yes, Block &no) {
    block.condition = condition;
    block.terminator.kind = TerminatorKind::ConditionalBranch;
    block.terminator.true_block = &yes;
    block.terminator.false_block = &no;
    block.AddBranch(&yes);
    block.AddBranch(&no);
  };
  // Two loop entries have no single header whose Phi can be discarded up front.
  branch(entry, u, left, right);
  branch(left, a, right, exit);
  branch(right, b, left, exit);
  auto plan = Extract(program);
  uint32_t value = 99;
  Check(!EvaluateSrtValue(plan, {}, plan.control_flow[1].condition, value) &&
            !EvaluateSrtValue(plan, {}, plan.control_flow[2].condition, value),
        "cyclic varying control acquired an unsupported uniform value");
  Check(ReachedBlocks(plan, {}) == std::vector<uint8_t>({1, 1, 1, 1}),
        "cyclic control lost a possible branch during reachability");
}

void TestBackEdgeReachesPhiPredecessor() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  using Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.user_data_count = 3;
  program.srt_plan_complete = program.resource_tracking_complete = true;
  for (uint32_t i = 0; i < 4; ++i) AddValueBlock(program);
  auto &entry = *program.blocks[0];
  auto &header = *program.blocks[1];
  auto &body = *program.blocks[2];
  auto &exit = *program.blocks[3];
  const auto branch = [](Block &block, Block &target, Value condition = {}, Block *other = nullptr) {
    block.terminator.kind = other ? TerminatorKind::ConditionalBranch : TerminatorKind::Branch;
    block.terminator.true_block = &target;
    block.terminator.false_block = other;
    block.condition = condition;
    block.AddBranch(&target);
    if (other) block.AddBranch(other);
  };
  // header: p = Phi(entry: 0, body: 1); table[1] == p enters body, which returns to header. The
  // first pass evaluates p and loads table[1] before body is reached; the walk must revisit both,
  // capture the load once and reach exit, which loads table[0].
  program.memory_info.push_back({.kind = ResourceKind::ScalarAddress});
  const auto table = Value(&entry.AppendNewInst(ValueOpcode::GetAddressResource,
      {Value(&entry.AppendNewInst(ValueOpcode::GetUserData, {Value(ScalarReg(0))})),
       Value(&entry.AppendNewInst(ValueOpcode::GetUserData, {Value(ScalarReg(1))}))}));
  const auto load = [&](Block &block, uint32_t offset) -> Inst & {
    auto &read = block.AppendNewInst(ValueOpcode::LoadAddressU32, {table, Value(offset), Value(0u), Value(true)});
    read.SetFlags(MemoryFlags{.index = 0});
    return read;
  };
  auto &p = header.AppendNewInst(ValueOpcode::Phi);
  p.SetFlags(Type::U32);
  p.AddPhiOperand(&entry, Value(0u));
  p.AddPhiOperand(&body, Value(1u));
  branch(entry, header);
  branch(header, body, Value(&header.AppendNewInst(ValueOpcode::IEqual32, {Value(&load(header, 4u)), Value(&p)})),
         &exit);
  branch(body, header);
  exit.terminator.kind = TerminatorKind::Return;
  program.srt_reads.push_back({Value(&load(exit, 0u)), 0});
  exit.AppendNewInst(ValueOpcode::ReadConst,
                     {Value(&exit.AppendNewInst(ValueOpcode::GetSrtResource)), Value(0u)});
  auto plan = Extract(program);
  Check(plan.control_flow.size() == 4 && plan.control_phi_predecessors.size() == 1,
        "back-edge plan lost its control flow or Phi");
  plan.capture_specialization_reads = true;
  const std::array<uint32_t, 2> user_data{0x1000u, 0u};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  {
    // The table is unmapped: its loads read zero instead of faulting the walk.
    FakeGuestMemory unmapped;
    Check(MaterializeResources(plan, {.user_data = user_data}, snapshot, specialization) &&
              snapshot.flattened_srt == std::vector<uint32_t>{0u},
          "faulting load in a reachable block did not read zero");
  }
  for (uint32_t selected : {0u, 1u}) {
    FakeGuestMemory fake(+[](void *userdata, uint64_t address, std::span<uint32_t> words) {
          words[0] = address == 0x1000u ? 0x5678u : *static_cast<uint32_t *>(userdata);
          return address == 0x1000u || address == 0x1004u;
        }, &selected);
    Check(MaterializeResources(plan, {.user_data = user_data}, snapshot, specialization) &&
              snapshot.flattened_srt == std::vector<uint32_t>{0x5678u},
          "back edge to an evaluated Phi left its exit unreachable");
    Check(snapshot.specialization_reads ==
              std::vector<std::pair<uint64_t, uint64_t>>{{0x1004u, 4u}, {0x1000u, 4u}},
          "Phi restart kept the loads of the abandoned pass");
    Check(ReachedBlocks(plan, {.user_data = user_data}) ==
              (selected == 0u ? std::vector<uint8_t>{1, 1, 1, 1} : std::vector<uint8_t>{1, 1, 0, 1}),
          "Phi restart followed a rejected edge or missed a reachable one");
  }
}

void TestEmptyScalarBufferSpecialization() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  struct Fixture {
    Program program;
    std::vector<std::pair<Inst *, Type>> uses;
    Inst *nonnull = nullptr;
  };
  const auto make_fixture = [] {
    Fixture fixture;
    auto &program = fixture.program;
    program.stage = Libs::Graphics::ShaderType::Compute;
    program.srt_plan_complete = program.resource_tracking_complete = true;
    program.user_data_count = 4u;
    auto &block = AddValueBlock(program);
    DescriptorSource source;
    source.dword_count = 4u;
    for (uint32_t word = 0; word < 4u; ++word)
      source.dwords[word] = Value(&block.AppendNewInst(ValueOpcode::GetUserData,
          {Value(static_cast<ScalarReg>(word))}));
    program.descriptor_sources.push_back(source);
    program.info.buffers.push_back({.source = 0u, .read = true, .scalar = true});
    const auto table = Value(&block.AppendNewInst(ValueOpcode::GetBufferResource,
        {source.dwords[0], source.dwords[1], source.dwords[2], source.dwords[3]}));
    std::array<Value, 4> words;
    for (uint32_t word = 0; word < 4u; ++word) {
      program.memory_info.push_back({.kind = ResourceKind::ScalarBuffer, .resource = 0u,
                                     .offset = word * 4u});
      auto &read = block.AppendNewInst(ValueOpcode::ReadConstBuffer, {table, Value(0u)});
      read.SetFlags(MemoryFlags{.index = word});
      words[word] = Value(&read);
    }
    const auto handle = Value(&block.AppendNewInst(ValueOpcode::GetBufferResource,
        {words[0], words[1], words[2], words[3]}));
    for (const auto opcode : {ValueOpcode::ReadConstBuffer, ValueOpcode::LoadBufferU8,
                              ValueOpcode::LoadBufferU16, ValueOpcode::LoadBufferU32,
                              ValueOpcode::LoadBufferU32x2, ValueOpcode::LoadBufferU32x3,
                              ValueOpcode::LoadBufferU32x4}) {
      const auto index = static_cast<uint32_t>(program.memory_info.size());
      program.memory_info.push_back({.kind = ResourceKind::IndirectBuffer, .resource = UINT32_MAX,
          .data_dwords = BufferComponentCount(opcode),
          .data_bits = opcode == ValueOpcode::LoadBufferU8 ? 8u : opcode == ValueOpcode::LoadBufferU16 ? 16u : 32u,
          .formatted = opcode == ValueOpcode::LoadBufferU32});
      auto &read = opcode == ValueOpcode::ReadConstBuffer
          ? block.AppendNewInst(opcode, {handle, Value(0u)})
          : block.AppendNewInst(opcode, {handle, Value(0u), Value(0u), Value(0u), Value(true)});
      read.SetFlags(MemoryFlags{.index = index});
      fixture.uses.emplace_back(&block.AppendNewInst(ValueOpcode::Identity, {Value(&read)}), read.GetType());
    }
    const auto nonnull = Value(&block.AppendNewInst(ValueOpcode::GetBufferResource,
        {words[0], words[1], words[2], Value(1u)}));
    auto &read = block.AppendNewInst(ValueOpcode::ReadConstBuffer, {nonnull, Value(0u)});
    read.SetFlags(MemoryFlags{.index = 4u});
    fixture.nonnull = &block.AppendNewInst(ValueOpcode::Identity, {Value(&read)});
    return fixture;
  };
  auto fixture = make_fixture();
  auto plan = Extract(fixture.program);
  std::array<uint32_t, 4> descriptor{0x1000u, 8u << 16u, 0u, 3u << 28u};
  uint32_t reads = 0u;
  const SrtRuntime runtime{.user_data = descriptor, .userdata = &reads,
                           .read_specialization_memory = RejectSpecializationRead};
  FakeGuestMemory fake(RejectSpecializationRead, &reads);
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization, empty, populated;
  for (const uint32_t records : {0u, 1u, 0x20000000u, 0u}) {
    descriptor[2] = records;
    Check(MaterializeResources(plan, runtime, snapshot, specialization) && reads == 0u &&
              specialization.buffers[0].scalar_empty == (records == 0u),
          "scalar emptiness read table contents or truncated its 64-bit byte size");
    if (records == 0u) {
      if (empty.buffers.empty()) empty = specialization;
      else Check(specialization == empty, "returning to an empty scalar table changed its specialization");
    }
    else if (records == 1u) populated = specialization;
    else Check(specialization == populated, "nonempty scalar size created a shader permutation");
    auto current = make_fixture();
    ApplyResourceSpecialization(current.program, specialization);
    for (const auto &[use, type] : current.uses) {
      const auto value = use->Arg(0).Resolve();
      Check(value.GetType() == type, "empty scalar-table folding changed the load result type");
      if (records != 0u) {
        Check(value.TryInstruction() != nullptr && BufferAccessOf(value.Instruction()->GetOpcode()) == BufferAccess::Read,
              "nonempty scalar table lost a runtime buffer load");
      } else if (value.IsImmediate()) {
        Check(value == Value(uint8_t{0}) || value == Value(uint16_t{0}) || value == Value(0u),
              "empty scalar table did not produce a typed zero");
      } else {
        for (size_t component = 0; component < value.Instruction()->NumArgs(); ++component)
          Check(value.Instruction()->Arg(component) == Value(0u),
                "empty scalar table retained a nonzero vector component");
      }
    }
    Check(current.nonnull->Arg(0).Resolve().TryInstruction() != nullptr,
          "a partly nonzero indirect descriptor was treated as unbound");
  }
  Check(empty != populated, "empty-to-populated scalar transition reused the folded specialization");
  plan.info.buffers[0].scalar = false;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            !specialization.buffers[0].scalar_empty,
        "vector-only resource acquired scalar-empty specialization");
  empty.buffers[0].indirect_root = 0u;
  empty.buffers.push_back(empty.buffers[0]);
  empty.buffers[1].scalar_empty = false;
  ApplyResourceSpecialization(fixture.program, empty);
  for (const auto &[use, type] : fixture.uses)
    Check(use->Arg(0).GetType() == type && use->Arg(0).Resolve().TryInstruction() != nullptr &&
              BufferAccessOf(use->Arg(0).ResolveInstruction()->GetOpcode()) == BufferAccess::Read,
          "empty root folded a scalar read with nonempty candidates");
}

void TestMixedSamplerVariantsShareRuntimeDescriptor() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto program = MixedSamplerProgram();
  auto plan = Extract(program);
  std::array<uint32_t, 2> user_data{0x11111111u, 0x22222222u};
  const SrtRuntime runtime{.user_data = user_data};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization),
        "mixed sampler materialization failed");
  ApplyResourceSpecialization(program, specialization);
  const auto &samplers = program.info.samplers;
  Check(snapshot.samplers.size() == 2 && samplers.size() == 3 &&
            samplers[0].snapshot_index == 0 && samplers[1].snapshot_index == 1 &&
            samplers[2].snapshot_index == 1 &&
            samplers[0].source == plan.info.samplers[0].source &&
            samplers[1].source == plan.info.samplers[1].source &&
            samplers[2].source == samplers[1].source &&
            !samplers[1].force_point_filtering && samplers[2].force_point_filtering &&
            program.info.sampled_pairs[2].sampler == 2,
        "native sampler variants lost their source identity or binding order");
  const auto capacity = snapshot.samplers.capacity();
  user_data[1] = 0x33333333u;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            snapshot.samplers.size() == 2 && snapshot.samplers.capacity() == capacity &&
            snapshot.samplers[samplers[0].snapshot_index].dwords[0] == user_data[0] &&
            snapshot.samplers[samplers[1].snapshot_index].dwords[0] == user_data[1] &&
            snapshot.samplers[samplers[2].snapshot_index].dwords[0] == user_data[1],
        "sampler variants retained stale or duplicated descriptors after refresh");
}

void TestSignedStorageSpecialization() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  using F = Libs::Graphics::Prospero::BufferFormat;
  using N = Libs::Graphics::Prospero::TextureNumericClass;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = program.resource_tracking_complete = true;
  program.user_data_count = 1u;
  auto &block = AddValueBlock(program);
  DescriptorSource source;
  source.dword_count = 8u;
  source.dwords.fill(Value(0u));
  source.dwords[0] = Value(0x100u);
  source.dwords[1] = Value(&block.AppendNewInst(ValueOpcode::GetUserData, {Value(ScalarReg(0))}));
  constexpr auto swizzle = Libs::Graphics::DstSel(5, 4, 1, 0);
  source.dwords[3] = Value(swizzle |
      (uint32_t(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u));
  program.descriptor_sources.push_back(source);
  program.info.images.push_back({.source = 0u, .resource_class = ImageResourceClass::Storage,
      .dimension = Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension::Dim2D, .written = true});
  auto plan = Extract(program);
  std::array<uint32_t, 1> data{};
  const SrtRuntime runtime{.user_data = data};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization, signed16;
  for (const auto format : {F::k16SInt, F::k32SInt, F::k16SInt}) {
    data[0] = uint32_t(format) << 20u;
    Check(MaterializeResources(plan, runtime, snapshot, specialization),
          "write-only signed storage image failed materialization");
    const auto conversion = format == F::k16SInt ? format : F::kInvalid;
    const auto view = format == F::k16SInt ? F::k16UInt : F::k32UInt;
    Check(specialization.images[0].numeric_class == N::Uint &&
              specialization.images[0].conversion_format == conversion &&
              specialization.images[0].shader_swizzle == swizzle &&
              snapshot.images[0].dwords[1] == data[0] &&
              RawSignedStorageFormat(plan.info.images[0], format) == view,
          "signed storage specialization lost guest format, conversion, swizzle or raw view width");
    if (format == F::k16SInt) {
      if (signed16.images.empty()) signed16 = specialization;
      else Check(specialization == signed16, "signed storage format refresh retained a stale specialization");
    } else {
      Check(specialization != signed16, "signed 16/32-bit stores shared incompatible conversion state");
    }
    ApplyResourceSpecialization(program, specialization);
    const auto binding = DescriptorBindingForImage(program.info.images[0]);
    Check(binding && ImageBindingResourceClass(*binding) == ImageResourceClass::Storage,
          "signed storage conversion did not reuse the unsigned storage binding");
  }
  for (const auto format : {F::k8SInt, F::k16SInt, F::k32SInt}) {
    data[0] = uint32_t(format) << 20u;
    auto &image = plan.info.images[0];
    image.read = true;
    Check(RawSignedStorageFormat(image, format) == F::kInvalid &&
              !MaterializeResources(plan, runtime, snapshot, specialization),
          "signed raw storage view accepted a non-atomic read/write image");
    image.atomic = true;
    Check((RawSignedStorageFormat(image, format) != F::kInvalid) == (format == F::k32SInt) &&
              MaterializeResources(plan, runtime, snapshot, specialization) == (format == F::k32SInt),
          "signed storage conversion changed atomic format eligibility");
    image.atomic64 = true;
    Check(RawSignedStorageFormat(image, format) == F::kInvalid &&
              !MaterializeResources(plan, runtime, snapshot, specialization),
          "signed 16/32-bit image acquired a 64-bit atomic raw view");
    image.resource_class = ImageResourceClass::Sampled;
    image.written = image.atomic = image.atomic64 = false;
    Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
              specialization.images[0].numeric_class == N::Sint &&
              specialization.images[0].conversion_format == F::kInvalid &&
              Libs::Graphics::Prospero::NumBytesPerElement(format) ==
                  (format == F::k8SInt ? 1u : format == F::k16SInt ? 2u : 4u) &&
              RawSignedStorageFormat(image, format) == F::kInvalid,
          "storage conversion changed ordinary signed image reads");
    image.resource_class = ImageResourceClass::Storage;
    image.written = true;
    image.read = false;
  }
}

void TestIndirectImageSamplerClasses(bool mixed) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  using Libs::Graphics::Prospero::BufferFormat;
  using Libs::Graphics::Prospero::TextureNumericClass;
  auto program = MixedSamplerProgram();
  auto &first = program.descriptor_sources[0];
  first.dwords[0] = Value(0x100u);
  first.dwords[1] = Value(static_cast<uint32_t>(BufferFormat::k8UNorm) << 20u);
  first.dwords[3] = program.descriptor_sources[1].dwords[3];
  program.descriptor_sources[1].dwords[1] = Value(static_cast<uint32_t>(
      mixed ? BufferFormat::k8UInt : BufferFormat::k8UNorm) << 20u);
  DescriptorSource root;
  root.dword_count = 8;
  root.dwords.fill(Value(0u));
  root.indirect_descriptor.emplace(DescriptorSource::IndirectDescriptor{}).sources = {0, 1, 0};
  program.info.images.resize(1);
  program.info.images[0].source = static_cast<uint32_t>(program.descriptor_sources.size());
  program.descriptor_sources.push_back(root);
  program.info.samplers.resize(1);
  program.info.sampled_pairs.resize(1);
  auto plan = Extract(program);
  std::array<uint32_t, 1> user_data{0x11111111u};
  uint32_t reads = 0;
  const SrtRuntime runtime{.user_data = user_data, .userdata = &reads,
                           .read_specialization_memory = RejectSpecializationRead};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) && reads == 0 &&
            snapshot.images.size() == 2 && snapshot.images[0].dwords[0] == 0x100u &&
            snapshot.images[1].dwords[0] == 1u &&
            specialization.images[0].numeric_class == TextureNumericClass::Float &&
            specialization.images[1].numeric_class ==
                (mixed ? TextureNumericClass::Uint : TextureNumericClass::Float),
        "indirect image materialization rejected or changed a typed candidate");
  const auto offset = specialization.images[0].indirect_mapping_offset;
  Check(snapshot.flattened_srt[offset] == 3u &&
            snapshot.flattened_srt[offset + 2u] == 0u &&
            snapshot.flattened_srt[offset + 4u] == 1u &&
            snapshot.flattened_srt[offset + 6u] == 0u,
        "mixed image classes changed selector ordinals or duplicated candidates");
  ApplyResourceSpecialization(program, specialization);
  Check(program.info.images[0].indirect_resources == std::vector<uint32_t>({0, 1}) &&
            snapshot.samplers.size() == 1 && program.info.samplers.size() == (mixed ? 2u : 1u),
        "indirect images lost candidates or created unnecessary sampler variants");
  user_data[0] = 0x33333333u;
  Check(MaterializeResources(plan, runtime, snapshot, specialization),
        "mixed image sampler refresh failed");
  for (uint32_t index = 0; index < program.info.samplers.size(); ++index) {
    const auto &sampler = program.info.samplers[index];
    Check(sampler.snapshot_index == 0u && sampler.source == plan.info.samplers[0].source &&
              sampler.force_point_filtering == (index != 0u) &&
              sampler.integer_border == (index != 0u) &&
              snapshot.samplers[sampler.snapshot_index].dwords[0] == user_data[0],
          "typed sampler variants lost filtering, border class, or shared live descriptor");
  }
}

void TestShaderCacheRecords() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  // Equivalent selector domains share one key group, which a load rebuilds.
  Program selectors;
  AddValueBlock(selectors);
  for (const uint32_t stride : {4u, 8u, 4u}) {
    DescriptorSource source;
    source.indirect_descriptor = DescriptorSource::IndirectDescriptor{
        .selector = DescriptorSource::IndirectDescriptor::SelectorRead{.stride = stride}};
    selectors.descriptor_sources.push_back(source);
  }
  selectors.descriptor_sources.emplace_back();
  const auto plan = Extract(selectors);
  Check(plan.selector_groups == std::vector<uint32_t>{0u, 1u, 0u, UINT32_MAX} &&
            plan.selector_keys.size() == 2u,
        "loaded plan lost its selector key groups");

  const std::array values{Value(), Value(ScalarReg(3)), Value(VectorReg(5)), Value(true),
                          Value(uint8_t{7}), Value(uint16_t{9}), Value(11u), Value(uint64_t{1} << 40u),
                          Value::F16(0x3c00u), Value::F32(-2.5f)};
  Writer writer;
  Io(writer, values);
  Reader reader{writer.bytes};
  std::array<Value, values.size()> loaded;
  Io(reader, loaded);
  Check(reader.ok && reader.bytes.empty() && loaded == values,
        "immediate values changed through the shader cache");
  // Damaged records: a missing instruction, a type that is no value type, an oversized count and
  // an unknown opcode.
  for (const uint8_t type : {1u, 3u}) {
    const std::array<uint8_t, 12> bytes{type};
    Reader damaged{bytes};
    Value value;
    Io(damaged, value);
    Check(!damaged.ok, "damaged value was accepted");
  }
  ResourcePlan oversized, unknown;
  Check(!LoadResourcePlan(std::array<uint8_t, 4>{0xffu, 0xffu, 0xffu, 0xffu}, oversized) &&
            !LoadResourcePlan(std::array<uint8_t, 14>{1u, 0u, 0u, 0u, 0xffu, 0xffu}, unknown),
        "damaged resource plan was accepted");
}

} // namespace

namespace Common {

int DbgExitHandler(const char *, int, std::string_view) { std::abort(); }

int DbgExitHandler(const char *, int, fmt::text_style, std::string_view) {
  std::abort();
}

int DbgExitIfHandler(const char *, const char *, int) { return 1; }

void DbgExit(int) { std::abort(); }

} // namespace Common

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--srt-descriptor-probe-only") {
    TestSrtDescriptorProbe();
    return 0;
  }
  if (argc == 2 && (std::string_view(argv[1]) == "--walker-control-only" ||
                    std::string_view(argv[1]) == "--walker-control-combined-only" ||
                    std::string_view(argv[1]) == "--walker-control-disabled-only")) {
    const auto* experiment = std::string_view(argv[1]) == "--walker-control-combined-only"
        ? "indirect-count-gpu" : std::string_view(argv[1]) == "--walker-control-disabled-only" ? "no-walker-control-gpu" : "";
#if defined(_WIN32)
    _putenv_s("KYTY_EXPERIMENT", experiment);
#else
    setenv("KYTY_EXPERIMENT", experiment, 1);
#endif
    TestWritableBufferControlProjection(true);
    std::puts("WalkerControlGpu: all cases passed");
    return 0;
  }
  if (argc == 2 && std::string_view(argv[1]) == "--readback-origins-only") {
#if defined(_WIN32)
    _putenv_s("KYTY_READBACK_DETAIL", "1");
    _putenv_s("KYTY_SUBMIT_PROFILE", "1");
#else
    setenv("KYTY_READBACK_DETAIL", "1", 1);
    setenv("KYTY_SUBMIT_PROFILE", "1", 1);
#endif
    TestWritableBufferControlProjection();
    {
      using namespace Libs::Graphics::ShaderRecompiler::IR;
      auto plan = Extract(SrtProgram(0x1000));
      uint32_t observed = 0;
      FakeGuestMemory fake(+[](void*, uint64_t, std::span<uint32_t> words) {
        words[0] = 17; return true;
      }, &observed, +[](void* data, uint64_t rip, uint64_t) {
        Common::ReadbackProfile::Origin reader;
        Check(DescribeSrtWalkerRead(rip, reader) && (reader.uses & 2u) &&
                  !(reader.uses & 1u) && reader.flat_slot == 0 && reader.planning_only,
              "mandatory flattened SRT load was classified as optional control");
        ++*static_cast<uint32_t*>(data);
      });
      std::vector<uint32_t> flat;
      Check(WalkSrt(plan, {}, flat, nullptr) && flat == std::vector<uint32_t>{17} && observed == 1,
            "flat-read diagnostic changed the walker result");
    }
    using namespace Common::ReadbackProfile;
    auto state = std::make_unique<State>(); // Large bounded history stays off the test stack.
    state->writers.Add({.address = 0x1000, .bytes = 4, .hash = 11, .tick = 3});
    state->writers.Add({.address = 0x1004, .bytes = 4, .hash = 22, .tick = 4});
    Check(state->writers.Find(0x1000, 4).hash == 11 &&
              state->writers.Find(0x1003, 2).hash == 22 &&
              state->writers.Find(0x1008, 4).sequence == 0,
          "writer history broke exact overlap or newest-first ordering");
    Origin reader;
    reader.kind = Kind::Walker; reader.tid = 7; reader.hash = 33;
    reader.address = 0x1000; reader.bytes = 4;
    Add(*state, reader, 50, 6, true);
    reader.tid = 8;
    Add(*state, reader, 75, 7, false);
    Check(state->calls == 2 && state->ns == 125 && state->entries[0].ns == 50 &&
              state->entries[1].ns == 75 && state->entries[0].writer.hash == 11,
          "requester identity or local/delegated wait attribution was merged");
    { OriginScope scope(reader); Check(origin.tid == 8, "request scope did not publish"); }
    Check(origin.kind == Kind::Unknown, "request scope leaked to next operation");
    // A delegated guest fault with unknown width can prove that its fault byte
    // is dirty. A clean fault byte alone cannot prove the whole read is clean.
    reader = {.kind = Kind::Fault, .tid = 9, .role = 1, .rip = 0x9073a9fce,
              .address = 0x1080, .fault_address = 0x1080};
    Common::SubmitProfile::SetRecording(true);
    {
      OriginScope scope(reader);
      Common::SubmitProfile::WaitSiteScope site(Common::SubmitProfile::WaitSite::ReadbackCpuReadFault);
      RecordDownload(0x1000, 0x1040);
      Check(origin.downloads.page_dirty && !origin.downloads.fault_dirty &&
                !origin.downloads.FalseShared(reader.address, 0, 0x1000),
            "unknown-width clean byte was mistaken for a wholly clean read");
      Add(*state, origin, 25, 8, false);
      // Even intervals beyond the bounded sample must affect classification.
      for (unsigned i = 0; i < 4; ++i) RecordDownload(0x1070 + 4 * i, 0x1074 + 4 * i);
      RecordDownload(0x1080, 0x1084);
      Check(origin.downloads.count == 6 && origin.downloads.fault_dirty && !origin.downloads.read_dirty,
            "unknown-width dirty byte or truncated range evidence was lost");
      Add(*state, origin, 30, 9, false);
    }
    Check(state->entries[2].unknown_width == 2 && state->entries[2].dirty_fault_byte == 1 &&
              state->entries[2].false_shared == 0 && state->entries[2].last.role == 1,
          "delegated fault evidence was misclassified or lost its requester");
    // An indirect record's exact fault byte survives exception attribution,
    // while nested/unrelated loads must not inherit the packet annotation.
    Origin record;
    record.kind = Kind::IndirectRecords;
    record.address = record.record_address = 0x3000;
    record.count_address = 0x5000;
    record.bytes = 20;
    record.record_index = 2; record.record_stride = 32; record.record_count = 5;
    record.vertex_register = 0x94; record.instance_register = 0x95;
    record.index_register = 0x280; record.indexed = true;
    for (uint64_t offset = 0; offset < record.bytes; ++offset) {
      const auto fault = FaultOrigin(record, 0xabcdef, record.address + offset);
      Check(fault.kind == Kind::IndirectRecords && fault.address == record.address + offset &&
                fault.bytes == 0 && fault.rip == 0xabcdef && fault.record_address == record.address &&
                fault.record_index == 2 && fault.record_stride == 32 && fault.record_count == 5 &&
                fault.vertex_register == 0x94 && fault.instance_register == 0x95 && fault.indexed &&
                fault.count_address == 0x5000,
            "indirect record fault lost packet context or invented a read width");
    }
    for (const uint64_t address : {uint64_t{0x2fff}, uint64_t{0x3014}, uint64_t{0x9000}}) {
      const auto fault = FaultOrigin(record, 0xabcdef, address);
      Check(fault.kind == Kind::Fault && fault.record_address == 0 && fault.bytes == 0,
            "unrelated fault inherited indirect record attribution");
    }
    for (const auto kind : {Kind::IndirectCount, Kind::IndirectInstances}) {
      Origin scalar; scalar.kind = kind; scalar.address = 0x4000; scalar.bytes = 4;
      Check(FaultOrigin(scalar, 0xabcdef, 0x4003).kind == kind &&
                FaultOrigin(scalar, 0xabcdef, 0x4004).kind == Kind::Fault,
            "counter/instance fault lost its four-byte boundary");
    }
    auto record_fault = FaultOrigin(record, 0xabcdef, 0x3010);
    record_fault.bytes = 4; // Width supplied by the instruction decoder.
    Add(*state, record_fault, 10, 9, false);
    Downloads clean;
    clean.Observe(0x1080, 4, 0x1080, true, 0x1000, 0x1040);
    Check(clean.FalseShared(0x1080, 4, 0x1000) && !clean.FalseShared(0x1fff, 4, 0x1000),
          "whole-read evidence accepted a cross-page or overlapping read");
    clean.Observe(0x1080, 4, 0x1080, true, 0x1083, 0x1084);
    Check(clean.read_dirty && !clean.fault_dirty && !clean.FalseShared(0x1080, 4, 0x1000),
          "clean first byte hid an overlapping tail of a decoded read");
    {
      OriginScope scope(reader);
      Common::SubmitProfile::SetRecording(false);
      RecordDownload(0x1080, 0x1084);
      Check(origin.downloads.count == 0, "closed profile gate recorded a download");
    }
    Report(*state, 5'000'000'001ull); // Also exercise the bounded JSON evidence report.
    for (unsigned i = 0; i < 2048; ++i) state->writers.Add({.address = 0x2000, .bytes = 4});
    Check(!state->writers.Find(0x1000, 4).sequence, "expired writer history survived wrap");
    ResetWindow(*state, 123, 4);
    Check(state->calls == 0 && state->ns == 0 && state->start == 123 && state->generation == 4,
          "readback attribution window did not reset");
    std::puts("ReadbackOrigins: all cases passed");
    return 0;
  }
  TestSrtDescriptorProbe();
  TestWalkerLoadsBypassSpecializationReader();
  TestIdentityResolutionPreservesTerminalValue();
  TestNativeNestedSrtRefresh();
  TestIntegerRuntimeValueFollowsSrtReads();
  TestSrtAliasesShareReads();
  TestUniformVectorDescriptorRead();
  TestExactReciprocalDescriptorArithmetic();
  TestUnbasedFlatCacheHitMaterializes();
  for (const bool memory_condition : {false, true})
    TestWrittenDescriptorPredicateReads(memory_condition);
  TestFailedMaterializationRejectsStage();
  TestWritableBufferControlProjection();
  TestSharedControlPredicates();
  TestNestedExecPredicatesReuseReads();
  TestFiniteImageRefreshReusesScalarReads();
  for (const bool independent_mask : {false, true})
    TestEmptyTraversalSkipsUnmappedDescriptor(independent_mask);
  TestEmptyTraversalSkipsUnmappedDescriptor(false, true);
  TestCyclicControlPredicatesRemainUnknown();
  TestBackEdgeReachesPhiPredecessor();
  TestEmptyScalarBufferSpecialization();
  TestMixedSamplerVariantsShareRuntimeDescriptor();
  TestSignedStorageSpecialization();
  for (const bool mixed : {false, true}) TestIndirectImageSamplerClasses(mixed);
  TestShaderCacheRecords();
  std::puts("ResourceMaterializationTests: all cases passed");
  return 0;
}

// Keep this focused standalone target self-contained by amalgamating its small
// typed-IR implementation set.
#include "graphics/shader/recompiler/ir/Block.cpp"
#include "graphics/shader/recompiler/ir/Program.cpp"
#include "graphics/shader/recompiler/ir/Type.cpp"
#include "graphics/shader/recompiler/ir/Value.cpp"
#include "graphics/shader/recompiler/ir/opcodes/ValueOpcodes.cpp"
