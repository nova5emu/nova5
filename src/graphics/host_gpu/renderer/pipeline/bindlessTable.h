#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_BINDLESSTABLE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_BINDLESSTABLE_H_

#include "common/common.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/shader/recompiler/ir/BindlessBindings.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <utility>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Libs::Graphics {

struct GraphicContext;
class CommandScheduler;
class PageManager;
// Descriptor set 1 of every pipeline that samples bindless images: typed arrays of sampled
// images a shader indexes with a slot it looks up per pixel, the key -> slot translation the
// host fills per guest descriptor heap, and a bitset where shaders flag keys whose texture is
// not resident yet. Created only when the device supports descriptor indexing.
class BindlessTable {
public:
	enum Binding : uint32_t {
		Images2D      = 0,
		Images2DArray = 1,
		ImagesCube    = 2,
		Images3D      = 3,
		Translation   = 4,
		Feedback      = 5,
	};
	static_assert(Images2D == ShaderRecompiler::IR::BindlessImages2D &&
	              Images2DArray == ShaderRecompiler::IR::BindlessImages2DArray &&
	              ImagesCube == ShaderRecompiler::IR::BindlessImagesCube &&
	              Images3D == ShaderRecompiler::IR::BindlessImages3D &&
	              Translation == ShaderRecompiler::IR::BindlessTranslation &&
	              Feedback == ShaderRecompiler::IR::BindlessFeedback);
	static constexpr uint32_t ImageArrays        = 4;
	static constexpr uint32_t PlaceholderColors  = 3;
	static constexpr uint32_t Placeholders       = ImageArrays * PlaceholderColors;
	static constexpr uint32_t MaxImagesPerArray  = 65536; // bounded by the device limits
	static constexpr uint32_t TranslationEntries = 1u << 20u;

	BindlessTable(GraphicContext& graphics, CommandScheduler& scheduler, PageManager& page_manager);
	~BindlessTable();
	KYTY_CLASS_NO_COPY(BindlessTable);

	[[nodiscard]] bool                    Enabled() const noexcept { return m_set != nullptr; }
	[[nodiscard]] vk::DescriptorSetLayout Layout() const noexcept { return m_layout; }
	[[nodiscard]] vk::DescriptorSet       Set() const noexcept { return m_set; }
	[[nodiscard]] uint32_t                ImagesPerArray() const noexcept { return m_images_per_array; }

	// A guest descriptor heap as one view type samples it: a region of the translation (and
	// feedback) buffer, one entry per key, and the textures resolved so far.
	struct Heap {
		uint64_t base = 0;
		uint32_t table_offset = 0, binding = 0, region = 0;
		uint32_t entries = 0, guest_entries = 0;
		uint32_t watched_entries = 0;
		uint64_t watch_generation = 0;
		ShaderRecompiler::IR::ImageResource resource;
		std::vector<uint32_t> slots;
		std::vector<uint8_t> settled;
		std::vector<ImageId> resolved;
		std::vector<ImageId> images;
	};
	// The heap for (base, table offset, view binding), created with every entry pending and
	// room to grow, or moved to a larger region when a descriptor covers more keys; null when
	// the translation buffer is full.
	[[nodiscard]] Heap* FindOrCreateHeap(uint64_t base, uint32_t table_offset, uint32_t binding,
	                                     uint32_t entries,
	                                     const ShaderRecompiler::IR::ImageResource& resource);
	[[nodiscard]] std::deque<Heap>& Heaps() noexcept { return m_heaps; }
	// 0 when the array is full. A released slot is reused once the GPU has finished the work
	// recorded before its release.
	// The binding and array slot a Heap::slots entry names.
	[[nodiscard]] static uint32_t SlotBinding(const Heap& heap, uint32_t) { return heap.binding; }
	[[nodiscard]] static uint32_t SlotIndex(uint32_t entry) { return entry; }
	// Placeholder 0 (transparent, or grey with KYTY_BINDLESS_DEBUG_COLORS) of a binding's view type.
	[[nodiscard]] vk::ImageView PlaceholderView(uint32_t binding) const {
		return m_placeholder_views[binding];
	}
	[[nodiscard]] uint32_t AllocateSlot(uint32_t binding);
	void WriteSlot(uint32_t binding, uint32_t slot, vk::ImageView view, vk::ImageLayout layout);
	// Resolve a key to a slot (0 = placeholder), or back to pending.
	void SetTranslation(const Heap& heap, uint32_t key, uint32_t slot);
	// The host reads the feedback flags from a copy in host-cached memory. The feedback buffer
	// itself is device memory, and reading every key of three heaps from it uncached took a
	// fifth of the GPU thread. Records that copy at the current point of the command stream.
	void RecordFeedbackSnapshot(CommandScheduler& scheduler);
	[[nodiscard]] bool SnapshotRecorded() const noexcept { return m_snapshot_tick != 0; }
	// Whether the GPU has executed the recorded copy; never waits.
	[[nodiscard]] bool SnapshotReady(CommandScheduler& scheduler);
	void               ConsumeSnapshot() noexcept { m_snapshot_tick = 0; }
	// Pending keys flagged in the completed snapshot (GPU clears follow the copy).
	void TakeRequests(const Heap& heap, std::vector<uint32_t>& keys);
	void AddImageReference(ImageId id, Heap& heap, uint32_t key);
	// The key no longer samples what it was settled to: it is pending again, and its image loses
	// the key's reference. Its slot keeps the old view, which frames in flight may still sample;
	// a new resolution takes a new slot. True when the image has no reference left, so it need
	// not stay pinned in the texture cache.
	[[nodiscard]] bool ReleaseKey(Heap& heap, uint32_t key);

