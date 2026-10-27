#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include "common/abi.h"
#include "common/assert.h"
#include "common/renderStageProfile.h"
#include "common/readbackProfile.h"
#include "common/submitProfile.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <xbyak/xbyak.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <deque>
#include <map>
#include <unordered_set>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::IR {

SrtRuntime SrtReadCapture::ObservedRuntime() {
	auto runtime = m_source;
	runtime.userdata = this;
	runtime.read_specialization_memory = m_source.read_specialization_memory != nullptr
	                                         ? Read : nullptr;
	return runtime;
}

bool SrtReadCapture::Read(void* userdata, uint64_t address, std::span<uint32_t> values) {
	auto& capture = *static_cast<SrtReadCapture*>(userdata);
	if (!capture.m_source.read_specialization_memory(capture.m_source.userdata, address, values)) {
		return false;
	}
	capture.m_ranges.emplace_back(address, values.size_bytes());
	return true;
}

namespace {

bool IsRawRead(const ResourcePlan& values, const Inst& inst) {
	const auto op = inst.GetOpcode();
	if (op != ValueOpcode::LoadAddressU32 && op != ValueOpcode::ReadConstBuffer &&
	    op != ValueOpcode::LoadBufferU32) {
		return false;
	}
	const auto index = inst.Flags<MemoryFlags>().index;
	if (index >= values.memory_info.size()) {
		return false;
	}
	const auto& memory = values.memory_info[index];
	if (op != ValueOpcode::LoadBufferU32) {
		return (op == ValueOpcode::LoadAddressU32 && memory.kind == ResourceKind::ScalarAddress) ||
		       (op == ValueOpcode::ReadConstBuffer && memory.kind == ResourceKind::ScalarBuffer);
	}
	if (inst.NumArgs() != 5u || memory.kind != ResourceKind::Buffer || memory.typed ||
	    memory.formatted || memory.coherent || memory.data_bits != 32u ||
	    memory.data_dwords != 1u || memory.idxen || memory.offen || memory.offset != 0u ||
	    inst.Arg(4).GetType() != Type::U1) {
		return false;
	}
	for (uint32_t arg = 1; arg <= 3; ++arg) {
		if (inst.Arg(arg).Resolve() != Value(0u)) return false;
	}
	return true;
}

bool IsDescriptorHandle(ValueOpcode opcode) {
	switch (opcode) {
		case ValueOpcode::GetBufferResource:
		case ValueOpcode::GetAddressResource:
		case ValueOpcode::GetImageResource:
		case ValueOpcode::GetSamplerResource: return true;
		default: return false;
	}
}

bool IsRuntimeSelect(ValueOpcode op) {
	return op == ValueOpcode::SelectU1 || op == ValueOpcode::SelectU32 ||
	       op == ValueOpcode::SelectF32;
}

bool IsRuntimeUniformOp(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::ConditionRef:
		case ValueOpcode::BitCastU32F32:
		case ValueOpcode::BitCastF32U32:
		case ValueOpcode::ConvertU32F32:
		case ValueOpcode::ConvertF32U32:
		case ValueOpcode::CompositeConstructU64:
		case ValueOpcode::CompositeExtractU64:
		case ValueOpcode::CompositeConstructU32x2:
		case ValueOpcode::CompositeExtractU32x2:
		case ValueOpcode::BitFieldInsert:
		case ValueOpcode::BitFieldUExtract:
		case ValueOpcode::BitFieldSExtract:
		case ValueOpcode::IAdd32:
		case ValueOpcode::IAdd64:
		case ValueOpcode::ISub32:
		case ValueOpcode::ISub64:
		case ValueOpcode::IMul32:
		case ValueOpcode::UMulHi:
		case ValueOpcode::UMin32:
		case ValueOpcode::UMax32:
		case ValueOpcode::ShiftLeftLogical32:
		case ValueOpcode::ShiftLeftLogical64:
		case ValueOpcode::ShiftRightLogical32:
		case ValueOpcode::ShiftRightLogical64:
		case ValueOpcode::ShiftRightArithmetic32:
		case ValueOpcode::ShiftRightArithmetic64:
		case ValueOpcode::BitwiseAnd32:
		case ValueOpcode::BitwiseAnd64:
		case ValueOpcode::BitwiseOr32:
		case ValueOpcode::BitwiseXor32:
		case ValueOpcode::BitwiseNot32:
		case ValueOpcode::SelectU1:
		case ValueOpcode::SelectU32:
		case ValueOpcode::SelectF32:
		case ValueOpcode::ULessThan32:
		case ValueOpcode::ULessThanEqual32:
		case ValueOpcode::IEqual32:
		case ValueOpcode::IEqual64:
		case ValueOpcode::UGreaterThan32:
		case ValueOpcode::SGreaterThanEqual32:
		case ValueOpcode::INotEqual32:
		case ValueOpcode::INotEqual64:
		case ValueOpcode::LogicalOr:
		case ValueOpcode::LogicalAnd:
		case ValueOpcode::LogicalXor:
		case ValueOpcode::LogicalNot:
		case ValueOpcode::FPOrdLessThanEqual32:
		case ValueOpcode::FPOrdGreaterThanEqual32:
		case ValueOpcode::FPIsNan32:
		case ValueOpcode::FPMul32:
		case ValueOpcode::FPRecipIFlag32:
		case ValueOpcode::FPTrunc32: return true;
		default: return false;
	}
}

bool ImmediateBits(Value value, uint64_t& bits) {
	switch (value.GetType()) {
		case Type::U1: bits = value.U1(); return true;
		case Type::U8: bits = value.U8(); return true;
		case Type::U16: bits = value.U16(); return true;
		case Type::U32: bits = value.U32(); return true;
		case Type::U64: bits = value.U64(); return true;
		case Type::F32: bits = std::bit_cast<uint32_t>(value.F32Value()); return true;
		default: return false;
	}
}

// Scratch layout in bytes: per descriptor source its eight DWORDs and a known DWORD, one
// activity byte per source, then one qword per walked value (all ones when unknown). The
// walker's value slots, captured addresses, reachability and states follow.
constexpr uint32_t DescriptorStride = 9u * sizeof(uint32_t);

uint32_t ActiveOffset(const ResourcePlan& plan) {
	return static_cast<uint32_t>(plan.descriptor_sources.size()) * DescriptorStride;
}

uint32_t OutputOffset(const ResourcePlan& plan) {
	return (ActiveOffset(plan) + static_cast<uint32_t>(plan.descriptor_sources.size()) + 7u) & ~7u;
}

const uint8_t* Scratch(const ResourcePlan& plan) {
	return reinterpret_cast<const uint8_t*>(plan.srt_scratch.data());
}

thread_local const ResourcePlan* t_running_walker = nullptr;
using CleanReader = bool (*)(uint64_t, void*, uint64_t);
thread_local CleanReader t_clean_reader = nullptr;
// Direct-mapped hints populated by the first clean fault. A stale/colliding
// hint only costs a live check; it never authorizes a read or stores guest data.
constexpr uint32_t CleanPageSlots = 256;
thread_local std::array<uint64_t, CleanPageSlots> t_clean_pages {};

uint64_t KYTY_SYSV_ABI ReadCleanWord(uint64_t address) {
	uint32_t word = 0;
	return SrtWalkerTryReadClean(address, &word, sizeof(word)) ? (uint64_t {1} << 32u) | word : 0;
}

// Every guest load is mov eax, [r10] (41 8b 02), so a fault completes it as a zero read.
uint32_t LoadGuest(Xbyak::CodeGenerator& code, const Xbyak::RegExp& address, Xbyak::Label& clean_load) {
	code.lea(code.r10, code.ptr[address]);
	Xbyak::Label direct, done;
	code.mov(code.r11, code.r10);
	code.shr(code.r11, 12);
	code.inc(code.r11); // Empty hint (zero) cannot match the null page.
	code.mov(code.rcx, code.r11);
	code.and_(code.ecx, CleanPageSlots - 1);
	code.cmp(code.qword[code.r12 + code.rcx * 8], code.r11);
	code.jne(direct);
	code.call(clean_load);
	code.bt(code.rax, 32);
	code.jnc(direct);
	code.mov(code.eax, code.eax);
	code.jmp(done);
	code.L(direct);
	const auto offset = static_cast<uint32_t>(code.getSize());
	code.mov(code.eax, code.dword[code.r10]);
	code.L(done);
	return offset;
}

// Compile-time proof only. packed_stride is not specialized yet here, so use
// the retained descriptor expressions rather than its default metadata value.
bool ReadBeyondFixedWrites(const ResourcePlan& program, const Inst& inst,
                           const MemoryInfo& memory, const BufferResource& buffer) {
	if (inst.GetOpcode() != ValueOpcode::ReadConstBuffer || inst.NumArgs() != 2 ||
	    memory.kind != ResourceKind::ScalarBuffer || buffer.write_extent == 0 ||
	    buffer.write_extent == UINT32_MAX || buffer.source >= program.descriptor_sources.size()) return false;
	const auto& source = program.descriptor_sources[buffer.source];
	if (source.dword_count != 4 || source.indirect_descriptor) return false;
	const auto low = source.dwords[0].Resolve();
	const auto high = source.dwords[1].Resolve();
	const auto flags = source.dwords[3].Resolve();
	const auto offset = inst.Arg(1).Resolve();
	for (const auto value: {low, high, flags, offset}) {
		if (!value.IsImmediate() || value.GetType() != Type::U32) return false;
	}
	// Scalar loads align the base/offset independently. Fixed vector writes
	// need the same aligned base, without ADD_TID or swizzled addressing.
	if ((low.U32() & 3u) != 0 || (high.U32() & (1u << 31u)) != 0 ||
	    (flags.U32() & (1u << 23u)) != 0) return false;
	const auto begin = int64_t {offset.U32() & ~3u} +
	    (static_cast<int64_t>(static_cast<int32_t>(memory.offset)) & ~int64_t {3});
	// Avoid U32 address arithmetic wrap and negative immediate offsets.
	return begin >= buffer.write_extent && begin <= int64_t {UINT32_MAX} - 3;
}

