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

#include <migraphx/apply_alpha_beta.hpp>
#include "verify_program.hpp"
#include <migraphx/program.hpp>
#include <migraphx/generate.hpp>
#include <migraphx/make_op.hpp>

template <migraphx::shape::type_t DType>
struct test_gemm_2args_vv : verify_program<test_gemm_2args_vv<DType>>
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        auto* mm = p.get_main_module();
        migraphx::shape m1_shape{DType, {8}};
        migraphx::shape m2_shape{DType, {8}};
        auto l1     = mm->add_parameter("1", m1_shape);
        auto ul1    = mm->add_instruction(migraphx::make_op("unsqueeze", {{"axes", {0}}}), l1);
        auto l2     = mm->add_parameter("2", m2_shape);
        auto ul2    = mm->add_instruction(migraphx::make_op("unsqueeze", {{"axes", {1}}}), l2);
        float alpha = 0.23f;
        auto res = migraphx::add_apply_alpha_beta(*mm, {ul1, ul2}, migraphx::make_op("dot"), alpha);
        auto sres = mm->add_instruction(migraphx::make_op("squeeze", {{"axes", {0}}}), res);
        mm->add_instruction(migraphx::make_op("squeeze", {{"axes", {0}}}), sres);

        return p;
    }
    std::string section() const { return "gemm"; }
};

template struct test_gemm_2args_vv<migraphx::shape::float_type>;
template struct test_gemm_2args_vv<migraphx::shape::half_type>;
template struct test_gemm_2args_vv<migraphx::shape::fp8e4m3fnuz_type>;
template struct test_gemm_2args_vv<migraphx::shape::fp8e4m3fn_type>;
template struct test_gemm_2args_vv<migraphx::shape::fp8e5m2_type>;
