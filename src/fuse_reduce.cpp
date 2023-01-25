/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2022 Advanced Micro Devices, Inc. All rights reserved.
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
#include <migraphx/fuse_reduce.hpp>
#include <migraphx/pass_manager.hpp>
#include <migraphx/dead_code_elimination.hpp>
#include <migraphx/instruction.hpp>
#include <migraphx/program.hpp>
#include <migraphx/make_op.hpp>
#include <migraphx/iterator_for.hpp>
#include <migraphx/ranges.hpp>
#include <migraphx/check_shapes.hpp>
#include <migraphx/matcher.hpp>
#include <migraphx/register_op.hpp>
#include <iterator>
#include <map>

namespace migraphx {
inline namespace MIGRAPHX_INLINE_NS {

struct fused_reduce
{
    std::vector<std::int64_t> axes{};

    template <class Self, class F>
    static auto reflect(Self& self, F f)
    {
        return pack(f(self.axes, "axes"));
    }

    shape compute_shape(const std::vector<shape>& inputs, std::vector<module_ref> mods) const
    {
        if(mods.size() != 1)
            MIGRAPHX_THROW("should have one submodule.");
        auto* sm = mods.front();
        if(sm->get_output_shapes().size() != 1)
            MIGRAPHX_THROW("Only one output supported");
        check_shapes{inputs, *this}.has(sm->get_parameter_shapes().size()).same_dims();
        auto s    = inputs.at(0);
        auto lens = s.lens();
        if(lens != sm->get_output_shapes().front().lens())
        {
            for(const auto& axis : axes)
            {
                lens[axis] = 1;
            }
        }

        return shape::from_permutation(
            sm->get_output_shapes().front().type(), lens, find_permutation(inputs));
    }

    std::string name() const { return "fused_reduce"; }
};
MIGRAPHX_REGISTER_OP(fused_reduce);

static std::unordered_map<instruction_ref, instruction_ref>
get_ins_param_map(const std::vector<instruction_ref>& inputs, const_module_ref sm)
{
    std::unordered_map<instruction_ref, instruction_ref> result;
    auto names = sm->get_parameter_names();
    std::sort(names.begin(), names.end());
    assert(names.size() == inputs.size());
    std::transform(names.begin(),
                   names.end(),
                   inputs.begin(),
                   std::inserter(result, result.end()),
                   [&](const auto& name, auto input) {
                       return std::make_pair(input, sm->get_parameter(name));
                   });
    return result;
}

static void insert_params(module_ref sm, instruction_ref ins, std::unordered_map<instruction_ref, instruction_ref>& map_ins)
{
    auto n = sm->get_parameter_shapes().size();
    for(auto input:ins->inputs())
    {
        if(contains(map_ins, input))
            continue;
        // TODO: Ensure standard shape
        map_ins[input] = sm->add_parameter("x" + std::to_string(n++), input->get_shape());
    }
}

static auto insert_ins_in_submodule(module_ref sm, instruction_ref ins, std::unordered_map<instruction_ref, instruction_ref>& map_ins)
{
    insert_params(sm, ins, map_ins);
    return sm->add_instructions({ins}, map_ins);
}

static auto insert_ins_in_submodule(module_ref sm, instruction_ref ins)
{
    std::unordered_map<instruction_ref, instruction_ref> map_ins;
    return insert_ins_in_submodule(sm, ins, map_ins);
}

static auto insert_module_in_submodule(module_ref sm, instruction_ref ins, std::unordered_map<instruction_ref, instruction_ref>& map_ins)
{
    insert_params(sm, ins, map_ins);
    auto* m = ins->module_inputs().front();
    auto param_map = get_ins_param_map(ins->inputs(), m);
    for(auto&& [input, param]:param_map)
    {
        map_ins[param] = map_ins.at(input);
    }
    return sm->add_instructions(m, map_ins);
}

static std::vector<instruction_ref> find_inputs(module_ref sm, const std::unordered_map<instruction_ref, instruction_ref>& map_ins)
{
    std::vector<instruction_ref> result;
    std::map<std::string, instruction_ref> names;
    for(auto&& [input, param]:map_ins)
    {
        if(not sm->has_instruction(param))
            continue;
        if(param->name() != "@param")
            continue;
        auto v = param->get_operator().to_value();
        auto name = v.at("parameter").to<std::string>();
        names[name] = input;
    }
    std::transform(names.begin(), names.end(), std::back_inserter(result), [](const auto& p) {
        return p.second;
    });
    return result;
}

static void create_reduce_modules(module_pass_manager& mpm)
{
    std::size_t n = 0;
    for(auto ins : iterator_for(mpm.get_module()))
    {
        if(not ins->get_operator().attributes().get("reduce", false))
            continue;
        if(ins->inputs().size() != 1)
            continue;

        auto* rm =
            mpm.create_module(mpm.get_module().name() + ":" + ins->name() + std::to_string(n++));
        rm->set_bypass();

        rm->add_return(insert_ins_in_submodule(rm, ins));

        auto v = ins->get_operator().to_value();
        mpm.get_module().replace_instruction(
            ins, make_op("fused_reduce", {{"axes", v["axes"]}}), ins->inputs(), {rm});
    }
}

static std::vector<instruction_ref> get_returns(module& m)
{
    auto last = std::prev(m.end());
    if(last->name() == "@return")
        return last->inputs();
    return {last};
}

namespace {
struct find_pointwise_reduce
{
    auto matcher() const
    {
        return match::name("fused_reduce")(match::any_of[match::inputs()](match::name("pointwise")(match::used_once()).bind("pointwise")));
    }

    void apply(module_pass_manager& mpm, const match::matcher_result& r) const
    {
        auto reduce    = r.result;
        auto pw = r.instructions["pointwise"];

        const auto* pm = pw->module_inputs().front();
        // const auto* old_rm = reduce->module_inputs().front();
        auto* rm           = mpm.create_module(pm->name() + ":reduce");
        rm->set_bypass();

        std::unordered_map<instruction_ref, instruction_ref> map_ins;
        // Insert pointwise
        auto rins = insert_ins_in_submodule(rm, pw, map_ins).front();
        map_ins[pw] = rins;
        // Insert fused_reduce
        insert_module_in_submodule(rm, reduce, map_ins);

        auto new_inputs = find_inputs(rm, map_ins);
        mpm.get_module().replace_instruction(reduce, reduce->get_operator(), new_inputs, {rm});
    }
};

struct find_reduce_pointwise
{
    auto matcher() const
    {
        return match::name("pointwise")(match::any_of[match::inputs()](
            match::name("fused_reduce")(match::used_once()).bind("reduce")));
    }

    void apply(module_pass_manager& mpm, const match::matcher_result& r) const
    {
        auto pw    = r.result;
        auto reduce = r.instructions["reduce"];

        const auto* old_rm = reduce->module_inputs().front();
        auto* rm           = mpm.create_module(old_rm->name() + ":pointwise");
        rm->set_bypass();
        std::unordered_map<instruction_ref, instruction_ref> map_ins;
        // Copy module instructions
        insert_module_in_submodule(rm, reduce, map_ins);
        map_ins[reduce] = get_returns(*rm).front();

        auto out = insert_ins_in_submodule(rm, pw, map_ins);
        rm->replace_return(out);

        auto new_inputs = find_inputs(rm, map_ins);
        mpm.get_module().replace_instruction(pw, reduce->get_operator(), new_inputs, {rm});
    }
};
}

void fuse_reduce::apply(module_pass_manager& mpm) const
{
    create_reduce_modules(mpm);
    mpm.run_pass(dead_code_elimination{});
    match::find_matches(mpm, find_reduce_pointwise{}, find_pointwise_reduce{});
    mpm.run_pass(dead_code_elimination{});
}

} // namespace MIGRAPHX_INLINE_NS
} // namespace migraphx
