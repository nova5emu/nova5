#include "common/gpuThreadSampler.h"

#if defined(_WIN32) && defined(_M_X64)
#include <dbghelp.h>
#include <tlhelp32.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <compare>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace Common::GpuThreadSampler {
namespace {
constexpr size_t kDepth = 32;
constexpr size_t kCapacity = 60000;
constexpr double kWindowSeconds = 60.0;
SRWLOCK g_session_lock = SRWLOCK_INIT;
HANDLE g_active_stop = nullptr, g_active_worker = nullptr;

struct SessionLock {
	SessionLock() { AcquireSRWLockExclusive(&g_session_lock); }
	~SessionLock() { ReleaseSRWLockExclusive(&g_session_lock); }
};

struct Range {
	DWORD64 begin = 0, end = 0;
	bool Contains(DWORD64 pc) const { return pc >= begin && pc < end; }
};

Range ModuleRange(HMODULE module) {
	if (!module) return {};
	const auto base = reinterpret_cast<DWORD64>(module);
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
	return {base, base + nt->OptionalHeader.SizeOfImage};
}

std::wstring Environment(const wchar_t* name) {
	const auto size = GetEnvironmentVariableW(name, nullptr, 0);
	if (!size) return {};
	std::wstring value(size, L'\0');
	const auto copied = GetEnvironmentVariableW(name, value.data(), size);
	if (!copied || copied >= size) return {};
	value.resize(copied);
	return value;
}

std::wstring AbsolutePath(const std::wstring& path) {
	const auto size = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
	if (!size) return path;
	std::wstring value(size, L'\0');
	const auto copied = GetFullPathNameW(path.c_str(), size, value.data(), nullptr);
	if (!copied || copied >= size) return path;
	value.resize(copied);
	return value;
}

enum class Location : uint8_t { Exe, WaitRip, SystemOther, External };
enum class Stop : uint8_t { UnknownModule, NoUnwind, StackBounds, DepthLimit, Fault, StackEnd };
struct Sample {
	DWORD64 pc[kDepth] {};
	uint8_t depth = 0;
	Location location = Location::External;
	Stop stop = Stop::UnknownModule;
	bool valid = false;
	DWORD context_error = 0;
	DWORD resume_error = 0;
	Phase phase = Phase::Other;
};

struct ModuleView {
	Range range;
	const RUNTIME_FUNCTION* functions = nullptr;
	size_t function_count = 0;
};

// Immutable during capture. No loader, heap, symbol engine or renderer access
// is permitted between SuspendThread and ResumeThread.
struct CaptureData {
	HANDLE target = nullptr;
	Range exe {}, ntdll {}, kernelbase {};
	ULONG_PTR stack_low = 0, stack_high = 0;
	ULONG_PTR alternate_low = 0, alternate_high = 0;
	const ModuleView* modules = nullptr;
	size_t module_count = 0;
	std::array<Range, 24> waits {};
	size_t wait_count = 0;
	const BoundaryState* boundary = nullptr;
};

struct Suspension {
	HANDLE target;
	Sample* sample;
	bool active;
};
thread_local Suspension* g_suspension = nullptr;

void Resume(Suspension* suspension) {
	if (suspension->active) {
		suspension->active = false;
		if (ResumeThread(suspension->target) == DWORD(-1)) suspension->sample->resume_error = GetLastError();
	}
}

LONG WINAPI UnwindException(PEXCEPTION_POINTERS) {
	// Run before the emulator's fault handlers, which may log or acquire locks.
	// No allocation, formatting or stack walk here. Normal SEH then handles the
	// fault; finally will see active=false and will NOT resume a second time.
	if (g_suspension && g_suspension->active) {
		g_suspension->sample->stop = Stop::Fault;
		Resume(g_suspension);
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

const ModuleView* LookupModule(const CaptureData* data, DWORD64 pc) {
	size_t lo = 0, hi = data->module_count;
	while (lo < hi) {
		const auto mid = lo + (hi - lo) / 2;
		const auto& module = data->modules[mid];
		if (pc < module.range.begin) hi = mid;
		else if (pc >= module.range.end) lo = mid + 1;
		else return &module;
	}
	return nullptr;
}

const RUNTIME_FUNCTION* LookupStatic(const ModuleView* module, DWORD64 pc) {
	if (!module) return nullptr;
	size_t lo = 0, hi = module->function_count;
	const auto rva = pc - module->range.begin;
	while (lo < hi) {
		const auto mid = lo + (hi - lo) / 2;
		const auto& entry = module->functions[mid];
		if (rva < entry.BeginAddress) hi = mid;
		else if (rva >= entry.EndAddress) lo = mid + 1;
		else return &entry;
	}
	return nullptr;
}

// POD only, including the SEH frames. NHANDLER does not call language handlers.
// RtlLookupFunctionEntry is deliberately NOT used here: it can consult dynamic
// tables/callbacks under locks owned by the suspended thread.
void CaptureSuspended(const CaptureData* data, Sample* sample) {
	CONTEXT context {};
	context.ContextFlags = CONTEXT_FULL;
	Suspension suspension {data->target, sample, false};
	g_suspension = &suspension; // Touch this thread's TLS before suspending.
	if (SuspendThread(data->target) == DWORD(-1)) {
		sample->context_error = GetLastError();
		g_suspension = nullptr;
		return;
	}
	suspension.active = true;
	__try {
		__try {
			if (!GetThreadContext(data->target, &context)) {
				sample->context_error = GetLastError();
			} else {
				sample->valid = true;
				if (data->boundary) sample->phase = data->boundary->phase.load(std::memory_order_relaxed);
				const auto top = context.Rip;
				if (data->exe.Contains(top)) sample->location = Location::Exe;
				else if (data->ntdll.Contains(top) || data->kernelbase.Contains(top)) {
					sample->location = Location::SystemOther;
					for (size_t i = 0; i < data->wait_count; ++i) {
						if (data->waits[i].Contains(top)) { sample->location = Location::WaitRip; break; }
					}
				}
				while (context.Rip != 0) {
					// Caller PCs are return addresses: attribute the call instruction,
					// not the next function if the return happens to be on a boundary.
					sample->pc[sample->depth] = context.Rip - (sample->depth != 0 ? 1 : 0);
					++sample->depth;
					if (sample->depth == kDepth) { sample->stop = Stop::DepthLimit; break; }
					const auto* module = LookupModule(data, sample->pc[sample->depth - 1]);
					if (!module) { sample->stop = Stop::UnknownModule; break; }
					// Unwind with the actual control PC; attribution uses PC-1 above.
					const auto* function = LookupStatic(module, context.Rip);
					if (!function) { sample->stop = Stop::NoUnwind; break; }
					const bool alternate = context.Rsp >= data->alternate_low && context.Rsp < data->alternate_high;
					const auto low = alternate ? data->alternate_low : data->stack_low;
					const auto high = alternate ? data->alternate_high : data->stack_high;
					if (context.Rsp < low || context.Rsp >= high) {
						sample->stop = Stop::StackBounds; break;
					}
					const auto previous_rsp = context.Rsp;
					const auto previous_rip = context.Rip;
					PVOID handler_data = nullptr;
					DWORD64 establisher = 0;
					RtlVirtualUnwind(UNW_FLAG_NHANDLER, module->range.begin, context.Rip,
					    const_cast<RUNTIME_FUNCTION*>(function), &context, &handler_data, &establisher, nullptr);
					if (!suspension.active) break; // Fault handler already resumed.
					if (context.Rsp < previous_rsp || context.Rsp > high ||
					    (context.Rsp == previous_rsp && context.Rip == previous_rip)) {
						sample->stop = Stop::StackBounds; break;
					}
				}
				if (context.Rip == 0) sample->stop = Stop::StackEnd;
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			sample->stop = Stop::Fault;
		}
	} __finally {
		// Exactly one resume for our successful suspend, including SEH faults.
		Resume(&suspension);
		g_suspension = nullptr;
	}
}

// Setup only, before starting the sampler. For leaf NT syscall stubs without
// pdata, use the next distinct export (capped at 64 bytes), never the whole DLL.
void AddWait(CaptureData& data, HMODULE module, const char* name) {
	if (!module || data.wait_count == data.waits.size()) return;
	const auto pc = reinterpret_cast<DWORD64>(GetProcAddress(module, name));
	const auto range = ModuleRange(module);
	if (!range.Contains(pc)) return;
	DWORD64 base = 0;
	const auto* function = RtlLookupFunctionEntry(pc, &base, nullptr);
	Range wait {pc, (std::min)(pc + 64, range.end)};
	if (function && base == range.begin) {
		wait = {base + function->BeginAddress, base + function->EndAddress};
	} else {
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(range.begin);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(range.begin + dos->e_lfanew);
		const auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
		if (directory.VirtualAddress) {
			const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(range.begin + directory.VirtualAddress);
			const auto* addresses = reinterpret_cast<const DWORD*>(range.begin + exports->AddressOfFunctions);
			for (DWORD i = 0; i < exports->NumberOfFunctions; ++i) {
				const auto next = range.begin + addresses[i];
				if (next > pc && next < wait.end) wait.end = next;
			}
		}
	}
	data.waits[data.wait_count++] = wait;
}

struct Export {
	DWORD rva;
	std::string name;
};

// Loaded only while the target is running. A normal reference (NOT PIN) keeps
// code/unwind data alive until the window has been reported, then is released.
struct HeldModule {
	HMODULE handle = nullptr;
	Range range;
	std::wstring path;
	std::string name;
	std::vector<RUNTIME_FUNCTION> functions;
	std::vector<Export> exports;
	~HeldModule() { if (handle) FreeLibrary(handle); }

	void ReadImage() {
		range = ModuleRange(handle);
		std::wstring filename(32768, L'\0');
		const auto length = GetModuleFileNameW(handle, filename.data(), static_cast<DWORD>(filename.size()));
		filename.resize(length < filename.size() ? length : 0);
		path = filename;
		filename = filename.substr(filename.find_last_of(L"\\/") + 1);
		const auto bytes = WideCharToMultiByte(CP_UTF8, 0, filename.data(), static_cast<int>(filename.size()), nullptr, 0, nullptr, nullptr);
		name.resize(bytes);
		if (bytes) WideCharToMultiByte(CP_UTF8, 0, filename.data(), static_cast<int>(filename.size()), name.data(), bytes, nullptr, nullptr);
		if (name.empty()) name = "unnamed_module";
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(range.begin);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(range.begin + dos->e_lfanew);
		if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return;
		const auto image_size = range.end - range.begin;
		const auto inside = [image_size](DWORD rva, uint64_t size) { return rva && uint64_t{rva} + size <= image_size; };
		const auto pdata = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
		if (inside(pdata.VirtualAddress, pdata.Size) && pdata.Size % sizeof(RUNTIME_FUNCTION) == 0) {
			const auto* entries = reinterpret_cast<const RUNTIME_FUNCTION*>(range.begin + pdata.VirtualAddress);
			functions.assign(entries, entries + pdata.Size / sizeof(RUNTIME_FUNCTION));
			DWORD previous_end = 0;
			for (const auto& entry : functions) {
				if (entry.BeginAddress < previous_end || entry.BeginAddress >= entry.EndAddress || entry.EndAddress > image_size) {
					functions.clear(); break;
				}
				previous_end = entry.EndAddress;
			}
		}
		const auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
		if (!inside(directory.VirtualAddress, directory.Size) || directory.Size < sizeof(IMAGE_EXPORT_DIRECTORY)) return;
		const auto* table = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(range.begin + directory.VirtualAddress);
		if (!inside(table->AddressOfNames, uint64_t{table->NumberOfNames} * sizeof(DWORD)) ||
		    !inside(table->AddressOfNameOrdinals, uint64_t{table->NumberOfNames} * sizeof(WORD)) ||
		    !inside(table->AddressOfFunctions, uint64_t{table->NumberOfFunctions} * sizeof(DWORD))) return;
		const auto* names = reinterpret_cast<const DWORD*>(range.begin + table->AddressOfNames);
		const auto* ordinals = reinterpret_cast<const WORD*>(range.begin + table->AddressOfNameOrdinals);
		const auto* addresses = reinterpret_cast<const DWORD*>(range.begin + table->AddressOfFunctions);
		for (DWORD i = 0; i < table->NumberOfNames; ++i) {
			if (ordinals[i] >= table->NumberOfFunctions || !inside(names[i], 1)) continue;
			const auto rva = addresses[ordinals[i]];
			if (!inside(rva, 1) || (rva >= directory.VirtualAddress && uint64_t{rva} < uint64_t{directory.VirtualAddress} + directory.Size)) continue;
			const auto* text = reinterpret_cast<const char*>(range.begin + names[i]);
			const auto* end = static_cast<const char*>(std::memchr(text, 0, image_size - names[i]));
			if (end) exports.push_back({rva, std::string(text, end)});
		}
		std::sort(exports.begin(), exports.end(), [](const auto& a, const auto& b) {
			return a.rva != b.rva ? a.rva < b.rva : a.name < b.name;
		});
	}
	std::string NearExport(DWORD64 pc) const {
		const auto rva = pc - range.begin;
		auto it = std::upper_bound(exports.begin(), exports.end(), rva,
		    [](DWORD64 address, const Export& symbol) { return address < symbol.rva; });
		if (it == exports.begin()) return "none";
		--it;
		char delta[32] {};
		std::snprintf(delta, sizeof(delta), "+0x%llx", static_cast<unsigned long long>(rva - it->rva));
		return it->name + delta; // A hint only, never an ownership/aggregation key.
	}
};

struct ModuleSnapshot {
	std::vector<std::unique_ptr<HeldModule>> owners;
	std::vector<ModuleView> views;
	DWORD error = 0;

	void Load() {
		views.clear(); owners.clear(); error = 0;
		HANDLE snapshot = INVALID_HANDLE_VALUE;
		for (int attempt = 0; attempt < 4; ++attempt) {
			snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
			if (snapshot != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH) break;
		}
		if (snapshot == INVALID_HANDLE_VALUE) { error = GetLastError(); return; }
		struct CloseSnapshot { HANDLE value; ~CloseSnapshot() { CloseHandle(value); } } close {snapshot};
		MODULEENTRY32W entry {};
		entry.dwSize = sizeof(entry);
		if (!Module32FirstW(snapshot, &entry)) { error = GetLastError(); return; }
		do {
			auto module = std::make_unique<HeldModule>();
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
			    reinterpret_cast<LPCWSTR>(entry.modBaseAddr), &module->handle)) continue;
			module->ReadImage();
			owners.push_back(std::move(module));
		} while (Module32NextW(snapshot, &entry));
		// ELF ranges are metadata only: never dereference guest code/unwind data
		// while suspended. They provide module+RVA; stacks stop at no_unwind.
		for (const auto& guest: GuestCpuProfile::modules) {
			if (!guest.ready.load(std::memory_order_acquire) || !guest.alive.load()) continue;
			auto module = std::make_unique<HeldModule>();
			module->range = {guest.base, guest.base + guest.size};
			module->name = std::string("guest:") + guest.name;
			owners.push_back(std::move(module));
		}
		std::sort(owners.begin(), owners.end(), [](const auto& a, const auto& b) { return a->range.begin < b->range.begin; });
		owners.erase(std::unique(owners.begin(), owners.end(), [](const auto& a, const auto& b) {
			return a->range.begin == b->range.begin;
		}), owners.end());
		for (const auto& module : owners) views.push_back({module->range, module->functions.data(), module->functions.size()});
	}
};

struct FunctionId {
	DWORD64 begin = 0, end = 0;
	const ModuleView* module = nullptr;
	bool pdata = false;
};
FunctionId Identify(const CaptureData& data, DWORD64 pc) {
	const auto* module = LookupModule(&data, pc);
	const auto* function = LookupStatic(module, pc);
	return function ? FunctionId {module->range.begin + function->BeginAddress, module->range.begin + function->EndAddress, module, true}
	                : FunctionId {pc, pc + 1, module, false};
}

struct FunctionCounts { FunctionId id; size_t self = 0, inclusive = 0; };
struct CallerKey {
	DWORD64 top_module = 0, callee = 0, caller = 0, callsite = 0;
	auto operator<=>(const CallerKey&) const = default;
};
struct ModuleCallerKey {
	DWORD64 top_module = 0, callee_module = 0, caller = 0;
	Phase phase = Phase::Other;
	auto operator<=>(const ModuleCallerKey&) const = default;
};
struct Counts {
	std::map<DWORD64, FunctionCounts> functions;
	std::map<DWORD64, size_t> pcs, modules;
	std::map<CallerKey, size_t> callers;
	std::map<ModuleCallerKey, size_t> module_callers;
	size_t phases[PhaseCount] {}, phase_wait_rip[PhaseCount] {};
	std::map<std::pair<Phase, DWORD64>, size_t> phase_modules;
	size_t locations[4] {}, stops[6] {}, valid = 0, failed = 0, frames = 0, unknown_self = 0;
	size_t external_with_exe_caller = 0, external_without_exe_caller = 0;
	DWORD first_context_error = 0, resume_error = 0;
};

Counts Aggregate(const CaptureData& data, const Sample* samples, size_t count) {
	Counts result;
	for (size_t i = 0; i < count; ++i) {
		const auto& sample = samples[i];
		if (sample.resume_error) result.resume_error = sample.resume_error;
		if (!sample.valid) { ++result.failed; if (!result.first_context_error) result.first_context_error = sample.context_error; continue; }
		++result.valid; ++result.locations[static_cast<size_t>(sample.location)];
		++result.phases[static_cast<size_t>(sample.phase)];
		if (sample.location == Location::WaitRip) ++result.phase_wait_rip[static_cast<size_t>(sample.phase)];
		++result.stops[static_cast<size_t>(sample.stop)];
		DWORD64 seen[kDepth] {};
		size_t seen_count = 0;
		const auto* top = sample.depth ? LookupModule(&data, sample.pc[0]) : nullptr;
		++result.phase_modules[{sample.phase, top ? top->range.begin : 0}];
		if (!sample.depth) { ++result.modules[0]; ++result.unknown_self; }
		bool caller_found = false;
		for (size_t j = 0; j < sample.depth; ++j) {
			const auto pc = sample.pc[j];
			const auto id = Identify(data, pc);
			auto& total = result.functions.try_emplace(id.begin, FunctionCounts {id}).first->second;
			++result.frames;
			if (j == 0) {
				++total.self; ++result.pcs[pc];
				++result.modules[top ? top->range.begin : 0];
				if (!top) ++result.unknown_self;
			}
			if (std::find(seen, seen + seen_count, id.begin) == seen + seen_count) {
				seen[seen_count++] = id.begin; ++total.inclusive;
			}
			if (j && sample.location != Location::Exe && !caller_found && data.exe.Contains(pc)) {
				const auto callee = Identify(data, sample.pc[j - 1]);
				++result.callers[{top ? top->range.begin : 0, callee.begin, id.begin, pc}];
				++result.module_callers[{top ? top->range.begin : 0, callee.module ? callee.module->range.begin : 0, id.begin, sample.phase}];
				caller_found = true;
			}
		}
		if (sample.location != Location::Exe) {
			if (caller_found) ++result.external_with_exe_caller;
			else {
				++result.external_without_exe_caller;
				++result.module_callers[{top ? top->range.begin : 0, 0, 0, sample.phase}];
			}
		}
	}
	return result;
}

// All DbgHelp operations occur after the last ResumeThread of a window. Use a
// unique session handle and local-only search path, never a symbol server.
class Symbols {
public:
	Symbols(const std::wstring& exe, Range range) : m_range(range) {
		m_library = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (!m_library) return;
#define LOAD_SYMBOL(name) name = reinterpret_cast<decltype(name)>(GetProcAddress(m_library, #name))
		LOAD_SYMBOL(SymInitializeW); LOAD_SYMBOL(SymCleanup); LOAD_SYMBOL(SymLoadModuleExW);
		LOAD_SYMBOL(SymFromAddr); LOAD_SYMBOL(SymGetModuleInfoW64);
		LOAD_SYMBOL(SymGetOptions); LOAD_SYMBOL(SymSetOptions);
#undef LOAD_SYMBOL
		if (!SymInitializeW || !SymCleanup || !SymLoadModuleExW || !SymFromAddr ||
		    !SymGetModuleInfoW64 || !SymGetOptions || !SymSetOptions) return;
		m_context = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!m_context) return;
		m_old_options = SymGetOptions();
		m_options_set = true;
		SymSetOptions((m_old_options & ~(SYMOPT_LOAD_ANYTHING | SYMOPT_DEFERRED_LOADS)) |
		    SYMOPT_EXACT_SYMBOLS | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS |
		    SYMOPT_NO_PROMPTS | SYMOPT_IGNORE_NT_SYMPATH);
		const auto directory = exe.substr(0, exe.find_last_of(L"\\/"));
		m_initialized = SymInitializeW(m_context, directory.c_str(), FALSE) != FALSE;
		if (!m_initialized) return;
		if (!SymLoadModuleExW(m_context, nullptr, exe.c_str(), nullptr, range.begin,
		    static_cast<DWORD>(range.end - range.begin), nullptr, 0)) return;
		IMAGEHLP_MODULEW64 info {};
		info.SizeOfStruct = sizeof(info);
		if (SymGetModuleInfoW64(m_context, range.begin, &info)) {
			m_ready = !info.PdbUnmatched && !info.DbgUnmatched && info.SymType == SymPdb;
			m_pdb = m_ready;
		}
	}
	~Symbols() {
		if (m_initialized) SymCleanup(m_context);
		if (m_options_set) SymSetOptions(m_old_options);
		if (m_context) CloseHandle(m_context);
		if (m_library) FreeLibrary(m_library);
	}
	bool HasPdb() const { return m_pdb; }
	std::string Resolve(const FunctionId& id, bool& resolved) {
		alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 2048] {};
		auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
		symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
		symbol->MaxNameLen = 2048;
		DWORD64 displacement = 0;
		// Public PDB symbols can have Size==0. Query the canonical entry and
		// accept an exact address, independently of Size. Never use a preceding
		// public symbol from a different pdata range (nor use names as keys).
		if (m_ready && m_range.Contains(id.begin) && SymFromAddr(m_context, id.begin, &displacement, symbol) &&
		    symbol->Address == id.begin) {
			resolved = true;
			return std::string(symbol->Name, symbol->NameLen);
		}
		resolved = false;
		return "unresolved";
	}
private:
	HMODULE m_library = nullptr;
	HANDLE m_context = nullptr;
	Range m_range;
	DWORD m_old_options = 0;
	bool m_initialized = false, m_options_set = false, m_ready = false, m_pdb = false;
#define SYMBOL_POINTER(name) decltype(&::name) name = nullptr
	SYMBOL_POINTER(SymInitializeW); SYMBOL_POINTER(SymCleanup); SYMBOL_POINTER(SymLoadModuleExW);
	SYMBOL_POINTER(SymFromAddr); SYMBOL_POINTER(SymGetModuleInfoW64);
	SYMBOL_POINTER(SymGetOptions); SYMBOL_POINTER(SymSetOptions);
#undef SYMBOL_POINTER
};
} // namespace

