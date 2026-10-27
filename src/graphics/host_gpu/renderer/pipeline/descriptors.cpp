#include "common/renderStageProfile.h"
#include "common/readbackProfile.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/hostMemory.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <bit>
#include <optional>
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"
#include "kernel/memory.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <fmt/format.h>
#include <limits>
#include <span>
#include <vector>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace Libs::Graphics {

namespace {

using BindingKind = ShaderRecompiler::IR::DescriptorBindingKind;

} // namespace

vk::DescriptorType NativeDescriptorType(BindingKind kind) {
	const auto image_class = ShaderRecompiler::IR::ImageBindingResourceClass(kind);
	if (image_class == ShaderRecompiler::IR::ImageResourceClass::Sampled) {
		return vk::DescriptorType::eSampledImage;
	}
	if (image_class == ShaderRecompiler::IR::ImageResourceClass::Storage) {
		return vk::DescriptorType::eStorageImage;
	}
	switch (kind) {
		case BindingKind::Samplers: return vk::DescriptorType::eSampler;
		case BindingKind::Buffers:
		case BindingKind::Gds:
		case BindingKind::BdaPagetable:
		case BindingKind::FaultBuffer:
		case BindingKind::FlattenedSrt:
		case BindingKind::ShaderData:
		case BindingKind::SharedMemory: return vk::DescriptorType::eStorageBuffer;
		case BindingKind::Count: EXIT("invalid native descriptor binding kind");
	}
	EXIT("invalid native descriptor binding kind");
}

vk::DescriptorImageInfo MakeImageInfo(const TextureBinding& texture, uint32_t element) {
	if (!texture.image_id) {
		EXIT_IF(texture.image_view != nullptr || !texture.mip_views.empty());
		return {nullptr, nullptr, vk::ImageLayout::eGeneral};
	}
	vk::ImageView view = nullptr;
	if (texture.mip_views.empty()) {
		if (element == 0u) {
			view = texture.image_view;
		}
	} else if (element < texture.mip_views.size()) {
		view = texture.mip_views[element];
	}
	EXIT_IF(view == nullptr || texture.layout == vk::ImageLayout::eUndefined);
	return {nullptr, view, texture.layout};
}

static Prospero::ImageType TextureType(const ShaderTextureResource& descriptor) {
	const auto type = descriptor.Type();
	return type == Prospero::ImageType::kCube ? Prospero::ImageType::kColor2DArray : type;
}

static Prospero::ImageType TextureBaseType(Prospero::ImageType type) {
	switch (type) {
		case Prospero::ImageType::kColor1DArray: return Prospero::ImageType::kColor1D;
		case Prospero::ImageType::kColor2DArray:
		case Prospero::ImageType::kColor2DMsaa:
		case Prospero::ImageType::kColor2DMsaaArray: return Prospero::ImageType::kColor2D;
		default: return type;
	}
}

static bool IsMultisampledTexture(Prospero::ImageType type) {
	return type == Prospero::ImageType::kColor2DMsaa ||
	       type == Prospero::ImageType::kColor2DMsaaArray;
}

template <bool Profile>
static vk::DescriptorBufferInfo
NativeStorageBuffer(RenderContext& context, const PreparedBindings::BufferSource& source,
                    const ShaderRecompiler::IR::BufferResource& resource, uint32_t& buffer_offset) {
	using namespace Common::RenderStageProfile;
	if constexpr (Profile) ++Detail::state.buffer_rebinds.descriptors;
	buffer_offset = 0;

	const auto& [address, size, id] = source;
	if (address == 0 || size == 0) {
		if constexpr (Profile) ++Detail::state.buffer_rebinds.null_descriptors;
		return {context.GetBufferCache().GetBuffer(NULL_BUFFER_ID).Handle(), 0, 16};
	}
	const auto& graphics  = context.GetGraphics();
	const auto  alignment = graphics.StorageMinAlignment();
	if (size > graphics.GetPhysicalDeviceProperties().limits.maxStorageBufferRange) {
		EXIT("storage buffer range is unsupported\n");
	}
	auto [buffer, offset] = [&] {
		[[maybe_unused]] ProgramScope<Profile> profile {ProgramStage::RebindObtain};
		return context.GetBufferCache().ObtainBuffer<Profile>(address, size, resource.written,
		    resource.formatted || (resource.read && Config::SyncRawImageBuffersEnabled()), id,
		    resource.written ? Common::BufferUploadProfile::Use::ShaderWrite :
		    resource.scalar ? Common::BufferUploadProfile::Use::ShaderScalarRead :
		    resource.formatted ? Common::BufferUploadProfile::Use::ShaderFormattedRead : Common::BufferUploadProfile::Use::ShaderRead);
	}();
	const auto result = [&] {
		[[maybe_unused]] ProgramScope<Profile> profile {ProgramStage::RebindDescriptors};
		const auto aligned_offset = Common::AlignDown(offset, alignment);
		const auto adjustment     = offset - aligned_offset;
		const auto max_range      = graphics.GetPhysicalDeviceProperties().limits.maxStorageBufferRange;
		if (adjustment >= 256 || size > max_range - adjustment) {
			EXIT("storage buffer offset adjustment is unsupported\n");
		}
		buffer_offset = static_cast<uint32_t>(adjustment);
		return vk::DescriptorBufferInfo {buffer->Handle(), aligned_offset, size + adjustment};
	}();
	if (resource.written) {
		[[maybe_unused]] ProgramScope<Profile> profile {ProgramStage::RebindInvalidate};
		context.GetTextureCache().InvalidateMemoryFromGPU<Profile>(address, size);
	}
	return result;
}

// Reserved and unsupported T# fields must be zero; perf_mod only scales sampler state.
static bool IsCanonicalTextureEncoding(const ShaderTextureResource& descriptor, bool r128) {
	constexpr uint32_t field1_reserved_mask = 0x200fff00u;
	constexpr uint32_t field2_reserved_mask = 0xf0003000u;
	const uint32_t     field3_expected = descriptor.DstSelXYZW() |
	                                     (static_cast<uint32_t>(descriptor.BaseLevel()) << 12u) |
	                                     (static_cast<uint32_t>(descriptor.LastLevel()) << 16u) |
	                                     (static_cast<uint32_t>(descriptor.TileMode()) << 20u) |
	                                     (static_cast<uint32_t>(descriptor.Type()) << 28u);
	const uint32_t     field4_expected = descriptor.Depth() | (descriptor.BaseArray5() << 16u);
	const uint32_t     field5_expected = (static_cast<uint32_t>(descriptor.PerfMod5()) << 20u) |
	                                     (static_cast<uint32_t>(descriptor.MaxMip()) << 4u);
	if ((descriptor.fields[1] & field1_reserved_mask) != 0 ||
	    (descriptor.fields[2] & field2_reserved_mask) != 0 || descriptor.fields[3] != field3_expected) {
		return false;
	}
	if (r128) {
		return descriptor.fields[4] == 0 && descriptor.fields[5] == 0 && descriptor.fields[6] == 0 &&
		       descriptor.fields[7] == 0;
	}
	return descriptor.fields[4] == field4_expected && descriptor.fields[5] == field5_expected;
}

bool IsSupportedDepthTextureEncoding(const ShaderTextureResource& descriptor, bool r128) {
	if (!IsCanonicalTextureEncoding(descriptor, r128)) {
		return false;
	}
	if (r128) {
		return true;
	}
	if (descriptor.MsaaDepth() && !IsMultisampledTexture(descriptor.Type())) {
		return false;
	}
	const auto metadata_control = descriptor.fields[6] & 0x00ffffffu;
	if (metadata_control == 0) {
		return true;
	}
	constexpr uint32_t htile_control = 0x00280000u;
	const uint32_t expected_control  = htile_control | (descriptor.MsaaDepth() ? (1u << 10u) : 0u);
	const auto     metadata_addr     = descriptor.MetaAddr() << 8u;
	return metadata_control == expected_control && GuestRange {metadata_addr, 1}.Valid() &&
	       (metadata_addr & 0x7fffu) == 0 &&
	       descriptor.TileMode() == Prospero::TileMode::kDepth;
}

