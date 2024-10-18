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

#include <migraphx/config.hpp>
#include <migraphx/bit_cast.hpp>
#include <algorithm>
#include <limits>

namespace migraphx {
inline namespace MIGRAPHX_INLINE_NS {

template<unsigned int N>
constexpr unsigned int all_ones() noexcept
{
    return (1 << N) - 1;
}

struct float32_parts 
{
    unsigned int mantissa : 23;
    unsigned int exponent : 8;
    unsigned int sign : 1;

    static constexpr unsigned int mantissa_width()
    {
        return 23;
    }

    static constexpr unsigned int max_exponent()
    {
        return all_ones<8>();
    }

    static constexpr int exponent_bias()
    {
        return all_ones<7>();
    }

    constexpr float to_float() const noexcept
    {
        return migraphx::bit_cast<float>(*this);
    }
};

constexpr float32_parts get_parts(float f)
{
    return migraphx::bit_cast<float32_parts>(f);
}

template<unsigned int MantissaSize, unsigned int ExponentSize, unsigned int Flags = 0>
struct generic_float
{
    unsigned int mantissa : MantissaSize;
    unsigned int exponent : ExponentSize;
    unsigned int sign : 1;

    static constexpr int exponent_bias()
    {
        return all_ones<ExponentSize - 1>();
    }

    explicit constexpr generic_float(float f = 0.0) noexcept
    {
        from_float(get_parts(f));
    }

    constexpr generic_float &operator=(float f) noexcept
    {
        from_float(get_parts(f));
        return *this;
    }

    constexpr generic_float operator-() const noexcept
    {
        generic_float result = *this;
        result.sign = !this->sign;
        return result;
    }


    constexpr float to_float() const noexcept
    {
        float32_parts f{};
        f.sign = sign;
        f.mantissa = mantissa << (float32_parts::mantissa_width() - MantissaSize);

        if(exponent == 1 and mantissa == 0) 
        {
            f.exponent = 1;
        }
        else if(exponent == all_ones<ExponentSize>())
        {
            f.exponent = float32_parts::max_exponent();
        }
        else
        {
            constexpr const auto diff = float32_parts::exponent_bias() - exponent_bias();
            f.exponent = exponent + diff;
        }
        return f.to_float();
    }

    constexpr void from_float(float32_parts f) noexcept
    {
        sign  = f.sign;
        mantissa = f.mantissa >> (float32_parts::mantissa_width() - MantissaSize);

        if(f.exponent == 0)
        {
            exponent = 0;
        }
        else if (f.exponent == 1 and f.mantissa == 0)
        {
            exponent = 1;
        }
        else if(f.exponent == float32_parts::max_exponent())
        {
            exponent = all_ones<ExponentSize>();
        }
        else
        {
            constexpr const int diff = float32_parts::exponent_bias() - exponent_bias();
            auto e = int(f.exponent) - diff;

            if(e >= static_cast<int>(all_ones<ExponentSize>()))
            {
                exponent = all_ones<ExponentSize>();
                mantissa = 0;
            }
            else if(e < 0)
            {
                exponent = 0;
                mantissa = 0;
            }
            else
            {
                exponent = f.exponent - diff;
            }
        }

        exponent = std::min(exponent, all_ones<ExponentSize>());
    }

    constexpr bool is_normal() const noexcept
    {
        return exponent != all_ones<ExponentSize>() and exponent != 0;
    }

    constexpr bool is_inf() const noexcept
    {
        return exponent == all_ones<ExponentSize>() and mantissa == 0;
    }

    constexpr bool is_nan() const noexcept
    {
        return exponent == all_ones<ExponentSize>() and mantissa != 0;
    }

    constexpr bool is_finite() const noexcept
    {
        return exponent != all_ones<ExponentSize>();
    }

    constexpr operator float() const noexcept
    {
        return this->to_float();
    }

    static constexpr generic_float infinity()
    {
        generic_float x{};
        x.exponent = all_ones<ExponentSize>();
        return x;
    }

    static constexpr generic_float snan()
    {
        generic_float x{};
        x.exponent = all_ones<ExponentSize>();
        x.mantissa = 1 << (MantissaSize - 2);
        return x;
    }

    static constexpr generic_float qnan()
    {
        generic_float x{};
        x.exponent = all_ones<ExponentSize>();
        x.mantissa = 1 << (MantissaSize - 1);
        return x;
    }

    static constexpr generic_float min()
    {
        generic_float x{};
        x.exponent = 1;
        x.mantissa = 0;
        return x;
    }

    static constexpr generic_float denorm_min()
    {
        generic_float x{};
        x.exponent = 0;
        x.mantissa = 1;
        x.sign = 0;
        return x;
    }

    static constexpr generic_float lowest()
    {
        generic_float x{};
        x.exponent = all_ones<ExponentSize>() - 1;
        x.mantissa = all_ones<MantissaSize>();
        x.sign = 1;
        return x;
    }

    static constexpr generic_float max()
    {
        generic_float x{};
        x.exponent = all_ones<ExponentSize>() - 1;
        x.mantissa = all_ones<MantissaSize>();
        x.sign = 0;
        return x;
    }

    static constexpr generic_float epsilon()
    {
        generic_float x{1.0};
        x.mantissa++;
        return generic_float{x.to_float() - 1.0f};
    }
// NOLINTNEXTLINE
#define MIGRAPHX_GENERIC_FLOAT_ASSIGN_OP(op) \
    constexpr generic_float& operator op(const generic_float& rhs) \
    { \
        float self = *this; \
        float frhs = rhs; \
        self op frhs; \
        *this = generic_float(self); \
        return *this; \
    }
    MIGRAPHX_GENERIC_FLOAT_ASSIGN_OP(*=)
    MIGRAPHX_GENERIC_FLOAT_ASSIGN_OP(-=)
    MIGRAPHX_GENERIC_FLOAT_ASSIGN_OP(+=)
    MIGRAPHX_GENERIC_FLOAT_ASSIGN_OP(/=)
// NOLINTNEXTLINE
#define MIGRAPHX_GENERIC_FLOAT_BINARY_OP(op) \
    friend constexpr generic_float operator op(const generic_float& x, const generic_float& y) \
    { \
        return generic_float(float(x) op float(y)); \
    }
    MIGRAPHX_GENERIC_FLOAT_BINARY_OP(*)
    MIGRAPHX_GENERIC_FLOAT_BINARY_OP(-)
    MIGRAPHX_GENERIC_FLOAT_BINARY_OP(+)
    MIGRAPHX_GENERIC_FLOAT_BINARY_OP(/)
    MIGRAPHX_GENERIC_FLOAT_BINARY_OP(<)
    MIGRAPHX_GENERIC_FLOAT_BINARY_OP(<=)
    MIGRAPHX_GENERIC_FLOAT_BINARY_OP(>)
    MIGRAPHX_GENERIC_FLOAT_BINARY_OP(>=)

    friend constexpr bool operator==(const generic_float& x, const generic_float& y)
    {
        if (not x.is_finite() or not y.is_finite())
            return false;
        return std::tie(x.mantissa, x.exponent, x.sign) == std::tie(y.mantissa, y.exponent, y.sign);
    }

    friend constexpr bool operator!=(const generic_float& x, const generic_float& y)
    {
        return not(x == y);
    }
};


}
}