__declspec(noinline) void NameCaller(const char* name) {
	if (auto* state = boundary_state) {
		const auto count = state->anchor_count.load(std::memory_order_relaxed);
		for (size_t i = 0; i < count; ++i) if (state->anchors[i].name == name) return;
		if (count == state->anchors.size()) return;
		state->anchors[count] = {reinterpret_cast<uintptr_t>(_ReturnAddress()) - 1, name};
		state->anchor_count.store(count + 1, std::memory_order_release);
	}
}

#if defined(KYTY_GPU_SAMPLER_TESTS)
bool CheckAggregationForTests() {
	const RUNTIME_FUNCTION exe_functions[] = {{0x100, 0x140, 0}, {0x140, 0x180, 0}};
	const RUNTIME_FUNCTION dll_functions[] = {{0x100, 0x180, 0}};
	const ModuleView modules[] = {{{0x1000, 0x2000}, exe_functions, 2}, {{0x3000, 0x4000}, dll_functions, 1}};
	CaptureData data;
	data.exe = modules[0].range; data.modules = modules; data.module_count = 2;
	Sample samples[4] {};
	samples[0].pc[0] = 0x1110; samples[0].pc[1] = 0x1120; samples[0].pc[2] = 0x1150; samples[0].depth = 3;
	samples[1].pc[0] = 0x1130; samples[1].pc[1] = 0x1160; samples[1].depth = 2;
	samples[2].pc[0] = 0x3110; samples[2].pc[1] = 0x1125; samples[2].pc[2] = 0x1170; samples[2].depth = 3;
	samples[3].pc[0] = 0x99999; samples[3].depth = 1;
	samples[0].phase = Phase::Prepare; samples[2].phase = Phase::DrawEmit;
	for (size_t i = 0; i < 4; ++i) { samples[i].valid = true; samples[i].location = i < 2 ? Location::Exe : Location::External; }
	const auto counts = Aggregate(data, samples, 4);
	const ModuleView guest_modules[] = {modules[0], modules[1], {{0x5000, 0x6000}, nullptr, 0}};
	auto guest_data = data; guest_data.modules = guest_modules; guest_data.module_count = 3;
	Sample guest_samples[2] {};
	guest_samples[0].valid = true; guest_samples[0].pc[0] = 0x5123; guest_samples[0].depth = 1;
	guest_samples[1].valid = true; guest_samples[1].pc[0] = 0x3110; guest_samples[1].pc[1] = 0x1125;
	guest_samples[1].depth = 2; guest_samples[1].location = Location::WaitRip;
	const auto guest_counts = Aggregate(guest_data, guest_samples, 2);
	if (guest_counts.modules.at(0x5000) != 1 || guest_counts.unknown_self != 0 ||
	    guest_counts.functions.at(0x5123).id.pdata || guest_counts.locations[static_cast<size_t>(Location::WaitRip)] != 1 ||
	    guest_counts.functions.at(0x1100).inclusive != 1) return false;
	return counts.valid == 4 && counts.functions.at(0x1100).self == 2 && counts.functions.at(0x1100).inclusive == 3 &&
	       counts.functions.at(0x1140).self == 0 && counts.functions.at(0x1140).inclusive == 3 &&
	       counts.functions.at(0x3100).self == 1 && counts.functions.at(0x3100).inclusive == 1 &&
	       counts.modules.at(0) == 1 && counts.unknown_self == 1 &&
	       counts.external_with_exe_caller == 1 && counts.external_without_exe_caller == 1 &&
	       counts.callers.at({0x3000, 0x3100, 0x1100, 0x1125}) == 1 &&
	       counts.module_callers.at({0x3000, 0x3000, 0x1100, Phase::DrawEmit}) == 1 &&
	       counts.module_callers.at({0, 0, 0, Phase::Other}) == 1 &&
	       counts.phases[static_cast<size_t>(Phase::Prepare)] == 1 &&
	       counts.phases[static_cast<size_t>(Phase::DrawEmit)] == 1 &&
	       counts.phase_modules.at({Phase::DrawEmit, 0x3000}) == 1 &&
	       Identify(data, 0x1140).begin == 0x1140 && !Identify(data, 0x1180).pdata;
}
#endif

