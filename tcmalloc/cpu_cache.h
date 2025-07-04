// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef TCMALLOC_CPU_CACHE_H_
#define TCMALLOC_CPU_CACHE_H_

#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>

#include <algorithm>
#include <atomic>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>

#include "absl/algorithm/container.h"
#include "absl/base/attributes.h"
#include "absl/base/call_once.h"
#include "absl/base/dynamic_annotations.h"
#include "absl/base/internal/cycleclock.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/optimization.h"
#include "absl/base/thread_annotations.h"
#include "absl/container/fixed_array.h"
#include "absl/functional/function_ref.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/experiment.h"
#include "tcmalloc/experiment_config.h"
#include "tcmalloc/internal/allocation_guard.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/cpu_utils.h"
#include "tcmalloc/internal/environment.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/numa.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/internal/percpu_tcmalloc.h"
#include "tcmalloc/internal/sysinfo.h"
#include "tcmalloc/internal/util.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/thread_cache.h"
#include "tcmalloc/transfer_cache.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
class CpuCachePeer;

namespace cpu_cache_internal {
template <class CpuCache>
struct DrainHandler;

// Determine number of bits we should use for allocating per-cpu cache.
// The amount of per-cpu cache is 2 ^ per-cpu-shift.
// When dynamic slab size is enabled, we start with kInitialPerCpuShift and
// grow as needed up to kMaxPerCpuShift. When dynamic slab size is disabled,
// we always use kMaxPerCpuShift.
#if defined(TCMALLOC_INTERNAL_SMALL_BUT_SLOW)
constexpr inline uint8_t kInitialBasePerCpuShift = 12;
constexpr inline uint8_t kMaxBasePerCpuShift = 12;
#else
constexpr inline uint8_t kInitialBasePerCpuShift = 14;
constexpr inline uint8_t kMaxBasePerCpuShift = 18;
#endif
constexpr inline uint8_t kNumPossiblePerCpuShifts =
    kMaxBasePerCpuShift - kInitialBasePerCpuShift + 1;

constexpr inline uint8_t kResizeSlabCopies = 2;
constexpr inline uint8_t kTotalPossibleSlabs =
    kNumPossiblePerCpuShifts * kResizeSlabCopies;
// StaticForwarder provides access to the SizeMap and transfer caches.
//
// This is a class, rather than namespaced globals, so that it can be mocked for
// testing.
class StaticForwarder {
 public:
  [[nodiscard]] static void* Alloc(size_t size, std::align_val_t alignment)
      ABSL_LOCKS_EXCLUDED(pageheap_lock) {
    TC_ASSERT(tc_globals.IsInited());
    // TODO(b/373944374): Arena is thread-safe, but we take the pageheap_lock to
    // present a consistent view of memory usage.
    PageHeapSpinLockHolder l;
    return tc_globals.arena().Alloc(size, alignment);
  }
  [[nodiscard]] static void* AllocReportedImpending(size_t size,
                                                    std::align_val_t alignment)
      ABSL_LOCKS_EXCLUDED(pageheap_lock) {
    TC_ASSERT(tc_globals.IsInited());
    // TODO(b/373944374): Arena is thread-safe, but we take the pageheap_lock to
    // present a consistent view of memory usage.
    PageHeapSpinLockHolder l;
    // Negate previous update to allocated that accounted for this allocation.
    tc_globals.arena().UpdateAllocatedAndNonresident(
        -static_cast<int64_t>(size), 0);
    return tc_globals.arena().Alloc(size, alignment);
  }

  static void Dealloc(void* ptr, size_t size, std::align_val_t alignment) {
    TC_ASSERT(false);
  }

  static void ArenaUpdateAllocatedAndNonresident(int64_t allocated,
                                                 int64_t nonresident)
      ABSL_LOCKS_EXCLUDED(pageheap_lock) {
    TC_ASSERT(tc_globals.IsInited());
    // TODO(b/373944374): Arena is thread-safe, but we take the pageheap_lock to
    // present a consistent view of memory usage.
    PageHeapSpinLockHolder l;
    if (allocated > 0) {
      tc_globals.page_allocator().ShrinkToUsageLimit(Length(allocated));
    }
    tc_globals.arena().UpdateAllocatedAndNonresident(allocated, nonresident);
  }

  static bool per_cpu_caches_dynamic_slab_enabled() {
    return Parameters::per_cpu_caches_dynamic_slab_enabled();
  }

  static double per_cpu_caches_dynamic_slab_grow_threshold() {
    return Parameters::per_cpu_caches_dynamic_slab_grow_threshold();
  }

  static double per_cpu_caches_dynamic_slab_shrink_threshold() {
    return Parameters::per_cpu_caches_dynamic_slab_shrink_threshold();
  }

  static bool reuse_size_classes() {
    return tc_globals.size_class_configuration() ==
           SizeClassConfiguration::kReuse;
  }

  static bool reuse_size_classes_v2() {
    return tc_globals.size_class_configuration() ==
           SizeClassConfiguration::kReuseV2;
  }

  static size_t class_to_size(int size_class) {
    return tc_globals.sizemap().class_to_size(size_class);
  }

  static absl::Span<const size_t> cold_size_classes() {
    return tc_globals.sizemap().ColdSizeClasses();
  }

  static size_t num_objects_to_move(int size_class) {
    return tc_globals.sizemap().num_objects_to_move(size_class);
  }

  static const NumaTopology<kNumaPartitions, kNumBaseClasses>& numa_topology() {
    return tc_globals.numa_topology();
  }

  static ShardedTransferCacheManager& sharded_transfer_cache() {
    return tc_globals.sharded_transfer_cache();
  }

  static TransferCacheManager& transfer_cache() {
    return tc_globals.transfer_cache();
  }

  static bool UseGenericShardedCache() {
    return tc_globals.sharded_transfer_cache().UseGenericCache();
  }

  static bool UseShardedCacheForLargeClassesOnly() {
    return tc_globals.sharded_transfer_cache().UseCacheForLargeClassesOnly();
  }

  static bool UseWiderSlabs() {
    // We use wider 512KiB slab only when NUMA partitioning is not enabled. NUMA
    // increases shift by 1 by itself, so we can not increase it further.
    return !numa_topology().numa_aware();
  }

  static bool HaveHooks() { return tc_globals.HaveHooks(); }
};

template <typename NumaTopology>
uint8_t NumaShift(const NumaTopology& topology) {
  return topology.numa_aware()
             ? absl::bit_ceil(topology.active_partitions() - 1)
             : 0;
}

// Translates from a shift value to the offset of that shift in arrays of
// possible shift values.
inline uint8_t ShiftOffset(uint8_t shift, uint8_t initial_shift) {
  TC_ASSERT_GE(shift, initial_shift);
  return shift - initial_shift;
}

// Tracks the range of allowed slab shifts.
struct SlabShiftBounds {
  uint8_t initial_shift;
  uint8_t max_shift;
};

struct GetShiftMaxCapacity {
  size_t operator()(size_t size_class) const {
    TC_ASSERT_GE(shift_bounds.max_shift, shift);
    const uint8_t relative_shift = shift_bounds.max_shift - shift;
    if (relative_shift == 0)
      return max_capacities[size_class].load(std::memory_order_relaxed);
    int mc = max_capacities[size_class].load(std::memory_order_relaxed) >>
             relative_shift;
    // We decrement by 3 because of (1) cost of per-size-class header, (2) cost
    // of per-size-class padding pointer, (3) there are a lot of empty size
    // classes that have headers and whose max capacities can't be decremented.
    // TODO(b/272085443): try using size_class_to_header_idx array to allow for
    // not having headers for empty size classes.
    // TODO(b/219565872): try not doing prefetching for large size classes to
    // allow for not having padding pointers for large size classes.
    mc = std::max(mc - 3, 0);
    return mc;
  }

  const std::atomic<uint16_t>* max_capacities;
  uint8_t shift;
  SlabShiftBounds shift_bounds;
};

template <typename Forwarder>
class CpuCache {
 public:
  struct CpuCacheMissStats {
    size_t underflows = 0;
    size_t overflows = 0;

    CpuCacheMissStats& operator+=(const CpuCacheMissStats rhs) {
      underflows += rhs.underflows;
      overflows += rhs.overflows;
      return *this;
    }
  };

  enum class DynamicSlabResize {
    kNoop = 0,
    kShrink,
    kGrow,
  };

  enum class PerClassMissType {
    // Tracks total number of capacity misses.
    kCapacityTotal = 0,
    // Tracks number of misses recorded as of the end of the last per-class
    // resize interval.
    kCapacityResize,
    // Tracks total number of misses due to insufficient max_capacity.
    kMaxCapacityTotal,
    // Tracks number of misses recorded as of the end of the last per-class
    // max capacity resize interval.
    kMaxCapacityResize,
    kNumTypes,
  };

  // We track the number of overflows/underflows for each of these cases.
  enum class MissCount {
    // Tracks total number of misses.
    kTotal = 0,
    // Tracks number of misses recorded as of the end of the last shuffle
    // interval.
    kShuffle,
    // Tracks number of misses recorded as of the end of the last resize
    // interval.
    kReclaim,
    // Tracks number of misses recorded as of the end of the last slab resize
    // interval.
    kSlabResize,
    kNumCounts,
  };

  struct SizeClassCapacityStats {
    size_t min_capacity = 0;
    double avg_capacity = 0;
    size_t max_capacity = 0;
    size_t max_capacity_misses = 0;
    absl::Duration min_last_underflow = absl::InfiniteDuration();
    absl::Duration max_last_underflow;
    absl::Duration min_last_overflow = absl::InfiniteDuration();
    absl::Duration max_last_overflow;
    int min_last_underflow_cpu_id = -1;
    int max_last_underflow_cpu_id = -1;
    int min_last_overflow_cpu_id = -1;
    int max_last_overflow_cpu_id = -1;
  };

  struct DynamicSlabInfo {
    std::atomic<size_t> grow_count[kNumPossiblePerCpuShifts];
    std::atomic<size_t> shrink_count[kNumPossiblePerCpuShifts];
    std::atomic<size_t> madvise_failed_bytes;
  };

  // Sets the lower limit on the capacity that can be stolen from the cpu cache.
  static constexpr double kCacheCapacityThreshold = 0.20;

  constexpr CpuCache() = default;

  // tcmalloc explicitly initializes its global state (to be safe for
  // use in global constructors) so our constructor must be trivial;
  // do all initialization here instead.
  void Activate();

  // For testing
  void Deactivate();

  // Allocate an object of the given size class.
  // Returns nullptr when allocation fails.
  [[nodiscard]] void* Allocate(size_t size_class);
  // Separate allocation fast/slow paths.
  // The fast path succeeds iff the thread has already cached the slab pointer
  // (done by AllocateSlow) and there is an available object in the slab.
  [[nodiscard]] void* AllocateFast(size_t size_class);
  [[nodiscard]] void* AllocateSlow(size_t size_class);
  // A slightly faster version of AllocateSlow that may be called only
  // when it's known that no hooks are installed.
  [[nodiscard]] void* AllocateSlowNoHooks(size_t size_class);

  // Free an object of the given class.
  void Deallocate(void* ptr, size_t size_class);
  // Separate deallocation fast/slow paths.
  // The fast path succeeds iff the thread has already cached the slab pointer
  // (done by DeallocateSlow) and there is free space in the slab.
  bool DeallocateFast(void* ptr, size_t size_class);
  void DeallocateSlow(void* ptr, size_t size_class);
  // A slightly faster version of DeallocateSlow that may be called only
  // when it's known that no hooks are installed.
  void DeallocateSlowNoHooks(void* ptr, size_t size_class);

  // Force all Allocate/DeallocateFast to fail in the current thread
  // if malloc hooks are installed.
  void MaybeForceSlowPath();

  // Give the number of bytes in <cpu>'s cache
  uint64_t UsedBytes(int cpu) const;

  // Give the allocated number of bytes in <cpu>'s cache
  uint64_t Allocated(int cpu) const;

  // Whether <cpu>'s cache has ever been populated with objects
  bool HasPopulated(int cpu) const;
  bool HasPhysicalPopulated(int cpu) const;

  PerCPUMetadataState MetadataMemoryUsage() const;

  // Give the number of bytes used in all cpu caches.
  uint64_t TotalUsedBytes() const;

  // Give the number of objects of a given class in all cpu caches.
  uint64_t TotalObjectsOfClass(size_t size_class) const;

  // Give the number of bytes unallocated to any sizeclass in <cpu>'s cache.
  uint64_t Unallocated(int cpu) const;

  // Gives the total capacity of <cpu>'s cache in bytes.
  //
  // The total capacity of <cpu>'s cache should be equal to the sum of allocated
  // and unallocated bytes for that cache.
  uint64_t Capacity(int cpu) const;

  // Give the per-cpu limit of cache size.
  uint64_t CacheLimit() const;
  void SetCacheLimit(uint64_t v);

  // Shuffles per-cpu caches using the number of underflows and overflows that
  // occurred in the prior interval. It selects the top per-cpu caches
  // with highest misses as candidates, iterates through the other per-cpu
  // caches to steal capacity from them and adds the stolen bytes to the
  // available capacity of the per-cpu caches. May be called from any processor.
  //
  // TODO(vgogte): There are quite a few knobs that we can play around with in
  // ShuffleCpuCaches.
  void ShuffleCpuCaches();

  // Tries to reclaim inactive per-CPU caches. It iterates through the set of
  // populated cpu caches and reclaims the caches that:
  // (1) had same number of used bytes since the last interval,
  // (2) had no change in the number of misses since the last interval.
  void TryReclaimingCaches();

  // Resize size classes for up to kNumCpuCachesToResize cpu caches per
  // interval.
  static constexpr int kNumCpuCachesToResize = 10;
  // Resizes size classes within up to kNumCpuCachesToResize per-cpu caches per
  // iteration in a round-robin fashion. Per cpu cache, it iterates through the
  // size classes and attempts to grow up to kMaxSizeClassesToResize number of
  // classes by stealing capacity from rest of them. Per iteration, it resizes
  // size classes for up to kNumCpuCachesToResize number of per-cpu caches.
  void ResizeSizeClasses();

