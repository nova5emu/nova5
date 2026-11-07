// Included inside VulkanHarness. Adapted from Nova5 1372e0c1 to GpuResourceManager
// and the unified cache's pending-download ownership. No guest binaries needed.
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  void CheckLinearImageCpuReadback() {
    using namespace Libs::LibKernel;
    static_assert(Wolverine::LinearReadbackEnabled("PPSA03671", nullptr));
    static_assert(Wolverine::LinearReadbackEnabled("PPSA03671", "1"));
    static_assert(!Wolverine::LinearReadbackEnabled("PPSA03671", "0"));
    static_assert(!Wolverine::LinearReadbackEnabled("PPSA03671", "invalid"));
    static_assert(!Wolverine::LinearReadbackEnabled("PPSA01341", "1"));
    static_assert(!Wolverine::LinearReadbackEnabled("PPSA01341", nullptr));
    static_assert(!Wolverine::LinearReadbackEnabled("", "1"));
    constexpr const char* name = "LinearImageCpuReadback";
    constexpr uint64_t base = 0x0000000205800000ull, size = 0x200000;
    constexpr uint64_t destination = base + 0xbf00, bytes = 384 * 216 * 4;
    constexpr uint64_t sentinel = destination + bytes - 4;
    EnsureRuntimeContext();
    int64_t physical = -1;
    Require(name, "allocate", Memory::KernelAllocateDirectMemory(
        0, Memory::KernelGetDirectMemorySize(), size, size, 0, &physical) == 0, "allocation failed");
    void* mapped = reinterpret_cast<void*>(base);
    Require(name, "map", Memory::KernelMapDirectMemory(&mapped, size, 3, 0x10, physical, size) == 0 &&
        mapped == reinterpret_cast<void*>(base), "mapping failed");
    std::memset(mapped, 0x5a, size);
    *reinterpret_cast<uint32_t*>(sentinel) = 0xbf800000u;
    {
      RenderContext context(m_runtime_context);
      context.InitializeGpu(nullptr);
      auto& resources = context.GetGpuResources();
      Memory::InstallGpuResources(&resources);
      auto& cache = context.GetTextureCache();
      auto& scheduler = context.GetCommandScheduler();
      auto on_gpu = [&](auto&& body) { context.GetGpu().SendCommandSync([&] { body(); }); };
      auto load = [](uint64_t address) { return *reinterpret_cast<const volatile uint32_t*>(address); };
      std::unique_ptr<CommandProcessor> processor;
      ImageId source_id{}, destination_id{};
      auto copy = [&](float value) {
        vk::ClearValue clear{};
        clear.color.float32[0] = value;
        TextureCacheTestAccess::ClearImage(cache, scheduler.Current(), source_id,
            {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}, clear);
        TextureCacheTestAccess::CopyImage(cache, destination_id, source_id);
        // Complete GPU work without presenting a frame or ProcessDownloadImages.
        scheduler.Finish();
      };
      on_gpu([&] {
        resources.MapMemory(base, size);
        processor = std::make_unique<CommandProcessor>(context, 0);
        processor->BufferInit();
        TextureCacheTestAccess::SetLinearReadback(cache, false);
        TextureCacheTestAccess::SetDemandLinearReadback(cache, false);
        ImageDesc desc{};
        desc.type = BindingType::Texture;
        desc.info.data = {destination, bytes};
        desc.info.pixel_format = vk::Format::eR32Sfloat;
        desc.info.guest_format = Prospero::BufferFormat::k32Float;
        desc.info.type = Prospero::ImageType::kColor2D;
        desc.info.extent = vk::Extent3D{384, 216, 1};
        desc.info.resources = {1, 1};
        desc.info.pitch = 384;
        desc.info.bytes_per_block = 4;
        desc.info.samples = 1;
        desc.info.tile_mode = Prospero::TileMode::kLinear;
        desc.info.mip_layout[0] = {0, bytes, 384, 216};
        desc.view_info.format = vk::Format::eR32Sfloat;
        desc.view_info.type = vk::ImageViewType::e2D;
        desc.view_info.aspect = vk::ImageAspectFlagBits::eColor;
        desc.view_info.layer_count = 1;
        desc.view_info.usage = vk::ImageUsageFlagBits::eSampled;
        destination_id = cache.FindImage(desc);
        (void)cache.FindTexture(destination_id, desc);
        desc.info.data.address = base + 0x100000;
        source_id = cache.FindImage(desc);
        (void)cache.FindTexture(source_id, desc);
        copy(0.25f);
      });
      Require(name, "disabled control", load(sentinel) == 0xbf800000u,
              "control no longer reproduces the stale CPU sentinel");
      on_gpu([&] {
        TextureCacheTestAccess::SetDemandLinearReadback(cache, true);
        copy(0.25f);
      });
      Require(name, "read-only polling", load(sentinel) == 0x3e800000u,
              "CPU kept reading -1 after the GPU image copy completed");
      uint64_t tick = 0;
      on_gpu([&] { tick = scheduler.CurrentTick(); });
      for (uint32_t attempt = 0; attempt < 1024; ++attempt)
        Require(name, "published reads", load(sentinel) == 0x3e800000u, "published value changed");
      on_gpu([&] {
        Require(name, "single download", tick == scheduler.CurrentTick(), "each CPU read submitted work");
        copy(0.5f);
      });
      *reinterpret_cast<volatile uint32_t*>(destination) = 0x12345678u;
      Require(name, "partial CPU write", load(sentinel) == 0x3f000000u &&
              load(destination) == 0x12345678u, "CPU write lost GPU data or readback overwrote the CPU");
      on_gpu([&] { copy(0.5f); });
      const uint32_t host_value = 0x87654321u;
      Memory::PrepareHostOverwrite(destination, sizeof(host_value));
      Require(name, "host partial write", Memory::TryWriteBacking(destination, &host_value, sizeof(host_value)) &&
              load(sentinel) == 0x3f000000u && load(destination) == host_value,
              "host overwrite failed to preserve the other GPU pixels");
      on_gpu([&] { copy(0.75f); });
      Require(name, "shared page", load(sentinel + 4) == 0x5a5a5a5au &&
              load(sentinel) == 0x3f400000u && load(destination - 4) == 0x5a5a5a5au,
              "page-neighbour fault lost bytes or did not publish the image");
      on_gpu([&] {
        copy(1.0f);
        auto [buffer, offset] = context.GetBufferCache().ObtainBuffer(sentinel + 4, 4, true);
        buffer->Fill(offset, 4, 0xdeadbeefu);
        uint32_t stale = 0;
        Require(name, "no clean-alias bypass",
                !Memory::TryReadGpuCleanBackingToHost(sentinel, &stale, sizeof(stale)),
                "clean-read helper bypassed a pending image readback");
      });
      Require(name, "buffer/image watchers", load(sentinel) == 0x3f800000u &&
              load(sentinel + 4) == 0xdeadbeefu, "buffer and image readbacks overwrote one another");
      on_gpu([&] {
        copy(0.25f);
        // No Finish after this: the image slot goes away before WriteBacking.
        TextureCacheTestAccess::DownloadAndRetire(cache, destination_id);
      });
      Require(name, "retired source", load(sentinel) == 0x3e800000u,
              "retiring an image exposed stale pixels before its pending download landed");
      on_gpu([&] {
        resources.UnmapMemory(base, size);
        scheduler.Finish();
        scheduler.DrainPriorityOperations();
        processor.reset();
      });
      Memory::InstallGpuResources(nullptr);
      context.ShutdownGpu();
    }
    Require(name, "unmap", Memory::KernelMunmap(base, size) == 0, "unmap failed");
    Require(name, "release", Memory::KernelReleaseDirectMemory(physical, size) == 0, "release failed");
    std::printf("[gpu]     %-32s ok\n", name);
  }
#endif
