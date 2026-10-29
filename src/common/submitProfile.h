#pragma once
#include "common/guestCpuProfile.h"
#include "common/guestHostImport.h"
#include "common/readbackAhead.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <mutex>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef MemoryBarrier
#elif defined(__APPLE__)
#include <pthread.h>
#else
#include <sys/syscall.h>
#include <unistd.h>
#endif

// Opt-in, bounded attribution of host queue submits, CPU waits on the GPU timeline and idle
// waits of the guest GPU thread. KYTY_SUBMIT_PROFILE=1 arms it; samples are only recorded
// while the runtime gate is open (see submitProfileRuntime.cpp), so loading screens can be
// skipped without restarting. Aggregates live in preallocated relaxed atomics: no
// allocation, lock or log write per event.
//
// Costs: disarmed, every hook is one cached-bool test (scopes included). Armed with the gate
// closed, hooks add one relaxed atomic load and the GPU thread reads the clock once per loop.
//
// Windows: Reset() starts a new generation. Durations that began in an older generation
// (waits, idle waits, blocked submissions) are counted as dropped, never mixed into the new
// window, and per-thread pending state from an older generation is discarded. Counters of one
// report are read field by field while producers keep running, so an event racing with the
// report may land in the next window; nothing is lost.
namespace Common::SubmitProfile {

// Immediate reason of a vkQueueSubmit, tagged by the caller through ReasonScope.
enum class Reason : uint8_t {
	Other, GfxComplete, GfxSuspended, ComputeComplete, ComputeSuspended, SuspendPoint,
	ReleaseMemInterrupt, WaitFlipDone, Flip, WaitTick, Finish, Present, Shutdown, Count
};
inline constexpr std::array ReasonNames {
	"other", "gfx_complete", "gfx_suspended", "compute_complete", "compute_suspended",
	"suspend_point", "release_mem_interrupt", "wait_flip_done", "flip", "wait_tick", "finish",
	"present", "shutdown"
};
static_assert(ReasonNames.size() == static_cast<size_t>(Reason::Count));

// Where the submitting thread was, independent of the immediate reason: e.g. a wait_tick
// raised while a graphics PM4 stream is being processed reports origin gfx_pm4.
enum class Origin : uint8_t { Outside, GfxPm4, ComputePm4, Count };
inline constexpr std::array OriginNames {"outside_pm4", "gfx_pm4", "compute_pm4"};
static_assert(OriginNames.size() == static_cast<size_t>(Origin::Count));

// Caller of MasterSemaphore::Wait that blocks the CPU, tagged through WaitSiteScope.
// Readback sites split BufferCache::ReadMemory by caller; the origin is carried into the GPU-thread
// command explicitly, since a scope on a guest CPU thread does not cross SendCommandSync.
enum class WaitSite : uint8_t {
	Other, StreamWrap, BufferReadback, BufferGc, FaultDownload, Finish, PriorityCallbacks,
	Present, Shutdown, ReadbackIndirectArgs, ReadbackColorMetadata, ReadbackCpuReadFault,
	ReadbackCpuWriteInvalidate, ReadbackPriorityOps, IndirectCountState, Count
};
inline constexpr std::array WaitSiteNames {
	"other", "stream_wrap", "readback_other", "buffer_gc", "fault_download", "finish",
	"priority_callbacks", "present", "shutdown", "readback_indirect_args",
	"readback_color_metadata", "readback_cpu_read_fault", "readback_cpu_write_invalidate",
	"readback_priority_ops", "indirect_count_state"
};
static_assert(WaitSiteNames.size() == static_cast<size_t>(WaitSite::Count));

// Why a PM4 stream suspended, recorded where the existing condition is evaluated.
enum class Block : uint8_t { WaitRegMem, WaitFlip, Rewind, Predication, Unknown, Count };
inline constexpr std::array BlockNames {"wait_reg_mem", "wait_flip", "rewind", "predication",
                                        "unknown"};
static_assert(BlockNames.size() == static_cast<size_t>(Block::Count));

// Plain event counters reported under "extra".
enum class Extra : uint16_t {
	Flips, OcclusionDumps, PredicationZpass, PredicationBool, PredicationOff, PredicationNoWait,
	PredicationNotReady, PredicationSkipOn, PacketsSkipped, MaterializeCalls,
	MaterializeRepeatLastForProgram, MaterializeRepeatSameFrame, MaterializeRepeatPrevFrame,
	MaterializeHashNs, TimelineRefreshes, TimelineRefreshNs, FaultReadbackEligible,
	FaultReadbackCurrentTick, FaultReadbackUnknown, FaultReadbackFast, GpuCommandBuffers,
	GpuCommandBufferNs, GpuDetiles, GpuDetileNs, GpuUploads, GpuUploadNs, GpuBlocksSkipped,
	GpuResultsNotReady, GpuPairsDroppedCapacity, GpuPairsDroppedFlush, GpuDepthDetiles,
	GpuDepthDetileNs, GpuDepthConverts, GpuDepthConvertNs, GpuDepthUploads, GpuDepthUploadNs,
	TrackedReadFaults, TrackedWriteFaults, BindingReadRanges, BindingWriteRanges,
	BindingImageComparisons, BindingBufferComparisons, PushDescriptorCalls, DescriptorSetCalls,
	FalseSharedServed, AttribCleanReads, AttribCleanFallbacks,
	WalkerCleanReads, WalkerCleanFallbacks, WalkerCleanFaults,
	WalkerControlPromotions, WalkerControlWalks, WalkerControlFallbacks, IndirectCountGpuDraws, IndirectCountGpuFallbacks,
	IndirectPackets, IndirectIndexed, IndirectAuto, IndirectMultiple, IndirectNoCount, IndirectZeroMax,
	IndirectCountAttempts, IndirectCountGateClosed, IndirectPatchVertex, IndirectPatchInstance, IndirectPatchIndex,
	IndirectMissingIndexSize, IndirectBadStride, IndirectCountUnaligned, IndirectCountDevice,
	IndirectCountLimit, IndirectCountHistoryFull, IndirectCountUnmapped, IndirectArgsUnmapped,
	IndirectRendererRejected, IndirectRejectColorMode, IndirectRejectDepthCopy, IndirectRejectDepthClear,
	IndirectRejectStencilClear, IndirectRejectMesh, IndirectRejectQuad, IndirectCpuRecords, IndirectGpuRecords,
	ImageDescObservations, ImageDescSameKey, ImageDescChangedKey, ImageDescFirstKey,
	ImageDescHistoryCapacityResets, ImageDescReuseHits, ImageDescReuseMisses,
	ImageFindHintHits, ImageFindHintMisses, ImageFindHintTooLarge,
	IndirectMeshDraws, IndirectMeshFallbacks, IndirectMeshRangeFallbacks,
	OverlapIntervalCommits, OverlapIntervalRanges,
	WalkerControlPlans, WalkerControlLearnedSites, WalkerControlFallbackPlans,
	WalkerControlRetries, WalkerControlRecoveries,
	IndirectIndexSamples, IndirectIndexRequested, IndirectIndexAvailable, IndirectIndexValid,
	IndirectIndexIssued, IndirectIndexInstances, IndirectIndexZeroInstances,
	IndirectMeshGroups, MeshGroups, IndirectIndexProfileDropped,
	IndirectMeshSnapshots, IndirectMeshEmpty, IndirectMeshNonempty,
	IndirectMeshZeroInstances, IndirectMeshZeroIndices,
	GpuIndirectMeshConverts, GpuIndirectMeshConvertNs,
	GpuIndirectMeshPrepares, GpuIndirectMeshPrepareNs,
	GpuIndirectMeshEmits, GpuIndirectMeshEmitNs,
	GpuIndirectPrechecks, GpuIndirectPrecheckNs, GpuIndirectDiscoveries, GpuIndirectDiscoveryNs,
	GpuTimestampsRecorded, GpuTimestampsPublished, GpuTimestampsLate, GpuTimestampsConflict,
	GpuTimestampsFallback, GpuTimestampsFailed, GpuTimestampBatches,
	GpuTimestampBufferInvalidations, GpuTimestampBuffersAbsent,
	HostImports, HostReimports, HostImportBytesAvoided, HostImportFallbackUnsupported,
	HostImportFallbackBacking, HostImportFallbackAlignment, HostImportFallbackType,
	HostImportFallbackAllocation, HostImportFallbackProbe, HostImportFallbackGpu, HostImportFallbackImage,
	HostImportFallbackResource, HostImportFallbackMerge, HostImportFallbackLimit,
	HostImportRetireMapping, HostImportRetireGate, HostImportGuardWaits, HostImportGuardNs,
	HostCandidateOwnerSmall, HostCandidateRequestSmall, HostCandidateClean, HostCandidateWarmup,
	HostCandidateCooldown, HostCandidateUnsupported, HostCandidateLimitCount, HostCandidateLimitSize,
	HostCandidateLimitTotal, HostCandidateDownloads, HostCandidateGpuDirty, HostCandidateGpuHistory,
	HostCandidateImageLive, HostCandidateImageHistory, HostCandidateResource, HostCandidateBackingBusy,
	HostCandidateBackingUnmapped, HostCandidateBackingDiscontiguous, HostCandidateBackingAlias,
	HostCandidateAlignment, HostCandidateType, HostCandidateAllocation, HostCandidateImported,
	HostImportRetirements, HostImportRetireNs, HostImportRestoreBytes, HostImportReadSnapshotBytes,
	HostImportCreateAttempts, HostImportCreateNs, HostImportDescriptorReads,
	HostImportExclusionCalls, HostImportExclusionSamples, HostImportExclusionNs,
	HostImportExclusionCacheHits, HostImportRetireWaits, HostImportRetireWaitNs, HostImportRetireFlushes,
	HostImportActivations, HostImportActivationNs,
	HostImportCandidateCalls, HostImportCandidateSamples, HostImportCandidateNs,
	HostImportGuardRangeWaits, HostImportGuardUnknownWaits, HostImportGuardBypassed,
	HostImportGuardDisjoint, HostImportGuardCompleted, HostImportUnknownBdaDraws,
	HostImportGuardLaneCalls, HostImportGuardLaneNs,
	HostImportDmaBytes, HostImportStagingBytes, HostImportCopyFallbackBytes,
	HostImportCopyFallbackNoImport, HostImportCopyFallbackProfile,
	HotPromotions, HotReprotected, HotFaultsObserved, HotFaultsAvoidedEstimated,
	HotResyncBytes, HotResyncUses, HotBdaBytes, HotRetireGate, HotRetireGpu,
	HotRetireImage, HotRetireReadback, HotRetireBudget, HotRetireAge, HotRetireMerge,
	HotRetireMapping, HotRejectGpu, HotRejectImage, HotRejectReadback, HotFrames, HotOwnerFrames,
	HotRejectExcluded, HotRejectMapping, HotRejectBdaBudget,
	HotCompletedFrames, HotFrameResyncBytes, HotFrameResyncUses, HotBdaCalls,
	HotExclusionCalls, HotExclusionVisits, HotExclusionSamples, HotExclusionNs,
	HotHistoryPages, HotMaintenanceNs,
	FcFaults, FcUniquePageFrames, FcRepeatPageFrames, FcRefaultAfterUpload,
	FcExpanded, FcDirtyBytes, FcNewDirtyBytes, FcExtraDirtyUpperBytes,
	FcUploadBytes, FcExtraUploadUpperBytes, FcRejectGpu, FcRejectImport,
	FcRejectWatcher, FcRejectReadback, FcPromotions, FcRetired, FcFrames, FcOwnerFrames, FcStreamBytes,
	SrtBatchCandidates, SrtBatchReadbacks, SrtBatchFallbacks, SrtBatchRequestedBytes,
	WriteAheadAttempts, WriteAheadBatches, WriteAheadPages, WriteAheadExtraBytes,
	WriteAheadRejectPending, WriteAheadRejectImport, WriteAheadRejectRange, WriteAheadRejectSparse,
	WriteAheadLimitHits, WriteAheadRejectFirstPage,
	ReadAheadAttempts, ReadAheadBatches, ReadAheadPages, ReadAheadExtraBytes,
	ReadAheadRejectPending, ReadAheadRejectImport, ReadAheadRejectRange, ReadAheadRejectSparse,
	ReadAheadLimitHits, ReadAheadRejectFirstPage,
	GuestCleanServed, GuestCleanRejectDirty, GuestCleanRejectPending, GuestCleanRejectWidth, GuestCleanRejectBacking,
	GuestCleanRejectInstruction, GuestCleanRejectBudget, GuestCleanRecovered,
	LinearCopyCandidates, LinearCopyRejectShape, LinearCopyRejectDescriptor, LinearCopyRejectParameters,
	LinearCopyRejectCount, LinearCopyRejectOverlap, LinearCopyRejectAlias, LinearCopyCpu, LinearCopyGpu, LinearCopyBytes, Count
};
inline constexpr std::array ExtraNames {
	"flips", "occlusion_dumps", "predication_zpass", "predication_bool", "predication_off",
	"predication_no_wait", "predication_not_ready", "predication_skip_on", "packets_skipped",
	"materialize_calls", "materialize_repeat_last_for_program", "materialize_repeat_same_frame",
	"materialize_repeat_prev_frame", "materialize_hash_ns", "timeline_refreshes", "timeline_refresh_ns",
	"fault_readback_eligible", "fault_readback_current_tick", "fault_readback_unknown",
	"fault_readback_fast", "gpu_command_buffers", "gpu_command_buffer_ns", "gpu_detiles",
	"gpu_detile_ns", "gpu_uploads", "gpu_upload_ns", "gpu_blocks_skipped", "gpu_results_not_ready",
	"gpu_pairs_dropped_capacity", "gpu_pairs_dropped_flush", "gpu_depth_detiles", "gpu_depth_detile_ns",
	"gpu_depth_converts", "gpu_depth_convert_ns", "gpu_depth_uploads", "gpu_depth_upload_ns",
	"tracked_read_faults", "tracked_write_faults", "binding_read_ranges", "binding_write_ranges",
	"binding_image_comparisons", "binding_buffer_comparisons", "push_descriptor_calls", "descriptor_set_calls",
	"false_shared_served", "attrib_clean_reads", "attrib_clean_fallbacks",
	"walker_clean_reads", "walker_clean_fallbacks", "walker_clean_faults",
	"walker_control_promotions", "walker_control_walks", "walker_control_fallbacks", "indirect_count_gpu_draws", "indirect_count_gpu_fallbacks",
	"indirect_packets", "indirect_indexed", "indirect_auto", "indirect_multiple", "indirect_no_count", "indirect_zero_max",
	"indirect_count_attempts", "indirect_count_gate_closed", "indirect_patch_vertex", "indirect_patch_instance", "indirect_patch_index",
	"indirect_missing_index_size", "indirect_bad_stride", "indirect_count_unaligned", "indirect_count_device",
	"indirect_count_limit", "indirect_count_history_full", "indirect_count_unmapped", "indirect_args_unmapped",
	"indirect_renderer_rejected", "indirect_reject_color_mode", "indirect_reject_depth_copy", "indirect_reject_depth_clear",
	"indirect_reject_stencil_clear", "indirect_reject_mesh", "indirect_reject_quad", "indirect_cpu_records", "indirect_gpu_records",
	"image_desc_observations", "image_desc_same_key", "image_desc_changed_key", "image_desc_first_key",
	"image_desc_history_capacity_resets", "image_desc_reuse_hits", "image_desc_reuse_misses",
	"image_find_hint_hits", "image_find_hint_misses", "image_find_hint_too_large",
	"indirect_mesh_gpu_draws", "indirect_mesh_gpu_fallbacks", "indirect_mesh_range_fallbacks",
	"overlap_interval_commits", "overlap_interval_ranges",
	"walker_control_plans", "walker_control_learned_sites", "walker_control_fallback_plans",
	"walker_control_retries", "walker_control_recoveries",
	"indirect_index_samples", "indirect_index_requested", "indirect_index_available", "indirect_index_valid",
	"indirect_index_issued", "indirect_index_instances", "indirect_index_zero_instances",
	"indirect_mesh_groups", "mesh_groups", "indirect_index_profile_dropped",
	"indirect_mesh_snapshots", "indirect_mesh_empty", "indirect_mesh_nonempty",
	"indirect_mesh_zero_instances", "indirect_mesh_zero_indices",
	"gpu_indirect_mesh_converts", "gpu_indirect_mesh_convert_ns",
	"gpu_indirect_mesh_prepares", "gpu_indirect_mesh_prepare_ns",
	"gpu_indirect_mesh_emits", "gpu_indirect_mesh_emit_ns",
	"gpu_indirect_prechecks", "gpu_indirect_precheck_ns", "gpu_indirect_discoveries", "gpu_indirect_discovery_ns",
	"gpu_timestamps_recorded", "gpu_timestamps_published", "gpu_timestamps_late", "gpu_timestamps_conflict",
	"gpu_timestamps_fallback", "gpu_timestamps_failed", "gpu_timestamp_batches",
	"gpu_timestamp_buffer_invalidations", "gpu_timestamp_buffers_absent",
	"host_imports", "host_reimports", "host_import_bytes_avoided", "host_import_fallback_unsupported",
	"host_import_fallback_backing", "host_import_fallback_alignment", "host_import_fallback_type",
	"host_import_fallback_allocation", "host_import_fallback_probe", "host_import_fallback_gpu", "host_import_fallback_image",
	"host_import_fallback_resource", "host_import_fallback_merge", "host_import_fallback_limit",
	"host_import_retire_mapping", "host_import_retire_gate", "host_import_guard_waits", "host_import_guard_ns",
	"host_candidate_owner_small", "host_candidate_request_small", "host_candidate_clean", "host_candidate_warmup",
	"host_candidate_cooldown", "host_candidate_unsupported", "host_candidate_limit_count", "host_candidate_limit_size",
	"host_candidate_limit_total", "host_candidate_downloads", "host_candidate_gpu_dirty", "host_candidate_gpu_history",
	"host_candidate_image_live", "host_candidate_image_history", "host_candidate_resource", "host_candidate_backing_busy",
	"host_candidate_backing_unmapped", "host_candidate_backing_discontiguous", "host_candidate_backing_alias",
	"host_candidate_alignment", "host_candidate_type", "host_candidate_allocation", "host_candidate_imported",
	"host_import_retirements", "host_import_retire_ns", "host_import_restore_bytes", "host_import_read_snapshot_bytes",
	"host_import_create_attempts", "host_import_create_ns", "host_import_descriptor_reads",
	"host_import_exclusion_calls", "host_import_exclusion_samples", "host_import_exclusion_ns",
	"host_import_exclusion_cache_hits", "host_import_retire_waits", "host_import_retire_wait_ns", "host_import_retire_flushes",
	"host_import_activations", "host_import_activation_ns",
	"host_import_candidate_calls", "host_import_candidate_samples", "host_import_candidate_ns",
	"host_import_guard_range_waits", "host_import_guard_unknown_waits", "host_import_guard_bypassed",
	"host_import_guard_disjoint", "host_import_guard_completed", "host_import_unknown_bda_draws",
	"host_import_guard_lane_calls", "host_import_guard_lane_ns",
	"host_import_dma_bytes", "host_import_staging_bytes", "host_import_copy_fallback_bytes",
	"host_import_copy_fallback_no_import", "host_import_copy_fallback_profile",
	"hot_promotions", "hot_reprotected", "hot_faults_observed", "hot_faults_avoided_estimated",
	"hot_resync_bytes", "hot_resync_uses", "hot_bda_bytes", "hot_retire_gate", "hot_retire_gpu",
	"hot_retire_image", "hot_retire_readback", "hot_retire_budget", "hot_retire_age", "hot_retire_merge",
	"hot_retire_mapping", "hot_reject_gpu", "hot_reject_image", "hot_reject_readback", "hot_frames", "hot_owner_frames",
	"hot_reject_excluded", "hot_reject_mapping", "hot_reject_bda_budget",
	"hot_completed_frames", "hot_frame_resync_bytes", "hot_frame_resync_uses", "hot_bda_calls",
	"hot_exclusion_calls", "hot_exclusion_visits", "hot_exclusion_samples", "hot_exclusion_ns",
	"hot_history_pages", "hot_maintenance_ns",
	"fc_faults", "fc_unique_page_frames", "fc_repeat_page_frames", "fc_refault_after_upload",
	"fc_expanded", "fc_dirty_bytes", "fc_new_dirty_bytes", "fc_extra_dirty_upper_bytes",
	"fc_upload_bytes", "fc_extra_upload_upper_bytes", "fc_reject_gpu", "fc_reject_import",
	"fc_reject_watcher", "fc_reject_readback", "fc_promotions", "fc_retired", "fc_frames", "fc_owner_frames", "fc_stream_bytes",
	"srt_batch_candidates", "srt_batch_readbacks", "srt_batch_fallbacks", "srt_batch_requested_bytes",
	"write_ahead_attempts", "write_ahead_batches", "write_ahead_pages", "write_ahead_extra_bytes",
	"write_ahead_reject_pending", "write_ahead_reject_import", "write_ahead_reject_range", "write_ahead_reject_sparse",
	"write_ahead_limit_hits", "write_ahead_reject_first_page",
	"read_ahead_attempts", "read_ahead_batches", "read_ahead_pages", "read_ahead_extra_bytes",
	"read_ahead_reject_pending", "read_ahead_reject_import", "read_ahead_reject_range", "read_ahead_reject_sparse",
	"read_ahead_limit_hits", "read_ahead_reject_first_page",
	"guest_clean_served", "guest_clean_reject_dirty", "guest_clean_reject_pending", "guest_clean_reject_width",
	"guest_clean_reject_backing", "guest_clean_reject_instruction", "guest_clean_reject_budget", "guest_clean_recovered",
	"linear_copy_candidates", "linear_copy_reject_shape", "linear_copy_reject_descriptor", "linear_copy_reject_parameters",
	"linear_copy_reject_count", "linear_copy_reject_overlap", "linear_copy_reject_alias", "linear_copy_cpu", "linear_copy_gpu", "linear_copy_bytes"
};
static_assert(ExtraNames.size() == static_cast<size_t>(Extra::Count));
inline std::atomic<uint64_t> hot_owner_count {0}, hot_owner_bytes {0};

inline constexpr uint32_t QueueKinds = 2; // 0 = graphics, 1 = async compute.
inline constexpr std::array QueueKindNames {"gfx", "compute"};

// Upper bounds (exclusive) of the idle-wait histogram, in microseconds; last bucket is open.
inline constexpr std::array<uint64_t, 6> IdleBucketLimitsUs {500, 1500, 4000, 8000, 17000, 34000};
inline constexpr size_t IdleBuckets = IdleBucketLimitsUs.size() + 1;

inline constexpr size_t MaxWaitRegMemSamples = 8;

struct WaitRegMemSample {
	std::atomic<uint64_t> address {0}, reference {0}, mask {0}, value {0};
	std::atomic<uint32_t> function {0}, width {0}, queue_kind {0};
};

namespace Detail {
inline bool Armed() {
	static const bool armed = [] {
		const auto* value = std::getenv("KYTY_SUBMIT_PROFILE");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
	}();
	return armed;
}
inline std::atomic<bool> recording {false};
inline std::atomic<bool> experiment {false};
inline std::atomic<bool> compact_operands {false};
// Tests that drive the scheduler's native command buffer turn this off and enable the recorder by hand.
inline std::atomic<bool> vk_recorder_managed {true};
// Diagnostics outside common (e.g. the SRT descriptor probe) flush their window here.
inline std::atomic<void (*)()> window_close_hook {nullptr};

using Counter = std::atomic<uint64_t>;
template <size_t N>
using Counters = std::array<Counter, N>;

// Last thread seen for a row, plus whether more than one thread contributed in the window.
struct ThreadTag {
	std::atomic<uint32_t> tid {0};
	std::atomic<bool>     multi {false};
};

inline constexpr size_t SubmitRows = static_cast<size_t>(Reason::Count) * static_cast<size_t>(Origin::Count);
inline constexpr size_t SiteRows   = static_cast<size_t>(WaitSite::Count);

struct Window {
	Counters<SubmitRows> submits {};
	Counters<SubmitRows> submits_without_draw_or_dispatch {};
	Counters<SubmitRows> draws {};
	Counters<SubmitRows> dispatches {};
	std::array<ThreadTag, SubmitRows> submit_threads {};
	Counters<SiteRows> waits {};
	Counters<SiteRows> wait_ns {};
	Counters<SiteRows> wait_max_ns {};
	std::array<ThreadTag, SiteRows> wait_threads {};
	Counters<SiteRows> readback_requested_bytes {};
	Counters<SiteRows> readback_copied_bytes {};
	Counters<static_cast<size_t>(Block::Count) * QueueKinds> blocks {};
	Counters<8> wait_reg_mem_functions {}; // Indexed by compare function 0..6, 7 = other.
	Counter idle_waits {0}, idle_timeouts {0}, idle_ns {0}, idle_max_ns {0};
	Counters<IdleBuckets> idle_histogram {};
	Counter reevaluations {0}, reevaluation_ns {0}, reevaluation_max_ns {0};
	// Durations that started before the current window began.
	Counter dropped_waits {0}, dropped_idle_waits {0}, dropped_reevaluations {0};
	std::array<WaitRegMemSample, MaxWaitRegMemSamples> samples {};
	Counter sample_count {0};
	Counters<static_cast<size_t>(Extra::Count)> extra {};
};
inline std::atomic<uint64_t> frame_index {0};
inline Window window;
inline std::atomic<uint64_t> generation {1};
inline std::atomic<uint64_t> window_start_ns {0};
inline std::atomic<uint32_t> gpu_thread_tid {0};

inline thread_local Reason   current_reason    = Reason::Other;
inline thread_local Origin   current_origin    = Origin::Outside;
inline thread_local WaitSite current_wait_site = WaitSite::Other;
// Per-thread state, valid only for `thread_generation`.
inline thread_local uint64_t thread_generation  = 0;
inline thread_local uint64_t pending_draws      = 0;
inline thread_local uint64_t pending_dispatches = 0;
inline thread_local Block    last_block         = Block::Count;

inline Reason&   ReasonSlot() { return current_reason; }
inline Origin&   OriginSlot() { return current_origin; }
inline WaitSite& WaitSiteSlot() { return current_wait_site; }

inline void SyncThread() {
	const auto current = generation.load(std::memory_order_relaxed);
	if (thread_generation != current) {
		thread_generation  = current;
		pending_draws      = 0;
		pending_dispatches = 0;
		last_block         = Block::Count;
	}
}
inline void Max(Counter& target, uint64_t value) {
	auto previous = target.load(std::memory_order_relaxed);
	while (previous < value &&
	       !target.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {
	}
}
inline void Add(Counter& target, uint64_t value = 1) {
	target.fetch_add(value, std::memory_order_relaxed);
}
inline uint64_t Take(Counter& source) {
	return source.exchange(0, std::memory_order_relaxed);
}
inline void Tag(ThreadTag& tag, uint32_t tid) {
	const auto previous = tag.tid.exchange(tid, std::memory_order_relaxed);
	if (previous != 0 && previous != tid) tag.multi.store(true, std::memory_order_relaxed);
}
inline void TakeTag(ThreadTag& tag, uint32_t& tid, bool& multi) {
	tid   = tag.tid.exchange(0, std::memory_order_relaxed);
	multi = tag.multi.exchange(false, std::memory_order_relaxed);
}
} // namespace Detail

[[nodiscard]] inline bool Armed() {
	return Detail::Armed();
}
[[nodiscard]] inline bool Recording() {
	return Detail::Armed() && Detail::recording.load(std::memory_order_relaxed);
}
// Candidate under A/B (see SubmitProfileRuntime); off unless the experiment gate is open.
enum class Experiment : uint32_t {
	None = 0, ReleaseMem = 1, IndirectCountGpu = 2,
	LegacyIndexClamp = 4, NoIndirectMeshGpu = 8, NoWalkerControlGpu = 16, NoGpuTimestamps = 32,
	GuestHostImport = 64, HotUnprotected = 128, FaultCoarsening = 256, VkRecorder = 512,
	SrtReadbackBatch = 1024, WriteReadbackAhead = 2048, ReadReadbackAhead = 4096, NoReadbackAhead = 8192,
	ReadbackAheadSparse = 16384, NoVkRecorder = 32768, GuestCleanReads = 65536, DsLinearCopy = 131072,
	DsGpuPages = 262144
};
inline Experiment ParseExperiment(std::string_view names) {
	uint32_t mask = 0;
	while (!names.empty()) {
		const auto comma = names.find(',');
		auto name = names.substr(0, comma);
		const auto first = name.find_first_not_of(" \t\r\n");
		if (first != std::string_view::npos) {
			name = name.substr(first, name.find_last_not_of(" \t\r\n") - first + 1);
			if (name == "release-mem") mask |= static_cast<uint32_t>(Experiment::ReleaseMem);
			if (name == "indirect-count-gpu") mask |= static_cast<uint32_t>(Experiment::IndirectCountGpu);
			if (name == "legacy-index-clamp") mask |= static_cast<uint32_t>(Experiment::LegacyIndexClamp);
			if (name == "no-indirect-mesh-gpu") mask |= static_cast<uint32_t>(Experiment::NoIndirectMeshGpu);
			if (name == "no-walker-control-gpu") mask |= static_cast<uint32_t>(Experiment::NoWalkerControlGpu);
			if (name == "no-gpu-timestamps") mask |= static_cast<uint32_t>(Experiment::NoGpuTimestamps);
			if (name == "guest-host-import") mask |= static_cast<uint32_t>(Experiment::GuestHostImport);
			if (name == "hot-unprotected") mask |= static_cast<uint32_t>(Experiment::HotUnprotected);
			if (name == "fault-coarsening") mask |= static_cast<uint32_t>(Experiment::FaultCoarsening);
			if (name == "vk-recorder") mask |= static_cast<uint32_t>(Experiment::VkRecorder);
			if (name == "srt-readback-batch") mask |= static_cast<uint32_t>(Experiment::SrtReadbackBatch);
			if (name == "write-readback-ahead") mask |= static_cast<uint32_t>(Experiment::WriteReadbackAhead);
			if (name == "read-readback-ahead") mask |= static_cast<uint32_t>(Experiment::ReadReadbackAhead);
			if (name == "no-readback-ahead") mask |= static_cast<uint32_t>(Experiment::NoReadbackAhead);
			if (name == "readback-ahead-sparse") mask |= static_cast<uint32_t>(Experiment::ReadbackAheadSparse);
			if (name == "no-vk-recorder") mask |= static_cast<uint32_t>(Experiment::NoVkRecorder);
			if (name == "guest-clean-reads") mask |= static_cast<uint32_t>(Experiment::GuestCleanReads);
			if (name == "ds-linear-copy") mask |= static_cast<uint32_t>(Experiment::DsLinearCopy);
			if (name == "ds-gpu-pages") mask |= static_cast<uint32_t>(Experiment::DsGpuPages);
			// gpu-timestamps is retired: native clocks are now the default.
			// Like other promoted experiment names, old helpers may keep it as a no-op.
		}
		if (comma == std::string_view::npos) break;
		names.remove_prefix(comma + 1);
	}
	return static_cast<Experiment>(mask);
}
inline Experiment SelectedExperiment() {
	static const auto selected = [] {
		const auto* value = std::getenv("KYTY_EXPERIMENT");
		return ParseExperiment(value == nullptr ? "" : value);
	}();
	return selected;
}
inline const char* ExperimentName() {
	// Canonical names only: no environment text is interpolated into the JSON log.
	static const auto name = [] {
		std::string result;
		constexpr std::array names {"release-mem", "indirect-count-gpu", "legacy-index-clamp",
		    "no-indirect-mesh-gpu", "no-walker-control-gpu", "no-gpu-timestamps", "guest-host-import", "hot-unprotected", "fault-coarsening", "vk-recorder", "srt-readback-batch", "write-readback-ahead", "read-readback-ahead", "no-readback-ahead", "readback-ahead-sparse", "no-vk-recorder", "guest-clean-reads", "ds-linear-copy", "ds-gpu-pages"};
		for (uint32_t bit = 0; bit < names.size(); ++bit) {
			if (!(static_cast<uint32_t>(SelectedExperiment()) & (1u << bit))) continue;
			if (!result.empty()) result += ',';
			result += names[bit];
		}
		return result.empty() ? std::string("none") : result;
	}();
	return name.c_str();
}
[[nodiscard]] inline bool ExperimentSelected(Experiment selected) {
	const auto mask = static_cast<uint32_t>(selected);
	return mask != 0 && (static_cast<uint32_t>(SelectedExperiment()) & mask) == mask;
}
[[nodiscard]] inline bool ExperimentActive() {
	return Detail::experiment.load(std::memory_order_relaxed);
}
[[nodiscard]] inline bool ExperimentActive(Experiment selected) {
	return ExperimentSelected(selected) && ExperimentActive();
}
[[nodiscard]] inline bool GpuTimestampsEnabled() {
	return !ExperimentActive(Experiment::NoGpuTimestamps);
}
// Promoted after Demon's Souls A/B/A (+8 %, Astro unaffected): neighbouring GPU-dirty pages are
// downloaded together on read and write faults. "no-readback-ahead" restores single-page readbacks;
// the old "write-readback-ahead"/"read-readback-ahead" names are accepted with no effect.
[[nodiscard]] inline bool ReadbackAheadEnabled() {
	return !ExperimentActive(Experiment::NoReadbackAhead);
}
// Promoted after A/B/A on all three games (Demon's Souls +8 %, Astro Sky Garden +5 %, galaxy menu
// +3 %, Sons of Sparta unchanged at 60): the guest command scheduler records through the vk-recorder.
// "no-vk-recorder" switches back to direct command buffers; the old "vk-recorder" name has no effect.
[[nodiscard]] inline bool VkRecorderEnabled() {
	return !ExperimentActive(Experiment::NoVkRecorder);
}
[[nodiscard]] inline bool VkRecorderManaged() {
	return Detail::vk_recorder_managed.load(std::memory_order_relaxed);
}
inline void SetVkRecorderManaged(bool managed) {
	Detail::vk_recorder_managed.store(managed, std::memory_order_relaxed);
}
inline void SetExperiment(bool active) {
	Detail::experiment.store(active, std::memory_order_relaxed);
}
// Compact SRT operands (+1.8 % in one A/B, not yet confirmed without profiling): off by default.
[[nodiscard]] inline bool CompactOperandsActive() {
	return Detail::compact_operands.load(std::memory_order_relaxed);
}
inline void SetCompactOperands(bool active) {
	Detail::compact_operands.store(active, std::memory_order_relaxed);
}
inline void SetRecording(bool recording) {
	Detail::recording.store(recording && Detail::Armed(), std::memory_order_relaxed);
}
[[nodiscard]] inline WaitSite CurrentWaitSite() {
	return Detail::current_wait_site;
}
[[nodiscard]] inline uint64_t NowNs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

// Fault/lock diagnostics: fixed storage, no allocation or output in a handler.
// Roles describe the owning thread (a host copy on a guest thread is still guest).
namespace Diagnostics {
enum class Role : uint8_t { Host, Guest, GuestGpu };
enum class Metric : uint8_t {
	ReadFault, WriteFault, UnhandledFault, TrackingLock, TextureLock, ImageHeapLock,
	FaultTrackingLock, FaultTextureLock, FaultImageHeapLock, WriteReadbackRequest, Count
};
inline constexpr std::array MetricNames {
	"read_fault", "write_fault", "unhandled_fault", "tracking_lock", "texture_lock", "image_heap_lock",
	"fault_tracking_lock", "fault_texture_lock", "fault_image_heap_lock", "write_readback_request"
};
static_assert(MetricNames.size() == static_cast<size_t>(Metric::Count));
inline constexpr std::array RoleNames {"host", "guest", "guest_gpu"};
struct Counters {
	Detail::Counter calls {0}, samples {0}, ns {0}, max_ns {0}, dropped {0};
};
struct Row {
	// Stable for the session: never reuse a slot while a producer may hold it.
	std::atomic<uint64_t> identity {0};
	std::array<Counters, MetricNames.size()> metrics {};
};
inline std::array<Row, 256> rows;
inline Detail::Counter overflow {0};
inline thread_local Role role = Role::Host;
inline thread_local Row* cached_row = nullptr;
inline thread_local bool row_exhausted = false;
inline thread_local std::array<uint32_t, MetricNames.size()> sequences {};
inline thread_local bool in_fault = false, sample_fault = false;

inline uint32_t ThreadId() {
#if defined(_WIN32)
	return GetCurrentThreadId();
#elif defined(__APPLE__)
	return pthread_mach_thread_np(pthread_self());
#else
	return static_cast<uint32_t>(::syscall(SYS_gettid));
#endif
}
inline void SetRole(Role value) {
	role = value;
	cached_row = nullptr;
	row_exhausted = false;
	if (value == Role::GuestGpu) Detail::gpu_thread_tid.store(ThreadId(), std::memory_order_relaxed);
}
inline Row* GetRow() {
	if (cached_row != nullptr) return cached_row;
	if (row_exhausted) { Detail::Add(overflow); return nullptr; }
	const auto tid = ThreadId();
	const uint64_t identity = (uint64_t {tid} << 2u) | (static_cast<uint8_t>(role) + 1u);
	for (size_t i = 0; i < rows.size(); ++i) {
		auto& row = rows[(tid + i) % rows.size()];
		auto key = row.identity.load(std::memory_order_relaxed);
		if (key == identity || (key == 0 && row.identity.compare_exchange_strong(key, identity))) {
			return cached_row = &row;
		}
	}
	Detail::Add(overflow);
	row_exhausted = true;
	return nullptr;
}
inline bool Sample(Metric metric) {
	// One clocked event per 256 of each kind on each thread. Keep phase across
	// report windows; otherwise sparse readers may never produce a sample.
	return (++sequences[static_cast<size_t>(metric)] & 255u) == 0;
}
inline void Record(Row* row, Metric metric, bool sampled, uint64_t start, uint64_t generation) {
	if (row == nullptr || !Recording()) return;
	auto& counters = row->metrics[static_cast<size_t>(metric)];
	Detail::Add(counters.calls);
	if (!sampled) return;
	if (generation != Detail::generation.load(std::memory_order_relaxed)) {
		Detail::Add(counters.dropped);
		return;
	}
	const auto elapsed = NowNs() - start;
	Detail::Add(counters.samples);
	Detail::Add(counters.ns, elapsed);
	Detail::Max(counters.max_ns, elapsed);
}

// All GuestGpu read faults, independently of the 1/256 lock sample. Publish after
// fault recovery; an outer draw can still own emulator locks. Fixed storage and
// try_lock keep producers nonblocking. No allocation/printing/decoding under this mutex.
namespace ReadFaults {
struct Range { uint64_t start = 0, end = 0; };
struct Event {
	uint64_t rip = 0, page = 0, fault = 0, read_address = 0, read_size = 0;
	uint64_t wait_ns = 0, priority_ns = 0, handler_ns = 0, bytes = 0, ranges = 0;
	bool byte_dirty = false, read_dirty = false, page_dirty = false, served = false, srt_walker = false;
	std::array<Range, 4> sample_ranges {};
};
struct Totals {
	uint64_t calls = 0, wait_ns = 0, priority_ns = 0, handler_ns = 0;
	uint64_t bytes = 0, ranges = 0, max_bytes = 0, no_download = 0;
	uint64_t decoded = 0, unknown = 0, false_shared = 0, byte_clean = 0;
	uint64_t served = 0, srt_walker_calls = 0;
	void Add(const Event& e) {
		++calls; wait_ns += e.wait_ns; priority_ns += e.priority_ns; handler_ns += e.handler_ns;
		bytes += e.bytes; ranges += e.ranges; max_bytes = std::max(max_bytes, e.bytes);
		no_download += e.ranges == 0;
		decoded += e.read_size != 0; unknown += e.read_size == 0;
		// A known whole load misses every copied dirty interval, while another
		// dirty interval shares the fault page. Unknown widths are never classified.
		const bool same_page = e.read_size != 0 && e.read_address >= e.page &&
		                       e.read_address - e.page < 4096 &&
		                       e.read_size <= 4096 - (e.read_address - e.page);
		false_shared += e.served || (same_page && e.page_dirty && !e.read_dirty);
		byte_clean += e.served || (e.page_dirty && !e.byte_dirty);
		served += e.served;
		srt_walker_calls += e.srt_walker;
	}
};
struct Entry { uint64_t rip = 0, page = 0; Totals totals; Event last; };
struct Table {
	std::array<Entry, 64> rips {};
	std::array<Entry, 256> pages {};
	Totals total;
	uint64_t rip_overflow = 0, page_overflow = 0;
};
inline Table table;
inline std::mutex mutex;
inline Detail::Counter dropped_busy {0}, dropped_boundary {0};
class Scope;
inline thread_local Scope* active = nullptr;
template <size_t N>
inline bool Insert(std::array<Entry, N>& entries, const Event& e, bool by_page) {
	const auto page = by_page ? e.page : 0;
	for (auto& entry: entries) {
		if (entry.totals.calls == 0 || (entry.rip == e.rip && entry.page == page)) {
			entry.rip = e.rip; entry.page = page; entry.totals.Add(e); entry.last = e;
			return true;
		}
	}
	return false;
}
class Scope {
public:
	Scope(bool read, uint64_t rip, uint64_t address): m_previous(active) {
		// Nested faults must not charge their readbacks to the outer instruction.
		active = nullptr;
		if (!read || role != Role::GuestGpu || !Recording()) return;
		m_enabled = true;
		m_generation = Detail::generation.load(std::memory_order_relaxed);
		m_start = NowNs();
		m_event.rip = rip; m_event.fault = address; m_event.page = address & ~uint64_t {4095};
		active = this;
	}
	~Scope() {
		active = m_previous;
		if (!m_enabled || !m_handled) return;
		if (!Recording() || m_generation != Detail::generation.load(std::memory_order_relaxed)) {
			Detail::Add(dropped_boundary); return;
		}
		m_event.handler_ns = NowNs() - m_start;
		std::unique_lock lock(mutex, std::try_to_lock);
		if (!lock.owns_lock()) { Detail::Add(dropped_busy); return; }
		if (m_generation != Detail::generation.load(std::memory_order_relaxed)) {
			Detail::Add(dropped_boundary); return;
		}
		table.total.Add(m_event);
		table.rip_overflow += !Insert(table.rips, m_event, false);
		table.page_overflow += !Insert(table.pages, m_event, true);
	}
	[[nodiscard]] bool Enabled() const { return m_enabled; }
	void Handled() { m_handled = true; }
	void ServedClean() { m_event.served = true; }
	void SrtWalker(bool value) { m_event.srt_walker = value; }
	void ReadAccess(uint64_t address, uint64_t size) {
		m_event.read_address = address; m_event.read_size = size;
	}
	void Download(uint64_t start, uint64_t end) {
		if (end <= start) return;
		auto& e = m_event;
		if (e.ranges < e.sample_ranges.size()) e.sample_ranges[e.ranges] = {start, end};
		++e.ranges; e.bytes += end - start;
		e.byte_dirty |= start <= e.fault && e.fault < end;
		// Subtractions avoid end-address overflow for the last address-space page.
		e.page_dirty |= end > e.page && (start < e.page || start - e.page < 4096);
		e.read_dirty |= e.read_size != 0 && start < e.read_address + e.read_size && end > e.read_address;
	}
	void Wait(uint64_t ns, WaitSite site) {
		if (site == WaitSite::ReadbackCpuReadFault) m_event.wait_ns += ns;
		else if (site == WaitSite::ReadbackPriorityOps) m_event.priority_ns += ns;
	}
	Scope(const Scope&) = delete;
	Scope& operator=(const Scope&) = delete;
private:
	Scope* m_previous;
	Event m_event;
	uint64_t m_generation = 0, m_start = 0;
	bool m_enabled = false, m_handled = false;
};
inline void Download(uint64_t start, uint64_t end) {
	if (active != nullptr && Detail::current_wait_site == WaitSite::ReadbackCpuReadFault)
		active->Download(start, end);
}
inline void Wait(uint64_t ns, WaitSite site) {
	if (active != nullptr) active->Wait(ns, site);
}
inline void Reset() {
	std::lock_guard lock(mutex);
	table = {};
	Detail::Take(dropped_busy); Detail::Take(dropped_boundary);
}
inline void PrintTotals(std::FILE* out, const Totals& t) {
	std::fprintf(out, "\"calls\":%llu,\"wait_ns\":%llu,\"priority_ns\":%llu,\"handler_ns\":%llu,"
	                 "\"download_bytes\":%llu,\"download_ranges\":%llu,\"max_download_bytes\":%llu,"
	                 "\"no_download\":%llu,\"decoded\":%llu,\"unknown_width\":%llu,"
	                 "\"false_shared\":%llu,\"fault_byte_clean\":%llu,\"false_shared_served\":%llu,\"srt_walker_calls\":%llu",
	    (unsigned long long)t.calls, (unsigned long long)t.wait_ns, (unsigned long long)t.priority_ns,
	    (unsigned long long)t.handler_ns, (unsigned long long)t.bytes, (unsigned long long)t.ranges,
	    (unsigned long long)t.max_bytes, (unsigned long long)t.no_download,
	    (unsigned long long)t.decoded, (unsigned long long)t.unknown,
	    (unsigned long long)t.false_shared, (unsigned long long)t.byte_clean, (unsigned long long)t.served,
	    (unsigned long long)t.srt_walker_calls);
}
template <size_t N>
inline void PrintTop(std::FILE* out, std::array<Entry, N>& entries, bool by_page) {
	std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
		if (a.totals.wait_ns != b.totals.wait_ns) return a.totals.wait_ns > b.totals.wait_ns;
		return a.totals.calls > b.totals.calls;
	});
	for (size_t i = 0; i < 16 && i < entries.size() && entries[i].totals.calls != 0; ++i) {
		const auto& e = entries[i];
		std::fprintf(out, "%s{\"rip\":\"0x%llx\",", i ? "," : "", (unsigned long long)e.rip);
#if defined(_WIN32)
		// Reporting only, outside the exception handler. ASLR-safe offline symbolization.
		MEMORY_BASIC_INFORMATION memory {};
		if (VirtualQuery(reinterpret_cast<const void*>(e.rip), &memory, sizeof(memory)) != 0 &&
		    memory.Type == MEM_IMAGE) {
			const auto base = reinterpret_cast<uint64_t>(memory.AllocationBase);
			std::fprintf(out, "\"module_base\":\"0x%llx\",\"rva\":\"0x%llx\",\"main_module\":%s,",
			    (unsigned long long)base, (unsigned long long)(e.rip - base),
			    memory.AllocationBase == GetModuleHandleW(nullptr) ? "true" : "false");
		}
#endif
		if (by_page) std::fprintf(out, "\"page\":\"0x%llx\",", (unsigned long long)e.page);
		PrintTotals(out, e.totals);
		if (by_page) {
			const auto& s = e.last;
			std::fprintf(out, ",\"last_sample\":{\"fault\":\"0x%llx\",\"read_address\":\"0x%llx\","
			                 "\"read_size\":%llu,\"download_ranges\":%llu,\"ranges\":[",
			    (unsigned long long)s.fault, (unsigned long long)s.read_address,
			    (unsigned long long)s.read_size, (unsigned long long)s.ranges);
			for (size_t j = 0; j < s.sample_ranges.size() && j < s.ranges; ++j) {
				std::fprintf(out, "%s{\"start\":\"0x%llx\",\"bytes\":%llu}", j ? "," : "",
				    (unsigned long long)s.sample_ranges[j].start,
				    (unsigned long long)(s.sample_ranges[j].end - s.sample_ranges[j].start));
			}
			std::fprintf(out, "]}");
		}
		std::fprintf(out, "}");
	}
}
inline void Report(std::FILE* out) {
	// Snapshot under a short lock, then sort and print without blocking producers.
	Table snapshot;
	{
		std::lock_guard lock(mutex);
		snapshot = table; table = {};
	}
	std::fprintf(out, ",\"gpu_read_faults\":{\"page_size\":4096,\"top_limit\":16,"
	                 "\"rip_capacity\":64,\"page_capacity\":256,\"dropped_busy\":%llu,"
	                 "\"dropped_boundary\":%llu,\"rip_overflow\":%llu,\"page_overflow\":%llu,\"total\":{",
	    (unsigned long long)Detail::Take(dropped_busy), (unsigned long long)Detail::Take(dropped_boundary),
	    (unsigned long long)snapshot.rip_overflow, (unsigned long long)snapshot.page_overflow);
	PrintTotals(out, snapshot.total);
	std::fprintf(out, "},\"top_rips\":["); PrintTop(out, snapshot.rips, false);
	std::fprintf(out, "],\"top_pages\":["); PrintTop(out, snapshot.pages, true);
	std::fprintf(out, "]}");
}
} // namespace ReadFaults

class FaultScope {
public:
	explicit FaultScope(bool write): m_metric(write ? Metric::WriteFault : Metric::ReadFault) {
		if (!Recording()) return;
		m_row = GetRow();
		m_previous_fault = in_fault;
		m_previous_sample = sample_fault;
		m_active = true;
		in_fault = true;
		sample_fault = m_sampled = Sample(m_metric);
		m_generation = Detail::generation.load(std::memory_order_relaxed);
		if (m_sampled) m_start = NowNs();
	}
	~FaultScope() {
		if (!m_active) return;
		Record(m_row, m_handled ? m_metric : Metric::UnhandledFault, m_sampled, m_start, m_generation);
		in_fault = m_previous_fault;
		sample_fault = m_previous_sample;
	}
	void Handled() { m_handled = true; }
	FaultScope(const FaultScope&) = delete;
	FaultScope& operator=(const FaultScope&) = delete;
private:
	Row* m_row = nullptr;
	Metric m_metric;
	uint64_t m_start = 0, m_generation = 0;
	bool m_active = false, m_sampled = false, m_handled = false;
	bool m_previous_fault = false, m_previous_sample = false;
};
class LockTimer {
public:
	explicit LockTimer(Metric metric): m_write_stage(metric == Metric::TrackingLock ? GuestCpuProfile::Part::TrackingLock :
	    metric == Metric::TextureLock ? GuestCpuProfile::Part::TextureLock : GuestCpuProfile::Part::ImageHeapLock), m_metric(metric) {
		if (!Recording()) return;
		m_row = GetRow();
		// Fault-side lock timings are conditional on the same sampled faults;
		// all other acquisitions use an independent 1/256 sequence per lock kind.
		m_sampled = in_fault ? sample_fault : Sample(metric);
		if (in_fault) m_metric = static_cast<Metric>(static_cast<size_t>(metric) + 3u);
		m_generation = Detail::generation.load(std::memory_order_relaxed);
		if (m_sampled) m_start = NowNs();
	}
	void Acquired() { m_write_stage.End(); Record(m_row, m_metric, m_sampled, m_start, m_generation); }
private:
	GuestCpuProfile::Stage m_write_stage;
	Row* m_row = nullptr;
	Metric m_metric;
	uint64_t m_start = 0, m_generation = 0;
	bool m_sampled = false;
};
template <typename Mutex, Metric metric>
class ProfiledMutex {
public:
	void lock() { LockTimer timer(metric); m_mutex.lock(); timer.Acquired(); }
	void unlock() { m_mutex.unlock(); }
	bool try_lock() { return m_mutex.try_lock(); }
private:
	Mutex m_mutex;
};
inline void WriteReadbackRequest() {
	if (Recording()) Record(GetRow(), Metric::WriteReadbackRequest, false, 0, 0);
}
inline void Reset() {
	ReadFaults::Reset();
	for (auto& row: rows) for (auto& c: row.metrics) {
		Detail::Take(c.calls); Detail::Take(c.samples); Detail::Take(c.ns);
		Detail::Take(c.max_ns); Detail::Take(c.dropped);
	}
	Detail::Take(overflow);
}
inline void Report(std::FILE* out) {
	ReadFaults::Report(out);
	std::fprintf(out, ",\"fault_lock_profile\":{\"sample_period\":256,\"overflow\":%llu,\"rows\":[",
	             static_cast<unsigned long long>(Detail::Take(overflow)));
	bool comma = false;
	for (auto& row: rows) {
		const auto key = row.identity.load(std::memory_order_relaxed);
		if (key == 0) continue;
		for (size_t i = 0; i < row.metrics.size(); ++i) {
			auto& c = row.metrics[i];
			const auto calls = Detail::Take(c.calls), samples = Detail::Take(c.samples);
			const auto ns = Detail::Take(c.ns), max = Detail::Take(c.max_ns), dropped = Detail::Take(c.dropped);
			if ((calls | samples | ns | max | dropped) == 0) continue;
			std::fprintf(out, "%s{\"tid\":%u,\"role\":\"%s\",\"site\":\"%s\",\"calls\":%llu,"
			                 "\"samples\":%llu,\"sample_ns\":%llu,\"sample_max_ns\":%llu,\"dropped\":%llu}",
			             comma ? "," : "", static_cast<uint32_t>(key >> 2u), RoleNames[(key & 3u) - 1u],
			             MetricNames[i], static_cast<unsigned long long>(calls),
			             static_cast<unsigned long long>(samples), static_cast<unsigned long long>(ns),
			             static_cast<unsigned long long>(max), static_cast<unsigned long long>(dropped));
			comma = true;
		}
	}
	std::fprintf(out, "]}");
}
} // namespace Diagnostics

