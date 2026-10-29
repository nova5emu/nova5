#pragma once

#include "common/submitProfile.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <fmt/format.h>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

// Read-only evidence for mandatory descriptor/SRT loads. This is NOT a value
// cache or an alternative evaluator: no guest pointer is ever dereferenced.
namespace Libs::Graphics::ShaderRecompiler::IR::SrtDescriptorProbe {
inline constexpr std::array<uint64_t, 2> Targets {0xfbb1e69b2c2c0abdull, 0x09c7784d7b18346cull};
inline constexpr size_t MaxNodes = 1024, MaxEntries = 256, MaxArgs = 32;
inline constexpr uint32_t MaxSamples = 4;

inline size_t Target(uint64_t hash) {
	for (size_t i = 0; i < Targets.size(); ++i) if (Targets[i] == hash) return i;
	return Targets.size();
}

struct Budget {
	struct Entry { uint64_t generation = 0; uint32_t samples = 0; };
	std::array<Entry, Targets.size()> entries {};

	uint32_t Claim(uint64_t hash, uint64_t generation, bool enabled, bool recording) {
		const auto index = Target(hash);
		if (!enabled || !recording || index == Targets.size()) return 0;
		auto& entry = entries[index];
		if (entry.samples == MaxSamples || (entry.samples && entry.generation == generation)) return 0;
		entry.generation = generation;
		return ++entry.samples;
	}
};

inline bool Enabled() {
	static const bool enabled = [] {
		const auto* value = std::getenv("KYTY_SRT_DESCRIPTOR_PROBE");
		return value && std::string_view(value) == "1";
	}();
	return enabled;
}

// Collections always carry their full count, including when output is capped.
// References to omitted nodes are explicit; never mistake a partial DAG for a
// proof that a load is unused. Do not Resolve/Evaluate values while reporting.
inline std::string Report(const ResourcePlan& plan, const ResourceSnapshot& snapshot,
                          uint64_t generation, uint32_t sample) {
	std::string out = fmt::format("{{\"hash\":\"0x{:016x}\",\"stage\":{},\"generation\":{},\"sample\":{},"
	    "\"diagnostic_only\":true,\"capture_specialization_reads\":{},\"node_limit\":{},\"entry_limit\":{},\"arg_limit\":{}",
	    plan.shader_hash, static_cast<uint32_t>(plan.stage), generation, sample,
	    plan.capture_specialization_reads, MaxNodes, MaxEntries, MaxArgs);
	std::unordered_map<const Inst*, uint32_t> ids;
	for (const auto& inst: plan.value_storage) {
		if (ids.size() == MaxNodes) break;
		ids.emplace(&inst, static_cast<uint32_t>(ids.size()));
	}
	bool truncated = false;
	const auto reference = [&](const Inst* inst) {
		const auto found = ids.find(inst);
		if (found != ids.end()) out += fmt::format("{{\"node\":{}}}", found->second);
		else { out += "{\"node\":null,\"unlisted\":true}"; truncated = true; }
	};
	const auto value = [&](Value v) {
		if (const auto* inst = v.TryInstruction()) { reference(inst); return; }
		uint64_t bits = 0;
		bool known = true;
		switch (v.GetType()) {
		case Type::Void: break;
		case Type::ScalarReg: bits = RegIndex(v.ScalarRegister()); break;
		case Type::VectorReg: bits = RegIndex(v.VectorRegister()); break;
		case Type::U1: bits = v.U1(); break;
		case Type::U8: bits = v.U8(); break;
		case Type::U16: bits = v.U16(); break;
		case Type::U32: bits = v.U32(); break;
		case Type::U64: bits = v.U64(); break;
		case Type::F16: bits = v.F16Bits(); break;
		case Type::F32: bits = std::bit_cast<uint32_t>(v.F32Value()); break;
		default: known = false; truncated = true; break;
		}
		out += fmt::format("{{\"type\":{},\"bits\":\"0x{:x}\",\"known\":{}}}",
		    static_cast<uint32_t>(v.GetType()), bits, known);
	};
	const auto collection = [&](std::string_view name, size_t count, size_t limit, const auto& emit) {
		out += fmt::format(",\"{}\":{{\"total\":{},\"items\":[", name, count);
		const auto size = std::min(count, limit);
		truncated |= size != count;
		for (size_t i = 0; i < size; ++i) { if (i) out += ','; emit(i); }
		out += "]}";
	};
	const auto numbers = [&](std::string_view name, const auto& values) {
		collection(name, values.size(), MaxEntries, [&](size_t i) { out += fmt::format("{}", values[i]); });
	};
	auto node = plan.value_storage.begin();
	collection("nodes", plan.value_storage.size(), MaxNodes, [&](size_t i) {
		const auto& inst = *node++;
		out += fmt::format("{{\"id\":{},\"op\":\"{}\",\"flags\":\"0x{:x}\"",
		    i, ValueOpcodeName(inst.GetOpcode()), inst.Flags<uint64_t>());
		collection("args", inst.NumArgs(), MaxArgs, [&](size_t a) { value(inst.Arg(a)); });
		if (const auto found = plan.control_phi_predecessors.find(&inst); found != plan.control_phi_predecessors.end())
			numbers("phi_predecessors", found->second);
		out += '}';
	});
	collection("read_sites", plan.walker_read_sites.size(), MaxEntries, [&](size_t i) {
		const auto& site = plan.walker_read_sites[i];
		out += fmt::format("{{\"jit_offset\":{},\"uses\":{},\"flat_slot\":{},\"descriptor\":{},\"control_slot\":{},\"load\":",
		    site.offset, site.uses, site.flat_slot, site.descriptor, site.control_slot);
		reference(site.inst);
		const auto flags = site.inst->Flags<MemoryFlags>();
		out += fmt::format(",\"memory\":{},\"pc\":{}", flags.index, flags.pc);
		if (flags.index < plan.memory_info.size()) {
			const auto& m = plan.memory_info[flags.index];
			out += fmt::format(",\"memory_kind\":{},\"resource\":{},\"immediate\":{},\"planning_only\":{}",
			    static_cast<uint32_t>(m.kind), m.resource, m.offset, m.planning_only);
		}
		out += '}';
	});
	collection("srt_roots", plan.srt_reads.size(), MaxEntries, [&](size_t i) {
		out += fmt::format("{{\"flat_offset\":{},\"value\":", plan.srt_reads[i].flat_offset);
		value(plan.srt_reads[i].value); out += '}';
	});
	collection("descriptor_roots", plan.descriptor_sources.size(), MaxEntries, [&](size_t i) {
		const auto& source = plan.descriptor_sources[i];
		out += fmt::format("{{\"source\":{},\"indirect\":{}", i, source.indirect_descriptor.has_value());
		collection("dwords", source.dword_count, source.dwords.size(), [&](size_t w) { value(source.dwords[w]); });
		if (source.indirect_descriptor) {
			// This probe describes direct roots completely. Indirect table metadata
			// needs a separate investigation; do not imply it has been dumped.
			out += ",\"indirect_details_omitted\":true"; truncated = true;
		}
		out += '}';
	});
	collection("blocks", plan.control_flow.size(), MaxEntries, [&](size_t i) {
		const auto& block = plan.control_flow[i];
		out += "{\"condition\":"; value(block.condition);
		numbers("successors", block.successors); numbers("sources", block.sources); numbers("srt_reads", block.srt_reads);
		out += '}';
	});
	const auto resources = [&](std::string_view name, const auto& roots, const auto& descriptors) {
		out += fmt::format(",\"{}\":{{", name);
		out += "\"mapping_kind\":\"direct_roots_only\"";
		collection("roots", roots.size(), MaxEntries, [&](size_t i) {
			const auto& root = roots[i];
			out += fmt::format("{{\"resource\":{},\"source\":{}", i, root.source);
			if constexpr (requires { root.snapshot_index; })
				out += fmt::format(",\"sampler_snapshot_index\":{}", root.snapshot_index);
			out += '}';
		});
		collection("values", descriptors.size(), MaxEntries, [&](size_t i) {
			out += fmt::format("{{\"index\":{}", i);
			const auto& descriptor = descriptors[i];
			collection("dwords", descriptor.dword_count, descriptor.dwords.size(),
			    [&](size_t w) { out += fmt::format("{}", descriptor.dwords[w]); });
			out += '}';
		});
		out += '}';
	};
	resources("buffers", plan.info.buffers, snapshot.buffers);
	resources("images", plan.info.images, snapshot.images);
	resources("samplers", plan.info.samplers, snapshot.samplers);
	numbers("flat_values", snapshot.flattened_srt);
	numbers("user_data", snapshot.user_data);
	collection("captured_ranges", snapshot.specialization_reads.size(), MaxEntries, [&](size_t i) {
		const auto [address, bytes] = snapshot.specialization_reads[i];
		out += fmt::format("{{\"address\":\"0x{:x}\",\"bytes\":{}}}", address, bytes);
	});
	out += fmt::format(",\"truncated\":{}}}", truncated);
	return out;
}

// Distinct values per profile window, compared on every dword (no hash collisions).
// Once full, further new values are counted as unclassified, never as distinct.
struct ValueTable {
	static constexpr size_t Capacity = 64;
	std::vector<std::pair<std::vector<uint32_t>, uint64_t>> values;
	std::vector<uint32_t> last;
	uint64_t observations = 0, repeats = 0, unclassified = 0;