  // Gets the max capacity for the size class using the current per-cpu shift.
  uint16_t GetMaxCapacity(int size_class, uint8_t shift) const;

  // Gets the current capacity for the <size_class> in a <cpu> cache.
  size_t GetCapacityOfSizeClass(int cpu, int size_class) const;

  // Computes maximum capacities that we want to update the size classes to. It
  // fetches number of capacity misses obvserved for the size classes, and
  // computes increases to the maximum capacities for the size classes with the
  // highest misses. It computes maximum capacities for kNumBaseClasses number
  // of size classes, starting with <start_size_class>. It records the resized
  // size classes and capacities in <max_capacity> starting from index
  // <valid_entries>.
  // Returns total number of valid size classes recorded in <max_capacity>
  // array.
  int GetUpdatedMaxCapacities(int start_size_class,
                              PerSizeClassMaxCapacity* max_capacity,
                              int valid_entries);

  // Resizes maximum capacities for the size classes. First, it computes
  // candidates to resize using GetUpdatedMaxCapacities(...), and then updates
  // maximum capacities for size classes for all per-cpu caches. Resizing is a
  // global operation. It stops all per-cpu caches, drains them, updates maximum
  // capacities and begin, current and end indices for the slabs and then
  // restarts the per-cpu caches. Because it's a global operation that involves
  // stopping all per-cpu caches, this mechanism should be used sparingly.
  void ResizeSizeClassMaxCapacities();

  // Empty out the cache on <cpu>; move all objects to the central
  // cache.  (If other threads run concurrently on that cpu, we can't
  // guarantee it will be fully empty on return, but if the cpu is
  // unused, this will eliminate stranded memory.)  Returns the number
  // of bytes we sent back.  This function is thread safe.
  uint64_t Reclaim(int cpu);

  // Reports number of times the size classes were resized for <cpu>.
  uint64_t GetNumResizes(int cpu) const;

  // Reports total number of times size classes were resized.
  uint64_t GetNumResizes() const;

  // Reports number of times the <cpu> has been reclaimed.
  uint64_t GetNumReclaims(int cpu) const;

  // Reports total number of times any CPU has been reclaimed.
  uint64_t GetNumReclaims() const;

  // When dynamic slab size is enabled, checks if there is a need to resize
  // the slab based on miss-counts and resizes if so.
  void ResizeSlabIfNeeded();

  // Reports total cache underflows and overflows for <cpu>.
  CpuCacheMissStats GetTotalCacheMissStats(int cpu) const;

  // Reports total cache underflows and overflows for all CPUs.
  CpuCacheMissStats GetTotalCacheMissStats() const;

  // Reports the cache underflows and overflows for <cpu> that were recorded
  // during the previous interval for <miss_count>.
  CpuCacheMissStats GetIntervalCacheMissStats(int cpu,
                                              MissCount miss_count) const;

  // Records current underflows and overflows in the <miss_count> underflow and
  // overflow stats.
  void UpdateIntervalCacheMissStats(int cpu, MissCount miss_count);

  // Reports the cache underflows and overflows for <cpu> that were recorded
  // during the previous interval for <miss_count>. Records current underflows
  // and overflows in the <miss_count> underflow and overflow stats.
  CpuCacheMissStats GetAndUpdateIntervalCacheMissStats(int cpu,
                                                       MissCount miss_count);

  // Scans through populated per-CPU caches, and reports minimum, average and
  // maximum capacity for size class <size_class>.
  SizeClassCapacityStats GetSizeClassCapacityStats(size_t size_class) const;

  // Reports the number of misses encountered by a <size_class> that were
  // recorded during the previous interval between <total_type> and
  // <interval_type> kinds of misses.
  size_t GetIntervalSizeClassMisses(int cpu, size_t size_class,
                                    PerClassMissType total_type,
                                    PerClassMissType interval_type);

  // Reports if we should use a wider 512KiB slab.
  bool UseWiderSlabs() const;

  // Reports allowed slab shift initial and maximum bounds.
  SlabShiftBounds GetPerCpuSlabShiftBounds() const;

  size_t GetDynamicSlabFailedBytes() const;

  // Report statistics
  void Print(Printer& out) const;
  void PrintInPbtxt(PbtxtRegion& region) const;

  const Forwarder& forwarder() const { return forwarder_; }

  Forwarder& forwarder() { return forwarder_; }

 private:
  friend struct DrainHandler<CpuCache>;
  friend class ::tcmalloc::tcmalloc_internal::CpuCachePeer;

  using Freelist = subtle::percpu::TcmallocSlab<kNumClasses>;

  struct PerClassMissCounts {
    std::atomic<size_t>
        misses[static_cast<size_t>(PerClassMissType::kNumTypes)];

    std::atomic<size_t>& operator[](PerClassMissType type) {
      return misses[static_cast<size_t>(type)];
    }
  };

  // Per-size-class freelist resizing info.
  class PerClassResizeInfo {
   public:
    void Init();
    // Updates info on overflow/underflow.
    // <overflow> says if it's overflow or underflow.
    // <grow> is caller approximation of whether we want to grow capacity.
    // <successive> will contain number of successive overflows/underflows.
    // Returns if capacity needs to be grown aggressively (i.e. by batch size).
    bool Update(bool overflow, bool grow, uint32_t* successive);
    uint32_t Tick();

    // Records a miss for a provided <type>. A miss occurs when size class
    // attempts to grow it's capacity on underflow/overflow, but we are already
    // at the maximum configured per-cpu cache capacity limit.
    void RecordMiss(PerClassMissType type);

    // Reports total number of misses recorded for this size class.
    size_t GetTotalMisses(PerClassMissType type);

    size_t GetAndUpdateIntervalMisses(PerClassMissType total_type,
                                      PerClassMissType interval_type);

    // Reports the number of misses encountered by this size class that
    // were recorded during the previous interval between misses <total_type>
    // and <interval_type>.
    size_t GetIntervalMisses(PerClassMissType total_type,
                             PerClassMissType interval_type);

    // Copies total misses of type <total_type> encountered by the size class to
    // the type <interval_type>.
    void UpdateIntervalMisses(PerClassMissType total_type,
                              PerClassMissType interval_type);

   private:
    std::atomic<int32_t> state_;
    // state_ layout:
    struct State {
      // last overflow/underflow?
      uint32_t overflow : 1;
      // number of times Steal checked this class since the last grow
      uint32_t quiescent_ticks : 15;
      // number of successive overflows/underflows
      uint32_t successive : 16;
    };
    PerClassMissCounts misses_;
    static_assert(sizeof(State) == sizeof(std::atomic<int32_t>),
                  "size mismatch");
  };

  // Helper type so we don't need to sprinkle `static_cast`s everywhere.
  struct MissCounts {
    std::atomic<size_t> misses[static_cast<size_t>(MissCount::kNumCounts)];

    std::atomic<size_t>& operator[](MissCount miss_count) {
      return misses[static_cast<size_t>(miss_count)];
    }
  };

  struct ABSL_CACHELINE_ALIGNED ResizeInfo {
    // cache space on this CPU we're not using.  Modify atomically;
    // we don't want to lose space.
    std::atomic<size_t> available;
    // Size class to steal from for the clock-wise algorithm.
    size_t next_steal = 1;
    // Track whether we have ever populated this CPU.
    std::atomic<bool> populated;
    std::atomic<bool> populated_physical_cpu{false};
    // For cross-cpu operations. We can't allocate while holding one of these so
    // please use AllocationGuardSpinLockHolder to hold it.
    absl::base_internal::SpinLock lock ABSL_ACQUIRED_BEFORE(pageheap_lock){
        absl::kConstInit, absl::base_internal::SCHEDULE_KERNEL_ONLY};
    PerClassResizeInfo per_class[kNumClasses];
    std::atomic<size_t> num_size_class_resizes;
    // Tracks number of underflows on allocate.
    MissCounts underflows;
    // Tracks number of overflows on deallocate.
    MissCounts overflows;
    std::atomic<int64_t> last_miss_cycles[2][kNumClasses];
    // total cache space available on this CPU. This tracks the total
    // allocated and unallocated bytes on this CPU cache.
    std::atomic<size_t> capacity;
    // Used bytes in the cache as of the end of the last resize interval.
    std::atomic<uint64_t> reclaim_used_bytes;
    // Tracks number of times this CPU has been reclaimed.
    std::atomic<size_t> num_reclaims;
    // Tracks last time this CPU was reclaimed.  If last underflow/overflow data
    // appears before this point in time, we ignore the CPU.
    std::atomic<int64_t> last_reclaim;
  };

  // Determines how we distribute memory in the per-cpu cache to the various
  // class sizes.
  size_t MaxCapacity(size_t size_class) const;

  // Updates maximum capacity for the <size_class> to <cap>.
  void UpdateMaxCapacity(int size_class, uint16_t cap);

  GetShiftMaxCapacity GetMaxCapacityFunctor(uint8_t shift) const;

  // Fetches objects from backing transfer cache.
  [[nodiscard]] int FetchFromBackingCache(size_t size_class,
                                          absl::Span<void*> batch);

  // Releases free batch of objects to the backing transfer cache.
  void ReleaseToBackingCache(size_t size_class, absl::Span<void*> batch);

  [[nodiscard]] void* Refill(int cpu, size_t size_class);
  std::pair<int, bool> CacheCpuSlab();
  void Populate(int cpu);

  // Returns true if we bypass cpu cache for a <size_class>. We may bypass
  // per-cpu cache when we enable certain configurations of sharded transfer
  // cache.
  bool BypassCpuCache(size_t size_class) const;

  // Returns true if we use sharded transfer cache as a backing cache for
  // per-cpu caches. If a sharded transfer cache is used, we fetch/release
  // from/to a sharded transfer cache. Else, we use a legacy transfer cache.
  bool UseBackingShardedTransferCache(size_t size_class) const;

  // Called on <size_class> freelist on <cpu> to record overflow/underflow
  // Returns number of objects to return/request from transfer cache.
  size_t UpdateCapacity(int cpu, size_t size_class, bool overflow);

  // Tries to grow freelist <size_class> on the current <cpu> by up to
  // <desired_increase> objects if there is available capacity.
  void Grow(int cpu, size_t size_class, size_t desired_increase);

  // Depending on the number of misses that cpu caches encountered in the
  // previous resize interval, returns if slabs should be grown, shrunk or
  // remain the same.
  DynamicSlabResize ShouldResizeSlab();

  // Determine if the <size_class> is a good candidate to be shrunk. We use
  // clock-like algorithm to prioritize size classes for shrinking.
  bool IsGoodCandidateForShrinking(int cpu, size_t size_class);

  struct SizeClassMissStat {
    size_t size_class;
    size_t misses;
  };
  struct CpuMissStat {
    int cpu;
    size_t misses;
  };

  // Tries to steal <bytes> for <size_class> on <cpu> from other size classes on
  // that CPU. Returns acquired bytes.
  size_t StealCapacityForSizeClassWithinCpu(
      int cpu, absl::Span<SizeClassMissStat> dest_size_classes, size_t bytes);

  // Records a cache underflow or overflow on <cpu>, increments underflow or
  // overflow by 1.
  // <is_alloc> determines whether the associated count corresponds to an
  // underflow or overflow.
  void RecordCacheMissStat(int cpu, bool is_alloc);

  // Tries to steal <bytes> for the destination <cpu>. It iterates through the
  // the set of populated cpu caches and steals the bytes from them. A cpu is
  // considered a good candidate to steal from if:
  // (1) the cache is populated
  // (2) the numbers of underflows and overflows are both less than 0.8x those
  // of the destination per-cpu cache
  // (3) source cpu is not the same as the destination cpu
  // (4) capacity of the source cpu/size_class is non-zero
  //
  // For a given source cpu, we iterate through the size classes to steal from
  // them. Currently, we use a clock-like algorithm to identify the size_class
  // to steal from.
  void StealFromOtherCache(int cpu, int max_populated_cpu,
                           absl::Span<CpuMissStat> skip_cpus, size_t bytes);

  // Try to steal one object from cpu/size_class. Return bytes stolen.
  size_t ShrinkOtherCache(int cpu, size_t size_class);

  // Resizes capacities of up to kMaxSizeClassesToResize size classes for a
  // single <cpu>.
  void ResizeCpuSizeClasses(int cpu);

  // <shift_offset> is the offset of the shift in slabs_by_shift_. Note that we
  // can't calculate this from `shift` directly due to numa shift.
  // Returns the allocated slabs and the number of reused bytes.
  [[nodiscard]] std::pair<void*, size_t> AllocOrReuseSlabs(
      absl::FunctionRef<void*(size_t, std::align_val_t)> alloc,
      subtle::percpu::Shift shift, int num_cpus, uint8_t shift_offset,
      uint8_t resize_offset);

  // madvise-away slab memory, pointed to by <slab_addr> of size <slab_size>.
  void MadviseAwaySlabs(void* slab_addr, size_t slab_size);

  Freelist freelist_;

  // Tracking data for each CPU's cache resizing efforts.
  ResizeInfo* resize_ = nullptr;

  // Tracks initial and maximum slab shift bounds.
  SlabShiftBounds shift_bounds_{};

  // The maximum capacity of each size class within the slab.
  std::atomic<uint16_t> max_capacity_[kNumClasses] = {0};

  // Provides a hint to StealFromOtherCache() so that we can steal from the
  // caches in a round-robin fashion.
  int next_cpu_cache_steal_ = 0;

  // Provides a hint to ResizeSizeClasses() that records the last CPU for which
  // we resized size classes. We use this to resize size classes for CPUs in a
  // round-robin fashion.
  std::atomic<int> last_cpu_size_class_resize_ = 0;

  // Records the slab copy currently in use. We maintain kResizeSlabCopies
  // sets of kNumPossiblePerCpuShifts slabs. While resizing maximum size class
  // capacity, we choose a new slab from one of the copies. resize_slab_offset_
  // is an index into the copy currently in use.
  std::atomic<int> resize_slab_offset_ = 0;

  // Per-core cache limit in bytes.
  std::atomic<uint64_t> max_per_cpu_cache_size_{kMaxCpuCacheSize};

  ABSL_ATTRIBUTE_NO_UNIQUE_ADDRESS Forwarder forwarder_;