// Scopes only touch thread-local state when armed.
template <typename T, T& (*Slot)()>
class TagScope {
public:
	explicit TagScope(T value) {
		if (!Detail::Armed()) return;
		m_active   = true;
		m_previous = Slot();
		Slot()     = value;
	}
	~TagScope() {
		if (m_active) Slot() = m_previous;
	}
	TagScope(const TagScope&)            = delete;
	TagScope& operator=(const TagScope&) = delete;

private:
	bool m_active   = false;
	T    m_previous {};
};
using ReasonScope   = TagScope<Reason, Detail::ReasonSlot>;
using OriginScope   = TagScope<Origin, Detail::OriginSlot>;
using WaitSiteScope = TagScope<WaitSite, Detail::WaitSiteSlot>;

inline void NoteDraw() {
	if (!Recording()) return;
	Detail::SyncThread();
	++Detail::pending_draws;
}
inline void NoteDispatch() {
	if (!Recording()) return;
	Detail::SyncThread();
	++Detail::pending_dispatches;
}

inline void RecordSubmit(uint32_t tid) {
	if (!Recording()) return;
	Detail::SyncThread();
	auto&      w   = Detail::window;
	const auto row = static_cast<size_t>(Detail::current_reason) *
	                     static_cast<size_t>(Origin::Count) +
	                 static_cast<size_t>(Detail::current_origin);
	Detail::Add(w.submits[row]);
	Detail::Add(w.draws[row], Detail::pending_draws);
	Detail::Add(w.dispatches[row], Detail::pending_dispatches);
	if (Detail::pending_draws == 0 && Detail::pending_dispatches == 0) {
		Detail::Add(w.submits_without_draw_or_dispatch[row]);
	}
	Detail::Tag(w.submit_threads[row], tid);
	Detail::pending_draws = Detail::pending_dispatches = 0;
}

