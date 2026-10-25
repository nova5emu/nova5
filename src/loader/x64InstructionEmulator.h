#ifndef KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_
#define KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_

#include <cstddef>
#include <cstdint>
#include <span>

namespace Common::HostException { struct ExceptionInfo; }

namespace Loader::X64InstructionEmulator {

[[nodiscard]] bool TryEmulate(void* native_context);
// Completes a faulting load into EAX as a zero read and resumes after it.
void SkipLoad(void* native_context, size_t length);

struct ReadAccess { uint64_t address = 0, size = 0; };
// Diagnostic only. Decode ordinary, unmasked host loads from readable code bytes.
// Unknown/string/masked instructions return size=0, never a guessed access width.
[[nodiscard]] ReadAccess DescribeReadAccess(std::span<const uint8_t> code,
                                           const Common::HostException::ExceptionInfo& context);

using ReadMemory = bool (*)(uint64_t address, void* destination, uint64_t size);
// Commit only after a supported single-page load and a successful safe read.
// On rejection the native context is unchanged; the caller may use normal fault recovery.
[[nodiscard]] bool TryEmulateRead(std::span<const uint8_t> code,
                                const Common::HostException::ExceptionInfo& context,
                                ReadMemory read);

} // namespace Loader::X64InstructionEmulator

#endif /* KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_ */
