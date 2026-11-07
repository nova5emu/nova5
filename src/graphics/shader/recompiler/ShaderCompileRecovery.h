#pragma once

#include "common/assert.h"

namespace Libs::Graphics::ShaderRecompiler {

// Only CPU translation/emission belongs inside this boundary. Vulkan creation,
// resource binding and submission errors must keep their normal failure path.
struct ShaderCompileRejected {
	std::string message;
};

template <typename Compile>
auto RecoverShaderCompile(bool enabled, Compile&& compile) -> decltype(compile()) {
	if (!enabled) return compile();
	try {
		Common::RecoverableExitScope recover;
		return compile();
	} catch (const Common::RecoverableExit& failure) {
		throw ShaderCompileRejected {failure.message};
	}
}
} // namespace Libs::Graphics::ShaderRecompiler
