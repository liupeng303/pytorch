#define TORCH_ASSERT_ONLY_METHOD_OPERATORS
#include <ATen/core/Tensor.h>

#include <ATen/Dispatch.h>
#include <ATen/Parallel.h>
#include <ATen/cpu/vec/vec.h>
#include <ATen/cpu/vec/functional.h>
#include <ATen/native/CPUBlas.h>
#include <ATen/native/cpu/utils.h>
#include <ATen/native/transformers/attention.h>
#include <ATen/native/transformers/sdp_utils_cpp.h>
#include <c10/util/irange.h>

#ifndef AT_PER_OPERATOR_HEADERS
#include <ATen/Functions.h>
#else
#include <ATen/ops/empty.h>
#endif

namespace at::native {

namespace {

inline void _store(
    float* dst,
    vec::Vectorized<float> src) {
  src.store(dst);
}

inline void _store(
    at::BFloat16* dst,
    vec::Vectorized<float> src) {
  auto res = vec::convert_float_bfloat16(src, src);
  res.store(dst, vec::Vectorized<float>::size());
}

template <typename scalar_t>
inline void fill_stub(scalar_t* data, scalar_t val, int64_t size) {
  using Vec = Vectorized<scalar_t>;
  Vec data_vec = Vec(val);
  int64_t d = 0;
  for (; d < size - (size % Vec::size()); d += Vec::size()) {
    data_vec.store(data + d);
  }
  #if !defined(_MSC_VER) && !defined(COMPILING_FOR_MIN_SIZE)
  # pragma unroll
  #endif
  for (; d < size; d++) {
    data[d] = val;
  }
}

inline void _exp_reduce_sum_fusion_kernel(
    float* a,
    const int& size,
    float* out,
    float& val) {
  using fVec = vec::Vectorized<float>;
  vec::map<float>(
    [val](fVec x) { return (x - fVec(val)).exp(); }, out, a, size);
  val = vec::reduce_all<float>(
    [](fVec& x, fVec& y) { return x + y; }, out, size);
}

template <typename scalar_t>
inline void _normalization_kernel(
    const float* a,
    const float& sum,
    const int& size,
    scalar_t* out) {
  using fVec = vec::Vectorized<float>;
  auto vec_sum = fVec(sum);
  int64_t i = 0;
  for (i = 0; i < fVec::size() * (size / fVec::size()); i += fVec::size()) {
    auto tmp0 = fVec::loadu(a + i);
    auto tmp1 = tmp0 / vec_sum;
    _store(out + i, tmp1);
  }
  #if !defined(_MSC_VER) && !defined(COMPILING_FOR_MIN_SIZE)
  # pragma unroll
  #endif
  for (; i < size; i++) {
    auto tmp0 = a[i];
    auto tmp1 = tmp0 / sum;
    out[i] = tmp1;
  }
}

inline void _reduce_max_fusion_kernel(
    const int& size,
    float* out,
    float& max) {
  using fVec = vec::Vectorized<float>;
  max = vec::reduce_all<float>(
    [](fVec& x, fVec& y) { return vec::maximum(x, y); }, out, size);
}

/**
 * This kernel is used to reorder the MHA output
 * with strides.
 * src: MKL GEMM output buffer
 * dst: Final MHA output
 */
template <typename scalar_t>
inline void _reorder_mha_output_kernel(
    float* src,
    scalar_t* dst,
    const int& rows,
    const int& cols,
    const int& dst_stride) {
  using fVec = vec::Vectorized<float>;
  for (int64_t i = 0; i < rows; ++i) {
    int64_t j = 0;
    for (j = 0; j < fVec::size() * (cols / fVec::size()); j += fVec::size()) {
      auto tmp0 = fVec::loadu(src + i * cols + j);
      _store(dst + i * dst_stride + j, tmp0);
    }
    #if !defined(_MSC_VER) && !defined(COMPILING_FOR_MIN_SIZE)
    # pragma unroll
    #endif
    for (; j < cols; j++) {
      dst[i * dst_stride + j] = src[i * cols + j];
    }
  }
}

/**
 * This kernel is used to update the MHA output with the latest MAX
 * and SUM values block by block.
 * exp_val: exp(max_old - max_new)
 * In the i th block, the softmax(qk - i th) * v - i th was calculated
 * with the old MAX and SUM values, max_old and sum_old. When moving to
 * the i + 1 th block, since softmax(qk - i + 1 th) will be calculated
 * with the new MAX and SUM values, max_new and sum_new, thus the MHA
 * buffer which stores the summation of blocked softmax(qk) * v should
 * be also updated using max_new and sum_new:
 * a = a * sum_old / sum_new
 * a = a * exp(max_old) / exp(max_new) = a * exp_val
 */
inline void _mha_update_sum_max_kernel(
    const float* a,
    const float& sum_old,
    const float& sum_new,
    const float& exp_val,
    const int& size,
    float* out) {
  using fVec = vec::Vectorized<float>;
  float sum_cor = sum_old / sum_new;
  vec::map<float>(
    [sum_cor, exp_val](fVec x)
      { return x * fVec(sum_cor) * fVec(exp_val); },
      out, a, size);
}

template <typename scalar_t>
void _mha_softmax_kernel(
    float* a,
    scalar_t* b,
    float* dst,
    float* max,
    float* sum,
    const int& qsize,
    const int& kvsize,
    const int& headsize,
    const int& idx) {
  using accum_t = at::opmath_type<float>;
  accum_t tmp_max = 0.f, tmp_sum = 0.f, sum_old = 0.f, exp_tmp = 0.f;

  for (int i = 0; i < qsize; ++i) {
    sum_old = sum[i];

    _reduce_max_fusion_kernel(
        kvsize, a + i * kvsize, tmp_max);

    tmp_max = max[i] > tmp_max ? max[i] : tmp_max;

    tmp_sum = tmp_max;
    _exp_reduce_sum_fusion_kernel(
        a + i * kvsize, kvsize, a + i * kvsize, tmp_sum);
    exp_tmp = std::exp(max[i] - tmp_max);
    sum[i] = tmp_sum + exp_tmp * sum[i];
    max[i] = tmp_max;

    _normalization_kernel<scalar_t>(
        a + i * kvsize, sum[i], kvsize, b + i * kvsize);

    if (idx) {
      _mha_update_sum_max_kernel(
        dst + i * headsize,
        sum_old,
        sum[i],
        exp_tmp,
        headsize,
        dst + i * headsize);
    }
  }
}

template <typename scalar_t, int64_t qSplitSize, int64_t kvSplitSize>
void cpu_flash_attention(
    const Tensor& output,
    const Tensor& logsumexp,
    const Tensor& cum_seq_q,
    const Tensor& cum_seq_k,
    int64_t& max_q,
    int64_t& max_k,
    const Tensor& philox_seed,
    const Tensor& philox_offset,
    const Tensor& debug_attn_mask,
    const at::Tensor& q,
    const at::Tensor& k,
    const at::Tensor& v,
    double dropout_p,
    bool is_causal,
    bool return_debug_mask,
    c10::optional<double> scale) {

  bool is_training =
      (q.requires_grad() || k.requires_grad() ||
      v.requires_grad());

  // Query (Batch x Num_heads  x Q_seq_len  x Dim_per_head)
  //    -> (Batch x Q_seq_len  x Num_heads  x Dim_per_head)
  // Key   (Batch x Num_heads  x KV_seq_len x Dim_per_head)
  //    -> (Batch x KV_seq_len x Num_heads  x Dim_per_head)
  // Value (Batch x Num_heads  x KV_seq_len x Dim_per_head)
  //    -> (Batch x KV_seq_len x Num_heads  x Dim_per_head)
  at::Tensor query = q.transpose(1, 2);
  at::Tensor key = k.transpose(1, 2);
  at::Tensor value = v.transpose(1, 2);

  float scaling_factor =
      sdp::calculate_scale(query, scale).as_float_unchecked();

  // Sizes
  int64_t batchSize = query.size(0);
  int64_t qSize = query.size(1);
  int64_t kvSize = value.size(1);
  int64_t num_head = query.size(2);
  int64_t headSize = query.size(3);

  // Strides
  int64_t qStrideB = query.stride(0);
  int64_t qStrideM = query.stride(1);
  int64_t qStrideH = query.stride(2);
  int64_t kStrideB = key.stride(0);
  int64_t kStrideN = key.stride(1);
  int64_t kStrideH = key.stride(2);
  int64_t vStrideB = value.stride(0);
  int64_t vStrideN = value.stride(1);
  int64_t vStrideH = value.stride(2);
  int64_t oStrideB = output.stride(0);
  int64_t oStrideM = output.stride(1);
  int64_t oStrideH = output.stride(2);
  int64_t lStrideB = logsumexp.stride(0);
  int64_t lStrideM = logsumexp.stride(1);
  int64_t lStrideH = logsumexp.stride(2);

  int64_t qSlice = (qSize - 1) / qSplitSize + 1;
  int64_t num_thread = at::get_num_threads();

  at::Tensor qk = at::empty({num_thread, qSplitSize, kvSplitSize}, at::kFloat);
  at::Tensor qk_norm = at::empty({num_thread, qSplitSize, kvSplitSize}, query.options());
  at::Tensor qk_max = at::empty({num_thread, qSplitSize}, at::kFloat);
  at::Tensor qk_sum = at::empty({num_thread, qSplitSize}, at::kFloat);
  at::Tensor dst = at::empty({num_thread, qSplitSize, headSize}, at::kFloat);

  // Data ptrs
  scalar_t* q_data = query.data_ptr<scalar_t>();
  scalar_t* k_data = key.data_ptr<scalar_t>();
  scalar_t* v_data = value.data_ptr<scalar_t>();
  scalar_t* out_data = output.data_ptr<scalar_t>();
  float* lse_data = is_training ? logsumexp.data_ptr<float>() : nullptr;
  float* qk_data = qk.data_ptr<float>();
  scalar_t* qk_norm_data = qk_norm.data_ptr<scalar_t>();
  float* qk_max_data = qk_max.data_ptr<float>();
  float* qk_sum_data = qk_sum.data_ptr<float>();
  float* dst_data = dst.data_ptr<float>();

  at::parallel_for(0, batchSize * num_head * qSlice, 1, [&](int64_t begin, int64_t end) {
    int64_t i = 0, j = 0, k = 0;
    data_index_init(begin, i, batchSize, j, num_head, k, qSlice);
    int ompIdx = at::get_thread_num();
    for (const auto x : c10::irange(begin, end)) {
      (void)x; // Suppress unused variable
      int64_t m = k * qSplitSize;
      int64_t qBlockSize = std::min(qSplitSize, qSize - m);
      // Initialize max and sum
      fill_stub(qk_max_data + ompIdx * qSplitSize,
          -std::numeric_limits<float>::infinity(), qBlockSize);
      fill_stub(qk_sum_data + ompIdx * qSplitSize,
          0.f, qBlockSize);
      int64_t num_keys = is_causal ? std::min(m + qBlockSize, kvSize) : kvSize;
      for (int64_t n = 0; n < num_keys; n += kvSplitSize) {
        int64_t kvBlockSize = std::min(kvSplitSize, kvSize - n);
        // Calculate scale * q @ k.T
        cpublas::gemm(
            TransposeType::Transpose,
            TransposeType::NoTranspose,
            kvBlockSize,
            qBlockSize,
            headSize,
            scaling_factor,
            k_data + i * kStrideB + j * kStrideH +
                n * kStrideN,
            kStrideN,
            q_data + i * qStrideB + j * qStrideH +
                m * qStrideM,
            qStrideM,
            0.f,
            qk_data + ompIdx * qSplitSize * kvSplitSize,
            kvBlockSize);
        // Apply causal mask, fill unused with -inf
        if (is_causal && num_keys - n <= kvSplitSize) {
          for (const auto row : c10::irange(qBlockSize)) {
            int64_t last_col = m + row - n;
            float* row_ptr = qk_data + ompIdx * qSplitSize * kvSplitSize + row * kvBlockSize;
            fill_stub(row_ptr + last_col + 1,
                -std::numeric_limits<float>::infinity(),
                kvBlockSize - last_col - 1);
          }
        }
        // Update coefficients with Softmax
        _mha_softmax_kernel<scalar_t>(
            qk_data + ompIdx * qSplitSize * kvSplitSize,
            qk_norm_data + ompIdx * qSplitSize * kvSplitSize,
            dst_data + ompIdx * qSplitSize * headSize,
            qk_max_data + ompIdx * qSplitSize,
            qk_sum_data + ompIdx * qSplitSize,
            qBlockSize,
            kvBlockSize,
            headSize,
            n);
        // Calculate Softmax(q @ k.T) @ v
        cpublas::gemm(
            TransposeType::NoTranspose,
            TransposeType::NoTranspose,
            headSize,
            qBlockSize,
            kvBlockSize,
            1.f,
            v_data + i * vStrideB + j * vStrideH +
                n * vStrideN,
            vStrideN,
            qk_norm_data + ompIdx * qSplitSize * kvSplitSize,
            kvBlockSize,
            n == 0 ? 0.f : 1.f,
            dst_data + ompIdx * qSplitSize * headSize,
            headSize);
      }
      _reorder_mha_output_kernel<scalar_t>(
          dst_data + ompIdx * qSplitSize * headSize,
          out_data + i * oStrideB + j * oStrideH +
              m * oStrideM,
          qBlockSize,
          headSize,
          oStrideM);
      // Store logsumexp for backward
      if (is_training) {
        float* lse_ptr = lse_data + i * lStrideB + j * lStrideH + m * lStrideM;
        for (const auto row : c10::irange(qBlockSize)) {
          lse_ptr[row * lStrideM] = qk_max_data[ompIdx * qSplitSize + row]
              + std::log(qk_sum_data[ompIdx * qSplitSize + row]);
        }
      }
      // Move to the next query
      data_index_step(i, batchSize, j, num_head, k, qSlice);
    }
  });

}

template <typename scalar_t, int64_t qSplitSize, int64_t kvSplitSize>
void cpu_flash_attention_backward(
    const at::Tensor& grad_q,
    const at::Tensor& grad_k,
    const at::Tensor& grad_v,
    const at::Tensor& grad_out,
    const at::Tensor& query,
    const at::Tensor& key,
    const at::Tensor& value,
    const at::Tensor& out,
    const at::Tensor& logsumexp,
    const Tensor& cumulative_sequence_length_q,
    const Tensor& cumulative_sequence_length_k,
    const int64_t max_seqlen_batch_q,
    const int64_t max_seqlen_batch_k,
    double dropout_p,
    bool is_causal,
    const at::Tensor& philox_seed,
    const at::Tensor& philox_offset,
    c10::optional<double> scale) {

  using Vec = vec::Vectorized<scalar_t>;
  float scaling_factor =
      sdp::calculate_scale(query, scale).as_float_unchecked();

  // Sizes
  // Query (Batch x Q_seq_len  x Num_heads x Dim_per_head)
  // Key   (Batch x KV_seq_len x Num_heads x Dim_per_head)
  // Value (Batch x KV_seq_len x Num_heads x Dim_per_head)
  int64_t batchSize = query.size(0);
  int64_t qSize = query.size(1);
  int64_t kvSize = value.size(1);
  int64_t num_head = query.size(2);
  int64_t headSize = query.size(3);

  // Strides
  int64_t qStrideB = query.stride(0);
  int64_t qStrideM = query.stride(1);
  int64_t qStrideH = query.stride(2);
  int64_t kStrideB = key.stride(0);
  int64_t kStrideN = key.stride(1);
  int64_t kStrideH = key.stride(2);
  int64_t vStrideB = value.stride(0);
  int64_t vStrideN = value.stride(1);
  int64_t vStrideH = value.stride(2);
  int64_t oStrideB = out.stride(0);
  int64_t oStrideM = out.stride(1);
  int64_t oStrideH = out.stride(2);
  int64_t lStrideB = logsumexp.stride(0);
  int64_t lStrideM = logsumexp.stride(1);
  int64_t lStrideH = logsumexp.stride(2);

  int64_t grad_qStrideB = grad_q.stride(0);
  int64_t grad_qStrideM = grad_q.stride(1);
  int64_t grad_qStrideH = grad_q.stride(2);
  int64_t grad_kStrideB = grad_k.stride(0);
  int64_t grad_kStrideN = grad_k.stride(1);
  int64_t grad_kStrideH = grad_k.stride(2);
  int64_t grad_vStrideB = grad_v.stride(0);
  int64_t grad_vStrideN = grad_v.stride(1);
  int64_t grad_vStrideH = grad_v.stride(2);
  int64_t grad_oStrideB = grad_out.stride(0);
  int64_t grad_oStrideM = grad_out.stride(1);
  int64_t grad_oStrideH = grad_out.stride(2);

  int64_t num_thread = at::get_num_threads();

  at::Tensor attn = at::empty({num_thread, qSplitSize, kvSplitSize}, at::kFloat);
  at::Tensor grad_attn = at::empty({num_thread, qSplitSize, kvSplitSize}, at::kFloat);

  scalar_t* grad_q_data = grad_q.data_ptr<scalar_t>();
  scalar_t* grad_k_data = grad_k.data_ptr<scalar_t>();
  scalar_t* grad_v_data = grad_v.data_ptr<scalar_t>();
  scalar_t* grad_out_data = grad_out.data_ptr<scalar_t>();
  scalar_t* q_data = query.data_ptr<scalar_t>();
  scalar_t* k_data = key.data_ptr<scalar_t>();
  scalar_t* v_data = value.data_ptr<scalar_t>();
  scalar_t* out_data = out.data_ptr<scalar_t>();
  scalar_t* lse_data = logsumexp.data_ptr<scalar_t>();
  scalar_t* attn_data = attn.data_ptr<scalar_t>();
  scalar_t* grad_attn_data = grad_attn.data_ptr<scalar_t>();

  at::parallel_for(0, batchSize * num_head, 1, [&](int64_t begin, int64_t end) {
    int64_t i = 0, j = 0;
    data_index_init(begin, i, batchSize, j, num_head);
    int ompIdx = at::get_thread_num();
    scalar_t dsum[qSplitSize];
    for (const auto x : c10::irange(begin, end)) {
      (void)x; // Suppress unused variable
      // rowsum of grad_out * out
      for (int64_t m = 0; m < qSize; m += qSplitSize) {
        int64_t qBlockSize = std::min(qSplitSize, qSize - m);
        // dsum <- rowsum(grad_out * out)
        for (const auto row : c10::irange(qBlockSize)) {
          dsum[row] = vec::map2_reduce_all<scalar_t>(
            [](Vec x, Vec y) { return x * y; },
            [](Vec x, Vec y) { return x + y; },
            grad_out_data + i * grad_oStrideB + j * grad_oStrideH + (m + row) * grad_oStrideM,
            out_data + i * oStrideB + j * oStrideH + (m + row) * oStrideM,
            headSize);
        }
        int64_t num_keys = is_causal ? std::min(m + qBlockSize, kvSize) : kvSize;
        for (int64_t n = 0; n < num_keys; n += kvSplitSize) {
          int64_t kvBlockSize = std::min(kvSplitSize, kvSize - n);
          // attn <- scale * q @ k.T
          cpublas::gemm(
            TransposeType::Transpose,
            TransposeType::NoTranspose,
            kvBlockSize,
            qBlockSize,
            headSize,
            scaling_factor,
            k_data + i * kStrideB + j * kStrideH +
                n * kStrideN,
            kStrideN,
            q_data + i * qStrideB + j * qStrideH +
                m * qStrideM,
            qStrideM,
            0.f,
            attn_data + ompIdx * qSplitSize * kvSplitSize,
            kvBlockSize);
          // restore self attention after softmax from logsumexp
          // attn <- exp(attn - normalizer)
          for (const auto row : c10::irange(qBlockSize)) {
            scalar_t normalizer = lse_data[i * lStrideB + j * lStrideH + (m + row) * lStrideM];
            vec::map<scalar_t>(
              [normalizer](Vec x) { return (x - Vec(normalizer)).exp(); },
              attn_data + ompIdx * qSplitSize * kvSplitSize + row * kvBlockSize,
              attn_data + ompIdx * qSplitSize * kvSplitSize + row * kvBlockSize,
              kvBlockSize);
          }
          // Apply causal mask, filled unused with 0
          if (is_causal && num_keys - n <= kvSplitSize) {
            for (const auto row : c10::irange(qBlockSize)) {
              int64_t last_col = m + row - n;
              float* row_ptr = attn_data + ompIdx * qSplitSize * kvSplitSize + row * kvBlockSize;
              fill_stub(row_ptr + last_col + 1, 0.f, kvBlockSize - last_col - 1);
            }
          }
          // grad_v <- grad_v + attn.T @ grad_out
          cpublas::gemm(
            TransposeType::NoTranspose,
            TransposeType::Transpose,
            headSize,
            kvBlockSize,
            qBlockSize,
            1.f,
            grad_out_data + i * grad_oStrideB + j * grad_oStrideH +
                m * grad_oStrideM,
            grad_oStrideM,
            attn_data + ompIdx * qSplitSize * kvSplitSize,
            kvBlockSize,
            1.f,
            grad_v_data + i * grad_vStrideB + j * grad_vStrideH +
                n * grad_vStrideN,
            grad_vStrideN);
          // grad_attn <- grad_out @ v.T
          cpublas::gemm(
            TransposeType::Transpose,
            TransposeType::NoTranspose,
            kvBlockSize,
            qBlockSize,
            headSize,
            1.f,
            v_data + i * vStrideB + j * vStrideH +
                n * vStrideN,
            vStrideN,
            grad_out_data + i * grad_oStrideB + j * grad_oStrideH +
                m * grad_oStrideM,
            grad_oStrideM,
            0.f,
            grad_attn_data + ompIdx * qSplitSize * kvSplitSize,
            kvBlockSize);
          // grad_attn <- attn * (grad_attn - dsum)
          for (const auto row : c10::irange(qBlockSize)) {
            scalar_t d = dsum[row];
            vec::map2<scalar_t>(
              [d](Vec attn, Vec grad_attn) { return attn * (grad_attn - Vec(d)); },
              grad_attn_data + ompIdx * qSplitSize * kvSplitSize + row * kvBlockSize,
              attn_data + ompIdx * qSplitSize * kvSplitSize + row * kvBlockSize,
              grad_attn_data + ompIdx * qSplitSize * kvSplitSize + row * kvBlockSize,
              kvBlockSize);
          }
          // grad_q <- grad_q + scale * grad_attn @ k
          cpublas::gemm(
            TransposeType::NoTranspose,
            TransposeType::NoTranspose,
            headSize,
            qBlockSize,
            kvBlockSize,
            scaling_factor,
            k_data + i * kStrideB + j * kStrideH +
                n * kStrideN,
            kStrideN,
            grad_attn_data + ompIdx * qSplitSize * kvSplitSize,
            kvBlockSize,
            1.f,
            grad_q_data + i * grad_qStrideB + j * grad_qStrideH +
                m * grad_qStrideM,
            grad_qStrideM);
          // grad_k <- grad_k + scale * grad_attn.T @ q
          cpublas::gemm(
            TransposeType::NoTranspose,
            TransposeType::Transpose,
            headSize,
            kvBlockSize,
            qBlockSize,
            scaling_factor,
            q_data + i * qStrideB + j * qStrideH +
                m * qStrideM,
            qStrideM,
            grad_attn_data + ompIdx * qSplitSize * kvSplitSize,
            kvBlockSize,
            1.f,
            grad_k_data + i * grad_kStrideB + j * grad_kStrideH +
                n * grad_kStrideN,
            grad_kStrideN);
        }
      }
      // Move to the next query
      data_index_step(i, batchSize, j, num_head);
    }
  });
}

void flash_attention_kernel_impl(
    const Tensor& output,
    const Tensor& logsumexp,
    const Tensor& cum_seq_q,
    const Tensor& cum_seq_k,
    int64_t& max_q,
    int64_t& max_k,
    const Tensor& philox_seed,
    const Tensor& philox_offset,
    const Tensor& debug_attn_mask,
    const at::Tensor& query,
    const at::Tensor& key,
    const at::Tensor& value,
    double dropout_p,
    bool is_causal,
    bool return_debug_mask,
    c10::optional<double> scale) {
  AT_DISPATCH_SWITCH(query.scalar_type(), "flash_attention",
    AT_DISPATCH_CASE(ScalarType::Float, [&] {
      cpu_flash_attention<scalar_t, 128, 256>(
          output, logsumexp, cum_seq_q, cum_seq_k,
          max_q, max_k, philox_seed, philox_offset, debug_attn_mask,
          query, key, value, dropout_p, is_causal, return_debug_mask, scale);
    });
    AT_DISPATCH_CASE(ScalarType::BFloat16, [&] {
      cpu_flash_attention<scalar_t, 128, 256>(
          output, logsumexp, cum_seq_q, cum_seq_k,
          max_q, max_k, philox_seed, philox_offset, debug_attn_mask,
          query, key, value, dropout_p, is_causal, return_debug_mask, scale);
    });
  );
}

void flash_attention_backward_kernel_impl(
    const at::Tensor& grad_q,
    const at::Tensor& grad_k,
    const at::Tensor& grad_v,
    const at::Tensor& grad_out,
    const at::Tensor& query,
    const at::Tensor& key,
    const at::Tensor& value,
    const at::Tensor& out,
    const at::Tensor& logsumexp,
    const Tensor& cum_seq_q,
    const Tensor& cum_seq_k,
    const int64_t max_q,
    const int64_t max_k,
    double dropout_p,
    bool is_causal,
    const at::Tensor& philox_seed,
    const at::Tensor& philox_offset,
    c10::optional<double> scale) {
  // make sure grad_out has no zero strides (broadcasted dimensions)
  // since we are going to call gemm next
  // zero stride in leading dimension would lead to slow impl for gemm
  auto grad_out_contig = grad_out.contiguous();

  AT_DISPATCH_SWITCH(query.scalar_type(), "flash_attention_backward",
    AT_DISPATCH_CASE(ScalarType::Float, [&] {
      cpu_flash_attention_backward<scalar_t, 128, 256>(
          grad_q, grad_k, grad_v, grad_out_contig,
          query, key, value, out, logsumexp,
          cum_seq_q, cum_seq_k, max_q, max_k, dropout_p,
          is_causal, philox_seed, philox_offset, scale);
    });
    // AT_DISPATCH_CASE(ScalarType::BFloat16, [&] {
    //   cpu_flash_attention_backward<scalar_t, 128, 256>(
    //       grad_q, grad_k, grad_v, grad_out_contig,
    //       query, key, value, out, logsumexp,
    //       cum_seq_q, cum_seq_k, max_q, max_k, dropout_p,
    //       is_causal, philox_seed, philox_offset, scale);
    // });
  );
}

} // anonymous namespace

REGISTER_DISPATCH(flash_attention_kernel, &flash_attention_kernel_impl);
REGISTER_DISPATCH(flash_attention_backward_kernel, &flash_attention_backward_kernel_impl);

} // at::native