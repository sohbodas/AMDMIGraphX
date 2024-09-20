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

#include "verify_program.hpp"
#include <migraphx/float8.hpp>
#include <migraphx/half.hpp>
#include <migraphx/program.hpp>
#include <migraphx/generate.hpp>
#include <migraphx/make_op.hpp>

template <typename CType>
struct test_atanh : verify_program<test_atanh<CType>>
{
    migraphx::program create_program() const
    {
        migraphx::program p;
        auto* mm                      = p.get_main_module();
        migraphx::shape::type_t dtype = migraphx::shape::get_type<CType>();
        migraphx::shape s{dtype, {16}};
        auto x       = mm->add_parameter("x", s);
        auto min_val = mm->add_literal(migraphx::literal{migraphx::shape{dtype}, {-0.95f}});
        auto max_val = mm->add_literal(migraphx::literal{migraphx::shape{dtype}, {0.95f}});
        min_val =
            mm->add_instruction(migraphx::make_op("multibroadcast", {{"out_lens", {16}}}), min_val);
        max_val =
            mm->add_instruction(migraphx::make_op("multibroadcast", {{"out_lens", {16}}}), max_val);
        auto cx = mm->add_instruction(migraphx::make_op("clip"), x, min_val, max_val);
        mm->add_instruction(migraphx::make_op("atanh"), cx);
        return p;
    }
};

template struct test_atanh<float>;
template struct test_atanh<migraphx::half>;
template struct test_atanh<migraphx::fp8::fp8e4m3fnuz>;
template struct test_atanh<migraphx::fp8::fp8e4m3fn>;
template struct test_atanh<migraphx::fp8::fp8e5m2>;
