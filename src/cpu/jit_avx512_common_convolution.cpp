/*******************************************************************************
* Copyright 2016-2017 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include "mkldnn_types.h"
#include "c_types_map.hpp"
#include "jit_avx512_common_convolution.hpp"
#include "mkldnn_thread.hpp"
#include "type_helpers.hpp"
#include "utils.hpp"

namespace mkldnn {
namespace impl {
namespace cpu {

using namespace mkldnn::impl::status;
using namespace mkldnn::impl::memory_format;
using namespace mkldnn::impl::utils;

template <bool with_relu>
void _jit_avx512_common_convolution_fwd_t<with_relu>::execute_forward()
{
    auto src = reinterpret_cast<const data_t *>(this->input_memory(0));
    auto weights = reinterpret_cast<const data_t *>(this->input_memory(1));
    auto bias = reinterpret_cast<const data_t *>(this->input_memory(2));
    auto dst = reinterpret_cast<data_t *>(this->memory());

    const memory_desc_wrapper src_d(conf_.src_pd());
    const memory_desc_wrapper dst_d(conf_.dst_pd());
    const memory_desc_wrapper weights_d(conf_.weights_pd(0));
    const memory_desc_wrapper bias_d(conf_.weights_pd(1));

    const auto &jcp = kernel_->jcp;

#   pragma omp parallel
    {
        const int ithr = omp_get_thread_num(), nthr = omp_get_num_threads();

        size_t start{0}, end{0};
        assert(jcp.nb_oc % jcp.nb_oc_blocking == 0);
        const int oc_chunks = jcp.nb_oc / jcp.nb_oc_blocking;
        const size_t work_amount = jcp.mb * jcp.ngroups * oc_chunks;
        size_t n{0}, g{0}, occ{0};
        jit_conv_call_s par_conv = {};

        balance211(work_amount, nthr, ithr, start, end);
        if (jcp.loop_order == loop_cgn)
            nd_iterator_init(start, occ, oc_chunks, g, jcp.ngroups, n, jcp.mb);
        else if (jcp.loop_order == loop_gnc)
            nd_iterator_init(start, g, jcp.ngroups, n, jcp.mb, occ, oc_chunks);
        else
            assert(!"unsupported loop order");

        par_conv.src_prf = NULL;
        par_conv.dst_prf = NULL;
        par_conv.filt_prf = NULL;
        par_conv.bias_prf = NULL;

        // TODO: check that assumptions make sense...
        const size_t dst_h_stride = dst_d.blk_off(0, 0, 1, 0);
        const size_t src_h_stride = src_d.blk_off(0, 0, 1, 0);
        const size_t src_c_stride = src_d.blk_off(0, 1, 0, 0);
        const size_t wht_h_stride = conf_.with_groups()
            ? weights_d.blk_off(0, 0, 0, 1, 0)
            : weights_d.blk_off(0, 0, 1, 0);
        const size_t wht_ic_stride = conf_.with_groups()
            ? weights_d.blk_off(0, 0, 1, 0, 0)
            : weights_d.blk_off(0, 1, 0, 0);

        for (size_t iwork = start; iwork < end; ++iwork) {
            const size_t ocb = occ * jcp.nb_oc_blocking;
            const size_t g_ocb = g * jcp.nb_oc + ocb;
            const size_t g_oc = g_ocb * jcp.oc_block;
            const size_t g_icb = g * jcp.nb_ic;
            const data_t *bias_ptr = bias ? bias + bias_d.blk_off(g_oc) : 0;
            const data_t *dst_ptr_base = dst + dst_d.blk_off(n, g_ocb, 0, 0);
            const data_t *src_ptr_base = src + src_d.blk_off(n, g_icb, -jcp.t_pad, 0);
            const data_t *weights_ptr_base = weights + (conf_.with_groups()
                    ? weights_d.blk_off(g, ocb, 0, 0, 0)
                    : weights_d.blk_off(ocb, 0, 0, 0));
            for (int icb = 0; icb < jcp.nb_ic; ++icb) {
                const data_t *src_ptr = src_ptr_base;
                const data_t *dst_ptr = dst_ptr_base;
                const data_t *weights_ptr = weights_ptr_base;
                for (int oh = 0, ij = -jcp.t_pad; oh < jcp.oh; ++oh, ij += jcp.stride_h) {
                    const int i_t_overflow = -nstl::min(0, ij);
                    const int i_b_overflow = nstl::max(jcp.ih, ij + jcp.kh) - jcp.ih;

                    par_conv.src = par_conv.src_prf;
                    par_conv.dst = par_conv.dst_prf;
                    par_conv.filt = par_conv.filt_prf;
                    par_conv.bias = par_conv.bias_prf;
                    par_conv.current_ic = par_conv.current_ic_prf;

                    par_conv.src_prf = src_ptr + i_t_overflow * src_h_stride;
                    par_conv.dst_prf = dst_ptr;
                    par_conv.bias_prf = bias_ptr;
                    par_conv.filt_prf = weights_ptr + i_t_overflow * wht_h_stride;

                    par_conv.kh_padding = par_conv.kh_padding_prf;
                    par_conv.kh_padding_prf
                        = jcp.kh - i_t_overflow - i_b_overflow;
                    par_conv.kw_padding = 0;
                    par_conv.current_ic_prf = icb;

                    if (par_conv.src != NULL)
                        kernel_->jit_ker(&par_conv);

                    src_ptr += src_h_stride * jcp.stride_h;
                    dst_ptr += dst_h_stride;
                }
                src_ptr_base += src_c_stride;
                weights_ptr_base += wht_ic_stride;
            }

            if (jcp.loop_order == loop_cgn)
                nd_iterator_step(occ, oc_chunks, g, jcp.ngroups, n, jcp.mb);
            else if (jcp.loop_order == loop_gnc)
                nd_iterator_step(g, jcp.ngroups, n, jcp.mb, occ, oc_chunks);
            else
                assert(!"unsupported loop order");
        }

        par_conv.src = par_conv.src_prf;
        par_conv.dst = par_conv.dst_prf;
        par_conv.filt = par_conv.filt_prf;
        par_conv.bias = par_conv.bias_prf;
        par_conv.current_ic = par_conv.current_ic_prf;

        par_conv.kh_padding = par_conv.kh_padding_prf;
        par_conv.kw_padding = 0;

        if (par_conv.src != NULL)
            kernel_->jit_ker(&par_conv);

    }
}
template void _jit_avx512_common_convolution_fwd_t<true>::execute_forward();
template void _jit_avx512_common_convolution_fwd_t<false>::execute_forward();

void jit_avx512_common_convolution_bwd_data_t::execute_backward_data() {
    auto diff_dst = reinterpret_cast<const data_t *>(this->input_memory(0));
    auto weights = reinterpret_cast<const data_t *>(this->input_memory(1));
    auto diff_src = reinterpret_cast<data_t*>(this->memory());

    const memory_desc_wrapper diff_dst_d(conf_.diff_dst_pd());
    const memory_desc_wrapper diff_src_d(conf_.diff_src_pd());
    const memory_desc_wrapper weights_d(conf_.weights_pd(0));

    const auto &jcp = kernel_->jcp;

#   pragma omp parallel
    {
        const int ithr = omp_get_thread_num(), nthr = omp_get_num_threads();

        size_t start{0}, end{0};
        size_t n{0}, g{0}, icc{0};
        jit_conv_call_s par_conv = {0};
        const int ic_chunks = jcp.nb_ic / jcp.nb_ic_blocking;
        const size_t work_amount = jcp.ngroups * jcp.mb * ic_chunks;

        balance211(work_amount, nthr, ithr, start, end);
        if (jcp.loop_order == loop_cgn)
            nd_iterator_init(start, icc, ic_chunks, g, jcp.ngroups, n, jcp.mb);
        else if (jcp.loop_order == loop_gnc)
            nd_iterator_init(start, g, jcp.ngroups, n, jcp.mb, icc, ic_chunks);
        else
            assert(!"unsupported loop order");

        par_conv.src_prf = NULL;
        par_conv.dst_prf = NULL;
        par_conv.filt_prf = NULL;
        for (size_t iwork = start; iwork < end; ++iwork) {
            const size_t ic = icc * jcp.nb_ic_blocking;
            for (int oc = 0; oc < jcp.nb_oc; ++oc) {
                for (int ih = 0; ih < jcp.ih; ++ih) {
                    size_t i_t_overflow = nstl::max(0, jcp.kh - 1 - ih
                            - jcp.t_pad);
                    size_t i_b_overflow = nstl::max(0, jcp.kh - 1
                            - (jcp.ih - 1 - ih) - jcp.b_pad);
                    size_t oh = ih + jcp.t_pad - i_b_overflow;

                    par_conv.src = par_conv.src_prf;
                    par_conv.dst = par_conv.dst_prf;
                    par_conv.filt = par_conv.filt_prf;
                    par_conv.current_ic = par_conv.current_ic_prf;

                    par_conv.src_prf = const_cast<data_t *>(&diff_src[
                            diff_src_d.blk_off(n, g * jcp.nb_ic + ic, ih, 0)]);
                    par_conv.dst_prf = const_cast<data_t *>(&diff_dst[
                            diff_dst_d.blk_off(n, g * jcp.nb_oc + oc, oh, 0)]);
                    par_conv.filt_prf = const_cast<data_t *>(&weights[
                            conf_.with_groups()
                            ? weights_d.blk_off(g, oc, ic, i_b_overflow, 0)
                            : weights_d.blk_off(oc, ic, i_b_overflow, 0)]);

                    par_conv.kh_padding = par_conv.kh_padding_prf;
                    par_conv.kh_padding_prf = jcp.kh - i_t_overflow
                        - i_b_overflow;
                    par_conv.kw_padding = 0;
                    par_conv.current_ic_prf = oc;

                    if (par_conv.src != NULL)
                        kernel_->jit_ker(&par_conv);
                }
            }

            if (jcp.loop_order == loop_cgn)
                nd_iterator_step(icc, ic_chunks, g, jcp.ngroups, n, jcp.mb);
            else if (jcp.loop_order == loop_gnc)
                nd_iterator_step(g, jcp.ngroups, n, jcp.mb, icc, ic_chunks);
            else
                assert(!"unsupported loop order");
        }
        par_conv.src = par_conv.src_prf;
        par_conv.dst = par_conv.dst_prf;
        par_conv.filt = par_conv.filt_prf;

        par_conv.src_prf = NULL;
        par_conv.dst_prf = NULL;
        par_conv.filt_prf = NULL;

        par_conv.kh_padding = par_conv.kh_padding_prf;
        par_conv.kw_padding = 0;
        par_conv.current_ic = par_conv.current_ic_prf;

        if (par_conv.src != NULL)
            kernel_->jit_ker(&par_conv);
    }
}

void jit_avx512_common_convolution_bwd_weights_t::execute_backward_weights() {
    auto src = reinterpret_cast<const data_t * > (this->input_memory(0));
    auto diff_dst = reinterpret_cast<const data_t * > (this->input_memory(1));
    auto diff_weights = reinterpret_cast<data_t *>(this->memory(0));
    auto diff_bias = reinterpret_cast<data_t *>(this->memory(1));
    auto tr_src = reinterpret_cast<data_t *>(this->ws_);

    const memory_desc_wrapper src_d(conf_.src_pd(0));
    const memory_desc_wrapper diff_dst_d(conf_.diff_dst_pd());
    const memory_desc_wrapper diff_weights_d(conf_.diff_weights_pd(0));

    const auto &jcp = kernel_->jcp;

    auto ker_transpose = [&](int ithr, int nthr) {
        const int trans_size = jcp.iw;
        const int spat_size = jcp.iw * jcp.ih;
        const int notrans_size = spat_size / trans_size;

        const size_t trans_work_amount
            = jcp.mb * jcp.ngroups * jcp.nb_ic * notrans_size;
        size_t start{0}, end{0};
        balance211(trans_work_amount, nthr, ithr, start, end);
        int img{0}, g{0}, ntd{0}, b_ic{0};
        nd_iterator_init(start, img, jcp.mb, g, jcp.ngroups, b_ic, jcp.nb_ic,
            ntd, notrans_size);

        const int _ic = g * jcp.nb_ic + b_ic;
        const data_t *src1 = &src[src_d.blk_off(img, _ic, ntd)];
        data_t *tr_src1 = &tr_src[src_d.blk_off(img, _ic, ntd)];

        for (size_t iwork = start; iwork < end; iwork++) {
            #pragma unroll
            for (int i = 0; i < trans_size; i++) {
                #pragma omp simd
                for (int j = 0; j < 16; j++)
                    tr_src1[j*trans_size + i] = src1[i*16 + j];
            }
            src1 += trans_size * jcp.ic_block;
            tr_src1 += trans_size * jcp.ic_block;
        }
        #pragma omp barrier
    };

    auto ker = [&](int ithr, int nthr) {
        auto rw = this->reducer_weights_;
        assert(nthr == rw->balancer_.nthr_);

        const int w_job_start = rw->balancer_.ithr_job_off(ithr);
        const int w_njobs = rw->balancer_.ithr_njobs(ithr);

        if (w_njobs == 0) return;

        /* reduction dimension */
        int img_start{0}, img_end{0};
        balance211(jcp.mb, rw->balancer_.nthr_per_group_,
            rw->balancer_.id_in_group(ithr), img_start, img_end);

        /* jobs */
        int g_start{0}, ocb_start{0}, icb_start{0};
        nd_iterator_init(w_job_start, g_start, jcp.ngroups, ocb_start,
            jcp.nb_oc, icb_start, jcp.nb_ic);

        for (int img = img_start; img < img_end; ++img) {
            int g = g_start, ocb = ocb_start, icb = icb_start;
            for (int w_job_loc = 0; w_job_loc < w_njobs; ++w_job_loc) {
                const size_t _oc = g * jcp.nb_oc + ocb;
                const size_t _ic = g * jcp.nb_ic + icb;

                jit_conv_call_s par_conv = { };
                par_conv.src = jcp.transpose_src
                    ? &tr_src[src_d.blk_off(img, _ic)]
                    : &src[src_d.blk_off(img, _ic)];
                par_conv.dst = &diff_dst[diff_dst_d.blk_off(img, _oc)];
                par_conv.filt = &rw->get_local_ptr(ithr, diff_weights)[
                    w_job_loc * rw->balancer_.job_size_];

                /* TODO: put dw <-- 0 in kernel */
                if (img == img_start) array_set((data_t *)par_conv.filt, 0,
                        rw->balancer_.job_size_);

                kernel_->jit_ker(&par_conv);

                nd_iterator_step(g, jcp.ngroups, ocb, jcp.nb_oc, icb,
                    jcp.nb_ic);
            }
        }
        rw->reduce(ithr, diff_weights);
    };

    auto ker_bias = [&](int ithr, int nthr) {
        auto rb = this->reducer_bias_;
        assert(nthr == rb->balancer_.nthr_);

        const int b_job_start = rb->balancer_.ithr_job_off(ithr);
        const int b_njobs = rb->balancer_.ithr_njobs(ithr);

        if (b_njobs == 0) return;

        /* reduction dimension */
        int img_start{0}, img_end{0};

        balance211(jcp.mb, rb->balancer_.nthr_per_group_,
            rb->balancer_.id_in_group(ithr), img_start, img_end);

        /* jobs */
        int g_start{0}, ocb_start{0};
        nd_iterator_init(b_job_start, g_start, jcp.ngroups, ocb_start,
            jcp.nb_oc);

        for (int img = img_start; img < img_end; ++img) {
            int g = g_start, ocb = ocb_start;
            for (int b_job_loc = 0; b_job_loc < b_njobs; ++b_job_loc) {
                const size_t _oc = g * jcp.nb_oc + ocb;

                const data_t *d_dst = &diff_dst[diff_dst_d.blk_off(img, _oc)];
                data_t *d_bias = &rb->get_local_ptr(ithr, diff_bias)[
                    b_job_loc * rb->balancer_.job_size_];

                if (img == img_start)
                    for (int o = 0; o < 16; ++o)
                        d_bias[o] = 0.;

                for (int hw = 0; hw < jcp.oh * jcp.ow; ++hw) {
                    for (int o = 0; o < 16; ++o)
                        d_bias[o] += d_dst[o];
                    d_dst += 16;
                }

                nd_iterator_step(g, jcp.ngroups, ocb, jcp.nb_oc);
            }
        }
        rb->reduce(ithr, diff_bias);
    };

#   pragma omp parallel
    {
        int ithr = omp_get_thread_num();
        int nthr = omp_get_num_threads();
        if (jcp.transpose_src)
            ker_transpose(ithr, nthr);
        ker(ithr, nthr);
        if (conf_.with_bias())
            ker_bias(ithr, nthr);
    }
}

}
}
}

// vim: et ts=4 sw=4 cindent cino^=l0,\:0,N-s