struct Session::Impl {
	struct GuestTarget {
		CaptureData data;
		const GuestCpuProfile::Row* row = nullptr;
		std::vector<Sample> samples;
		size_t count = 0;
		uint64_t selection_cpu = 0, cpu_start = 0, cpu_end = 0, pause_ticks = 0, max_pause_ticks = 0;
		bool exited = false;
		~GuestTarget() { if (data.target) CloseHandle(data.target); }
	};
	std::vector<std::unique_ptr<GuestTarget>> guests;
	struct Candidate { const GuestCpuProfile::Row* row; uint64_t cpu; bool selected; };
	std::vector<Candidate> candidates;
	std::array<GuestCpuProfile::Totals, 1024> fault_start {};
	std::array<bool, 1024> fault_start_valid {};
	std::map<std::pair<size_t, uint64_t>, uint64_t> fault_page_start;
	std::array<uint64_t, 1024> fault_page_drop_start {};
	uint64_t owner_token = 0, fault_begin_ticks = 0, fault_end_ticks = 0;
	bool gpu_sampling = GuestCpuProfile::EnvOn("KYTY_GPU_THREAD_SAMPLER");
	double selection_seconds = 0;
	CaptureData data;
	HANDLE stop = nullptr, worker = nullptr, timer = nullptr;
	PVOID exception_handler = nullptr;
	ModuleSnapshot modules;
	std::vector<Sample> samples;
	std::wstring gate, output, exe;
	LARGE_INTEGER frequency {};
	DWORD tid = 0;
	size_t count = 0;
	uint64_t pause_ticks = 0, max_pause_ticks = 0;
	unsigned window = 0;
	bool high_resolution = false;
	std::unique_ptr<BoundaryState> boundary;