  DynamicSlabInfo dynamic_slab_info_{};

  // Pointers to allocations for slabs of each shift value for use in
  // ResizeSlabs. This memory is allocated on the arena, and it is nonresident
  // while not in use.
  void* slabs_by_shift_[kTotalPossibleSlabs] = {nullptr};
};

template <class Forwarder>
void* CpuCache<Forwarder>::Allocate(size_t size_class) {
  void* ret = AllocateFast(size_class);
  if (ABSL_PREDICT_TRUE(ret != nullptr)) {
    return ret;
  }
  TCMALLOC_MUSTTAIL return AllocateSlow(size_class);
}

template <class Forwarder>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* CpuCache<Forwarder>::AllocateFast(
    size_t size_class) {
  TC_ASSERT_GT(size_class, 0);
  return freelist_.Pop(size_class);
}

template <class Forwarder>
void CpuCache<Forwarder>::Deallocate(void* ptr, size_t size_class) {
  if (ABSL_PREDICT_FALSE(!DeallocateFast(ptr, size_class))) {
    TCMALLOC_MUSTTAIL return DeallocateSlow(ptr, size_class);
  }
}

template <class Forwarder>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE bool CpuCache<Forwarder>::DeallocateFast(
    void* ptr, size_t size_class) {
  TC_ASSERT_GT(size_class, 0);
  return freelist_.Push(size_class, ptr);
}

template <class Forwarder>
void CpuCache<Forwarder>::MaybeForceSlowPath() {
  if (ABSL_PREDICT_FALSE(forwarder_.HaveHooks())) {
    freelist_.UncacheCpuSlab();
  }
}

static CpuSet FillActiveCpuMask() {
  CpuSet allowed_cpus;
  if (!allowed_cpus.GetAffinity(0)) {
    allowed_cpus.Zero();
  }

#ifdef PERCPU_USE_RSEQ
  const bool real_cpus = !subtle::percpu::UsingVirtualCpus();
#else
  const bool real_cpus = true;
#endif

  if (real_cpus) {
    return allowed_cpus;
  }

  const int virtual_cpu_count = allowed_cpus.Count();
  allowed_cpus.Zero();
  for (int cpu = 0; cpu < virtual_cpu_count; ++cpu) {
    allowed_cpus.Set(cpu);
  }
  return allowed_cpus;
}

template <class Forwarder>
inline size_t CpuCache<Forwarder>::MaxCapacity(size_t size_class) const {
  // The number of size classes that are commonly used and thus should be
  // allocated more slots in the per-cpu cache.
  static constexpr size_t kNumSmall = 10;

  // When we use wider slabs, we also want to double the maximum capacities for
  // size classes to use that slab.
  const size_t kWiderSlabMultiplier = UseWiderSlabs() ? 2 : 1;

  // The memory used for each per-CPU slab is the sum of:
  //   sizeof(std::atomic<int64_t>) * kNumClasses
  //   sizeof(void*) * (kSmallObjectDepth + 1) * kNumSmall
  //   sizeof(void*) * (kLargeObjectDepth + 1) * kNumLarge
  //
  // Class size 0 has MaxCapacity() == 0, which is the reason for using
  // kNumClasses - 1 above instead of kNumClasses.
  //
  // Each Size class region in the slab is preceded by one padding pointer that
  // points to itself, because prefetch instructions of invalid pointers are
  // slow. That is accounted for by the +1 for object depths.
#if defined(TCMALLOC_INTERNAL_SMALL_BUT_SLOW)
  // With SMALL_BUT_SLOW we have 4KiB of per-cpu slab and 46 class sizes we
  // allocate:
  //   == 8 * 46 + 8 * ((16 + 1) * 10 + (6 + 1) * 35) = 4038 bytes of 4096
  static const uint16_t kSmallObjectDepth = 16;
  static const uint16_t kLargeObjectDepth = 6;
#else
  // We allocate 256KiB per-cpu for pointers to cached per-cpu memory.
  // Max(kNumClasses) is 89, so the maximum footprint per CPU for a 256KiB
  // slab is:
  //   89 * 8 + 8 * ((2048 + 1) * 10 + (152 + 1) * 78) = 254 KiB
  // For 512KiB slab, with a multiplier of 2, maximum footprint is:
  //   89 * 8 + 8 * ((4096 + 1) * 10 + (304 + 1) * 78) = 506 KiB
  const uint16_t kSmallObjectDepth = 2048 * kWiderSlabMultiplier;
  const uint16_t kLargeObjectDepth = 152 * kWiderSlabMultiplier;
#endif
  if (size_class == 0 || size_class >= kNumClasses) {
    return 0;
  }

  if (BypassCpuCache(size_class)) {
    return 0;
  }

  if (forwarder_.class_to_size(size_class) == 0) {
    return 0;
  }

  if (!IsExpandedSizeClass(size_class) &&
      (size_class % kNumBaseClasses) <= kNumSmall) {
    // Small object sizes are very heavily used and need very deep caches for
    // good performance (well over 90% of malloc calls are for size_class
    // <= 10.)
    return kSmallObjectDepth;
  }

  if (ColdFeatureActive()) {
    // We reduce the number of cached objects for some sizes to fit into the
    // slab.
    //
    // We use fewer number of size classes when using reuse size classes. So,
    // we may use larger capacity for some sizes.
    const uint16_t kLargeUninterestingObjectDepth =
        forwarder_.reuse_size_classes()      ? 246 * kWiderSlabMultiplier
        : forwarder_.reuse_size_classes_v2() ? 326 * kWiderSlabMultiplier
                                             : 123 * kWiderSlabMultiplier;
    const uint16_t kLargeInterestingObjectDepth =
        forwarder_.reuse_size_classes() ? 46 * kWiderSlabMultiplier
                                        : 28 * kWiderSlabMultiplier;

    absl::Span<const size_t> cold = forwarder_.cold_size_classes();
    if (absl::c_binary_search(cold, size_class)) {
      return kLargeInterestingObjectDepth;
    } else if (!IsExpandedSizeClass(size_class)) {
      return kLargeUninterestingObjectDepth;
    } else {
      return 0;
    }
  }

  if (IsExpandedSizeClass(size_class)) {
    return 0;
  }

  return kLargeObjectDepth;
}

// Returns estimated bytes required and the bytes available.
inline std::pair<size_t, size_t> EstimateSlabBytes(
    GetShiftMaxCapacity get_shift_capacity) {
  size_t bytes_required = sizeof(std::atomic<int64_t>) * kNumClasses;

  for (int size_class = 0; size_class < kNumClasses; ++size_class) {
    // Each non-empty size class region in the slab is preceded by one padding
    // pointer that points to itself. (We do this because prefetches of invalid
    // pointers are slow.)
    size_t num_pointers = get_shift_capacity(size_class);
    if (num_pointers > 0) ++num_pointers;
    bytes_required += sizeof(void*) * num_pointers;
  }

  const size_t bytes_available = 1 << get_shift_capacity.shift;
  return {bytes_required, bytes_available};
}

template <class Forwarder>
inline uint16_t CpuCache<Forwarder>::GetMaxCapacity(int size_class,
                                                    uint8_t shift) const {
  return GetMaxCapacityFunctor(shift)(size_class);
}

template <class Forwarder>
inline size_t CpuCache<Forwarder>::GetCapacityOfSizeClass(
    int cpu, int size_class) const {
  return freelist_.Capacity(cpu, size_class);
}

template <class Forwarder>
inline GetShiftMaxCapacity CpuCache<Forwarder>::GetMaxCapacityFunctor(
    uint8_t shift) const {
  return {max_capacity_, shift, shift_bounds_};
}

template <class Forwarder>
inline void CpuCache<Forwarder>::UpdateMaxCapacity(int size_class,
                                                   uint16_t cap) {
  max_capacity_[size_class].store(cap, std::memory_order_relaxed);
}

template <class Forwarder>
inline bool CpuCache<Forwarder>::UseWiderSlabs() const {
  return forwarder_.UseWiderSlabs();
}

template <class Forwarder>
inline SlabShiftBounds CpuCache<Forwarder>::GetPerCpuSlabShiftBounds() const {
  return shift_bounds_;
}

template <class Forwarder>
inline size_t CpuCache<Forwarder>::GetDynamicSlabFailedBytes() const {
  return dynamic_slab_info_.madvise_failed_bytes.load(
      std::memory_order_relaxed);
}

template <class Forwarder>
inline void CpuCache<Forwarder>::Activate() {
  int num_cpus = NumCPUs();

  shift_bounds_.initial_shift = kInitialBasePerCpuShift;
  shift_bounds_.max_shift = kMaxBasePerCpuShift;
  uint8_t per_cpu_shift = forwarder_.per_cpu_caches_dynamic_slab_enabled()
                              ? kInitialBasePerCpuShift
                              : kMaxBasePerCpuShift;

  const auto& topology = forwarder_.numa_topology();
  const uint8_t numa_shift = NumaShift(topology);
  const uint8_t wider_slab_shift = UseWiderSlabs() ? 1 : 0;

  shift_bounds_.initial_shift += numa_shift + wider_slab_shift;
  shift_bounds_.max_shift += numa_shift + wider_slab_shift;
  per_cpu_shift += numa_shift + wider_slab_shift;

  TC_CHECK_LE(shift_bounds_.initial_shift, shift_bounds_.max_shift);
  TC_CHECK_GE(per_cpu_shift, shift_bounds_.initial_shift);
  TC_CHECK_LE(per_cpu_shift, shift_bounds_.max_shift);
  TC_CHECK_EQ(shift_bounds_.max_shift - shift_bounds_.initial_shift + 1,
              kNumPossiblePerCpuShifts);

  // Deal with size classes that correspond only to NUMA partitions that are in
  // use. If NUMA awareness is disabled then we may have a smaller shift than
  // would suffice for all of the unused size classes.
  for (int size_class = 0;
       size_class < topology.active_partitions() * kNumBaseClasses;
       ++size_class) {
    max_capacity_[size_class].store(MaxCapacity(size_class),
                                    std::memory_order_relaxed);
  }

  // Deal with expanded size classes.
  for (int size_class = kExpandedClassesStart; size_class < kNumClasses;
       ++size_class) {
    max_capacity_[size_class].store(MaxCapacity(size_class),
                                    std::memory_order_relaxed);
  }

  // Verify that all the possible shifts will have valid max capacities.
  for (uint8_t shift = shift_bounds_.initial_shift;
       shift <= shift_bounds_.max_shift; ++shift) {
    const auto [bytes_required, bytes_available] =
        EstimateSlabBytes({max_capacity_, shift, shift_bounds_});
    // We may make certain size classes no-ops by selecting "0" at runtime, so
    // using a compile-time calculation overestimates worst-case memory usage.
    if (ABSL_PREDICT_FALSE(bytes_required > bytes_available)) {
      TC_BUG("per-CPU memory exceeded, have %v, need %v", bytes_available,
             bytes_required);
    }
  }

  resize_ = reinterpret_cast<ResizeInfo*>(forwarder_.Alloc(
      sizeof(ResizeInfo) * num_cpus, std::align_val_t{alignof(ResizeInfo)}));

  auto max_cache_size = CacheLimit();

  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    new (&resize_[cpu]) ResizeInfo();

    for (int size_class = 1; size_class < kNumClasses; ++size_class) {
      resize_[cpu].per_class[size_class].Init();
    }
    resize_[cpu].available.store(max_cache_size, std::memory_order_relaxed);
    resize_[cpu].capacity.store(max_cache_size, std::memory_order_relaxed);
  }

  void* slabs =
      AllocOrReuseSlabs(&forwarder_.Alloc,
                        subtle::percpu::ToShiftType(per_cpu_shift), num_cpus,
                        ShiftOffset(per_cpu_shift, shift_bounds_.initial_shift),
                        /*resize_offset=*/0)
          .first;
  freelist_.Init(
      &forwarder_.Alloc, slabs,
      GetShiftMaxCapacity{max_capacity_, per_cpu_shift, shift_bounds_},
      subtle::percpu::ToShiftType(per_cpu_shift));
}

template <class Forwarder>
inline void CpuCache<Forwarder>::Deactivate() {
  int num_cpus = NumCPUs();
  for (int i = 0; i < num_cpus; i++) {
    Reclaim(i);
  }

  freelist_.Destroy(&forwarder_.Dealloc);
  static_assert(std::is_trivially_destructible<decltype(*resize_)>::value,
                "ResizeInfo is expected to be trivially destructible");
  forwarder_.Dealloc(resize_, sizeof(*resize_) * num_cpus,
                     std::align_val_t{alignof(decltype(*resize_))});
}

template <class Forwarder>
inline int CpuCache<Forwarder>::FetchFromBackingCache(size_t size_class,
                                                      absl::Span<void*> batch) {
  if (UseBackingShardedTransferCache(size_class)) {
    return forwarder_.sharded_transfer_cache().RemoveRange(size_class, batch);
  }
  return forwarder_.transfer_cache().RemoveRange(size_class, batch);
}

template <class Forwarder>
inline void CpuCache<Forwarder>::ReleaseToBackingCache(
    size_t size_class, absl::Span<void*> batch) {
  if (UseBackingShardedTransferCache(size_class)) {
    forwarder_.sharded_transfer_cache().InsertRange(size_class, batch);
    return;
  }

  forwarder_.transfer_cache().InsertRange(size_class, batch);
}

template <class Forwarder>
void* CpuCache<Forwarder>::AllocateSlow(size_t size_class) {
  void* ret = AllocateSlowNoHooks(size_class);
  MaybeForceSlowPath();
  return ret;
}

template <class Forwarder>
void* CpuCache<Forwarder>::AllocateSlowNoHooks(size_t size_class) {
  if (BypassCpuCache(size_class)) {
    return forwarder_.sharded_transfer_cache().Pop(size_class);
  }
  auto [cpu, cached] = CacheCpuSlab();
  if (ABSL_PREDICT_FALSE(cached)) {
    if (ABSL_PREDICT_FALSE(cpu < 0)) {
      // The cpu is stopped.
      void* ptr = nullptr;
      int r = FetchFromBackingCache(size_class, absl::MakeSpan(&ptr, 1));
#ifndef NDEBUG
      TC_ASSERT(r == 1 || ptr == nullptr);
#else
      (void)r;
#endif
      return ptr;
    }
    if (void* ret = AllocateFast(size_class)) {
      return ret;
    }
  }
  RecordCacheMissStat(cpu, true);
  return Refill(cpu, size_class);
}