// Measures one blocking MasterSemaphore wait; attributes it to the innermost WaitSiteScope.
class WaitTimer {
public:
	explicit WaitTimer(uint32_t tid) {
		if (!Recording()) return;
		m_tid        = tid;
		m_generation = Detail::generation.load(std::memory_order_relaxed);
		m_start      = NowNs();
	}
	~WaitTimer() {
		if (m_start == 0 || !Recording()) return;
		auto& w = Detail::window;
		if (Detail::generation.load(std::memory_order_relaxed) != m_generation) {
			Detail::Add(w.dropped_waits);
			return;
		}
		const auto elapsed = NowNs() - m_start;
		const auto s       = static_cast<size_t>(Detail::current_wait_site);
		Diagnostics::ReadFaults::Wait(elapsed, Detail::current_wait_site);
		Detail::Add(w.waits[s]);
		Detail::Add(w.wait_ns[s], elapsed);
		Detail::Max(w.wait_max_ns[s], elapsed);
		Detail::Tag(w.wait_threads[s], m_tid);
	}
	WaitTimer(const WaitTimer&)            = delete;
	WaitTimer& operator=(const WaitTimer&) = delete;

private:
	uint32_t m_tid        = 0;
	uint64_t m_generation = 0;
	uint64_t m_start      = 0;
};

