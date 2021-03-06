
/*
    pbrt source code is Copyright(c) 1998-2016
                        Matt Pharr, Greg Humphreys, and Wenzel Jakob.

    This file is part of pbrt.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are
    met:

    - Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    - Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
    IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
    TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
    PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
    HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 */


// samplers/halton.cpp*
#include "samplers/halton.h"
#include "paramset.h"
#include "rng.h"

namespace pbrt {

// HaltonSampler Local Constants
static PBRT_CONSTEXPR int kMaxResolution = 128;

// HaltonSampler Utility Functions
static void extendedGCD(uint64_t a, uint64_t b, int64_t *x, int64_t *y);
static uint64_t multiplicativeInverse(int64_t a, int64_t n) 
{
    int64_t x, y;
    extendedGCD(a, n, &x, &y);
    return Mod(x, n);
}

static void extendedGCD(uint64_t a, uint64_t b, int64_t *x, int64_t *y) 
{
    if (b == 0) 
    {
        *x = 1;
        *y = 0;
        return;
    }

    int64_t d = a / b, xp, yp;
    extendedGCD(b, a % b, &xp, &yp);
    *x = yp;
    *y = xp - (d * yp);
}

// HaltonSampler Method Definitions
HaltonSampler::HaltonSampler(int samplesPerPixel, const Bounds2i &sampleBounds,
                             bool sampleAtPixelCenter)
    : GlobalSampler(samplesPerPixel), sampleAtPixelCenter(sampleAtPixelCenter) 
{
    // Generate random digit permutations for Halton sampler
    if (radicalInversePermutations.empty()) // 对所有 HaltonSampler 的实例只生成一次
    {
        RNG rng;
        radicalInversePermutations = ComputeRadicalInversePermutations(rng);
    }

    // Find radical inverse base scales and exponents that cover sampling area
    Vector2i res = sampleBounds.pMax - sampleBounds.pMin;
    for (int i = 0; i < 2; ++i) 
    {
        int base = (i == 0) ? 2 : 3;
        int scale = 1, exp = 0;

        while (scale < std::min(res[i], kMaxResolution)) 
        {
            scale *= base;
            ++exp;
        }

        // 假如 res 是 (1920, 1080), 则 scales = (128,243), exps = (7,5)
        // ..., 这样的限制是为了保证浮点数的精度(, 也方便 [0, 1)^2 和像素坐标的映射)
        baseScales[i] = scale;
        baseExponents[i] = exp;
    }

    // Compute stride in samples for visiting each pixel area
    sampleStride = baseScales[0] * baseScales[1]; // 128 * 243

    // Compute multiplicative inverses for _baseScales_
    multInverse[0] = multiplicativeInverse(baseScales[1], baseScales[0]); // (243, 128) => 59
    multInverse[1] = multiplicativeInverse(baseScales[0], baseScales[1]); // (128, 243) => 131
}

std::vector<uint16_t> HaltonSampler::radicalInversePermutations;
int64_t HaltonSampler::GetIndexForSample(int64_t sampleNum) const 
{
    if (currentPixel != pixelForOffset)
    {
        pixelForOffset = currentPixel; // 每采样一个像素才更新一次

        // Compute Halton sample offset for _currentPixel_
        // TODO: 这个偏移量是怎么计算出来的呢
        offsetForCurrentPixel = 0;
        if (sampleStride > 1) 
        {
            Point2i pm(Mod(currentPixel[0], kMaxResolution),
                       Mod(currentPixel[1], kMaxResolution));

            for (int i = 0; i < 2; ++i) 
            {
                uint64_t dimOffset =
                    (i == 0)
                        ? InverseRadicalInverse<2>(pm[i], baseExponents[i])
                        : InverseRadicalInverse<3>(pm[i], baseExponents[i]);

                offsetForCurrentPixel +=
                    dimOffset * baseScales[1 - i] * multInverse[i];
            }
            offsetForCurrentPixel %= sampleStride; // offsetForCurrentPixel ∈ [0, sampleStride)
        }
    }
    return offsetForCurrentPixel + sampleNum * sampleStride; // 在采样一个像素时, 每次变换的是 sampleNum. 所以又是相当于在取一个二维数组的一列数据
}

Float HaltonSampler::SampleDimension(int64_t index, int dim) const 
{
    if (sampleAtPixelCenter && (dim == 0 || dim == 1)) // 在生成 xy 坐标的时候允许均匀采样
        return 0.5f;

    // 前两个维度计算的是 xy, 也就是 Film 平面采样的位置, 对更高维度则...
    if (dim == 0)
        return RadicalInverse(dim, index >> baseExponents[0]);
    else if (dim == 1)
        return RadicalInverse(dim, index / baseScales[1]);
    else
        // Float ScrambledRadicalInverse(int base, uint64_t a, const uint16_t *perm)
        return ScrambledRadicalInverse(dim, index,
                                       PermutationForDimension(dim));
}

std::unique_ptr<Sampler> HaltonSampler::Clone(int seed) {
    return std::unique_ptr<Sampler>(new HaltonSampler(*this));
}

HaltonSampler *CreateHaltonSampler(const ParamSet &params,
                                   const Bounds2i &sampleBounds) {
    int nsamp = params.FindOneInt("pixelsamples", 16);
    if (PbrtOptions.quickRender) nsamp = 1;
    bool sampleAtCenter = params.FindOneBool("samplepixelcenter", false);
    return new HaltonSampler(nsamp, sampleBounds, sampleAtCenter);
}

}  // namespace pbrt