// Fetch more items from the central cache, refill our local cache,
// and try to grow it if necessary.
//
// This is complicated by the fact that we can only tweak the cache on
// our current CPU and we might get migrated whenever (in fact, we
// might already have been migrated since failing to get memory...)
//
// So make sure only to make changes to one CPU's cache; at all times,
// it must be safe to find ourselves migrated (at which point we atomically
// return memory to the correct CPU.)
template <class Forwarder>
inline void* CpuCache<Forwarder>::Refill(int cpu, size_t size_class) {
  const size_t target = UpdateCapacity(cpu, size_class, false);

  // Refill target objects in batch_length batches.
  size_t total = 0;
  size_t got;
  size_t i;
  void* result = nullptr;
  void* batch[kMaxObjectsToMove];

  do {
    const size_t want = std::min(kMaxObjectsToMove, target - total);
    got = FetchFromBackingCache(size_class, absl::MakeSpan(batch, want));
    if (got == 0) {
      break;
    }
    total += got;
    i = got;
    if (result == nullptr) {
      i--;
      result = batch[i];
    }
    if (i) {
      i -= freelist_.PushBatch(size_class, batch, i);
      if (i != 0) {
        ReleaseToBackingCache(size_class, {batch, i});
      }
    }
  } while (got == kMaxObjectsToMove && i == 0 && total < target);
  return result;
}

template <class Forwarder>
inline bool CpuCache<Forwarder>::BypassCpuCache(size_t size_class) const {
  // We bypass per-cpu cache when sharded transfer cache is enabled for large
  // size classes (i.e. when we use the traditional configuration of the sharded
  // transfer cache).
  return forwarder_.sharded_transfer_cache().should_use(size_class) &&
         forwarder_.UseShardedCacheForLargeClassesOnly();
}

template <class Forwarder>
inline bool CpuCache<Forwarder>::UseBackingShardedTransferCache(
    size_t size_class) const {
  // Make sure that the thread is registered with rseq.
  TC_ASSERT(subtle::percpu::IsFastNoInit());
  // We enable sharded cache as a backing cache for all size classes when
  // generic configuration is enabled.
  return forwarder_.sharded_transfer_cache().should_use(size_class) &&
         forwarder_.UseGenericShardedCache();
}

// Calculate number of objects to return/request from transfer cache.
inline size_t TargetOverflowRefillCount(size_t capacity, size_t batch_length,
                                        size_t successive) {
  // If the freelist is large and we are hitting a series of overflows or
  // underflows, return/request several batches at once. On the first overflow
  // we return 1 batch, on the second -- 2, on the third -- 4 and so on up to
  // half of the batches we have. We do this to save on the cost of hitting
  // malloc/free slow path, reduce instruction cache pollution, avoid cache
  // misses when accessing transfer/central caches, etc.
  const size_t max = (1 << std::min<uint32_t>(successive, 10)) * batch_length;
  // Aim at returning/refilling roughly half of objects.
  // Round up odd sizes, e.g. if the capacity is 3, we want to refill 2 objects.
  // Also always add 1 to the result to account for the additional object
  // we need to return to the caller on refill, or return on overflow.
  size_t target = std::min((capacity + 1) / 2 + 1, max);
  if (capacity == 1 && successive < 3) {
    // If the capacity is 1, it's generally impossible to avoid bad behavior.
    // Consider refills (but the same stands for overflows): if we fetch an
    // additional object and put it into the cache, and the caller is doing
    // malloc/free in a loop, then we both fetched an unnecessary object and
    // we will immediately hit an overflow on the free. On the other hand
    // if we don't fetch an additional object, and the caller is allocating
    // in a loop, then we also hit underflow again on the next malloc.
    // Currently we fetch/return an additional objects only if we are hitting
    // successive underflows/overflows.
    // But note that this behavior is also easy to compromise: if the caller is
    // allocating 3 objects and then freeing 3 objects in a loop, then we always
    // do the wrong thing.
    target = 1;
  }
  TC_ASSERT_LE(target, capacity + 1);
  TC_ASSERT_NE(target, 0);
  return target;
}

template <class Forwarder>
inline size_t CpuCache<Forwarder>::UpdateCapacity(int cpu, size_t size_class,
                                                  bool overflow) {
  // Freelist size balancing strategy:
  //  - We grow a size class only on overflow/underflow.
  //  - We shrink size classes in Steal as it scans all size classes.
  //  - If overflows/underflows happen on a size class, we want to grow its
  //    capacity to at least 2 * batch_length. It enables usage of the
  //    transfer cache and leaves the list half-full after we insert/remove
  //    a batch from the transfer cache.
  //  - We increase capacity beyond 2 * batch_length only when an overflow is
  //    followed by an underflow. That's the only case when we could benefit
  //    from larger capacity -- the overflow and the underflow would collapse.
  //
  // Note: we can't understand when we have a perfectly-sized list, because for
  // a perfectly-sized list we don't hit any slow paths which looks the same as
  // inactive list. Eventually we will shrink a perfectly-sized list a bit and
  // then it will grow back. This won't happen very frequently for the most
  // important small sizes, because we will need several ticks before we shrink
  // it again. Also we will shrink it by 1, but grow by a batch. So we should
  // have lots of time until we need to grow it again.

  // We assert that the return value, target, is non-zero, so starting from an
  // initial capacity of zero means we may be populating this core for the
  // first time.
  size_t batch_length = forwarder_.num_objects_to_move(size_class);
  const size_t max_capacity = GetMaxCapacity(size_class, freelist_.GetShift());
  size_t capacity = freelist_.Capacity(cpu, size_class);
  const bool grow_by_one = capacity < 2 * batch_length;
  uint32_t successive = 0;
  ResizeInfo& resize = resize_[cpu];
  const int64_t now = absl::base_internal::CycleClock::Now();
  // TODO(ckennelly): Use a strongly typed enum.
  resize.last_miss_cycles[overflow][size_class].store(
      now, std::memory_order_relaxed);
  bool grow_by_batch =
      resize.per_class[size_class].Update(overflow, grow_by_one, &successive);
  if ((grow_by_one || grow_by_batch) && capacity != max_capacity) {
    size_t increase = 1;
    if (grow_by_batch) {
      increase = std::min(batch_length, max_capacity - capacity);
    } else if (!overflow && capacity < batch_length) {
      // On underflow we want to grow to at least batch size, because that's
      // what we want to request from transfer cache.
      increase = batch_length - capacity;
    }
    Grow(cpu, size_class, increase);
    capacity = freelist_.Capacity(cpu, size_class);
  }
  // We hit the maximum capacity limit when the size class capacity is equal to
  // its maximum allowed capacity. Record a miss due to that so that we can
  // potentially grow the max capacity for this size class later.
  if (capacity == max_capacity) {
    resize_[cpu].per_class[size_class].RecordMiss(
        PerClassMissType::kMaxCapacityTotal);
  }
  return TargetOverflowRefillCount(capacity, batch_length, successive);
}

template <class Forwarder>
std::pair<int, bool> CpuCache<Forwarder>::CacheCpuSlab() {
  auto [cpu, cached] = freelist_.CacheCpuSlab();
  if (ABSL_PREDICT_FALSE(cached) && ABSL_PREDICT_TRUE(cpu >= 0)) {
    if (ABSL_PREDICT_FALSE(
            !resize_[cpu].populated.load(std::memory_order_acquire))) {
      Populate(cpu);
    }
    const int physical_cpu = subtle::percpu::GetRealCpuUnsafe();
    if (std::atomic<bool>& populated_physical =
            resize_[physical_cpu].populated_physical_cpu;
        ABSL_PREDICT_FALSE(
            !populated_physical.load(std::memory_order_relaxed))) {
      populated_physical.store(true, std::memory_order_relaxed);
    }
  }
  return {cpu, cached};
}

template <class Forwarder>
ABSL_ATTRIBUTE_NOINLINE void CpuCache<Forwarder>::Populate(int cpu) {
  AllocationGuardSpinLockHolder h(&resize_[cpu].lock);
  if (resize_[cpu].populated.load(std::memory_order_relaxed)) {
    return;
  }
  freelist_.InitCpu(cpu, GetMaxCapacityFunctor(freelist_.GetShift()));
  resize_[cpu].populated.store(true, std::memory_order_release);
}

inline size_t subtract_at_least(std::atomic<size_t>* a, size_t min,
                                size_t max) {
  size_t cmp = a->load(std::memory_order_relaxed);
  for (;;) {
    if (cmp < min) {
      return 0;
    }
    size_t got = std::min(cmp, max);
    if (a->compare_exchange_weak(cmp, cmp - got, std::memory_order_relaxed)) {
      return got;
    }
  }
}

template <class Forwarder>
inline void CpuCache<Forwarder>::Grow(int cpu, size_t size_class,
                                      size_t desired_increase) {
  const size_t size = forwarder_.class_to_size(size_class);
  const size_t desired_bytes = desired_increase * size;
  size_t acquired_bytes =
      subtract_at_least(&resize_[cpu].available, size, desired_bytes);
  if (acquired_bytes < desired_bytes) {
    resize_[cpu].per_class[size_class].RecordMiss(
        PerClassMissType::kCapacityTotal);
  }
  if (acquired_bytes == 0) {
    return;
  }
  size_t actual_increase = acquired_bytes / size;
  TC_ASSERT_GT(actual_increase, 0);
  TC_ASSERT_LE(actual_increase, desired_increase);
  // Remember, Grow may not give us all we ask for.
  size_t increase = freelist_.Grow(
      cpu, size_class, actual_increase,
      [&](uint8_t shift) { return GetMaxCapacity(size_class, shift); });
  if (size_t unused = acquired_bytes - increase * size) {
    // return whatever we didn't use to the slack.
    resize_[cpu].available.fetch_add(unused, std::memory_order_relaxed);
  }
}

template <class Forwarder>
inline void CpuCache<Forwarder>::TryReclaimingCaches() {
  const int num_cpus = NumCPUs();

  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    // Nothing to reclaim if the cpu is not populated.
    if (!HasPopulated(cpu)) {
      continue;
    }

    uint64_t used_bytes = UsedBytes(cpu);
    uint64_t prev_used_bytes =
        resize_[cpu].reclaim_used_bytes.load(std::memory_order_relaxed);

    // Get reclaim miss and used bytes stats that were captured at the end of
    // the previous interval.
    const CpuCacheMissStats miss_stats =
        GetAndUpdateIntervalCacheMissStats(cpu, MissCount::kReclaim);
    uint64_t misses =
        uint64_t{miss_stats.underflows} + uint64_t{miss_stats.overflows};

    // Reclaim the cache if the number of used bytes and total number of misses
    // stayed constant since the last interval.
    if (used_bytes != 0 && used_bytes == prev_used_bytes && misses == 0) {
      Reclaim(cpu);
    }

    // Takes a snapshot of used bytes in the cache at the end of this interval
    // so that we can calculate if cache usage changed in the next interval.
    //
    // Reclaim occurs on a single thread. So, the relaxed store to used_bytes
    // is safe.
    resize_[cpu].reclaim_used_bytes.store(used_bytes,
                                          std::memory_order_relaxed);
  }
}

template <class Forwarder>
int CpuCache<Forwarder>::GetUpdatedMaxCapacities(
    int start_size_class, PerSizeClassMaxCapacity* max_capacity,
    int valid_entries) {
  TC_ASSERT_LT(valid_entries, kNumClasses);
  // Collect miss stats incurred during the current resize interval for all the
  // size classes.
  const int num_cpus = NumCPUs();
  absl::FixedArray<size_t> total_misses(kNumBaseClasses, 0);
  int index = 0;
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    index = 0;
    if (!HasPopulated(cpu)) continue;
    for (size_t size_class = start_size_class;
         size_class < start_size_class + kNumBaseClasses; ++size_class) {
      total_misses[index] +=
          resize_[cpu].per_class[size_class].GetAndUpdateIntervalMisses(
              PerClassMissType::kMaxCapacityTotal,
              PerClassMissType::kMaxCapacityResize);

      ++index;
    }
  }

  absl::FixedArray<SizeClassMissStat> miss_stats(kNumBaseClasses);
  index = 0;
  for (size_t size_class = start_size_class;
       size_class < start_size_class + kNumBaseClasses; ++size_class) {
    miss_stats[index] = SizeClassMissStat{.size_class = size_class,
                                          .misses = total_misses[index]};
    ++index;
  }

  // Sort the collected stats to record size classes with largest number of
  // misses in the last interval.
  std::sort(miss_stats.begin(), miss_stats.end(),
            [](SizeClassMissStat a, SizeClassMissStat b) {
              // In case of a conflict, prefer growing smaller size classes.
              if (a.misses == b.misses) {
                return a.size_class < b.size_class;
              }
              return a.misses > b.misses;
            });

  // Computing number of size classes to resize is a light-weight operation, but
  // resizing size classes involves stopping all per-cpu caches, and hence is a
  // heavy-weight operation. So, we try to be aggressive in the number of size
  // classes we would like to resize when we can, but perform resizing operation
  // sparingly.
  constexpr int kMaxCapacitiesToGrow = kNumBaseClasses / 2;

  int grown = 0;
  int max_capacity_index = valid_entries;

  // We try to grow size class max capacities by batch_size times the growth
  // factor. The growth factor starts with 5 times the batch size for the size
  // class that suffers the highest misses, and then gradually shrinks to 1 for
  // the size class with fifth-highest misses and onwards. There is nothing
  // interesting about this factor; it may be tuned in the future to increase or
  // decrease the aggresiveness of the growth.
  int growth_factor = 5;
  // Indices in miss_stats corresponding to the size classes we aim to grow
  // and shrink.
  int shrink_index = kNumBaseClasses - 1;
  for (int grow_index = 0; grow_index < kNumBaseClasses; ++grow_index) {
    // If a size class with largest misses is zero, break. Other size classes
    // should also have suffered zero misses as well.
    if (miss_stats[grow_index].misses == 0) break;

    // We grow a size class by its batch_size, while trying to shrink max
    // capacities of other size classes by the same amount. We shrink each
    // size classes' max capacity by its batch size too.
    const size_t size_class_to_grow = miss_stats[grow_index].size_class;
    const int to_grow = forwarder_.num_objects_to_move(size_class_to_grow);

    // max_capacity_index keeps track of number of entries in max_capacity
    // that are valid. If we do not find enough size classes to shrink, we
    // give up and return early. So, we only `commit` index in max_capacity
    // once we find enough size classes to shrink max capacities equal to the
    // target. next_capacity_index records a temporary index in max_capacity.
    int next_capacity_index = max_capacity_index;
    int target = to_grow * growth_factor;
    int shrunk = 0;

    // Loop until we found enough capacity from other size classes, or if we run
    // out of size classes to shrink.
    while (shrink_index > grow_index && target > 0) {
      size_t size_class_to_shrink = miss_stats[shrink_index].size_class;
      int batch_size = forwarder_.num_objects_to_move(size_class_to_shrink);
      size_t cap =
          max_capacity_[size_class_to_shrink].load(std::memory_order_relaxed);
      --shrink_index;

      // We retain at least batch_size amount of max capacity for a size
      // class.
      if (cap <= batch_size) continue;

      int to_shrink = std::min(target, batch_size);
      // Do not shrink such that max capacity falls below batch_size.
      to_shrink = std::min<size_t>(to_shrink, cap - batch_size);
      if (to_shrink == 0) continue;

      max_capacity[next_capacity_index] = PerSizeClassMaxCapacity{
          .size_class = size_class_to_shrink, .max_capacity = cap - to_shrink};
      ++next_capacity_index;
      target -= to_shrink;
      shrunk += to_shrink;
    }

    // We didn't find any size classes that may be shrunk. Break.
    if (shrunk == 0) break;

    // Update maximum capacity for the size class we intend to grow by the
    // amount we shrunk from other size classes.
    size_t cap =
        max_capacity_[size_class_to_grow].load(std::memory_order_relaxed);
    max_capacity[next_capacity_index] = PerSizeClassMaxCapacity{
        .size_class = size_class_to_grow, .max_capacity = cap + shrunk};
    ++next_capacity_index;
    max_capacity_index = next_capacity_index;

    ++grown;
    growth_factor = std::max(growth_factor - 1, 1);
    // We have enough candidates to grow. Break.
    if (grown == kMaxCapacitiesToGrow) break;
  }

  return max_capacity_index;
}

