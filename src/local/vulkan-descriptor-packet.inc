// Included within Libs::Graphics in descriptors.cpp, only for the local worker.
// Pipeline keys include the exact shader permutation IDs, hence the binding
// layout is immutable for a pipeline. Runtime image mip counts are checked too.
struct LocalDescriptorPlan {
    struct Stage {
        size_t buffer_count = 0, sampler_count = 0, image_count = 0;
        std::vector<uint32_t> image_mips;
        uint32_t data_dwords = 0, push_start = ShaderRecompiler::IR::PushData::NoStart;
        bool lod = false, bda = false, fault = false;
    };
    struct Write {
        vk::WriteDescriptorSet prototype;
        size_t start = 0;
        bool image = false;
    };
    std::vector<Stage> stages;
    std::vector<Write> writes;
    std::vector<uint32_t> buffer_indices, image_indices;
    vk::ShaderStageFlags push_stages;
};

static std::shared_ptr<const LocalDescriptorPlan> MakeLocalDescriptorPlan(
    vk::PipelineBindPoint point, std::span<PreparedBindings* const> prepared) {
    auto plan = std::make_shared<LocalDescriptorPlan>();
    if (point == vk::PipelineBindPoint::eGraphics)
        plan->push_stages = vk::ShaderStageFlagBits::eFragment;
    uint32_t buffer_base = 0, image_base = 0;
    for (const auto* input : prepared) {
        const auto& program = *input->runtime->program;
        LocalDescriptorPlan::Stage stage;
        stage.buffer_count = input->buffers.size();
        stage.sampler_count = input->samplers.size();
        stage.data_dwords = program.bindings.ShaderDataDwords();
        stage.push_start = program.bindings.push_data_start_dword;
        plan->push_stages |= NativeShaderStage(program.stage);
        std::vector<uint32_t> image_offsets, occurrences(input->images.size(), 0);
        uint32_t sampler_base = image_base;
        for (size_t resource = 0; resource < input->images.size(); ++resource) {
            image_offsets.push_back(sampler_base);
            const auto count = Set0ImageDescriptorCount(*input, resource);
            stage.image_mips.push_back(count);
            sampler_base += count;
        }
        stage.image_count = sampler_base - image_base;
        for (const auto& binding : program.bindings.descriptors) {
            LocalDescriptorPlan::Write write;
            write.prototype.dstBinding = ShaderRecompiler::IR::NativeBinding(program.stage, binding.kind);
            write.prototype.descriptorType = NativeDescriptorType(binding.kind);
            write.prototype.descriptorCount = NativeDescriptorCount(binding);
            write.image = ShaderRecompiler::IR::ImageBindingResourceClass(binding.kind) !=
                ShaderRecompiler::IR::ImageResourceClass::None || binding.kind == BindingKind::Samplers;
            auto& indices = write.image ? plan->image_indices : plan->buffer_indices;
            write.start = indices.size();
            if (write.image) {
                for (const auto resource : binding.resources) {
                    if (binding.kind == BindingKind::Samplers) {
                        EXIT_IF(resource >= input->samplers.size());
                        indices.push_back(sampler_base + resource);
                    } else {
                        const auto element = occurrences.at(resource)++;
                        EXIT_IF(element >= stage.image_mips.at(resource));
                        indices.push_back(image_offsets.at(resource) + element);
                    }
                }
            } else if (binding.kind == BindingKind::Buffers) {
                for (const auto resource : binding.resources) {
                    EXIT_IF(resource >= input->buffers.size());
                    indices.push_back(buffer_base + resource);
                }
            } else {
                uint32_t extra = 0;
                switch (binding.kind) {
                    case BindingKind::Gds: extra = 0; break;
                    case BindingKind::FlattenedSrt: extra = 1; break;
                    case BindingKind::ShaderData: extra = 2; break;
                    case BindingKind::LodStats: extra = 3; stage.lod = true; break;
                    case BindingKind::BdaPagetable: extra = 4; stage.bda = true; break;
                    case BindingKind::FaultBuffer: extra = 5; stage.fault = true; break;
                    default: EXIT("invalid local descriptor binding kind");
                }
                indices.push_back(buffer_base + uint32_t(stage.buffer_count) + extra);
            }
            EXIT_IF(indices.size() - write.start != write.prototype.descriptorCount);
            plan->writes.push_back(write);
        }
        EXIT_IF(!ImageDescriptorCountsMatch(*input, occurrences));
        buffer_base += uint32_t(stage.buffer_count) + 6;
        image_base = sampler_base + uint32_t(stage.sampler_count);
        plan->stages.push_back(std::move(stage));
    }
    return plan;
}