inline void AddExtra(Extra extra, uint64_t value = 1) {
	if (Recording()) Detail::Add(Detail::window.extra[static_cast<size_t>(extra)], value);
}

// Per record, not multiplied by instances. Available is the remaining hardware
// bound; valid is the portion actually requested inside it. Zero-instance records
// are reported too, but do not imply vertex work.
inline void RecordIndirectIndices(uint32_t requested, uint32_t first, uint32_t limit,
                                  uint32_t instances, uint32_t issued) {
	if (!Recording()) return;
	const uint32_t available = first < limit ? limit - first : 0;
	AddExtra(Extra::IndirectIndexSamples);
	AddExtra(Extra::IndirectIndexRequested, requested);
	AddExtra(Extra::IndirectIndexAvailable, available);
	AddExtra(Extra::IndirectIndexValid, std::min(requested, available));
	AddExtra(Extra::IndirectIndexIssued, issued);
	AddExtra(Extra::IndirectIndexInstances, instances);
	if (!instances) AddExtra(Extra::IndirectIndexZeroInstances);
}

// Called once per guest flip; frames delimit the materialization repetition sets.
inline void NoteFlip() {
	if (!Recording()) return;
	Detail::frame_index.fetch_add(1, std::memory_order_relaxed);
	AddExtra(Extra::Flips);
}