template <class Forwarder>
void CpuCache<Forwarder>::MadviseAwaySlabs(void* slab_addr, size_t slab_size) {
  // It is important that we do not MADV_REMOVE the memory, since file-backed
  // pages may SIGSEGV/SIGBUS if another thread sees the previous slab after
  // this point and reads it.
  //
  // TODO(b/214241843): we should be able to remove MADV_NOHUGEPAGE once the
  // kernel enables huge zero pages.
  // Note: we use bitwise OR to avoid short-circuiting.
  ErrnoRestorer errno_restorer;
  bool madvise_failed = false;
  do {
    madvise_failed = madvise(slab_addr, slab_size, MADV_NOHUGEPAGE) |
                     madvise(slab_addr, slab_size, MADV_DONTNEED);
  } while (madvise_failed && errno == EAGAIN);

  int ret = 0;
  if (madvise_failed) {
    // Try to unlock if madvise fails the first time.
    do {
      ret = munlock(slab_addr, slab_size);
    } while (ret == -1 && errno == EAGAIN);

    do {
      madvise_failed = madvise(slab_addr, slab_size, MADV_NOHUGEPAGE) |
                       madvise(slab_addr, slab_size, MADV_DONTNEED);
    } while (madvise_failed && errno == EAGAIN);
  }

  if (ret != 0 || madvise_failed) {
    dynamic_slab_info_.madvise_failed_bytes.fetch_add(
        slab_size, std::memory_order_relaxed);
  }
}

template <class Forwarder>
void CpuCache<Forwarder>::ResizeSizeClassMaxCapacities()
    ABSL_NO_THREAD_SAFETY_ANALYSIS {
  const int num_cpus = NumCPUs();
  const auto& topology = forwarder_.numa_topology();

  PerSizeClassMaxCapacity new_max_capacities[kNumClasses];
  size_t start_size_class = 0;
  int to_update = 0;

  // Obtain candidates to resize for size classes within each NUMA domain. We do
  // not resize across NUMA domains.
  for (int i = 0; i < topology.active_partitions(); ++i) {
    to_update = GetUpdatedMaxCapacities(start_size_class, new_max_capacities,
                                        to_update);
    start_size_class += kNumBaseClasses;
  }

  // Obtain candidates to resize within expanded size classes.
  if (kHasExpandedClasses) {
    to_update = GetUpdatedMaxCapacities(start_size_class, new_max_capacities,
                                        to_update);
  }

  // Nothing to update.
  if (to_update == 0) return;

  uint8_t per_cpu_shift = freelist_.GetShift();
  const auto shift = subtle::percpu::ToShiftType(per_cpu_shift);
  const int64_t new_slabs_size =
      subtle::percpu::GetSlabsAllocSize(shift, num_cpus);
  // Account for impending allocation/reusing of new slab so that we can avoid
  // going over memory limit.
  forwarder_.ArenaUpdateAllocatedAndNonresident(new_slabs_size, 0);

  int64_t reused_bytes;
  ResizeSlabsInfo info;
  for (int cpu = 0; cpu < num_cpus; ++cpu) resize_[cpu].lock.Lock();
  uint8_t new_resize_slab_offset =
      resize_slab_offset_.load(std::memory_order_relaxed) + 1;
  if (new_resize_slab_offset >= kResizeSlabCopies) {
    new_resize_slab_offset = 0;
  }
  resize_slab_offset_.store(new_resize_slab_offset, std::memory_order_relaxed);

  {
    // We can't allocate while holding the per-cpu spinlocks.
    AllocationGuard enforce_no_alloc;
    void* new_slabs;
    std::tie(new_slabs, reused_bytes) = AllocOrReuseSlabs(
        [&](size_t size, std::align_val_t align) {
          return forwarder_.AllocReportedImpending(size, align);
        },
        shift, num_cpus,
        ShiftOffset(per_cpu_shift, shift_bounds_.initial_shift),
        new_resize_slab_offset);

    info = freelist_.UpdateMaxCapacities(
        new_slabs,
        GetShiftMaxCapacity{max_capacity_, per_cpu_shift, shift_bounds_},
        [this](int size_class, uint16_t cap) {
          UpdateMaxCapacity(size_class, cap);
        },
        [this](int cpu) { return HasPopulated(cpu); },
        DrainHandler<CpuCache>{*this, nullptr}, new_max_capacities, to_update);
  }
  for (int cpu = 0; cpu < num_cpus; ++cpu) resize_[cpu].lock.Unlock();

  MadviseAwaySlabs(info.old_slabs, info.old_slabs_size);
  const int64_t old_slabs_size = info.old_slabs_size;
  forwarder_.ArenaUpdateAllocatedAndNonresident(-old_slabs_size,
                                                old_slabs_size - reused_bytes);
}

template <class Forwarder>
inline void CpuCache<Forwarder>::ResizeSizeClasses() {
  const int num_cpus = NumCPUs();
  // Start resizing from where we left off the last time, and resize size class
  // capacities for up to kNumCpuCachesToResize per-cpu caches.
  int cpu = last_cpu_size_class_resize_.load(std::memory_order_relaxed);
  int num_cpus_resized = 0;

  // Record the cumulative misses for the caches so that we can select the
  // size classes with the highest misses as the candidates to resize.
  for (int cpu_offset = 0; cpu_offset < num_cpus; ++cpu_offset) {
    if (++cpu >= num_cpus) {
      cpu = 0;
    }
    TC_ASSERT_GE(cpu, 0);
    TC_ASSERT_LT(cpu, num_cpus);

    // Nothing to resize if the cache is not populated.
    if (!HasPopulated(cpu)) {
      continue;
    }

    ResizeCpuSizeClasses(cpu);

    // Record full stats in previous full stat counters so that we can collect
    // stats per interval.
    for (size_t size_class = 1; size_class < kNumClasses; ++size_class) {
      resize_[cpu].per_class[size_class].UpdateIntervalMisses(
          PerClassMissType::kCapacityTotal, PerClassMissType::kCapacityResize);
    }

    if (++num_cpus_resized >= kNumCpuCachesToResize) break;
  }
  // Record the cpu hint for which the size classes were resized so that we
  // can start from the subsequent cpu in the next interval.
  last_cpu_size_class_resize_.store(cpu, std::memory_order_relaxed);
}

template <class Forwarder>
void CpuCache<Forwarder>::ResizeCpuSizeClasses(int cpu) {
  if (resize_[cpu].available.load(std::memory_order_relaxed) >=
      kMaxCpuCacheSize) {
    // We still have enough available capacity, so all size classes can just
    // grow as they see fit.
    return;
  }

  absl::FixedArray<SizeClassMissStat> miss_stats(kNumClasses - 1);
  for (size_t size_class = 1; size_class < kNumClasses; ++size_class) {
    miss_stats[size_class - 1] = SizeClassMissStat{
        .size_class = size_class,
        .misses = resize_[cpu].per_class[size_class].GetIntervalMisses(
            PerClassMissType::kCapacityTotal,
            PerClassMissType::kCapacityResize)};
  }

  // Sort the collected stats to record size classes with largest number of
  // misses in the last interval.
  std::sort(miss_stats.begin(), miss_stats.end(),
            [](SizeClassMissStat a, SizeClassMissStat b) {
              // In case of a conflict, prefer growing smaller size classes.
              if (a.misses == b.misses) {
                return a.size_class < b.size_class;
              }
              return a.misses > b.misses;
            });

  size_t available =
      resize_[cpu].available.exchange(0, std::memory_order_relaxed);
  size_t num_resizes = 0;
  {
    AllocationGuardSpinLockHolder h(&resize_[cpu].lock);
    subtle::percpu::ScopedSlabCpuStop<kNumClasses> cpu_stop(freelist_, cpu);
    const auto max_capacity = GetMaxCapacityFunctor(freelist_.GetShift());
    size_t size_classes_to_resize = 5;
    TC_ASSERT_LT(size_classes_to_resize, kNumClasses);
    for (size_t i = 0; i < size_classes_to_resize; ++i) {
      // If a size class with largest misses is zero, break. Other size classes
      // should also have suffered zero misses as well.
      if (miss_stats[i].misses == 0) break;
      const size_t size_class_to_grow = miss_stats[i].size_class;

      // If we are already at a maximum capacity, nothing to grow.
      const ssize_t can_grow = max_capacity(size_class_to_grow) -
                               freelist_.Capacity(cpu, size_class_to_grow);
      // can_grow can be negative only if slabs were resized,
      // but since we hold resize_[cpu].lock it must not happen.
      TC_ASSERT_GE(can_grow, 0);
      if (can_grow <= 0) {
        // If one of the highest miss classes is already at the max capacity,
        // we need to try to grow more classes. Otherwise, if first 5 are at
        // max capacity, resizing will stop working.
        if (size_classes_to_resize < kNumClasses) {
          size_classes_to_resize++;
        }
        continue;
      }

      num_resizes++;

      size_t size = forwarder_.class_to_size(size_class_to_grow);
      // Get total bytes to steal from other size classes. We would like to grow
      // the capacity of the size class by a batch size.
      const size_t need_bytes =
          std::min<size_t>(can_grow,
                           forwarder_.num_objects_to_move(size_class_to_grow)) *
          size;
      const ssize_t to_steal_bytes = need_bytes - available;
      if (to_steal_bytes > 0) {
        available += StealCapacityForSizeClassWithinCpu(
            cpu, {miss_stats.begin(), size_classes_to_resize}, to_steal_bytes);
      }
      size_t capacity_acquired = std::min<size_t>(can_grow, available / size);
      if (capacity_acquired != 0) {
        size_t got = freelist_.GrowOtherCache(
            cpu, size_class_to_grow, capacity_acquired, [&](uint8_t shift) {
              return GetMaxCapacity(size_class_to_grow, shift);
            });
        available -= got * size;
      }
    }
  }
  resize_[cpu].available.fetch_add(available, std::memory_order_relaxed);
  resize_[cpu].num_size_class_resizes.fetch_add(num_resizes,
                                                std::memory_order_relaxed);
}

template <class Forwarder>
inline void CpuCache<Forwarder>::ShuffleCpuCaches() {
  // Knobs that we can potentially tune depending on the workloads.
  constexpr double kBytesToStealPercent = 5.0;
  constexpr int kMaxNumStealCpus = 5;

  const int num_cpus = NumCPUs();
  absl::FixedArray<CpuMissStat> misses(num_cpus);

  // Record the cumulative misses for the caches so that we can select the
  // caches with the highest misses as the candidates to steal the cache for.
  int max_populated_cpu = -1;
  int num_populated_cpus = 0;
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    if (!HasPopulated(cpu)) {
      continue;
    }
    const CpuCacheMissStats miss_stats =
        GetIntervalCacheMissStats(cpu, MissCount::kShuffle);
    misses[num_populated_cpus] = {cpu,
                                  miss_stats.underflows + miss_stats.overflows};
    max_populated_cpu = cpu;
    ++num_populated_cpus;
  }
  if (max_populated_cpu == -1) {
    return;
  }

  // Sorts misses to identify cpus with highest misses.
  //
  // TODO(vgogte): We can potentially sort the entire misses array and use that
  // in StealFromOtherCache to determine cpus to steal from. That is, [0,
  // num_dest_cpus) may be the destination cpus and [num_dest_cpus, num_cpus)
  // may be cpus we may steal from. We can iterate through the array in a
  // descending order to steal from them. The upside of this mechanism is that
  // we would be able to do a more fair stealing, starting with cpus with lowest
  // misses. The downside of this mechanism is that we would have to sort the
  // entire misses array. This might be compute intensive on servers with high
  // number of cpus (eg. Rome, Milan). We need to investigate the compute
  // required to implement this.
  const int num_dest_cpus = std::min(num_populated_cpus, kMaxNumStealCpus);
  std::partial_sort(misses.begin(), misses.begin() + num_dest_cpus,
                    misses.begin() + num_populated_cpus,
                    [](CpuMissStat a, CpuMissStat b) {
                      if (a.misses == b.misses) {
                        return a.cpu < b.cpu;
                      }
                      return a.misses > b.misses;
                    });

  // Try to steal kBytesToStealPercent percentage of max_per_cpu_cache_size for
  // each destination cpu cache.
  size_t to_steal = kBytesToStealPercent / 100.0 * CacheLimit();
  for (int i = 0; i < num_dest_cpus; ++i) {
    if (misses[i].misses == 0) {
      break;
    }
    absl::Span<CpuMissStat> skip = {misses.begin(), static_cast<size_t>(i + 1)};
    StealFromOtherCache(misses[i].cpu, max_populated_cpu, skip, to_steal);
  }

  // Takes a snapshot of underflows and overflows at the end of this interval
  // so that we can calculate the misses that occurred in the next interval.
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    UpdateIntervalCacheMissStats(cpu, MissCount::kShuffle);
  }
}

