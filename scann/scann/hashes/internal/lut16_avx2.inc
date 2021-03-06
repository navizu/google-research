#include "scann/hashes/internal/lut16_avx2.h"
#include "scann/utils/common.h"

#include "scann/oss_wrappers/scann_bits.h"

#ifdef __x86_64__

#include "scann/utils/bits.h"
#include "scann/utils/intrinsics/avx2.h"
#include "tensorflow/core/platform/prefetch.h"

namespace tensorflow {
namespace scann_ops {
namespace asymmetric_hashing_internal {
namespace {

template <typename T, size_t kSize>
class Avx2Array {
 public:
  SCANN_AVX2_INLINE Avx2Array() {
    for (size_t i : Seq(kSize)) {
      payload_[i] = T();
    }
  }

  SCANN_AVX2_INLINE T* begin() { return payload_; }
  SCANN_AVX2_INLINE T* end() { return begin() + kSize; }
  SCANN_AVX2_INLINE const T* begin() const { return payload_; }
  SCANN_AVX2_INLINE const T* end() const { return begin() + kSize; }
  SCANN_AVX2_INLINE const T& operator[](size_t i) const { return payload_[i]; }
  SCANN_AVX2_INLINE T& operator[](size_t i) { return payload_[i]; }
  SCANN_AVX2_INLINE size_t size() const { return kSize; }

 private:
  T payload_[kSize];
};

struct Accum32Int16s {
  M256_16Xint16 acc00 = _mm256_setzero_si256();
  M256_16Xint16 acc16 = _mm256_setzero_si256();

  SCANN_AVX2_INLINE Accum32Int16s() {}
};

struct Accum64Int16s {
  M256_16Xint16 acc00a = _mm256_setzero_si256();
  M256_16Xint16 acc00b = _mm256_setzero_si256();
  M256_16Xint16 acc16a = _mm256_setzero_si256();
  M256_16Xint16 acc16b = _mm256_setzero_si256();

  SCANN_AVX2_INLINE Accum64Int16s() {}
};

struct Accum32Int32s {
  M256_8Xint32 acc00 = _mm256_setzero_si256();
  M256_8Xint32 acc08 = _mm256_setzero_si256();
  M256_8Xint32 acc16 = _mm256_setzero_si256();
  M256_8Xint32 acc24 = _mm256_setzero_si256();