class RuntimeValidator {
public:
	explicit RuntimeValidator(const ResourcePlan& program, RuntimeValueType type,
	                          bool wave_uniform = false)
	    : m_program(program), m_type(type), m_wave_uniform(wave_uniform) {}

	bool Run(Value value) { return Validate(value); }

private:
	bool ValidateArguments(const Inst& inst, bool require_uniform) {
		for (size_t index = 0; index < inst.NumArgs(); index++) {
			if (!Validate(inst.Arg(index), require_uniform)) return false;
		}
		return true;
	}

	bool Validate(Value value, bool require_uniform = true) {
		value = value.Resolve();
		// Host floating-point evaluation does not model shader rounding/denormal modes.
		if (m_type != RuntimeValueType::Any &&
		    TypesOverlap(value.GetType(), Type::F16 | Type::F32 | Type::F32x2)) {
			return false;
		}
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			if (!require_uniform) return true;
			switch (value.GetType()) {
				case Type::U1:
				case Type::U8:
				case Type::U16:
				case Type::U32:
				case Type::U64:
				case Type::F32: return true;
				default: return false;
			}
		}
		if (require_uniform && !m_active_mask.IsEmpty() && value == m_active_mask) return true;
		// Integer-only dependency checks do not depend on the active EXEC mask.
		if (!require_uniform && m_validated_dependencies.contains(inst)) return true;
		if (!m_visiting.insert(inst).second) {
			return !require_uniform;
		}
		const auto finish = [&](bool valid) {
			m_visiting.erase(inst);
			if (valid && !require_uniform) m_validated_dependencies.insert(inst);
			return valid;
		};
		const auto op = inst->GetOpcode();
		if (m_type == RuntimeValueType::ReadOnlyControlInteger &&
		    (op == ValueOpcode::LoadBufferU32 || op == ValueOpcode::ReadConstBuffer)) {
			const auto index = inst->Flags<MemoryFlags>().index;
			if (index >= m_program.memory_info.size()) return finish(false);
			const auto& memory = m_program.memory_info[index];
			// Control projection is optional. A GPU read that can overlap a write
			// must not become a host decision that drops resource branches.
			// Planning-only reads remain mandatory SRT dependencies: the renderer's
			// overlap check still rejects them if they alias a write.
			if (!memory.planning_only &&
			    (memory.kind == ResourceKind::Buffer || memory.kind == ResourceKind::ScalarBuffer ||
			     memory.kind == ResourceKind::IndirectBuffer) &&
			    memory.resource < m_program.info.buffers.size() &&
			    m_program.info.buffers[memory.resource].written &&
			    !ReadBeyondFixedWrites(m_program, *inst, memory,
			                           m_program.info.buffers[memory.resource])) return finish(false);
		}
		if (m_type == RuntimeValueType::ImmutableInteger &&
		    (op == ValueOpcode::ReadConst || BufferAccessOf(op) != BufferAccess::None ||
		     AddressOpcodeInfoOf(op).access != AddressAccess::None ||
		     ImageOpcodeInfoOf(op).access != ImageAccess::None ||
		     SharedAccessOf(op) != SharedAccess::None)) {
			return finish(false);
		}
		if (op == ValueOpcode::ReadConst) {
			const auto slot = inst->NumArgs() == 2 ? inst->Arg(1).Resolve() : Value {};
			if (inst->NumArgs() != 2 || inst->Arg(0).Resolve().TryInstruction() == nullptr ||
			    inst->Arg(0).Resolve().TryInstruction()->GetOpcode() !=
			        ValueOpcode::GetSrtResource ||
			    !slot.IsImmediate() || slot.GetType() != Type::U32 ||
			    slot.U32() >= m_program.srt_reads.size()) {
				return finish(false);
			}
			if (m_type != RuntimeValueType::Any) {
				const auto active_mask = m_active_mask;
				m_active_mask          = {};
				const bool valid       = Validate(m_program.srt_reads[slot.U32()].value);
				m_active_mask          = active_mask;
				if (!valid) return finish(false);
			}
		}
		if (!require_uniform) return finish(ValidateArguments(*inst, false));
		if (!m_active_mask.IsEmpty() && IsRuntimeSelect(op) && inst->NumArgs() == 3 &&
		    inst->Arg(0).Resolve() == m_active_mask) {
			// Empty EXEC reads lane zero, so ignored operands still require integer types.
			if (m_type != RuntimeValueType::Any && !Validate(inst->Arg(2), false)) {
				return finish(false);
			}
			return finish(Validate(inst->Arg(1)));
		}
		if (op == ValueOpcode::UndefU1 || op == ValueOpcode::UndefU32 || op == ValueOpcode::Void) {
			return finish(false);
		}
		if (op == ValueOpcode::GetUserData) {
			if (inst->NumArgs() != 1 || inst->Arg(0).GetType() != Type::ScalarReg) {
				return finish(false);
			}
			const auto reg = RegIndex(inst->Arg(0).ScalarRegister());
			if (reg < m_program.user_data_base ||
			    reg - m_program.user_data_base >= m_program.user_data_count) {
				return finish(false);
			}
			return finish(true);
		}
		if (op == ValueOpcode::GetShaderBase) {
			if (inst->NumArgs() != 0) {
				return finish(false);
			}
			return finish(true);
		}
		if (m_wave_uniform && op == ValueOpcode::GetBuiltin) {
			const auto axis = inst->NumArgs() == 2u ? inst->Arg(1).Resolve() : Value {};
			return finish(m_program.stage == ShaderType::Compute && inst->NumArgs() == 2u &&
			              inst->Arg(0) == Value(static_cast<uint32_t>(StageInputKind::WorkgroupId)) &&
			              axis.IsImmediate() && axis.GetType() == Type::U32 && axis.U32() < 3u);
		}
		if (op == ValueOpcode::Phi) {
			if (m_type != RuntimeValueType::Any && !ValidateArguments(*inst, false)) {
				return finish(false);
			}
			const auto invariant = ResolveInvariantPhi(m_program, value);
			if (invariant.IsEmpty()) {
				return finish(false);
			}
			return finish(Validate(invariant));
		}
		if (op == ValueOpcode::ReadFirstLane) {
			if (inst->NumArgs() != 2 || inst->Arg(0).GetType() != Type::U32 ||
			    inst->Arg(1).GetType() != Type::U1) {
				return finish(false);
			}
			if (m_type != RuntimeValueType::Any && !Validate(inst->Arg(1), false)) {
				return finish(false);
			}
			const auto active_mask = m_active_mask;
			m_active_mask          = inst->Arg(1).Resolve();
			const bool valid       = Validate(inst->Arg(0));
			m_active_mask          = active_mask;
			return finish(valid);
		}
		if (op == ValueOpcode::GetSrtResource) {
			if (inst->NumArgs() != 0) {
				return finish(false);
			}
			return finish(true);
		}
		if (op == ValueOpcode::LoadAddressU32 || op == ValueOpcode::ReadConstBuffer ||
		    op == ValueOpcode::LoadBufferU32) {
			const auto  expected = op == ValueOpcode::LoadAddressU32
			                           ? ValueOpcode::GetAddressResource
			                           : ValueOpcode::GetBufferResource;
			const auto* handle = inst->NumArgs() != 0 ? inst->Arg(0).ResolveInstruction() : nullptr;
			if (!IsRawRead(m_program, *inst) || handle == nullptr ||
			    handle->GetOpcode() != expected) {
				return finish(false);
			}
			if (op == ValueOpcode::LoadBufferU32) {
				const auto guard = inst->Arg(4).Resolve();
				return finish(Validate(inst->Arg(0)) &&
				              ((!m_active_mask.IsEmpty() && guard == m_active_mask) || Validate(guard)));
			}
		} else if (op == ValueOpcode::CompositeExtractU64) {
			const auto index = inst->NumArgs() == 2 ? inst->Arg(1).Resolve() : Value {};
			if (!index.IsImmediate() || index.GetType() != Type::U32 || index.U32() >= 2u) {
				return finish(false);
			}
		} else if (op == ValueOpcode::CompositeExtractU32x2) {
			const auto* source = inst->NumArgs() == 2 ? inst->Arg(0).ResolveInstruction() : nullptr;
			const auto  index  = inst->NumArgs() == 2 ? inst->Arg(1).Resolve() : Value {};
			if (source == nullptr || !index.IsImmediate() || index.GetType() != Type::U32 ||
			    index.U32() >= 2u || source->GetOpcode() != ValueOpcode::CompositeConstructU32x2) {
				return finish(false);
			}
		}
		if (IsDescriptorHandle(op)) {
			size_t expected = 4u;
			if (op == ValueOpcode::GetImageResource) {
				expected = 8u;
			} else if (op == ValueOpcode::GetAddressResource) {
				expected = 2u;
			}
			if (inst->NumArgs() != expected) {
				return finish(false);
			}
		} else if (op != ValueOpcode::ReadConst && op != ValueOpcode::ReadConstBuffer &&
		           op != ValueOpcode::LoadAddressU32 && !IsRuntimeUniformOp(op)) {
			return finish(false);
		}
		return finish(ValidateArguments(*inst, true));
	}

	const ResourcePlan&             m_program;
	RuntimeValueType                m_type;
	bool                            m_wave_uniform;
	Value                           m_active_mask;
	std::unordered_set<const Inst*> m_visiting;
	std::unordered_set<const Inst*> m_validated_dependencies;
};