bool TestLocalDescriptorPlanBindless() {
    namespace IR = ShaderRecompiler::IR;
    for (const auto first_stage: {ShaderType::Compute, ShaderType::Vertex, ShaderType::Mesh}) {
        const auto point = first_stage == ShaderType::Compute ? vk::PipelineBindPoint::eCompute
                                                              : vk::PipelineBindPoint::eGraphics;
        for (uint32_t mode = 0; mode < 3; ++mode) {
            // 0: resident only; 1: mixed, with a resident image before normal
            // resources; 2: ordinary images, preserving the old packet layout.
            IR::CompiledShaderInfo programs[2];
            ShaderStageRuntime runtimes[2];
            PreparedBindings prepared[2];
            std::array<PreparedBindings*, 2> inputs {&prepared[0], &prepared[1]};
            const size_t stages = point == vk::PipelineBindPoint::eCompute ? 1u : 2u;
            const std::vector<uint32_t> mips = mode == 0 ? std::vector<uint32_t> {0u}
                : std::vector<uint32_t> {mode == 1 ? 0u : 1u, 3u, 1u};
            const std::vector<uint32_t> indices = mode == 0 ? std::vector<uint32_t> {0u}
                : mode == 1 ? std::vector<uint32_t> {3u, 0u, 1u, 2u, 4u}
                            : std::vector<uint32_t> {0u, 4u, 1u, 2u, 3u, 5u};
            for (size_t s = 0; s < stages; ++s) {
                auto& program = programs[s];
                program.stage = s == 0 ? first_stage : ShaderType::Pixel;
                program.info.images.resize(mips.size());
                for (auto& image: program.info.images) {
                    image.resource_class = IR::ImageResourceClass::Sampled;
                    image.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
                    image.numeric_class = Prospero::TextureNumericClass::Float;
                }
                program.info.images[0].bindless = mode != 2;
                if (mode != 2) {
                    // Guest cubes use a six-layer array view in the resident set.
                    program.info.images[0].dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2DArray;
                    program.info.images[0].cube = true;
                }
                prepared[s].images.resize(mips.size());
                if (mode != 0) {
                    auto& storage = program.info.images[1];
                    storage.resource_class = IR::ImageResourceClass::Storage;
                    storage.mip_mode = IR::ImageMipMode::DynamicStorage;
                    storage.mip_count = 3;
                    prepared[s].images[1].mip_views.resize(3);
                    program.bindings.descriptors.push_back({
                        .kind = *IR::DescriptorBindingForImage(program.info.images[2]),
                        .resources = mode == 1 ? std::vector<uint32_t> {2u} : std::vector<uint32_t> {0u, 2u}});
                    program.bindings.descriptors.push_back({
                        .kind = *IR::DescriptorBindingForImage(storage), .resources = {1u, 1u, 1u}});
                }
                program.info.samplers.resize(1);
                prepared[s].samplers.resize(1);
                program.bindings.descriptors.push_back({.kind = BindingKind::Samplers, .resources = {0u}});
                runtimes[s].program = &program;
                prepared[s].runtime = &runtimes[s];
            }
            const auto plan = MakeLocalDescriptorPlan(point, {inputs.data(), stages});
            std::vector<uint32_t> expected;
            for (size_t s = 0; s < stages; ++s) {
                if (plan->stages[s].image_mips != mips ||
                    plan->stages[s].image_count + 1u != indices.size()) return false;
                for (auto index: indices) expected.push_back(index + uint32_t(s * indices.size()));
            }
            if (plan->image_indices != expected) return false;
            vk::ShaderStageFlags expected_stages = NativeShaderStage(first_stage);
            if (stages == 2) expected_stages |= vk::ShaderStageFlagBits::eFragment;
            if (plan->push_stages != expected_stages) return false;
            size_t write_index = 0;
            for (size_t s = 0; s < stages; ++s) {
                for (const auto& binding: programs[s].bindings.descriptors) {
                    const auto& write = plan->writes.at(write_index++).prototype;
                    if (write.dstBinding != IR::NativeBinding(programs[s].stage, binding.kind) ||
                        write.descriptorCount != NativeDescriptorCount(binding)) return false;
                }
            }
            if (write_index != plan->writes.size()) return false;
            // A changed ordinary mip range invalidates a cached plan; a resident
            // image still occupies zero packet slots regardless of its dummy view.
            prepared[0].images[0].mip_views.resize(2);
            if (Set0ImageDescriptorCount(prepared[0], 0) != (mode == 2 ? 2u : 0u)) return false;
            if (mode != 0) {
                prepared[0].images[1].mip_views.resize(4);
                if (Set0ImageDescriptorCount(prepared[0], 1) == plan->stages[0].image_mips[1]) return false;
            }
        }
    }
    return true;
}