static void ValidateSampledDepthBinding(const ShaderRecompiler::IR::ImageResource& resource,
                                        const ShaderTextureResource& descriptor, const Image& image,
                                        vk::Format view_format, uint64_t size) {
	const bool resource_ok = IsSupportedSampledDepthResource(resource);
	// Comparison sampling changes the host format, not the guest metadata encoding.
	const bool encoding_ok = descriptor.TileMode() != Prospero::TileMode::kDepth ||
	                         IsSupportedDepthTextureEncoding(descriptor, resource.r128);
	const bool view_ok =
	    IsSupportedSampledDepthView(image.info.pixel_format, view_format, descriptor.DstSelXYZW());
	if (resource_ok && encoding_ok && view_ok) {
		return;
	}
	const auto descriptor_pitch =
	    TileGetTexturePitch(descriptor.Format(), static_cast<uint32_t>(descriptor.Width5()) + 1u,
	                        descriptor.TileMode());
	EXIT("unsupported sampled depth image: resource=%d encoding=%d view=%d "
	     "class=%u numeric=%u dimension=%u mip_mode=%u read=%d written=%d atomic=%d compare=%d "
	     "guest_format=%u swizzle=0x%03x image_format=%d view_format=%d image_layers=%u "
	     "descriptor_type=%u base_array=%u depth=%u descriptor_pitch=%u target_pitch=%u "
	     "addr=0x%016" PRIx64 " size=0x%016" PRIx64
	     " dwords=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
	     resource_ok, encoding_ok, view_ok,
	     static_cast<uint32_t>(resource.resource_class),
	     static_cast<uint32_t>(resource.numeric_class), static_cast<uint32_t>(resource.dimension),
	     static_cast<uint32_t>(resource.mip_mode), resource.read, resource.written, resource.atomic,
	     resource.depth_compare, static_cast<uint32_t>(descriptor.Format()),
	     descriptor.DstSelXYZW(), static_cast<int>(image.info.pixel_format),
	     static_cast<int>(view_format), image.info.resources.layers,
	     static_cast<uint32_t>(descriptor.Type()), descriptor.BaseArray5(), descriptor.Depth(),
	     descriptor_pitch, image.info.pitch, descriptor.Base40(), size, descriptor.fields[0],
	     descriptor.fields[1], descriptor.fields[2], descriptor.fields[3], descriptor.fields[4],
	     descriptor.fields[5], descriptor.fields[6], descriptor.fields[7]);
}

static bool IsSupportedStorageTextureDescriptor(const ShaderRecompiler::IR::ImageResource& resource,
                                                const ShaderTextureResource& descriptor) {
	const auto tile              = descriptor.TileMode();
	const bool is_color_1d       = descriptor.Type() == Prospero::ImageType::kColor1D;
	const bool is_color_1d_array = descriptor.Type() == Prospero::ImageType::kColor1DArray;
	const bool valid_1d_slice =
	    (is_color_1d && descriptor.Depth() == 0 && descriptor.BaseArray5() == 0) ||
	    (is_color_1d_array && descriptor.BaseArray5() <= descriptor.Depth());
	const bool is_1d = resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim1D &&
	                   descriptor.Height5() == 0 && valid_1d_slice;
	const bool is_1d_array =
	    resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim1DArray &&
	    is_color_1d_array && descriptor.Height5() == 0 &&
	    descriptor.BaseArray5() <= descriptor.Depth();
	const bool is_color_2d       = descriptor.Type() == Prospero::ImageType::kColor2D;
	const bool is_color_2d_array = descriptor.Type() == Prospero::ImageType::kColor2DArray;
	const bool valid_2d_slice =
	    (is_color_2d && descriptor.Depth() == 0 && descriptor.BaseArray5() == 0) ||
	    (is_color_2d_array && descriptor.BaseArray5() <= descriptor.Depth());
	const bool is_2d =
	    resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2D && valid_2d_slice;
	// Storage cube coordinates address individual faces, including partial cube views.
	const bool is_cube = resource.cube && descriptor.Type() == Prospero::ImageType::kCube &&
	                     descriptor.Width5() == descriptor.Height5() &&
	                     descriptor.BaseArray5() <= descriptor.Depth();
	const bool is_2d_array =
	    resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2DArray &&
	    ((!resource.cube && is_color_2d_array && descriptor.BaseArray5() <= descriptor.Depth()) ||
	     is_cube);
	const bool is_3d = resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim3D &&
	                   descriptor.Type() == Prospero::ImageType::kColor3D &&
	                   descriptor.BaseArray5() == 0;
	const bool is_2d_msaa =
	    resource.dimension == ShaderRecompiler::Decoder::ImageDimension::Dim2DMsaa &&
	    descriptor.Type() == Prospero::ImageType::kColor2DMsaa && descriptor.Depth() == 0 &&
	    descriptor.BaseArray5() == 0;
	TileTextureBlockLayout tile_layout {};
	bool                   supported_tile = false;
	switch (tile) {
		case Prospero::TileMode::kLinear: supported_tile = true; break;
		case Prospero::TileMode::kDepth:
			supported_tile =
			    !resource.read && !Prospero::IsFmaskTextureFormat(descriptor.Format()) &&
			    (is_2d || is_2d_array) &&
			    TileGetTextureBlockLayout(descriptor.Format(), tile, false, tile_layout);
			break;
		case Prospero::TileMode::kStandard256B:
			supported_tile =
			    (is_2d || is_2d_array) &&
			    TileGetTextureBlockLayout(descriptor.Format(), tile, false, tile_layout);
			break;
		case Prospero::TileMode::kStandard4KB:
		case Prospero::TileMode::kStandard64KB:
			supported_tile =
			    TileGetTextureBlockLayout(descriptor.Format(), tile, is_3d, tile_layout);
			break;
		case Prospero::TileMode::kRenderTarget:
			supported_tile =
			    TileGetTextureBlockLayout(descriptor.Format(), tile, false, tile_layout);
			break;
		default: break;
	}
	const auto swizzle = descriptor.DstSelXYZW();
	const bool supported_swizzle =
	    IsValidImageSwizzle(swizzle) &&
	    (swizzle == DstSel(4, 5, 6, 7) || !resource.read || resource.atomic);
	return (is_1d || is_1d_array || is_2d || is_2d_array || is_3d || is_2d_msaa) &&
	       supported_tile && descriptor.BaseLevel() <= descriptor.LastLevel() &&
	       descriptor.MinLod() == 0 && supported_swizzle && descriptor.BCSwizzle() == 0 &&
	       !descriptor.MsaaDepth();
}

void ValidateStorageTexture(const ShaderRecompiler::IR::ImageResource& resource,
                            const ShaderTextureResource& descriptor, uint64_t size) {
	const auto format        = descriptor.Format();
	const bool resource_ok   = IsSupportedStorageImageResource(resource);
	const bool descriptor_ok = IsSupportedStorageTextureDescriptor(resource, descriptor);
	const bool encoding_ok   = IsCanonicalTextureEncoding(descriptor, resource.r128);
	const bool uint_resource    = resource.numeric_class == Prospero::TextureNumericClass::Uint;
	const bool raw_sint_storage = uint_resource &&
	    ShaderRecompiler::IR::RawSignedStorageFormat(resource, format) != Prospero::BufferFormat::kInvalid;
	const auto numeric_class =
	    Prospero::SampledTextureNumericClass(Prospero::RemapTextureFormat(format));
	const bool raw_float_atomic = format == Prospero::BufferFormat::k32Float && uint_resource &&
	                              resource.atomic && !resource.atomic64;
	const bool format_ok =
	    raw_sint_storage || raw_float_atomic ||
	    (numeric_class != Prospero::TextureNumericClass::Unsupported &&
	     numeric_class != Prospero::TextureNumericClass::Sint &&
	     uint_resource == (numeric_class == Prospero::TextureNumericClass::Uint) &&
	     (!resource.atomic || format == (resource.atomic64 ? Prospero::BufferFormat::k32_32UInt
	                                                       : Prospero::BufferFormat::k32UInt)));
	if (resource_ok && descriptor_ok && encoding_ok && format_ok && size != 0) {
		return;
	}
	EXIT("unsupported storage texture: resource=%d descriptor=%d encoding=%d format=%d "
	     "class=%u numeric=%u dimension=%u mip_mode=%u atomic=%d compare=%d "
	     "base_level=%u last_level=%u max_mip=%u min_lod=%u base_array=%u bc=%u msaa=%d "
	     "depth_tile_bpe=%u swizzle_ok=%d "
	     "addr=0x%016" PRIx64 " size=0x%016" PRIx64
	     " extent=%ux%ux%u type=%u format=%u tile=%u swizzle=0x%03x read=%d written=%d "
	     "dwords=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
	     resource_ok, descriptor_ok, encoding_ok, format_ok,
	     static_cast<uint32_t>(resource.resource_class),
	     static_cast<uint32_t>(resource.numeric_class), static_cast<uint32_t>(resource.dimension),
	     static_cast<uint32_t>(resource.mip_mode), resource.atomic, resource.depth_compare,
	     descriptor.BaseLevel(), descriptor.LastLevel(), descriptor.MaxMip(), descriptor.MinLod(),
	     descriptor.BaseArray5(), descriptor.BCSwizzle(), descriptor.MsaaDepth(),
	     Prospero::RenderTargetBytesPerElement(format),
	     IsValidImageSwizzle(descriptor.DstSelXYZW()), descriptor.Base40(), size,
	     static_cast<uint32_t>(descriptor.Width5()) + 1u,
	     static_cast<uint32_t>(descriptor.Height5()) + 1u,
	     static_cast<uint32_t>(descriptor.Depth()) + 1u, static_cast<uint32_t>(descriptor.Type()),
	     static_cast<uint32_t>(format), static_cast<uint32_t>(descriptor.TileMode()),
	     descriptor.DstSelXYZW(), resource.read, resource.written, descriptor.fields[0],
	     descriptor.fields[1], descriptor.fields[2], descriptor.fields[3], descriptor.fields[4],
	     descriptor.fields[5], descriptor.fields[6], descriptor.fields[7]);
}

