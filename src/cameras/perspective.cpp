
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


// cameras/perspective.cpp*
#include "cameras/perspective.h"
#include "paramset.h"
#include "sampler.h"
#include "sampling.h"
#include "light.h"
#include "stats.h"

namespace pbrt {

/*
    相机空间

                y(正上方)
                ^           /
                |          /                                       *
                |         /                                   *
                |        /                               *
                |       /                           *
                |      /                     *
                |     /                 *           *
                |    /             *
                |   /         *                     *
                |  /      *
                | /   *                             *
                |/ *
              z=0 -   -   -   -    -    -    -      -    -    -    -    ->z(视见方向)
               /|  *    #
              / |     *       #                     *
             /  |         *            #
            /   |             *                     *
           /    |                  *                #
          /     |                       *           *
         /      |                            *                  #
        /       |                                   *
       /        |                                   ^   *                    #
      V         |                                   |        *
     x          |                                  z=1            *


*/

// PerspectiveCamera Method Definitions
PerspectiveCamera::PerspectiveCamera(const AnimatedTransform &CameraToWorld,
                                     const Bounds2f &screenWindow,
                                     Float shutterOpen, Float shutterClose,
                                     Float lensRadius, Float focalDistance,
                                     Float fov, Film *film,
                                     const Medium *medium)

