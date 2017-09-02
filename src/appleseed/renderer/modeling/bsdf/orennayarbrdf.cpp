
//
// This source file is part of appleseed.
// Visit http://appleseedhq.net/ for additional information and resources.
//
// This software is released under the MIT license.
//
// Copyright (c) 2014-2017 Luis B. Barrancos, The appleseedhq Organization
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

// Interface header.
#include "orennayarbrdf.h"

// appleseed.renderer headers.
#include "renderer/kernel/lighting/scatteringmode.h"
#include "renderer/kernel/shading/directshadingcomponents.h"
#include "renderer/modeling/bsdf/bsdf.h"
#include "renderer/modeling/bsdf/bsdfwrapper.h"

// appleseed.foundation headers.
#include "foundation/math/basis.h"
#include "foundation/math/sampling/mappings.h"
#include "foundation/math/scalar.h"
#include "foundation/math/vector.h"
#include "foundation/utility/api/specializedapiarrays.h"
#include "foundation/utility/containers/dictionary.h"

// Standard headers.
#include <algorithm>
#include <cmath>

// Forward declarations.
namespace foundation    { class IAbortSwitch; }
namespace renderer      { class Assembly; }
namespace renderer      { class Project; }

using namespace foundation;
using namespace std;

namespace renderer
{

namespace
{
    //
    // Oren-Nayar BRDF.
    //
    // Reference:
    //
    //   Generalization of Lambert's Reflectance Model
    //   http://www1.cs.columbia.edu/CAVE/publications/pdfs/Oren_SIGGRAPH94.pdf
    //

    const char* Model = "orennayar_brdf";

    class OrenNayarBRDFImpl
      : public BSDF
    {
      public:
        OrenNayarBRDFImpl(
            const char*                 name,
            const ParamArray&           params)
          : BSDF(name, Reflective, ScatteringMode::Diffuse, params)
        {
            m_inputs.declare("reflectance", InputFormatSpectralReflectance);
            m_inputs.declare("reflectance_multiplier", InputFormatFloat, "1.0");
            m_inputs.declare("roughness" , InputFormatFloat, "0.1");
        }

        virtual void release() override
        {
            delete this;
        }

        virtual const char* get_model() const override
        {
            return Model;
        }

        virtual void sample(
            SamplingContext&            sampling_context,
            const void*                 data,
            const bool                  adjoint,
            const bool                  cosine_mult,
            const int                   modes,
            BSDFSample&                 sample) const override
        {
            if (!ScatteringMode::has_diffuse(modes))
                return;

            // Compute the incoming direction in local space.
            sampling_context.split_in_place(2, 1);
            const Vector2f s = sampling_context.next2<Vector2f>();
            const Vector3f wi = sample_hemisphere_cosine(s);

            // Transform the incoming direction to parent space.
            const Vector3f incoming = sample.m_shading_basis.transform_to_parent(wi);

            // Compute the BRDF value.
            const InputValues* values = static_cast<const InputValues*>(data);
            if (values->m_roughness != 0.0f)
            {
                const Vector3f& n = sample.m_shading_basis.get_normal();

                // No reflection below the shading surface.
                const float cos_on = dot(sample.m_outgoing.get_value(), n);
                if (cos_on < 0.0f)
                    return;

                // No reflection below the shading surface.
                const float cos_in = dot(incoming, n);
                if (cos_in < 0.0f)
                    return;

                oren_nayar_qualitative(
                    cos_on,
                    cos_in,
                    values->m_roughness,
                    values->m_reflectance,
                    values->m_reflectance_multiplier,
                    sample.m_outgoing.get_value(),
                    incoming,
                    n,
                    sample.m_value.m_diffuse);
            }
            else
            {
                // Revert to Lambertian when roughness is zero.
                sample.m_value.m_diffuse = values->m_reflectance;
                sample.m_value.m_diffuse *= values->m_reflectance_multiplier * RcpPi<float>();
            }

            sample.m_value.m_beauty = sample.m_value.m_diffuse;

            // Compute the probability density of the sampled direction.
            sample.m_probability = wi.y * RcpPi<float>();
            assert(sample.m_probability > 0.0f);

            // Set the scattering mode.
            sample.m_mode = ScatteringMode::Diffuse;

            sample.m_incoming = Dual3f(incoming);
            sample.compute_reflected_differentials();
        }

        virtual float evaluate(
            const void*                 data,
            const bool                  adjoint,
            const bool                  cosine_mult,
            const Vector3f&             geometric_normal,
            const Basis3f&              shading_basis,
            const Vector3f&             outgoing,
            const Vector3f&             incoming,
            const int                   modes,
            DirectShadingComponents&    value) const override
        {
            if (!ScatteringMode::has_diffuse(modes))
                return 0.0f;

            // No reflection below the shading surface.
            const Vector3f& n = shading_basis.get_normal();
            const float cos_in = dot(incoming, n);
            if (cos_in < 0.0f)
                return 0.0f;

            value.set(0.0f);

            // Compute the BRDF value.
            const InputValues* values = static_cast<const InputValues*>(data);
            if (values->m_roughness != 0.0f)
            {
                // No reflection below the shading surface.
                const float cos_on = dot(outgoing, n);
                if (cos_on < 0.0f)
                    return 0.0f;

                oren_nayar_qualitative(
                    cos_on,
                    cos_in,
                    values->m_roughness,
                    values->m_reflectance,
                    values->m_reflectance_multiplier,
                    outgoing,
                    incoming,
                    n,
                    value.m_diffuse);
            }
            else
            {
                // Revert to Lambertian when roughness is zero.
                value.m_diffuse = values->m_reflectance;
                value.m_diffuse *= values->m_reflectance_multiplier * RcpPi<float>();
            }

            value.m_beauty = value.m_diffuse;

            // Return the probability density of the sampled direction.
            return cos_in * RcpPi<float>();
        }

        virtual float evaluate_pdf(
            const void*                 data,
            const Vector3f&             geometric_normal,
            const Basis3f&              shading_basis,
            const Vector3f&             outgoing,
            const Vector3f&             incoming,
            const int                   modes) const override
        {
            if (!ScatteringMode::has_diffuse(modes))
                return 0.0f;

            // No reflection below the shading surface.
            const Vector3f& n = shading_basis.get_normal();
            const float cos_in = dot(incoming, n);
            if (cos_in < 0.0f)
                return 0.0f;

            const InputValues* values = static_cast<const InputValues*>(data);
            if (values->m_roughness != 0.0f)
            {
                // No reflection below the shading surface.
                const float cos_on = dot(outgoing, n);
                if (cos_on < 0.0f)
                    return 0.0f;
            }

            return cos_in * RcpPi<float>();
        }

      private:
        typedef OrenNayarBRDFInputValues InputValues;

        static void oren_nayar_qualitative(
            const float                 cos_on,
            const float                 cos_in,
            const float                 roughness,
            const Spectrum&             reflectance,
            const float                 reflectance_multiplier,
            const Vector3f&             outgoing,
            const Vector3f&             incoming,
            const Vector3f&             n,
            Spectrum&                   value)
        {
            const float sigma2 = square(roughness);
            const float theta_r = min(acos(cos_on), HalfPi<float>());
            const float theta_i = acos(cos_in);
            const float alpha = max(theta_r, theta_i);
            const float beta = min(theta_r, theta_i);

            // Project outgoing and incoming vectors onto the tangent plane
            // and compute the cosine of the angle between them.
            const Vector3f V_perp_N = normalize(project(outgoing, n));
            const Vector3f I_perp_N = normalize(incoming - n * cos_in);
            const float delta_cos_phi = dot(V_perp_N, I_perp_N);

            // Compute C1 coefficient.
            const float C1 = 1.0f - 0.5f * (sigma2 / (sigma2 + 0.33f));

            // Compute C2 coefficient.
            const float sigma2_009 = sigma2 / (sigma2 + 0.09f);
            const float C2 =
                  0.45f
                * sigma2_009
                * (delta_cos_phi >= 0.0f
                      ? sin(alpha)
                      : sin(alpha) - pow_int<3>(2.0f * beta * RcpPi<float>()));
            assert(C2 >= 0.0f);

            // Compute C3 coefficient.
            const float C3 =
                  0.125f
                * sigma2_009
                * square(4.0f * alpha * beta * RcpPiSquare<float>());
            assert(C3 >= 0.0f);

            // Direct illumination component.
            value = reflectance;
            value *=
                reflectance_multiplier *
                RcpPi<float>() * (
                      C1
                    + delta_cos_phi * C2 * tan(beta)
                    + (1.0f - abs(delta_cos_phi)) * C3 * tan(0.5f * (alpha + beta)));

            // Add interreflection component.
            Spectrum r2 = reflectance;
            r2 *= r2;
            r2 *=
                  0.17f
                * square(reflectance_multiplier) * RcpPi<float>()
                * cos_in
                * sigma2 / (sigma2 + 0.13f)
                * (1.0f - delta_cos_phi * square(2.0f * beta * RcpPi<float>()));
            value += r2;
            clamp_low_in_place(value, 0.0f);
        }
    };

