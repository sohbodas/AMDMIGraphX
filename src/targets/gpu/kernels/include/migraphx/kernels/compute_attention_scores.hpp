/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2024 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifndef MIGRAPHX_GUARD_KERNELS_GROUP_QUERY_ATTENTION_HPP
#define MIGRAPHX_GUARD_KERNELS_GROUP_QUERY_ATTENTION_HPP

#include <migraphx/kernels/index.hpp>
#include <migraphx/kernels/algorithm.hpp>
#include <migraphx/kernels/integral_constant.hpp>
#include <migraphx/kernels/tensor_view.hpp>
#include <migraphx/kernels/ck.hpp>
#include <migraphx/kernels/gemm_batcher.hpp>
#include <limits>
#include <migraphx/kernels/type_traits.hpp>

namespace migraphx {

struct RotaryParameters
{
    float scale;
    int batch_size;           // Batch size used by input
    int sequence_length;      // Sequence length used by input
    int hidden_size;          // Hidden size used by input
    int head_size;            // Head size
    int rotary_embedding_dim; // Rotary embedding dimension.
    int num_heads;            // num_heads = hidden_size / head_size
    int max_sequence_length;  // Sequence length used by cos/sin cache
    int head_stride;          // Head stride
    int seq_stride;           // Sequence stride
    int batch_stride;         // Batch stride
    int position_ids_format;  // Format of position ids - 0 is (1), 1 is (batch_size,
                              // sequence_length)
    int transposed; // Whether the input tensor has been transposed into (batch, num_heads,
                     // seq_len, hidden)
    int seqlen_present_kv_cache;

    int do_rotary;
    int kv_num_heads;
    int local_window_size;
    int rotary_interleaved;

    __host__ __device__ void print() const {
        printf("scale: %f\n", scale);
        printf("batch_size: %d\n", batch_size);
        printf("sequence_length: %d\n", sequence_length);
        printf("hidden_size: %d\n", hidden_size);
        printf("head_size: %d\n", head_size);
        printf("rotary_embedding_dim: %d\n", rotary_embedding_dim);
        printf("num_heads: %d\n", num_heads);
        printf("max_sequence_length: %d\n", max_sequence_length);
        printf("head_stride: %d\n", head_stride);
        printf("seq_stride: %d\n", seq_stride);
        printf("batch_stride: %d\n", batch_stride);
        printf("position_ids_format: %d\n", position_ids_format);
        printf("transposed: %d\n", transposed);
        printf("seqlen_present_kv_cache: %d\n", seqlen_present_kv_cache);
        printf("do_rotary: %d\n", do_rotary);
        printf("kv_num_heads: %d\n", kv_num_heads);
        printf("local_window_size: %d\n", local_window_size);
        printf("rotary_interleaved: %d\n", rotary_interleaved);
    }
};

template<class S, class... Ts>
__device__ RotaryParameters make_rotary_params(S s, Ts... ts)
{
    return {static_cast<float>(s), ts...};
}

template <class T, class A, class B, class F>
__device__ void gemm(std::size_t M, std::size_t N, std::size_t K, std::size_t lda, std::size_t ldb, std::size_t ldc, T cmat, A amat, B bmat, F alpha, F beta, std::size_t idx, const bool b_transpose = false)
{
    auto m = idx / N;
    auto n = idx % N;
    auto a_idx = [&](auto ii, auto kk){ return kk + (ii * lda); };
    auto b_idx = [&](auto kk, auto jj){ return jj + (kk * ldb); };
    auto bt_idx = [&](auto kk, auto jj){ return jj + (kk * ldb); };
    auto c_idx = [&](auto ii, auto jj){ return jj + (ii * ldc); };

    if (m < M)
    {
        if (n < N)
        {
            double s = 0.0;
            for (int k = 0; k < K; ++k)
            {
                auto a_i = a_idx(m, k);
                auto b_i = b_transpose ? bt_idx(n, k) : b_idx(k, n);
                s += static_cast<double>(amat[a_i]) *
                     static_cast<double>(bmat[b_i]);
            }
            auto c_i = c_idx(m, n);
            cmat[c_i] = static_cast<double>(alpha) * s + cmat[c_i] * static_cast<double>(beta);
        }
    }
}

template <class Output, 
          class Attn_Probs,
          class SeqLens,
          class PresentValue,
          class Params>
__device__ void CalculateVxAttentionScore(
        Output output,                           // buffer for the result with size BxSxNxH
        const Attn_Probs attention_probs,            // Attention probs with size BxNxSxT
        const SeqLens seqlens_k,                  // past sequence lengths tensor
        int batch_size,                     // batch size
        int sequence_length,                // sequence length
        int present_buffer_sequence_length, // sequence length in past state
        int head_size,                      // head size of Q, K, V
        int hidden_size,                    // hidden size of Output
        PresentValue present_value,                    // present value only
        Params params,
        int idx)  
{
    const int num_heads = params.num_heads;
    const int kv_num_heads = params.kv_num_heads;
    const int kv_num_heads_factor   = num_heads / kv_num_heads;
    const size_t present_buff_chunk_length =
        static_cast<size_t>(present_buffer_sequence_length) * head_size; // T x H

    auto loop_len = batch_size * num_heads;
    auto i = idx / (sequence_length * head_size);
    auto inner_i = idx %  (sequence_length * head_size);
    if (i < loop_len)
    {
        const int batch_index = static_cast<int>(i / num_heads);
        const int head_index  = static_cast<int>(i % num_heads);
        const int total_seqlen         = seqlens_k[batch_index] + 1;
        
        auto pv = present_value + ((i / kv_num_heads_factor) * present_buff_chunk_length);
        Output output_current =
            output + (batch_index * sequence_length * num_heads + head_index) * head_size;
        ptrdiff_t attention_probs_offset = sequence_length * present_buffer_sequence_length * i;

        gemm(sequence_length,
            head_size,
            total_seqlen,
            present_buffer_sequence_length, // 4096
            head_size,
            hidden_size,
            output_current,
            attention_probs + attention_probs_offset,
            pv,
            1.0f,
            0.0f,
            inner_i);
    }
}

template <class Output,
          class Query,
          class Key,
          class Value,
          class Seqlens_K,
          class Attn_Probs,
          class Params>
__device__ void compute_attention_scores(Output output,
                                        Query query,
                                        Key,
                                        Value,
                                        Seqlens_K seqlens_k,
                                        Attn_Probs attn_probs,
                                        Params params)
{
    auto ind = make_index();
    ind.global_stride(query.get_shape().elements(), [&](auto idx) {
        const int batch_size      = params.batch_size;
        const int sequence_length = params.sequence_length;
        const int head_size       = params.head_size;

        int seqlen_present_kv_cache = params.seqlen_present_kv_cache;
        output([&](auto output0, auto, auto v_cache) {
            const int hidden_size     = params.hidden_size;
        
            CalculateVxAttentionScore(output0.begin(),
                                    attn_probs.begin(),
                                    seqlens_k.begin(),
                                    batch_size,
                                    sequence_length,
                                    seqlen_present_kv_cache,
                                    head_size,
                                    hidden_size,
                                    v_cache.begin(),
                                    params,
                                    idx);
        });
    });
}

} // namespace migraphx
#endif