static ImageViewInfo TextureViewInfo(const ShaderRecompiler::IR::ImageResource& resource,
                                     const ShaderTextureResource& descriptor, vk::Format format,
                                     const SurfaceFormatInfo& surface_format, bool storage,
                                     uint32_t view_levels, uint32_t image_layers) {
	ImageViewInfo view {};
	view.format      = format;
	view.aspect      = vk::ImageAspectFlagBits::eColor;
	view.base_level  = descriptor.BaseLevel();
	view.level_count = view_levels;
	if (descriptor.MinLod() > descriptor.LastLevel() * 256u) {
		EXIT("texture minimum LOD exceeds last mip level: min_lod=%u last_level=%u\n",
		     descriptor.MinLod(), descriptor.LastLevel());
	}
	const auto base_lod = view.base_level * 256u;
	if (descriptor.MinLod() > base_lod) {
		view.min_lod = descriptor.MinLod() - base_lod;
	}
	view.usage = storage ? vk::ImageUsageFlagBits::eStorage : vk::ImageUsageFlagBits::eSampled;
	view.mapping =
	    storage || resource.conversion_format != Prospero::BufferFormat::kInvalid
	        ? vk::ComponentMapping {}
	        : TextureGetComponentMapping(descriptor.DstSelXYZW(), surface_format.host_to_storage);
	switch (resource.dimension) {
		case ShaderRecompiler::Decoder::ImageDimension::Dim1D:
			view.type       = vk::ImageViewType::e1D;
			view.base_layer = descriptor.BaseArray5();
			if (view.base_layer >= image_layers) {
				EXIT("texture base layer is out of bounds\n");
			}
			view.layer_count = 1;
			break;
		case ShaderRecompiler::Decoder::ImageDimension::Dim1DArray:
			view.type       = vk::ImageViewType::e1DArray;
			view.base_layer = descriptor.BaseArray5();
			if (view.base_layer >= image_layers) {
				EXIT("texture array base layer is out of bounds\n");
			}
			view.layer_count = image_layers - view.base_layer;
			break;
		case ShaderRecompiler::Decoder::ImageDimension::Dim3D:
			view.type        = vk::ImageViewType::e3D;
			view.base_layer  = 0;
			view.layer_count = 1;
			break;
		case ShaderRecompiler::Decoder::ImageDimension::Dim2DArray:
		case ShaderRecompiler::Decoder::ImageDimension::Dim2DMsaaArray:
			view.type       = vk::ImageViewType::e2DArray;
			view.base_layer = descriptor.BaseArray5();
			if (view.base_layer >= image_layers) {
				EXIT("texture array base layer is out of bounds\n");
			}
			view.layer_count = image_layers - view.base_layer;
			break;
		case ShaderRecompiler::Decoder::ImageDimension::Dim2D:
		case ShaderRecompiler::Decoder::ImageDimension::Dim2DMsaa:
			view.type       = vk::ImageViewType::e2D;
			view.base_layer = descriptor.BaseArray5();
			if (view.base_layer >= image_layers) {
				EXIT("texture base layer is out of bounds\n");
			}
			view.layer_count = 1;
			break;
		default: EXIT("unsupported texture view dimension\n");
	}
	return view;
}

static bool ResolveTextureMipView(ImageInfo& info, uint32_t physical_levels, bool metadata,
                                  uint32_t view_levels, uint32_t& base_level) {
	if (!info.IsTiled()) {
		return false;
	}
	ImageInfo physical        = info;
	physical.resources.levels = physical_levels;
	physical.UpdateSize();
	info.UpdateSize();
	const auto same_mip = [&](uint32_t target, uint32_t source) {
		const auto& left  = physical.mip_layout[target];
		const auto& right = info.mip_layout[source];
		return left.offset == right.offset && left.size == right.size &&
		       left.pitch == right.pitch && left.height == right.height &&
		       left.tail_x == right.tail_x && left.tail_y == right.tail_y &&
		       left.surface_z == right.surface_z &&
		       physical.MipExtent(target) == info.MipExtent(source);
	};
	if (physical.first_tail_level == info.first_tail_level &&
	    physical.tiled_slice_stride == info.tiled_slice_stride &&
	    physical.data.size == info.data.size) {
		bool matches = true;
		for (uint32_t level = 0; level < physical_levels; ++level) {
			matches &= same_mip(level, level);
		}
		if (matches) {
			return true;
		}
	}
	if (metadata ||
	    (info.TransferLayers() > 1 && physical.tiled_slice_stride != info.tiled_slice_stride)) {
		return false;
	}
	// T# addresses the last mip. A view can select the same stored subresources
	// with different mip indices; inaccessible mips need no host representation.
	for (uint32_t base = 0; base + view_levels <= physical_levels; ++base) {
		bool matches = true;
		for (uint32_t i = 0; i < view_levels; ++i) {
			const auto source = base_level + i;
			const auto target = base + i;
			if (!same_mip(target, source) ||
			    (target >= physical.first_tail_level) != (source >= info.first_tail_level)) {
				matches = false;
				break;
			}
		}
		if (matches) {
			info       = std::move(physical);
			base_level = base;
			return true;
		}
	}
	return false;
}

// Called at the original point between size calculation and view construction.
// Keeping the callback here preserves unmapped-heap early returns and EXIT order.
template <typename Validate>
static bool BuildTextureDescriptionImpl(
    const ShaderRecompiler::IR::ImageResource& resource, const ShaderTextureResource& descriptor,
    BuiltImageDescription& built, Validate&& validate) {
	const bool storage = resource.written;
	const auto address         = descriptor.Base40();
	const auto width           = static_cast<uint32_t>(descriptor.Width5()) + 1u;
	const auto height          = static_cast<uint32_t>(descriptor.Height5()) + 1u;
	const auto base_level      = descriptor.BaseLevel();
	const auto last_level      = descriptor.LastLevel();
	const auto type            = TextureType(descriptor);
	const bool multisampled    = IsMultisampledTexture(type);
	const auto max_mip         = resource.r128 ? last_level : descriptor.MaxMip();
	const auto physical_levels = multisampled ? 1u : static_cast<uint32_t>(max_mip) + 1u;
	// IMAGE_STORE addresses BASE_LEVEL; only IMAGE_STORE_MIP selects other view mips.
	const bool single_storage_mip =
	    storage && resource.mip_mode != ShaderRecompiler::IR::ImageMipMode::Dynamic;
	const auto view_levels = multisampled || single_storage_mip
	                             ? 1u
	                             : static_cast<uint32_t>(last_level - base_level) + 1u;
	auto levels =
	    multisampled ? 1u : std::max(physical_levels, base_level + view_levels);
	const auto tile       = descriptor.TileMode();
	const bool depth_tile = tile == Prospero::TileMode::kDepth;
	const bool msaa_tile  = depth_tile || tile == Prospero::TileMode::kRenderTarget;
	const bool msaa_array = type == Prospero::ImageType::kColor2DMsaaArray;
	if ((!multisampled && base_level > last_level) ||
	    (multisampled &&
	     (base_level != 0 || last_level == 0 || last_level > 3 || max_mip != last_level ||
	      !msaa_tile || (descriptor.MsaaDepth() && !depth_tile) ||
	      (!msaa_array && (descriptor.Depth() != 0 || descriptor.BaseArray5() != 0))))) {
		EXIT("unsupported texture mip view: base=%u last=%u levels=%u max=%u type=%u tile=%u "
		     "class=%u numeric=%u dimension=%u mip_mode=%u read=%d written=%d "
		     "dwords=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
		     base_level, last_level, levels, descriptor.MaxMip(),
		     static_cast<uint32_t>(descriptor.Type()), static_cast<uint32_t>(tile),
		     static_cast<uint32_t>(resource.resource_class),
		     static_cast<uint32_t>(resource.numeric_class),
		     static_cast<uint32_t>(resource.dimension), static_cast<uint32_t>(resource.mip_mode),
		     resource.read, resource.written, descriptor.fields[0], descriptor.fields[1],
		     descriptor.fields[2], descriptor.fields[3], descriptor.fields[4], descriptor.fields[5],
		     descriptor.fields[6], descriptor.fields[7]);
	}
	const auto samples = multisampled ? 1u << last_level : 1u;
	const auto depth          = static_cast<uint32_t>(descriptor.Depth()) + 1u;
	const auto format         = ShaderRecompiler::IR::ReadsDepthBits(resource)
	                                ? Prospero::BufferFormat::k16UNorm
	                                : descriptor.Format();
	const auto surface_format = TextureGetSurfaceFormatInfo(format);
	const bool shader_conversion =
	    surface_format.conversion_format != Prospero::BufferFormat::kInvalid;
	const bool sampled_numeric_class =
	    storage || resource.numeric_class ==
	                   Prospero::SampledTextureNumericClass(Prospero::RemapTextureFormat(format));
	if (!storage && resource.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled &&
	    !sampled_numeric_class) {
		EXIT("sampled image numeric class mismatch: numeric=%u format=%u addr=0x%016" PRIx64 "\n",
		     static_cast<uint32_t>(resource.numeric_class), static_cast<uint32_t>(format), address);
	}

	const bool    volume       = type == Prospero::ImageType::kColor3D;
	const bool    layered      = type == Prospero::ImageType::kColor1DArray ||
	                             type == Prospero::ImageType::kColor2DArray ||
	                             type == Prospero::ImageType::kColor2DMsaaArray;
	const auto    image_layers = layered ? depth : 1u;
	auto          view_base    = static_cast<uint32_t>(base_level);
	auto pixel_format = surface_format.vk_format;
	if (resource.depth_compare) {
		if (const auto* depth_format = FindGuestDepthFormatPolicy(format)) {
			pixel_format = depth_format->depth_attachment_format;
		}
	}
	const auto raw_sint_format = ShaderRecompiler::IR::RawSignedStorageFormat(resource, format);
	const auto storage_view_format = resource.atomic64 ? vk::Format::eR64Uint
	                                 : storage && resource.atomic ? vk::Format::eR32Uint
	                                 : raw_sint_format != Prospero::BufferFormat::kInvalid
	                                     ? VulkanFormat(raw_sint_format)
	                                     : SrgbStorageViewFormat(pixel_format);
	const auto view_format         = storage && storage_view_format != vk::Format::eUndefined
	                                     ? storage_view_format
	                                     : pixel_format;
	const auto block_bytes         = Prospero::BlockCompressedBytesPerBlock(format);
	auto& desc = built.desc;
	desc.info.data         = {address, 0};
	desc.info.pixel_format = pixel_format;
	desc.info.guest_format = format;
	desc.info.type         = TextureBaseType(type);
	desc.info.extent       = {width, height, volume ? depth : 1u};
	desc.info.resources    = {levels, image_layers};
	desc.info.bytes_per_block =
	    block_bytes != 0 ? block_bytes : Prospero::NumBytesPerElement(format);
	desc.info.samples   = samples;
	desc.info.tile_mode = tile;
	if (!resource.r128 && descriptor.MetaCompress() && tile != Prospero::TileMode::kDepth) {
		desc.info.metadata.kind = ImageMetadataKind::Dcc;
	}
	if (levels > physical_levels) {
		if (!ResolveTextureMipView(desc.info, physical_levels,
		                           !resource.r128 && descriptor.MetaCompress(), view_levels,
		                           view_base)) {
			EXIT("unsupported texture mip view changes physical layout: base=%u last=%u max=%u "
			     "extent=%ux%ux%u tile=%u\n",
			     base_level, last_level, max_mip, width, height, depth,
			     static_cast<uint32_t>(tile));
		}
	} else {
		desc.info.UpdateSize();
	}
	if (!validate(desc.info)) return false;

	if (!resource.r128 && descriptor.MetaCompress() && tile != Prospero::TileMode::kDepth) {
		TileSizeAlign metadata_size {};
		(void)TileGetDccSize(width, height, volume ? depth : image_layers,
		                     desc.info.bytes_per_block, physical_levels, tile, metadata_size,
		                     std::countr_zero(samples));
		desc.info.metadata.range         = {descriptor.MetaAddr() << 8u, metadata_size.size};
		desc.info.metadata.dcc_alpha_msb = descriptor.DccAlphaPos();
	}
	desc.view_info = TextureViewInfo(resource, descriptor, view_format, surface_format, storage,
	                                 view_levels, desc.info.resources.layers);
	desc.view_info.base_level = view_base;
	desc.type = storage ? TextureCache::BindingType::Storage : TextureCache::BindingType::Texture;

	built.shader_conversion = shader_conversion;
	built.pixel_format = pixel_format;
	built.view_format = view_format;
	return true;
}