// Diagnostic only: does a resource plan produce the same materialized output again? output is
// a hash of values the materialization already produced; nothing is re-read from guest memory.
// Per-thread bounded sets; allocation happens only while recording.
inline void NoteMaterialization(const void* program, uint64_t output, uint64_t hash_ns) {
	if (!Recording()) return;
	struct Sets {
		uint64_t                                frame = UINT64_MAX;
		std::unordered_set<uint64_t>            current, previous;
		std::unordered_map<const void*, uint64_t> last;
	};
	static thread_local Sets sets;
	const auto frame = Detail::frame_index.load(std::memory_order_relaxed);
	if (frame != sets.frame) {
		sets.previous.swap(sets.current);
		sets.current.clear();
		if (frame != sets.frame + 1) sets.previous.clear();
		sets.frame = frame;
	}
	if (sets.current.size() > 65536 || sets.last.size() > 65536) {
		sets.current.clear();
		sets.last.clear();
	}
	const auto key = output ^ (reinterpret_cast<uint64_t>(program) * 0x9E3779B97F4A7C15ull);
	AddExtra(Extra::MaterializeCalls);
	AddExtra(Extra::MaterializeHashNs, hash_ns);
	auto [it, inserted] = sets.last.try_emplace(program, output);
	if (!inserted) {
		if (it->second == output) AddExtra(Extra::MaterializeRepeatLastForProgram);
		it->second = output;
	}
	if (!sets.current.insert(key).second) AddExtra(Extra::MaterializeRepeatSameFrame);
	else if (sets.previous.contains(key)) AddExtra(Extra::MaterializeRepeatPrevFrame);
}