// Emits one x86 function per plan. Each value, per EXEC-mask context, is a memoized thunk with a
// state byte (0 unevaluated, 1 unknown or in progress, 2 known, 3 a control Phi holding its first
// value) and a qword slot; a reference is `call thunk; cmp state, 2`. Operands that are unknown,
// constant or plain user data are folded at emission. Registers: rdi user data, rsi flat buffer,
// rdx scratch, r8 capture cursor.
class WalkerCompiler {
public:
	WalkerCompiler(ResourcePlan& plan, Xbyak::CodeGenerator& code):
	    m_plan(plan), m_code(code) {}

	void Compile(std::span<const Value> values);

private:
	enum class Kind : uint8_t { Unknown, Constant, UserData, ShaderBase, Node };
	struct Operand {
		Kind     kind  = Kind::Unknown;
		uint64_t value = 0;

		bool operator==(const Operand& other) const = default;
	};
	// masked: the operand depends on its context's EXEC mask, so it is not the root context's.
	struct Ref {
		Operand operand;
		bool    masked = false;
	};
	struct Node {
		const Inst*          inst    = nullptr;
		uint32_t             context = 0;
		Operand              result;
		std::vector<Operand> args;
		uint32_t             slot = 0, state = 0;
		Xbyak::Label         label;
		bool                 pending = true, referenced = false, masked = false;
		uint32_t uses = 0, flat_slot = UINT32_MAX, descriptor = UINT32_MAX;
		uint32_t control_slot = UINT32_MAX;
	};

	static constexpr auto Near = Xbyak::CodeGenerator::T_NEAR;

	bool Thunk(uint32_t index) const {
		const auto& node = m_nodes[index];
		return node.referenced || node.result == Operand {Kind::Node, index};
	}
	static bool MayBeUnknown(const Operand& operand) {
		return operand.kind == Kind::Unknown || operand.kind == Kind::Node;
	}

	uint32_t Context(Value mask) {
		mask = mask.Resolve();
		const auto found = std::ranges::find(m_masks, mask);
		if (found != m_masks.end()) return static_cast<uint32_t>(found - m_masks.begin());
		m_masks.push_back(mask);
		return static_cast<uint32_t>(m_masks.size() - 1u);
	}

	void AddOutput(Value value, uint32_t gate);
	void MarkUses(Operand root, uint32_t uses, uint32_t index = UINT32_MAX) {
		std::vector<Operand> pending {root};
		while (!pending.empty()) {
			const auto operand = pending.back(); pending.pop_back();
			if (operand.kind != Kind::Node) continue;
			auto& node = m_nodes[operand.value];
			if (node.uses & uses) continue;
			node.uses |= uses;
			if (uses == 2) node.flat_slot = index;
			if (uses == 4) node.descriptor = index;
			pending.insert(pending.end(), node.args.begin(), node.args.end());
		}
	}
	void Consume(std::vector<std::vector<uint32_t>>& gates);
	Ref Evaluate(Value value, uint32_t context);
	Operand Analyze(uint32_t index);
	Operand AnalyzeRead(Node& node, const auto& arg, Operand self);
	void Layout();
	void Gate(std::vector<uint32_t>& gates, const Xbyak::Label& skip);
	void Zero(uint32_t offset, uint32_t size);
	void Branch(const Operand& operand, const Xbyak::Label& unknown);
	void Load(const Xbyak::Reg64& reg, const Operand& operand);
	void Fetch(const Operand& operand, const Xbyak::Label& unknown);
	void Set(void (Xbyak::CodeGenerator::*condition)(const Xbyak::Operand&));
	void Store(const Node& node);
	void EmitNode(uint32_t index);
	void EmitRead(const Node& node);
	void EmitStrict(const Node& node);

	ResourcePlan&                                     m_plan;
	Xbyak::CodeGenerator&                             m_code;
	std::vector<Value>                                m_masks {Value {}};
	std::deque<Node>                                  m_nodes;
	std::map<std::pair<const Inst*, uint32_t>, uint32_t> m_memo;
	std::vector<Value>                                m_outputs;
	std::vector<std::vector<uint32_t>>                m_output_gates;
	Xbyak::Label                                      m_ret;
	Xbyak::Label                                      m_clean_load;
	uint32_t m_shader_base = 0, m_reach = 0, m_states = 0, m_states_end = 0;
	uint32_t m_user_data = 0, m_captures = 0;
};

void WalkerCompiler::AddOutput(Value value, uint32_t gate) {
	const auto* inst = value.Resolve().TryInstruction();
	if (inst == nullptr) return;
	auto index = inst->WalkOutput();
	if (index >= m_outputs.size() || m_outputs[index].TryInstruction() != inst) {
		index = static_cast<uint32_t>(m_outputs.size());
		inst->SetWalkOutput(index);
		m_outputs.push_back(value.Resolve());
		m_output_gates.emplace_back();
	}
	m_output_gates[index].push_back(gate);
}

// Mirrors MaterializeResources: each descriptor it reads back is gated by the activity of the
// resource root that needs it, and so are an indirect root's selector values.
void WalkerCompiler::Consume(std::vector<std::vector<uint32_t>>& gates) {
	const auto& sources = m_plan.descriptor_sources;
	const auto  consume = [&](uint32_t root) {
		if (root >= sources.size()) return;
		const auto& indirect = sources[root].indirect_descriptor;
		if (!indirect) {
			gates[root].push_back(root);
			return;
		}
		const auto add = [&](uint32_t source) {
			if (source < sources.size()) gates[source].push_back(root);
		};
		if (indirect->sources.empty()) {
			add(indirect->table_source);
			if (indirect->selector) {
				add(indirect->selector->source);
				if (indirect->selector->rows) add(indirect->selector->rows->source);
			}
		}
		for (const auto source: indirect->sources) add(source);
		AddOutput(indirect->key_count, root);
		AddOutput(indirect->selector_first, root);
		AddOutput(indirect->selector_mask, root);
	};
	for (const auto& buffer: m_plan.info.buffers) consume(buffer.source);
	for (const auto& image: m_plan.info.images) consume(image.source);
	for (const auto& sampler: m_plan.info.samplers) {
		const auto* indirect = sampler.source < sources.size() && sources[sampler.source].indirect_descriptor
		    ? &*sources[sampler.source].indirect_descriptor : nullptr;
		if (indirect == nullptr || indirect->sources.empty()) {
			consume(sampler.source);
		} else if (indirect->sources[0] < sources.size()) {
			gates[indirect->sources[0]].push_back(indirect->sources[0]);
		}
	}
	const auto& fill = m_plan.uniform_fill;
	if (fill.fill.words != 0) {
		AddOutput(fill.pattern_size, UINT32_MAX);
		AddOutput(fill.element_count, UINT32_MAX);
		for (const auto value: fill.values) AddOutput(value, UINT32_MAX);
	}
}

WalkerCompiler::Ref WalkerCompiler::Evaluate(Value value, uint32_t context) {
	value = value.Resolve();
	if (value.IsEmpty()) return {};
	if (value.IsImmediate()) {
		uint64_t bits = 0;
		return ImmediateBits(value, bits) ? Ref {{Kind::Constant, bits}} : Ref {};
	}
	const auto mask = m_masks[context];
	if (context != 0 && value == mask) return {{Kind::Constant, 1u}, true};
	const auto* inst = value.TryInstruction();
	if (inst == nullptr) return {};
	const auto op = inst->GetOpcode();
	if (context != 0 && IsRuntimeSelect(op) && inst->NumArgs() == 3 && inst->Arg(0).Resolve() == mask) {
		return {Evaluate(inst->Arg(1), context).operand, true};
	}
	switch (op) {
		case ValueOpcode::BitCastU32F32:
		case ValueOpcode::BitCastF32U32:
		case ValueOpcode::ConditionRef: return Evaluate(inst->Arg(0), context);
		case ValueOpcode::CompositeExtractU32x2: {
			const auto* source = inst->Arg(0).ResolveInstruction();
			const auto  index  = inst->Arg(1).Resolve();
			if (source == nullptr || source->GetOpcode() != ValueOpcode::CompositeConstructU32x2 ||
			    !index.IsImmediate() || index.GetType() != Type::U32 || index.U32() >= 2u) return {};
			return Evaluate(source->Arg(index.U32()), context);
		}
		case ValueOpcode::ReadFirstLane:
			// The lane read does not depend on the EXEC mask it is read under.
			return {Evaluate(inst->Arg(0), Context(inst->Arg(1))).operand};
		case ValueOpcode::GetUserData: {
			const auto reg = RegIndex(inst->Arg(0).ScalarRegister());
			if (reg < m_plan.user_data_base || reg - m_plan.user_data_base >= m_plan.user_data_count) return {};
			return {{Kind::UserData, reg - m_plan.user_data_base}};
		}
		case ValueOpcode::GetShaderBase: return {{Kind::ShaderBase}};
		case ValueOpcode::Phi:
			if (!m_plan.control_phi_predecessors.contains(inst)) {
				const auto invariant = ResolveInvariantPhi(m_plan, value);
				return invariant.IsEmpty() ? Ref {} : Evaluate(invariant, context);
			}
			break;
		default: break;
	}
	const auto [found, inserted] = m_memo.try_emplace({inst, context}, static_cast<uint32_t>(m_nodes.size()));
	const auto index = found->second;
	if (!inserted) {
		auto& node = m_nodes[index];
		if (!node.pending) return {node.result, node.masked};
		// A cycle: the thunk reads as unknown while it runs.
		node.referenced = true;
		return {{Kind::Node, index}, context != 0};
	}
	auto& node   = m_nodes.emplace_back();
	node.inst    = inst;
	node.context = context;
	node.result  = Analyze(index);
	node.pending = false;
	if (context != 0 && !node.masked && !node.referenced) {
		node.result = Evaluate(value, 0).operand;
		return {node.result};
	}
	return {node.result, node.masked};
}