    typedef BSDFWrapper<OrenNayarBRDFImpl> OrenNayarBRDF;
}


//
// OrenNayarBRDFFactory class implementation.
//

const char* OrenNayarBRDFFactory::get_model() const
{
    return Model;
}

Dictionary OrenNayarBRDFFactory::get_model_metadata() const
{
    return
        Dictionary()
            .insert("name", Model)
            .insert("label", "Oren-Nayar BRDF");
}

DictionaryArray OrenNayarBRDFFactory::get_input_metadata() const
{
    DictionaryArray metadata;

    metadata.push_back(
        Dictionary()
            .insert("name", "reflectance")
            .insert("label", "Reflectance")
            .insert("type", "colormap")
            .insert("entity_types",
                Dictionary()
                    .insert("color", "Colors")
                    .insert("texture_instance", "Textures"))
            .insert("use", "required")
            .insert("default", "0.5"));

    metadata.push_back(
        Dictionary()
            .insert("name", "reflectance_multiplier")
            .insert("label", "Reflectance Multiplier")
            .insert("type", "colormap")
            .insert("entity_types",
                Dictionary().insert("texture_instance", "Textures"))
            .insert("use", "optional")
            .insert("default", "1.0"));

    metadata.push_back(
        Dictionary()
            .insert("name", "roughness")
            .insert("label", "Roughness")
            .insert("type", "colormap")
            .insert("entity_types",
                Dictionary().insert("texture_instance", "Textures"))
            .insert("use", "required")
            .insert("default", "0.1"));

    return metadata;
}

auto_release_ptr<BSDF> OrenNayarBRDFFactory::create(
    const char*         name,
    const ParamArray&   params) const
{
    return auto_release_ptr<BSDF>(new OrenNayarBRDF(name, params));
}

auto_release_ptr<BSDF> OrenNayarBRDFFactory::static_create(
    const char*         name,
    const ParamArray&   params)
{
    return auto_release_ptr<BSDF>(new OrenNayarBRDF(name, params));
}

}   // namespace renderer
