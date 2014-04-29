#ifndef MESH_HPP_
#define MESH_HPP_

#include "Triangle.hpp"
#include "Vertex.hpp"

#include "sampling/Distribution1D.hpp"
#include "sampling/Sample.hpp"

#include "math/TangentFrame.hpp"
#include "math/Mat4f.hpp"
#include "math/Vec.hpp"
#include "math/Box.hpp"

#include "io/JsonSerializable.hpp"

#include "EmbreeUtil.hpp"
#include "Primitive.hpp"

#include <embree/include/embree.h>
#include <rapidjson/document.h>
#include <iostream>
#include <memory>
#include <vector>
#include <string>

namespace Tungsten
{

class Scene;

class TriangleMesh : public Primitive
{
    struct MeshIntersection
    {
        Vec3f Ng;
        Vec3f p;
        float u;
        float v;
        int id0;
        int id1;
        bool backSide;
    };

    std::string _path;
    bool _dirty;
    bool _smoothed;

    std::vector<Vertex> _verts;
    std::vector<Vertex> _tfVerts;
    std::vector<TriangleI> _tris;

    std::unique_ptr<Distribution1D> _triSampler;
    float _totalArea;

    Box3f _bounds;

    embree::RTCGeometry *_geom = nullptr;
    embree::RTCIntersector1 *_intersector = nullptr;

    Vec3f normalAt(int triangle, float u, float v) const
    {
        const TriangleI &t = _tris[triangle];
        Vec3f n0 = _tfVerts[t.v0].normal();
        Vec3f n1 = _tfVerts[t.v1].normal();
        Vec3f n2 = _tfVerts[t.v2].normal();
        return ((1.0f - u - v)*n0 + u*n1 + v*n2).normalized();
    }

    Vec2f uvAt(int triangle, float u, float v) const
    {
        const TriangleI &t = _tris[triangle];
        Vec2f uv0 = _tfVerts[t.v0].uv();
        Vec2f uv1 = _tfVerts[t.v1].uv();
        Vec2f uv2 = _tfVerts[t.v2].uv();
        return (1.0f - u - v)*uv0 + u*uv1 + v*uv2;
    }

public:
    TriangleMesh()
    : _dirty(false),
      _smoothed(false)
    {
    }

    TriangleMesh(const TriangleMesh &o)
    : Primitive(o),
      _path(o._path),
      _dirty(true),
      _smoothed(o._smoothed),
      _verts(o._verts),
      _tris(o._tris),
      _bounds(o._bounds)
    {
    }

    TriangleMesh(std::vector<Vertex> verts, std::vector<TriangleI> tris,
                 const std::shared_ptr<Bsdf> &bsdf,
                 const std::string &name, bool smoothed)
    : Primitive(name, bsdf),
      _path(std::string(name).append(".wo3")),
      _dirty(true),
      _smoothed(smoothed),
      _verts(std::move(verts)),
      _tris(std::move(tris))
    {
    }

    void fromJson(const rapidjson::Value &v, const Scene &scene) override;
    rapidjson::Value toJson(Allocator &allocator) const override;

    void saveData() const override;
    void saveAsObj(std::ostream &out) const;
    void calcSmoothVertexNormals();
    void computeBounds();

    virtual bool intersect(Ray &ray, IntersectionTemporary &data) const override
    {
        embree::Ray eRay(toERay(ray));
        _intersector->intersect(eRay);
        if (eRay && eRay.tfar < ray.farT()) {
            ray.setFarT(eRay.tfar);

            data.primitive = this;
            MeshIntersection *isect = data.as<MeshIntersection>();
            isect->Ng = fromE(eRay.Ng);
            isect->p = fromE(eRay.org + eRay.dir*eRay.tfar);
            isect->u = eRay.u;
            isect->v = eRay.v;
            isect->id0 = eRay.id0;
            isect->id1 = eRay.id1;
            isect->backSide = isect->Ng.dot(ray.dir()) > 0.0f;

            return true;
        }
        return false;
    }