	~Impl() {
		if (stop) SetEvent(stop);
		if (worker) { WaitForSingleObject(worker, INFINITE); CloseHandle(worker); }
		if (exception_handler) RemoveVectoredExceptionHandler(exception_handler);
		if (timer) CloseHandle(timer);
		if (stop) CloseHandle(stop);
		if (data.target) CloseHandle(data.target);
	}
	bool Start() {
		if (Environment(L"KYTY_GPU_THREAD_SAMPLER_BOUNDARY") == L"1") {
			boundary = std::make_unique<BoundaryState>();
			data.boundary = boundary.get();
		}
		tid = GetCurrentThreadId();
		if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
		    &data.target, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
		    FALSE, 0)) return false;
		GetCurrentThreadStackLimits(&data.stack_low, &data.stack_high);
		// StackLimit can move down as the target commits more pages after startup.
		// Keep the reservation base, not the initial committed low watermark.
		MEMORY_BASIC_INFORMATION stack_region {};
		if (!VirtualQuery(&stack_region, &stack_region, sizeof(stack_region))) return false;
		data.stack_low = reinterpret_cast<ULONG_PTR>(stack_region.AllocationBase);
		data.exe = ModuleRange(GetModuleHandleW(nullptr));
		const auto ntdll = GetModuleHandleW(L"ntdll.dll");
		const auto kernelbase = GetModuleHandleW(L"KernelBase.dll");
		data.ntdll = ModuleRange(ntdll); data.kernelbase = ModuleRange(kernelbase);
		for (const auto* name : {"NtWaitForSingleObject", "NtWaitForMultipleObjects", "NtDelayExecution",
		    "NtSignalAndWaitForSingleObject", "NtWaitForAlertByThreadId", "NtWaitForKeyedEvent"}) AddWait(data, ntdll, name);
		for (const auto* name : {"WaitForSingleObjectEx", "WaitForMultipleObjectsEx", "SleepEx", "WaitOnAddress",
		    "SleepConditionVariableCS", "SleepConditionVariableSRW"}) AddWait(data, kernelbase, name);
		gate = Environment(L"KYTY_GPU_THREAD_SAMPLER_GATE");
		gate = AbsolutePath(gate.empty() ? L"gpu-sampler.trigger" : gate);
		output = Environment(L"KYTY_GPU_THREAD_SAMPLER_FILE");
		if (!output.empty()) output = AbsolutePath(output);
		exe.resize(32768);
		const auto length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
		if (!length || length == exe.size()) return false;
		exe.resize(length);
		if (gpu_sampling) samples.resize(kCapacity); // Allocate and touch BEFORE suspending.
		if (!QueryPerformanceFrequency(&frequency)) return false;
		stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		// High resolution is local to this timer. Do not change timeBeginPeriod.
		timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
		high_resolution = timer != nullptr;
		if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
		if (!stop || !timer) return false;
		exception_handler = AddVectoredExceptionHandler(1, UnwindException);
		if (!exception_handler) return false;
		worker = CreateThread(nullptr, 0, Run, this, 0, nullptr);
		return worker != nullptr;
	}
	bool GateOpen() const {
		const auto attributes = GetFileAttributesW(gate.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
	}
	static bool CpuTime(HANDLE thread, uint64_t creation, uint64_t& cpu) {
		FILETIME created {}, exit {}, kernel {}, user {};
		if (!GetThreadTimes(thread, &created, &exit, &kernel, &user)) return false;
		using GuestCpuProfile::FileTimeTicks;
		if (creation && FileTimeTicks(created.dwLowDateTime, created.dwHighDateTime) != creation) return false;
		cpu = FileTimeTicks(kernel.dwLowDateTime, kernel.dwHighDateTime) + FileTimeTicks(user.dwLowDateTime, user.dwHighDateTime);
		return true;
	}
	void SelectGuests() {
		guests.clear(); candidates.clear(); selection_seconds = 0;
		const auto limit = GuestCpuProfile::GuestCount();
		if (!limit) return;
		for (const auto& row: GuestCpuProfile::rows) {
			if (!row.ready.load(std::memory_order_acquire) || !row.alive.load() || row.tid == tid) continue;
			auto target = std::make_unique<GuestTarget>();
			target->row = &row; target->data = data; target->data.boundary = nullptr;
			target->data.target = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | SYNCHRONIZE, FALSE, row.tid);
			if (!target->data.target || !CpuTime(target->data.target, row.creation, target->cpu_start)) continue;
			target->data.stack_low = row.guest_low; target->data.stack_high = row.guest_high;
			target->data.alternate_low = row.host_low; target->data.alternate_high = row.host_high;
			guests.push_back(std::move(target));
		}
		const auto start = GuestCpuProfile::Now();
		// Rank a short, current gameplay interval, not accumulated startup CPU.
		WaitForSingleObject(stop, 500);
		selection_seconds = double(GuestCpuProfile::Now() - start) / frequency.QuadPart;
		for (auto& target: guests) {
			uint64_t end = target->cpu_start;
			if (CpuTime(target->data.target, target->row->creation, end) && end >= target->cpu_start)
				target->selection_cpu = end - target->cpu_start;
			target->cpu_start = end;
		}
		std::erase_if(guests, [](const auto& target) { return WaitForSingleObject(target->data.target, 0) == WAIT_OBJECT_0; });
		std::sort(guests.begin(), guests.end(), [](const auto& a, const auto& b) {
			return a->selection_cpu != b->selection_cpu ? a->selection_cpu > b->selection_cpu : a->row->tid < b->row->tid;
		});
		for (size_t i = 0; i < guests.size(); ++i) candidates.push_back({guests[i]->row, guests[i]->selection_cpu, i < limit});
		if (guests.size() > limit) guests.resize(limit);
		for (auto& target: guests) target->samples.resize(kCapacity);
	}
	void BeginFaults() {
		if (!GuestCpuProfile::FaultEnabled()) return;
		owner_token = GuestCpuProfile::epoch.fetch_add(1) + 1;
		fault_page_start.clear(); fault_page_drop_start.fill(0);
		for (size_t i = 0; i < fault_start.size(); ++i) {
			fault_start[i] = {};
			fault_start_valid[i] = GuestCpuProfile::Read(GuestCpuProfile::rows[i], fault_start[i]);
			const auto& row = GuestCpuProfile::rows[i];
			if (GuestCpuProfile::AddressesEnabled() && row.ready.load(std::memory_order_acquire) && row.pages) {
				row.pages->ForEach([&](uint64_t page, uint64_t n) { fault_page_start[{i, page}] = n; });
				fault_page_drop_start[i] = row.pages->dropped.load();
			}
		}
		fault_begin_ticks = GuestCpuProfile::Now();
		if (GuestCpuProfile::AddressesEnabled()) {
			GuestCpuProfile::buffer_owners[1].requested.store(0, std::memory_order_release);
			GuestCpuProfile::buffer_owners[0].requested.store(owner_token, std::memory_order_release);
		}
		GuestCpuProfile::recording.store(true);
	}
	void EndFaults() {
		GuestCpuProfile::recording.store(false);
		GuestCpuProfile::epoch.fetch_add(1);
	}
	void FinishOwnerSnapshot() {
		if (!GuestCpuProfile::FaultEnabled() || !GuestCpuProfile::AddressesEnabled()) return;
		fault_end_ticks = GuestCpuProfile::Now();
		auto& end = GuestCpuProfile::buffer_owners[1];
		end.requested.store(owner_token, std::memory_order_release);
		// Only the GPU lane traverses BufferCache. Wait outside the measured
		// interval and outside suspension; shutdown/no frames produce a missing
		// snapshot, never a guessed owner or an unbounded wait.
		for (unsigned i = 0; i < 50 && end.published.load(std::memory_order_acquire) != owner_token; ++i)
			if (WaitForSingleObject(stop, 5) == WAIT_OBJECT_0) break;
	}
	void ReportFaultAddresses(std::FILE* file, double seconds, uint64_t total_calls) {
		if (!GuestCpuProfile::AddressesEnabled()) return;
		std::map<uint64_t, uint64_t> pages, regions, owners[2], owner_calls;
		uint64_t addressed = 0, dropped = 0;
		for (size_t i = 0; i < GuestCpuProfile::rows.size(); ++i) {
			const auto& row = GuestCpuProfile::rows[i];
			if (!row.ready.load(std::memory_order_acquire) || !row.pages) continue;
			row.pages->ForEach([&](uint64_t page, uint64_t n) {
				const auto it = fault_page_start.find({i, page});
				const auto before = it == fault_page_start.end() ? 0 : it->second;
				if (n > before) { pages[page] += n - before; addressed += n - before; }
			});
			dropped += row.pages->dropped.load() - fault_page_drop_start[i];
		}
		bool ready[2] {};
		for (size_t i = 0; i < 2; ++i) {
			const auto& snapshot = GuestCpuProfile::buffer_owners[i];
			ready[i] = snapshot.published.load(std::memory_order_acquire) == owner_token;
			if (ready[i]) for (size_t j = 0; j < snapshot.count; ++j)
				owners[i].emplace(snapshot.ranges[j].base, snapshot.ranges[j].size);
			const auto requested_ticks = i ? fault_end_ticks : fault_begin_ticks;
			std::fprintf(file, "[WRITE_FAULT_ADDRESSES] snapshot=%s ready=%u owners=%zu dropped=%zu lag_ms=%.4f\n",
			    i ? "end" : "start", unsigned(ready[i]), ready[i] ? snapshot.count : 0,
			    ready[i] ? snapshot.dropped : 0,
			    ready[i] && snapshot.captured_ticks >= requested_ticks ?
			        (snapshot.captured_ticks - requested_ticks) * 1000.0 / frequency.QuadPart : 0);
		}
		const auto percent = [&](uint64_t n) { return total_calls ? n * 100.0 / total_calls : 0; };
		uint64_t classified = 0, large_owner_calls = 0;
		for (const auto& [page, n]: pages) {
			regions[page & ~uint64_t{65535}] += n;
			if (!ready[0] || !ready[1]) continue;
			auto it = owners[0].upper_bound(page);
			if (it == owners[0].begin()) continue;
			--it;
			const auto same = owners[1].find(it->first);
			if (same == owners[1].end() || same->second != it->second ||
			    !GuestCpuProfile::ContainsFaultPage(it->first, it->second, page)) continue;
			owner_calls[it->first] += n; classified += n;
			if (it->second >= 1024 * 1024) large_owner_calls += n;
		}
		std::fprintf(file, "[WRITE_FAULT_ADDRESSES] total window=%u handler_calls=%llu addressed=%llu table_dropped=%llu coverage_pct=%.4f accounting_delta=%lld pages=%zu regions=%zu stable_endpoint_owner_calls=%llu unclassified=%llu large_owner_calls=%llu large_owner_pct=%.4f owner_identity=address_size_at_endpoints_not_lifetime\n",
		    window, total_calls, addressed, dropped, percent(addressed),
		    static_cast<long long>(total_calls) - static_cast<long long>(addressed + dropped),
		    pages.size(), regions.size(), classified, addressed - classified, large_owner_calls, percent(large_owner_calls));
		const auto print_sorted = [&](const char* kind, const auto& counts, bool is_owner) {
			std::vector<std::pair<uint64_t, uint64_t>> sorted(counts.begin(), counts.end());
			std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
				return a.second != b.second ? a.second > b.second : a.first < b.first;
			});
			for (const auto& [base, n]: sorted) {
				const auto owner = owners[0].find(base);
				const auto size = is_owner && owner != owners[0].end() ? owner->second : uint64_t{65536};
				std::fprintf(file, "[WRITE_FAULT_ADDRESSES] %s base=0x%llx size=%llu calls=%llu calls_per_s=%.4f pct_all_faults=%.4f\n",
				    kind, base, size, n, seconds > 0 ? n / seconds : 0, percent(n));
			}
		};
		print_sorted("owner", owner_calls, true);
		print_sorted("region64k", regions, false); // ALL regions, including unclassified/texture-only addresses.
	}
	void ReportFaults(double seconds) {
		if (!GuestCpuProfile::FaultEnabled() && !GuestCpuProfile::GuestCount()) return;
		std::FILE* file = output.empty() ? stderr : _wfopen(output.c_str(), L"a");
		if (!file) file = stderr;
		std::fprintf(file, "[GUEST_CPU] window=%u selection_seconds=%.6f requested=%u selected=%zu registered_overflow=%llu\n",
		    window, selection_seconds, GuestCpuProfile::GuestCount(), guests.size(), GuestCpuProfile::overflow.load());
		for (size_t i = 0; i < candidates.size(); ++i) {
			const auto& c = candidates[i];
			std::fprintf(file, "[GUEST_CPU] rank=%zu tid=%u creation=%llu name=\"%s\" selection_cpu_ms=%.4f selected=%u\n",
			    i + 1, c.row->tid, c.row->creation, c.row->name, c.cpu / 10000.0, unsigned(c.selected));
		}
		if (GuestCpuProfile::FaultEnabled()) {
			uint64_t calls = 0, ticks = 0, dropped = 0, unhandled = 0, rows_skipped = 0;
			const auto ms = [&](uint64_t ticks) { return ticks * 1000.0 / frequency.QuadPart; };
			for (size_t i = 0; i < GuestCpuProfile::rows.size(); ++i) {
				const auto& row = GuestCpuProfile::rows[i];
				if (!row.ready.load(std::memory_order_acquire)) continue;
				GuestCpuProfile::Totals end;
				if (!fault_start_valid[i] || !GuestCpuProfile::Read(row, end)) { ++rows_skipped; continue; }
				const auto delta = GuestCpuProfile::Difference(end, fault_start[i]);
				if (!delta.calls && !delta.unhandled && !delta.dropped) continue;
				calls += delta.calls; ticks += delta.ticks; dropped += delta.dropped; unhandled += delta.unhandled;
				std::fprintf(file, "[WRITE_FAULT_PROFILE] window=%u tid=%u creation=%llu name=\"%s\" calls=%llu calls_per_s=%.3f handler_ms=%.6f handler_ms_per_s=%.6f mean_us=%.6f detail_calls=%llu detail_handler_ms=%.6f unhandled=%llu dropped_boundary=%llu\n",
				    window, row.tid, row.creation, row.name, delta.calls, seconds > 0 ? delta.calls / seconds : 0,
				    ms(delta.ticks), seconds > 0 ? ms(delta.ticks) / seconds : 0, delta.calls ? ms(delta.ticks) * 1000 / delta.calls : 0,
				    delta.detail_calls, ms(delta.detail_ticks), delta.unhandled, delta.dropped);
				for (size_t part = 0; part < GuestCpuProfile::Parts; ++part)
					std::fprintf(file, "[WRITE_FAULT_PROFILE] tid=%u part=%s sampled_calls=%llu sampled_exclusive_ms=%.6f estimated_ms=%.6f\n",
					    row.tid, GuestCpuProfile::PartNames[part], delta.events[part], ms(delta.parts[part]),
					    delta.detail_calls ? ms(delta.parts[part]) * delta.calls / delta.detail_calls : 0);
			}
			// Clock cost only; does not estimate the full cost of hooks/aggregation.
			const auto clock_start = GuestCpuProfile::Now();
			for (unsigned i = 0; i < 10000; ++i) { (void)GuestCpuProfile::Now(); (void)GuestCpuProfile::Now(); }
			const auto clock_ticks = GuestCpuProfile::Now() - clock_start;
			std::fprintf(file, "[WRITE_FAULT_PROFILE] total window=%u seconds=%.6f calls=%llu calls_per_s=%.3f handler_ms=%.6f handler_ms_per_s=%.6f unhandled=%llu dropped_boundary=%llu rows_skipped=%llu detail_period=%u qpc_pair_loop_ns=%.3f time_is_wall_excludes_os_exception_dispatch=1\n",
			    window, seconds, calls, seconds > 0 ? calls / seconds : 0, ms(ticks), seconds > 0 ? ms(ticks) / seconds : 0,
			    unhandled, dropped, rows_skipped, GuestCpuProfile::DetailPeriod(), ms(clock_ticks) * 100.0);
			ReportFaultAddresses(file, seconds, calls);
		}
		std::fflush(file); if (file != stderr) std::fclose(file);
	}
	void Report(const char* reason, double seconds, const CaptureData& data, const std::vector<Sample>& samples,
	    size_t count, DWORD tid, uint64_t pause_ticks, uint64_t max_pause_ticks,
	    const GuestCpuProfile::Row* guest = nullptr, double cpu_ms = 0) {
		const auto* boundary = guest ? nullptr : this->boundary.get();
		// Serialize our DbgHelp users. The sampled thread is running throughout.
		static std::mutex symbol_mutex;
		const std::lock_guard lock(symbol_mutex);
		std::FILE* file = output.empty() ? stderr : _wfopen(output.c_str(), L"a");
		if (!file) {
			std::fprintf(stderr, "[GPU_SAMPLER] cannot open output; using stderr\n");
			file = stderr;
		}
		struct CloseFile { std::FILE* file; ~CloseFile() { if (file != stderr) std::fclose(file); } } close_file {file};
		std::fprintf(file, "[GPU_SAMPLER] begin_target window=%u kind=%s tid=%lu name=\"%s\" creation=%llu window_cpu_ms=%.4f cpu_percent_one_core=%.4f\n",
		    window, guest ? "guest" : "gpu", tid, guest ? guest->name : "GPU command", guest ? guest->creation : 0,
		    cpu_ms, seconds > 0 ? cpu_ms / (seconds * 10) : 0);
		const auto counts = Aggregate(data, samples.data(), count);
		Symbols symbols(exe, data.exe);
		const auto percent = [&](size_t n) { return counts.valid ? n * 100.0 / counts.valid : 0; };
		const auto module_id = [&](const ModuleView* module) { return module ? static_cast<int>(module - data.modules) : -1; };
		const auto module_name = [&](const ModuleView* module) -> const char* {
			return module ? modules.owners[module_id(module)]->name.c_str() : "unknown";
		};
		const auto rva = [](const FunctionId& id) { return id.module ? id.begin - id.module->range.begin : DWORD64{0}; };
		std::map<DWORD64, std::string> labels;
		size_t unresolved_exe_functions = 0;
		for (const auto& [address, total] : counts.functions) {
			bool resolved = false;
			labels[address] = symbols.Resolve(total.id, resolved);
			if (data.exe.Contains(address) && !resolved) ++unresolved_exe_functions;
		}
		if (boundary) {
			const auto anchors = boundary->anchor_count.load(std::memory_order_acquire);
			for (size_t i = 0; i < anchors; ++i) {
				const auto& anchor = boundary->anchors[i];
				const auto id = Identify(data, anchor.pc);
				if (id.pdata && data.exe.Contains(id.begin)) {
					const auto label = labels.find(id.begin);
					if (label != labels.end() && label->second == "unresolved") --unresolved_exe_functions;
					labels[id.begin] = anchor.name;
					std::fprintf(file, "[GPU_SAMPLER] source_anchor rva=0x%llx callsite_rva=0x%llx name=\"%s\"\n",
					    static_cast<unsigned long long>(rva(id)), static_cast<unsigned long long>(anchor.pc - data.exe.begin), anchor.name);
				}
			}
		}
		std::fprintf(file, "[GPU_SAMPLER] schema=3 grouping=module_pdata_entry names_are_labels_only\n");
		std::fprintf(file, "[GPU_SAMPLER] boundary_enabled=%u emission_is_candidate_not_proven_movable=1\n", boundary ? 1u : 0u);
		if (boundary) {
			for (size_t i = 0; i < PhaseCount; ++i)
				std::fprintf(file, "[GPU_SAMPLER] phase name=%s samples=%zu percent=%.4f wait_rip=%zu\n",
				    PhaseNames[i], counts.phases[i], percent(counts.phases[i]), counts.phase_wait_rip[i]);
			for (const auto& [key, n] : counts.phase_modules)
				std::fprintf(file, "[GPU_SAMPLER] phase_module phase=%s module=\"%s\" samples=%zu percent=%.4f\n",
				    PhaseNames[static_cast<size_t>(key.first)], module_name(LookupModule(&data, key.second)), n, percent(n));
			BoundaryFrame totals;
			size_t complete_frames = 0, partial_frames = 0;
			const auto rows = boundary->published.load(std::memory_order_acquire);
			for (size_t i = 0; i < rows; ++i) {
				const auto& row = boundary->frames[i];
				if (row.window != window) continue;
				if (!row.complete) { ++partial_frames; continue; }
				++complete_frames;
				totals.draws += row.draws; totals.dispatches += row.dispatches;
				totals.batches += row.batches; totals.batch_packets += row.batch_packets;
				totals.longest = (std::max)(totals.longest, row.longest);
				totals.ge8 += row.ge8; totals.ge32 += row.ge32; totals.interruptions += row.interruptions;
				std::fprintf(file, "[GPU_SAMPLER] boundary_frame frame=%llu draws=%llu dispatches=%llu batches=%llu packets=%llu mean_packets=%.4f mean_draws=%.4f longest=%llu ge8=%llu ge32=%llu tail=%llu interruptions=%llu",
				    row.frame, row.draws, row.dispatches, row.batches, row.batch_packets,
				    row.batches ? double(row.batch_packets) / row.batches : 0, row.batches ? double(row.draws) / row.batches : 0,
				    row.longest, row.ge8, row.ge32, row.frame_tail, row.interruptions);
				for (size_t k = 0; k < BoundaryCount; ++k) {
					totals.events[k] += row.events[k]; totals.breaks[k] += row.breaks[k];
					std::fprintf(file, " %s_events=%llu %s_breaks=%llu", BoundaryNames[k], row.events[k], BoundaryNames[k], row.breaks[k]);
				}
				std::fputc('\n', file);
			}
			std::fprintf(file, "[GPU_SAMPLER] boundary_totals complete_frames=%zu initial_partial_frames=%zu final_partial_excluded=1 dropped_rows_session=%llu draws=%llu dispatches=%llu batches=%llu mean_packets=%.4f mean_draws=%.4f longest=%llu ge8=%llu ge32=%llu interruptions=%llu\n",
			    complete_frames, partial_frames, boundary->dropped.load(std::memory_order_relaxed), totals.draws, totals.dispatches, totals.batches,
			    totals.batches ? double(totals.batch_packets) / totals.batches : 0, totals.batches ? double(totals.draws) / totals.batches : 0,
			    totals.longest, totals.ge8, totals.ge32, totals.interruptions);
			for (size_t k = 0; k < BoundaryCount; ++k)
				std::fprintf(file, "[GPU_SAMPLER] boundary_kind name=%s events=%llu breaks=%llu events_per_frame=%.4f breaks_per_frame=%.4f\n",
				    BoundaryNames[k], totals.events[k], totals.breaks[k], complete_frames ? double(totals.events[k]) / complete_frames : 0,
				    complete_frames ? double(totals.breaks[k]) / complete_frames : 0);
		}
		std::fprintf(file, "[GPU_SAMPLER] window=%u pid=%lu tid=%lu reason=%s seconds=%.6f attempts=%zu samples=%zu failed=%zu rate_hz=%.1f high_resolution_timer=%u\n",
		    window, GetCurrentProcessId(), tid, reason, seconds, count, counts.valid, counts.failed,
		    seconds > 0 ? counts.valid / seconds : 0, high_resolution ? 1u : 0u);
		std::fprintf(file, "[GPU_SAMPLER] exe=%zu wait_rip=%zu system_other=%zu external=%zu denominator=%zu first_context_error=%lu resume_error=%lu\n",
		    counts.locations[0], counts.locations[1], counts.locations[2], counts.locations[3], counts.valid,
		    counts.first_context_error, counts.resume_error);
		std::fprintf(file, "[GPU_SAMPLER] stack_stop unknown_module=%zu no_unwind=%zu bounds=%zu depth_limit=%zu fault=%zu stack_end=%zu frames=%zu unresolved_exe_functions=%zu pdb_matched=%u\n",
		    counts.stops[0], counts.stops[1], counts.stops[2], counts.stops[3], counts.stops[4], counts.stops[5], counts.frames,
		    unresolved_exe_functions, symbols.HasPdb() ? 1u : 0u);
		std::fprintf(file, "[GPU_SAMPLER] module_snapshot count=%zu error=%lu unknown_self=%zu non_exe_with_exe_caller=%zu non_exe_without_exe_caller=%zu\n",
		    modules.views.size(), modules.error, counts.unknown_self, counts.external_with_exe_caller, counts.external_without_exe_caller);
		std::fprintf(file, "[GPU_SAMPLER] suspend_request_us total=%.1f mean=%.1f max=%.1f exe_base=0x%llx\n",
		    pause_ticks * 1.e6 / frequency.QuadPart, count ? pause_ticks * 1.e6 / frequency.QuadPart / count : 0,
		    max_pause_ticks * 1.e6 / frequency.QuadPart, static_cast<unsigned long long>(data.exe.begin));
		for (size_t i = 0; i < modules.owners.size(); ++i) {
			const auto& module = *modules.owners[i];
			const auto found = counts.modules.find(module.range.begin);
			const auto self = found == counts.modules.end() ? 0 : found->second;
			std::fprintf(file, "[GPU_SAMPLER] module id=%zu name=\"%s\" self=%zu percent=%.4f base=0x%llx size=0x%llx pdata_entries=%zu path=\"%ls\"\n",
			    i, module.name.c_str(), self, percent(self), static_cast<unsigned long long>(module.range.begin),
			    static_cast<unsigned long long>(module.range.end - module.range.begin), module.functions.size(), module.path.c_str());
		}
		std::fprintf(file, "[GPU_SAMPLER] module id=-1 name=\"unknown\" self=%zu percent=%.4f\n", counts.unknown_self, percent(counts.unknown_self));
		std::vector<std::pair<DWORD64, FunctionCounts>> sorted(counts.functions.begin(), counts.functions.end());
		for (const bool inclusive : {false, true}) {
			const auto value = [inclusive](const auto& entry) { return inclusive ? entry.second.inclusive : entry.second.self; };
			std::sort(sorted.begin(), sorted.end(), [&](const auto& a, const auto& b) {
				return value(a) != value(b) ? value(a) > value(b) : a.first < b.first;
			});
			std::fprintf(file, "[GPU_SAMPLER] top200_plus_0.1pct=%s percent_of_all_samples\n", inclusive ? "inclusive" : "self");
			for (size_t rank = 0; rank < sorted.size() && value(sorted[rank]); ++rank) {
				if (rank >= 200 && percent(value(sorted[rank])) < 0.1) break;
				const auto& entry = sorted[rank];
				const auto& id = entry.second.id;
				std::fprintf(file, "[GPU_SAMPLER] %s rank=%zu samples=%zu percent=%.4f module_id=%d module=\"%s\" rva=0x%llx end_rva=0x%llx va=0x%llx identity=%s name=\"%s\"\n",
				    inclusive ? "inclusive" : "self", rank + 1, value(entry), percent(value(entry)), module_id(id.module), module_name(id.module),
				    static_cast<unsigned long long>(rva(id)), static_cast<unsigned long long>(id.module ? id.end - id.module->range.begin : 0),
				    static_cast<unsigned long long>(id.begin), id.pdata ? "pdata" : "pc_no_unwind", labels.at(entry.first).c_str());
			}
		}
		// Keep raw top PCs as a separate diagnostic. They do not fragment the
		// function totals above. Exports are approximate hints with displacement.
		std::vector<std::pair<DWORD64, size_t>> pcs(counts.pcs.begin(), counts.pcs.end());
		std::sort(pcs.begin(), pcs.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
		std::map<int, size_t> pc_ranks;
		for (const auto& [pc, n] : pcs) {
			const auto id = Identify(data, pc);
			const auto mi = module_id(id.module);
			const auto rank = ++pc_ranks[mi];
			if (rank > 200 && percent(n) < 0.1) continue;
			const auto nearest_export = id.module ? modules.owners[mi]->NearExport(pc) : "none";
			std::fprintf(file, "[GPU_SAMPLER] module_pc module_id=%d module=\"%s\" rank=%zu samples=%zu percent=%.4f rva=0x%llx va=0x%llx function_rva=0x%llx identity=%s near_export=\"%s\"\n",
			    mi, module_name(id.module), rank, n, percent(n), static_cast<unsigned long long>(id.module ? pc - id.module->range.begin : 0),
			    static_cast<unsigned long long>(pc), static_cast<unsigned long long>(rva(id)), id.pdata ? "pdata" : "pc_no_unwind", nearest_export.c_str());
		}
		std::vector<std::pair<CallerKey, size_t>> callers(counts.callers.begin(), counts.callers.end());
		std::sort(callers.begin(), callers.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
		for (size_t rank = 0; rank < callers.size(); ++rank) {
			const auto& [edge, n] = callers[rank];
			const auto* top = LookupModule(&data, edge.top_module);
			const auto callee = Identify(data, edge.callee);
			const auto nearest_export = callee.module ? modules.owners[module_id(callee.module)]->NearExport(callee.begin) : "none";
			std::fprintf(file, "[GPU_SAMPLER] external_caller rank=%zu samples=%zu percent=%.4f top_module=\"%s\" callee_module=\"%s\" callee_rva=0x%llx caller_rva=0x%llx callsite_rva=0x%llx caller_name=\"%s\" near_export=\"%s\"\n",
			    rank + 1, n, percent(n), module_name(top), module_name(callee.module), static_cast<unsigned long long>(rva(callee)),
			    static_cast<unsigned long long>(edge.caller - data.exe.begin), static_cast<unsigned long long>(edge.callsite - data.exe.begin),
			    labels.at(edge.caller).c_str(), nearest_export.c_str());
		}
		// No truncation, including unknown callers. Rows partition ALL non-exe
		// self samples; top_module is where the CPU was, callee_module is the
		// external frame adjacent to the exe (e.g. win32u -> driver -> exe).
		for (const auto& [key, n] : counts.module_callers) {
			std::fprintf(file, "[GPU_SAMPLER] module_caller top_module=\"%s\" callee_module=\"%s\" caller_rva=0x%llx caller_name=\"%s\" phase=%s samples=%zu percent=%.4f\n",
			    module_name(LookupModule(&data, key.top_module)), module_name(LookupModule(&data, key.callee_module)),
			    static_cast<unsigned long long>(key.caller ? key.caller - data.exe.begin : 0), key.caller ? labels.at(key.caller).c_str() : "unknown",
			    PhaseNames[static_cast<size_t>(key.phase)], n, percent(n));
		}
		std::fprintf(file, "[GPU_SAMPLER] end_window=%u\n", window);
		std::fprintf(file, "[GPU_SAMPLER] end_target window=%u tid=%lu\n", window, tid);
		std::fflush(file);
	}

	static DWORD WINAPI Run(void* argument) {
		auto* self = static_cast<Impl*>(argument);
		(void)SetThreadDescription(GetCurrentThread(), L"Nova5 GPU sampler");
		try { self->Loop(); }
		catch (...) { std::fprintf(stderr, "[GPU_SAMPLER] worker failed outside suspension; diagnostic stopped\n"); }
		self->EndFaults();
		return 0;
	}
	bool CaptureOne(const CaptureData& target, Sample& sample, uint64_t& pause, uint64_t& maximum) {
		sample = {};
		const auto before = GuestCpuProfile::Now();
		CaptureSuspended(&target, &sample);
		const auto ticks = GuestCpuProfile::Now() - before;
		pause += ticks; maximum = (std::max)(maximum, ticks);
		if (!sample.resume_error) return true;
		// Never allocate, symbolize or log if resumption is uncertain.
		DWORD exit_code = STILL_ACTIVE;
		return GetExitCodeThread(target.target, &exit_code) && exit_code != STILL_ACTIVE;
	}
	void Loop() {
		std::fprintf(stderr, "[GPU_SAMPLER] ready tid=%lu gate=%ls max_seconds=60 max_samples=60000 depth=32 gpu=%u guest_top=%u write_faults=%u\n",
		    tid, gate.c_str(), unsigned(gpu_sampling), GuestCpuProfile::GuestCount(), unsigned(GuestCpuProfile::FaultEnabled()));
		bool armed = true;
		uint32_t random = tid | 1u;
		for (;;) {
			if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) return;
			const bool open = GateOpen();
			if (!open) armed = true;
			if (!open || !armed) { WaitForSingleObject(stop, 100); continue; }
			armed = false;
			count = 0; pause_ticks = 0; max_pause_ticks = 0; ++window;
			// Driver DLLs may load long after ThreadRun starts. Snapshot at the gate,
			// outside suspension/timing; keep all references until after reporting.
			modules.Load();
			data.modules = modules.views.data(); data.module_count = modules.views.size();
			SelectGuests();
			if (!GateOpen() || WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) continue;
			uint64_t gpu_cpu_start = 0, gpu_cpu_end = 0;
			CpuTime(data.target, 0, gpu_cpu_start);
			for (auto& target: guests) CpuTime(target->data.target, target->row->creation, target->cpu_start);
			BeginFaults();
			if (boundary) boundary->requested_window.store(window, std::memory_order_release);
			LARGE_INTEGER start {}, now {};
			QueryPerformanceCounter(&start);
			const char* reason = "limit";
			size_t cycles = 0;
			for (;;) {
				if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) { reason = "shutdown"; break; }
				if (!GateOpen()) { reason = "trigger_closed"; armed = true; break; }
				QueryPerformanceCounter(&now);
				if (cycles++ == kCapacity || (now.QuadPart - start.QuadPart) / double(frequency.QuadPart) >= kWindowSeconds) break;
				if (gpu_sampling && !CaptureOne(data, samples[count++], pause_ticks, max_pause_ticks)) return;
				for (auto& target: guests) {
					if (target->exited) continue;
					auto& sample = target->samples[target->count++];
					if (!CaptureOne(target->data, sample, target->pause_ticks, target->max_pause_ticks)) return;
					if (sample.context_error || sample.resume_error)
						target->exited = WaitForSingleObject(target->data.target, 0) == WAIT_OBJECT_0;
				}
				// Jitter avoids phase locking with periodic command/driver work.
				random = random * 1664525u + 1013904223u;
				LARGE_INTEGER due {};
				due.QuadPart = -static_cast<LONGLONG>(750 + random % 501) * 10;
				if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) { reason = "timer_failed"; break; }
				const HANDLE events[] = {stop, timer};
				const auto result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
				if (result == WAIT_OBJECT_0) { reason = "shutdown"; break; }
				if (result != WAIT_OBJECT_0 + 1) { reason = "timer_failed"; break; }
			}
			QueryPerformanceCounter(&now);
			EndFaults();
			CpuTime(data.target, 0, gpu_cpu_end);
			for (auto& target: guests) {
				target->cpu_end = target->cpu_start;
				CpuTime(target->data.target, target->row->creation, target->cpu_end);
			}
			if (boundary) boundary->requested_window.store(0, std::memory_order_release);
			const auto seconds = (now.QuadPart - start.QuadPart) / double(frequency.QuadPart);
			FinishOwnerSnapshot();
			ReportFaults(seconds);
			if (gpu_sampling) Report(reason, seconds, data, samples, count, tid, pause_ticks, max_pause_ticks,
			    nullptr, gpu_cpu_end >= gpu_cpu_start ? (gpu_cpu_end - gpu_cpu_start) / 10000.0 : 0);
			for (const auto& target: guests)
				Report(target->exited ? "thread_exited" : reason, seconds, target->data, target->samples, target->count, target->row->tid,
				    target->pause_ticks, target->max_pause_ticks, target->row,
				    target->cpu_end >= target->cpu_start ? (target->cpu_end - target->cpu_start) / 10000.0 : 0);
			guests.clear();
			data.modules = nullptr; data.module_count = 0;
			modules.views.clear(); modules.owners.clear();
			if (std::strcmp(reason, "resume_failed") == 0 || std::strcmp(reason, "timer_failed") == 0) return;
		}
	}
};

