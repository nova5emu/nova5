// Ported instruction fixtures from Nova5 VirtualMemoryAllocationTests.cpp.
// Host-only harness: real patcher/emulator, VirtualAlloc memory, no GPU or game.
#include "common/abi.h"
#include "common/assert.h"
#include "common/virtualMemory.h"
#include "common/termination.h"
#include "common/memoryWriteWatch.h"
#include "loader/guestInstructionPatcher.h"
#include "loader/x64InstructionEmulator.h"
#include "loader/wolverine.h"
#include <windows.h>
#include <Zydis/Zydis.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <csignal>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>
#include <stdexcept>
#define XBYAK_NO_EXCEPTION
#include <xbyak/xbyak.h>
#include <xbyak/xbyak_util.h>

namespace Log {
bool IsSilent() { return true; }
void Write(std::string_view text) { std::fwrite(text.data(), 1, text.size(), stdout); }
}
namespace Common {
int DbgExitIfHandler(const char* expr, const char*, int) { throw std::runtime_error(expr); }
int DbgExitHandler(const char*, int, std::string_view text) { throw std::runtime_error(std::string(text)); }
void DbgExit(int status) { std::exit(status); }
namespace VirtualMemory {
bool FlushInstructionCache(uint64_t address, uint64_t size) {
    return ::FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), size) != 0;
}
}
}
namespace Libs::LibKernel::Memory {
uint64_t AllocateProgramMemory(uint64_t address, uint64_t size, Common::VirtualMemory::Mode, const char*) {
    return reinterpret_cast<uint64_t>(VirtualAlloc(reinterpret_cast<void*>(address), size,
        MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
}
bool FreeGuestMemory(uint64_t address, uint64_t) { return VirtualFree(reinterpret_cast<void*>(address), 0, MEM_RELEASE) != 0; }
}
void Check(const char* test, bool value, const std::string& message) {
    if (!value) throw std::runtime_error(std::string(test) + ": " + message);
}
volatile sig_atomic_t g_instruction_traps = 0;

bool EmulateInstructionContext(void* context) {
	const auto host_mxcsr = _mm_getcsr();
	_mm_setcsr(0x7fe1); // Exercise the handler under a distinct rounding mode and raised flags.
	const bool emulated        = Loader::X64InstructionEmulator::TryEmulate(context);
	const bool preserved_mxcsr = _mm_getcsr() == 0x7fe1;
	_mm_setcsr(host_mxcsr);
	if (!emulated || !preserved_mxcsr) {
		return false;
	}
	g_instruction_traps = g_instruction_traps + 1;
	return true;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
LONG CALLBACK InstructionHandler(EXCEPTION_POINTERS* exception) {
	if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION ||
	    !EmulateInstructionContext(exception->ContextRecord)) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	return EXCEPTION_CONTINUE_EXECUTION;
}
#else
void InstructionHandler(int, siginfo_t*, void* context) {
	if (!EmulateInstructionContext(context)) {
		_exit(190);
	}
}
#endif

struct InstructionTestScope {
	uint64_t mapping;
	uint64_t size;
	uint32_t mxcsr = _mm_getcsr();
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	void* handler = nullptr;
#else
	struct sigaction previous {};
	bool             installed = false;
#endif
	~InstructionTestScope() {
		_mm_setcsr(mxcsr);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (handler != nullptr) {
			RemoveVectoredExceptionHandler(handler);
		}
#else
		if (installed) {
			sigaction(SIGILL, &previous, nullptr);
		}
#endif
		Loader::UnregisterGuestInstructionPatchModule(reinterpret_cast<void*>(mapping));
		Libs::LibKernel::Memory::FreeGuestMemory(mapping, size);
	}

	void InstallHandler(const char* test) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		handler = AddVectoredExceptionHandler(1, InstructionHandler);
		Check(test, handler != nullptr, "failed to install instruction handler");
#else
		struct sigaction action {};
		action.sa_sigaction = InstructionHandler;
		action.sa_flags     = SA_SIGINFO;
		sigemptyset(&action.sa_mask);
		installed = sigaction(SIGILL, &action, &previous) == 0;
		Check(test, installed, "failed to install instruction handler");
#endif
	}
};


void TestPackedReciprocalSquareRoot() {
	const char* test = "PackedReciprocalSquareRoot";
	constexpr uint64_t code_size = 0x4000;
	constexpr uint64_t allocation_size = code_size * 2;
	const auto mapping = Libs::LibKernel::Memory::AllocateProgramMemory(
	    0x902000000, allocation_size, Common::VirtualMemory::Mode::ExecuteReadWrite, "rsqrt_test");
	Check(test, mapping != 0, "failed to allocate instruction test code");
	InstructionTestScope restore {mapping, allocation_size};
	restore.InstallHandler(test);
	using GuestFunction = void(KYTY_SYSV_ABI*)(const uint32_t*, uint32_t*);
	const auto function = reinterpret_cast<GuestFunction>(mapping);
	const std::array<uintptr_t, 1> function_starts {mapping};
	const auto register_module = [&](uint64_t trampoline_size) {
		Loader::RegisterGuestInstructionPatchModule(
		    reinterpret_cast<void*>(mapping), code_size,
		    reinterpret_cast<void*>(mapping + code_size), trampoline_size);
	};
	const auto refine = [](float estimate) {
		return (estimate * 0.5f) * std::fma(-estimate, estimate, 3.0f);
	};
	constexpr uint64_t red_zone_sentinel = 0x1122334455667788ull;
	std::array<uint32_t, 16> input {};
	std::array<uint32_t, 74> output {};
	input.fill(0x3f800000);
	for (const auto registers: {std::array {2, 1}, std::array {10, 9}, std::array {1, 1},
	                            std::array {0, 1}, std::array {2, 0}, std::array {0, 0}}) {
		const auto destination = registers[0];
		const auto source = registers[1];
		input[0] = 0x3f800000;
		uint32_t expected = 0x3f800000;
		if (source == 9) {
			input[0] = 0x40800000;
			expected = 0x3f000000;
		}
		Xbyak::CodeGenerator code(code_size, reinterpret_cast<void*>(mapping));
		for (int reg = 0; reg < 3; ++reg) {
			code.vmovups(Xbyak::Ymm(reg), code.ptr[code.rdi + 32]);
		}
		code.vmovups(Xbyak::Ymm(source), code.ptr[code.rdi]);
		if (destination != source) {
			code.vmovups(Xbyak::Ymm(destination), code.ptr[code.rdi + 32]);
		}
		for (uint32_t offset = 8; offset <= 128; offset += 8) {
			code.mov(code.rax, red_zone_sentinel ^ offset);
			code.mov(code.qword[code.rsp - offset], code.rax);
		}
		code.cmp(code.rdi, code.rdi);
		code.stc();
		code.vrsqrtps(Xbyak::Xmm(destination), Xbyak::Xmm(source));
		const uint64_t instruction_count = destination == 2 && source == 1 ? 2 : 1;
		if (instruction_count == 2) {
			// Adjacent four-byte instructions share one relocation span.
			code.vrsqrtps(Xbyak::Xmm(destination), Xbyak::Xmm(source));
		}
		code.lea(code.rsp, code.ptr[code.rsp - 128]);
		code.pushfq();
		code.pop(code.rax);
		code.lea(code.rsp, code.ptr[code.rsp + 128]);
		code.mov(code.qword[code.rsi + 288], code.rax);
		for (uint32_t offset = 8; offset <= 128; offset += 8) {
			code.mov(code.rax, code.qword[code.rsp - offset]);
			code.mov(code.qword[code.rsi + 64 + offset - 8], code.rax);
		}
		code.vmovups(code.ptr[code.rsi], Xbyak::Ymm(destination));
		code.vmovups(code.ptr[code.rsi + 32], Xbyak::Ymm(source));
		for (int reg = 0; reg < 3; ++reg) {
			code.vmovups(code.ptr[code.rsi + 192 + reg * 32], Xbyak::Ymm(reg));
		}
		code.vzeroupper();
		code.ret();
		Check(test, Xbyak::GetError() == 0, "failed to generate reciprocal-root fixture");
		const std::vector<uint8_t> original(code.getCode(), code.getCurr());
		Check(test, Common::VirtualMemory::FlushInstructionCache(mapping, code.getSize()),
		      "failed to flush generated instruction test code");
		function(input.data(), output.data());
		const float native = std::bit_cast<float>(output[0]);
		const auto red_zone_intact = [&] {
			for (uint32_t offset = 8; offset <= 128; offset += 8) {
				const auto index = 16 + (offset - 8) / sizeof(uint32_t);
				const auto value = static_cast<uint64_t>(output[index]) |
				                   (static_cast<uint64_t>(output[index + 1]) << 32);
				if (value != (red_zone_sentinel ^ offset)) {
					return false;
				}
			}
			return true;
		};
		Check(test, red_zone_intact(), "native instruction corrupted the guest red zone");
		Check(test, std::isfinite(native) && std::abs(native - std::bit_cast<float>(expected)) < 0.001f,
		      "native reciprocal root is outside its error bound");
		register_module(code_size);
		const std::vector<uint8_t> original_trampoline(code.getCode() + code_size,
		                                              code.getCode() + allocation_size);
		const auto disabled = Loader::PatchGuestInstructions(
		    mapping, code.getSize(), function_starts, false, false);
		Check(test, Xbyak::GetError() == 0 && disabled.reciprocal_sqrt.found == 0 &&
		                disabled.reciprocal_sqrt.native == 0 &&
		                disabled.reciprocal_sqrt.trapped == 0 &&
		                std::equal(original.begin(), original.end(), code.getCode()) &&
		                std::equal(original_trampoline.begin(), original_trampoline.end(),
		                           code.getCode() + code_size),
		      "disabled instruction patching modified executable code or trampolines");
		if (destination == 2 && source == 1) {
			// Fail initially, after the RIP-relative constant reference, and inside its literal.
			for (const uint64_t capacity: {1, 48, 80}) {
				register_module(capacity);
				const auto exhausted = Loader::PatchGuestInstructions(
				    mapping, code.getSize(), function_starts, false, true);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
				const uint64_t expected_traps = capacity == 1 ? 0 : instruction_count;
#else
				const uint64_t expected_traps = instruction_count;
#endif
				Check(test, exhausted.reciprocal_sqrt.found == instruction_count &&
				                exhausted.reciprocal_sqrt.native == 0 &&
				                exhausted.reciprocal_sqrt.trapped == expected_traps,
				      "exhausted reciprocal-root generation lost fallback coverage");
				Check(test, Xbyak::GetError() == 0,
				      "failed trampoline generation leaked Xbyak's thread-local error");
				if (expected_traps == 0) {
					Check(test, std::equal(original.begin(), original.end(), code.getCode()),
					      "unsafe fallback modified a live guest red zone site");
				}
				const auto traps_before = g_instruction_traps;
				function(input.data(), output.data());
				Check(test, static_cast<uint64_t>(g_instruction_traps - traps_before) == expected_traps &&
				                red_zone_intact() &&
				                output[0] == (expected_traps ? expected : std::bit_cast<uint32_t>(native)),
				      "exhaustion fallback changed guest state or failed to execute");
				std::memcpy(reinterpret_cast<void*>(mapping), original.data(), original.size());
			}
			register_module(code_size);
		}
		// Memory protection alone must retain the host's original reciprocal-root result.
		const bool protect_memory = source == 9;
		if (protect_memory) {
			const auto memory_only = Loader::PatchGuestInstructions(
			    mapping, code.getSize(), function_starts, true, false);
			Check(test, Xbyak::GetError() == 0 &&
			                memory_only.reciprocal_sqrt.found == 0 &&
			                memory_only.reciprocal_sqrt.native == 0 &&
			                memory_only.reciprocal_sqrt.trapped == 0 &&
			                memory_only.patched_memory_instruction_count > 0,
			      "memory protection enabled reciprocal-root replacement");
			function(input.data(), output.data());
			Check(test, output[0] == std::bit_cast<uint32_t>(native) && red_zone_intact(),
			      "memory protection changed native reciprocal-root behavior");
			std::memcpy(reinterpret_cast<void*>(mapping), original.data(), original.size());
			register_module(code_size);
		}
		for (const bool trap_fallback: {false, true}) {
			if (trap_fallback) {
				std::memcpy(reinterpret_cast<void*>(mapping), original.data(), original.size());
				register_module(48);
			}
			const bool protect_memory_sites = protect_memory && !trap_fallback;
			const auto patched = Loader::PatchGuestInstructions(
			    mapping, code.getSize(), function_starts, protect_memory_sites, true);
			Check(test, Xbyak::GetError() == 0 &&
			                patched.reciprocal_sqrt.found == instruction_count &&
			                patched.reciprocal_sqrt.native ==
			                    (trap_fallback ? 0 : instruction_count) &&
			                patched.reciprocal_sqrt.trapped ==
			                    (trap_fallback ? instruction_count : 0) &&
			                patched.unrelocatable_memory_instruction_count == 0 &&
			                (protect_memory_sites ? patched.patched_memory_instruction_count > 0
			                                      : patched.patched_memory_instruction_count == 0),
			      "instruction pass lost reciprocal root or memory patch coverage");
			const auto traps_before = g_instruction_traps;
			function(input.data(), output.data());
			Check(test, static_cast<uint64_t>(g_instruction_traps - traps_before) ==
			                (trap_fallback ? instruction_count : 0),
			      "reciprocal-root execution did not match its native/fallback patch count");
			Check(test, (output[72] & 0x8d5u) == 0x45u, "patched instruction corrupted RFLAGS");
			for (int reg = 0; reg < 3; ++reg) {
				if (reg != destination && reg != source) {
					Check(test, std::equal(input.begin() + 8, input.end(), output.begin() + 48 + reg * 8),
					      "patched instruction corrupted an unrelated YMM register");
				}
			}
			Check(test, red_zone_intact(), "patched instruction corrupted the guest red zone");
			Check(test, output[0] == expected, "patched instruction read or wrote the wrong register");
			if (destination != source) {
				Check(test, std::equal(input.begin(), input.begin() + 4, output.begin() + 8),
				      "source lower XMM lanes were corrupted");
			}
			if (source != 9) {
				Check(test, refine(refine(std::bit_cast<float>(output[0]))) == 1.0f,
				      "identity quaternion normalization drifted below one");
			}
			for (size_t i = 0; i < 4; ++i) {
				Check(test, output[4 + i] == 0, "128-bit VEX destination retained upper YMM lanes");
				const auto expected_upper = destination == source ? 0u : input[4 + i];
				Check(test, output[12 + i] == expected_upper, "source upper YMM lanes were corrupted");
			}
			std::printf("[host]    rsqrt xmm%d,xmm%d %s native=%08x patched=%08x\n",
			            destination, source, trap_fallback ? "fallback" : "replacement",
			            std::bit_cast<uint32_t>(native), output[0]);
			if (destination != 2) {
				continue;
			}
			constexpr std::array<std::array<uint32_t, 8>, 4> cases {{
			    {0x00000000, 0x80000000, 0x00000001, 0x80000001,
			     0x7f800000, 0xff800000, 0x7f800000, 0xff800000},
			    {0x7f800000, 0xff800000, 0xbf800000, 0x7fc12345,
			     0x00000000, 0xffc00000, 0xffc00000, 0x7fc12345},
			    {0x7f812345, 0xff812345, 0x40800000, 0x40000000,
			     0x7fc12345, 0xffc12345, 0x3f000000, 0x3f3504f3},
			    {0x00800000, 0x7f7fffff, 0x3f800000, 0x41800000,
			     0x5f000000, 0x1f800000, 0x3f800000, 0x3e800000},
			}};
			for (const auto& values: cases) {
				std::copy_n(values.begin(), 4, input.begin());
				for (uint32_t controls = 0; controls < 8; ++controls) {
					const uint32_t mxcsr = 0x1fa1u | ((controls & 3u) << 13u) | ((controls & 4u) << 4u);
					_mm_setcsr(mxcsr);
					function(input.data(), output.data());
					const auto result_mxcsr = _mm_getcsr();
					_mm_setcsr(restore.mxcsr);
					Check(test, red_zone_intact(), "special-value replacement corrupted the guest red zone");
					Check(test, result_mxcsr == mxcsr, "instruction changed MXCSR controls or exception flags");
					Check(test, std::equal(output.begin(), output.begin() + 4, values.begin() + 4),
					      "special values or round-independent reciprocal roots differ from ISA semantics");
				}
			}
			input.fill(0x3f800000);
		}
	}
	// An indirect branch prevents borrowing neighboring bytes for this four-byte site.
	Xbyak::CodeGenerator unsupported_code(code_size, reinterpret_cast<void*>(mapping));
	Xbyak::Label continuation;
	unsupported_code.vmovups(unsupported_code.ymm1, unsupported_code.ptr[unsupported_code.rdi]);
	unsupported_code.lea(unsupported_code.rax,
	                     unsupported_code.ptr[unsupported_code.rip + continuation]);
	const auto trap_offset = unsupported_code.getSize();
	unsupported_code.vrsqrtps(unsupported_code.xmm1, unsupported_code.xmm1);
	unsupported_code.jmp(unsupported_code.rax);
	unsupported_code.L(continuation);
	unsupported_code.vmovups(unsupported_code.ptr[unsupported_code.rsi], unsupported_code.ymm1);
	unsupported_code.vzeroupper();
	unsupported_code.ret();
	Check(test, Xbyak::GetError() == 0, "failed to generate unsupported-relocation fixture");
	std::vector<uint8_t> expected_fallback(unsupported_code.getCode(), unsupported_code.getCurr());
	expected_fallback[trap_offset + 1] &= ~0x08u;
	register_module(code_size);
	const auto unsupported = Loader::PatchGuestInstructions(
	    mapping, unsupported_code.getSize(), function_starts, false, true);
	Check(test, Xbyak::GetError() == 0 && unsupported.reciprocal_sqrt.found == 1 &&
	                unsupported.reciprocal_sqrt.native == 0 &&
	                unsupported.reciprocal_sqrt.trapped == 1 &&
	                std::equal(expected_fallback.begin(), expected_fallback.end(),
	                           unsupported_code.getCode()),
	      "unsafe native relocation did not select the in-place trap fallback");
	const auto traps_before = g_instruction_traps;
	function(input.data(), output.data());
	Check(test, g_instruction_traps == traps_before + 1 && output[0] == 0x3f800000 &&
	                std::all_of(output.begin() + 4, output.begin() + 8,
	                            [](uint32_t lane) { return lane == 0; }),
	      "unsafe native relocation fallback did not execute correctly");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	CONTEXT context {};
	auto& instruction_pointer = context.Rip;
#else
	ucontext_t context {};
	_libc_fpstate fpstate {};
	context.uc_mcontext.fpregs = &fpstate;
	auto& instruction_pointer = context.uc_mcontext.gregs[REG_RIP];
#endif
	std::array<uint8_t, 16> unknown {0x0f, 0x0b};
	instruction_pointer = reinterpret_cast<uintptr_t>(unknown.data());
	Check(test, !Loader::X64InstructionEmulator::TryEmulate(&context), "unrecognized UD2 was swallowed");
	std::array<uint8_t, 16> unmarked_rsqrt {0xc5, 0xf8, 0x52, 0xd1};
	instruction_pointer = reinterpret_cast<uintptr_t>(unmarked_rsqrt.data());
	Check(test, !Loader::X64InstructionEmulator::TryEmulate(&context) &&
	                instruction_pointer == reinterpret_cast<uintptr_t>(unmarked_rsqrt.data()),
	      "unmarked native reciprocal root was incorrectly emulated");
#if defined(__linux__)
	const auto emulate = [&](std::array<uint8_t, 16> instruction, size_t length) {
		context.uc_mcontext.gregs[REG_RIP] = reinterpret_cast<greg_t>(instruction.data());
		Check(test, Loader::X64InstructionEmulator::TryEmulate(&context), "existing instruction emulation was rejected");
		Check(test, context.uc_mcontext.gregs[REG_RIP] == reinterpret_cast<greg_t>(instruction.data() + length),
		      "existing instruction emulation advanced RIP incorrectly");
	};
	fpstate._xmm[8].element[0] = 0x1234;
	fpstate._xmm[8].element[2] = 0xdeadbeef;
	emulate({0x66, 0x41, 0x0f, 0x78, 0xc0, 8, 4}, 7); // extrq xmm8, 8, 4
	Check(test, fpstate._xmm[8].element[0] == 0x23 && fpstate._xmm[8].element[2] == 0,
	      "SSE4a extraction lost its extended register or upper-half semantics");
	fpstate._xmm[1].element[0] = 0x1234;
	fpstate._xmm[8].element[0] = 0xffffc4c8;
	emulate({0x66, 0x41, 0x0f, 0x79, 0xc8}, 5); // observed fault: extrq xmm1, xmm8
	Check(test, fpstate._xmm[1].element[0] == 0x23 && fpstate._xmm[8].element[0] == 0xffffc4c8,
	      "register EXTRQ lost its source controls, ignored bits, or separate destination");
	fpstate._xmm[11].element[0] = 0x89abcdef;
	fpstate._xmm[11].element[1] = 0x01234567;
	fpstate._xmm[0].element[0] = 0x2010;
	emulate({0x66, 0x44, 0x0f, 0x79, 0xd8}, 5); // extrq xmm11, xmm0
	Check(test, fpstate._xmm[11].element[0] == 0x4567 && fpstate._xmm[11].element[1] == 0,
	      "register EXTRQ lost its extended destination or 64-bit extraction");
	fpstate._xmm[8].element[0] = 0x89abcdef;
	fpstate._xmm[8].element[1] = 0x01234567;
	fpstate._xmm[9].element[0] = 0;
	emulate({0x66, 0x45, 0x0f, 0x79, 0xc1}, 5); // extrq xmm8, xmm9
	Check(test, fpstate._xmm[8].element[0] == 0x89abcdef && fpstate._xmm[8].element[1] == 0x01234567,
	      "register EXTRQ did not interpret zero length as 64 bits");
	fpstate._xmm[3].element[0] = 0xab0408;
	emulate({0x66, 0x0f, 0x79, 0xdb}, 4); // extrq xmm3, xmm3
	Check(test, fpstate._xmm[3].element[0] == 0x40,
	      "register EXTRQ overwrote aliased controls before reading them");
	fpstate._xmm[8].element[0] = 0x1111;
	fpstate._xmm[8].element[2] = 0xdeadbeef;
	fpstate._xmm[9].element[0] = 0xab;
	emulate({0xf2, 0x45, 0x0f, 0x78, 0xc1, 8, 4}, 7); // insertq xmm8, xmm9, 8, 4
	Check(test, fpstate._xmm[8].element[0] == 0x1ab1 && fpstate._xmm[8].element[2] == 0xdeadbeef,
	      "SSE4a insertion corrupted its destination lanes");
	std::array<uint32_t, 8> sha_source {0, 0, 0, 0, 5, 6, 7, 8};
	context.uc_mcontext.gregs[REG_RDI] = reinterpret_cast<greg_t>(sha_source.data());
	for (uint32_t lane = 0; lane < 4; ++lane) {
		fpstate._xmm[8].element[lane] = lane + 1;
	}
	emulate({0x44, 0x0f, 0x38, 0xc9, 0x47, 0x10}, 6); // sha1msg1 xmm8, [rdi+16]
	constexpr std::array<uint32_t, 4> sha_expected {6, 10, 2, 6};
	Check(test, std::equal(sha_expected.begin(), sha_expected.end(), fpstate._xmm[8].element),
	      "SHA emulation lost its memory operand or extended destination register");
	emulate({0x0f, 0x01, 0xfa}, 3); // monitorx
#endif
	std::printf("[host]    %-48s ok\n", test);
}

void TestPackedBitField(bool insert) {
	const char*        test      = insert ? "PackedBitFieldInsert" : "PackedBitFieldExtract";
	constexpr uint64_t code_size = 0x80000;
	const auto         mapping   = Libs::LibKernel::Memory::AllocateProgramMemory(
	    0x904000000, code_size * 2, Common::VirtualMemory::Mode::ExecuteReadWrite, "bitfield_test");
	Check(test, mapping != 0, "failed to allocate SSE4a bitfield test code");
	InstructionTestScope restore {mapping, code_size * 2};
	restore.InstallHandler(test);
	const bool host_sse4a      = Xbyak::util::Cpu().has(Xbyak::util::Cpu::tSSE4a);
	const auto register_module = [&](uint64_t capacity) {
		Loader::RegisterGuestInstructionPatchModule(reinterpret_cast<void*>(mapping), code_size,
		                                            reinterpret_cast<void*>(mapping + code_size),
		                                            capacity);
	};
	// Xbyak has no SSE4a bitfield mnemonics. A negative source selects the immediate form.
	const auto emit_bit_field = [insert](Xbyak::CodeGenerator& code, int destination, int source,
	                                     uint8_t length = 0, uint8_t index = 0) {
		const bool immediate = source < 0;
		if (insert && immediate) {
			source = 9;
		}
		code.db(insert ? 0xf2 : 0x66);
		const int rex = source < 0 ? (destination >> 3) : ((destination >> 3) << 2) | (source >> 3);
		if (rex != 0) {
			code.db(0x40 | rex);
		}
		code.db(0x0f);
		code.db(immediate ? 0x78 : 0x79);
		code.db(source < 0 ? 0xc0 | (destination & 7)
		                   : 0xc0 | ((destination & 7) << 3) | (source & 7));
		if (immediate) {
			code.db(length);
			code.db(index);
		}
	};
	// Copy individual source bits so the oracle is independent of the emitter's shift/mask.
	const auto extract = [](uint64_t value, uint32_t length, uint32_t index) {
		const uint32_t count  = (length & 63u) == 0 ? 64 : length & 63u;
		const uint32_t first  = index & 63u;
		uint64_t       result = 0;
		for (uint32_t bit = 0; bit < count && first + bit < 64; ++bit) {
			result |= ((value >> (first + bit)) & 1u) << bit;
		}
		return result;
	};
	const auto insert_bits = [](uint64_t destination, uint64_t source, uint32_t length,
	                            uint32_t index) {
		const uint32_t count = (length & 63u) == 0 ? 64 : length & 63u;
		const uint32_t first = index & 63u;
		for (uint32_t bit = 0; bit < count && first + bit < 64; ++bit) {
			const uint64_t mask = uint64_t {1} << (first + bit);
			destination         = (destination & ~mask) | (((source >> bit) & 1u) << (first + bit));
		}
		return destination;
	};
	struct Result {
		std::array<uint64_t, 4>  destination;
		std::array<uint64_t, 4>  source;
		std::array<uint64_t, 13> gprs;
		uint64_t                 flags;
		uint64_t                 stack_before;
		uint64_t                 stack_after;
		std::array<uint64_t, 16> red_zone;
	};
	constexpr uint64_t             gpr_sentinel      = 0x8192a3b4c5d6e700ull;
	constexpr uint64_t             red_zone_sentinel = 0xa1b2c3d4e5f60000ull;
	std::array<uint64_t, 12>       input {0,
	                                      0xdeadbeef12345678,
	                                      0x8877665544332211,
	                                      0x1122334455667788,
	                                      0,
	                                      0x123456789abcdef0,
	                                      0x13579bdf2468ace0,
	                                      0xfdb97531eca86420,
	                                      0x3f8000003f800000,
	                                      0x3f8000003f800000,
	                                      0x3f8000003f800000,
	                                      0x3f8000003f800000};
	const std::array<uintptr_t, 1> function_starts {mapping};
	using GuestFunction = void(KYTY_SYSV_ABI*)(const uint64_t*, void*);
	const auto function = reinterpret_cast<GuestFunction>(mapping);
	for (const auto registers: {std::array {1, 8}, std::array {8, 9}, std::array {3, 3},
	                            std::array {15, 0}, std::array {8, -1}}) {
		const auto           destination = registers[0];
		const auto           source      = insert && registers[1] < 0 ? 9 : registers[1];
		const bool           immediate   = registers[1] < 0;
		Xbyak::CodeGenerator code(code_size, reinterpret_cast<void*>(mapping));
		const std::array     saved {code.rbx, code.rbp, code.r12, code.r13, code.r14, code.r15};
		const std::array gprs {code.rax, code.rcx, code.rdx, code.rbx, code.rbp, code.r8, code.r9,
		                       code.r10, code.r11, code.r12, code.r13, code.r14, code.r15};
		for (const auto& reg: saved) {
			code.push(reg);
		}
		code.vmovups(Xbyak::Ymm(destination), code.ptr[code.rdi]);
		if (source >= 0 && source != destination) {
			code.vmovups(Xbyak::Ymm(source), code.ptr[code.rdi + 32]);
		}
		for (uint32_t offset = 8; offset <= 128; offset += 8) {
			code.mov(code.rax, red_zone_sentinel | offset);
			code.mov(code.qword[code.rsp - offset], code.rax);
		}
		code.mov(code.eax, 0x7fffffff);
		code.add(code.eax, 1);
		code.stc(); // CF, PF, AF, SF and OF set; ZF clear.
		for (size_t index = 0; index < gprs.size(); ++index) {
			code.mov(gprs[index], gpr_sentinel | index);
		}
		code.mov(code.qword[code.rsi + offsetof(Result, stack_before)], code.rsp);
		emit_bit_field(code, destination, registers[1], 0xc8, 0xc4);
		code.mov(code.qword[code.rsi + offsetof(Result, stack_after)], code.rsp);
		for (size_t index = 0; index < gprs.size(); ++index) {
			code.mov(code.qword[code.rsi + offsetof(Result, gprs) + index * 8], gprs[index]);
		}
		code.lea(code.rsp, code.ptr[code.rsp - 128]);
		code.pushfq();
		code.pop(code.rax);
		code.lea(code.rsp, code.ptr[code.rsp + 128]);
		code.mov(code.qword[code.rsi + offsetof(Result, flags)], code.rax);
		for (uint32_t offset = 8; offset <= 128; offset += 8) {
			code.mov(code.rax, code.qword[code.rsp - offset]);
			code.mov(code.qword[code.rsi + offsetof(Result, red_zone) + offset - 8], code.rax);
		}
		code.vmovups(code.ptr[code.rsi + offsetof(Result, destination)], Xbyak::Ymm(destination));
		code.vmovups(code.ptr[code.rsi + offsetof(Result, source)],
		             Xbyak::Ymm(source < 0 ? destination : source));
		code.vzeroupper();
		for (auto reg = saved.rbegin(); reg != saved.rend(); ++reg) {
			code.pop(*reg);
		}
		code.ret();
		Check(test, Xbyak::GetError() == 0, "failed to generate SSE4a bitfield state fixture");
		const std::vector<uint8_t> original(code.getCode(), code.getCurr());
		register_module(code_size);
		const std::vector<uint8_t> trampoline(code.getCode() + code_size,
		                                      code.getCode() + code_size * 2);
		const auto                 disabled =
		    Loader::PatchGuestInstructions(mapping, code.getSize(), function_starts, false, false);
		Check(test,
		      disabled.extrq.found == 0 && disabled.insertq.found == 0 &&
		          std::equal(original.begin(), original.end(), code.getCode()) &&
		          std::equal(trampoline.begin(), trampoline.end(), code.getCode() + code_size),
		      "disabled AMD patching changed SSE4a bitfield code or trampolines");
		for (const bool fallback: {false, true}) {
			if (insert && fallback) {
				continue; // INSERTQ always uses the trap path.
			}
			std::memcpy(reinterpret_cast<void*>(mapping), original.data(), original.size());
			register_module(fallback ? 32 : code_size);
			const auto patched = Loader::PatchGuestInstructions(mapping, code.getSize(),
			                                                    function_starts, false, true);
			const auto counts  = insert ? patched.insertq : patched.extrq;
			if (host_sse4a) {
				Check(test,
				      counts.found == 0 &&
				          std::equal(original.begin(), original.end(), code.getCode()),
				      "SSE4a host received unnecessary SSE4a bitfield patches");
				std::printf("[host]    %-48s skipped (native SSE4a host)\n", test);
				return;
			}
			std::printf("[host]    bitfield xmm%d,%d %s found=%" PRIu64 " native=%" PRIu64
			            " trapped=%" PRIu64 "\n",
			            destination, source, fallback ? "fallback" : "replacement", counts.found,
			            counts.native, counts.trapped);
			Check(test,
			      Xbyak::GetError() == 0 && counts.found == 1 && counts.Skipped() == 0 &&
			          counts.native == (fallback || insert ? 0u : 1u) &&
			          counts.trapped == (fallback || insert ? 1u : 0u),
			      "SSE4a bitfield native/fallback patch counts differ");
			const auto traps_before = g_instruction_traps;
			uint64_t   calls        = 0;
			for (uint32_t length = 0; length < 64; ++length) {
				for (uint32_t index = 0; index < 64; ++index) {
					if (immediate && (length != 8 || index != 4)) {
						continue;
					}
					for (const uint64_t pattern: {UINT64_MAX, uint64_t {0x0123456789abcdef}}) {
						const uint64_t controls = (pattern & ~uint64_t {0xffff}) |
						                          ((index | 0xc0u) << 8) | (length | 0xc0u);
						input[0] = !insert && source == destination ? controls : pattern;
						input[4] = insert ? ~pattern : controls;
						if (insert) {
							input[source == destination ? 1 : 5] = controls;
						}
						Result output {};
						_mm_setcsr(0x5fa5);
						function(input.data(), &output);
						const auto mxcsr = _mm_getcsr();
						_mm_setcsr(restore.mxcsr);
						++calls;
						Check(test,
						      output.destination[0] ==
						              (insert ? insert_bits(input[0],
						                                    input[source == destination ? 0 : 4],
						                                    length, index)
						                      : extract(input[0], length, index)) &&
						          output.destination[1] == (insert ? input[1] : 0) &&
						          output.destination[2] == input[2] &&
						          output.destination[3] == input[3],
						      "SSE4a bitfield result or destination lanes differ");
						Check(test,
						      source < 0 || source == destination
						          ? output.source == output.destination
						          : std::equal(output.source.begin(), output.source.end(),
						                       input.begin() + 4),
						      "SSE4a bitfield changed its separate source register");
						Check(test,
						      mxcsr == 0x5fa5 && (output.flags & 0x8d5u) == 0x895u &&
						          output.stack_before == output.stack_after,
						      "SSE4a bitfield changed MXCSR, RFLAGS or RSP");
						for (size_t reg = 0; reg < output.gprs.size(); ++reg) {
							Check(test, output.gprs[reg] == (gpr_sentinel | reg),
							      "SSE4a bitfield corrupted a guest GPR");
						}
						for (size_t slot = 0; slot < output.red_zone.size(); ++slot) {
							Check(test,
							      output.red_zone[slot] == (red_zone_sentinel | ((slot + 1) * 8)),
							      "SSE4a bitfield corrupted the guest red zone");
						}
					}
				}
			}
			Check(test,
			      static_cast<uint64_t>(g_instruction_traps - traps_before) ==
			          (fallback || insert ? calls : 0),
			      "SSE4a bitfield execution did not match native/fallback selection");
		}
	}
	if (insert) {
		std::printf("[host]    %-48s ok (all 4096 length/index pairs)\n", test);
		return;
	}

	// Cover every immediate length/index pair without re-decoding thousands of large fixtures.
	Xbyak::CodeGenerator   code(code_size, reinterpret_cast<void*>(mapping));
	std::vector<uintptr_t> starts;
	for (uint32_t length = 0; length < 64; ++length) {
		for (uint32_t index = 0; index < 64; ++index) {
			starts.push_back(mapping + code.getSize());
			code.vmovups(code.ymm8, code.ptr[code.rdi]);
			emit_bit_field(code, 8, -1, length | 0xc0, index | 0xc0);
			code.vmovups(code.ptr[code.rsi], code.ymm8);
			code.vzeroupper();
			code.ret();
		}
	}
	Check(test, Xbyak::GetError() == 0, "failed to generate exhaustive immediate EXTRQ fixture");
	const std::vector<uint8_t> original(code.getCode(), code.getCurr());
	for (const bool fallback: {false, true}) {
		std::memcpy(reinterpret_cast<void*>(mapping), original.data(), original.size());
		register_module(fallback ? 1 : code_size);
		const auto counts =
		    Loader::PatchGuestInstructions(mapping, code.getSize(), starts, false, true).extrq;
		Check(test,
		      Xbyak::GetError() == 0 && counts.found == starts.size() && counts.Skipped() == 0 &&
		          counts.native == (fallback ? 0 : starts.size()) &&
		          counts.trapped == (fallback ? starts.size() : 0),
		      "immediate EXTRQ coverage was lost");
		const auto traps_before = g_instruction_traps;
		for (size_t entry = 0; entry < starts.size(); ++entry) {
			for (const uint64_t pattern: {UINT64_MAX, uint64_t {0x0123456789abcdef}}) {
				input[0] = pattern;
				std::array<uint64_t, 4> output {};
				reinterpret_cast<GuestFunction>(starts[entry])(input.data(), output.data());
				Check(test,
				      output[0] == extract(pattern, entry / 64, entry % 64) && output[1] == 0 &&
				          output[2] == input[2] && output[3] == input[3],
				      "immediate EXTRQ bit slice differs");
			}
		}
		Check(test,
		      static_cast<uint64_t>(g_instruction_traps - traps_before) ==
		          (fallback ? starts.size() * 2 : 0),
		      "immediate EXTRQ did not execute its selected path");
	}

	// An indirect branch prevents relocating this four-byte register form; retain its natural trap.
	code.reset();
	Xbyak::Label continuation;
	code.vmovups(code.ymm1, code.ptr[code.rdi]);
	code.vmovups(code.ymm2, code.ptr[code.rdi + 32]);
	code.lea(code.rax, code.ptr[code.rip + continuation]);
	emit_bit_field(code, 1, 2);
	code.jmp(code.rax);
	code.L(continuation);
	code.vmovups(code.ptr[code.rsi], code.ymm1);
	code.vzeroupper();
	code.ret();
	Check(test, Xbyak::GetError() == 0, "failed to generate unrelocatable EXTRQ fixture");
	const std::vector<uint8_t> unsafe_original(code.getCode(), code.getCurr());
	register_module(code_size);
	const auto unsafe =
	    Loader::PatchGuestInstructions(mapping, code.getSize(), function_starts, false, true).extrq;
	Check(test,
	      unsafe.found == 1 && unsafe.native == 0 && unsafe.trapped == 1 &&
	          std::equal(unsafe_original.begin(), unsafe_original.end(), code.getCode()),
	      "unrelocatable EXTRQ did not retain its natural trap");
	input[0] = 0x0123456789abcdef;
	input[4] = 0xffffc4c8;
	std::array<uint64_t, 8> output {};
	const auto              traps_before = g_instruction_traps;
	function(input.data(), output.data());
	Check(test, g_instruction_traps == traps_before + 1 && output[0] == extract(input[0], 8, 4),
	      "unrelocatable EXTRQ fallback did not execute");

	// Neighboring replacement kinds must share the same relocation/fallback span safely.
	code.reset();
	code.vmovups(code.ymm1, code.ptr[code.rdi]);
	code.vmovups(code.ymm2, code.ptr[code.rdi + 32]);
	code.vmovups(code.ymm3, code.ptr[code.rdi + 64]);
	emit_bit_field(code, 1, 2);
	code.vrsqrtps(code.xmm4, code.xmm3);
	code.vmovups(code.ptr[code.rsi], code.ymm1);
	code.vmovups(code.ptr[code.rsi + 32], code.ymm4);
	code.vzeroupper();
	code.ret();
	Check(test, Xbyak::GetError() == 0, "failed to generate mixed AMD instruction fixture");
	const std::vector<uint8_t> mixed_original(code.getCode(), code.getCurr());
	for (const bool fallback: {false, true}) {
		std::memcpy(reinterpret_cast<void*>(mapping), mixed_original.data(), mixed_original.size());
		register_module(fallback ? 48 : code_size);
		const auto mixed =
		    Loader::PatchGuestInstructions(mapping, code.getSize(), function_starts, false, true);
		for (const auto counts: {mixed.extrq, mixed.reciprocal_sqrt}) {
			Check(test,
			      counts.found == 1 && counts.native == (fallback ? 0u : 1u) &&
			          counts.trapped == (fallback ? 1u : 0u),
			      "mixed AMD instruction counts differ");
		}
		const auto before = g_instruction_traps;
		function(input.data(), output.data());
		Check(test,
		      g_instruction_traps == before + (fallback ? 2 : 0) &&
		          output[0] == extract(input[0], 8, 4) && output[4] == 0x3f8000003f800000 &&
		          output[5] == 0x3f8000003f800000 && output[6] == 0 && output[7] == 0,
		      "mixed native/fallback AMD instructions lost their results");
	}
	std::printf("[host]    %-48s ok (all 4096 length/index pairs)\n", test);
}

void TestPackedBitFieldExtract() {
	TestPackedBitField(false);
}

void TestPackedBitFieldInsert() {
	TestPackedBitField(true);
}

void TestCpuExtensionPatches() {
	const char*        test      = "CpuExtensionPatches";
	constexpr uint64_t code_size = 0x4000;
	const auto         mapping   = Libs::LibKernel::Memory::AllocateProgramMemory(
	    0x906000000, code_size * 2, Common::VirtualMemory::Mode::ExecuteReadWrite,
	    "cpu_extension_test");
	Check(test, mapping != 0, "failed to allocate CPU extension fixture");
	InstructionTestScope restore {mapping, code_size * 2};
	restore.InstallHandler(test);
	const auto                     host = Loader::GetGuestInstructionHostFeatures();
	const std::array<uintptr_t, 1> starts {mapping};
	const auto                     register_module = [&](uint64_t capacity) {
		std::memset(reinterpret_cast<void*>(mapping + code_size), 0, code_size);
		Loader::RegisterGuestInstructionPatchModule(reinterpret_cast<void*>(mapping), code_size,
		                                            reinterpret_cast<void*>(mapping + code_size),
		                                            capacity);
	};
	Xbyak::CodeGenerator code(code_size, reinterpret_cast<void*>(mapping));
	using GuestFunction = void(KYTY_SYSV_ABI*)(const uint64_t*, uint64_t*);
	const auto function = reinterpret_cast<GuestFunction>(mapping);
	for (const bool writes_rsp: {false, true}) {
		code.reset();
		code.push(code.r12);
		code.mov(code.r12, code.rsp);
		code.vmovups(code.ymm8, code.ptr[code.rdi]);
		code.vmovups(code.ymm9, code.ptr[code.rdi + 32]);
		code.mov(code.r10, 0x123456789abcdef0ull);
		code.mov(code.qword[code.rsp - 8], code.r10);
		code.clwb(code.ptr[code.rsp - 8]);
		for (const uint8_t byte: {0xf2, 0x45, 0x0f, 0x79, 0xc1}) {
			code.db(byte); // INSERTQ xmm8,xmm9
		}
		code.db(0xf3); // RDPID rax/rsp
		code.db(0x48);
		code.db(0x0f);
		code.db(0xc7);
		code.db(writes_rsp ? 0xfc : 0xf8);
		if (writes_rsp) {
			code.mov(code.rax, code.rsp);
			code.mov(code.rsp, code.r12);
		}
		code.mov(code.qword[code.rsi], code.rax);
		code.mov(code.rax, code.qword[code.rsp - 8]);
		code.mov(code.qword[code.rsi + 8], code.rax);
		code.vmovups(code.ptr[code.rsi + 16], code.ymm8);
		code.vzeroupper();
		code.pop(code.r12);
		code.ret();
		Check(test, Xbyak::GetError() == 0, "failed to generate mixed CPU extension fixture");
		const std::vector<uint8_t> original(code.getCode(), code.getCurr());
		register_module(code_size);
		const auto disabled =
		    Loader::PatchGuestInstructions(mapping, code.getSize(), starts, false, false, {});
		Check(test,
		      disabled.insertq.found == 0 && disabled.rdpid.found == 0 &&
		          disabled.clwb.found == 0 &&
		          std::equal(original.begin(), original.end(), code.getCode()) &&
		          std::all_of(code.getCode() + code_size, code.getCode() + code_size * 2,
		                      [](uint8_t byte) { return byte == 0; }),
		      "disabled AMD option changed CPU extension code or trampolines");
		const auto supported = Loader::PatchGuestInstructions(mapping, code.getSize(), starts,
		                                                      false, true, {true, true, true});
		Check(test,
		      supported.insertq.found == 0 && supported.rdpid.found == 0 &&
		          supported.clwb.found == 0 &&
		          std::equal(original.begin(), original.end(), code.getCode()),
		      "available host CPU extensions were unnecessarily patched");
		const auto enabled =
		    Loader::PatchGuestInstructions(mapping, code.getSize(), starts, false, true, {});
		Check(test,
		      enabled.clwb.found == 1 && enabled.clwb.native == 1 && enabled.clwb.trapped == 0 &&
		          enabled.insertq.found == 1 && enabled.insertq.native == 0 &&
		          enabled.insertq.trapped == 1 && enabled.rdpid.found == 1 &&
		          enabled.rdpid.native == 0 && enabled.rdpid.trapped == 1,
		      "mixed native CLWB and trapped INSERTQ/RDPID counts differ");
		// Verify the actual stack-relative CLFLUSH operand, since flushing the wrong mapped
		// cache line would otherwise leave the sentinel unchanged too.
		ZydisDecoder decoder {};
		Check(test,
		      ZYAN_SUCCESS(
		          ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64)),
		      "failed to initialize decoder");
		bool found_flush = false;
		for (size_t offset = 0; offset < 512 && !found_flush;) {
			ZydisDecodedInstruction instruction {};
			ZydisDecodedOperand     operands[ZYDIS_MAX_OPERAND_COUNT] {};
			Check(test,
			      ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, code.getCode() + code_size + offset,
			                                          code_size - offset, &instruction, operands)),
			      "failed to decode CPU extension trampoline");
			if (instruction.mnemonic == ZYDIS_MNEMONIC_CLFLUSH) {
				found_flush = true;
				Check(test,
				      operands[0].mem.base == ZYDIS_REGISTER_RSP &&
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
				          operands[0].mem.disp.value == 120,
#else
				          operands[0].mem.disp.value == -8,
#endif
				      "CLWB replacement did not preserve its guest stack address");
			}
			offset += instruction.length;
		}
		Check(test, found_flush, "CLWB native replacement did not emit CLFLUSH");
		const std::array<uint64_t, 8> input {0x1111, 0xdeadbeef, 0x12345678, 0x87654321,
		                                     0xab,   0xffffc4c8, 0,          0};
		std::array<uint64_t, 6>       output {};
		bool                          checked_aux = false;
		for (size_t attempt = 0; attempt < 100 && !checked_aux; ++attempt) {
			uint32_t   before = 0;
			uint32_t   after  = 0;
			const auto traps  = g_instruction_traps;
			__rdtscp(&before);
			function(input.data(), output.data());
			__rdtscp(&after);
			Check(test, g_instruction_traps == traps + !host.sse4a + !host.rdpid,
			      "mixed CPU extension fixture used an unexpected trap path");
			Check(test,
			      output[1] == 0x123456789abcdef0ull && output[2] == 0x1ab1 &&
			          (host.sse4a || output[3] == input[1]) &&
			          std::equal(output.begin() + 4, output.end(), input.begin() + 2),
			      "mixed CPU extensions corrupted INSERTQ state or the guest red zone");
			if (before == after) {
				Check(test, output[0] == before,
				      "RDPID patch lost TSC_AUX or adjusted its RSP destination");
				checked_aux = true;
			}
		}
		Check(test, checked_aux, "thread migrated repeatedly during patched RDPID comparison");
	}

	code.reset();
	code.clwb(code.ptr[code.rdi]);
	code.ret();
	Check(test, Xbyak::GetError() == 0, "failed to generate exhausted CLWB fixture");
	const std::vector<uint8_t> original(code.getCode(), code.getCurr());
	register_module(1);
	const auto exhausted =
	    Loader::PatchGuestInstructions(mapping, code.getSize(), starts, false, true, {}).clwb;
	Check(test,
	      exhausted.found == 1 && exhausted.native == 0 && exhausted.trapped == 1 &&
	          std::equal(original.begin(), original.end(), code.getCode()) &&
	          Xbyak::GetError() == 0,
	      "exhausted CLWB replacement did not retain its natural trap fallback");
	std::array<uint64_t, 8> data;
	data.fill(0x123456789abcdef0ull);
	const auto traps = g_instruction_traps;
	function(data.data(), nullptr);
	Check(test,
	      g_instruction_traps == traps + !host.clwb &&
	          std::all_of(data.begin(), data.end(),
	                      [](uint64_t value) { return value == 0x123456789abcdef0ull; }),
	      "CLWB direct fallback changed memory or used an unexpected trap path");
	std::printf("[host]    %-48s ok\n", test);
}
void* g_red_zone_fault_page = nullptr;
size_t g_red_zone_fault_offset = 0x18;
uint32_t g_red_zone_fault_count = 0;