	// The guest pages holding a heap's entries are write-watched; a CPU write
	// to one (a page fault on a game thread, or an explicit invalidation such as a file read into
	// the heap) stops watching that page and records it as written. TakeWrittenRanges, on the GPU
	// thread at the start of a guest submission, watches the written pages again before it
	// returns them, so a write after that is caught by the next submission; the caller then
	// invalidates their resolved keys. Heaps of different view bindings over
	// the same guest table share one watch.
	void WatchHeap(Heap& heap);
	[[nodiscard]] bool NeedsWatch(const Heap& heap) const noexcept {
		return heap.watched_entries != heap.guest_entries ||
		       heap.watch_generation != m_watch_generation.load(std::memory_order_acquire);
	}
	void OnCpuWrite(uint64_t vaddr, uint64_t size); // any thread
	void UnwatchRange(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool HasWrittenRanges() const noexcept {
		return m_heap_writes.load(std::memory_order_acquire);
	}
	void TakeWrittenRanges(std::vector<std::pair<uint64_t, uint64_t>>& ranges);

	// The texture cache dropped a resolved image: retire its slots and
	// make its keys pending again, so a draw that still needs it asks for it anew.
	void OnImageUnregistered(ImageId id);
	// Keys made pending because the texture cache dropped their image (statistics; reset on read).
	[[nodiscard]] uint64_t TakeUnregisteredKeys() noexcept { return std::exchange(m_unregistered_keys, 0); }
	// Before an image is destroyed: a slot that still holds one of its views is repointed to the
	// placeholder and the keys that use it are pending again. True when one did, so the caller
	// keeps the image until the GPU has finished the work recorded with that slot.
	[[nodiscard]] bool ReleaseViews(std::span<const vk::ImageView> views);

private:
	[[nodiscard]] bool AllocateRegion(Heap& heap, uint32_t entries);

	// The placeholders of every image array: transparent by default, debug colors optional, for keys
	// outside a heap and pending textures. Created, cleared and made read-only once.
	void CreatePlaceholders(CommandScheduler& scheduler);

	[[nodiscard]] static uint64_t ImageKey(ImageId id) {
		return static_cast<uint64_t>(id.index) | (static_cast<uint64_t>(id.generation) << 32u);
	}

	// A key gave up its slot: free it for reuse after the GPU passes the current tick.
	void ReleaseSlot(uint32_t binding, uint32_t slot);

	GraphicContext&         m_graphics;
	CommandScheduler&       m_scheduler;
	PageManager&            m_page_manager;
	// Write-watched guest ranges of the heaps, page by page (TRACKER_PAGE_SIZE).
	struct Watch {
		uint64_t             begin = 0;
		uint64_t             end   = 0;
		std::vector<uint8_t> watched;
		std::vector<uint8_t> written;
	};
	std::mutex            m_watch_mutex;
	std::vector<Watch>    m_watches;
	std::vector<std::pair<uint64_t, uint64_t>> m_unmapped_ranges;
	std::atomic_bool      m_heap_writes {false};
	// Bounds of every watch, so a write far from the heaps returns without the lock.
	std::atomic<uint64_t> m_watch_low {UINT64_MAX};
	std::atomic<uint64_t> m_watch_high {0};
	std::atomic<uint64_t> m_watch_generation {1};
	uint64_t              m_unregistered_keys = 0;
	std::deque<Heap>        m_heaps;
	std::array<std::deque<std::pair<uint32_t, uint64_t>>, ImageArrays> m_free_slots; // slot, tick
	std::unordered_map<uint64_t, std::vector<std::pair<Heap*, uint32_t>>> m_image_refs;
	uint32_t                m_next_region = 1; // translation[0] is the out-of-range entry
	std::array<uint32_t, ImageArrays> m_next_slot {PlaceholderColors, PlaceholderColors,
	                                               PlaceholderColors, PlaceholderColors};
	std::array<VulkanImage, Placeholders>   m_placeholders;
	std::array<vk::ImageView, Placeholders> m_placeholder_views {};
	vk::DescriptorPool      m_pool   = nullptr;
	vk::DescriptorSetLayout m_layout = nullptr;
	vk::DescriptorSet       m_set    = nullptr;
	uint32_t                m_images_per_array = 0;
	std::unique_ptr<Buffer> m_translation;
	std::unique_ptr<Buffer> m_feedback;
	std::unique_ptr<Buffer> m_feedback_snapshot;
	uint64_t                m_snapshot_tick = 0; // 0 = none recorded
	// The view each image slot holds, and the slots each view is in (placeholders excluded).
	std::array<std::vector<vk::ImageView>, ImageArrays>                        m_slot_views;
	std::unordered_map<VkImageView, std::vector<std::pair<uint32_t, uint32_t>>> m_view_slots;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_BINDLESSTABLE_H_