WalkerCompiler::Operand WalkerCompiler::Analyze(uint32_t index) {
	auto&       node = m_nodes[index];
	const auto& inst = *node.inst;
	const auto  self = Operand {Kind::Node, index};
	const auto  arg  = [&](Value value) {
		const auto ref = Evaluate(value, node.context);
		node.masked |= ref.masked;
		return ref.operand;
	};
	const auto constant = [](uint64_t value) { return Operand {Kind::Constant, value}; };
	switch (inst.GetOpcode()) {
		case ValueOpcode::Phi: {
			bool known = false;
			for (size_t i = 0; i < inst.NumArgs(); ++i) {
				node.args.push_back(arg(inst.Arg(i)));
				known |= node.args.back().kind != Kind::Unknown;
			}
			return known ? self : Operand {};
		}
		case ValueOpcode::LoadAddressU32:
		case ValueOpcode::ReadConstBuffer:
		case ValueOpcode::LoadBufferU32: return AnalyzeRead(node, arg, self);
		case ValueOpcode::SelectU1:
		case ValueOpcode::SelectU32:
		case ValueOpcode::SelectF32: {
			const auto predicate = arg(inst.Arg(0));
			if (predicate.kind == Kind::Unknown) return {};
			if (predicate.kind == Kind::Constant) return arg(inst.Arg(predicate.value != 0u ? 1 : 2));
			node.args = {predicate, arg(inst.Arg(1)), arg(inst.Arg(2))};
			return node.args[1].kind == Kind::Unknown && node.args[2].kind == Kind::Unknown ? Operand {} : self;
		}
		case ValueOpcode::ULessThan32:
		case ValueOpcode::ULessThanEqual32: {
			// No U32 is below zero and every U32 is at least zero, including an unknown loop-carried
			// or varying lane value. args[0] is that zero-tested bound.
			const bool less  = inst.GetOpcode() == ValueOpcode::ULessThan32;
			const auto bound = arg(inst.Arg(less ? 1 : 0));
			if (bound.kind == Kind::Unknown) return {};
			if (bound.kind == Kind::Constant && static_cast<uint32_t>(bound.value) == 0u) return constant(!less);
			node.args = {bound, arg(inst.Arg(less ? 0 : 1))};
			return node.args[1].kind == Kind::Unknown && bound.kind == Kind::Constant ? Operand {} : self;
		}
		case ValueOpcode::LogicalOr:
		case ValueOpcode::LogicalAnd: {
			// A deciding operand, nonzero for Or and zero for And, fixes the result.
			const bool conjunction = inst.GetOpcode() == ValueOpcode::LogicalAnd;
			const auto decides     = [&](const Operand& x) {
				return x.kind == Kind::Constant && (x.value == 0u) == conjunction;
			};
			const auto a = arg(inst.Arg(0));
			if (decides(a)) return constant(!conjunction);
			const auto b = arg(inst.Arg(1));
			if (decides(b)) return constant(!conjunction);
			if (a.kind == Kind::Constant && b.kind == Kind::Constant) return constant(conjunction);
			if (a.kind == Kind::Constant && b.kind == Kind::Unknown) return {};
			// Only lanes satisfying the left EXEC factor can make a conjunction true, so an
			// unknown left side retries the right one under that factor as its mask.
			Operand refined;
			if (conjunction && MayBeUnknown(a)) {
				refined = Evaluate(inst.Arg(1), Context(inst.Arg(0))).operand;
				if (refined == b || (refined.kind == Kind::Constant && refined.value != 0u)) refined = {};
				if (a.kind == Kind::Unknown && refined.kind == Kind::Constant) return constant(0u);
			}
			if (a.kind == Kind::Unknown && refined.kind == Kind::Unknown &&
			    (b.kind == Kind::Unknown || b.kind == Kind::Constant)) return {};
			node.args = {a, b, refined};
			return self;
		}
		case ValueOpcode::CompositeExtractU64: {
			const auto component = inst.Arg(1).Resolve();
			if (!component.IsImmediate() || component.GetType() != Type::U32 || component.U32() >= 2u) return {};
			[[fallthrough]];
		}
		default:
			// The other runtime-uniform ops are strict; a U32x2 pair is only read through its extracts.
			if (!IsRuntimeUniformOp(inst.GetOpcode()) || inst.GetOpcode() == ValueOpcode::CompositeConstructU32x2)
				return {};
			for (size_t i = 0; i < inst.NumArgs(); ++i) {
				node.args.push_back(arg(inst.Arg(i)));
				if (node.args.back().kind == Kind::Unknown) return {};
			}
			return self;
	}
}

WalkerCompiler::Operand WalkerCompiler::AnalyzeRead(Node& node, const auto& arg, Operand self) {
	const auto& inst = *node.inst;
	const auto* handle = inst.Arg(0).ResolveInstruction();
	if (!IsRawRead(m_plan, inst) || handle == nullptr) return {};
	const bool vector = inst.GetOpcode() == ValueOpcode::LoadBufferU32;
	if (vector) {
		const auto guard = arg(inst.Arg(4));
		if (guard.kind == Kind::Unknown) return {};
		if (guard.kind == Kind::Constant && guard.value == 0u) return {Kind::Constant, 0u};
		node.args.push_back(guard);
	}
	const bool buffer = inst.GetOpcode() != ValueOpcode::LoadAddressU32;
	const auto offset = static_cast<int32_t>(m_plan.memory_info[inst.Flags<MemoryFlags>().index].offset);
	bool failed = buffer && (handle->NumArgs() != 4u || offset < 0);
	for (const auto& word: {handle->Arg(0), handle->Arg(1), inst.Arg(1)}) {
		node.args.push_back(arg(word));
		failed |= node.args.back().kind == Kind::Unknown;
	}
	for (uint32_t word = 2; buffer && !failed && word < 4u; ++word) {
		node.args.push_back(arg(handle->Arg(word)));
		failed |= node.args.back().kind == Kind::Unknown;
	}
	if (!failed) return self;
	// A disabled vector lane still reads zero.
	if (!vector || node.args[0].kind == Kind::Constant) return {};
	node.args.resize(1);
	return self;
}

void WalkerCompiler::Layout() {
	uint32_t offset = OutputOffset(m_plan) + static_cast<uint32_t>(m_outputs.size()) * 8u;
	m_shader_base = offset;
	offset += 8u;
	for (uint32_t index = 0; index < m_nodes.size(); ++index) {
		if (!Thunk(index)) continue;
		m_nodes[index].slot = offset;
		offset += 8u;
	}
	m_reach = offset;
	offset += static_cast<uint32_t>(m_plan.control_flow.size());
	m_states = offset;
	for (uint32_t index = 0; index < m_nodes.size(); ++index) {
		if (Thunk(index)) m_nodes[index].state = offset++;
	}
	m_states_end = offset;
	// Captured addresses come last; EmitRead counts them.
	m_plan.srt_captures = (offset + 7u) & ~7u;
}

// Falls through when any gating source is active. The list is not empty.
void WalkerCompiler::Gate(std::vector<uint32_t>& gates, const Xbyak::Label& skip) {
	auto& c = m_code;
	std::ranges::sort(gates);
	if (gates.back() == UINT32_MAX) return;
	gates.erase(std::unique(gates.begin(), gates.end()), gates.end());
	Xbyak::Label active;
	for (size_t i = 0; i < gates.size(); ++i) {
		c.cmp(c.byte[c.rdx + ActiveOffset(m_plan) + gates[i]], 0);
		if (i + 1u < gates.size()) {
			c.jne(active, Near);
		} else {
			c.je(skip, Near);
		}
	}
	c.L(active);
}

void WalkerCompiler::Zero(uint32_t offset, uint32_t size) {
	auto& c = m_code;
	if (size == 0) return;
	c.mov(c.r11, c.rdi);
	c.lea(c.rdi, c.ptr[c.rdx + offset]);
	c.mov(c.ecx, size);
	c.xor_(c.eax, c.eax);
	c.rep();
	c.stosb();
	c.mov(c.rdi, c.r11);
}