// Bytes a synchronous readback was asked for and actually copied, by current wait site.
inline void RecordReadbackBytes(uint64_t requested, uint64_t copied) {
	if (!Recording()) return;
	const auto s = static_cast<size_t>(Detail::current_wait_site);
	Detail::Add(Detail::window.readback_requested_bytes[s], requested);
	Detail::Add(Detail::window.readback_copied_bytes[s], copied);
}

// Forget the previous stream's suspension cause before processing another one.
inline void BeginProcessing() {
	if (!Recording()) return;
	Detail::SyncThread();
	Detail::last_block = Block::Count;
}

inline void NoteBlock(Block block) {
	if (!Recording()) return;
	Detail::SyncThread();
	Detail::last_block = block;
}

// Called with values the existing WAIT_REG_MEM evaluation already read; never reads guest memory.
inline void NoteWaitRegMem(uint32_t queue_kind, const volatile void* address, uint64_t value,
                           uint64_t reference, uint64_t mask, uint32_t function, uint32_t width) {
	if (!Recording()) return;
	Detail::SyncThread();
	Detail::last_block = Block::WaitRegMem;
	auto& w = Detail::window;
	Detail::Add(w.wait_reg_mem_functions[function < 7 ? function : 7]);
	const auto index = w.sample_count.fetch_add(1, std::memory_order_relaxed);
	if (index < MaxWaitRegMemSamples) {
		auto& s = w.samples[index];
		s.address.store(reinterpret_cast<uint64_t>(address), std::memory_order_relaxed);
		s.value.store(value, std::memory_order_relaxed);
		s.reference.store(reference, std::memory_order_relaxed);
		s.mask.store(mask, std::memory_order_relaxed);
		s.function.store(function, std::memory_order_relaxed);
		s.width.store(width, std::memory_order_relaxed);
		s.queue_kind.store(queue_kind, std::memory_order_relaxed);
	}
}