BuiltImageDescription BuildTextureDescription(const ShaderRecompiler::IR::ImageResource& resource,
                                               const ShaderTextureResource& descriptor) {
	EXIT_IF(resource.heap || descriptor.IsNull());
	if (resource.written) ValidateStorageImageResource(resource);
	BuiltImageDescription built {};
	BuildTextureDescriptionImpl(resource, descriptor, built, [&](const ImageInfo& info) {
		const auto alignment = info.DataAlignment();
		EXIT_NOT_IMPLEMENTED(info.data.size == 0 || alignment == 0 ||
		                     (descriptor.Base40() & (static_cast<uint64_t>(alignment) - 1u)) != 0);
		if (resource.written) ValidateStorageTexture(resource, descriptor, info.data.size);
		return true;
	});
	return built;
}

TextureBinding RenderExecutor::ResolveTexture(const ShaderRecompiler::IR::ImageResource& resource,
                                              const ShaderRecompiler::IR::DescriptorValue& value) {
	// Heap/rebind callers do not have the immutable program/resource association.
	return ResolveTextureImpl<false>(resource, value, 0, 0);
}

template <bool Profile>
TextureBinding RenderExecutor::ResolveTextureImpl(const ShaderRecompiler::IR::ImageResource&   resource,
                                              const ShaderRecompiler::IR::DescriptorValue& value,
                                              uint64_t description_program,
                                              uint32_t description_resource) {
	using Stage = Common::RenderStageProfile::Stage;
	using Scope = Common::RenderStageProfile::ScopeFor<Profile>;
	{
		[[maybe_unused]] Scope validation {Stage::ImageValidate};
		if (resource.atomic64 && !m_context.GetGraphics().shader_image_int64_atomics_enabled) {
			EXIT("64-bit image atomics require shaderImageInt64Atomics\n");
		}
	}
	auto descriptor = DecodeNativeDescriptor<ShaderTextureResource>(value);
	const bool storage = resource.written;
	if (storage) {
		[[maybe_unused]] Scope validation {Stage::ImageValidate};
		ValidateStorageImageResource(resource);
	}

	auto& texture_cache = m_context.GetTextureCache();
	if (descriptor.IsNull()) {
		return {.layout = vk::ImageLayout::eGeneral};
	}

	BuiltImageDescription built {};
	{
		[[maybe_unused]] Scope build_profile {Stage::ImageDescBuild};
		auto validate = [&](const ImageInfo& info) {
			[[maybe_unused]] Scope validation {Stage::ImageValidate};
			const auto alignment = info.DataAlignment();
			EXIT_NOT_IMPLEMENTED(info.data.size == 0 || alignment == 0 ||
			                     (descriptor.Base40() & (static_cast<uint64_t>(alignment) - 1u)) != 0);
			// Image heaps keep the descriptors of unmapped textures, which PS5 never samples.
			if (resource.heap && !m_context.IsMapped(descriptor.Base40(), info.data.size)) {
				return false;
			}
			if (storage) {
				ValidateStorageTexture(resource, descriptor, info.data.size);
			}
			return true;
		};

		const BuiltImageDescription* cached = nullptr;
		if (description_program != 0) {
			if (!m_image_descriptions) m_image_descriptions = std::make_unique<ImageDescriptionCache>();
			cached = m_image_descriptions->Find(description_program, description_resource, value.dwords);
		}
		if (cached != nullptr) {
			built = *cached;
			if (!validate(built.desc.info)) return {.layout = vk::ImageLayout::eGeneral};
			Common::SubmitProfile::AddExtra(Common::SubmitProfile::Extra::ImageDescReuseHits);
		} else {
			if (!BuildTextureDescriptionImpl(resource, descriptor, built, validate))
				return {.layout = vk::ImageLayout::eGeneral};
			if (description_program != 0) {
				using namespace Common::SubmitProfile;
				AddExtra(Extra::ImageDescReuseMisses);
				if (m_image_descriptions->Store(description_program, description_resource, value.dwords, built))
					AddExtra(Extra::ImageDescHistoryCapacityResets);
			}
		}
	}
	auto& desc = built.desc;
	const auto shader_conversion = built.shader_conversion;
	const auto pixel_format = built.pixel_format;
	const auto view_format = built.view_format;
	ImageId id;
	{
		[[maybe_unused]] Scope find {Stage::ImageFind};
		id = texture_cache.FindImage(desc, shader_conversion);
	}
	[[maybe_unused]] Scope validation {Stage::ImageValidate};
	auto*      image               = &texture_cache.GetImage(id);
	const bool stencil_association = static_cast<bool>(image->depth_id);
	if (stencil_association) {
		id    = image->depth_id;
		image = &texture_cache.GetImage(id);
	} else if (image->info.IsDepth()) {
		if (storage) {
			EXIT("depth target cannot be bound as a storage image\n");
		}
		ValidateSampledDepthBinding(resource, descriptor, *image, pixel_format,
		                            desc.info.data.size);
	} else if (storage) {
		ValidateStorageColorView(image->info.pixel_format, view_format, descriptor.DstSelXYZW());
	} else {
		(void)SelectSampledColorView(image->info.pixel_format, pixel_format,
		                             descriptor.DstSelXYZW());
	}
	return {id, nullptr, std::move(desc)};
}

static vk::Sampler NativeSampler(RenderContext&                       context,
                                 const ShaderRecompiler::IR::CompiledShaderInfo& program,
                                 uint32_t index,
                                 const ShaderRecompiler::IR::DescriptorValue& value) {
	auto        descriptor = DecodeNativeDescriptor<ShaderSamplerResource>(value);
	const auto& sampler = program.info.samplers[index];
	if (!sampler.depth_compare) {
		descriptor.fields[0] &= ~(0x7u << 12u);
	}
	if (sampler.force_point_filtering) {
		descriptor.SetPointFiltering();
	}
	return context.GetSamplerCache().GetSampler(descriptor, sampler.integer_border);
}