// Jumps to unknown unless the operand has a value at run time.
void WalkerCompiler::Branch(const Operand& operand, const Xbyak::Label& unknown) {
	auto& c = m_code;
	if (operand.kind == Kind::Unknown) {
		c.jmp(unknown, Near);
	} else if (operand.kind == Kind::Node) {
		auto& node = m_nodes[operand.value];
		c.call(node.label);
		c.cmp(c.byte[c.rdx + node.state], 2);
		c.jne(unknown, Near);
	}
}

void WalkerCompiler::Load(const Xbyak::Reg64& reg, const Operand& operand) {
	auto& c = m_code;
	switch (operand.kind) {
		case Kind::Constant: c.mov(reg, operand.value); break;
		case Kind::UserData:
			m_user_data = std::max(m_user_data, static_cast<uint32_t>(operand.value) + 1u);
			c.mov(reg.cvt32(), c.dword[c.rdi + operand.value * sizeof(uint32_t)]);
			break;
		case Kind::ShaderBase: c.mov(reg, c.qword[c.rdx + m_shader_base]); break;
		case Kind::Node: c.mov(reg, c.qword[c.rdx + m_nodes[operand.value].slot]); break;
		case Kind::Unknown: break;
	}
}

// Loads the operand into rax, or jumps to unknown when it has no value at run time.
void WalkerCompiler::Fetch(const Operand& operand, const Xbyak::Label& unknown) {
	Branch(operand, unknown);
	Load(m_code.rax, operand);
}

// Sets eax to 1 when the condition holds, else 0.
void WalkerCompiler::Set(void (Xbyak::CodeGenerator::*condition)(const Xbyak::Operand&)) {
	(m_code.*condition)(m_code.al);
	m_code.movzx(m_code.eax, m_code.al);
}

void WalkerCompiler::Store(const Node& node) {
	auto& c = m_code;
	c.mov(c.qword[c.rdx + node.slot], c.rax);
	c.mov(c.byte[c.rdx + node.state], 2);
	c.ret();
}

void WalkerCompiler::EmitNode(uint32_t index) {
	auto&       c    = m_code;
	auto&       node = m_nodes[index];
	const auto& args = node.args;
	c.L(node.label);
	c.cmp(c.byte[c.rdx + node.state], 0);
	c.jne(m_ret, Near);
	c.mov(c.byte[c.rdx + node.state], 1);
	if (node.result != Operand {Kind::Node, index}) {
		// A cycle referenced this value before it folded to another operand.
		Fetch(node.result, m_ret);
		Store(node);
		return;
	}
	switch (node.inst->GetOpcode()) {
		case ValueOpcode::Phi: {
			// Only predecessors reached by this walk contribute, and they must agree. The state is 3
			// once one of them has stored the value; callers read it like 1, as unknown.
			const auto& blocks = m_plan.control_phi_predecessors.at(node.inst);
			const auto  state  = c.byte[c.rdx + node.state];
			for (size_t i = 0; i < args.size(); ++i) {
				Xbyak::Label next, first;
				c.cmp(c.byte[c.rdx + m_reach + blocks[i]], 0);
				c.je(next, Near);
				Fetch(args[i], m_ret);
				c.cmp(state, 1);
				c.je(first, Near);
				c.cmp(c.rax, c.qword[c.rdx + node.slot]);
				c.jne(m_ret, Near);
				c.jmp(next, Near);
				c.L(first);
				c.mov(c.qword[c.rdx + node.slot], c.rax);
				c.mov(state, 3);
				c.L(next);
			}
			c.cmp(state, 3);
			c.jne(m_ret, Near);
			c.mov(state, 2);
			c.ret();
			return;
		}
		case ValueOpcode::LoadAddressU32:
		case ValueOpcode::ReadConstBuffer:
		case ValueOpcode::LoadBufferU32: EmitRead(node); return;
		case ValueOpcode::SelectU1:
		case ValueOpcode::SelectU32:
		case ValueOpcode::SelectF32: {
			Xbyak::Label other;
			Fetch(args[0], m_ret);
			c.test(c.rax, c.rax);
			c.jz(other, Near);
			Fetch(args[1], m_ret);
			Store(node);
			c.L(other);
			Fetch(args[2], m_ret);
			Store(node);
			return;
		}
		case ValueOpcode::ULessThan32:
		case ValueOpcode::ULessThanEqual32: {
			const bool less = node.inst->GetOpcode() == ValueOpcode::ULessThan32;
			Xbyak::Label bound_zero;
			Fetch(args[0], m_ret);
			c.test(c.eax, c.eax);
			c.jz(bound_zero, Near);
			Branch(args[1], m_ret);
			Load(c.rax, args[0]);
			Load(c.rcx, args[1]);
			// ULT tests other < bound, ULE bound <= other.
			c.cmp(c.ecx, c.eax);
			Set(less ? &Xbyak::CodeGenerator::setb : &Xbyak::CodeGenerator::setae);
			Store(node);
			c.L(bound_zero);
			c.mov(c.eax, uint32_t {!less});
			Store(node);
			return;
		}
		case ValueOpcode::LogicalOr:
		case ValueOpcode::LogicalAnd: {
			const bool conjunction = node.inst->GetOpcode() == ValueOpcode::LogicalAnd;
			Xbyak::Label unknown_left, refine, decided;
			const auto decide = [&] {
				if (conjunction) {
					c.jz(decided, Near);
				} else {
					c.jnz(decided, Near);
				}
			};
			Branch(args[0], unknown_left);
			if (args[0].kind != Kind::Unknown) {
				Load(c.rax, args[0]);
				c.test(c.rax, c.rax);
				decide();
				Fetch(args[1], m_ret);
				c.test(c.rax, c.rax);
				Set(&Xbyak::CodeGenerator::setne);
				Store(node);
			}
			c.L(unknown_left);
			if (MayBeUnknown(args[0])) {
				// Without the left side only a deciding right side, or a conjunction's zero
				// refined right side, is known.
				if (args[1].kind != Kind::Unknown) {
					Fetch(args[1], refine);
					c.test(c.rax, c.rax);
					decide();
				}
				c.L(refine);
				Fetch(args[2], m_ret);
				c.test(c.rax, c.rax);
				c.jnz(m_ret, Near);
			}
			c.L(decided);
			c.mov(c.eax, uint32_t {!conjunction});
			Store(node);
			return;
		}
		default: EmitStrict(node); return;
	}
}

// Raw scalar and uniform DWORD buffer loads; out-of-bounds reads are zero.
void WalkerCompiler::EmitRead(const Node& node) {
	auto&      c    = m_code;
	if (node.control_slot != UINT32_MAX) {
		Xbyak::Label original;
		// Unknown, not zero: resource reachability must follow both GPU branches.
		c.test(c.r13, c.r13);
		c.jz(original, Near);
		c.cmp(c.byte[c.r13 + node.control_slot], 0);
		c.jne(m_ret, Near);
		c.L(original);
	}
	const auto& inst = *node.inst;
	const auto op = inst.GetOpcode();
	const bool vector = op == ValueOpcode::LoadBufferU32;
	const auto load = [&](const Xbyak::RegExp& address) {
		const auto offset = LoadGuest(c, address, m_clean_load);
		m_plan.walker_read_sites.push_back({offset, node.uses, node.flat_slot, node.descriptor, node.inst, node.control_slot});
	};
	auto args = std::span(node.args);
	Xbyak::Label zero;
	if (vector) {
		Fetch(args[0], m_ret);
		c.test(c.rax, c.rax);
		c.jz(zero, Near);
		args = args.subspan(1);
		if (args.empty()) c.jmp(m_ret, Near);
	}
	if (!args.empty()) {
		for (const auto& arg: args) Branch(arg, m_ret);
		const auto immediate =
		    static_cast<int64_t>(static_cast<int32_t>(m_plan.memory_info[inst.Flags<MemoryFlags>().index].offset)) &
		    ~int64_t {3};
		// rax = the 48-bit base (high << 32 | low) with its low two bits clear.
		Load(c.rax, args[1]);
		c.shl(c.rax, 48);
		c.shr(c.rax, 16);
		Load(c.rcx, args[0]);
		c.and_(c.ecx, ~3u);
		c.or_(c.rax, c.rcx);
		Load(c.r9, args[2]);
		c.and_(c.r9d, ~3u);
		if (op == ValueOpcode::LoadAddressU32) {
			c.lea(c.rax, c.ptr[c.rax + c.r9 + static_cast<size_t>(immediate)]);
			c.mov(c.rcx, c.rax);
			c.shr(c.rcx, 48);
			c.jnz(m_ret, Near);
			load(c.rax);
		} else {
			Load(c.r11, args[1]);
			c.shr(c.r11d, 16);
			if (vector) {
				// Only the uniform, unswizzled structured DWORD address is evaluated on the host.
				c.test(c.r11d, 1u << 15u);
				c.jnz(m_ret, Near);
				Load(c.rcx, args[4]);
				c.test(c.ecx, (1u << 23u) | 0xf0000000u);
				c.jnz(m_ret, Near);
				c.test(c.r11d, 0x3fffu);
				c.jz(zero, Near);
				c.test(c.ecx, 0x7fu << 12u);
				c.jz(zero, Near);
			}
			c.and_(c.r11d, 0x3fffu);
			Load(c.rcx, args[3]);
			if (vector) {
				c.test(c.ecx, c.ecx);
				c.jz(zero, Near);
			}
			Xbyak::Label sized;
			c.test(c.r11d, c.r11d);
			c.jz(sized, Near);
			c.imul(c.rcx, c.r11);
			c.L(sized);
			c.lea(c.r11, c.ptr[c.r9 + static_cast<size_t>(immediate) + 4u]);
			c.cmp(c.r11, c.rcx);
			c.ja(zero, Near);
			load(c.rax + c.r9 + static_cast<size_t>(immediate));
		}
		++m_captures;
		c.mov(c.qword[c.r8], c.r10);
		c.add(c.r8, 8);
		Store(node);
	}
	c.L(zero);
	c.xor_(c.eax, c.eax);
	Store(node);
}