template <class Forwarder>
inline void CpuCache<Forwarder>::StealFromOtherCache(
    int cpu, int max_populated_cpu, absl::Span<CpuMissStat> skip_cpus,
    size_t bytes) {
  constexpr double kCacheMissThreshold = 0.80;

  const CpuCacheMissStats dest_misses =
      GetIntervalCacheMissStats(cpu, MissCount::kShuffle);

  if (resize_[cpu].available.load(std::memory_order_relaxed) >= kMaxSize) {
    // We still have enough available capacity, so all size classes can just
    // grow as they see fit.
    return;
  }

  size_t acquired = 0;

  // We use next_cpu_cache_steal_ as a hint to start our search for cpu ids to
  // steal from so that we can iterate through the cpus in a nice round-robin
  // fashion.
  int src_cpu = next_cpu_cache_steal_;

  // We iterate through max_populate_cpus number of cpus to steal from.
  // max_populate_cpus records the max cpu id that has been populated. Note
  // that, any intermediate changes since the max_populated_cpus was measured
  // may have populated higher cpu ids, but we do not include those in the
  // search. The approximation prevents us from doing another pass through the
  // cpus to just find the latest populated cpu id.
  //
  // We break from the loop once we iterate through all the cpus once, or if the
  // total number of acquired bytes is higher than or equal to the desired bytes
  // we want to steal.
  for (int i = 0; i <= max_populated_cpu && acquired < bytes; ++i, ++src_cpu) {
    if (src_cpu > max_populated_cpu) {
      src_cpu = 0;
    }
    TC_ASSERT_LE(0, src_cpu);
    TC_ASSERT_LE(src_cpu, max_populated_cpu);

    // We do not steal from the CPUs we want to grow. Maybe we can explore
    // combining this with stealing from the same CPU later.
    bool skip = false;
    for (auto dest : skip_cpus) {
      if (src_cpu == dest.cpu) {
        skip = true;
        break;
      }
    }
    if (skip) continue;

    // We do not steal from the cache that hasn't been populated yet.
    if (!HasPopulated(src_cpu)) continue;

    // We do not steal from cache that has capacity less than our lower
    // capacity threshold.
    if (Capacity(src_cpu) < kCacheCapacityThreshold * CacheLimit()) continue;

    const CpuCacheMissStats src_misses =
        GetIntervalCacheMissStats(src_cpu, MissCount::kShuffle);

    // If underflows and overflows from the source cpu are higher, we do not
    // steal from that cache. We consider the cache as a candidate to steal from
    // only when its misses are lower than 0.8x that of the dest cache.
    if (src_misses.underflows > kCacheMissThreshold * dest_misses.underflows ||
        src_misses.overflows > kCacheMissThreshold * dest_misses.overflows)
      continue;

    // Try to steal available capacity from the target cpu, if any.
    // This is cheaper than remote slab operations.
    size_t stolen =
        subtract_at_least(&resize_[src_cpu].available, 0, bytes - acquired);
    if (stolen != 0) {
      resize_[src_cpu].capacity.fetch_sub(stolen, std::memory_order_relaxed);
      acquired += stolen;
      if (acquired >= bytes) {
        continue;
      }
    }

    AllocationGuardSpinLockHolder h(&resize_[src_cpu].lock);
    subtle::percpu::ScopedSlabCpuStop<kNumClasses> cpu_stop(freelist_, src_cpu);
    size_t source_size_class = resize_[src_cpu].next_steal;
    for (size_t i = 1; i < kNumClasses; ++i, ++source_size_class) {
      if (source_size_class >= kNumClasses) {
        source_size_class = 1;
      }
      if (size_t stolen = ShrinkOtherCache(src_cpu, source_size_class)) {
        resize_[src_cpu].capacity.fetch_sub(stolen, std::memory_order_relaxed);
        acquired += stolen;
        if (acquired >= bytes) {
          break;
        }
      }
    }
    resize_[src_cpu].next_steal = source_size_class;
  }
  // Record the last cpu id we stole from, which would provide a hint to the
  // next time we iterate through the cpus for stealing.
  next_cpu_cache_steal_ = src_cpu;

  // Increment the capacity of the destination cpu cache by the amount of bytes
  // acquired from source caches.
  if (acquired) {
    resize_[cpu].available.fetch_add(acquired, std::memory_order_relaxed);
    resize_[cpu].capacity.fetch_add(acquired, std::memory_order_relaxed);
  }
}

template <class Forwarder>
size_t CpuCache<Forwarder>::ShrinkOtherCache(int cpu, size_t size_class) {
  TC_ASSERT(cpu >= 0 && cpu < NumCPUs(), "cpu=%d", cpu);
  TC_ASSERT(size_class >= 1 && size_class < kNumClasses);
  TC_ASSERT(resize_[cpu].lock.IsHeld());
  const size_t capacity = freelist_.Capacity(cpu, size_class);
  if (capacity == 0) {
    return 0;  // Nothing to steal.
  }

  const size_t length = freelist_.Length(cpu, size_class);
  const size_t batch_length = forwarder_.num_objects_to_move(size_class);
  size_t size = forwarder_.class_to_size(size_class);

  // Clock-like algorithm to prioritize size classes for shrinking.
  //
  // Each size class has quiescent ticks counter which is incremented as we
  // pass it, the counter is reset to 0 in UpdateCapacity on grow.
  // If the counter value is 0, then we've just tried to grow the size class,
  // so it makes little sense to shrink it back. The higher counter value
  // the longer ago we grew the list and the more probable it is that
  // the full capacity is unused.
  //
  // Then, we calculate "shrinking score", the higher the score the less we
  // we want to shrink this size class. The score is considerably skewed
  // towards larger size classes: smaller classes are usually used more
  // actively and we also benefit less from shrinking smaller classes (steal
  // less capacity). Then, we also avoid shrinking full freelists as we will
  // need to evict an object and then go to the central freelist to return it.
  // Then, we also avoid shrinking freelists that are just above batch size,
  // because shrinking them will disable transfer cache.
  //
  // Finally, we shrink if the ticks counter is >= the score.
  uint32_t score = 0;
  // Note: the following numbers are based solely on intuition, common sense
  // and benchmarking results.
  if (size <= 144) {
    score = 2 + (length >= capacity) +
            (length >= batch_length && length < 2 * batch_length);
  } else if (size <= 1024) {
    score = 1 + (length >= capacity) +
            (length >= batch_length && length < 2 * batch_length);
  } else if (size <= (64 << 10)) {
    score = (length >= capacity);
  }
  if (resize_[cpu].per_class[size_class].Tick() < score) {
    return 0;
  }

  // Finally, try to shrink.
  if (!freelist_.ShrinkOtherCache(
          cpu, size_class, /*len=*/1,
          [this](size_t size_class, void** batch, size_t count) {
            TC_ASSERT_EQ(count, 1);
            ReleaseToBackingCache(size_class, {batch, count});
          })) {
    return 0;
  }
  return size;
}

template <class Forwarder>
inline size_t CpuCache<Forwarder>::StealCapacityForSizeClassWithinCpu(
    int cpu, absl::Span<SizeClassMissStat> dest_size_classes, size_t bytes) {
  // Steal from other sizeclasses.  Try to go in a nice circle.
  // Complicated by sizeclasses actually being 1-indexed.
  size_t acquired = 0;
  size_t source_size_class = resize_[cpu].next_steal;
  for (size_t i = 1; i < kNumClasses; ++i, ++source_size_class) {
    if (source_size_class >= kNumClasses) {
      source_size_class = 1;
    }
    // Decide if we want to steal source_size_class.
    // Don't shrink classes we want to grow.
    bool skip = false;
    for (auto dest : dest_size_classes) {
      if (source_size_class == dest.size_class && dest.misses != 0) {
        skip = true;
        break;
      }
    }
    if (skip) {
      continue;
    }
    acquired += ShrinkOtherCache(cpu, source_size_class);
    if (acquired >= bytes) {
      // can't steal any more or don't need to
      break;
    }
  }
  // update the hint
  resize_[cpu].next_steal = source_size_class;
  return acquired;
}

template <class Forwarder>
void CpuCache<Forwarder>::DeallocateSlow(void* ptr, size_t size_class) {
  DeallocateSlowNoHooks(ptr, size_class);
  MaybeForceSlowPath();
}

template <class Forwarder>
void CpuCache<Forwarder>::DeallocateSlowNoHooks(void* ptr, size_t size_class) {
  if (BypassCpuCache(size_class)) {
    return forwarder_.sharded_transfer_cache().Push(size_class, ptr);
  }
  auto [cpu, cached] = CacheCpuSlab();
  if (ABSL_PREDICT_FALSE(cached)) {
    if (ABSL_PREDICT_FALSE(cpu < 0)) {
      // The cpu is stopped.
      return ReleaseToBackingCache(size_class, {&ptr, 1});
    }
    if (DeallocateFast(ptr, size_class)) {
      return;
    }
  }
  RecordCacheMissStat(cpu, false);
  const size_t target = UpdateCapacity(cpu, size_class, true);
  size_t total = 0;
  size_t count = 1;
  void* batch[kMaxObjectsToMove];
  batch[0] = ptr;
  do {
    size_t want = std::min(kMaxObjectsToMove, target - total);
    if (count < want) {
      count += freelist_.PopBatch(size_class, batch + count, want - count);
    }
    if (!count) break;

    total += count;
    ReleaseToBackingCache(size_class, absl::Span<void*>(batch, count));
    if (count != kMaxObjectsToMove) break;
    count = 0;
  } while (total < target);
}

template <class Forwarder>
inline uint64_t CpuCache<Forwarder>::Allocated(int target_cpu) const {
  TC_ASSERT_GE(target_cpu, 0);
  if (!HasPopulated(target_cpu)) {
    return 0;
  }

  uint64_t total = 0;
  for (int size_class = 1; size_class < kNumClasses; size_class++) {
    int size = forwarder_.class_to_size(size_class);
    total += size * freelist_.Capacity(target_cpu, size_class);
  }
  return total;
}

template <class Forwarder>
inline uint64_t CpuCache<Forwarder>::UsedBytes(int target_cpu) const {
  TC_ASSERT_GE(target_cpu, 0);
  if (!HasPopulated(target_cpu)) {
    return 0;
  }

  uint64_t total = 0;
  for (int size_class = 1; size_class < kNumClasses; size_class++) {
    int size = forwarder_.class_to_size(size_class);
    total += size * freelist_.Length(target_cpu, size_class);
  }
  return total;
}

template <class Forwarder>
inline bool CpuCache<Forwarder>::HasPopulated(int target_cpu) const {
  TC_ASSERT_GE(target_cpu, 0);
  return resize_[target_cpu].populated.load(std::memory_order_relaxed);
}

template <class Forwarder>
inline bool CpuCache<Forwarder>::HasPhysicalPopulated(int target_cpu) const {
  TC_ASSERT_GE(target_cpu, 0);
  return resize_[target_cpu].populated_physical_cpu.load(
      std::memory_order_relaxed);
}

template <class Forwarder>
inline PerCPUMetadataState CpuCache<Forwarder>::MetadataMemoryUsage() const {
  return freelist_.MetadataMemoryUsage();
}

template <class Forwarder>
inline uint64_t CpuCache<Forwarder>::TotalUsedBytes() const {
  uint64_t total = 0;
  for (int cpu = 0, num_cpus = NumCPUs(); cpu < num_cpus; ++cpu) {
    total += UsedBytes(cpu);
  }
  return total;
}

template <class Forwarder>
inline uint64_t CpuCache<Forwarder>::TotalObjectsOfClass(
    size_t size_class) const {
  TC_ASSERT_LT(size_class, kNumClasses);
  uint64_t total_objects = 0;
  if (size_class > 0) {
    for (int cpu = 0, n = NumCPUs(); cpu < n; cpu++) {
      if (!HasPopulated(cpu)) {
        continue;
      }
      total_objects += freelist_.Length(cpu, size_class);
    }
  }
  return total_objects;
}

template <class Forwarder>
inline uint64_t CpuCache<Forwarder>::Unallocated(int cpu) const {
  return resize_[cpu].available.load(std::memory_order_relaxed);
}

template <class Forwarder>
inline uint64_t CpuCache<Forwarder>::Capacity(int cpu) const {
  return resize_[cpu].capacity.load(std::memory_order_relaxed);
}

template <class Forwarder>
inline uint64_t CpuCache<Forwarder>::CacheLimit() const {
  return max_per_cpu_cache_size_.load(std::memory_order_relaxed);
}

template <class Forwarder>
inline void CpuCache<Forwarder>::SetCacheLimit(uint64_t v) {
  // TODO(b/179516472): Drain cores as required.
  max_per_cpu_cache_size_.store(v, std::memory_order_relaxed);
}

template <class CpuCache>
struct DrainHandler {
  void operator()(int cpu, size_t size_class, void** batch, size_t count,
                  size_t cap) const {
    const size_t size = cache.forwarder_.class_to_size(size_class);
    const size_t batch_length =
        cache.forwarder_.num_objects_to_move(size_class);
    if (bytes != nullptr) *bytes += count * size;
    // Drain resets capacity to 0, so return the allocated capacity to that
    // CPU's slack.
    cache.resize_[cpu].available.fetch_add(cap * size,
                                           std::memory_order_relaxed);
    for (size_t i = 0; i < count; i += batch_length) {
      size_t n = std::min(batch_length, count - i);
      cache.ReleaseToBackingCache(size_class, absl::Span<void*>(batch + i, n));
    }
  }

