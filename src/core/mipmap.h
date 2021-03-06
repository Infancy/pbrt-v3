
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

#if defined(_MSC_VER)
#define NOMINMAX
#pragma once
#endif

#ifndef PBRT_CORE_MIPMAP_H
#define PBRT_CORE_MIPMAP_H

// core/mipmap.h*
#include "pbrt.h"
#include "spectrum.h"
#include "texture.h"
#include "stats.h"
#include "parallel.h"

namespace pbrt {

STAT_COUNTER("Texture/EWA lookups", nEWALookups);
STAT_COUNTER("Texture/Trilinear lookups", nTrilerpLookups);
STAT_MEMORY_COUNTER("Memory/Texture MIP maps", mipMapMemory);

// MIPMap Helper Declarations
enum class ImageWrap { Repeat, Black, Clamp };

// 向下重采样时用到的权重表
struct ResampleWeight 
{
    int firstTexel; // 只需要记录第一个纹素的下标即可, 其它三个是连续的
    Float weight[4]; // 四个纹素的权重
};

// MIPMap Declarations
template <typename T>
class MIPMap {
  public:
    // MIPMap Public Methods
    MIPMap(const Point2i &resolution, const T *data, bool doTri = false,
           Float maxAniso = 8.f, ImageWrap wrapMode = ImageWrap::Repeat);

    int Width() const  { return resolution[0]; } // resolution.x
    int Height() const { return resolution[1]; } // resolution.y
    int Levels() const { return pyramid.size(); }

    // 根据环绕方式获取 st 坐标
    const T &Texel(int level, int s, int t) const;

    // 两种过滤方式: 各向同性/各向异性, 后者效果更好, 但性能开销也更大
    T Lookup(const Point2f &st, Float width = 0.f) const;
    T Lookup(const Point2f &st, Vector2f dstdx, Vector2f dstdy) const;

  private:
    // MIPMap Private Methods
    
    // 在每个方向上预计算过滤器的权重
    std::unique_ptr<ResampleWeight[]> resampleWeights(int oldRes, int newRes) 
    {
        CHECK_GE(newRes, oldRes);

        std::unique_ptr<ResampleWeight[]> wt(new ResampleWeight[newRes]);
        Float filterwidth = 2.f;
        for (int i = 0; i < newRes; ++i) 
        {
            // Compute image resampling weights for _i_th texel
            // P628/Figure10.12, 左右各取两个
            Float center = (i + .5f) * oldRes / newRes;
            wt[i].firstTexel = std::floor((center - filterwidth) + 0.5f);

            for (int j = 0; j < 4; ++j) 
            {
                Float pos = wt[i].firstTexel + j + .5f;
                // LanczosSincFilter::Sinc()
                wt[i].weight[j] = Lanczos((pos - center) / filterwidth);
            }

            // Normalize filter weights for texel resampling
            // sum to one
            Float invSumWts = 1 / (wt[i].weight[0] + wt[i].weight[1] +
                                   wt[i].weight[2] + wt[i].weight[3]);
            for (int j = 0; j < 4; ++j) 
                wt[i].weight[j] *= invSumWts;
        }
        return wt;
    }

    Float clamp(Float v)                            { return Clamp(v, 0.f, Infinity); }
    RGBSpectrum clamp(const RGBSpectrum &v)         { return  v.Clamp(0.f, Infinity); }
    SampledSpectrum clamp(const SampledSpectrum &v) { return  v.Clamp(0.f, Infinity); }

    T triangle(int level, const Point2f &st) const;
    T EWA(int level, Point2f st, Vector2f dst0, Vector2f dst1) const;

    // MIPMap Private Data
    const bool doTrilinear;
    const Float maxAnisotropy;
    const ImageWrap wrapMode;
    Point2i resolution;
    // TODO: texture_pyramid pyramid; 只用一个 vector 来分配连续的纹理是否可行???
    std::vector<std::unique_ptr<BlockedArray<T>>> pyramid; // 纹理金字塔