    virtual bool occluded(const Ray &ray) const override
    {
        embree::Ray eRay(toERay(ray));
        return _intersector->occluded(eRay);
    }

    virtual void intersectionInfo(const IntersectionTemporary &data, IntersectionInfo &info) const override
    {
        const MeshIntersection *isect = data.as<MeshIntersection>();
        info.Ng = -isect->Ng.normalized();
        if (_smoothed)
            info.Ns = normalAt(isect->id0, isect->u, isect->v);
        else
            info.Ns = info.Ng;
        info.uv = uvAt(isect->id0, isect->u, isect->v);
        info.primitive = this;
        info.p = isect->p;
    }

    virtual bool hitBackside(const IntersectionTemporary &data) const
    {
        return data.as<MeshIntersection>()->backSide;
    }

    virtual bool tangentSpace(const IntersectionTemporary &data, const IntersectionInfo &/*info*/, Vec3f &T, Vec3f &B) const override
    {
        const MeshIntersection *isect = data.as<MeshIntersection>();
        const TriangleI &t = _tris[isect->id0];
        Vec3f p0 = _tfVerts[t.v0].pos();
        Vec3f p1 = _tfVerts[t.v1].pos();
        Vec3f p2 = _tfVerts[t.v2].pos();
        Vec2f uv0 = _tfVerts[t.v0].uv();
        Vec2f uv1 = _tfVerts[t.v1].uv();
        Vec2f uv2 = _tfVerts[t.v2].uv();
        Vec3f q1 = p1 - p0;
        Vec3f q2 = p2 - p0;
        float s1 = uv1.x() - uv0.x(), t1 = uv1.y() - uv0.y();
        float s2 = uv2.x() - uv0.x(), t2 = uv2.y() - uv0.y();
        float invDet = s1*t2 - s2*t1;
        if (std::abs(invDet) < 1e-4)
            return false;
        float det = 1.0f/invDet;
        T = det*(q1*t2 - t1*q2);
        B = det*(q2*s1 - s2*q1);

        return true;
    }

    virtual Box3f bounds() const
    {
        return _bounds;
    }

    virtual const TriangleMesh &asTriangleMesh() final override
    {
        return *this;
    }

    virtual void prepareForRender() override
    {
        computeBounds();

        _geom = embree::rtcNewTriangleMesh(_tris.size(), _verts.size(), "bvh2");
        embree::RTCVertex   *vs = embree::rtcMapPositionBuffer(_geom);
        embree::RTCTriangle *ts = embree::rtcMapTriangleBuffer(_geom);

        for (size_t i = 0; i < _tris.size(); ++i) {
            const TriangleI &t = _tris[i];
            ts[i] = embree::RTCTriangle(t.v0, t.v1, t.v2, i, 0);
        }

        _tfVerts.resize(_verts.size());
        Mat4f normalTform(_transform.toNormalMatrix());
        for (size_t i = 0; i < _verts.size(); ++i) {
            _tfVerts[i] = Vertex(
                _transform*_verts[i].pos(),
                normalTform.transformVector(_verts[i].normal()),
                _verts[i].uv()
            );
            const Vec3f &p = _tfVerts[i].pos();
            vs[i] = embree::RTCVertex(p.x(), p.y(), p.z());
        }

        _totalArea = 0.0f;
        for (size_t i = 0; i < _tris.size(); ++i) {
            Vec3f p0 = _tfVerts[_tris[i].v0].pos();
            Vec3f p1 = _tfVerts[_tris[i].v1].pos();
            Vec3f p2 = _tfVerts[_tris[i].v2].pos();
            _totalArea += MathUtil::triangleArea(p0, p1, p2);
        }

        embree::rtcUnmapPositionBuffer(_geom);
        embree::rtcUnmapTriangleBuffer(_geom);

        embree::rtcBuildAccel(_geom, "objectsplit");
        _intersector = embree::rtcQueryIntersector1(_geom, "fast.moeller");
    }

