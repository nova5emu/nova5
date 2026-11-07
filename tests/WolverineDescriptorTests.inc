// CPU-only: exercise the payload builder and final invariant used by the direct
// CommitBindings path. Handles below are opaque sentinels, never sent to Vulkan.
void CheckWolverineDescriptorCounts() {
  namespace IR = ShaderRecompiler::IR;
  using Dimension = ShaderRecompiler::Decoder::ImageDimension;
  constexpr const char* name = "WolverineDescriptorCounts";
  for (const auto stage : {ShaderType::Compute, ShaderType::Vertex,
                           ShaderType::Mesh, ShaderType::Pixel}) {
    for (uint32_t mode = 0; mode < 3; ++mode) {
      for (uint32_t sampler_count : {0u, 2u}) {
        // Resident only; mixed resident/ordinary; ordinary-only control.
        const std::vector<uint32_t> expected = mode == 0
            ? std::vector<uint32_t>{0u}
            : std::vector<uint32_t>{mode == 1 ? 0u : 1u, 3u, 1u, 1u,
                                    mode == 1 ? 0u : 1u};
        IR::Program source;
        source.stage = stage;
        source.shader_info_complete = true;
        source.info.images.resize(expected.size());
        source.info.samplers.resize(sampler_count);
        for (auto& image : source.info.images) {
          image.resource_class = IR::ImageResourceClass::Sampled;
          image.numeric_class = Prospero::TextureNumericClass::Float;
          image.dimension = Dimension::Dim2D;
          image.mip_count = 1;
        }
        source.info.images[0].bindless = mode != 2;
        source.info.images[0].dimension = Dimension::Dim2DArray;
        source.info.images[0].cube = true;
        if (mode != 0) {
          auto& storage = source.info.images[1];
          storage.resource_class = IR::ImageResourceClass::Storage;
          storage.mip_mode = IR::ImageMipMode::DynamicStorage;
          storage.mip_count = 3;
          source.info.images[2].dimension = Dimension::Dim2DArray;
          source.info.images[2].cube = true;
          source.info.images[3].dimension = Dimension::Dim2DArray;
          source.info.images[4].dimension = Dimension::Dim3D;
          source.info.images[4].bindless = mode == 1;
        }
        IR::AllocateBindings(source);
        IR::CompiledShaderInfo program;
        program.stage = source.stage;
        program.info = source.info;
        program.bindings = source.bindings;
        ShaderStageRuntime runtime;
        runtime.program = &program;
        PreparedBindings prepared;
        prepared.runtime = &runtime;
        prepared.images.resize(expected.size());
        std::vector<vk::ImageView> expected_views;
        for (size_t i = 0; i < expected.size(); ++i) {
          auto& texture = prepared.images[i];
          if (expected[i] == 0) {
            // Even a leftover mip vector must not consume set-0 descriptors.
            texture.mip_views.resize(6);
            continue;
          }
          texture.image_id = ImageId(static_cast<uint32_t>(i));
          texture.layout = vk::ImageLayout::eGeneral;
          texture.desc.view_info.layer_count = 6;
          texture.desc.view_info.level_count = 5;
          for (uint32_t mip = 0; mip < expected[i]; ++mip) {
            const auto view = vk::ImageView(reinterpret_cast<VkImageView>(uintptr_t(100 + i * 10 + mip)));
            expected_views.push_back(view);
            if (expected[i] == 1) texture.image_view = view;
            else texture.mip_views.push_back(view);
          }
        }
        for (uint32_t i = 0; i < sampler_count; ++i)
          prepared.samplers.emplace_back(reinterpret_cast<VkSampler>(uintptr_t(200 + i)));

        std::vector<uint32_t> occurrences(expected.size(), 0);
        std::vector<vk::DescriptorImageInfo> infos;
        for (const auto& binding : program.bindings.descriptors) {
          if (binding.kind != IR::DescriptorBindingKind::Samplers &&
              IR::ImageBindingResourceClass(binding.kind) == IR::ImageResourceClass::None) continue;
          const auto start = infos.size();
          AppendImageDescriptorInfos(prepared, binding, occurrences, infos);
          Require(name, "binding payload count", infos.size() - start == NativeDescriptorCount(binding),
                  "direct payload disagrees with the Vulkan layout");
        }
        Require(name, "direct set-0 occurrences", occurrences == expected &&
                    ImageDescriptorCountsMatch(prepared, occurrences),
                "resident image counted in set 0, or ordinary mip descriptor lost");
        Require(name, "total images and samplers", infos.size() == expected_views.size() + sampler_count,
                "cube faces, sampled mip levels or resident placeholders changed descriptor count");
        for (const auto view : expected_views)
          Require(name, "ordinary views", std::ranges::count(infos, view, &vk::DescriptorImageInfo::imageView) == 1,
                  "an ordinary mip view was omitted, duplicated or reordered onto another resource");
        for (const auto sampler : prepared.samplers)
          Require(name, "samplers", std::ranges::count(infos, sampler, &vk::DescriptorImageInfo::sampler) == 1,
                  "resident images displaced a sampler");

        auto invalid = occurrences;
        ++invalid[0];
        Require(name, "extra occurrence rejected", !ImageDescriptorCountsMatch(prepared, invalid),
                "extra set-0 descriptor was accepted");
        if (mode != 0) {
          invalid = occurrences;
          --invalid[1];
          Require(name, "missing storage mip rejected", !ImageDescriptorCountsMatch(prepared, invalid),
                  "missing storage mip was accepted");
          prepared.images[1].mip_views.resize(4);
          Require(name, "changed mip range rejected", !ImageDescriptorCountsMatch(prepared, occurrences),
                  "stale mip counts were accepted");
        }
      }
    }
  }
#ifdef KYTY_LOCAL_VULKAN_RECORDING
  Require(name, "deferred packet offsets and stages", TestLocalDescriptorPlanBindless(),
          "deferred descriptor packet disagrees for resident images or Mesh/Pixel stages");
#endif
  std::printf("[host] WolverineDescriptorCounts ok (CPU only)\n");
}