  SCANN_AVX2_INLINE Accum32Int32s() {}
};

SCANN_AVX2_INLINE M256_16Xint16 CombineAvxLanes(const M256_16Xint16& a,
                                                const M256_16Xint16& b) {
  constexpr uint8_t kDestLoEqALo = 0x00;
  constexpr uint8_t kDestLoEqAHi = 0x01;
  constexpr uint8_t kDestHiEqBLo = 0x20;
  constexpr uint8_t kDestHiEqBHi = 0x30;
  constexpr uint8_t t1spec = (kDestLoEqALo + kDestHiEqBHi);
  constexpr uint8_t t2spec = (kDestLoEqAHi + kDestHiEqBLo);
  M256_16Xint16 term0 = _mm256_permute2x128_si256(a.val(), b.val(), t1spec);
  M256_16Xint16 term1 = _mm256_permute2x128_si256(a.val(), b.val(), t2spec);
  return term0 + term1;
}

SCANN_AVX2_INLINE M256_16Xint16 PostprocessAccumulatorPair(
    const M256_16Xint16& even_plus_tag_along_bits, const M256_16Xint16& odd) {
  M256_16Xint16 even = even_plus_tag_along_bits - (odd << 8);

  M256_16Xint16 lo_per_lane = even.InterleaveBottomPerLane(odd);
  M256_16Xint16 hi_per_lane = even.InterleaveTopPerLane(odd);

  return CombineAvxLanes(lo_per_lane, hi_per_lane);
}

SCANN_AVX2_INLINE M256_16Xint16 ConvertToInt16s(const M128_16Xuint8& x) {
  return M256_16Xint16(_mm256_cvtepu8_epi16(x.val()));
}

template <size_t size, typename T>
SCANN_INLINE array<T, size> ToLocalArray(ConstSpan<T> span) {
  DCHECK_EQ(span.size(), size);
  array<T, size> result;
  std::copy(span.begin(), span.begin() + size, result.begin());
  return result;
}

SCANN_INLINE ssize_t ComputeSmartPrefetchIndex(size_t num_blocks,
                                               size_t num_32dp_simd_iters) {
  return std::min(0l, static_cast<ssize_t>(
                          DivRoundUp(kPrefetchBytesAhead / 16, num_blocks)) -
                          static_cast<ssize_t>(num_32dp_simd_iters));
}

template <size_t kNumQueries, PrefetchStrategy kPrefetch>
SCANN_AVX2_INLINE Avx2Array<Accum32Int16s, kNumQueries> Avx2LUT16BottomLoop(
    const uint8_t* data_start, array<const uint8_t*, kNumQueries> lookup_starts,
    const DimensionIndex num_blocks, const uint8_t* prefetch_start) {
  static_assert(kNumQueries <= 3,
                "Register spilling happens when kNumQueries > 3");
  Accum64Int16s int16_accumulators[kNumQueries];
  const auto sign7 = M256_32Xuint8::Broadcast(0x0F);

  DimensionIndex num_unroll_iter = num_blocks / 2;
  for (; num_unroll_iter != 0; --num_unroll_iter) {
    constexpr uint32_t kPointsPerIter = 32;
    if constexpr (kPrefetch == PrefetchStrategy::kSeq) {
      ::tensorflow::port::prefetch<::tensorflow::port::PREFETCH_HINT_T0>(
          data_start + kPrefetchBytesAhead);
    } else if constexpr (kPrefetch == PrefetchStrategy::kSmart) {
      ::tensorflow::port::prefetch<::tensorflow::port::PREFETCH_HINT_T0>(
          prefetch_start);
      prefetch_start += kPointsPerIter;
    }

    auto mask = M256_32Xuint8::Load(data_start);
    data_start += kPointsPerIter;

    M256_32Xuint8 mask0 = mask & sign7;

    M256_32Xuint8 mask1 =
        M256_32Xuint8((M256_16Xuint16(mask.val()) >> 4).val()) & sign7;

    for (size_t j : Seq(kNumQueries)) {
      const M256_32Xuint8 dict = M256_32Xuint8::Load(lookup_starts[j]);
      lookup_starts[j] += kPointsPerIter;
      const M256_32Xuint8 res0 = dict.Perform32LUT16Lookups(mask0);
      const M256_32Xuint8 res1 = dict.Perform32LUT16Lookups(mask1);

      int16_accumulators[j].acc00a += M256_16Xint16(res0.val());
      int16_accumulators[j].acc00b += res0.ExtractOddAs16Xint16();
      int16_accumulators[j].acc16a += M256_16Xint16(res1.val());
      int16_accumulators[j].acc16b += res1.ExtractOddAs16Xint16();
    }
  }

  Avx2Array<Accum32Int16s, kNumQueries> results;
  for (size_t j : Seq(kNumQueries)) {
    const auto& int16_accums = int16_accumulators[j];
    results[j].acc00 =
        PostprocessAccumulatorPair(int16_accums.acc00a, int16_accums.acc00b);
    results[j].acc16 =
        PostprocessAccumulatorPair(int16_accums.acc16a, int16_accums.acc16b);
  }

  const bool has_odd_block = num_blocks & 1;
  if (has_odd_block) {
    const M128_16Xuint8 sign7 = M128_16Xuint8::Broadcast(0x0F);
    const M128_16Xuint8 mask = M128_16Xuint8::Load(data_start);
    const M128_16Xuint8 mask0 = mask & sign7;
    const M128_16Xuint8 mask1 =
        M128_16Xuint8((M128_8Xuint16(mask.val()) >> 4).val()) & sign7;

    for (size_t j : Seq(kNumQueries)) {
      auto dict = M128_16Xuint8::Load(lookup_starts[j]);
      M128_16Xuint8 val0 = dict.Perform16LUT16Lookups(mask0);
      M128_16Xuint8 val1 = dict.Perform16LUT16Lookups(mask1);
      results[j].acc00 += ConvertToInt16s(val0);
      results[j].acc16 += ConvertToInt16s(val1);
    }
  }

  const auto total_bias =
      M256_16Xint16::Broadcast(static_cast<int16_t>(num_blocks * 128));
  for (auto& result : results) {
    result.acc00 -= total_bias;
    result.acc16 -= total_bias;
  }
  return results;
}

template <size_t kBottomLevelBatchSize, size_t kNumQueries>
SCANN_AVX2_INLINE array<const uint8_t*, kBottomLevelBatchSize>
MakeBottomLevelBatchLookupArray(
    array<const uint8_t*, kNumQueries> mid_level_lookups, size_t start) {
  DCHECK_LE(start + kBottomLevelBatchSize, kNumQueries);
  array<const uint8_t*, kBottomLevelBatchSize> result;
  for (size_t j : Seq(kBottomLevelBatchSize)) {
    result[j] = mid_level_lookups[start + j];
  }
  return result;
}

template <size_t kNumQueries, PrefetchStrategy kPrefetch>
SCANN_AVX2_INLINE Avx2Array<Accum32Int16s, kNumQueries> Avx2LUT16MiddleLoop(
    const uint8_t* data_start, array<const uint8_t*, kNumQueries> lookup_starts,
    const DimensionIndex num_blocks, const uint8_t* prefetch_start) {
  constexpr size_t kSizeB = (kNumQueries == 1) ? 1 : 2;
  constexpr size_t kNumBCases[] = {0, 2, 1};
  constexpr size_t kNumB = (kNumQueries == 1) ? 1 : kNumBCases[kNumQueries % 3];

  constexpr size_t kRemaining = kNumQueries - kNumB * kSizeB;
  static_assert(kRemaining % 3 == 0, "");

  constexpr size_t kSizeA = 3;
  constexpr size_t kNumA = kRemaining / 3;

  Avx2Array<Accum32Int16s, kNumQueries> result;
  for (size_t j : Seq(kNumA)) {
    const size_t start = j * kSizeA;
    auto bottom_level_lookups =
        MakeBottomLevelBatchLookupArray<kSizeA>(lookup_starts, start);
    auto acc = Avx2LUT16BottomLoop<kSizeA, kPrefetch>(
        data_start, bottom_level_lookups, num_blocks, prefetch_start);
    for (size_t jj : Seq(kSizeA)) {
      result[start + jj] = acc[jj];
    }
  }

  for (size_t j : Seq(kNumB)) {
    const size_t start = kNumA * kSizeA + j * kSizeB;
    auto bottom_level_lookups =
        MakeBottomLevelBatchLookupArray<kSizeB>(lookup_starts, start);
    auto acc = Avx2LUT16BottomLoop<kSizeB, kPrefetch>(
        data_start, bottom_level_lookups, num_blocks, prefetch_start);
    for (size_t jj : Seq(kSizeB)) {
      result[start + jj] = acc[jj];
    }
  }
  return result;
}

template <size_t kNumQueries, PrefetchStrategy kPrefetch>
SCANN_AVX2_INLINE Avx2Array<Accum32Int16s, kNumQueries> PrefetchDispatcher(
    const uint8_t* data_start, array<const uint8_t*, kNumQueries> lookups,
    const DimensionIndex num_blocks, const uint8_t* next_partition,
    ssize_t* __restrict__ next_prefetch_idx) {
  if constexpr (kPrefetch == PrefetchStrategy::kSmart) {
    if ((*next_prefetch_idx) >= 0) {
      const uint8_t* prefetch_start =
          next_partition + (*next_prefetch_idx) * 16 * num_blocks;
      (*next_prefetch_idx)++;
      return Avx2LUT16MiddleLoop<kNumQueries, kPrefetch>(
          data_start, lookups, num_blocks, prefetch_start);
    } else {
      (*next_prefetch_idx)++;
      return Avx2LUT16MiddleLoop<kNumQueries, PrefetchStrategy::kSeq>(
          data_start, lookups, num_blocks, nullptr);
    }
  } else {
    return Avx2LUT16MiddleLoop<kNumQueries, kPrefetch>(data_start, lookups,
                                                       num_blocks, nullptr);
  }
}

template <size_t kNumQueries, PrefetchStrategy kPrefetch>
SCANN_AVX2_INLINE Avx2Array<Accum32Int32s, kNumQueries>
Avx2LUT16MiddleLoopInt32(const uint8_t* data_start,
                         array<const uint8_t*, kNumQueries> lookup_starts,
                         const DimensionIndex num_blocks,
                         const uint8_t* next_partition,
                         ssize_t* __restrict__ next_prefetch_idx) {
  Avx2Array<Accum32Int32s, kNumQueries> int32_accumulators;
  for (DimensionIndex k = 0; k < num_blocks;) {
    DimensionIndex reaccumulate_limit = std::min(num_blocks - k, uint64_t{256});

    auto int16_accumulators = [&]() SCANN_AVX2_INLINE_LAMBDA {
      if constexpr (kPrefetch == PrefetchStrategy::kSmart) {
        if ((*next_prefetch_idx) >= 0) {
          const uint8_t* prefetch_start =
              next_partition + (*next_prefetch_idx) * 16 * num_blocks + k * 16;
          return Avx2LUT16MiddleLoop<kNumQueries, kPrefetch>(
              data_start, lookup_starts, reaccumulate_limit, prefetch_start);
        } else {
          return Avx2LUT16MiddleLoop<kNumQueries, PrefetchStrategy::kSeq>(
              data_start, lookup_starts, reaccumulate_limit, nullptr);
        }
      } else {
        return Avx2LUT16MiddleLoop<kNumQueries, kPrefetch>(
            data_start, lookup_starts, reaccumulate_limit, nullptr);
      }
    }();
    data_start += 16 * reaccumulate_limit;
    k += reaccumulate_limit;
    for (size_t j : Seq(kNumQueries)) {
      lookup_starts[j] += 16 * reaccumulate_limit;
      const auto& int16_accums = int16_accumulators[j];
      auto& int32_accums = int32_accumulators[j];
      int32_accums.acc00 += int16_accums.acc00.ExtractBotAs8Xint32();
      int32_accums.acc08 += int16_accums.acc00.ExtractTopAs8Xint32();
      int32_accums.acc16 += int16_accums.acc16.ExtractBotAs8Xint32();
      int32_accums.acc24 += int16_accums.acc16.ExtractTopAs8Xint32();
    }
  }
  if constexpr (kPrefetch == PrefetchStrategy::kSmart) (*next_prefetch_idx)++;
  return int32_accumulators;
}

}  // namespace

template <size_t kNumQueries, PrefetchStrategy kPrefetch>
SCANN_AVX2_OUTLINE void LUT16Avx2<kNumQueries, kPrefetch>::GetInt16Distances(
    LUT16Args<int16_t> args) {
  const uint8_t* packed_dataset = args.packed_dataset;
  const uint8_t* next_partition = args.next_partition;
  const size_t num_32dp_simd_iters = args.num_32dp_simd_iters;
  const size_t num_blocks = args.num_blocks;
  auto lookups = ToLocalArray<kNumQueries>(args.lookups);
  auto distances = ToLocalArray<kNumQueries>(args.distances);

  ssize_t next_prefetch_idx =
      ComputeSmartPrefetchIndex(num_blocks, num_32dp_simd_iters);
  for (DatapointIndex k : Seq(num_32dp_simd_iters)) {
    const uint8_t* data_start = packed_dataset + k * 16 * num_blocks;
    auto int16_accumulators = PrefetchDispatcher<kNumQueries, kPrefetch>(
        data_start, lookups, num_blocks, next_partition, &next_prefetch_idx);
    for (size_t j : Seq(kNumQueries)) {
      const auto& int16_accums = int16_accumulators[j];
      int16_accums.acc00.Store(distances[j] + 32 * k);
      int16_accums.acc16.Store(distances[j] + 32 * k + 16);
    }
  }
}

template <size_t kNumQueries, PrefetchStrategy kPrefetch>
SCANN_AVX2_OUTLINE void LUT16Avx2<kNumQueries, kPrefetch>::GetInt32Distances(
    LUT16Args<int32_t> args) {
  const uint8_t* packed_dataset = args.packed_dataset;
  const uint8_t* next_partition = args.next_partition;
  const size_t num_32dp_simd_iters = args.num_32dp_simd_iters;
  const size_t num_blocks = args.num_blocks;
  auto lookups = ToLocalArray<kNumQueries>(args.lookups);
  auto distances = ToLocalArray<kNumQueries>(args.distances);

  ssize_t next_prefetch_idx =
      ComputeSmartPrefetchIndex(num_blocks, num_32dp_simd_iters);
  for (DatapointIndex k : Seq(num_32dp_simd_iters)) {
    const uint8_t* data_start = packed_dataset + k * 16 * num_blocks;
    auto int32_accumulators = Avx2LUT16MiddleLoopInt32<kNumQueries, kPrefetch>(
        data_start, lookups, num_blocks, next_partition, &next_prefetch_idx);
    for (size_t j : Seq(kNumQueries)) {
      const auto& int32_accums = int32_accumulators[j];
      int32_accums.acc00.Store(distances[j] + 32 * k);
      int32_accums.acc08.Store(distances[j] + 32 * k + 8);
      int32_accums.acc16.Store(distances[j] + 32 * k + 16);
      int32_accums.acc24.Store(distances[j] + 32 * k + 24);
    }
  }
}

template <size_t kNumQueries, PrefetchStrategy kPrefetch>
SCANN_AVX2_OUTLINE void LUT16Avx2<kNumQueries, kPrefetch>::GetFloatDistances(
    LUT16Args<float> args, ConstSpan<float> inv_fp_multipliers) {
  const uint8_t* packed_dataset = args.packed_dataset;
  const uint8_t* next_partition = args.next_partition;
  const size_t num_32dp_simd_iters = args.num_32dp_simd_iters;
  const size_t num_blocks = args.num_blocks;
  auto lookups = ToLocalArray<kNumQueries>(args.lookups);
  auto distances = ToLocalArray<kNumQueries>(args.distances);
  auto mults = ToLocalArray<kNumQueries>(inv_fp_multipliers);

  ssize_t next_prefetch_idx =
      ComputeSmartPrefetchIndex(num_blocks, num_32dp_simd_iters);
  for (DatapointIndex k : Seq(num_32dp_simd_iters)) {
    const uint8_t* data_start = packed_dataset + k * 16 * num_blocks;
    auto int32_accumulators = Avx2LUT16MiddleLoopInt32<kNumQueries, kPrefetch>(
        data_start, lookups, num_blocks, next_partition, &next_prefetch_idx);
    for (size_t j : Seq(kNumQueries)) {
      const auto& int32_accums = int32_accumulators[j];
      float* d = distances[j];
      const M256_8Xfloat mult = M256_8Xfloat::Broadcast(mults[j]);
      (int32_accums.acc00.ConvertToFloat() * mult).Store(d + 32 * k + 0);
      (int32_accums.acc08.ConvertToFloat() * mult).Store(d + 32 * k + 8);
      (int32_accums.acc16.ConvertToFloat() * mult).Store(d + 32 * k + 16);
      (int32_accums.acc24.ConvertToFloat() * mult).Store(d + 32 * k + 24);
    }
  }
}

namespace {
template <size_t kNumQueries, PrefetchStrategy kPrefetch, typename TopN>
SCANN_AVX2_INLINE void GetTopInt16DistancesImpl(
    LUT16ArgsTopN<int16_t, TopN> args) {
  const uint8_t* packed_dataset = args.packed_dataset;
  const uint8_t* next_partition = args.next_partition;
  const size_t num_32dp_simd_iters = args.num_32dp_simd_iters;
  const size_t num_blocks = args.num_blocks;
  auto lookups = ToLocalArray<kNumQueries>(args.lookups);
  const DatapointIndex first_dp_index = args.first_dp_index;
  const uint32_t final_mask = GetFinalMask32(args.num_datapoints);
  DCHECK_EQ(num_32dp_simd_iters, DivRoundUp(args.num_datapoints, 32));

  M256_16Xint16 simd_thresholds[kNumQueries];
  for (size_t j : Seq(kNumQueries)) {
    const int16_t int16_threshold = args.fast_topns[j]->epsilon();
    simd_thresholds[j] = M256_16Xint16::Broadcast(int16_threshold);
  }

  typename TopN::Mutator topn_mutators[kNumQueries];
  for (size_t j : Seq(kNumQueries)) {
    args.fast_topns[j]->AcquireMutator(&topn_mutators[j]);
  }

  int16_t distances_buffer[32];
  auto restrict_whitelist_ptrs =
      args.template GetRestrictWhitelistPtrs<kNumQueries>();
  ssize_t next_prefetch_idx =
      ComputeSmartPrefetchIndex(num_blocks, num_32dp_simd_iters);
  for (DatapointIndex k : Seq(num_32dp_simd_iters)) {
    const uint8_t* data_start = packed_dataset + k * 16 * num_blocks;
    auto int16_accumulators = PrefetchDispatcher<kNumQueries, kPrefetch>(
        data_start, lookups, num_blocks, next_partition, &next_prefetch_idx);
    for (size_t j : Seq(kNumQueries)) {
      auto& int16_accums = int16_accumulators[j];

      auto compute_push_mask = [&]() SCANN_AVX2_INLINE_LAMBDA {
        auto cmp00 = (int16_accums.acc00 < simd_thresholds[j]);
        auto cmp16 = (int16_accums.acc16 < simd_thresholds[j]);
        return MaskFromHighBits(cmp00, cmp16);
      };
      uint32_t push_mask = compute_push_mask();

      if (!push_mask) continue;

      int16_accums.acc00.Store(distances_buffer);
      int16_accums.acc16.Store(distances_buffer + 16);

      if (k == num_32dp_simd_iters - 1) {
        push_mask &= final_mask;
      }
      if (restrict_whitelist_ptrs[j]) {
        push_mask &= restrict_whitelist_ptrs[j][k];
      }

      while (push_mask) {
        const int offset = bits::FindLSBSetNonZero(push_mask);
        push_mask &= (push_mask - 1);
        const DatapointIndex dp_idx = first_dp_index + 32 * k + offset;
        DCHECK(
            !restrict_whitelist_ptrs[j] ||
            args.restrict_whitelists[j].IsWhitelisted(dp_idx - first_dp_index))
            << dp_idx;
        const int16_t distance = distances_buffer[offset];
        const bool needs_collection = topn_mutators[j].Push(dp_idx, distance);
        if (ABSL_PREDICT_FALSE(needs_collection)) {
          topn_mutators[j].GarbageCollect();

          simd_thresholds[j] =
              M256_16Xint16::Broadcast(topn_mutators[j].epsilon());

          push_mask &= compute_push_mask();
        }
      }
    }
  }
}
}  // namespace

template <size_t kNumQueries, PrefetchStrategy kPrefetch>
SCANN_AVX2_OUTLINE void LUT16Avx2<kNumQueries, kPrefetch>::GetTopInt16Distances(
    LUT16ArgsTopN<int16_t> args) {
  return GetTopInt16DistancesImpl<kNumQueries, kPrefetch>(std::move(args));
}

SCANN_AVX2_INLINE int16_t GetInt16Threshold(float float_threshold) {
  constexpr float kMaxValue = numeric_limits<int16_t>::max();

  return std::min(float_threshold, kMaxValue);
}

namespace {
template <size_t kNumQueries, PrefetchStrategy kPrefetch, typename TopN>
SCANN_AVX2_INLINE void GetTopFloatDistancesImpl(
    LUT16ArgsTopN<float, TopN> args) {
  const uint8_t* packed_dataset = args.packed_dataset;
  const uint8_t* next_partition = args.next_partition;
  const size_t num_32dp_simd_iters = args.num_32dp_simd_iters;
  const size_t num_blocks = args.num_blocks;
  auto lookups = ToLocalArray<kNumQueries>(args.lookups);
  const DatapointIndex first_dp_index = args.first_dp_index;
  const uint32_t final_mask = GetFinalMask32(args.num_datapoints);
  DCHECK_EQ(num_32dp_simd_iters, DivRoundUp(args.num_datapoints, 32));

  auto biases = ToLocalArray<kNumQueries>(args.biases);
  M256_8Xfloat simd_biases[kNumQueries];
  for (size_t j : Seq(kNumQueries)) {
    simd_biases[j] = M256_8Xfloat::Broadcast(biases[j]);
  }

  auto mults = ToLocalArray<kNumQueries>(args.fixed_point_multipliers);
  M256_8Xfloat inv_mults[kNumQueries];
  for (size_t j : Seq(kNumQueries)) {
    inv_mults[j] = M256_8Xfloat::Broadcast(1.0 / mults[j]);
  }

  M256_16Xint16 simd_thresholds[kNumQueries];
  for (size_t j : Seq(kNumQueries)) {
    const float epsilon = args.fast_topns[j]->epsilon();
    const float float_threshold = (epsilon - biases[j]) * mults[j];
    const int16_t int16_threshold = GetInt16Threshold(float_threshold);
    simd_thresholds[j] = M256_16Xint16::Broadcast(int16_threshold);
  }

  typename TopN::Mutator topn_mutators[kNumQueries];
  for (size_t j : Seq(kNumQueries)) {
    args.fast_topns[j]->AcquireMutator(&topn_mutators[j]);
  }

  float distances_buffer[32];
  auto restrict_whitelist_ptrs =
      args.template GetRestrictWhitelistPtrs<kNumQueries>();
  ssize_t next_prefetch_idx =
      ComputeSmartPrefetchIndex(num_blocks, num_32dp_simd_iters);
  for (DatapointIndex k : Seq(num_32dp_simd_iters)) {
    const uint8_t* data_start = packed_dataset + k * 16 * num_blocks;

    auto int16_accumulators = PrefetchDispatcher<kNumQueries, kPrefetch>(
        data_start, lookups, num_blocks, next_partition, &next_prefetch_idx);
    for (size_t j : Seq(kNumQueries)) {
      const auto& int16_accums = int16_accumulators[j];

      auto compute_push_mask = [&]() SCANN_AVX2_INLINE_LAMBDA {
        auto cmp00 = (int16_accums.acc00 < simd_thresholds[j]);
        auto cmp16 = (int16_accums.acc16 < simd_thresholds[j]);
        return MaskFromHighBits(cmp00, cmp16);
      };
      uint32_t push_mask = compute_push_mask();

      if (!push_mask) continue;

      auto fvals00 = int16_accums.acc00.ExtractBotAs8Xint32().ConvertToFloat();
      auto fvals08 = int16_accums.acc00.ExtractTopAs8Xint32().ConvertToFloat();
      auto fvals16 = int16_accums.acc16.ExtractBotAs8Xint32().ConvertToFloat();
      auto fvals24 = int16_accums.acc16.ExtractTopAs8Xint32().ConvertToFloat();

      M256_8Xfloat imul = inv_mults[j];
      M256_8Xfloat sbias = simd_biases[j];
      (imul * fvals00 + sbias).Store(distances_buffer + 0);
      (imul * fvals08 + sbias).Store(distances_buffer + 8);
      (imul * fvals16 + sbias).Store(distances_buffer + 16);
      (imul * fvals24 + sbias).Store(distances_buffer + 24);

      if (k == num_32dp_simd_iters - 1) {
        push_mask &= final_mask;
      }
      if (restrict_whitelist_ptrs[j]) {
        push_mask &= restrict_whitelist_ptrs[j][k];
      }

      while (push_mask) {
        const int offset = bits::FindLSBSetNonZero(push_mask);
        push_mask &= (push_mask - 1);
        const DatapointIndex dp_idx = first_dp_index + 32 * k + offset;
        DCHECK(
            !restrict_whitelist_ptrs[j] ||
            args.restrict_whitelists[j].IsWhitelisted(dp_idx - first_dp_index))
            << dp_idx;
        if (args.final_predicate && !args.final_predicate(dp_idx)) continue;

        const bool needs_gc =
            topn_mutators[j].Push(dp_idx, distances_buffer[offset]);
        if (ABSL_PREDICT_FALSE(needs_gc)) {
          topn_mutators[j].GarbageCollect();

          const float new_epsilon = topn_mutators[j].epsilon();
          const float float_threshold = (new_epsilon - biases[j]) * mults[j];
          const int16_t int16_threshold = GetInt16Threshold(float_threshold);
          simd_thresholds[j] = M256_16Xint16::Broadcast(int16_threshold);

          push_mask &= compute_push_mask();
        }
      }
    }
  }
}
}  // namespace

template <size_t kNumQueries, PrefetchStrategy kPrefetch>
SCANN_AVX2_OUTLINE void LUT16Avx2<kNumQueries, kPrefetch>::GetTopFloatDistances(
    LUT16ArgsTopN<float> args) {
  return GetTopFloatDistancesImpl<kNumQueries, kPrefetch>(std::move(args));
}

}  // namespace asymmetric_hashing_internal
}  // namespace scann_ops
}  // namespace tensorflow

#endif