    static PBRT_CONSTEXPR int WeightLUTSize = 128;
    static Float weightLut[WeightLUTSize];
};

// MIPMap Method Definitions
template <typename T>
MIPMap<T>::MIPMap(const Point2i &res, const T *img, bool doTrilinear,
                  Float maxAnisotropy, ImageWrap wrapMode)
    : doTrilinear(doTrilinear),
      maxAnisotropy(maxAnisotropy),
      wrapMode(wrapMode),
      resolution(res) 
{
    ProfilePhase _(Prof::MIPMapCreation);

    std::unique_ptr<T[]> resampledImage = nullptr;
    // 如果原始图像不是 PowerOf2, 就先向上采样到 PowerOf2
    if (!IsPowerOf2(resolution[0]) || !IsPowerOf2(resolution[1])) 
    {
        // Resample image to power-of-two resolution
        Point2i resPow2(RoundUpPow2(resolution[0]), RoundUpPow2(resolution[1]));
        LOG(INFO) << "Resampling MIPMap from " << resolution << " to " <<
            resPow2 << ". Ratio= " << (Float(resPow2.x * resPow2.y) /
                                       Float(resolution.x * resolution.y));

        // P627, 使用 separable reconstruction filter, 可以在两个方向独立过滤, (s, t) -> (s', t) -> (s', t')

        // Resample image in $s$ direction
        // 对每行/列来说, 权重表是相同的, 所以预先计算并复用它
        std::unique_ptr<ResampleWeight[]> sWeights = resampleWeights(resolution[0], resPow2[0]);
        resampledImage.reset(new T[resPow2[0] * resPow2[1]]);

        // Apply _sWeights_ to zoom in $s$ direction
        // 并行的对每一行处理
        ParallelFor([&](int t) 
        {
            // (Orig)t: 原分辨率的当前列, (New)s: 现分辨率的当前行
            for (int s = 0; s < resPow2[0]; ++s) 
            {
                // Compute texel $(s,t)$ in $s$-zoomed image
                // auto CurrPixel = resampledImage + t * resPow2[0] + s
                resampledImage[t * resPow2[0] + s] = 0.f;
                for (int j = 0; j < 4; ++j) 
                {
                    int origS = sWeights[s].firstTexel + j;

                    if (wrapMode == ImageWrap::Repeat)
                        origS = Mod(origS, resolution[0]);
                    else if (wrapMode == ImageWrap::Clamp)
                        origS = Clamp(origS, 0, resolution[0] - 1);

                    if (origS >= 0 && origS < (int)resolution[0])
                        resampledImage[t * resPow2[0] + s] +=
                            sWeights[s].weight[j] *
                            img[t * resolution[0] + origS];
                }
            }
        }, resolution[1], 16);

        // Resample image in $t$ direction
        std::unique_ptr<ResampleWeight[]> tWeights = resampleWeights(resolution[1], resPow2[1]);
        std::vector<T *> resampleBufs;
        int nThreads = MaxThreadIndex();
        for (int i = 0; i < nThreads; ++i)
            resampleBufs.push_back(new T[resPow2[1]]);
        ParallelFor([&](int s) 
        {
            // 每列数据在内存中不是紧邻的, 这里用单独一行来存, 缓存友好
            T *workData = resampleBufs[ThreadIndex];
            for (int t = 0; t < resPow2[1]; ++t) 
            {
                workData[t] = 0.f;
                for (int j = 0; j < 4; ++j) 
                {
                    int offset = tWeights[t].firstTexel + j;

                    if (wrapMode == ImageWrap::Repeat)
                        offset = Mod(offset, resolution[1]);
                    else if (wrapMode == ImageWrap::Clamp)
                        offset = Clamp(offset, 0, (int)resolution[1] - 1);

                    if (offset >= 0 && offset < (int)resolution[1])
                        workData[t] += tWeights[t].weight[j] *
                                       resampledImage[offset * resPow2[0] + s];
                }
            }
            for (int t = 0; t < resPow2[1]; ++t)
                resampledImage[t * resPow2[0] + s] = clamp(workData[t]);
        }, resPow2[0], 32);

        for (auto ptr : resampleBufs) delete[] ptr;

        resolution = resPow2;
    }

    // Initialize levels of MIPMap from image
    int nLevels = 1 + Log2Int(std::max(resolution[0], resolution[1]));
    pyramid.resize(nLevels);

    // Initialize most detailed level of MIPMap
    // level0 是分辨率最高的图像, levelMax 是 1*1 的图像???
    // P630: mimap 的两种过滤方法都是访问一定范围内的纹素, BlockedArray 有更好的缓存连续性(Section A.4.4 in Appendix A.)
    pyramid[0].reset(new BlockedArray<T>(
        resolution[0], resolution[1],
        resampledImage ? resampledImage.get() : img));

    for (int i = 1; i < nLevels; ++i) 
    {
        // Initialize $i$th MIPMap level from $i-1$st level
        int sRes = std::max(1, pyramid[i - 1]->uSize() / 2);
        int tRes = std::max(1, pyramid[i - 1]->vSize() / 2);
        pyramid[i].reset(new BlockedArray<T>(sRes, tRes));

        // Filter four texels from finer level of pyramid
        ParallelFor([&](int t) 
        {
            for (int s = 0; s < sRes; ++s)
                // BlockedArray<T>(s, t) = 
                (*pyramid[i])(s, t) =
                    .25f * (Texel(i - 1, 2 * s,     2 * t    ) +
                            Texel(i - 1, 2 * s + 1, 2 * t    ) +
                            Texel(i - 1, 2 * s,     2 * t + 1) +
                            Texel(i - 1, 2 * s + 1, 2 * t + 1));
        }, tRes, 16); // 对每一行并行处理
    }

    // Initialize EWA filter weights if needed
    // 初始化高斯过滤器的权重查找表
    if (weightLut[0] == 0.) 
    {
        for (int i = 0; i < WeightLUTSize; ++i) 
        {
            Float alpha = 2;
            Float r2 = Float(i) / Float(WeightLUTSize - 1);
            // P639, TODO
            weightLut[i] = std::exp(-alpha * r2) - std::exp(-alpha);
        }
    }

    // https://en.wikipedia.org/wiki/1/4_%2B_1/16_%2B_1/64_%2B_1/256_%2B_%E2%8B%AF
    mipMapMemory += (4 * resolution[0] * resolution[1] * sizeof(T)) / 3;
}

template <typename T>
const T &MIPMap<T>::Texel(int level, int s, int t) const 
{
    CHECK_LT(level, pyramid.size());
    const BlockedArray<T> &l = *pyramid[level];

    // Compute texel $(s,t)$ accounting for boundary conditions
    switch (wrapMode) 
    {
    case ImageWrap::Repeat:
        s = Mod(s, l.uSize());
        t = Mod(t, l.vSize());
        break;
    case ImageWrap::Clamp:
        s = Clamp(s, 0, l.uSize() - 1);
        t = Clamp(t, 0, l.vSize() - 1);
        break;
    case ImageWrap::Black: {
        static const T black = 0.f;
        if (s < 0 || s >= (int)l.uSize() || t < 0 || t >= (int)l.vSize())
            return black;
        break;
    }
    }
    return l(s, t);
}



// P632
template <typename T>
T MIPMap<T>::Lookup(const Point2f &st, Float width) const 
{
    ++nTrilerpLookups;
    ProfilePhase p(Prof::TexFiltTrilerp);

    // Compute MIPMap level for trilinear filtering
    // Figure10.13, chooses a level such that the filter covers four texels.
    // 1 / width == 2^(nLevels-1-level)
    // (如果让 level0 是最小的一级, 则可以求解 1 / width == 2^level)
    Float level = Levels() - 1 + Log2(std::max(width, (Float)1e-8));

    // Perform trilinear interpolation at appropriate MIPMap level
    if (level < 0)
        return triangle(0, st);
    else if (level >= Levels() - 1)
        return Texel(Levels() - 1, 0, 0);
    else 
    {
        int iLevel = std::floor(level);
        Float delta = level - iLevel;
        // The implementation here applies the triangle filter at both of
        // these levels and blends between them according to how close level is to each of them
        // This helps hide the transitions from one MIP map level to the next at nearby pixels in the final image.
        return Lerp(delta, triangle(iLevel, st), triangle(iLevel + 1, st));
    }
}

// Filtering techniques like this one that do not support a filter extent that 
// is non-square or non-axis-aligned are known as isotropic.
// 这里使用三角形过滤器, BilerpTexture::Evaluate()
template <typename T>
T MIPMap<T>::triangle(int level, const Point2f &st) const 
{
    level = Clamp(level, 0, Levels() - 1);

    // mapping the continuous texture coordinates to discrete space.
    Float s = st[0] * pyramid[level]->uSize() - 0.5f;
    Float t = st[1] * pyramid[level]->vSize() - 0.5f;

    // TODO P634, Figure10.14
    int s0 = std::floor(s), t0 = std::floor(t);
    Float ds = s - s0, dt = t - t0;

    return (1 - ds) * (1 - dt) * Texel(level, s0,     t0    ) +
           (1 - ds) *      dt  * Texel(level, s0,     t0 + 1) +
                ds  * (1 - dt) * Texel(level, s0 + 1, t0    ) +
                ds  *      dt  * Texel(level, s0 + 1, t0 + 1);
}



template <typename T>
T MIPMap<T>::Lookup(const Point2f &st, Vector2f dst0, Vector2f dst1) const 
{
    if (doTrilinear) 
    {
        Float width = std::max(std::max(std::abs(dst0[0]), std::abs(dst0[1])),
                               std::max(std::abs(dst1[0]), std::abs(dst1[1])));
        return Lookup(st, width);
    }

    ++nEWALookups;
    ProfilePhase p(Prof::TexFiltEWA);

    // Compute ellipse minor and major axes
    if (dst0.LengthSquared() < dst1.LengthSquared()) std::swap(dst0, dst1);
    Float majorLength = dst0.Length();
    Float minorLength = dst1.Length();

    // Clamp ellipse eccentricity if too large
    // 根据 maxAnisotropy 限制 majorLength 的采样率
    // highly eccentric ellipses mean that a large number of texels need to be filtered
    // To avoid this expense, the length of the minor axis may be increased to limit the eccentricity
    // The result may be an increase in blurring, although this effect usually isn’t noticeable **in practice**
    if (minorLength * maxAnisotropy < majorLength && minorLength > 0) 
    {
        Float scale = majorLength / (minorLength * maxAnisotropy);
        dst1 *= scale;
        minorLength *= scale;
    }
    if (minorLength == 0) return triangle(0, st);

    // Choose level of detail for EWA lookup by minorLength
    // and perform EWA filtering
    Float lod = std::max((Float)0, Levels() - (Float)1 + Log2(minorLength));
    int ilod = std::floor(lod);

    // blends between the filtered results at the two levels around the computed level of detail,
    // again to reduce artifacts from transitions from one level to another
    return Lerp(lod - ilod, EWA(ilod,     st, dst0, dst1),
                            EWA(ilod + 1, st, dst0, dst1));
}

// elliptically weighted average, 基于椭圆加权的高斯滤波
// it can properly adapt to different sampling rates along the two image axes
template <typename T>
T MIPMap<T>::EWA(int level, Point2f st, Vector2f dst0, Vector2f dst1) const 
{
    if (level >= Levels()) 
        return Texel(Levels() - 1, 0, 0);

    // Convert EWA coordinates to appropriate scale for level
    // 连续坐标
    // subtracts 0.5 from the continuous position coordinate to align the sample 
    // point with the discrete texel coordinates
    st[0] = st[0] * pyramid[level]->uSize() - 0.5f;
    st[1] = st[1] * pyramid[level]->vSize() - 0.5f;
    dst0[0] *= pyramid[level]->uSize(); dst0[1] *= pyramid[level]->vSize();
    dst1[0] *= pyramid[level]->uSize(); dst1[1] *= pyramid[level]->vSize();

    // Compute ellipse coefficients to bound EWA filter region
    // 假设椭圆位于原点上, 可以简化系数的计算
    Float A = dst0[1] * dst0[1] + dst1[1] * dst1[1] + 1;
    Float B = -2 * (dst0[0] * dst0[1] + dst1[0] * dst1[1]);
    Float C = dst0[0] * dst0[0] + dst1[0] * dst1[0] + 1;
    Float invF = 1 / (A * C - B * B * 0.25f);
    A *= invF;
    B *= invF;
    C *= invF;

    // Compute the ellipse's $(s,t)$ bounding box in texture space
    Float det = -B * B + 4 * A * C;
    Float invDet = 1 / det;
    Float uSqrt = std::sqrt(det * C), vSqrt = std::sqrt(A * det);
    // 离散坐标
    int s0 = std::ceil(st[0] - 2 * invDet * uSqrt);
    int s1 = std::floor(st[0] + 2 * invDet * uSqrt);
    int t0 = std::ceil(st[1] - 2 * invDet * vSqrt);
    int t1 = std::floor(st[1] + 2 * invDet * vSqrt);

    // 遍历椭圆的包围盒
    // Scan over ellipse bound and compute quadratic equation
    T sum(0.f);
    Float sumWts = 0;
    for (int it = t0; it <= t1; ++it) 
    {
        Float tt = it - st[1];
        for (int is = s0; is <= s1; ++is) 
        {
            Float ss = is - st[0]; // 左下角是起点, [t0-t1, 0] * [0, s1-s0]

            // Compute squared radius and filter texel if inside ellipse
            Float r2 = A * ss * ss + B * ss * tt + C * tt * tt;
            if (r2 < 1) // Figure10.16, 是否位于椭圆内
            {
                int index = std::min((int)(r2 * WeightLUTSize), WeightLUTSize - 1);
                Float weight = weightLut[index];

                sum += Texel(level, is, it) * weight;
                sumWts += weight;
            }
        }
    }
    return sum / sumWts; // P638
}

template <typename T>
Float MIPMap<T>::weightLut[WeightLUTSize];

}  // namespace pbrt

#endif  // PBRT_CORE_MIPMAP_H