    : ProjectiveCamera(
        /*
            这里把 perspective 的 near, far 给写死了
            不过 PerspectiveCamera 构造函数的参数也太多了, 写成 ParamSet 的形式要好一些
        */
        CameraToWorld, Perspective(fov, 1e-2f, 1000.f),
        screenWindow, shutterOpen, shutterClose, 
        lensRadius, focalDistance, 
        film, medium) 
{
    // Compute differential changes in origin for perspective camera rays
    dxCamera =
        (RasterToCamera(Point3f(1, 0, 0)) - RasterToCamera(Point3f(0, 0, 0)));
    dyCamera =
        (RasterToCamera(Point3f(0, 1, 0)) - RasterToCamera(Point3f(0, 0, 0)));

    // Compute image plane bounds at $z=1$ for _PerspectiveCamera_
    Point2i res = film->fullResolution;

    // pMin 和 pMax 被变换到相机空间的近平面中
    Point3f pMin = RasterToCamera(Point3f(0, 0, 0));
    Point3f pMax = RasterToCamera(Point3f(res.x, res.y, 0));

    // 除以 z 值投影到 z=1 的位置, 然后计算 Area 的大小
    pMin /= pMin.z;
    pMax /= pMax.z;
    A = std::abs((pMax.x - pMin.x) * (pMax.y - pMin.y));
}

/*
真实世界中, 凸透镜成像的侧视示意图
                                                                *|
                                                        *   *    |
                                                 *     *         |
|                                         *       *              |
|                                  *         *                   |
|              |            *           *                        |
|              |     *             *                             |
|              |*              *                                 |
|           *  |           *                                     |
|         *    |      *                                          |
|       *      |  *                                              |
|     *       *|                                                 focal(焦平面)
|   *     *    |
| *  *         |
|*             |
|            lens
|
image(像平面)



虚拟相机的成像:
                                                                *|<- scene sample
                                                        *   *    |
                                                 *     *         |
                       sample ray         *       *              |
                                   *         *                   |
               |            *           *                        |
               |     *             *                             |
   len sample->|*              *     primary ray                 |
               |          *                                      |
               |      *                                          |
               |  *                                              |
               |                                                 focal
               |
               |
               |
             lens(z = 0)

先使用穿过透镜中心的主光线(这条光线不会发生折射)确定焦平面的采样位置，
再根据 len sample 和 scene sample 生成采样光线, 发射到场景中进行采样
*/

Float PerspectiveCamera::GenerateRay(const CameraSample &sample,
                                     Ray *ray) const 
{
    ProfilePhase prof(Prof::GenerateCameraRay);

    // Compute raster and camera sample positions
    Point3f pFilm = Point3f(sample.pFilm.x, sample.pFilm.y, 0);
    Point3f pCamera = RasterToCamera(pFilm); // pFilm 的 z 坐标为 0, 所以会变换到相机空间的近平面上

    // 生成主光线(起点是相机原点, 所以就直接把 pCamera 当方向用了)
    *ray = Ray(Point3f(0, 0, 0), Normalize(Vector3f(pCamera)));

    // Modify ray for depth of field
    if (lensRadius > 0) 
    {
        // Sample point on lens
        Point2f pLens = lensRadius * ConcentricSampleDisk(sample.pLens);

        // Compute point on plane of focus
        // 通过主光线找到焦平面上的采样点
        Float ft = focalDistance / ray->d.z; // 根据相似三角形计算原点到采样点的距离
        Point3f pFocus = (*ray)(ft);

        // Update ray for effect of lens
        // 生成薄透镜上的采样光线
        ray->o = Point3f(pLens.x, pLens.y, 0);
        ray->d = Normalize(pFocus - ray->o); // 当对焦时, 这根光线可以击中焦平面上的采样点, 否则失焦产生景深
    }

    ray->time = Lerp(sample.time, shutterOpen, shutterClose);
    ray->medium = medium;
    *ray = CameraToWorld(*ray);

    return 1;
}

Float PerspectiveCamera::GenerateRayDifferential(const CameraSample &sample,
                                                 RayDifferential *ray) const {
    ProfilePhase prof(Prof::GenerateCameraRay);

    // Compute raster and camera sample positions
    Point3f pFilm = Point3f(sample.pFilm.x, sample.pFilm.y, 0);
    Point3f pCamera = RasterToCamera(pFilm);
    Vector3f dir = Normalize(Vector3f(pCamera.x, pCamera.y, pCamera.z));
    // 计算主光线/中心光线
    *ray = RayDifferential(Point3f(0, 0, 0), dir);

    // Modify ray for depth of field
    if (lensRadius > 0)
    {
        // Sample point on lens
        Point2f pLens = lensRadius * ConcentricSampleDisk(sample.pLens);

        // Compute point on plane of focus
        Float ft = focalDistance / ray->d.z;
        Point3f pFocus = (*ray)(ft);

        // Update ray for effect of lens
        ray->o = Point3f(pLens.x, pLens.y, 0);
        ray->d = Normalize(pFocus - ray->o);
    }

    // Compute offset rays for _PerspectiveCamera_ ray differentials
    if (lensRadius > 0) 
    {
        // Compute _PerspectiveCamera_ ray differentials accounting for lens

        // Sample point on lens
        Point2f pLens = lensRadius * ConcentricSampleDisk(sample.pLens);
        Vector3f dx = Normalize(Vector3f(pCamera + dxCamera)); // dx 的偏移方向

        Float ft = focalDistance / dx.z;
        Point3f pFocus = Point3f(0, 0, 0) + (ft * dx); // 焦平面的采样点

        ray->rxOrigin = Point3f(pLens.x, pLens.y, 0);
        ray->rxDirection = Normalize(pFocus - ray->rxOrigin);

        Vector3f dy = Normalize(Vector3f(pCamera + dyCamera));
        ft = focalDistance / dy.z;
        pFocus = Point3f(0, 0, 0) + (ft * dy);
        ray->ryOrigin = Point3f(pLens.x, pLens.y, 0);
        ray->ryDirection = Normalize(pFocus - ray->ryOrigin);
    } 
    else 
    {
        // 针孔相机下, 选取同个起点, 只是发射方向做偏移
        ray->rxOrigin = ray->ryOrigin = ray->o;
        ray->rxDirection = Normalize(Vector3f(pCamera) + dxCamera);
        ray->ryDirection = Normalize(Vector3f(pCamera) + dyCamera);
    }

    ray->time = Lerp(sample.time, shutterOpen, shutterClose);
    ray->medium = medium;
    *ray = CameraToWorld(*ray);
    ray->hasDifferentials = true;

    return 1;
}

Spectrum PerspectiveCamera::We(const Ray &ray, Point2f *pRaster2) const {
    // Interpolate camera matrix and check if $\w{}$ is forward-facing
    Transform c2w;
    CameraToWorld.Interpolate(ray.time, &c2w);
    Float cosTheta = Dot(ray.d, c2w(Vector3f(0, 0, 1)));
    if (cosTheta <= 0) return 0;

    // Map ray $(\p{}, \w{})$ onto the raster grid
    Point3f pFocus = ray((lensRadius > 0 ? focalDistance : 1) / cosTheta);
    Point3f pRaster = Inverse(RasterToCamera)(Inverse(c2w)(pFocus));

    // Return raster position if requested
    if (pRaster2) *pRaster2 = Point2f(pRaster.x, pRaster.y);

    // Return zero importance for out of bounds points
    Bounds2i sampleBounds = film->GetSampleBounds();
    if (pRaster.x < sampleBounds.pMin.x || pRaster.x >= sampleBounds.pMax.x ||
        pRaster.y < sampleBounds.pMin.y || pRaster.y >= sampleBounds.pMax.y)
        return 0;

    // Compute lens area of perspective camera
    Float lensArea = lensRadius != 0 ? (Pi * lensRadius * lensRadius) : 1;

    // Return importance for point on image plane
    Float cos2Theta = cosTheta * cosTheta;
    return Spectrum(1 / (A * lensArea * cos2Theta * cos2Theta));
}

void PerspectiveCamera::Pdf_We(const Ray &ray, Float *pdfPos,
                               Float *pdfDir) const {
    // Interpolate camera matrix and fail if $\w{}$ is not forward-facing
    Transform c2w;
    CameraToWorld.Interpolate(ray.time, &c2w);
    Float cosTheta = Dot(ray.d, c2w(Vector3f(0, 0, 1)));
    if (cosTheta <= 0) {
        *pdfPos = *pdfDir = 0;
        return;
    }

    // Map ray $(\p{}, \w{})$ onto the raster grid
    Point3f pFocus = ray((lensRadius > 0 ? focalDistance : 1) / cosTheta);
    Point3f pRaster = Inverse(RasterToCamera)(Inverse(c2w)(pFocus));

    // Return zero probability for out of bounds points
    Bounds2i sampleBounds = film->GetSampleBounds();
    if (pRaster.x < sampleBounds.pMin.x || pRaster.x >= sampleBounds.pMax.x ||
        pRaster.y < sampleBounds.pMin.y || pRaster.y >= sampleBounds.pMax.y) {
        *pdfPos = *pdfDir = 0;
        return;
    }

    // Compute lens area of perspective camera
    Float lensArea = lensRadius != 0 ? (Pi * lensRadius * lensRadius) : 1;
    *pdfPos = 1 / lensArea;
    *pdfDir = 1 / (A * cosTheta * cosTheta * cosTheta);
}

Spectrum PerspectiveCamera::Sample_Wi(const Interaction &ref, const Point2f &u,
                                      Vector3f *wi, Float *pdf,
                                      Point2f *pRaster,
                                      VisibilityTester *vis) const {
    // Uniformly sample a lens interaction _lensIntr_
    Point2f pLens = lensRadius * ConcentricSampleDisk(u);
    Point3f pLensWorld = CameraToWorld(ref.time, Point3f(pLens.x, pLens.y, 0));
    Interaction lensIntr(pLensWorld, ref.time, medium);
    lensIntr.n = Normal3f(CameraToWorld(ref.time, Vector3f(0, 0, 1)));

    // Populate arguments and compute the importance value
    *vis = VisibilityTester(ref, lensIntr);
    *wi = lensIntr.p - ref.p;
    Float dist = wi->Length();
    *wi /= dist;

    // Compute PDF for importance arriving at _ref_

    // Compute lens area of perspective camera
    Float lensArea = lensRadius != 0 ? (Pi * lensRadius * lensRadius) : 1;
    *pdf = (dist * dist) / (AbsDot(lensIntr.n, *wi) * lensArea);
    return We(lensIntr.SpawnRay(-*wi), pRaster);
}

PerspectiveCamera *CreatePerspectiveCamera(const ParamSet &params,
                                           const AnimatedTransform &cam2world,
                                           Film *film, const Medium *medium) {
    // Extract common camera parameters from _ParamSet_
    Float shutteropen = params.FindOneFloat("shutteropen", 0.f);
    Float shutterclose = params.FindOneFloat("shutterclose", 1.f);
    if (shutterclose < shutteropen) {
        Warning("Shutter close time [%f] < shutter open [%f].  Swapping them.",
                shutterclose, shutteropen);
        std::swap(shutterclose, shutteropen);
    }

    Float lensradius = params.FindOneFloat("lensradius", 0.f);
    Float focaldistance = params.FindOneFloat("focaldistance", 1e6);

    Float frame = params.FindOneFloat(
        "frameaspectratio",
        Float(film->fullResolution.x) / Float(film->fullResolution.y));

    Bounds2f screen; // screen 默认通过 frame 计算得到
    if (frame > 1.f) {
        // 如果是 1920 * 1080 的分辨率, 则 screen.pMin = (-16/9, -1), .pMax = (16/9, 1)
        screen.pMin.x = -frame;
        screen.pMax.x = frame;
        screen.pMin.y = -1.f;
        screen.pMax.y = 1.f;
    } else {
        screen.pMin.x = -1.f;
        screen.pMax.x = 1.f;
        screen.pMin.y = -1.f / frame;
        screen.pMax.y = 1.f / frame;
    }

    int swi;
    const Float *sw = params.FindFloat("screenwindow", &swi);
    if (sw) {
        if (swi == 4) {
            screen.pMin.x = sw[0];
            screen.pMax.x = sw[1];
            screen.pMin.y = sw[2];
            screen.pMax.y = sw[3];
        } else
            Error("\"screenwindow\" should have four values");
    }
    Float fov = params.FindOneFloat("fov", 90.);
    Float halffov = params.FindOneFloat("halffov", -1.f);
    if (halffov > 0.f)
        // hack for structure synth, which exports half of the full fov
        fov = 2.f * halffov;
    return new PerspectiveCamera(cam2world, screen, shutteropen, shutterclose,
                                 lensradius, focaldistance, fov, film, medium);
}

}  // namespace pbrt