	void Add(std::vector<uint32_t> value) {
		if (observations++ && value == last) ++repeats;
		last = value;
		for (auto& [seen, count]: values) if (seen == value) { ++count; return; }
		if (values.size() == Capacity) { ++unclassified; return; }
		values.emplace_back(std::move(value), 1);
	}
	std::string Json() const {
		std::string out = fmt::format("{{\"observations\":{},\"repeats_last\":{},\"changes\":{},\"distinct\":{},"
		    "\"unclassified\":{},\"values\":[", observations, repeats, observations ? observations - 1 - repeats : 0,
		    values.size(), unclassified);
		for (size_t i = 0; i < values.size(); ++i) {
			out += fmt::format("{}{{\"calls\":{},\"dwords\":\"", i ? "," : "", values[i].second);
			for (size_t w = 0; w < values[i].first.size(); ++w) out += fmt::format("{}{:08x}", w ? " " : "", values[i].first[w]);
			out += "\"}";
		}
		return out + "]}";
	}
};

// How many distinct image/sampler descriptors (and remaining flattened SRT values) a
// target shader materializes per profile window. Diagnostic only; reads no guest memory.
struct Variety {
	uint64_t generation = 0, materializations = 0;
	uint32_t stage = 0;
	ValueTable images, samplers, pairs, flat;

