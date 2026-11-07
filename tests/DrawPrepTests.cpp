#include "graphics/host_gpu/renderer/drawPrep/readSet.h"
#include "graphics/host_gpu/renderer/drawPrep/window.h"
#include "graphics/host_gpu/renderer/drawPrep/packetClass.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace Libs::Graphics;
using namespace Libs::Graphics::DrawPrep;

static void Check(bool ok, const char* what) {
	if (!ok) { std::fprintf(stderr, "DrawPrepTests: %s\n", what); std::abort(); }
}

static void ReadCertificates() {
	std::array<uint32_t, 8> memory {1, 2, 3, 4, 5, 6, 7, 8};
	constexpr uint64_t base = 0x10000;
	ReadSet reads;
	std::vector<uint8_t> scratch;
	bool clean = true;
	auto read = [&](uint64_t address, void* destination, uint64_t size) {
		if (!clean || address < base || address - base > sizeof(memory) || size > sizeof(memory) - (address - base)) return false;
		std::memcpy(destination, reinterpret_cast<const uint8_t*>(memory.data()) + address - base, size);
		return true;
	};
	Check(reads.Validate(read, scratch) != ValidateResult::Ok, "unfinished certificate accepted");
	Check(reads.Record(base + 4, memory.data() + 1, 12) && reads.Record(base, memory.data(), 8) && reads.Finish(), "merge");
	Check(reads.Ranges().size() == 1 && reads.Ranges()[0].begin == base && reads.Ranges()[0].end == base + 16, "range union");
	Check(reads.Validate(read, scratch) == ValidateResult::Ok, "unchanged bytes");
	memory[2] ^= 8;
	Check(reads.Validate(read, scratch) == ValidateResult::Changed, "changed byte not rejected");
	memory[2] ^= 8;
	Check(reads.Validate(read, scratch) == ValidateResult::Ok, "ABA values should certify");
	clean = false;
	Check(reads.Validate(read, scratch) == ValidateResult::Unclean, "GPU authority not rejected");
	reads.Reset();
	Check(reads.Record(base, memory.data(), 8), "first overlap");
	memory[1] ^= 1;
	Check(reads.Record(base + 4, memory.data() + 1, 4) && !reads.Finish() && reads.Failure() == ReadFailure::Inconsistent,
	      "inconsistent repeated read accepted");
	reads.Reset();
	Check(reads.Record(0x1ffc, memory.data(), 4) && reads.Record(0x2000, memory.data() + 1, 4) && reads.Finish() &&
	      reads.Ranges().size() == 2, "touching mappings merged across a page boundary");
	Check(!reads.Record(UINT64_MAX - 1, memory.data(), 4) && !reads.Finish(), "address wrap");
	reads.Reset();
	std::vector<uint8_t> cap(ReadSet::MaxBytes);
	Check(reads.Record(base, cap.data(), cap.size()) && !reads.Record(base, cap.data(), 1), "byte cap");
	reads.Reset();
	for (uint32_t i = 0; i < ReadSet::MaxReads; ++i) Check(reads.Record(base, memory.data(), 1), "read limit early");
	Check(!reads.Record(base, memory.data(), 1), "read cap");
	reads.Reset();
	Recorder recorder {&reads, +[](uint64_t, void*, uint64_t) { return false; }};
	{
		RecordScope scope(recorder);
		uint32_t word = 17;
		Check(!ReadMemory(1, &word, 4) && word == 17 && SpeculativeFailed(), "failed probe dereferenced guest memory");
		ReadSet nested;
		Recorder other {&nested, recorder.probe};
		{ RecordScope inner(other); Check(ActiveRecorder() == &other, "nested recorder"); }
		Check(ActiveRecorder() == &recorder, "recorder restoration");
	}
	Check(!Speculative(), "TLS recorder leaked");
}

static void ClassifyAllOpcodes() {
	constexpr std::array safe {Pm4::IT_SET_CONTEXT_REG, Pm4::IT_SET_SH_REG, Pm4::IT_SET_UCONFIG_REG,
	    Pm4::IT_SET_UCONFIG_REG_INDEX, Pm4::IT_INDEX_TYPE, Pm4::IT_INDEX_BASE, Pm4::IT_INDEX_BUFFER_SIZE,
	    Pm4::IT_NUM_INSTANCES, Pm4::IT_DRAW_INDEX_2, Pm4::IT_DRAW_INDEX_OFFSET_2, Pm4::IT_DRAW_INDEX_AUTO};
	for (uint32_t opcode = 0; opcode < 256; ++opcode) {
		bool expected = false;
		for (auto allowed : safe) expected |= opcode == allowed;
		const uint32_t header = 0xc0010000u | (opcode << 8u);
		Check(WindowSafePacket(header) == expected, "opcode whitelist");
		Check(!WindowSafePacket(header | 1u), "predication is a fence");
	}
	Check(!WindowSafePacket(0x80000000), "type-2 packet");
}

// Exercise the production ring for many laps, both CP self-claims and competing
// workers. The payload's output is only consumed after acquire; retirement order
// and exactly-once preparation are checked independently of who wins a claim.
static void OrderedWindow(uint32_t worker_count, uint32_t capacity) {
	struct Payload { uint64_t input = 0, output = 0; uint32_t writes = 0; };
	Window<Payload> window(capacity);
	constexpr uint64_t total = 20000;
	std::atomic<bool> stop {false};
	std::vector<std::thread> workers;
	auto prepare = [](Payload& payload) {
		++payload.writes;
		if (payload.input % 113 == 0) std::this_thread::yield();
		payload.output = payload.input * 17 + 3;
	};
	for (uint32_t i = 0; i < worker_count; ++i) workers.emplace_back([&] {
		while (!stop.load()) {
			uint64_t seq = 0;
			if (auto* payload = window.TryClaim(seq)) { prepare(*payload); window.Complete(seq); }
			else std::this_thread::yield();
		}
	});
	uint64_t retired = 0;
	auto commit = [&] {
		Check(!window.Empty() && window.Head() == retired, "retirement order");
		if (window.TryClaimHead()) prepare(window.HeadPayload());
		else while (!window.HeadDone()) std::this_thread::yield();
		const auto& payload = window.HeadPayload();
		Check(payload.input == retired && payload.output == retired * 17 + 3 && payload.writes == 1,
		      "lost, duplicate or stale preparation");
		window.Retire(); ++retired;
	};
	for (uint64_t i = 0; i < total; ++i) {
		if (window.Full()) commit();
		window.Reserve() = {i, 0, 0};
		window.Publish();
		// Explicit fences as well as capacity pressure.
		if (i % 41 == 0) while (!window.Empty()) commit();
	}
	while (!window.Empty()) commit();
	stop.store(true);
	for (auto& worker : workers) worker.join();
	Check(retired == total && window.Empty(), "final drain");
}

int main() {
	ReadCertificates();
	ClassifyAllOpcodes();
	for (auto capacity : {2u, 4u, 32u}) for (auto workers : {0u, 1u, 4u}) OrderedWindow(workers, capacity);
	std::array<std::thread, 8> readers;
	for (auto& reader : readers) reader = std::thread([] { ReadCertificates(); });
	for (auto& reader : readers) reader.join();
	std::puts("DrawPrepTests: passed");
}
