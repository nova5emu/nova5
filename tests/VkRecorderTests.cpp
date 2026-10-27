#include "common/commandRecorder.h"
#include "common/submitProfile.h"
#include "graphics/host_gpu/renderer/vkRecorderPackets.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <semaphore>
#include <thread>
#include <vector>

namespace {
using Common::CommandRecording::Drain;
using Common::CommandRecording::Queue;
namespace Packets = Libs::Graphics::VkRecording;

void Check(bool value, const char* text) {
	if (!value) { std::fprintf(stderr, "VkRecorderTests: %s\n", text); std::abort(); }
}
struct Record { uint16_t opcode; std::vector<std::byte> bytes; };
struct Sink {
	std::thread::id owner;
	unsigned starts = 0, stops = 0;
	std::binary_semaphore entered {0}, release {0};
	std::vector<Record> records;
	static constexpr uint16_t Pause = UINT16_MAX;
	static void Start(void* data) noexcept {
		auto& self = *static_cast<Sink*>(data);
		self.owner = std::this_thread::get_id(); ++self.starts;
	}
	static void Stop(void* data) noexcept {
		auto& self = *static_cast<Sink*>(data);
		Check(self.owner == std::this_thread::get_id(), "shutdown changed consumer ownership"); ++self.stops;
	}
	static void Consume(void* data, uint16_t opcode, std::span<const std::byte> bytes) noexcept {
		auto& self = *static_cast<Sink*>(data);
		Check(self.owner == std::this_thread::get_id(), "packet ran outside its owner thread");
		if (opcode == Pause) { self.entered.release(); self.release.acquire(); return; }
		self.records.push_back({opcode, {bytes.begin(), bytes.end()}});
	}
};

void TestQueue() {
	Sink sink;
	Queue queue(Sink::Consume, &sink, {64, 2, true}, Sink::Start, Sink::Stop);
	uint64_t value = 77;
	Check(!queue.AppendValue(1, value) && queue.Snapshot().storage_bytes == 0 && sink.starts == 0,
	      "disabled queue allocated, started or accepted work");
	Check(queue.SetEnabled(true), "enable failed");
	Check(sink.owner != std::this_thread::get_id(), "consumer runs on producer");
	Check(queue.Append(Sink::Pause, {}), "pause append failed"); queue.Publish(); sink.entered.acquire();
	Check(queue.AppendValue(1, value), "append failed"); value = 99;
	// The worker cannot inspect the producer's original storage: it was changed
	// while consumption was deterministically blocked on a semaphore.
	sink.release.release(); queue.DrainUntilIdle(Drain::Readback);
	Packets::Reader first(sink.records[0].bytes); uint64_t read = 0;
	Check(first.Read(read) && read == 77 && first.Empty(), "packet borrowed producer data");

	// Many more packets than the fixed pool can hold: wrap/reuse must preserve
	// order without allocating additional blocks or dropping under backpressure.
	for (uint64_t i = 0; i < 2048; ++i) {
		Check(queue.AppendValue(2, i), "bounded pool dropped work");
		if (i % 512 == 0) queue.Frame();
	}
	const std::array<std::byte, 64> oversized {};
	Check(!queue.Append(3, oversized), "oversized packet accepted");
	const std::array<std::span<const std::byte>, 2> too_many {oversized, oversized};
	Check(!queue.AppendParts(3, too_many), "oversized multipart accepted");
	Check(queue.SetEnabled(false), "gate off failed");
	Check(sink.records.size() == 2049, "gate off returned before last packet");
	for (uint64_t i = 0; i < 2048; ++i) {
		Packets::Reader item(sink.records[i + 1].bytes);
		Check(item.Read(read) && read == i && item.Empty(), "block recycle reordered/corrupted packets");
	}
	Check(!queue.AppendValue(4, value), "gate off accepted a packet");
	Check(queue.SetEnabled(true) && sink.starts == 1, "second B restarted owner/pool");
	for (const auto reason: {Drain::Native, Drain::Upload, Drain::Submit, Drain::Finish, Drain::WaitTick}) {
		Check(queue.AppendValue(4, value), "second B append failed");
		queue.DrainUntilIdle(reason);
		Check(queue.Snapshot().drains[static_cast<size_t>(reason)] == 1, "drain cause lost");
	}
	Check(queue.AppendValue(5, value), "shutdown append failed"); queue.Stop();
	const auto stats = queue.Snapshot();
	uint64_t attributed_wait = 0;
	for (const auto ns: stats.wait_by_cause_ns) attributed_wait += ns;
	Check(attributed_wait == stats.wait_ns, "recording wait attribution lost or double-counted time");
	Check(stats.completed_batches == stats.batches && stats.frames == 4 && stats.storage_bytes == 128 &&
	      stats.oversized == 2 && stats.drains[static_cast<size_t>(Drain::Gate)] == 1 &&
	      stats.drains[static_cast<size_t>(Drain::Shutdown)] == 1, "queue accounting or shutdown incomplete");
	Check(sink.stops == 1 && !queue.SetEnabled(true), "stop is not final");
	queue.Report(stdout);
}

void TestSnapshots() {
	Sink sink;
	Queue queue(Sink::Consume, &sink, {65536, 4, false}, Sink::Start, Sink::Stop);
	Check(queue.SetEnabled(true), "snapshot enable failed");
	Check(queue.Append(Sink::Pause, {}), "snapshot pause failed"); queue.Publish(); sink.entered.acquire();
	Packets::Encoder encoder(queue);

	std::array<uint32_t, 4> constants {10, 20, 30, 40};
	Check(encoder.PushConstants({}, vk::ShaderStageFlagBits::eVertex, 16, std::as_bytes(std::span(constants))), "push constants rejected");
	constants.fill(0);
	Libs::Graphics::RenderState rendering {}; rendering.width = 128; rendering.height = 64;
	rendering.num_color_attachments = 1; rendering.color_attachments[0].is_clear = true;
	rendering.color_attachments[0].clear_value[0] = 0xabcdef;
	Check(encoder.BeginRendering(rendering), "rendering rejected"); rendering = {};
	Libs::Graphics::DynamicState dynamic {}; dynamic.num_viewports = 1; dynamic.viewports[0].x = 42;
	dynamic.dirty_state.viewports = true;
	Check(encoder.Dynamic(dynamic), "dynamic state rejected"); dynamic = {};

	vk::DescriptorBufferInfo buffer {}; buffer.offset = 48; buffer.range = 96;
	vk::DescriptorImageInfo image {}; image.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
	std::array<vk::WriteDescriptorSet, 2> writes {};
	writes[0].dstBinding = 3; writes[0].descriptorType = vk::DescriptorType::eUniformBuffer;
	writes[0].descriptorCount = 1; writes[0].pBufferInfo = &buffer;
	writes[1].dstBinding = 4; writes[1].descriptorType = vk::DescriptorType::eSampledImage;
	writes[1].descriptorCount = 1; writes[1].pImageInfo = &image;
	Check(encoder.Descriptors(true, vk::PipelineBindPoint::eGraphics, {}, 2, writes), "push descriptors rejected");
	Check(encoder.Descriptors(false, vk::PipelineBindPoint::eGraphics, {}, 0, writes), "descriptor updates rejected");
	buffer.offset = 0; buffer.range = 1; image.imageLayout = vk::ImageLayout::eGeneral;
	writes[0].pNext = &writes[1];
	Check(!encoder.Descriptors(true, vk::PipelineBindPoint::eGraphics, {}, 0, writes), "unsupported descriptor chain accepted");
	writes = {};

	vk::BufferMemoryBarrier2 barrier {}; barrier.offset = 32; barrier.size = 128;
	barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
	vk::DependencyInfo dependency {}; dependency.bufferMemoryBarrierCount = 1; dependency.pBufferMemoryBarriers = &barrier;
	Check(encoder.Barriers(dependency), "barrier rejected");
	barrier.pNext = &dependency;
	Check(!encoder.Barriers(dependency), "unsupported barrier chain accepted"); barrier = {}; dependency = {};
	Packets::Draw draw {7, 2, 3, 4, -5};
	Check(encoder.Value(Packets::Op::DrawIndexed, draw), "draw rejected"); draw = {};
	Check(encoder.EndRendering(), "end rendering rejected");
	sink.release.release(); queue.DrainUntilIdle(Drain::Submit);
	Check(sink.records.size() == 8, "rejected encoding partially appended a packet");

	Packets::Reader pc(sink.records[0].bytes); Packets::Constants pc_header {};
	Check(pc.Read(pc_header) && pc_header.offset == 16 && pc_header.bytes == 16 &&
	      pc.ReadArray(std::span(constants)) && constants == std::array<uint32_t, 4>{10,20,30,40} && pc.Empty(), "push payload lifetime");
	Packets::Reader render(sink.records[1].bytes);
	Check(render.Read(rendering) && rendering.width == 128 && rendering.color_attachments[0].clear_value[0] == 0xabcdef && render.Empty(), "rendering payload lifetime");
	Packets::Reader state(sink.records[2].bytes);
	Check(state.Read(dynamic) && dynamic.dirty_state.viewports && dynamic.viewports[0].x == 42 && state.Empty(), "dynamic payload lifetime");
	for (size_t i: {size_t{3}, size_t{4}}) {
		Packets::Reader desc(sink.records[i].bytes); Packets::Writes header {}; Packets::Write write {};
		Check(desc.Read(header) && header.count == 2 && desc.Read(write) && write.binding == 3 &&
		      desc.Read(buffer) && buffer.offset == 48 && buffer.range == 96 && desc.Read(write) && write.binding == 4 &&
		      desc.Read(image) && image.imageLayout == vk::ImageLayout::eShaderReadOnlyOptimal && desc.Empty(), "nested descriptor arrays borrowed producer memory");
	}
	Packets::Reader barriers(sink.records[5].bytes); Packets::Barrier barrier_header {};
	Check(barriers.Read(barrier_header) && barrier_header.buffers == 1 && barriers.Read(barrier) &&
	      barrier.offset == 32 && barrier.size == 128 && !barrier.pNext && barriers.Empty(), "barrier payload lifetime");
	Packets::Reader drawing(sink.records[6].bytes);
	Check(drawing.Read(draw) && draw.count == 7 && draw.vertex_offset == -5 && drawing.Empty(), "draw payload lifetime");
	Check(sink.records[7].bytes.empty(), "end rendering carries stray memory");
	Check(!queue.Snapshot().timing && queue.Snapshot().wait_ns == 0, "disabled timing still measured");
	queue.Stop();
}

void TestBindingPacket() {
	Sink sink;
	Queue queue(Sink::Consume, &sink, {65536, 2, false}, Sink::Start, Sink::Stop);
	Check(queue.SetEnabled(true), "binding packet enable failed");
	Check(queue.Append(Sink::Pause, {}), "binding pause failed"); queue.Publish(); sink.entered.acquire();
	Packets::Encoder encoder(queue);
	Packets::BindingState state {};
	state.push_descriptors = true; state.heap_index = 7;
	state.point = vk::PipelineBindPoint::eCompute; state.stages = vk::ShaderStageFlagBits::eCompute;
	state.constant_bytes = 8;
	std::array<uint32_t, 2> constants {11, 22};
	vk::DescriptorBufferInfo info {}; info.offset = 32; info.range = 128;
	vk::WriteDescriptorSet write {}; write.dstBinding = 3; write.descriptorCount = 1;
	write.descriptorType = vk::DescriptorType::eStorageBuffer; write.pBufferInfo = &info;
	Check(encoder.Bindings(state, std::as_bytes(std::span(constants)), std::span(&write, 1)), "binding bundle rejected");
	const auto accepted = queue.Snapshot().packets;
	write.pNext = &state;
	Check(!encoder.Bindings(state, std::as_bytes(std::span(constants)), std::span(&write, 1)), "binding pNext accepted");
	write.pNext = nullptr;
	std::vector<uint32_t> oversized(20000);
	state.constant_bytes = static_cast<uint32_t>(oversized.size() * sizeof(uint32_t));
	Check(!encoder.Bindings(state, std::as_bytes(std::span(oversized)), std::span(&write, 1)), "oversized bundle accepted");
	Check(queue.Snapshot().packets == accepted, "binding rejection appended part of the bundle");
	constants.fill(0); info = {}; write = {}; state = {};
	sink.release.release(); queue.DrainUntilIdle(Drain::Submit);
	Check(sink.records.size() == 1 && sink.records[0].opcode == static_cast<uint16_t>(Packets::Op::Bindings), "binding bundle was split");
	Packets::Reader reader(sink.records[0].bytes); Packets::Writes header {}; Packets::Write flat {};
	Check(reader.Read(state) && state.heap_index == 7 && state.push_descriptors && state.constant_bytes == 8 &&
	      reader.ReadArray(std::span(constants)) && constants == std::array<uint32_t, 2>{11, 22} &&
	      reader.Read(header) && header.count == 1 && reader.Read(flat) && flat.binding == 3 &&
	      reader.Read(info) && info.offset == 32 && info.range == 128 && reader.Empty(), "binding bundle borrowed its source");
	queue.Stop();
}

void TestSizedVertices() {
	Sink sink;
	Queue queue(Sink::Consume, &sink, {65536, 2, false}, Sink::Start, Sink::Stop);
	Check(queue.SetEnabled(true), "sized vertex enable failed");
	Check(queue.Append(Sink::Pause, {}), "sized vertex pause failed"); queue.Publish(); sink.entered.acquire();
	Packets::Encoder encoder(queue);
	std::array<vk::Buffer, 2> buffers {};
	std::array<uint64_t, 2> offsets {16, 32}, sizes {64, 128};
	Check(encoder.BindVerticesSized(3, buffers, offsets, sizes), "sized vertex packet rejected");
	const auto accepted = queue.Snapshot().packets;
	Check(!encoder.BindVerticesSized(3, buffers, offsets, std::span(sizes).first(1)) &&
	      queue.Snapshot().packets == accepted, "mismatched sizes partially appended a packet");
	sizes.fill(0); offsets.fill(UINT64_MAX);
	sink.release.release(); queue.DrainUntilIdle(Drain::Submit);
	Check(sink.records.size() == 1 && sink.records[0].opcode == static_cast<uint16_t>(Packets::Op::BindVerticesSized),
	      "sized vertex packet split or opcode lost");
	Packets::Reader reader(sink.records[0].bytes); Packets::Vertices header {};
	Check(reader.Read(header) && header.first == 3 && header.count == 2 && reader.ReadArray(std::span(buffers)) &&
	      reader.ReadArray(std::span(offsets)) && reader.ReadArray(std::span(sizes)) && reader.Empty() &&
	      offsets == std::array<uint64_t, 2>{16, 32} && sizes == std::array<uint64_t, 2>{64, 128},
	      "vertex bounds borrowed producer data");
	queue.Stop();
}

void TestTransferSnapshots() {
	Sink sink;
	Queue queue(Sink::Consume, &sink, {65536, 2, false}, Sink::Start, Sink::Stop);
	Check(queue.SetEnabled(true), "transfer enable failed");
	Check(queue.Append(Sink::Pause, {}), "transfer pause failed"); queue.Publish(); sink.entered.acquire();
	Packets::Encoder encoder(queue);
	vk::BufferCopy copy {}; copy.srcOffset = 64; copy.dstOffset = 128; copy.size = 256;
	Check(encoder.TransferArray(Packets::Op::CopyBuffer, {}, std::span<const vk::BufferCopy>(&copy, 1)), "copy rejected");
	copy = {};
	vk::BufferMemoryBarrier barrier {}; barrier.offset = 128; barrier.size = 256;
	barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	Check(encoder.LegacyBarriers(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eComputeShader,
	    {}, {}, std::span(&barrier, 1), {}), "legacy barrier rejected");
	barrier.pNext = &copy;
	Check(!encoder.LegacyBarriers({}, {}, {}, {}, std::span(&barrier, 1), {}), "legacy pNext accepted");
	barrier = {};
	vk::RenderingAttachmentInfo attachment {}; attachment.loadOp = vk::AttachmentLoadOp::eClear;
	attachment.clearValue.color.uint32[0] = 123;
	vk::RenderingInfo rendering {}; rendering.layerCount = 1; rendering.renderArea.extent = {64, 32};
	rendering.colorAttachmentCount = 1; rendering.pColorAttachments = &attachment;
	Check(encoder.NativeRendering(rendering), "native rendering rejected");
	attachment.pNext = &rendering;
	Check(!encoder.NativeRendering(rendering), "rendering attachment chain accepted");
	rendering = {}; attachment = {};
	vk::ClearAttachment clear {}; clear.colorAttachment = 2; clear.clearValue.color.uint32[0] = 456;
	vk::ClearRect rectangle {}; rectangle.rect.extent = {32, 16}; rectangle.layerCount = 1;
	Check(encoder.ClearAttachments(std::span(&clear, 1), std::span(&rectangle, 1)), "clear rejected");
	clear = {}; rectangle = {};
	std::array<vk::BufferCopy, 513> oversized {};
	Check(!encoder.TransferArray(Packets::Op::CopyBuffer, {}, std::span<const vk::BufferCopy>(oversized)), "oversized transfer accepted");
	sink.release.release(); queue.DrainUntilIdle(Drain::Upload);
	Check(sink.records.size() == 4, "rejected transfer/render packet partially appended");
	Packets::Reader transfer(sink.records[0].bytes); Packets::Transfer header {};
	Check(transfer.Read(header) && header.count == 1 && transfer.Read(copy) && copy.srcOffset == 64 &&
	    copy.dstOffset == 128 && copy.size == 256 && transfer.Empty(), "copy regions borrowed producer memory");
	Packets::Reader legacy(sink.records[1].bytes); Packets::LegacyBarrier legacy_header {};
	Check(legacy.Read(legacy_header) && legacy_header.counts.buffers == 1 && legacy.Read(barrier) &&
	    barrier.offset == 128 && barrier.size == 256 && !barrier.pNext && legacy.Empty(), "legacy barriers borrowed producer memory");
	Packets::Reader render(sink.records[2].bytes); Packets::Rendering render_header {};
	Check(render.Read(render_header) && !render_header.info.pColorAttachments && render_header.info.renderArea.extent.width == 64 &&
	    render.Read(attachment) && attachment.clearValue.color.uint32[0] == 123 && !attachment.pNext && render.Empty(),
	    "render attachment arrays borrowed producer memory");
	Packets::Reader clearing(sink.records[3].bytes); Packets::Attachments clear_header {};
	Check(clearing.Read(clear_header) && clear_header.count == 1 && clear_header.rectangles == 1 && clearing.Read(clear) &&
	    clearing.Read(rectangle) && clear.clearValue.color.uint32[0] == 456 && rectangle.rect.extent.width == 32 && clearing.Empty(),
	    "clear rectangles borrowed producer memory");
	queue.Stop();
}

void TestTickLifetime() {
	Sink sink;
	Queue queue(Sink::Consume, &sink, {64, 2, false}, Sink::Start, Sink::Stop);
	Common::CommandRecording::TickPins pins;
	auto resource = std::make_shared<int>(123);
	std::weak_ptr<int> weak = resource;
	pins.Hold(5, resource); pins.Hold(7, resource); resource.reset();
	Check(queue.SetEnabled(true) && queue.AppendValue(1, uint64_t{5}), "lifetime enqueue failed");
	queue.DrainUntilIdle(Drain::Submit);
	Check(!weak.expired() && pins.Pending() == 2, "recording drain freed native resources");
	queue.SetEnabled(false);
	Check(!weak.expired(), "gate off freed native resources");
	pins.Complete(5); Check(!weak.expired() && pins.Pending() == 1, "earlier tick freed a newer reference");
	pins.Complete(6); Check(!weak.expired(), "uncompleted native tick freed resource");
	pins.Complete(7); Check(weak.expired() && pins.Pending() == 0, "native completion retained resource");
}
} // namespace

int main() {
	using namespace Common::SubmitProfile;
	Check(ParseExperiment("vk-recorder") == Experiment::VkRecorder, "experiment parse failed");
	Check(ParseExperiment("vk-recorder-extra") == Experiment::None, "experiment matched prefix");
	TestQueue(); TestSnapshots(); TestBindingPacket(); TestSizedVertices(); TestTransferSnapshots(); TestTickLifetime();
	std::puts("VkRecorderTests: transport, snapshots and tick pins passed (no native Vulkan execution)");
}