// The submission on `queue_kind` returned incomplete; attribute it to the last cause noted
// while processing it (unknown if none was noted, never guessed).
inline void RecordBlocked(uint32_t queue_kind) {
	if (!Recording()) return;
	Detail::SyncThread();
	const auto block = Detail::last_block == Block::Count ? Block::Unknown : Detail::last_block;
	Detail::last_block = Block::Count;
	const auto kind    = queue_kind < QueueKinds ? queue_kind : QueueKinds - 1;
	Detail::Add(Detail::window.blocks[static_cast<size_t>(block) * QueueKinds + kind]);
}

inline bool StartedInWindow(uint64_t start_ns) {
	return start_ns >= Detail::window_start_ns.load(std::memory_order_relaxed);
}

// Time from a submission blocking until the GPU thread processes it again.
inline void RecordReevaluation(uint64_t blocked_at_ns, uint64_t now_ns) {
	if (!Recording() || blocked_at_ns == 0 || now_ns < blocked_at_ns) return;
	auto& w = Detail::window;
	if (!StartedInWindow(blocked_at_ns)) {
		Detail::Add(w.dropped_reevaluations);
		return;
	}
	const auto elapsed = now_ns - blocked_at_ns;
	Detail::Add(w.reevaluations);
	Detail::Add(w.reevaluation_ns, elapsed);
	Detail::Max(w.reevaluation_max_ns, elapsed);
}

// One timed wait of the guest GPU thread while every queue front was blocked.
inline void RecordIdleWait(uint64_t start_ns, uint64_t end_ns, bool timed_out, uint32_t tid) {
	if (!Recording() || start_ns == 0 || end_ns < start_ns) return;
	auto& w = Detail::window;
	if (!StartedInWindow(start_ns)) {
		Detail::Add(w.dropped_idle_waits);
		return;
	}
	const auto elapsed_ns = end_ns - start_ns;
	Detail::Add(w.idle_waits);
	if (timed_out) Detail::Add(w.idle_timeouts);
	Detail::Add(w.idle_ns, elapsed_ns);
	Detail::Max(w.idle_max_ns, elapsed_ns);
	size_t bucket = 0;
	while (bucket < IdleBucketLimitsUs.size() && elapsed_ns >= IdleBucketLimitsUs[bucket] * 1000) {
		++bucket;
	}
	Detail::Add(w.idle_histogram[bucket]);
	Detail::gpu_thread_tid.store(tid, std::memory_order_relaxed);
}

// Opens a new window: durations in flight and per-thread pending state from the previous one
// stop counting. Aggregates are left alone, so nothing already published is lost.
inline void StartWindow(uint64_t now_ns) {
	Detail::generation.fetch_add(1, std::memory_order_relaxed);
	Detail::window_start_ns.store(now_ns, std::memory_order_relaxed);
}

// Opens a new window and discards everything aggregated so far (recording start).
inline void Reset(uint64_t now_ns = NowNs()) {
	StartWindow(now_ns);
	Diagnostics::Reset();
	auto& w = Detail::window;
	auto take_all = [](auto& counters) {
		for (auto& counter: counters) Detail::Take(counter);
	};
	auto take_tags = [](auto& tags) {
		uint32_t tid   = 0;
		bool     multi = false;
		for (auto& tag: tags) Detail::TakeTag(tag, tid, multi);
	};
	take_all(w.submits);
	take_all(w.submits_without_draw_or_dispatch);
	take_all(w.draws);
	take_all(w.dispatches);
	take_tags(w.submit_threads);
	take_all(w.waits);
	take_all(w.wait_ns);
	take_all(w.wait_max_ns);
	take_tags(w.wait_threads);
	take_all(w.readback_requested_bytes);
	take_all(w.readback_copied_bytes);
	take_all(w.blocks);
	take_all(w.wait_reg_mem_functions);
	take_all(w.idle_histogram);
	take_all(w.extra);
	for (auto* counter: {&w.idle_waits, &w.idle_timeouts, &w.idle_ns, &w.idle_max_ns,
	                     &w.reevaluations, &w.reevaluation_ns, &w.reevaluation_max_ns,
	                     &w.dropped_waits, &w.dropped_idle_waits, &w.dropped_reevaluations,
	                     &w.sample_count}) {
		Detail::Take(*counter);
	}
}