  CpuCache& cache;
  uint64_t* bytes;
};

template <class Forwarder>
inline uint64_t CpuCache<Forwarder>::Reclaim(int cpu) {
  AllocationGuardSpinLockHolder h(&resize_[cpu].lock);

  // If we haven't populated this core, freelist_.Drain() will touch the memory
  // (for writing) as part of its locking process.  Avoid faulting new pages as
  // part of a release process.
  if (!HasPopulated(cpu)) {
    return 0;
  }

  uint64_t bytes = 0;
  freelist_.Drain(cpu, DrainHandler<CpuCache>{*this, &bytes});

  // Record that the reclaim occurred for this CPU.
  resize_[cpu].num_reclaims.store(
      resize_[cpu].num_reclaims.load(std::memory_order_relaxed) + 1,
      std::memory_order_relaxed);
  resize_[cpu].last_reclaim.store(absl::base_internal::CycleClock::Now(),
                                  std::memory_order_relaxed);

  return bytes;
}
template <class Forwarder>
inline uint64_t CpuCache<Forwarder>::GetNumResizes(int cpu) const {
  return resize_[cpu].num_size_class_resizes.load(std::memory_order_relaxed);
}

template <class Forwarder>
inline uint64_t CpuCache<Forwarder>::GetNumResizes() const {
  uint64_t resizes = 0;
  const int num_cpus = NumCPUs();
  for (int cpu = 0; cpu < num_cpus; ++cpu)
    resizes +=
        resize_[cpu].num_size_class_resizes.load(std::memory_order_relaxed);
  return resizes;
}

template <class Forwarder>
inline uint64_t CpuCache<Forwarder>::GetNumReclaims(int cpu) const {
  return resize_[cpu].num_reclaims.load(std::memory_order_relaxed);
}

template <class Forwarder>
inline uint64_t CpuCache<Forwarder>::GetNumReclaims() const {
  uint64_t reclaims = 0;
  const int num_cpus = NumCPUs();
  for (int cpu = 0; cpu < num_cpus; ++cpu)
    reclaims += resize_[cpu].num_reclaims.load(std::memory_order_relaxed);
  return reclaims;
}

template <class Forwarder>
inline std::pair<void*, size_t> CpuCache<Forwarder>::AllocOrReuseSlabs(
    absl::FunctionRef<void*(size_t, std::align_val_t)> alloc,
    subtle::percpu::Shift shift, int num_cpus, uint8_t shift_offset,
    uint8_t resize_offset) {
  TC_ASSERT_LT(resize_offset, kResizeSlabCopies);
  TC_ASSERT_LT(shift_offset, kNumPossiblePerCpuShifts);
  int slab_offset = kNumPossiblePerCpuShifts * resize_offset + shift_offset;
  TC_ASSERT_LT(slab_offset, kTotalPossibleSlabs);
  void*& reused_slabs = slabs_by_shift_[slab_offset];
  const size_t size = GetSlabsAllocSize(shift, num_cpus);
  const bool can_reuse = reused_slabs != nullptr;
  if (can_reuse) {
    // Enable huge pages for reused slabs.
    // TODO(b/214241843): we should be able to remove this once the kernel
    // enables huge zero pages.
    ErrnoRestorer errno_restorer;
    madvise(reused_slabs, size, MADV_HUGEPAGE);
  } else {
    reused_slabs = alloc(size, subtle::percpu::kPhysicalPageAlign);
    // MSan does not see writes in assembly.
    ANNOTATE_MEMORY_IS_INITIALIZED(reused_slabs, size);
  }
  return {reused_slabs, can_reuse ? size : 0};
}

template <class Forwarder>
inline typename CpuCache<Forwarder>::DynamicSlabResize
CpuCache<Forwarder>::ShouldResizeSlab() {
  const int num_cpus = NumCPUs();
  CpuCacheMissStats total_misses{};
  DynamicSlabResize resize = DynamicSlabResize::kNoop;
  const bool wider_slabs_enabled = UseWiderSlabs();
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    CpuCacheMissStats misses =
        GetAndUpdateIntervalCacheMissStats(cpu, MissCount::kSlabResize);
    total_misses += misses;

    // If overflows to underflows ratio exceeds the threshold, grow the slab.
    // Increase counts by 1 during comparison so that we can still compare the
    // ratio to the threshold when underflows is zero.
    if (misses.overflows + 1 >
        (misses.underflows + 1) *
            forwarder_.per_cpu_caches_dynamic_slab_grow_threshold()) {
      resize = DynamicSlabResize::kGrow;
    }
  }

  // When wider slabs featuee is enabled, we try to grow slabs when the
  // condition for at least one cpu cache is met. Else, we use total misses to
  // figure out whether to grow the slab, shrink it, or do nothing.
  if (wider_slabs_enabled && resize == DynamicSlabResize::kGrow) {
    return resize;
  }

  // As a simple heuristic, we decide to grow if the total number of overflows
  // is large compared to total number of underflows during the growth period.
  // If the slab size was infinite, we would expect 0 overflows. If the slab
  // size was 0, we would expect approximately equal numbers of underflows and
  // overflows.
  if (total_misses.overflows + 1 >
      (total_misses.underflows + 1) *
          forwarder_.per_cpu_caches_dynamic_slab_grow_threshold()) {
    return DynamicSlabResize::kGrow;
  } else if (total_misses.overflows <
             total_misses.underflows *
                 forwarder_.per_cpu_caches_dynamic_slab_shrink_threshold()) {
    return DynamicSlabResize::kShrink;
  }

  return DynamicSlabResize::kNoop;
}

template <class Forwarder>
void CpuCache<Forwarder>::ResizeSlabIfNeeded() ABSL_NO_THREAD_SAFETY_ANALYSIS {
  uint8_t per_cpu_shift = freelist_.GetShift();

  const int num_cpus = NumCPUs();
  const DynamicSlabResize resize = ShouldResizeSlab();

  if (resize == DynamicSlabResize::kGrow) {
    if (per_cpu_shift == shift_bounds_.max_shift) return;
    ++per_cpu_shift;
    dynamic_slab_info_
        .grow_count[ShiftOffset(per_cpu_shift, shift_bounds_.initial_shift)]
        .fetch_add(1, std::memory_order_relaxed);
  } else if (resize == DynamicSlabResize::kShrink) {
    if (per_cpu_shift == shift_bounds_.initial_shift) return;
    --per_cpu_shift;
    dynamic_slab_info_
        .shrink_count[ShiftOffset(per_cpu_shift, shift_bounds_.initial_shift)]
        .fetch_add(1, std::memory_order_relaxed);
  } else {
    return;
  }

  const auto new_shift = subtle::percpu::ToShiftType(per_cpu_shift);
  const int64_t new_slabs_size =
      subtle::percpu::GetSlabsAllocSize(new_shift, num_cpus);
  // Account for impending allocation/reusing of new slab so that we can avoid
  // going over memory limit.
  forwarder_.ArenaUpdateAllocatedAndNonresident(new_slabs_size, 0);

  for (int cpu = 0; cpu < num_cpus; ++cpu) resize_[cpu].lock.Lock();
  ResizeSlabsInfo info;
  const uint8_t resize_offset =
      resize_slab_offset_.load(std::memory_order_relaxed);
  int64_t reused_bytes;
  {
    // We can't allocate while holding the per-cpu spinlocks.
    AllocationGuard enforce_no_alloc;

    void* new_slabs;
    std::tie(new_slabs, reused_bytes) = AllocOrReuseSlabs(
        [&](size_t size, std::align_val_t align) {
          return forwarder_.AllocReportedImpending(size, align);
        },
        new_shift, num_cpus,
        ShiftOffset(per_cpu_shift, shift_bounds_.initial_shift), resize_offset);
    info = freelist_.ResizeSlabs(
        new_shift, new_slabs,
        GetShiftMaxCapacity{max_capacity_, per_cpu_shift, shift_bounds_},
        [this](int cpu) { return HasPopulated(cpu); },
        DrainHandler<CpuCache>{*this, nullptr});
  }
  for (int cpu = 0; cpu < num_cpus; ++cpu) resize_[cpu].lock.Unlock();

  MadviseAwaySlabs(info.old_slabs, info.old_slabs_size);
  const int64_t old_slabs_size = info.old_slabs_size;
  forwarder_.ArenaUpdateAllocatedAndNonresident(-old_slabs_size,
                                                old_slabs_size - reused_bytes);
}

template <class Forwarder>
inline void CpuCache<Forwarder>::RecordCacheMissStat(const int cpu,
                                                     const bool is_alloc) {
  MissCounts& misses =
      is_alloc ? resize_[cpu].underflows : resize_[cpu].overflows;
  auto& c = misses[MissCount::kTotal];
  c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

template <class Forwarder>
inline typename CpuCache<Forwarder>::CpuCacheMissStats
CpuCache<Forwarder>::GetTotalCacheMissStats(int cpu) const {
  CpuCacheMissStats stats;
  stats.underflows = resize_[cpu].underflows[MissCount::kTotal].load(
      std::memory_order_relaxed);
  stats.overflows =
      resize_[cpu].overflows[MissCount::kTotal].load(std::memory_order_relaxed);
  return stats;
}

template <class Forwarder>
inline typename CpuCache<Forwarder>::CpuCacheMissStats
CpuCache<Forwarder>::GetTotalCacheMissStats() const {
  CpuCacheMissStats stats;
  const int num_cpus = NumCPUs();
  for (int cpu = 0; cpu < num_cpus; ++cpu) stats += GetTotalCacheMissStats(cpu);
  return stats;
}

template <class Forwarder>
inline typename CpuCache<Forwarder>::CpuCacheMissStats
CpuCache<Forwarder>::GetIntervalCacheMissStats(int cpu,
                                               MissCount miss_count) const {
  TC_ASSERT_NE(miss_count, MissCount::kTotal);
  TC_ASSERT_LT(miss_count, MissCount::kNumCounts);
  const auto get_safe_miss_diff = [miss_count](MissCounts& misses) {
    const size_t total_misses =
        misses[MissCount::kTotal].load(std::memory_order_relaxed);
    const size_t interval_misses =
        misses[miss_count].load(std::memory_order_relaxed);
    // In case of a size_t overflow, we wrap around to 0.
    return total_misses > interval_misses ? total_misses - interval_misses : 0;
  };
  return {get_safe_miss_diff(resize_[cpu].underflows),
          get_safe_miss_diff(resize_[cpu].overflows)};
}

template <class Forwarder>
void CpuCache<Forwarder>::UpdateIntervalCacheMissStats(int cpu,
                                                       MissCount miss_count) {
  CpuCacheMissStats total_stats = GetTotalCacheMissStats(cpu);
  // Takes a snapshot of misses at the end of this interval so that we can
  // calculate the misses that occurred in the next interval.
  //
  // Interval updates occur on a single thread so relaxed stores to interval
  // miss stats are safe.
  resize_[cpu].underflows[miss_count].store(total_stats.underflows,
                                            std::memory_order_relaxed);
  resize_[cpu].overflows[miss_count].store(total_stats.overflows,
                                           std::memory_order_relaxed);
}

template <class Forwarder>
inline typename CpuCache<Forwarder>::CpuCacheMissStats
CpuCache<Forwarder>::GetAndUpdateIntervalCacheMissStats(int cpu,
                                                        MissCount miss_count) {
  // Note: it's possible for cache misses to occur between these two calls, but
  // there's likely to be few of them so we don't handle them specially.
  CpuCacheMissStats interval_stats = GetIntervalCacheMissStats(cpu, miss_count);
  UpdateIntervalCacheMissStats(cpu, miss_count);
  return interval_stats;
}

template <class Forwarder>
size_t CpuCache<Forwarder>::GetIntervalSizeClassMisses(
    int cpu, size_t size_class, PerClassMissType total_type,
    PerClassMissType interval_type) {
  return resize_[cpu].per_class[size_class].GetIntervalMisses(total_type,
                                                              interval_type);
}

template <class Forwarder>
inline typename CpuCache<Forwarder>::SizeClassCapacityStats
CpuCache<Forwarder>::GetSizeClassCapacityStats(size_t size_class) const {
  SizeClassCapacityStats stats;
  int num_populated = 0;
  // We use a local variable here, instead of directly updating min_capacity in
  // SizeClassCapacityStats struct to make sure we do not end up with SIZE_MAX
  // in stats.min_capacity when num_populated is equal to zero.
  size_t min_capacity = SIZE_MAX;
  const double now = absl::base_internal::CycleClock::Now();
  const double frequency = absl::base_internal::CycleClock::Frequency();

  // Scan through all per-CPU caches and calculate minimum, average and maximum
  // capacities for the size class <size_class> across all the populated caches.
  for (int cpu = 0, num_cpus = NumCPUs(); cpu < num_cpus; ++cpu) {
    // We do not include stats for non-populated cpus in our average.
    if (!HasPopulated(cpu)) {
      continue;
    }

    ++num_populated;

    const auto last_reclaim =
        resize_[cpu].last_reclaim.load(std::memory_order_relaxed);

    const auto last_underflow_cycles =
        resize_[cpu].last_miss_cycles[0][size_class].load(
            std::memory_order_relaxed);
    const auto last_overflow_cycles =
        resize_[cpu].last_miss_cycles[1][size_class].load(
            std::memory_order_relaxed);

    size_t cap = freelist_.Capacity(cpu, size_class);
    stats.max_capacity = std::max(stats.max_capacity, cap);
    min_capacity = std::min(min_capacity, cap);
    stats.avg_capacity += cap;

    if (last_reclaim >= last_underflow_cycles ||
        last_reclaim >= last_overflow_cycles) {
      // Don't consider the underflow/overflow time on this CPU if we have
      // recently reclaimed.
      continue;
    }

    if (cap == 0) {
      // Or if the capacity is empty.  We may simply not be allocating this size
      // class.
      continue;
    }

    const absl::Duration last_underflow =
        absl::Seconds((now - last_underflow_cycles) / frequency);
    const absl::Duration last_overflow =
        absl::Seconds((now - last_overflow_cycles) / frequency);

    if (last_overflow < stats.min_last_overflow) {
      stats.min_last_overflow = last_overflow;
      stats.min_last_overflow_cpu_id = cpu;
    }
    if (last_overflow > stats.max_last_overflow) {
      stats.max_last_overflow = last_overflow;
      stats.max_last_overflow_cpu_id = cpu;
    }
    if (last_underflow < stats.min_last_underflow) {
      stats.min_last_underflow = last_underflow;
      stats.min_last_underflow_cpu_id = cpu;
    }
    if (last_underflow > stats.max_last_underflow) {
      stats.max_last_underflow = last_underflow;
      stats.max_last_underflow_cpu_id = cpu;
    }
    stats.max_capacity_misses +=
        resize_[cpu].per_class[size_class].GetIntervalMisses(
            PerClassMissType::kMaxCapacityTotal,
            PerClassMissType::kMaxCapacityResize);
  }
  if (num_populated > 0) {
    stats.avg_capacity /= num_populated;
    stats.min_capacity = min_capacity;
  }
  return stats;
}

template <class Forwarder>
inline void CpuCache<Forwarder>::Print(Printer& out) const {
  out.printf("------------------------------------------------\n");
  out.printf("Bytes in per-CPU caches (per cpu limit: %u bytes)\n",
             CacheLimit());
  out.printf("------------------------------------------------\n");

  const CpuSet allowed_cpus = FillActiveCpuMask();
  const int num_cpus = NumCPUs();

  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    static constexpr double MiB = 1048576.0;

    uint64_t rbytes = UsedBytes(cpu);
    bool populated = HasPopulated(cpu);
    bool physical_populated = HasPhysicalPopulated(cpu);
    uint64_t unallocated = Unallocated(cpu);
    out.printf(
        "cpu %3d: %12u"
        " bytes (%7.1f MiB) with"
        "%12u bytes unallocated %s%s%s\n",
        cpu, rbytes, rbytes / MiB, unallocated,
        allowed_cpus.IsSet(cpu) ? " active" : "", populated ? " populated" : "",
        physical_populated ? " physical-populated" : "");
  }

  out.printf("------------------------------------------------\n");
  out.printf("Size class capacity statistics in per-cpu caches\n");
  out.printf("------------------------------------------------\n");

  for (int size_class = 1; size_class < kNumClasses; ++size_class) {
    SizeClassCapacityStats stats = GetSizeClassCapacityStats(size_class);
    out.printf(
        "class %3d [ %8zu bytes ] : "
        "%6zu (minimum), %7.1f (average), %6zu (maximum), %6zu maximum "
        "allowed capacity, "
        "maximum capacity misses %8zu, "
        "(underflow: [%d us CPU %d, %d us CPU %d]; "
        "overflow [%d us CPU %d, %d us CPU %d]\n",
        size_class, forwarder_.class_to_size(size_class), stats.min_capacity,
        stats.avg_capacity, stats.max_capacity,
        GetMaxCapacity(size_class, freelist_.GetShift()),
        stats.max_capacity_misses,
        absl::ToInt64Microseconds(stats.min_last_underflow),
        stats.min_last_underflow_cpu_id,
        absl::ToInt64Microseconds(stats.max_last_underflow),
        stats.max_last_underflow_cpu_id,
        absl::ToInt64Microseconds(stats.min_last_overflow),
        stats.min_last_overflow_cpu_id,
        absl::ToInt64Microseconds(stats.max_last_overflow),
        stats.max_last_overflow_cpu_id);
  }

  out.printf("------------------------------------------------\n");
  out.printf("Number of per-CPU cache underflows, overflows, and reclaims\n");
  out.printf("------------------------------------------------\n");
  const auto print_miss_stats = [&out](CpuCacheMissStats miss_stats,
                                       uint64_t reclaims, uint64_t resizes) {
    out.printf(
        "%12u underflows,"
        "%12u overflows, overflows / underflows: %5.2f, "
        "%12u reclaims,"
        "%12u resizes\n",
        miss_stats.underflows, miss_stats.overflows,
        safe_div(miss_stats.overflows, miss_stats.underflows), reclaims,
        resizes);
  };
  out.printf("Total  :");
  print_miss_stats(GetTotalCacheMissStats(), GetNumReclaims(), GetNumResizes());
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    out.printf("cpu %3d:", cpu);
    print_miss_stats(GetTotalCacheMissStats(cpu), GetNumReclaims(cpu),
                     GetNumResizes(cpu));
  }

  out.printf("------------------------------------------------\n");
  out.printf("Per-CPU cache slab resizing info:\n");
  out.printf("------------------------------------------------\n");
  uint8_t current_shift = freelist_.GetShift();
  out.printf("Current shift: %3d (slab size: %4d KiB)\n", current_shift,
             (1 << current_shift) / 1024);
  for (int shift = 0; shift < kNumPossiblePerCpuShifts; ++shift) {
    out.printf("shift %3d:", shift + shift_bounds_.initial_shift);
    out.printf(
        "%12u growths, %12u shrinkages\n",
        dynamic_slab_info_.grow_count[shift].load(std::memory_order_relaxed),
        dynamic_slab_info_.shrink_count[shift].load(std::memory_order_relaxed));
  }
  out.printf(
      "%12u bytes for which MADVISE_DONTNEED failed\n",
      dynamic_slab_info_.madvise_failed_bytes.load(std::memory_order_relaxed));
}