LONG CALLBACK RedZoneFaultHandler(EXCEPTION_POINTERS* exception) {
	if (exception == nullptr || exception->ExceptionRecord == nullptr ||
	    exception->ContextRecord == nullptr ||
	    exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
	    reinterpret_cast<void*>(exception->ExceptionRecord->ExceptionInformation[1]) !=
	        g_red_zone_fault_page) {
		return EXCEPTION_CONTINUE_SEARCH;
	}

	// Model the Windows exception stack footprint that prompted the static patch:
	// data below the interrupted RSP is not part of the Windows ABI contract.
	*reinterpret_cast<uint64_t*>(exception->ContextRecord->Rsp - g_red_zone_fault_offset) = 0;
	++g_red_zone_fault_count;
	DWORD old_protection = 0;
	if (VirtualProtect(g_red_zone_fault_page, 0x1000, PAGE_READWRITE, &old_protection) == FALSE) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	return EXCEPTION_CONTINUE_EXECUTION;
}


void TestWolverineRedZonePointer() {
	using Loader::Wolverine::RedZoneProtectionEnabled;
	static_assert(RedZoneProtectionEnabled(false, "PPSA03671", nullptr));
	static_assert(RedZoneProtectionEnabled(false, "PPSA03671", "1"));
	static_assert(!RedZoneProtectionEnabled(false, "PPSA03671", "0"));
	static_assert(!RedZoneProtectionEnabled(false, "PPSA03671", "invalid"));
	static_assert(RedZoneProtectionEnabled(true, "PPSA03671", "0"));
	static_assert(!RedZoneProtectionEnabled(false, "PPSA01341", nullptr));
	static_assert(!RedZoneProtectionEnabled(false, "PPSA01341", "1"));
	static_assert(!RedZoneProtectionEnabled(false, "", "1"));
	static_assert(RedZoneProtectionEnabled(true, "", nullptr));
	const char* test = "WolverineRedZonePointer";
	constexpr uint64_t code_size = 0x4000;
	const auto mapping = Libs::LibKernel::Memory::AllocateProgramMemory(
	    0x902000000, code_size * 2, Common::VirtualMemory::Mode::ExecuteReadWrite, test);
	Check(test, mapping != 0, "allocate generated code");
	InstructionTestScope restore {mapping, code_size * 2};
	g_red_zone_fault_page = VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	Check(test, g_red_zone_fault_page != nullptr, "allocate fault destination");
	struct FaultScope {
		void* handler = nullptr;
		~FaultScope() {
			if (handler != nullptr) RemoveVectoredExceptionHandler(handler);
			VirtualFree(g_red_zone_fault_page, 0, MEM_RELEASE);
			g_red_zone_fault_page = nullptr;
			g_red_zone_fault_offset = 0x18;
		}
	} fault;
	fault.handler = AddVectoredExceptionHandler(1, RedZoneFaultHandler);
	Check(test, fault.handler != nullptr, "install exception handler");
	g_red_zone_fault_offset = 0x38;
	Xbyak::CodeGenerator code(code_size, reinterpret_cast<void*>(mapping));
	Xbyak::Label loop, bad, finish;
	// Shape of eboot+5e95610: five pushes, a live descriptor pointer at
	// [rsp-38h], an indexed YMM store and a reload after the loop. No guest bytes.
	code.push(code.r15); code.push(code.r14); code.push(code.r13); code.push(code.r12); code.push(code.rbx);
	code.mov(code.r13, code.rsi);
	code.mov(code.r12, code.rsi);
	code.mov(code.qword[code.rsp - 0x38], code.r13);
	code.mov(code.qword[code.rsp - 0x30], code.rsi);
	code.vmovups(code.ymm1, code.ptr[code.rsi]);
	code.mov(code.r10, code.rdi);
	code.xor_(code.r8d, code.r8d);
	code.mov(code.r14d, 2);
	code.L(loop);
	code.vmovups(code.ptr[code.r10 + code.r8], code.ymm1);
	code.dec(code.r14d);
	code.jnz(loop);
	code.mov(code.rsi, code.qword[code.rsp - 0x30]);
	code.mov(code.r13, code.qword[code.rsp - 0x38]);
	// The control returns failure instead of dereferencing the destroyed pointer.
	code.cmp(code.r13, code.r12); code.jne(bad);
	code.cmp(code.rsi, code.r12); code.jne(bad);
	code.mov(code.eax, code.dword[code.r13]);
	code.cmp(code.eax, 0x40001234); code.jne(bad);
	code.mov(code.eax, 1); code.jmp(finish);
	code.L(bad); code.xor_(code.eax, code.eax);
	code.L(finish);
	code.vzeroupper();
	code.pop(code.rbx); code.pop(code.r12); code.pop(code.r13); code.pop(code.r14); code.pop(code.r15);
	code.ret();
	Check(test, Xbyak::GetError() == 0, "generate pointer-spill fixture");
	const std::array<uintptr_t, 1> starts {mapping};
	const std::array<uint32_t, 8> descriptor {0x40001234, 2, 3, 4, 5, 6, 7, 8};
	using GuestFunction = uint64_t(KYTY_SYSV_ABI*)(void*, const uint32_t*);
	const auto function = reinterpret_cast<GuestFunction>(mapping);
	const auto run = [&] {
		std::memset(g_red_zone_fault_page, 0, 32);
		DWORD old = 0;
		Check(test, VirtualProtect(g_red_zone_fault_page, 0x1000, PAGE_NOACCESS, &old) != 0, "arm page fault");
		const auto before = g_red_zone_fault_count;
		const bool intact = function(g_red_zone_fault_page, descriptor.data()) == 1;
		Check(test, g_red_zone_fault_count == before + 1, "exactly one real access violation");
		Check(test, std::memcmp(g_red_zone_fault_page, descriptor.data(), 32) == 0, "vector store completed");
		return intact;
	};
	Check(test, Common::VirtualMemory::FlushInstructionCache(mapping, code.getSize()), "flush fixture");
	Check(test, !run(), "control did not reproduce the destroyed descriptor pointer");
	Loader::RegisterGuestInstructionPatchModule(reinterpret_cast<void*>(mapping), code_size,
	    reinterpret_cast<void*>(mapping + code_size), code_size);
	const auto result = Loader::PatchGuestInstructions(mapping, code.getSize(), starts,
	    RedZoneProtectionEnabled(false, "PPSA03671", nullptr), false);
	Check(test, result.red_zone_function_count == 1 && result.patched_memory_instruction_count > 0 &&
	            result.unrelocatable_memory_instruction_count == 0, "protect live pointer across indexed YMM store");
	Check(test, run(), "protected fault destroyed the descriptor pointer");
	std::printf("[host]    %-48s ok\n", test);
}