namespace std {

template<unsigned int E, unsigned int M, unsigned int F>
class numeric_limits<migraphx::generic_float<E, M, F>>
{
    public:
    static constexpr bool has_infinity = true;
    static constexpr migraphx::generic_float<E, M, F> epsilon() { return migraphx::generic_float<E, M, F>::epsilon(); }

    static constexpr migraphx::generic_float<E, M, F> quiet_NaN() { return migraphx::generic_float<E, M, F>::qnan(); }

    static constexpr migraphx::generic_float<E, M, F> max() { return migraphx::generic_float<E, M, F>::max(); }

    static constexpr migraphx::generic_float<E, M, F> min() { return migraphx::generic_float<E, M, F>::min(); }

    static constexpr migraphx::generic_float<E, M, F> lowest() { return migraphx::generic_float<E, M, F>::lowest(); }

    static constexpr migraphx::generic_float<E, M, F> infinity() { return migraphx::generic_float<E, M, F>::infinity(); }

};

template<unsigned int E, unsigned int M, unsigned int F, class T>
struct common_type<migraphx::generic_float<E, M, F>, T> : std::common_type<float, T> // NOLINT
{
};

template<unsigned int E, unsigned int M, unsigned int F, class T>
struct common_type<T, migraphx::generic_float<E, M, F>> : std::common_type<float, T> // NOLINT
{
};

// template<unsigned int E, unsigned int M, unsigned int F, bool FNUZ>
// struct common_type<migraphx::generic_float<E, M, F>, migraphx::fp8::float8<migraphx::fp8::f8_type::fp8, FNUZ>> : std::common_type<float, float>
// {};

// template<unsigned int E, unsigned int M, unsigned int F, bool FNUZ>
// struct common_type<migraphx::fp8::float8<migraphx::fp8::f8_type::fp8, FNUZ>, migraphx::generic_float<E, M, F>> : std::common_type<float, float>
// {};

// template<unsigned int E, unsigned int M, unsigned int F, migraphx::fp8::f8_type T, bool FNUZ>
// struct common_type<migraphx::generic_float<E, M, F>, migraphx::fp8::float8<T, FNUZ>> : std::common_type<float, float>
// {};

// template<unsigned int E, unsigned int M, unsigned int F, migraphx::fp8::f8_type T, bool FNUZ>
// struct common_type<migraphx::fp8::float8<T, FNUZ>, migraphx::generic_float<E, M, F>> : std::common_type<float, float>
// {};

template<unsigned int E, unsigned int M, unsigned int F>
struct common_type<migraphx::generic_float<E, M, F>,  migraphx::generic_float<E, M, F>>
{
    using type = migraphx::generic_float<E, M, F>;
};

// template<unsigned int E, unsigned int M, unsigned int F, unsigned int E1, .....>
// struct common_type<migraphx::generic_float<E, M, F>,  migraphx::generic_float<E1, M1, F1>>
// {
//     using type = float;
// };


}