struct LocalDescriptorPacket {
    const LocalDescriptorPlan* plan = nullptr; // Retained by the queue's owner.
    vk::CommandBuffer command;
    vk::PipelineLayout layout;
    vk::PipelineBindPoint point;
};
static void ReplayLocalDescriptors(std::span<const LocalVulkanRecording::Segment> segments,
                                  const vk::detail::DispatchLoaderDynamic& dispatch) {
    const auto& packet = *static_cast<const LocalDescriptorPacket*>(segments[0].data);
    const auto& plan = *packet.plan;
    const auto* buffers = static_cast<const vk::DescriptorBufferInfo*>(segments[1].data);
    const auto* images = static_cast<const vk::DescriptorImageInfo*>(segments[2].data);
    struct Scratch {
        std::vector<vk::DescriptorBufferInfo> buffers;
        std::vector<vk::DescriptorImageInfo> images;
        std::vector<vk::WriteDescriptorSet> writes;
    };
    static thread_local Scratch scratch;
    scratch.buffers.resize(plan.buffer_indices.size());
    scratch.images.resize(plan.image_indices.size());
    scratch.writes.resize(plan.writes.size());
    for (size_t i = 0; i < plan.buffer_indices.size(); ++i) {
        const auto& value = buffers[plan.buffer_indices[i]];
        EXIT_IF(!value.buffer);
        scratch.buffers[i] = value;
    }
    for (size_t i = 0; i < plan.image_indices.size(); ++i)
        scratch.images[i] = images[plan.image_indices[i]];
    for (size_t i = 0; i < plan.writes.size(); ++i) {
        const auto& item = plan.writes[i];
        auto& write = scratch.writes[i];
        write = item.prototype;
        if (item.image) write.pImageInfo = scratch.images.data() + item.start;
        else write.pBufferInfo = scratch.buffers.data() + item.start;
    }
    ShaderRecompiler::IR::PushData push_data;
    bool has_push = false;
    for (size_t i = 0; i < plan.stages.size(); ++i) {
        const auto& stage = plan.stages[i];
        if (stage.push_start == ShaderRecompiler::IR::PushData::NoStart) continue;
        EXIT_IF(stage.push_start + stage.data_dwords > push_data.dwords.size());
        if (stage.data_dwords)
            std::memcpy(push_data.dwords.data() + stage.push_start, segments[3 + i].data,
                        stage.data_dwords * sizeof(uint32_t));
        has_push = true;
    }
    if (has_push)
        packet.command.pushConstants(packet.layout, plan.push_stages, 0, sizeof(push_data),
                                     push_data.dwords.data(), dispatch);
    if (!scratch.writes.empty())
        packet.command.pushDescriptorSetKHR(packet.point, packet.layout, 0,
            static_cast<uint32_t>(scratch.writes.size()), scratch.writes.data(), dispatch);
}