void TestWindowsGuestRedZoneStaticPatcher() {
	const char* test = "WindowsGuestRedZoneStaticPatcher";
	constexpr uint64_t SENTINEL = 0x1122334455667788ull;
	constexpr uint64_t CODE_SIZE = 0x4000;
	constexpr uint64_t TRAMPOLINE_SIZE = 0x4000;
	const auto mapping = Libs::LibKernel::Memory::AllocateProgramMemory(
	    0x0000000902000000ull, CODE_SIZE + TRAMPOLINE_SIZE,
	    Common::VirtualMemory::Mode::ExecuteReadWrite, "red_zone_patcher_test");
	Check(test, mapping != 0, "failed to allocate patch test code");

	g_red_zone_fault_page = VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
	Check(test, g_red_zone_fault_page != nullptr, "failed to allocate fault page");
	auto* handler = AddVectoredExceptionHandler(1, RedZoneFaultHandler);
	Check(test, handler != nullptr, "failed to install test exception handler");

	using GuestFunction = uint64_t(KYTY_SYSV_ABI*)(const uint64_t*);
	const auto function = reinterpret_cast<GuestFunction>(mapping);
	const std::array<uintptr_t, 1> function_starts = {static_cast<uintptr_t>(mapping)};
	const auto run_fault = [&] {
		DWORD old_protection = 0;
		Check(test, VirtualProtect(g_red_zone_fault_page, 0x1000, PAGE_NOACCESS, &old_protection) != FALSE,
		      "failed to reset fault page protection");
		return function(static_cast<const uint64_t*>(g_red_zone_fault_page)) == 1;
	};
	bool reproduced = true;
	bool covered = true;
	bool preserved = true;
	// The partial excursions discard opposite halves of [rsp-24, rsp-16). Repair
	// only the retained half before moving RSP, leaving the discarded half live
	// across the fault. The zero excursion retains the original fixed-stack case.
	for (const auto [excursion, live]: {std::pair {0, true}, {0, false}, {8, true}, {20, true},
	                                    {128, true}, {256, true}, {-108, true}, {-128, true},
	                                    {-256, true}}) {
		Xbyak::CodeGenerator code(CODE_SIZE, reinterpret_cast<void*>(mapping));
		code.mov(code.rax, SENTINEL);
		code.mov(code.qword[code.rsp - 24], code.rax);
		code.mov(code.rax, code.ptr[code.rdi]); // Faultable three-byte instruction.
		if (!live) {
			code.mov(code.rax, SENTINEL);
			code.mov(code.qword[code.rsp - 24], code.rax);
		} else if (excursion < 0) {
			code.mov(code.dword[code.rsp - 20], static_cast<uint32_t>(SENTINEL >> 32));
		} else if (excursion > 0) {
			code.mov(code.dword[code.rsp - 24], static_cast<uint32_t>(SENTINEL));
		}
		if (excursion != 0) {
			code.lea(code.rsp, code.ptr[code.rsp - excursion]);
			code.lea(code.rsp, code.ptr[code.rsp + excursion]);
		}
		code.mov(code.rax, code.qword[code.rsp - 24]);
		code.mov(code.rcx, SENTINEL);
		code.cmp(code.rax, code.rcx);
		code.sete(code.al);
		code.movzx(code.eax, code.al);
		code.ret();
		Check(test, Xbyak::GetError() == 0, "failed to generate red-zone patch fixture");
		Check(test, Common::VirtualMemory::FlushInstructionCache(mapping, code.getSize()),
		      "failed to flush generated test code");
		reproduced &= run_fault() == !live;
		Loader::RegisterGuestInstructionPatchModule(
		    reinterpret_cast<void*>(mapping), CODE_SIZE, reinterpret_cast<void*>(mapping + CODE_SIZE),
		    TRAMPOLINE_SIZE);
		const auto result = Loader::PatchGuestInstructions(
		    mapping, code.getSize(), function_starts, true, false);
		const bool intact = run_fault();
		const uint64_t expected_patches = live ? 1 : 0;
		covered &= result.red_zone_function_count == 1 &&
		           result.memory_instruction_count == expected_patches &&
		           result.patched_memory_instruction_count == expected_patches &&
		           result.unrelocatable_memory_instruction_count == 0;
		preserved &= intact;
		std::printf("[host]    red-zone excursion=%d live=%d patched=%llu preserved=%d\n", excursion, live,
		            static_cast<unsigned long long>(result.patched_memory_instruction_count), intact);
	}

	Loader::UnregisterGuestInstructionPatchModule(reinterpret_cast<void*>(mapping));
	RemoveVectoredExceptionHandler(handler);
	VirtualFree(g_red_zone_fault_page, 0, MEM_RELEASE);
	g_red_zone_fault_page = nullptr;
	const bool freed = Libs::LibKernel::Memory::FreeGuestMemory(mapping, CODE_SIZE + TRAMPOLINE_SIZE);

	Check(test, reproduced, "test harness did not reproduce red-zone corruption");
	Check(test, covered, "static patcher did not cover the faultable instruction");
	Check(test, preserved, "patched fault still corrupted the guest red zone");
	Check(test, freed, "failed to free patch test code");
	std::printf("[host]    %-48s ok\n", test);
}
void TestAutoKeepsNativeRsqrt() {
    const char* test = "AutoKeepsNativeRsqrt";
    auto mapping = Libs::LibKernel::Memory::AllocateProgramMemory(0, 0x8000,
        Common::VirtualMemory::Mode::ExecuteReadWrite, test);
    Check(test, mapping != 0, "allocation failed");
    InstructionTestScope scope {mapping, 0x8000};
    Xbyak::CodeGenerator code(0x4000, reinterpret_cast<void*>(mapping));
    code.vrsqrtps(code.xmm0, code.xmm1); code.ret();
    std::vector<uint8_t> original(code.getCode(), code.getCurr());
    Loader::RegisterGuestInstructionPatchModule(reinterpret_cast<void*>(mapping), 0x4000,
        reinterpret_cast<void*>(mapping + 0x4000), 0x4000);
    std::array<uintptr_t, 1> starts {mapping};
    auto counts = Loader::PatchGuestInstructions(mapping, code.getSize(), starts, false, true, {}, false);
    Check(test, counts.reciprocal_sqrt.found == 0 &&
        std::equal(original.begin(), original.end(), code.getCode()), "auto changed native VRSQRTPS");
    std::printf("[host] AutoKeepsNativeRsqrt ok (byte-identical)\n");
}
void TestGuestCallStackArgument() {
    const char* test = "GuestCallStackArgument";
    auto mapping = Libs::LibKernel::Memory::AllocateProgramMemory(0, 0x8000,
        Common::VirtualMemory::Mode::ExecuteReadWrite, test);
    Check(test, mapping != 0, "allocation failed");
    InstructionTestScope scope {mapping, 0x8000};
    Xbyak::CodeGenerator code(0x4000, reinterpret_cast<void*>(mapping));
    Xbyak::Label helper;
    constexpr uint64_t sentinel = 0x123456789abcdef0ull;
    // Dynamic aligned alloca, like Il2Cpp+3177600: conservative analysis must
    // not shift an ordinary callee's stack arguments to protect a red zone.
    code.push(code.rbp); code.mov(code.rbp, code.rsp);
    code.sub(code.rsp, 0x100); code.and_(code.rsp, -32);
    code.lea(code.rax, code.ptr[code.rsp - 16]); code.mov(code.rsp, code.rax);
    code.mov(code.rax, sentinel); code.mov(code.qword[code.rsp], code.rax);
    code.call(helper);
    code.mov(code.rsp, code.rbp); code.pop(code.rbp); code.ret();
    const auto helper_address = reinterpret_cast<uintptr_t>(code.getCurr());
    code.L(helper); code.mov(code.rax, code.qword[code.rsp + 8]); code.ret();
    Common::VirtualMemory::FlushInstructionCache(mapping, code.getSize());
    auto function = reinterpret_cast<uint64_t(KYTY_SYSV_ABI*)()>(mapping);
    Check(test, function() == sentinel, "unpatched control failed");
    Loader::RegisterGuestInstructionPatchModule(reinterpret_cast<void*>(mapping), 0x4000,
        reinterpret_cast<void*>(mapping + 0x4000), 0x4000);
    std::array<uintptr_t, 2> starts {mapping, helper_address};
    auto result = Loader::PatchGuestInstructions(mapping, code.getSize(), starts, true, false);
    Check(test, function() == sentinel, "patcher shifted the guest stack argument");
    Check(test, result.live_call_count == 0, "ordinary guest call treated as TLS helper");
    std::printf("[host] GuestCallStackArgument ok\n");
}
void TestForkTlsCallProtection(bool prefixed) {
    const char* test = "ForkTlsCallProtection";
    auto mapping = Libs::LibKernel::Memory::AllocateProgramMemory(0, 0x8000,
        Common::VirtualMemory::Mode::ExecuteReadWrite, test);
    Check(test, mapping != 0, "allocation failed");
    InstructionTestScope scope {mapping, 0x8000};
    Xbyak::CodeGenerator code(0x4000, reinterpret_cast<void*>(mapping));
    Xbyak::Label helper;
    constexpr uint64_t sentinel = 0x123456789abcdef0ull;
    code.mov(code.rax, sentinel);
    code.mov(code.qword[code.rsp - 8], code.rax);
    const auto tls_call = reinterpret_cast<uintptr_t>(code.getCurr());
    if (prefixed) code.db(0x66);
    code.db(0x48); // Same REX.W CALL encoding as the loader's Call9.
    code.call(helper); // Models the fork's replacement of a TLS load by a call.
    code.mov(code.rax, code.qword[code.rsp - 8]);
    code.ret();
    const auto helper_address = reinterpret_cast<uintptr_t>(code.getCurr());
    code.L(helper); code.ret();
    Common::VirtualMemory::FlushInstructionCache(mapping, code.getSize());
    auto function = reinterpret_cast<uint64_t(KYTY_SYSV_ABI*)()>(mapping);
    Check(test, function() != sentinel, "control did not overwrite live red zone");
    Loader::RegisterGuestInstructionPatchModule(reinterpret_cast<void*>(mapping), 0x4000,
        reinterpret_cast<void*>(mapping + 0x4000), 0x4000);
    Loader::RegisterGuestInstructionTlsCall(tls_call);
    if (prefixed) Loader::RegisterGuestInstructionTlsCall(tls_call + 1);
    std::array<uintptr_t, 2> starts {mapping, helper_address};
    auto result = Loader::PatchGuestInstructions(mapping, code.getSize(), starts, true, false);
    Check(test, result.live_call_count == 1 && function() == sentinel,
        "TLS direct-call protection lost during port");
    std::printf("[host] ForkTlsCallProtection prefixed=%d ok\n", prefixed);
}
void TestTerminationMap() {
    using namespace Common::Termination;
    char text[300] {};
    ModuleLoaded(0x900000000, 0x1000, "fixture.bin");
    Check("TerminationMap", Describe(0x900000123, text, sizeof(text)) &&
        std::string(text) == "fixture.bin+0x123", "module resolution failed");
    Report("host-test", 7, 0x900000123, true, "synthetic");
    ModuleUnloaded(0x900000000);
    Check("TerminationMap", !Describe(0x900000123, text, sizeof(text)), "unloaded module retained");
    ModuleLoaded(0x900000000, 0x1000, "replacement.bin");
    Check("TerminationMap", Describe(0x900000123, text, sizeof(text)) &&
        std::string(text) == "replacement.bin+0x123", "reused mapping resolution failed");
    std::printf("[host] TerminationMap ok\n");
}
// Decode synthetic functions without executing them: isolates loader work from
// guest faults, GPU activity and file I/O when comparing debugger launches.
void BenchmarkPatcher() {
    constexpr size_t code_size = 4 * 1024 * 1024;
    auto mapping = Libs::LibKernel::Memory::AllocateProgramMemory(0, 2 * code_size,
        Common::VirtualMemory::Mode::ExecuteReadWrite, "patch-benchmark");
    Check("PatchBenchmark", mapping != 0, "allocation failed");
    InstructionTestScope scope {mapping, 2 * code_size};
    Xbyak::CodeGenerator code(code_size, reinterpret_cast<void*>(mapping));
    std::vector<uintptr_t> starts;
    for (unsigned n = 0; n < 4000; ++n) {
        starts.push_back(reinterpret_cast<uintptr_t>(code.getCurr()));
        for (unsigned i = 0; i < 40; ++i) { code.mov(code.eax, code.dword[code.rdi]); code.add(code.eax, 1); }
        code.ret();
    }
    Check("PatchBenchmark", Xbyak::GetError() == 0, "code generation failed");
    Loader::RegisterGuestInstructionPatchModule(reinterpret_cast<void*>(mapping), code_size,
        reinterpret_cast<void*>(mapping + code_size), code_size);
    const auto begin = std::chrono::steady_clock::now();
    auto counts = Loader::PatchGuestInstructions(mapping, code.getSize(), starts, true, true, {}, false);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
    Check("PatchBenchmark", counts.function_count == 4000 && counts.instruction_count == 324000, "decode count changed");
    std::printf("[PATCH_BENCHMARK] functions=%llu instructions=%llu elapsed_ms=%lld debugger=%d\n",
        counts.function_count, counts.instruction_count, ms, IsDebuggerPresent());
}
// Real Windows faults through two views of one section; no game or GPU. A
// neighbour's write must rearm the watch before a later write changes the table.
void TestMemoryWriteWatch() {
    const char* test = "MemoryWriteWatch";
    using namespace Common::MemoryWriteWatch;
    struct Views {
        HANDLE section = nullptr;
        uint8_t* guest = nullptr;
        uint8_t* backing = nullptr;
        ~Views() {
            Disarm();
            if (guest) UnmapViewOfFile(guest);
            if (backing) UnmapViewOfFile(backing);
            if (section) CloseHandle(section);
        }
    } views;
    views.section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 0x10000, nullptr);
    Check(test, views.section != nullptr, "section allocation failed");
    views.guest = static_cast<uint8_t*>(MapViewOfFile(views.section, FILE_MAP_ALL_ACCESS, 0, 0, 0x10000));
    views.backing = static_cast<uint8_t*>(MapViewOfFile(views.section, FILE_MAP_ALL_ACCESS, 0, 0, 0x10000));
    Check(test, views.guest && views.backing, "alias mapping failed");
    constexpr size_t Offset = 0xf70;
    auto* table = reinterpret_cast<volatile uint64_t*>(views.guest + Offset);
    auto* mirror = reinterpret_cast<volatile uint64_t*>(views.backing + Offset);
    for (unsigned path = 0; path < 3; ++path) {
        for (unsigned i = 0; i < 8; ++i) table[i] = i + 1;
        Check(test, Arm(reinterpret_cast<uint64_t>(views.guest + Offset),
                        reinterpret_cast<uint64_t>(views.backing + Offset)), "watch failed to arm");
        *reinterpret_cast<volatile uint64_t*>(views.guest + 0x20) = 0xabcdef;
        Check(test, writes.load() == 1 && active.load() && !changed.load(), "neighbour write ended watch");
        Check(test, Protection(reinterpret_cast<uint64_t>(views.guest)) == PAGE_READONLY,
              "neighbour write did not rearm page");
        // Model an external guest protection change: the backing watch survives.
        if (path == 2) {
            DWORD ignored = 0;
            {
                ProtectionScope protection(reinterpret_cast<uint64_t>(views.guest), Page);
                Check(test, VirtualProtect(views.guest, Page, PAGE_READONLY, &ignored) != 0,
                      "external read protection failed");
            }
            Check(test, !aliases[0].owned.load() && Protection(reinterpret_cast<uint64_t>(views.guest)) == PAGE_READONLY,
                  "watch took ownership of another reader's protection");
            {
                ProtectionScope protection(reinterpret_cast<uint64_t>(views.guest), Page);
                Check(test, VirtualProtect(views.guest, Page, PAGE_READWRITE, &ignored) != 0,
                      "external write protection failed");
            }
            Check(test, aliases[0].owned.load() && Protection(reinterpret_cast<uint64_t>(views.guest)) == PAGE_READONLY,
                  "watch was not rearmed when external protection became writable");
            Notify("test-protect", reinterpret_cast<uint64_t>(views.guest), Page, true);
            Check(test, !aliases[0].owned.load() && aliases[1].owned.load(), "lost backing watch on VM change");
        }
        if (path == 0) table[0] = 0;
        else mirror[0] = 0;
        Check(test, changed.load() && !active.load() && table[0] == 0 && mirror[0] == 0,
              "writer was not observed or instruction was not executed");
        Check(test, table[1] == 2 && writes.load() == 2 && !step.pending,
              "watch damaged neighbouring data or left a pending step");
        Check(test, Protection(reinterpret_cast<uint64_t>(views.guest)) == PAGE_READWRITE &&
                    Protection(reinterpret_cast<uint64_t>(views.backing)) == PAGE_READWRITE,
              "watch did not restore both aliases");
    }
    std::puts("[host] MemoryWriteWatch ok (guest, backing, neighbour rearm, VM change)");
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::strcmp(argv[1], "--patch-benchmark") == 0) { BenchmarkPatcher(); return 0; }
        if (argc == 2 && std::strcmp(argv[1], "--wolverine-redzone-only") == 0) { TestWolverineRedZonePointer(); return 0; }
        if (argc == 2 && std::strcmp(argv[1], "--memory-write-watch-only") == 0) { TestMemoryWriteWatch(); return 0; }
        TestMemoryWriteWatch();
        TestWindowsGuestRedZoneStaticPatcher();
        TestWolverineRedZonePointer();
        TestGuestCallStackArgument();
        TestForkTlsCallProtection(false);
        TestForkTlsCallProtection(true);
        TestAutoKeepsNativeRsqrt();
        TestPackedReciprocalSquareRoot();
        TestPackedBitFieldExtract();
        TestPackedBitFieldInsert();
        TestCpuExtensionPatches();
        TestTerminationMap();
        std::puts("Unified guest CPU host tests: all passed");
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