static vk::DescriptorBufferInfo NativeUpload(RenderContext&            context,
                                             std::span<const uint32_t> data) {
	EXIT_IF(data.empty());
	auto& command_buffer = context.GetCommandScheduler().Current();
	EXIT_IF(command_buffer.IsInvalid());
	auto&      buffer = context.GetBufferCache().GetUtilityBuffer(MemoryUsage::Stream);
	const auto offset = buffer.Copy(data.data(), data.size_bytes(), 256);
	return {buffer.Handle(), offset, data.size_bytes()};
}

void RenderExecutor::BindImage(ImageId id, bool storage) {
	if (!id) {
		return;
	}
	auto& image = m_context.GetTextureCache().GetImage(id);
	if (image.binding.is_bound) {
		image.binding.force_general |= image.binding.shader_write != storage;
	}
	image.binding.is_bound = true;
	image.binding.shader_write |= storage;
	m_bound_images.push_back(id);
}

void RenderExecutor::BindRenderTarget(ImageId id) {
	auto& image             = m_context.GetTextureCache().GetImage(id);
	image.binding.is_target = true;
	m_bound_images.push_back(id);
}

void RenderExecutor::ResetBindings() {
	Common::RenderStageProfile::Scope profile {Common::RenderStageProfile::Stage::ResetBindings};
	for (const auto id: m_bound_images) {
		m_context.GetTextureCache().m_slot_images[id].binding = {};
	}
	m_bound_images.clear();
}

template <bool Profile>
void RenderExecutor::PrepareImages(const ShaderStageRuntime& runtime, PreparedBindings& prepared) {
	const auto& program = runtime.module->info;
	const auto& snapshot = *runtime.resources;
	[[maybe_unused]] Common::RenderStageProfile::ScopeFor<Profile> profile {Common::RenderStageProfile::Stage::PrepareImages};
	// Vulkan modules survive PipelineCache's vector moves and are destroyed only
	// with that cache. Its immutable resource metadata is part of this identity.
	const uint64_t description_program = runtime.module->module
	    ? std::bit_cast<uint64_t>(static_cast<VkShaderModule>(runtime.module->module)) : 0;
	for (uint32_t i = 0; i < program.info.images.size(); i++) {
		if (program.info.images[i].heap) {
			prepared.images[i] = {};
			continue;
		}
		auto binding = ResolveTextureImpl<Profile>(program.info.images[i], snapshot.images[i], description_program, i);
		{
			[[maybe_unused]] Common::RenderStageProfile::ScopeFor<Profile> bind {Common::RenderStageProfile::Stage::ImageBind};
			BindImage(binding.image_id, binding.desc.type == TextureCache::BindingType::Storage);
		}
		binding.mip_views.swap(prepared.images[i].mip_views);
		binding.mip_views.clear();
		prepared.images[i] = std::move(binding);
	}
}

void RenderExecutor::PrepareBindings(const ShaderStageRuntime& runtime,
                                     PreparedBindings& prepared) {
	Common::RenderStageProfile::Scope stage_profile {Common::RenderStageProfile::Stage::PrepareBindings};
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(!runtime);
	const auto& program  = runtime.module->info;
	const auto& snapshot = *runtime.resources;
	prepared.runtime = &runtime;
	prepared.gds = {nullptr, 0, VK_WHOLE_SIZE};
	prepared.flattened_srt = {};
	prepared.shader_data_buffer = {};
	prepared.shared_memory = {};
	prepared.images.resize(program.info.images.size());
	prepared.samplers.clear();
	prepared.shader_data.clear();
	// The disabled path contains no per-image profiling checks or scope objects.
	if (Common::RenderStageProfile::Detail::Enabled()) {
		PrepareImages<true>(runtime, prepared);
	} else {
		PrepareImages<false>(runtime, prepared);
	}
	{
		Common::RenderStageProfile::Scope profile {Common::RenderStageProfile::Stage::PrepareSamplers};
		prepared.samplers.reserve(program.info.samplers.size());
		for (uint32_t i = 0; i < program.info.samplers.size(); i++) {
			prepared.samplers.push_back(NativeSampler(
			    m_context, program, i, snapshot.samplers[program.info.samplers[i].snapshot_index]));
		}
	}
	{
		Common::RenderStageProfile::Scope profile {Common::RenderStageProfile::Stage::PrepareShaderData};
		prepared.shader_data.reserve(program.bindings.ShaderDataDwords());
		for (const auto reg: program.info.user_data_registers) {
			prepared.shader_data.push_back(snapshot.user_data[reg - program.user_data_base]);
		}
		prepared.shader_data.resize(program.bindings.ShaderDataDwords());
		if (program.bindings.descriptor_counts[static_cast<size_t>(BindingKind::Gds)] != 0) {
			prepared.gds.buffer = m_context.GetBufferCache().GetGdsBuffer()->Handle();
		}
	}
}

void RenderExecutor::FindBuffers(std::span<PreparedBindings* const> stages) {
	Common::RenderStageProfile::Scope stage_profile {Common::RenderStageProfile::Stage::FindBuffers};
	KYTY_PROFILER_FUNCTION();
	auto& cache = m_context.GetBufferCache();
	for (auto* stage: stages) {
		auto& prepared = *stage;
		EXIT_IF(prepared.runtime == nullptr || !*prepared.runtime);
		const auto& program  = prepared.runtime->module->info;
		const auto& snapshot = *prepared.runtime->resources;
		const auto& layout = program.bindings;
		prepared.buffer_sources.resize(layout.descriptor_counts[static_cast<size_t>(BindingKind::Buffers)]);
		if (layout.descriptor_counts[static_cast<size_t>(BindingKind::Buffers)] == 0) {
			continue;
		}
		for (uint32_t resource = 0; resource < program.info.buffers.size(); ++resource) {
			const auto& metadata = program.info.buffers[resource];
			if (metadata.descriptor_index == UINT32_MAX) continue;
			auto& source = prepared.buffer_sources[metadata.descriptor_index];
			const auto descriptor = DecodeNativeDescriptor<ShaderBufferResource>(snapshot.buffers[resource]);
			const auto address = descriptor.Base48();
			auto size = descriptor.GetSize();
			if (size != 0 && descriptor.Stride() == 0 && descriptor.OutOfBounds() == 2 &&
			    !metadata.scalar) {
				// OOB_SELECT=2 only tests NumRecords != 0; a zero-stride fetch
				// still needs its full payload when NumRecords is smaller than it.
				size = std::max(size, uint64_t {metadata.max_byte_extent});
			}
			if (address == 0 || size == 0) {
				source = {};
				continue;
			}
			if (descriptor.NumRecords() == UINT32_MAX) {
				if (program.info.buffers[resource].written) {
					// Sentinel write ranges end before tables captured by any bound stage.
					for (const auto* reader: stages) {
						for (const auto [read_address, read_size]: reader->runtime->resources->specialization_reads) {
							if (read_size == 0) continue;
							if (read_address > address) {
								size = std::min(size, read_address - address);
							} else if (address - read_address < read_size) {
								EXIT("scalar resource reads overlap a shader buffer write\n");
							}
						}
					}
				}
				//  PPSA15246
				const auto& graphics = m_context.GetGraphics();
				const auto  limit =
				    uint64_t {graphics.GetPhysicalDeviceProperties().limits.maxStorageBufferRange} -
				    (graphics.StorageMinAlignment() - 1);
				size = std::min({size, uint64_t {256} * 1024 * 1024, limit});
			}
			// Finite GPU descriptors can span sparse PRT holes; clamp only oversized/sentinel ranges.
			if (size >= (uint64_t {1} << 30u) || descriptor.NumRecords() == UINT32_MAX) {
				size = Libs::LibKernel::Memory::ClampRangeSize(address, size);
			}
			source = {address, size, cache.FindBuffer(address, size)};
		}
	}
}

void RenderExecutor::RebindBuffers(PreparedBindings& prepared) {
	Common::RenderStageProfile::Scope stage_profile {Common::RenderStageProfile::Stage::RebindBuffers};
	// One gate outside the resource loop; disabled instantiations contain no
	// detailed clocks, TLS counters or sampling state, including cache helpers.
	if (Common::RenderStageProfile::Detail::DetailedEnabled()) RebindBuffersImpl<true>(prepared);
	else RebindBuffersImpl<false>(prepared);
}