static bool RecordLocalDescriptors(RenderContext& context, vk::CommandBuffer command,
    vk::PipelineBindPoint point, const PipelineCache::Pipeline& pipeline,
    std::span<PreparedBindings* const> inputs, std::vector<vk::DescriptorBufferInfo>& buffers,
    std::vector<vk::DescriptorImageInfo>& images) {
    if (!pipeline.uses_push_descriptors || inputs.empty() || inputs.size() > 2 ||
        !LocalVulkanRecording::PacketsEnabled()) return false;
    auto matches = [&] {
        if (!pipeline.local_descriptor_plan) return false;
        const auto& stages = pipeline.local_descriptor_plan->stages;
        if (stages.size() != inputs.size()) return false;
        for (size_t i = 0; i < stages.size(); ++i) {
            const auto& stage = stages[i];
            const auto& input = *inputs[i];
            if (input.buffers.size() != stage.buffer_count || input.samplers.size() != stage.sampler_count ||
                input.images.size() != stage.image_mips.size()) return false;
            for (size_t j = 0; j < input.images.size(); ++j)
                if (Set0ImageDescriptorCount(input, j) != stage.image_mips[j]) return false;
        }
        return true;
    };
    if (!matches()) pipeline.local_descriptor_plan = MakeLocalDescriptorPlan(point, inputs);
    const auto& plan = *pipeline.local_descriptor_plan;
    buffers.clear();
    images.clear();
    for (size_t i = 0; i < inputs.size(); ++i) {
        const auto& input = *inputs[i];
        const auto& stage = plan.stages[i];
        EXIT_IF(input.shader_data.size() != stage.data_dwords);
        buffers.insert(buffers.end(), input.buffers.begin(), input.buffers.end());
        buffers.push_back(input.gds);
        buffers.push_back(input.flattened_srt);
        buffers.push_back(input.shader_data_buffer);
        auto& cache = context.GetBufferCache();
        const auto view = [](const Buffer* value) -> vk::DescriptorBufferInfo {
            EXIT_IF(!value);
            return {value->Handle(), 0, value->Size()};
        };
        buffers.push_back(stage.lod ? view(cache.GetLodStatsBuffer()) : vk::DescriptorBufferInfo {});
        buffers.push_back(stage.bda ? view(cache.GetBdaPageTableBuffer()) : vk::DescriptorBufferInfo {});
        buffers.push_back(stage.fault ? view(cache.GetFaultBuffer()) : vk::DescriptorBufferInfo {});
        for (size_t j = 0; j < input.images.size(); ++j)
            for (uint32_t mip = 0; mip < stage.image_mips[j]; ++mip)
                images.push_back(MakeImageInfo(input.images[j], mip));
        for (const auto sampler : input.samplers) {
            EXIT_IF(!sampler);
            images.emplace_back(sampler, nullptr, vk::ImageLayout::eUndefined);
        }
    }
    EXIT_IF(!plan.writes.empty() && !pipeline.descriptor_set_layout);
    const LocalDescriptorPacket packet {&plan, command, pipeline.pipeline_layout, point};
    LocalVulkanRecording::Segment segments[5] {
        {&packet, sizeof(packet)}, {buffers.data(), buffers.size() * sizeof(buffers[0])},
        {images.data(), images.size() * sizeof(images[0])}
    };
    for (size_t i = 0; i < inputs.size(); ++i)
        segments[3 + i] = {inputs[i]->shader_data.data(), inputs[i]->shader_data.size() * sizeof(uint32_t)};
    const std::span data {segments, inputs.size() + 3};
    if (!LocalVulkanRecording::EnqueuePacket(ReplayLocalDescriptors, data, pipeline.local_descriptor_plan))
        LocalVulkanRecording::ReplayInline(ReplayLocalDescriptors, data);
    return true;
}