// Closes the window and writes its aggregates as one JSON line. The next window opens before the
// counters are read, so an update racing with the report lands in this report or the next one,
// never nowhere. Callers must not hold emulator locks; output is bounded by the caller.
inline void WriteReport(std::FILE* out, double interval_ms, bool timer_1ms,
                        uint64_t now_ns = NowNs(), bool occlusion_bound = false,
                        bool experiment = false) {
	StartWindow(now_ns);
	auto& w = Detail::window;
	std::fprintf(out,
	             "[SUBMIT_PROFILE] {\"interval_ms\":%.3f,\"timer_1ms\":%s,\"occlusion_bound\":%s,"
	             "\"experiment\":%s,\"gpu_thread_tid\":%u",
	             interval_ms, timer_1ms ? "true" : "false", occlusion_bound ? "true" : "false",
	             experiment ? "true" : "false",
	             Detail::gpu_thread_tid.load(std::memory_order_relaxed));
	std::fprintf(out, ",\"experiment_selected\":\"%s\"", ExperimentName());
	std::fprintf(out, ",\"readback_ahead_config\":{\"write_pages\":%u,\"read_pages\":%u}",
	             Common::ReadbackAhead::WritePages(), Common::ReadbackAhead::ReadPages());
	Diagnostics::Report(out);

	std::fprintf(out, ",\"submits\":[");
	bool comma = false;
	for (size_t row = 0; row < Detail::SubmitRows; ++row) {
		const auto submits = Detail::Take(w.submits[row]);
		const auto bare    = Detail::Take(w.submits_without_draw_or_dispatch[row]);
		const auto draws   = Detail::Take(w.draws[row]);
		const auto disp    = Detail::Take(w.dispatches[row]);
		uint32_t   tid     = 0;
		bool       multi   = false;
		Detail::TakeTag(w.submit_threads[row], tid, multi);
		if ((submits | bare | draws | disp) == 0) continue;
		std::fprintf(out,
		             "%s{\"reason\":\"%s\",\"origin\":\"%s\",\"count\":%llu,\"draws\":%llu,"
		             "\"dispatches\":%llu,\"without_draw_or_dispatch\":%llu,\"tid\":%u,"
		             "\"multi_thread\":%s}",
		             comma ? "," : "", ReasonNames[row / static_cast<size_t>(Origin::Count)],
		             OriginNames[row % static_cast<size_t>(Origin::Count)],
		             static_cast<unsigned long long>(submits), static_cast<unsigned long long>(draws),
		             static_cast<unsigned long long>(disp), static_cast<unsigned long long>(bare), tid,
		             multi ? "true" : "false");
		comma = true;
	}

	std::fprintf(out, "],\"cpu_waits\":[");
	comma = false;
	for (size_t i = 0; i < Detail::SiteRows; ++i) {
		const auto waits = Detail::Take(w.waits[i]);
		const auto ns    = Detail::Take(w.wait_ns[i]);
		const auto max   = Detail::Take(w.wait_max_ns[i]);
		uint32_t   tid   = 0;
		bool       multi = false;
		Detail::TakeTag(w.wait_threads[i], tid, multi);
		const auto requested = Detail::Take(w.readback_requested_bytes[i]);
		const auto copied    = Detail::Take(w.readback_copied_bytes[i]);
		if ((waits | ns | max | requested | copied) == 0) continue;
		std::fprintf(out,
		             "%s{\"site\":\"%s\",\"count\":%llu,\"ms\":%.3f,\"max_ms\":%.3f,\"tid\":%u,"
		             "\"multi_thread\":%s,\"bytes_requested\":%llu,\"bytes_copied\":%llu}",
		             comma ? "," : "", WaitSiteNames[i], static_cast<unsigned long long>(waits),
		             ns / 1e6, max / 1e6, tid, multi ? "true" : "false",
		             static_cast<unsigned long long>(requested), static_cast<unsigned long long>(copied));
		comma = true;
	}

	std::fprintf(out, "],\"blocks\":[");
	comma = false;
	for (size_t b = 0; b < BlockNames.size(); ++b) {
		for (uint32_t q = 0; q < QueueKinds; ++q) {
			const auto count = Detail::Take(w.blocks[b * QueueKinds + q]);
			if (count == 0) continue;
			std::fprintf(out, "%s{\"block\":\"%s\",\"queue\":\"%s\",\"count\":%llu}",
			             comma ? "," : "", BlockNames[b], QueueKindNames[q],
			             static_cast<unsigned long long>(count));
			comma = true;
		}
	}

	std::fprintf(out, "],\"wait_reg_mem_functions\":[");
	for (size_t i = 0; i < w.wait_reg_mem_functions.size(); ++i) {
		std::fprintf(out, "%s%llu", i == 0 ? "" : ",",
		             static_cast<unsigned long long>(Detail::Take(w.wait_reg_mem_functions[i])));
	}

	const auto samples = std::min<uint64_t>(Detail::Take(w.sample_count), MaxWaitRegMemSamples);
	std::fprintf(out, "],\"wait_reg_mem_samples\":[");
	for (uint64_t i = 0; i < samples; ++i) {
		auto& s = w.samples[i];
		std::fprintf(out,
		             "%s{\"queue\":\"%s\",\"address\":\"0x%llx\",\"value\":\"0x%llx\","
		             "\"reference\":\"0x%llx\",\"mask\":\"0x%llx\",\"function\":%u,\"width\":%u}",
		             i == 0 ? "" : ",",
		             QueueKindNames[s.queue_kind.load(std::memory_order_relaxed) % QueueKinds],
		             static_cast<unsigned long long>(s.address.load(std::memory_order_relaxed)),
		             static_cast<unsigned long long>(s.value.load(std::memory_order_relaxed)),
		             static_cast<unsigned long long>(s.reference.load(std::memory_order_relaxed)),
		             static_cast<unsigned long long>(s.mask.load(std::memory_order_relaxed)),
		             s.function.load(std::memory_order_relaxed),
		             s.width.load(std::memory_order_relaxed));
	}

	const auto idle_waits    = Detail::Take(w.idle_waits);
	const auto idle_timeouts = Detail::Take(w.idle_timeouts);
	const auto idle_ns       = Detail::Take(w.idle_ns);
	const auto idle_max_ns   = Detail::Take(w.idle_max_ns);
	std::fprintf(out,
	             "],\"gpu_idle\":{\"waits\":%llu,\"timeouts\":%llu,\"ms\":%.3f,\"max_ms\":%.3f,"
	             "\"histogram_us_upper\":[",
	             static_cast<unsigned long long>(idle_waits),
	             static_cast<unsigned long long>(idle_timeouts), idle_ns / 1e6, idle_max_ns / 1e6);
	for (size_t i = 0; i < IdleBucketLimitsUs.size(); ++i) {
		std::fprintf(out, "%s%llu", i == 0 ? "" : ",",
		             static_cast<unsigned long long>(IdleBucketLimitsUs[i]));
	}
	std::fprintf(out, "],\"histogram\":[");
	for (size_t i = 0; i < IdleBuckets; ++i) {
		std::fprintf(out, "%s%llu", i == 0 ? "" : ",",
		             static_cast<unsigned long long>(Detail::Take(w.idle_histogram[i])));
	}

	const auto reevaluations = Detail::Take(w.reevaluations);
	const auto reeval_ns     = Detail::Take(w.reevaluation_ns);
	const auto reeval_max    = Detail::Take(w.reevaluation_max_ns);
	std::fprintf(out, "]},\"extra\":{");
	uint64_t host_guard_ns = 0;
	for (size_t i = 0; i < ExtraNames.size(); ++i) {
		const auto value = Detail::Take(w.extra[i]);
		if (i == static_cast<size_t>(Extra::HostImportGuardNs)) host_guard_ns = value;
		std::fprintf(out, "%s\"%s\":%llu", i == 0 ? "" : ",", ExtraNames[i],
		             static_cast<unsigned long long>(value));
	}
	std::fprintf(out, ",\"host_import_guard_ms\":%.3f,\"host_import_retained_bytes\":%llu",
	    host_guard_ns / 1e6, static_cast<unsigned long long>(Common::GuestHostImport::retained_bytes.load()));
	std::fprintf(out, ",\"host_import_pending_read_bytes\":%llu,\"host_import_pending_known_bytes\":%llu,\"host_import_pending_unknown_bytes\":%llu",
	    static_cast<unsigned long long>(Common::GuestHostImport::pending_read_bytes.load()),
	    static_cast<unsigned long long>(Common::GuestHostImport::pending_known_bytes.load()),
	    static_cast<unsigned long long>(Common::GuestHostImport::pending_unknown_bytes.load()));
	std::fprintf(out, ",\"hot_owners\":%llu,\"hot_owner_bytes\":%llu",
	    static_cast<unsigned long long>(hot_owner_count.load()), static_cast<unsigned long long>(hot_owner_bytes.load()));
	std::fprintf(out,
	             "},\"reevaluation\":{\"count\":%llu,\"ms\":%.3f,\"max_ms\":%.3f},"
	             "\"dropped_cross_window\":{\"waits\":%llu,\"idle_waits\":%llu,"
	             "\"reevaluations\":%llu}}\n",
	             static_cast<unsigned long long>(reevaluations), reeval_ns / 1e6, reeval_max / 1e6,
	             static_cast<unsigned long long>(Detail::Take(w.dropped_waits)),
	             static_cast<unsigned long long>(Detail::Take(w.dropped_idle_waits)),
	             static_cast<unsigned long long>(Detail::Take(w.dropped_reevaluations)));
	std::fflush(out);
}

} // namespace Common::SubmitProfile