template <bool Profile>
void RenderExecutor::RebindBuffersImpl(PreparedBindings& prepared) {
	using namespace Common::RenderStageProfile;
	if constexpr (Profile) ++Detail::state.buffer_rebinds.calls;
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(prepared.runtime == nullptr || !*prepared.runtime);
	const auto& program   = prepared.runtime->module->info;
	const auto& snapshot  = *prepared.runtime->resources;
	const auto& layout    = program.bindings;
	EXIT_IF(prepared.buffer_sources.size() != layout.descriptor_counts[static_cast<size_t>(BindingKind::Buffers)]);

	{
		[[maybe_unused]] ProgramScope<Profile> profile {ProgramStage::RebindSetup};
		prepared.buffers.resize(layout.descriptor_counts[static_cast<size_t>(BindingKind::Buffers)]);
		EXIT_IF(prepared.shader_data.size() != layout.ShaderDataDwords());
		std::fill(prepared.shader_data.begin() + layout.memory_offset_dword,
		          prepared.shader_data.end(), 0);
	}
	auto pack_memory_offset = [&](uint32_t index, uint32_t offset) {
		const auto dword = layout.memory_offset_dword + index / 4u;
		const auto shift = (index % 4u) * 8u;
		prepared.shader_data[dword] |= offset << shift;
	};
	for (const auto& resource: program.info.buffers) {
		const auto index = resource.descriptor_index;
		if (index == UINT32_MAX) continue;
		uint32_t buffer_offset = 0;
		prepared.buffers[index] = NativeStorageBuffer<Profile>(m_context, prepared.buffer_sources[index],
		                                              resource, buffer_offset);
		pack_memory_offset(index, buffer_offset);
	}
	if (layout.descriptor_counts[static_cast<size_t>(BindingKind::FlattenedSrt)] != 0) {
		[[maybe_unused]] ProgramScope<Profile> profile {ProgramStage::RebindFlattenedUpload};
		if constexpr (Profile) {
			++Detail::state.buffer_rebinds.flattened_uploads;
			Detail::state.buffer_rebinds.flattened_bytes += snapshot.flattened_srt.size() * sizeof(uint32_t);
		}
		prepared.flattened_srt = NativeUpload(m_context, snapshot.flattened_srt);
	}
	if (layout.descriptor_counts[static_cast<size_t>(BindingKind::ShaderData)] != 0) {
		[[maybe_unused]] ProgramScope<Profile> profile {ProgramStage::RebindShaderUpload};
		if constexpr (Profile) {
			++Detail::state.buffer_rebinds.shader_uploads;
			Detail::state.buffer_rebinds.shader_bytes += prepared.shader_data.size() * sizeof(uint32_t);
		}
		prepared.shader_data_buffer = NativeUpload(m_context, prepared.shader_data);
	}
}

void RenderExecutor::RebindImages(PreparedBindings& prepared) {
	Common::RenderStageProfile::Scope stage_profile {Common::RenderStageProfile::Stage::RebindImages};
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(prepared.runtime == nullptr || !*prepared.runtime);
	const auto& program  = prepared.runtime->module->info;
	const auto& snapshot = *prepared.runtime->resources;
	auto&       images   = prepared.images;
	EXIT_IF(images.size() != program.info.images.size());
	auto& texture_cache = m_context.GetTextureCache();
	// Acquire each view before a later overlapping descriptor can replace its image.
	for (uint32_t i = 0; i < program.info.images.size(); i++) {
		if (!images[i].image_id) continue;
		auto& old_image = texture_cache.m_slot_images[images[i].image_id];
		if (!old_image.registered || old_image.binding.needs_rebind) {
			old_image.binding = {};
			images[i] = ResolveTexture(program.info.images[i], snapshot.images[i]);
			BindImage(images[i].image_id,
			          images[i].desc.type == TextureCache::BindingType::Storage);
		}
		auto& binding = images[i];
		binding.mip_views.clear();
		const auto& resource = program.info.images[i];
		if (resource.mip_mode == ShaderRecompiler::IR::ImageMipMode::Dynamic) {
			EXIT_IF(resource.mip_count == 0u ||
			        resource.mip_count != binding.desc.view_info.level_count);
			binding.mip_views.reserve(resource.mip_count);
			for (uint32_t mip = 0; mip < resource.mip_count; mip++) {
				auto desc = binding.desc;
				desc.view_info.base_level += mip;
				desc.view_info.level_count = 1;
				// The shader selects the mip after applying the guest minimum LOD.
				desc.view_info.min_lod = 0;
				binding.mip_views.push_back(texture_cache.FindTexture(binding.image_id, desc));
			}
			binding.image_view = binding.mip_views.front();
		} else {
			binding.image_view = texture_cache.FindTexture(binding.image_id, binding.desc);
		}
		auto&      image   = texture_cache.GetImage(binding.image_id);
		const bool storage = binding.desc.type == TextureCache::BindingType::Storage;
		image.usage.storage |= storage;
	}
}

void RenderExecutor::PrepareGraphicsBindings(std::span<PreparedBindings* const> stages,
                                             std::span<RenderColorInfo> colors) {
	Common::RenderStageProfile::Scope profile {Common::RenderStageProfile::Stage::PrepareGraphicsBindings};
	bool uses_dma        = false;
	bool uses_image_heap = false;
	FindBuffers(stages);
	for (auto* stage: stages) {
		if (stage->runtime->module->info.has_address_writes)
			m_context.GetBufferCache().ExcludeHotRange(0, UINT64_MAX, Common::SubmitProfile::Extra::HotRetireGpu);
		uses_dma |= stage->runtime->module->info.info.uses_dma;
		uses_image_heap |= stage->runtime->module->info.info.uses_image_heap;
	}
	if (uses_dma) {
		m_context.PrepareBda();
	}
	if (uses_image_heap) {
		m_context.GetImageHeap().Rebind(*this);
	}
	for (auto* stage: stages) {
		RebindImages(*stage);
	}
	auto& cache = m_context.GetTextureCache();
	for (auto& target: colors) {
		EXIT_IF(!target.image_id);
		auto& old_image = cache.m_slot_images[target.image_id];
		if (!old_image.registered || old_image.binding.needs_rebind) {
			old_image.binding = {};
			target.desc.view_info.base_level = target.guest_mip_level;
			target.desc.view_info.base_layer = target.guest_array_layer;
			target.image_id = cache.FindImage(target.desc);
			BindRenderTarget(target.image_id);
		}
	}
	// Discovery can read back PS5 metadata and submit the scheduler. Reserve draw buffers only
	// after image identities are final; attachment layout transitions follow buffer alias copies.
	for (auto* stage: stages) {
		RebindBuffers(*stage);
	}
}

// Cold failure diagnosis: recover the first writer with the pre-compaction
// traversal. Do not retain provenance or decode descriptors on successful draws.
[[gnu::noinline]] static void ReportBufferOverlap(
    const PreparedBindings& reader, uint64_t address, uint64_t size,
    std::span<PreparedBindings* const> stages, vk::PipelineBindPoint bind_point) {

	const auto& reader_module = *reader.runtime->module;
	const auto& reader_info = reader_module.info;
	for (size_t stage_index = 0; stage_index < stages.size(); ++stage_index) {
		const auto& writer = *stages[stage_index];
		const auto& writer_module = *writer.runtime->module;
		const auto& program = writer_module.info;
		for (size_t index = 0; index < program.info.buffers.size(); ++index) {
			const auto& resource = program.info.buffers[index];
			if (!resource.written || resource.descriptor_index == UINT32_MAX) continue;
			const auto& source = writer.buffer_sources[resource.descriptor_index];
			const bool linear = (resource.packed_stride & ((1u << 14u) | (1u << 20u))) == 0u;
			const auto written_size =
			    linear ? std::min<uint64_t>(source.size, resource.write_extent) : source.size;
			if (written_size == 0 || !ImageRangeOverlaps(address, size, source.address, written_size)) continue;

			// Snapshot only: never reread guest memory while reporting this failure.
			const auto descriptor = DecodeNativeDescriptor<ShaderBufferResource>(
			    writer.runtime->resources->buffers[index]);
			const auto descriptor_size = descriptor.GetSize();
			const bool sentinel = descriptor.NumRecords() == UINT32_MAX;
			auto table_limit = descriptor_size;
			if (sentinel) {
				for (const auto* bound: stages) {
					for (const auto [read_address, read_size]: bound->runtime->resources->specialization_reads) {
						if (read_size != 0 && read_address > source.address)
							table_limit = std::min(table_limit, read_address - source.address);
					}
				}
			}
			const auto message = fmt::format(
			    "scalar resource reads overlap a shader buffer write\n"
			    "[BINDING_OVERLAP] legacy_buffer_match=1 operation={} same_stage={}\n"
			    " reader: stage={} hash=0x{:016x} module=0x{:016x} read=0x{:016x}+0x{:x} read_count={}\n"
			    " writer: stage={} hash=0x{:016x} module=0x{:016x} stage_index={} resource={} source={} descriptor_index={} first_use_pc=0x{:x}\n"
			    " write: address=0x{:016x} checked_size=0x{:x} prepared_size=0x{:x} descriptor_address=0x{:016x} descriptor_size=0x{:x}\n"
			    " buffer: NumRecords=0x{:08x} stride={} packed_stride=0x{:08x} write_extent=0x{:08x} max_byte_extent=0x{:08x} linear={} add_tid={} swizzle={} read={} written={} atomic={} formatted={} scalar={} live={}\n"
			    " sentinel: present={} clipped={} table_clipped={} table_limit=0x{:x}\n"
			    " descriptor_dwords={:08x},{:08x},{:08x},{:08x}\n",
			    bind_point == vk::PipelineBindPoint::eCompute ? "dispatch" : "draw", &reader == &writer,
			    ShaderRecompiler::StageName(reader_info.stage), reader_info.shader_hash, reader_module.id,
			    address, size, reader.runtime->resources->specialization_reads.size(),
			    ShaderRecompiler::StageName(program.stage), program.shader_hash, writer_module.id,
			    stage_index, index, resource.source, resource.descriptor_index, resource.first_use_pc,
			    source.address, written_size, source.size, descriptor.Base48(), descriptor_size,
			    descriptor.NumRecords(), descriptor.Stride(), resource.packed_stride, resource.write_extent,
			    resource.max_byte_extent, linear, (resource.packed_stride & (1u << 20u)) != 0,
			    (resource.packed_stride & (1u << 14u)) != 0, resource.read, resource.written, resource.atomic,
			    resource.formatted, resource.scalar, resource.live, sentinel,
			    sentinel && source.size < descriptor_size, sentinel && table_limit < descriptor_size, table_limit,
			    descriptor.fields[0], descriptor.fields[1], descriptor.fields[2], descriptor.fields[3]);
			EXIT("%s", message.c_str());
			return;
		}
	}
	EXIT("scalar resource reads overlap a shader buffer write\n"
	     "[BINDING_OVERLAP] legacy_buffer_match=0 (compact/legacy mismatch) "
	     "reader_hash=0x%016" PRIx64 " read=0x%016" PRIx64 "+0x%" PRIx64 "\n",
	     reader_info.shader_hash, address, size);
}