	void Flush(uint64_t hash) {
		if (!materializations) return;
		std::fprintf(stderr, "[SRT_DESCRIPTOR_VARIETY] {\"hash\":\"0x%016llx\",\"stage\":%u,\"generation\":%llu,"
		    "\"materializations\":%llu,\"images\":%s,\"samplers\":%s,\"image_sampler_pairs\":%s,\"flat_srt\":%s}\n",
		    (unsigned long long)hash, stage, (unsigned long long)generation, (unsigned long long)materializations,
		    images.Json().c_str(), samplers.Json().c_str(), pairs.Json().c_str(), flat.Json().c_str());
		*this = {};
	}
};

template <typename Descriptors>
std::vector<uint32_t> DescriptorWords(const Descriptors& descriptors) {
	std::vector<uint32_t> words;
	for (const auto& descriptor: descriptors)
		words.insert(words.end(), descriptor.dwords.begin(), descriptor.dwords.begin() + descriptor.dword_count);
	return words;
}

struct Shared {
	std::mutex mutex;
	Budget budget;
	std::array<Variety, Targets.size()> variety;
};
inline Shared& State() {
	static Shared shared;
	return shared;
}

// Emit and reset the open windows (called when submit-profile closes a window).
inline void FlushVariety() {
	if (!Enabled()) return;
	auto& shared = State();
	std::lock_guard lock(shared.mutex);
	for (size_t i = 0; i < Targets.size(); ++i) shared.variety[i].Flush(Targets[i]);
}

inline void Observe(const ResourcePlan& plan, const ResourceSnapshot& snapshot) {
	if (!Enabled() || Target(plan.shader_hash) == Targets.size() || !Common::SubmitProfile::Recording()) return;
	auto& shared = State();
	Common::SubmitProfile::Detail::window_close_hook.store(&FlushVariety, std::memory_order_release);
	const auto generation = Common::SubmitProfile::Detail::generation.load(std::memory_order_relaxed);
	uint32_t sample;
	{
		std::lock_guard lock(shared.mutex);
		// Counted before the sample budget: the budget limits the IR dump, not this count.
		auto& stats = shared.variety[Target(plan.shader_hash)];
		if (stats.materializations && stats.generation != generation) stats.Flush(plan.shader_hash);
		stats.generation = generation;
		stats.stage = static_cast<uint32_t>(plan.stage);
		++stats.materializations;
		auto images = DescriptorWords(snapshot.images);
		auto samplers = DescriptorWords(snapshot.samplers);
		auto pair = images;
		pair.insert(pair.end(), samplers.begin(), samplers.end());
		stats.images.Add(std::move(images));
		stats.samplers.Add(std::move(samplers));
		stats.pairs.Add(std::move(pair));
		stats.flat.Add({snapshot.flattened_srt.begin(), snapshot.flattened_srt.end()});
		sample = shared.budget.Claim(plan.shader_hash, generation, true, true);
	}
	if (!sample) return;
	const auto report = Report(plan, snapshot, generation, sample);
	// Assemble a whole record before output instead of writing individual fields.
	std::fprintf(stderr, "[SRT_DESCRIPTOR_PROBE] %s\n", report.c_str());
}
} // namespace Libs::Graphics::ShaderRecompiler::IR::SrtDescriptorProbe