template <class Forwarder>
inline void CpuCache<Forwarder>::PrintInPbtxt(PbtxtRegion& region) const {
  const CpuSet allowed_cpus = FillActiveCpuMask();

  int physical_used = 0;
  for (int cpu = 0, num_cpus = NumCPUs(); cpu < num_cpus; ++cpu) {
    PbtxtRegion entry = region.CreateSubRegion("cpu_cache");
    uint64_t rbytes = UsedBytes(cpu);
    bool populated = HasPopulated(cpu);
    bool physical_populated = HasPhysicalPopulated(cpu);
    if (physical_populated) {
      physical_used++;
    }
    uint64_t unallocated = Unallocated(cpu);
    CpuCacheMissStats miss_stats = GetTotalCacheMissStats(cpu);
    uint64_t reclaims = GetNumReclaims(cpu);
    uint64_t resizes = GetNumResizes(cpu);
    entry.PrintI64("cpu", cpu);
    entry.PrintI64("used", rbytes);
    entry.PrintI64("unused", unallocated);
    entry.PrintBool("active", allowed_cpus.IsSet(cpu));
    entry.PrintBool("populated", populated);
    entry.PrintI64("underflows", miss_stats.underflows);
    entry.PrintI64("overflows", miss_stats.overflows);
    entry.PrintI64("reclaims", reclaims);
    entry.PrintI64("size_class_resizes", resizes);
  }

  region.PrintI64("physical_cores_used", physical_used);

  // Record size class capacity statistics.
  for (int size_class = 1; size_class < kNumClasses; ++size_class) {
    SizeClassCapacityStats stats = GetSizeClassCapacityStats(size_class);
    PbtxtRegion entry = region.CreateSubRegion("size_class_capacity");
    entry.PrintI64("sizeclass", forwarder_.class_to_size(size_class));
    entry.PrintI64("min_capacity", stats.min_capacity);
    entry.PrintDouble("avg_capacity", stats.avg_capacity);
    entry.PrintI64("max_capacity", stats.max_capacity);
    entry.PrintI64("max_allowed_capacity",
                   GetMaxCapacity(size_class, freelist_.GetShift()));

    entry.PrintI64("min_last_underflow_ns",
                   absl::ToInt64Nanoseconds(stats.min_last_underflow));
    entry.PrintI64("max_last_underflow_ns",
                   absl::ToInt64Nanoseconds(stats.max_last_underflow));
    entry.PrintI64("min_last_overflow_ns",
                   absl::ToInt64Nanoseconds(stats.min_last_overflow));
    entry.PrintI64("max_last_overflow_ns",
                   absl::ToInt64Nanoseconds(stats.max_last_overflow));
    entry.PrintI64("max_capacity_misses", stats.max_capacity_misses);
  }

  // Record dynamic slab statistics.
  region.PrintI64("dynamic_per_cpu_slab_size", 1 << freelist_.GetShift());
  for (int shift = 0; shift < kNumPossiblePerCpuShifts; ++shift) {
    PbtxtRegion entry = region.CreateSubRegion("dynamic_slab");
    entry.PrintI64("shift", shift + shift_bounds_.initial_shift);
    entry.PrintI64("grow_count", dynamic_slab_info_.grow_count[shift].load(
                                     std::memory_order_relaxed));
    entry.PrintI64("shrink_count", dynamic_slab_info_.shrink_count[shift].load(
                                       std::memory_order_relaxed));
  }
  region.PrintI64(
      "dynamic_slab_madvise_failed_bytes",
      dynamic_slab_info_.madvise_failed_bytes.load(std::memory_order_relaxed));
}

template <class Forwarder>
inline void CpuCache<Forwarder>::PerClassResizeInfo::Init() {
  state_.store(0, std::memory_order_relaxed);
}

template <class Forwarder>
inline bool CpuCache<Forwarder>::PerClassResizeInfo::Update(
    bool overflow, bool grow, uint32_t* successive) {
  int32_t raw = state_.load(std::memory_order_relaxed);
  State state;
  memcpy(&state, &raw, sizeof(state));
  const bool overflow_then_underflow = !overflow && state.overflow;
  grow |= overflow_then_underflow;
  // Reset quiescent ticks for Steal clock algorithm if we are going to grow.
  State new_state;
  new_state.overflow = overflow;
  new_state.quiescent_ticks = grow ? 0 : state.quiescent_ticks;
  new_state.successive = overflow == state.overflow ? state.successive + 1 : 0;
  memcpy(&raw, &new_state, sizeof(raw));
  state_.store(raw, std::memory_order_relaxed);
  *successive = new_state.successive;
  return overflow_then_underflow;
}

template <class Forwarder>
inline uint32_t CpuCache<Forwarder>::PerClassResizeInfo::Tick() {
  int32_t raw = state_.load(std::memory_order_relaxed);
  State state;
  memcpy(&state, &raw, sizeof(state));
  state.quiescent_ticks++;
  memcpy(&raw, &state, sizeof(raw));
  state_.store(raw, std::memory_order_relaxed);
  return state.quiescent_ticks - 1;
}

template <class Forwarder>
inline void CpuCache<Forwarder>::PerClassResizeInfo::RecordMiss(
    PerClassMissType type) {
  auto& c = misses_[type];
  c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

template <class Forwarder>
inline size_t CpuCache<Forwarder>::PerClassResizeInfo::GetTotalMisses(
    PerClassMissType type) {
  return misses_[type].load(std::memory_order_relaxed);
}

template <class Forwarder>
inline size_t
CpuCache<Forwarder>::PerClassResizeInfo::GetAndUpdateIntervalMisses(
    PerClassMissType total_type, PerClassMissType interval_type) {
  TC_ASSERT_LT(total_type, PerClassMissType::kNumTypes);
  TC_ASSERT_LT(interval_type, PerClassMissType::kNumTypes);

  const size_t total_misses =
      misses_[total_type].load(std::memory_order_relaxed);
  const size_t interval_misses =
      misses_[interval_type].load(std::memory_order_relaxed);
  misses_[interval_type].store(total_misses, std::memory_order_relaxed);
  // In case of a size_t overflow, we wrap around to 0.
  return total_misses > interval_misses ? total_misses - interval_misses : 0;
}

template <class Forwarder>
inline size_t CpuCache<Forwarder>::PerClassResizeInfo::GetIntervalMisses(
    PerClassMissType total_type, PerClassMissType interval_type) {
  TC_ASSERT_LT(total_type, PerClassMissType::kNumTypes);
  TC_ASSERT_LT(interval_type, PerClassMissType::kNumTypes);

  const size_t total_misses =
      misses_[total_type].load(std::memory_order_relaxed);
  const size_t interval_misses =
      misses_[interval_type].load(std::memory_order_relaxed);
  // In case of a size_t overflow, we wrap around to 0.
  return total_misses > interval_misses ? total_misses - interval_misses : 0;
}

template <class Forwarder>
void CpuCache<Forwarder>::PerClassResizeInfo::UpdateIntervalMisses(
    PerClassMissType total_type, PerClassMissType interval_type) {
  const size_t total_misses = GetTotalMisses(total_type);
  // Takes a snapshot of misses at the end of this interval so that we can
  // calculate the misses that occurred in the next interval.
  //
  // Interval updates occur on a single thread so relaxed stores to interval
  // miss stats are safe.
  misses_[interval_type].store(total_misses, std::memory_order_relaxed);
}

}  // namespace cpu_cache_internal

// Static forward declares CpuCache to avoid a cycle in headers.  Make
// "CpuCache" be non-templated to avoid breaking that forward declaration.
class CpuCache final
    : public cpu_cache_internal::CpuCache<cpu_cache_internal::StaticForwarder> {
};

template <typename State>
inline bool UsePerCpuCache(State& state) {
  // We expect a fast path of per-CPU caches being active and the thread being
  // registered with rseq.
  if (ABSL_PREDICT_FALSE(!state.CpuCacheActive())) {
    return false;
  }

  if (ABSL_PREDICT_TRUE(subtle::percpu::IsFastNoInit())) {
    return true;
  }

  // When rseq is not registered, use this transition edge to shutdown the
  // thread cache for this thread.
  //
  // We call IsFast() on every non-fastpath'd malloc or free since IsFast() has
  // the side-effect of initializing the per-thread state needed for "unsafe"
  // per-cpu operations in case this is the first time a new thread is calling
  // into tcmalloc.
  //
  // If the per-CPU cache for a thread is not initialized, we push ourselves
  // onto the slow path until this occurs.  See fast_alloc's use of
  // TryRecordAllocationFast.
  if (ABSL_PREDICT_TRUE(subtle::percpu::IsFast())) {
    ThreadCache::BecomeIdle();
    return true;
  }

  return false;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
#endif  // TCMALLOC_CPU_CACHE_H_