Session::Session(bool another_symbol_engine_active) {
	if (!AnnounceCurrentThread()) return;
	if (another_symbol_engine_active) {
		std::fprintf(stderr, "[GPU_SAMPLER] disabled: turn off Tracy before using the sampler (shared DbgHelp)\n");
		return;
	}
	const SessionLock lock;
	if (g_active_stop) {
		std::fprintf(stderr, "[GPU_SAMPLER] disabled: another sampler session is active\n");
		return;
	}
	try {
		auto impl = std::make_unique<Impl>();
		if (impl->Start()) {
			boundary_state = impl->boundary.get();
			g_active_stop = impl->stop; g_active_worker = impl->worker;
			m_impl = impl.release();
		} else std::fprintf(stderr, "[GPU_SAMPLER] initialization failed error=%lu\n", GetLastError());
	} catch (...) {
		std::fprintf(stderr, "[GPU_SAMPLER] initialization failed (allocation)\n");
	}
}
Session::~Session() {
	if (!m_impl) return;
	boundary_state = nullptr;
	const SessionLock lock;
	g_active_stop = nullptr; g_active_worker = nullptr;
	delete m_impl;
}
void StopAndFlush() {
	const SessionLock lock;
	if (g_active_stop) {
		SetEvent(g_active_stop);
		WaitForSingleObject(g_active_worker, INFINITE);
	}
}
} // namespace Common::GpuThreadSampler
#else
namespace Common::GpuThreadSampler {
void NameCaller(const char*) {}
Session::Session(bool) { (void)AnnounceCurrentThread(); }
Session::~Session() = default;
void StopAndFlush() {}
} // namespace Common::GpuThreadSampler
#endif