void WalkerCompiler::EmitStrict(const Node& node) {
	auto&       c    = m_code;
	const auto& inst = *node.inst;
	for (const auto& arg: node.args) Branch(arg, m_ret);
	const std::array regs {c.rax, c.rcx, c.r9, c.r11};
	for (size_t i = 0; i < node.args.size() && i < regs.size(); ++i) Load(regs[i], node.args[i]);
	// Field ops require offset + width <= 32. Leaves (1 << width) - 1 in r11, the offset in
	// cl and the former rcx (the offset or the inserted value) in r10.
	const auto field = [&](const Xbyak::Reg32& offset, const Xbyak::Reg32& width) {
		c.lea(c.r10, c.ptr[offset.cvt64() + width.cvt64()]);
		c.cmp(c.r10, 32);
		c.ja(m_ret, Near);
		c.mov(c.r10, c.rcx);
		c.mov(c.ecx, width);
		c.mov(c.r11d, 1);
		c.shl(c.r11, c.cl);
		c.dec(c.r11);
		c.mov(c.ecx, offset.getIdx() == Xbyak::Operand::RCX ? c.r10d : offset);
	};
	switch (inst.GetOpcode()) {
		case ValueOpcode::CompositeConstructU64:
			c.shl(c.rcx, 32);
			c.or_(c.rax, c.rcx);
			break;
		case ValueOpcode::CompositeExtractU64:
			if (inst.Arg(1).Resolve().U32() != 0u) c.shr(c.rax, 32);
			c.mov(c.eax, c.eax);
			break;
		case ValueOpcode::IAdd32: c.add(c.eax, c.ecx); break;
		case ValueOpcode::IAdd64: c.add(c.rax, c.rcx); break;
		case ValueOpcode::ISub32: c.sub(c.eax, c.ecx); break;
		case ValueOpcode::ISub64: c.sub(c.rax, c.rcx); break;
		case ValueOpcode::IMul32: c.imul(c.eax, c.ecx); break;
		case ValueOpcode::UMulHi:
			c.imul(c.rax, c.rcx);
			c.shr(c.rax, 32);
			break;
		case ValueOpcode::UMin32:
			c.cmp(c.eax, c.ecx);
			c.cmova(c.eax, c.ecx);
			break;
		case ValueOpcode::UMax32:
			c.cmp(c.eax, c.ecx);
			c.cmovb(c.eax, c.ecx);
			break;
		case ValueOpcode::ConvertF32U32:
			c.cvtsi2ss(c.xmm0, c.rax);
			c.movd(c.eax, c.xmm0);
			break;
		case ValueOpcode::ConvertU32F32: {
			// The conversion saturates and maps NaN to zero.
			Xbyak::Label done;
			c.movd(c.xmm0, c.eax);
			c.xorps(c.xmm1, c.xmm1);
			c.xor_(c.eax, c.eax);
			c.ucomiss(c.xmm0, c.xmm1);
			c.jbe(done, Near);
			c.mov(c.ecx, 0x4f800000u);
			c.movd(c.xmm1, c.ecx);
			c.mov(c.eax, UINT32_MAX);
			c.ucomiss(c.xmm0, c.xmm1);
			c.jae(done, Near);
			c.cvttss2si(c.rax, c.xmm0);
			c.L(done);
			break;
		}
		case ValueOpcode::FPMul32:
			c.movd(c.xmm0, c.eax);
			c.movd(c.xmm1, c.ecx);
			c.mulss(c.xmm0, c.xmm1);
			c.movd(c.eax, c.xmm0);
			break;
		case ValueOpcode::FPTrunc32:
			c.movd(c.xmm0, c.eax);
			c.roundss(c.xmm0, c.xmm0, 3);
			c.movd(c.eax, c.xmm0);
			break;
		case ValueOpcode::FPRecipIFlag32:
			// Normal positive powers of two have exact normal reciprocals in every FP mode.
			c.test(c.eax, 0x807fffffu);
			c.jnz(m_ret, Near);
			c.shr(c.eax, 23);
			c.jz(m_ret, Near);
			c.cmp(c.eax, 254);
			c.jae(m_ret, Near);
			c.neg(c.eax);
			c.add(c.eax, 254);
			c.shl(c.eax, 23);
			break;
		case ValueOpcode::FPIsNan32:
			c.and_(c.eax, 0x7fffffffu);
			c.cmp(c.eax, 0x7f800000u);
			Set(&Xbyak::CodeGenerator::seta);
			break;
		case ValueOpcode::FPOrdLessThanEqual32:
		case ValueOpcode::FPOrdGreaterThanEqual32:
			if (inst.Flags<FPCompareFlags>().flush_input_denorms) {
				for (const auto& reg: {c.eax, c.ecx}) {
					Xbyak::Label normal;
					c.mov(c.r11d, reg);
					c.and_(c.r11d, 0x7fffffffu);
					c.cmp(c.r11d, 0x00800000u);
					c.jae(normal, Near);
					c.and_(reg, 0x80000000u);
					c.L(normal);
				}
			}
			c.movd(c.xmm0, c.eax);
			c.movd(c.xmm1, c.ecx);
			// Unordered operands set CF, so AE is false for NaN.
			if (inst.GetOpcode() == ValueOpcode::FPOrdLessThanEqual32) {
				c.ucomiss(c.xmm1, c.xmm0);
			} else {
				c.ucomiss(c.xmm0, c.xmm1);
			}
			Set(&Xbyak::CodeGenerator::setae);
			break;
		case ValueOpcode::BitwiseAnd32: c.and_(c.eax, c.ecx); break;
		case ValueOpcode::BitwiseAnd64: c.and_(c.rax, c.rcx); break;
		case ValueOpcode::BitwiseOr32: c.or_(c.eax, c.ecx); break;
		case ValueOpcode::BitwiseXor32: c.xor_(c.eax, c.ecx); break;
		case ValueOpcode::BitwiseNot32: c.not_(c.eax); break;
		case ValueOpcode::ShiftLeftLogical32: c.shl(c.eax, c.cl); break;
		case ValueOpcode::ShiftLeftLogical64: c.shl(c.rax, c.cl); break;
		case ValueOpcode::ShiftRightLogical32: c.shr(c.eax, c.cl); break;
		case ValueOpcode::ShiftRightLogical64: c.shr(c.rax, c.cl); break;
		case ValueOpcode::ShiftRightArithmetic32: c.sar(c.eax, c.cl); break;
		case ValueOpcode::ShiftRightArithmetic64: c.sar(c.rax, c.cl); break;
		case ValueOpcode::BitFieldUExtract:
			field(c.ecx, c.r9d);
			c.shr(c.rax, c.cl);
			c.and_(c.rax, c.r11);
			break;
		case ValueOpcode::BitFieldSExtract: {
			Xbyak::Label done;
			field(c.ecx, c.r9d);
			c.shr(c.rax, c.cl);
			c.and_(c.rax, c.r11);
			// Width zero tests bit 63, which is clear.
			c.lea(c.ecx, c.ptr[c.r9 - 1]);
			c.bt(c.rax, c.rcx);
			c.jnc(done, Near);
			c.not_(c.r11);
			c.or_(c.rax, c.r11);
			c.mov(c.eax, c.eax);
			c.L(done);
			break;
		}
		case ValueOpcode::BitFieldInsert:
			field(c.r9d, c.r11d);
			c.shl(c.r11, c.cl);
			c.shl(c.r10, c.cl);
			c.and_(c.r10, c.r11);
			c.not_(c.r11);
			c.and_(c.rax, c.r11);
			c.or_(c.rax, c.r10);
			c.mov(c.eax, c.eax);
			break;
		case ValueOpcode::IEqual32:
			c.cmp(c.eax, c.ecx);
			Set(&Xbyak::CodeGenerator::sete);
			break;
		case ValueOpcode::INotEqual32:
			c.cmp(c.eax, c.ecx);
			Set(&Xbyak::CodeGenerator::setne);
			break;
		case ValueOpcode::IEqual64:
			c.cmp(c.rax, c.rcx);
			Set(&Xbyak::CodeGenerator::sete);
			break;
		case ValueOpcode::INotEqual64:
			c.cmp(c.rax, c.rcx);
			Set(&Xbyak::CodeGenerator::setne);
			break;
		case ValueOpcode::UGreaterThan32:
			c.cmp(c.eax, c.ecx);
			Set(&Xbyak::CodeGenerator::seta);
			break;
		case ValueOpcode::SGreaterThanEqual32:
			c.cmp(c.eax, c.ecx);
			Set(&Xbyak::CodeGenerator::setge);
			break;
		case ValueOpcode::LogicalXor:
			c.test(c.rax, c.rax);
			c.setne(c.al);
			c.test(c.rcx, c.rcx);
			c.setne(c.cl);
			c.xor_(c.al, c.cl);
			c.movzx(c.eax, c.al);
			break;
		case ValueOpcode::LogicalNot:
			c.test(c.rax, c.rax);
			Set(&Xbyak::CodeGenerator::sete);
			break;
		default: EXIT("unexpected SRT walker opcode\n");
	}
	Store(node);
}