void RenderExecutor::CommitBindings(CommandBuffer&                     buffer,
                                    vk::PipelineBindPoint              pipeline_bind_point,
                                    const PipelineCache::Pipeline&     pipeline,
                                    std::span<PreparedBindings* const> prepared_bindings) {
	Common::RenderStageProfile::Scope stage_profile {Common::RenderStageProfile::Stage::CommitBindings};
	const auto emission = PrepareBindingEmission(buffer, pipeline_bind_point, pipeline, prepared_bindings);
	Common::RenderStageProfile::Scope emit_profile {Common::RenderStageProfile::Stage::BindingsEmit};
	if (emission.state.heap_set)
		Common::GpuThreadSampler::NativeBoundary(Common::GpuThreadSampler::Boundary::OtherNative);
	// Preparation may have submitted and rotated the command buffer. No native
	// handle from before it may be used for any part of this binding emission.
	buffer.EmitBindings(emission);
}

VkRecording::BindingEmission RenderExecutor::PrepareBindingEmission(
    CommandBuffer& buffer, vk::PipelineBindPoint pipeline_bind_point,
    const PipelineCache::Pipeline& pipeline, std::span<PreparedBindings* const> prepared_bindings) {
	VkRecording::BindingEmission emission {};
	emission.state.layout = pipeline.pipeline_layout;
	emission.state.point = pipeline_bind_point;
	emission.state.push_descriptors = pipeline.uses_push_descriptors;
	bool unknown_import_bda = false;
	if (m_context.GetBufferCache().HasHostImports()) {
		for (auto* prepared: prepared_bindings) {
			unknown_import_bda |= prepared->runtime->module->info.info.uses_dma;
			for (const auto& resource: prepared->runtime->module->info.info.buffers) {
				const auto index = resource.descriptor_index;
				if (index == UINT32_MAX || resource.written) continue;
				const auto& source = prepared->buffer_sources[index];
				m_context.GetBufferCache().ResolveHostImportRead(source.address, source.size, prepared->buffers[index]);
			}
		}
	}
	using Stage = Common::RenderStageProfile::Stage;
	using Extra = Common::SubmitProfile::Extra;
	const bool count_ranges = Common::SubmitProfile::Recording();
	uint64_t read_ranges = 0, write_ranges = 0, image_comparisons = 0, buffer_comparisons = 0;
	KYTY_PROFILER_FUNCTION();
	size_t image_descriptor_count = 0;
	size_t write_count      = 0;
	m_binding_push_data = {};
	auto& push_data = m_binding_push_data;
	bool                           has_push_data = false;
	constexpr auto                 GraphicsStages =
	    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eMeshEXT |
	    vk::ShaderStageFlagBits::eTessellationControl |
	    vk::ShaderStageFlagBits::eTessellationEvaluation | vk::ShaderStageFlagBits::eFragment;
	vk::ShaderStageFlags push_stages = pipeline_bind_point == vk::PipelineBindPoint::eGraphics
	                                       ? vk::ShaderStageFlagBits::eFragment
	                                       : vk::ShaderStageFlags {};
	{
		Common::RenderStageProfile::Scope profile {Stage::BindingsBuild};
		for (const auto* prepared: prepared_bindings) {
			EXIT_IF(prepared == nullptr || prepared->runtime == nullptr || !*prepared->runtime);
			const auto& program = prepared->runtime->module->info;
			write_count += std::popcount(program.bindings.descriptor_mask);
			for (auto mask = program.bindings.descriptor_mask; mask != 0; mask &= mask - 1) {
				const auto index = std::countr_zero(mask);
				const auto count = program.bindings.descriptor_counts[index];
				if (index >= ShaderRecompiler::IR::FirstImageBinding &&
				    index <= static_cast<size_t>(BindingKind::Samplers)) {
					image_descriptor_count += count;
				}
			}
			const auto shader_stage = NativeShaderStage(program.stage);
			push_stages |= shader_stage;
			EXIT_IF((pipeline_bind_point == vk::PipelineBindPoint::eGraphics &&
			         (shader_stage & GraphicsStages) == vk::ShaderStageFlags {}) ||
			        (pipeline_bind_point == vk::PipelineBindPoint::eCompute &&
			         shader_stage != vk::ShaderStageFlagBits::eCompute));
		}
	}
	{
		Common::RenderStageProfile::Scope profile {Stage::BindingsOverlap};
		bool collected = false;
		for (const auto* reader: prepared_bindings) {
			const auto& reads = reader->runtime->resources->specialization_reads;
			if (count_ranges) read_ranges += reads.size();
			if (reads.empty()) continue;
			for (const auto* writer: prepared_bindings) {
				if (writer->runtime->module->info.has_address_writes) {
					EXIT("scalar resource reads cannot be proven disjoint from shader address writes\n");
				}
			}
			// Check address writes above even when there are no bounded write ranges.
			// Build lazily: commits without scalar reads need no overlap work.
			if (!collected) {
				m_binding_overlap_writes.Clear();
				for (const auto id: m_bound_images) {
					const auto& image = m_context.GetTextureCache().m_slot_images[id];
					m_binding_overlap_writes.AddImage(image.info, image.binding.shader_write,
					                                  image.binding.is_target);
				}
				for (const auto* writer: prepared_bindings) {
					for (const auto& resource: writer->runtime->module->info.info.buffers) {
						m_binding_overlap_writes.AddBuffer(resource, writer->buffer_sources);
					}
				}
				if (count_ranges) write_ranges = m_binding_overlap_writes.Size();
				m_binding_overlap_writes.PrepareIntervals();
				Common::SubmitProfile::AddExtra(Extra::OverlapIntervalCommits);
				Common::SubmitProfile::AddExtra(Extra::OverlapIntervalRanges, m_binding_overlap_writes.IntervalCount());
				collected = true;
			}
			if (m_binding_overlap_writes.Size() == 0) continue;
			for (const auto [address, size]: reads) {
				switch (m_binding_overlap_writes.CheckRead(address, size, count_ranges,
				                                           image_comparisons, buffer_comparisons)) {
					case BindingOverlapWrites::Hit::Image:
						EXIT("scalar resource reads overlap an image or attachment write\n");
						break;
					case BindingOverlapWrites::Hit::Buffer:
						ReportBufferOverlap(*reader, address, size, prepared_bindings, pipeline_bind_point);
						break;
					case BindingOverlapWrites::Hit::None: break;
				}
			}
		}
	}
	// Writer ranges are counted once per commit that actually checks any reads;
	// Comparisons count interval endpoints
	// plus the original-order recovery on the cold conflict/invalid-range path.
	if (count_ranges) {
		Common::SubmitProfile::AddExtra(Extra::BindingReadRanges, read_ranges);
		Common::SubmitProfile::AddExtra(Extra::BindingWriteRanges, write_ranges);
		Common::SubmitProfile::AddExtra(Extra::BindingImageComparisons, image_comparisons);
		Common::SubmitProfile::AddExtra(Extra::BindingBufferComparisons, buffer_comparisons);
	}
	if (pipeline.uses_image_heap) {
		Common::RenderStageProfile::Scope profile {Stage::BindingsImageHeap};
		emission.state.heap_set = m_context.GetImageHeap().PrepareBinding(buffer);
		emission.state.heap_index = ShaderRecompiler::IR::ImageHeapSet;
	}
	{
		Common::RenderStageProfile::Scope profile {Stage::BindingsBuild};
		m_descriptor_images.resize(image_descriptor_count);
		m_descriptor_writes.clear();
		m_descriptor_writes.reserve(write_count);
	}
	size_t image_cursor = 0;
	auto& address_buffers = m_address_descriptors;

	for (auto* prepared: prepared_bindings) {
		const auto& program       = prepared->runtime->module->info;
		auto&       descriptors   = *prepared;
		const auto  shader_stage  = NativeShaderStage(program.stage);
		const auto  shader_stages = ShaderPipelineStages(shader_stage);
		{
			Common::RenderStageProfile::Scope profile {Stage::BindingsTransitions};
			if (descriptors.gds.buffer != nullptr) {
				buffer.EndRendering();
				const auto barrier = MakeGdsDependency(descriptors.gds.buffer);
				buffer.Commands().pipelineBarrier(
				    vk::PipelineStageFlagBits::eHost | vk::PipelineStageFlagBits::eTransfer |
				        vk::PipelineStageFlagBits::eAllGraphics |
				        vk::PipelineStageFlagBits::eComputeShader,
				    shader_stages, vk::DependencyFlags {}, 0, nullptr, 1, &barrier, 0, nullptr);
			}

			for (uint32_t i = 0; i < program.info.images.size(); i++) {
				auto& binding = descriptors.images[i];
				if (!binding.image_id) continue;
				auto& image = m_context.GetTextureCache().GetImage(binding.image_id);
				const auto&                 view = binding.desc.view_info;
				const ImageSubresourceRange range {view.base_level, view.level_count, view.base_layer,
				                                   view.layer_count};
				const bool storage = binding.desc.type == TextureCache::BindingType::Storage;
				if (image.binding.is_target) {
					const auto layout = image.binding.attachment_layout;
					EXIT_IF(layout == vk::ImageLayout::eUndefined);
					if (image.info.IsDepth()) {
						const auto host_view =
						    std::ranges::find(image.views, binding.image_view, &CachedImageView::view);
						EXIT_IF(storage || host_view == image.views.end());
						const auto aspect = host_view->info.aspect;
						if (aspect & ~DepthReadableAspects(layout)) {
							EXIT("sampling a writable depth/stencil attachment aspect\n");
						}
					}
					image.Transit(layout,
					              image.binding.attachment_access | vk::AccessFlagBits2::eShaderRead |
					                  (image.binding.shader_write ? vk::AccessFlagBits2::eShaderWrite
					                                              : vk::AccessFlags2 {}),
					              {}, buffer.Commands());
				} else if (image.binding.force_general && !image.info.IsDepth()) {
					const vk::AccessFlags2 storage_access = image.binding.shader_write
					                                            ? vk::AccessFlagBits2::eShaderWrite
					                                            : vk::AccessFlags2 {};
					image.Transit(vk::ImageLayout::eGeneral,
					              vk::AccessFlagBits2::eShaderRead | storage_access, {}, buffer.Commands());
				} else if (storage) {
					image.Transit(vk::ImageLayout::eGeneral,
					              vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
					              range, buffer.Commands());
				} else {
					image.Transit(image.ReadLayout(), vk::AccessFlagBits2::eShaderRead, range, buffer.Commands());
				}
				binding.layout = image.backing.state.layout;
			}

		}
		Common::RenderStageProfile::Scope build_profile {Stage::BindingsBuild};
		std::array<size_t, ShaderRecompiler::IR::ImageBindingCount> image_starts {};
		for (auto mask = program.bindings.descriptor_mask; mask != 0; mask &= mask - 1) {
			const auto index = std::countr_zero(mask);
			const auto kind = static_cast<BindingKind>(index);
			vk::WriteDescriptorSet write {};
			write.dstBinding = ShaderRecompiler::IR::NativeBinding(program.stage, kind);
			write.descriptorType = NativeDescriptorType(kind);
			write.descriptorCount = program.bindings.descriptor_counts[index];
			const auto image_start = image_cursor;
			if (ShaderRecompiler::IR::ImageBindingResourceClass(kind) !=
			    ShaderRecompiler::IR::ImageResourceClass::None) {
				image_starts[ShaderRecompiler::IR::ImageBindingIndex(kind)] = image_start;
				image_cursor += write.descriptorCount;
			} else {
				switch (kind) {
					case BindingKind::Buffers:
						EXIT_IF(descriptors.buffers.size() != write.descriptorCount);
						for (const auto& view: descriptors.buffers) {
							EXIT_IF(view.buffer == nullptr);
						}
						write.pBufferInfo = descriptors.buffers.data();
						break;
					case BindingKind::BdaPagetable:
					case BindingKind::FaultBuffer: {
						auto& cache = m_context.GetBufferCache();
						const auto* bda_buffer = kind == BindingKind::BdaPagetable
						                             ? cache.GetBdaPageTableBuffer()
						                             : cache.GetFaultBuffer();
						auto& view = address_buffers[kind == BindingKind::FaultBuffer];
						view = {bda_buffer->Handle(), 0, bda_buffer->Size()};
						write.pBufferInfo = &view;
						break;
					}
					case BindingKind::FlattenedSrt:
					case BindingKind::ShaderData:
					case BindingKind::SharedMemory:
					case BindingKind::Gds: {
						const vk::DescriptorBufferInfo* view = &descriptors.gds;
						if (kind == BindingKind::FlattenedSrt) {
							view = &descriptors.flattened_srt;
						} else if (kind == BindingKind::ShaderData) {
							view = &descriptors.shader_data_buffer;
						} else if (kind == BindingKind::SharedMemory) {
							view = &descriptors.shared_memory;
						}
						EXIT_IF(view->buffer == nullptr);
						write.pBufferInfo = view;
						break;
					}
					case BindingKind::Samplers:
						for (const auto sampler: descriptors.samplers) {
							EXIT_IF(sampler == nullptr);
							m_descriptor_images[image_cursor++] = {sampler, nullptr, vk::ImageLayout::eUndefined};
						}
						break;
					case BindingKind::Count: EXIT("invalid descriptor binding kind");
				}
			}
			if (image_cursor != image_start) {
				write.pImageInfo = m_descriptor_images.data() + image_start;
			}
			m_descriptor_writes.push_back(write);
		}
		for (uint32_t i = 0; i < program.info.images.size(); ++i) {
			const auto& resource = program.info.images[i];
			const auto& texture = descriptors.images[i];
			if (resource.heap) continue;
			EXIT_IF(texture.image_id &&
			        resource.mip_count != (texture.mip_views.empty() ? 1u : texture.mip_views.size()));
			const auto kind = *ShaderRecompiler::IR::DescriptorBindingForImage(resource);
			const auto start = image_starts[ShaderRecompiler::IR::ImageBindingIndex(kind)] +
			                   resource.descriptor_index;
			for (uint32_t mip = 0; mip < resource.mip_count; ++mip) {
				m_descriptor_images[start + mip] = MakeImageInfo(texture, mip);
			}
		}

		const auto shader_data_dwords = program.bindings.ShaderDataDwords();
		EXIT_IF(prepared->shader_data.size() != shader_data_dwords);
		if (program.bindings.UsesPushData()) {
			std::ranges::copy(prepared->shader_data,
			                  push_data.dwords.begin() + program.bindings.push_data_start_dword);
			has_push_data = true;
		}
	}

	if (has_push_data) {
		emission.state.stages = push_stages;
		emission.state.constant_bytes = sizeof(push_data);
		emission.constants = std::as_bytes(std::span(push_data.dwords));
	}

	if (!m_descriptor_writes.empty()) {
		EXIT_IF(pipeline.descriptor_set_layout == nullptr);
		if (pipeline.uses_push_descriptors) {
			Common::SubmitProfile::AddExtra(Extra::PushDescriptorCalls);
		} else {
			const auto set = m_context.GetDescriptorHeap().Commit(pipeline.descriptor_set_layout);
			emission.state.set = set;
			Common::SubmitProfile::AddExtra(Extra::DescriptorSetCalls);
			for (auto& write: m_descriptor_writes) {
				write.dstSet = set;
			}
		}
	}
	if (Common::ReadbackProfile::Enabled()) {
		const auto tick = m_context.GetCommandScheduler().CurrentTick();
		for (const auto* prepared: prepared_bindings) {
			const auto& module = *prepared->runtime->module;
			const auto& program = module.info;
			for (uint32_t i = 0; i < program.info.buffers.size(); ++i) {
				const auto& resource = program.info.buffers[i];
				if (!resource.written || resource.descriptor_index >= prepared->buffer_sources.size()) continue;
				const auto& source = prepared->buffer_sources[resource.descriptor_index];
				const bool linear = (resource.packed_stride & ((1u << 14u) | (1u << 20u))) == 0;
				const auto bytes = linear ? std::min<uint64_t>(source.size, resource.write_extent) : source.size;
				Common::ReadbackProfile::RecordWriter({source.address, bytes, program.shader_hash,
				    module.id, tick, 0, static_cast<uint32_t>(program.stage), i, resource.atomic});
			}
		}
	}
	// ImageHeap::PrepareBinding may rotate the command buffer when its stream wraps.
	// Publish references to the FINAL tick, immediately before draw/dispatch;
	// descriptor binding itself does not execute a read. No submit occurs here.
	m_context.GetBufferCache().PrepareHostImportReads(unknown_import_bda);
	emission.writes = m_descriptor_writes;
	return emission;
}

} // namespace Libs::Graphics