    virtual void cleanupAfterRender() override
    {
        if (_geom)
            embree::rtcDeleteGeometry(_geom);
        _geom = nullptr;
        _intersector = nullptr;
        _tfVerts.clear();
    }

    float area() const override
    {
        return _totalArea;
    }

    bool isSamplable() const override final
    {
        return _triSampler.operator bool();
    }

    void makeSamplable() override final
    {
        std::vector<float> areas(_tris.size());
        _totalArea = 0.0f;
        for (size_t i = 0; i < _tris.size(); ++i) {
            Vec3f p0 = _tfVerts[_tris[i].v0].pos();
            Vec3f p1 = _tfVerts[_tris[i].v1].pos();
            Vec3f p2 = _tfVerts[_tris[i].v2].pos();
            areas[i] = MathUtil::triangleArea(p0, p1, p2);
            _totalArea += areas[i];
        }
        _triSampler.reset(new Distribution1D(std::move(areas)));
    }

    float inboundPdf(const IntersectionTemporary &data, const Vec3f &p, const Vec3f &d) const override final
    {
        const MeshIntersection *isect = data.as<MeshIntersection>();

        return (p - isect->p).lengthSq()/(-d.dot(isect->Ng.normalized())*_totalArea);
    }

    bool sampleInboundDirection(LightSample &sample) const override final
    {
        float u = sample.sampler.next1D();
        int idx;
        _triSampler->warp(u, idx);

        Vec3f p0 = _tfVerts[_tris[idx].v0].pos();
        Vec3f p1 = _tfVerts[_tris[idx].v1].pos();
        Vec3f p2 = _tfVerts[_tris[idx].v2].pos();
        Vec3f normal = (p1 - p0).cross(p2 - p0).normalized();

        Vec3f p = Sample::uniformTriangle(sample.sampler.next2D(), p0, p1, p2);
        Vec3f L = p - sample.p;

        float rSq = L.lengthSq();
        sample.dist = std::sqrt(rSq);
        sample.d = L/sample.dist;
        float cosTheta = -(normal.dot(sample.d));
        if (cosTheta <= 0.0f)
            return false;
        sample.pdf = rSq/(cosTheta*_totalArea);

        return true;
    }

    bool sampleOutboundDirection(LightSample &sample) const override final
    {
        float u = sample.sampler.next1D();
        int idx;
        _triSampler->warp(u, idx);

        Vec3f p0 = _tfVerts[_tris[idx].v0].pos();
        Vec3f p1 = _tfVerts[_tris[idx].v1].pos();
        Vec3f p2 = _tfVerts[_tris[idx].v2].pos();
        Vec3f normal = (p1 - p0).cross(p2 - p0).normalized();
        TangentFrame frame(normal);

        sample.p = Sample::uniformTriangle(sample.sampler.next2D(), p0, p1, p2);
        sample.d = Sample::cosineHemisphere(sample.sampler.next2D());
        sample.pdf = Sample::cosineHemispherePdf(sample.d)/_totalArea;
        sample.d = frame.toGlobal(sample.d);

        return true;
    }

    virtual bool invertParametrization(Vec2f /*uv*/, Vec3f &/*pos*/) const
    {
        return false;
    }

    virtual bool isDelta() const
    {
        return false;
    }

    virtual Primitive *clone()
    {
        return new TriangleMesh(*this);
    }


    const std::vector<TriangleI>& tris() const
    {
        return _tris;
    }

    const std::vector<Vertex>& verts() const
    {
        return _verts;
    }

    std::vector<TriangleI>& tris()
    {
        return _tris;
    }

    std::vector<Vertex>& verts()
    {
        return _verts;
    }

    bool smoothed() const
    {
        return _smoothed;
    }

    void setSmoothed(bool v)
    {
        _smoothed = v;
    }

    bool isDirty() const
    {
        return _dirty;
    }

    void markDirty()
    {
        _dirty = true;
    }

    const std::string& path() const
    {
        return _path;
    }
};

}

#endif /* MESH_HPP_ */