void WalkerCompiler::Compile(std::span<const Value> values) {
	auto&       c         = m_code;
	const auto& sources   = m_plan.descriptor_sources;
	const auto& blocks    = m_plan.control_flow;
	const bool  resources = values.empty();
	std::vector<std::vector<uint32_t>> gates(sources.size());
	if (resources) Consume(gates);
	for (const auto value: values) AddOutput(value, UINT32_MAX);
	// Analyze every root first, so thunk bodies only reuse finished operands.
	std::vector<Operand>              conditions, reads, outputs;
	std::vector<std::vector<Operand>> descriptors(sources.size());
	if (resources) {
		for (const auto& block: blocks) conditions.push_back(Evaluate(block.condition, 0).operand);
		for (const auto& read: m_plan.srt_reads) reads.push_back(Evaluate(read.value, 0).operand);
		for (uint32_t source = 0; source < sources.size(); ++source) {
			if (gates[source].empty()) continue;
			for (uint32_t word = 0; word < sources[source].dword_count; ++word)
				descriptors[source].push_back(Evaluate(sources[source].dwords[word], 0).operand);
		}
	}
	for (const auto value: m_outputs) outputs.push_back(Evaluate(value, 0).operand);
	{
		for (const auto root: conditions) MarkUses(root, 1);
		for (uint32_t i = 0; i < reads.size(); ++i) MarkUses(reads[i], 2, i);
		for (uint32_t i = 0; i < descriptors.size(); ++i)
			for (const auto root: descriptors[i]) MarkUses(root, 4, i);
		for (const auto root: outputs) MarkUses(root, 8);
	}
	{
		// Only optional control loads can learn to stay on the GPU. Descriptor,
		// flattened SRT and explicit output dependencies must always be read.
		for (auto& node: m_nodes) {
			if (node.uses != 1 || !IsRawRead(m_plan, *node.inst) ||
			    m_plan.memory_info[node.inst->Flags<MemoryFlags>().index].planning_only) continue;
			node.control_slot = static_cast<uint32_t>(m_plan.walker_control_disabled.size());
			m_plan.walker_control_disabled.push_back(0);
		}
	}
	Layout();

	Xbyak::Label pass, fail, reset;
	c.mov(c.qword[c.rdx + m_shader_base], c.rcx);
	Zero(m_reach, m_states_end - m_reach);
	c.lea(c.r8, c.ptr[c.rdx + m_plan.srt_captures]);
	if (!resources) {
		for (uint32_t block = 0; block < blocks.size(); ++block) c.mov(c.byte[c.rdx + m_reach + block], 1);
	} else if (!blocks.empty()) {
		// Reachability grows monotonically. A newly reached predecessor of an evaluated
		// control Phi restarts the walk with fresh values; a retreating edge resumes at its target.
		std::vector<std::vector<uint32_t>> rechecks(blocks.size());
		for (uint32_t index = 0; index < m_nodes.size(); ++index) {
			const auto& node = m_nodes[index];
			if (node.inst->GetOpcode() != ValueOpcode::Phi || node.result != Operand {Kind::Node, index}) continue;
			for (const auto block: m_plan.control_phi_predecessors.at(node.inst)) rechecks[block].push_back(node.state);
		}
		std::vector<Xbyak::Label> starts(blocks.size());
		c.mov(c.byte[c.rdx + m_reach], 1);
		c.L(pass);
		for (uint32_t block = 0; block < blocks.size(); ++block) {
			Xbyak::Label next;
			const auto mark = [&](uint32_t target) {
				Xbyak::Label reached;
				c.cmp(c.byte[c.rdx + m_reach + target], 0);
				c.jne(reached, Near);
				c.mov(c.byte[c.rdx + m_reach + target], 1);
				for (const auto state: rechecks[target]) {
					c.cmp(c.byte[c.rdx + state], 0);
					c.jne(reset, Near);
				}
				if (target <= block) c.jmp(starts[target], Near);
				c.L(reached);
			};
			const auto& successors = blocks[block].successors;
			const auto  condition  = conditions[block];
			c.L(starts[block]);
			c.cmp(c.byte[c.rdx + m_reach + block], 0);
			c.je(next, Near);
			if (condition.kind == Kind::Constant) {
				mark(successors[condition.value != 0u ? 0 : 1]);
			} else {
				if (condition.kind != Kind::Unknown) {
					Xbyak::Label both, other;
					Fetch(condition, both);
					c.test(c.rax, c.rax);
					c.jz(other, Near);
					mark(successors[0]);
					c.jmp(next, Near);
					c.L(other);
					mark(successors[1]);
					c.jmp(next, Near);
					c.L(both);
				}
				for (const auto successor: successors) mark(successor);
			}
			c.L(next);
		}
	}
	if (resources) {
		const auto flat = [&](uint32_t slot) {
			const auto offset = slot < reads.size() ? m_plan.srt_reads[slot].flat_offset : UINT32_MAX;
			if (offset >= reads.size()) {
				c.jmp(fail, Near);
				return;
			}
			Fetch(reads[slot], fail);
			c.mov(c.dword[c.rsi + offset * sizeof(uint32_t)], c.eax);
		};
		if (blocks.empty()) {
			for (uint32_t slot = 0; slot < reads.size(); ++slot) flat(slot);
		}
		for (uint32_t block = 0; block < blocks.size(); ++block) {
			if (blocks[block].srt_reads.empty()) continue;
			Xbyak::Label skip;
			c.cmp(c.byte[c.rdx + m_reach + block], 0);
			c.je(skip, Near);
			for (const auto slot: blocks[block].srt_reads) flat(slot);
			c.L(skip);
		}
		// A source no block lists is always active.
		for (uint32_t source = 0; source < sources.size(); ++source) {
			bool listed = false;
			for (uint32_t block = 0; block < blocks.size(); ++block) {
				if (std::ranges::find(blocks[block].sources, source) == blocks[block].sources.end()) continue;
				if (listed) {
					c.or_(c.al, c.byte[c.rdx + m_reach + block]);
				} else {
					c.mov(c.al, c.byte[c.rdx + m_reach + block]);
				}
				listed = true;
			}
			const auto active = c.byte[c.rdx + ActiveOffset(m_plan) + source];
			if (listed) {
				c.mov(active, c.al);
			} else {
				c.mov(active, 1);
			}
		}
		for (uint32_t source = 0; source < sources.size(); ++source) {
			if (gates[source].empty()) continue;
			Xbyak::Label skip;
			const auto known = c.dword[c.rdx + source * DescriptorStride + 8u * sizeof(uint32_t)];
			c.mov(known, 0);
			Gate(gates[source], skip);
			for (uint32_t word = 0; word < descriptors[source].size(); ++word) {
				Fetch(descriptors[source][word], skip);
				c.mov(c.dword[c.rdx + source * DescriptorStride + word * sizeof(uint32_t)], c.eax);
			}
			c.mov(known, 1);
			c.L(skip);
		}
	}
	for (uint32_t index = 0; index < outputs.size(); ++index) {
		Xbyak::Label skip;
		const auto output = c.qword[c.rdx + OutputOffset(m_plan) + index * 8u];
		c.mov(output, -1);
		Gate(m_output_gates[index], skip);
		Fetch(outputs[index], skip);
		c.mov(c.eax, c.eax);
		c.mov(output, c.rax);
		c.L(skip);
	}
	c.mov(c.rax, c.r8);
	c.L(m_ret);
	c.ret();
	c.L(fail);
	c.xor_(c.eax, c.eax);
	c.ret();
	if (resources && !blocks.empty()) {
		c.L(reset);
		Zero(m_states, m_states_end - m_states);
		c.lea(c.r8, c.ptr[c.rdx + m_plan.srt_captures]);
		c.jmp(pass, Near);
	}
	for (uint32_t index = 0; index < m_nodes.size(); ++index) {
		if (Thunk(index)) EmitNode(index);
	}
	c.L(m_clean_load);
	// Thunks have variable nesting depth. Preserve walker registers, then
	// align explicitly before calling C++; a miss resumes the original JIT
	// mov, preserving its fault/zero-read contract and captured address.
	for (const auto reg: {c.rcx, c.rdx, c.rsi, c.rdi, c.r8, c.r9, c.r10, c.r11}) c.push(reg);
	c.mov(c.r11, c.rsp);
	c.and_(c.rsp, -16);
	c.sub(c.rsp, 16);
	c.mov(c.qword[c.rsp], c.r11);
	c.mov(c.rdi, c.r10);
	c.mov(c.rax, reinterpret_cast<uint64_t>(&ReadCleanWord));
	c.call(c.rax);
	c.mov(c.rsp, c.qword[c.rsp]);
	for (const auto reg: {c.r11, c.r10, c.r9, c.r8, c.rdi, c.rsi, c.rdx, c.rcx}) c.pop(reg);
	c.ret();
	m_plan.srt_scratch.assign(m_plan.srt_captures / 8u + m_captures, 0u);
	m_plan.srt_user_data = m_user_data;
}

} // namespace

