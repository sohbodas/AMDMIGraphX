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
#include <migraphx/instruction_ref.hpp>
#include <migraphx/onnx/op_parser.hpp>
#include <migraphx/onnx/onnx_parser.hpp>
#include <migraphx/onnx/checks.hpp>
#include <migraphx/ranges.hpp>
#include <migraphx/instruction.hpp>
#include <migraphx/make_op.hpp>

namespace migraphx {
inline namespace MIGRAPHX_INLINE_NS {
namespace onnx {

struct parse_if : op_parser<parse_if>
{
    std::vector<op_desc> operators() const { return {{"If"}}; }

    std::vector<instruction_ref> parse(const op_desc& /*opd*/,
                                       onnx_parser& parser,
                                       const onnx_parser::node_info& info,
                                       std::vector<instruction_ref> args) const
    {
        const auto& then_graph = info.attributes.at("then_branch").g();
        const auto& else_graph = info.attributes.at("else_branch").g();

        if(args.front()->get_shape().elements() != 1)
        {
            MIGRAPHX_THROW("PARSE_IF: " + info.name +
                           " condition input can have only one element!");
        }

        std::string then_name = info.name + "_if";
        module_ref then_mdl   = parser.prog.create_module(then_name);

        std::string else_name = info.name + "_else";
        module_ref else_mdl   = parser.prog.create_module(else_name);

        // parse the then sub_graph
        parser.parse_graph(then_mdl, then_graph);

        // parse_the else sub_graph
        parser.parse_graph(else_mdl, else_graph);

        auto then_out_shapes = then_mdl->get_output_shapes();
        auto else_out_shapes = else_mdl->get_output_shapes();

        assert(then_out_shapes.size() == else_out_shapes.size());

        // Must have the same type for both if/else blocks by onnx spec
        // Add exception for empty constant scalars
        if(then_out_shapes.at(0).type() != else_out_shapes.at(0).type())
        {
            MIGRAPHX_THROW("PARSE_IF: " + info.name +
                           " then and else sub_grahps must have same output type! " +
                           then_out_shapes.at(0).type_string() + " vs " +
                           else_out_shapes.at(0).type_string());
        }

        if(not then_out_shapes.at(0).dynamic() && not else_out_shapes.at(0).dynamic())
        {
            if(then_out_shapes.at(0).scalar() && not else_out_shapes.at(0).scalar())
            {
                auto convert_ins = std::prev(then_mdl->end());
                if(then_out_shapes.at(0).type() != else_out_shapes.at(0).type() &&
                   then_out_shapes.at(0).elements() < 1)
                {
                    convert_ins = then_mdl->insert_instruction(
                        convert_ins,
                        migraphx::make_op("convert",
                                          {{"target_type", else_out_shapes.at(0).type()}}),
                        convert_ins->inputs().back());
                    // then_mdl->replace_return({convert_ins});
                }

                migraphx::shape s{else_out_shapes.at(0).type(),
                                  else_out_shapes.at(0).lens(),
                                  else_out_shapes.at(0).strides()};
                auto reshape_ins = then_mdl->insert_instruction(
                    convert_ins, migraphx::make_op("unsqueeze", {{"axes", {0, 1}}}), convert_ins);

                then_mdl->replace_return({reshape_ins});
            }
            else if(not then_out_shapes.at(0).scalar() && else_out_shapes.at(0).scalar())
            {
                auto convert_ins = std::prev(else_mdl->end());
                if(then_out_shapes.at(0).type() != else_out_shapes.at(0).type() &&
                   else_out_shapes.at(0).elements() < 1)
                {
                    convert_ins = then_mdl->insert_instruction(
                        std::prev(then_mdl->end()),
                        migraphx::make_op("convert",
                                          {{"target_type", then_out_shapes.at(0).type()}}),
                        std::prev(then_mdl->end())->inputs().front());
                    then_mdl->replace_return({convert_ins});
                }
                migraphx::shape s{then_out_shapes.at(0).type(),
                                  then_out_shapes.at(0).lens(),
                                  then_out_shapes.at(0).strides()};
                auto reshape_ins = then_mdl->insert_instruction(
                    convert_ins, migraphx::make_op("unsqueeze", {{"axes", {0, 1}}}), convert_ins);

                else_mdl->replace_return({reshape_ins});
            }

            // First dimension must agree
            if(then_out_shapes.at(0).lens().at(0) != else_out_shapes.at(0).lens().at(0))
            {
                MIGRAPHX_THROW("PARSE_IF: " + then_out_shapes.at(0).type_string() + " & " +
                               else_out_shapes.at(0).type_string() +
                               " are incompatible output shapes for then/cases");
            }

            auto then_out_strides = then_out_shapes.at(0).strides();
            auto else_out_strides = else_out_shapes.at(0).strides();

            // Generate compatible output types based on largest dimension with rank 1 tensor
            if(then_out_strides.size() > else_out_strides.size())
            {
                auto reshape_ins = else_mdl->insert_instruction(
                    std::prev(else_mdl->end()),
                    migraphx::make_op("reshape",
                                      {{"dims", {else_out_shapes.at(0).lens().at(0), 1}}}),
                    std::prev(else_mdl->end())->inputs().front());
                else_mdl->replace_return({reshape_ins});
            }
            else if(then_out_strides.size() < else_out_strides.size())
            {
                auto reshape_ins = then_mdl->insert_instruction(
                    std::prev(then_mdl->end()),
                    migraphx::make_op("reshape",
                                      {{"dims", {then_out_shapes.at(0).lens().at(0), 1}}}),
                    std::prev(then_mdl->end())->inputs().front());
                then_mdl->replace_return({reshape_ins});
            }
        }

        auto if_ret = info.add_instruction(make_op("if"), args, {then_mdl, else_mdl});
        auto out_s  = if_ret->get_shape();
        assert(out_s.type() == shape::tuple_type);

        const auto& vec_shapes = out_s.sub_shapes();
        std::vector<instruction_ref> out_inss;
        for(std::size_t i = 0; i < vec_shapes.size(); ++i)
        {
            auto ret = info.add_instruction(make_op("get_tuple_elem", {{"index", i}}), if_ret);
            out_inss.push_back(ret);
        }

        return out_inss;
    }
};

} // namespace onnx
} // namespace MIGRAPHX_INLINE_NS
} // namespace migraphx
