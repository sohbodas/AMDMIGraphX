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
#include <iterator>
#include <migraphx/eliminate_concat.hpp>
#include <migraphx/program.hpp>
#include <migraphx/instruction.hpp>
#include <migraphx/op/load.hpp>
#include <migraphx/op/identity.hpp>
#include <migraphx/iterator_for.hpp>
#include <migraphx/ranges.hpp>
#include <migraphx/make_op.hpp>

#include <migraphx/dfor.hpp>
#include <migraphx/tune_axis.hpp>

namespace migraphx {
inline namespace MIGRAPHX_INLINE_NS {

namespace {


struct concat_optimizer
{
    module* m = nullptr;
    allocation_model am;

    static operation make_slice(instruction_ref input, std::size_t axis, std::size_t start)
    {
        return make_op("slice", {{"axes", {axis}}, {"starts", {start}}, {"ends", {input->get_shape().lens()[axis] + start}}});
    }

    bool is_allocation(instruction_ref ins) const
    {
        return ins->name() == "allocate" or ins->name() == am.name();
    }
    
    bool need_copy(instruction_ref ins) const
    {
        auto alloc = instruction::get_output_alias(ins, true);
        return not is_allocation(alloc);
    }
    
    instruction_ref insert_copy(const operation& op, instruction_ref input, instruction_ref super)
    {
        auto slice = m->insert_instruction(input, op, super);
        // If its packed then replace the allocation with the slice instead
        if(not need_copy(input) and slice->get_shape().packed() and input->outputs().size() == 1)
        {
            auto alloc = instruction::get_output_alias(input, true);
            m->replace_instruction(alloc, slice);
            return input;
        }
        auto copy = m->insert_instruction(std::next(input), make_op(am.copy()), input, slice);
        m->replace_instruction(input, copy);
        return copy;
    }

    void replace_concat(instruction_ref ins, std::size_t axis)
    {
        // Last input should be an allocation
        auto last = ins->inputs().back();
        if(not is_allocation(last))
            return;
        // Where are the allocations for the tensors to be concatenated?
        std::vector<instruction_ref> allocations;

        std::transform(
            ins->inputs().begin(),
            std::prev(ins->inputs().end()),
            std::back_inserter(allocations),
            [&](instruction_ref x) { return instruction::get_output_alias(x, true); });

        // if(std::any_of(allocations.begin(), allocations.end(), [&](auto x) {
        //        return x->name() != am.name();
        //    }))
        //     return;

        // Need to sort the allocations, so that we know where to
        // insert the "super"-allocation
        auto sorted_allocations = allocations;
        std::sort(sorted_allocations.begin(),
                  sorted_allocations.end(),
                  [&](instruction_ref x, instruction_ref y) {
                      return std::distance(m->begin(), x) < std::distance(m->begin(), y);
                  });
        // Move "super" allocation to the front
        auto first = sorted_allocations.front();
        auto super = m->move_instruction(last, first);
        std::vector<instruction_ref> args = {super};
        std::size_t start = 0;
        std::transform(ins->inputs().begin(), ins->inputs().end() - 1, std::back_inserter(args), [&](instruction_ref input) {
            auto x = insert_copy(make_slice(input, axis, start), input, super);
            start += x->get_shape().lens()[axis];
            return x;
        });
        m->replace_instruction(ins, migraphx::make_op("identity"), args);
    }
};

bool is_packed(instruction_ref ins, std::size_t axis)
{
    auto astride = ins->get_shape().strides()[axis];
    return migraphx::equal(ins->get_shape().lens(), ins->get_shape().strides(), [&](std::size_t len, std::size_t stride) {
        if(stride < astride)
            return true;
        return len == 1;
    });
}

} // namespace

void eliminate_concat::apply(module& m) const
{
    concat_optimizer co{&m, concat_opt.allocation()};
    for(auto ins : iterator_for(m))
    {
        auto concat_op = concat_opt.get_concat(ins->get_operator());
        // Look for the concat operator
        if(not concat_op.has_value())
            continue;
        auto lens              = ins->inputs().front()->get_shape().lens();
        std::size_t axis = tune_axis(lens.size(), concat_op->axis, concat_op->name());
        auto ncopies = std::count_if(ins->inputs().begin(), std::prev(ins->inputs().end()), [&](instruction_ref input) {
            if(co.need_copy(input)) {
                return true;
            }
            if (is_packed(input, axis))
                return false;
            return not concat_opt.supports_non_packed_output(input);
        });
        if(ncopies > 1)
            continue;
        co.replace_concat(ins, axis);
        // // If any inputs are builtin or context free then abort
        // // If any inputs are used more than once, then abort since there could
        // // be errors due to aliasing
        // if(std::any_of(ins->inputs().begin(), ins->inputs().end(), [](auto arg) {
        //        return arg->name().front() == '@' or
        //               (arg->get_operator().is_context_free() and
        //                not contains({"concat", "identity"}, arg->name())) or
        //               arg->outputs().size() > 1;
        //    }))
        //     continue;
        // // We can only do this optimization when concat axis is either the leftmost
        // // axis OR the sizes to the left of this axis are all equal to 1
        // // Since we've already checked that the non-axis dimensions are identical
        // // we only need to check the first input
        // std::size_t axis_index = tune_axis(lens.size(), concat_op->axis, concat_op->name());
        // if(axis_index == 0 or
        //    std::all_of(lens.begin(), lens.begin() + axis_index, [](auto x) { return x == 1; }))
        // {
        //     co.replace_concat(ins, axis_index);
        // }
    }
}
} // namespace MIGRAPHX_INLINE_NS
} // namespace migraphx