void CompileSrtWalker(ResourcePlan& plan, std::span<const Value> values) {
	plan.walker_read_sites.clear();
	plan.walker_control_disabled.clear();
	plan.walker_control_rejected = false;
	plan.walker_control_history = {};
	plan.srt_code = std::make_unique<Xbyak::CodeGenerator>(4096, Xbyak::AutoGrow);
	auto& c = *plan.srt_code;
	// One body. r12 holds page hints across thunks and the C++ clean-read
	// helper; preserve the caller's callee-saved register around the walk.
	Xbyak::Label body;
	c.push(c.r12);
	c.mov(c.r12, c.r8);
	c.push(c.r13);
	c.mov(c.r13, c.r9);
	c.call(body);
	c.pop(c.r13);
	c.pop(c.r12);
	c.ret();
	c.L(body);
	WalkerCompiler(plan, c).Compile(values);
	c.readyRE();
	EXIT_IF(Xbyak::GetError() != 0);
}

template <bool Profile>
static bool WalkSrtImpl(const ResourcePlan& plan, const SrtRuntime& runtime, std::vector<uint32_t>& flat,
                        std::vector<std::pair<uint64_t, uint64_t>>* reads) {
	using namespace Common::RenderStageProfile;
	if (plan.srt_code == nullptr || runtime.user_data.size() < plan.srt_user_data) return false;
	flat.assign(plan.srt_reads.size(), 0u);
	using Walker = const uint64_t* KYTY_SYSV_ABI (*)(const uint32_t*, uint32_t*, uint64_t*, uint64_t, const uint64_t*, const uint8_t*);
	const uint64_t* end;
	{
		[[maybe_unused]] ProgramScope<Profile> profile {ProgramStage::SrtNative};
		const auto* previous_walker = std::exchange(t_running_walker, &plan);
		const auto previous_reader = std::exchange(t_clean_reader, runtime.read_clean_memory);
		const auto* controls = !plan.walker_control_disabled.empty() && !plan.walker_control_rejected &&
		    !Common::SubmitProfile::ExperimentActive(Common::SubmitProfile::Experiment::NoWalkerControlGpu)
		    ? plan.walker_control_disabled.data() : nullptr;
		// A vector of eligible sites alone is not an active promotion.
		if (controls && plan.walker_control_history.learned_sites != 0)
			Common::SubmitProfile::AddExtra(Common::SubmitProfile::Extra::WalkerControlWalks);
		end = plan.srt_code->getCode<Walker>()(runtime.user_data.data(), flat.data(),
		    plan.srt_scratch.data(), runtime.shader_base, t_clean_pages.data(), controls);
		t_clean_reader = previous_reader;
		t_running_walker = previous_walker;
	}
	if (end == nullptr) return false;
	if (reads != nullptr) {
		[[maybe_unused]] ProgramScope<Profile> profile {ProgramStage::SrtCapture};
		for (auto* read = plan.srt_scratch.data() + plan.srt_captures / 8u; read != end; ++read)
			reads->emplace_back(*read, sizeof(uint32_t));
	}
	return true;
}

bool WalkSrt(const ResourcePlan& plan, const SrtRuntime& runtime, std::vector<uint32_t>& flat,
             std::vector<std::pair<uint64_t, uint64_t>>* reads) {
	using namespace Common::RenderStageProfile;
	if (!Detail::Enabled()) return WalkSrtImpl<false>(plan, runtime, flat, reads);
	Scope stage_profile {Stage::ScalarTables};
	if (Detail::DetailedEnabled())
		return WalkSrtImpl<true>(plan, runtime, flat, reads);
	return WalkSrtImpl<false>(plan, runtime, flat, reads);
}

bool WalkedValue(const ResourcePlan& plan, Value value, uint32_t& result) {
	value = value.Resolve();
	uint64_t bits = 0;
	if (value.IsImmediate()) {
		if (!ImmediateBits(value, bits)) return false;
		result = static_cast<uint32_t>(bits);
		return true;
	}
	const auto* inst = value.TryInstruction();
	if (inst == nullptr) return false;
	EXIT_IF(inst->WalkOutput() == UINT32_MAX);
	std::memcpy(&bits, Scratch(plan) + OutputOffset(plan) + inst->WalkOutput() * 8u, sizeof(bits));
	result = static_cast<uint32_t>(bits);
	return bits <= UINT32_MAX;
}

bool WalkedDescriptor(const ResourcePlan& plan, uint32_t source, DescriptorValue& result) {
	if (source >= plan.descriptor_sources.size()) return false;
	std::array<uint32_t, 9> words {};
	std::memcpy(words.data(), Scratch(plan) + source * DescriptorStride, DescriptorStride);
	if (words[8] == 0u) return false;
	result = {};
	result.dword_count = plan.descriptor_sources[source].dword_count;
	std::copy_n(words.begin(), result.dword_count, result.dwords.begin());
	return true;
}

bool WalkedActive(const ResourcePlan& plan, uint32_t source) {
	return Scratch(plan)[ActiveOffset(plan) + source] != 0u;
}

bool ValidateRuntimeValue(const ResourcePlan& program, Value value, RuntimeValueType type) {
	return RuntimeValidator(program, type).Run(value);
}

bool ValidateWaveUniformValue(const ResourcePlan& program, Value value) {
	return RuntimeValidator(program, RuntimeValueType::Any, true).Run(value);
}

uint32_t SrtWalkerLoadLength(uint64_t rip) {
	if (t_running_walker == nullptr ||
	    t_running_walker->srt_code->getSize() < 3 ||
	    rip - reinterpret_cast<uint64_t>(t_running_walker->srt_code->getCode()) > t_running_walker->srt_code->getSize() - 3)
		return 0;
	const auto* code = reinterpret_cast<const uint8_t*>(rip);
	return code[0] == 0x41u && code[1] == 0x8bu && code[2] == 0x02u ? 3 : 0;
}

bool NoteSrtControlReadback(uint64_t rip) {
	using namespace Common::SubmitProfile;
	if (!SrtWalkerLoadLength(rip)) return false;
	const auto& plan = *t_running_walker;
	const auto offset = rip - reinterpret_cast<uint64_t>(plan.srt_code->getCode());
	const auto site = std::lower_bound(plan.walker_read_sites.begin(), plan.walker_read_sites.end(), offset,
	    [](const ResourcePlan::WalkerReadSite& site, uint64_t key) { return site.offset < key; });
	if (site == plan.walker_read_sites.end() || site->offset != offset || site->control_slot == UINT32_MAX) return false;
	auto& disabled = plan.walker_control_disabled[site->control_slot];
	// Keep learning during the fallback cooldown too. Handling/delegating the
	// readback has already completed on the walker owner before this callback.
	if (!disabled) {
		++plan.walker_control_history.learned_sites;
		AddExtra(Extra::WalkerControlPromotions);
	}
	disabled = 1;
	return true;
}

bool DescribeSrtWalkerRead(uint64_t rip, Common::ReadbackProfile::Origin& origin) {
	if (!SrtWalkerLoadLength(rip)) return false;
	const auto& plan = *t_running_walker;
	const auto offset = rip - reinterpret_cast<uint64_t>(plan.srt_code->getCode());
	const auto site = std::lower_bound(plan.walker_read_sites.begin(), plan.walker_read_sites.end(), offset,
	    [](const ResourcePlan::WalkerReadSite& site, uint64_t key) { return site.offset < key; });
	if (site == plan.walker_read_sites.end() || site->offset != offset) return false;
	origin.kind = Common::ReadbackProfile::Kind::Walker;
	origin.hash = plan.shader_hash; origin.stage = static_cast<uint32_t>(plan.stage);
	origin.jit_offset = site->offset; origin.uses = site->uses;
	origin.flat_slot = site->flat_slot; origin.descriptor = site->descriptor;
	origin.opcode = ValueOpcodeName(site->inst->GetOpcode()).data();
	const auto flags = site->inst->Flags<MemoryFlags>();
	origin.pc = flags.pc; origin.memory = flags.index;
	if (flags.index < plan.memory_info.size()) {
		const auto& memory = plan.memory_info[flags.index];
		origin.resource = memory.resource; origin.memory_kind = static_cast<uint32_t>(memory.kind);
		origin.immediate = memory.offset; origin.planning_only = memory.planning_only;
	}
	return true;
}

bool SrtWalkerTryReadClean(uint64_t address, void* data, uint64_t bytes) {
	if (t_clean_reader == nullptr || bytes != sizeof(uint32_t)) return false;
	const auto tag = (address >> 12u) + 1u;
	auto& hint = t_clean_pages[tag & (CleanPageSlots - 1)];
	const bool copied = t_clean_reader(address, data, bytes);
	if (copied) hint = tag;
	else if (hint == tag) hint = 0; // Dirty/ordinary page: resume direct loads until its next clean fault.
	using namespace Common::SubmitProfile;
	AddExtra(copied ? Extra::WalkerCleanReads : Extra::WalkerCleanFallbacks);
	return copied;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
