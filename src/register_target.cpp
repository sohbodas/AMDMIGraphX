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
#include <string>
#include <unordered_map>
#include <migraphx/register_target.hpp>
#include <migraphx/ranges.hpp>
#include <migraphx/dynamic_loader.hpp>
#include <migraphx/fileutils.hpp>

namespace migraphx {
inline namespace MIGRAPHX_INLINE_NS {

void store_target_lib(const dynamic_loader& lib)
{
    static std::vector<dynamic_loader> target_loader;
    try
    {
        lib.get_function<void()>("register_target")();
        target_loader.push_back(lib);
    }
    catch(const std::runtime_error& err)
    {
        std::cerr << "Invalid target library: " << err.what() << std::endl;
    }
}

std::unordered_map<std::string, target>& target_map()
{
    static std::unordered_map<std::string, target> m; // NOLINT
    return m;
}

void register_target_init() { (void)target_map(); }

void unregister_target(const std::string& name)
{
    if(target_map().find(name) != target_map().end())
        target_map().erase(name);
}

void register_target(const target& t)
{
    if(target_map().find(t.name()) == target_map().end())
        target_map()[t.name()] = t;
}

target make_target(const std::string& name)
{
    if(not contains(target_map(), name))
    {
        auto target_name = make_shared_object_filename("migraphx_" + name);
        store_target_lib(dynamic_loader(target_name));
    }
    const auto it = target_map().find(name);
    if(it == target_map().end())
    {
        MIGRAPHX_THROW("Requested target '" + name + "' is not loaded or not supported");
    }
    return it->second;
}

std::vector<std::string> get_targets()
{
    std::vector<std::string> result;
    std::transform(target_map().begin(),
                   target_map().end(),
                   std::back_inserter(result),
                   [&](auto&& p) { return p.first; });
    std::sort(result.begin(), result.end());
    return result;
}

} // namespace MIGRAPHX_INLINE_NS
} // namespace migraphx
