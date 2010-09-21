
//
// This source file is part of appleseed.
// Visit http://appleseedhq.net/ for additional information and resources.
//
// This software is released under the MIT license.
//
// Copyright (c) 2010 Francois Beaune
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
#include "voxelaosurfaceshader.h"

// appleseed.renderer headers.
#include "renderer/kernel/shading/ambientocclusion.h"
#include "renderer/kernel/shading/fastambientocclusion.h"
#include "renderer/kernel/shading/shadingcontext.h"
#include "renderer/kernel/shading/shadingpoint.h"
#include "renderer/kernel/shading/shadingresult.h"
#include "renderer/modeling/input/inputarray.h"
#include "renderer/modeling/input/source.h"
#include "renderer/modeling/project/project.h"
#include "renderer/modeling/scene/scene.h"
#include "renderer/modeling/surfaceshader/surfaceshader.h"

// appleseed.foundation headers.
#include "foundation/image/colorspace.h"
#include "foundation/utility/containers/dictionaryarray.h"
#include "foundation/utility/version.h"

// boost headers.
#include "boost/filesystem/convenience.hpp"
#include "boost/filesystem/path.hpp"

using namespace foundation;
using namespace std;

namespace renderer
{

namespace
{
    //
    // Voxel-based ambient occlusion surface shader.
    //

    const char* Model = "voxel_ao_surface_shader";

    class VoxelAOSurfaceShader
      : public SurfaceShader
    {
      public:
        VoxelAOSurfaceShader(
            const char*             name,
            const ParamArray&       params)
          : SurfaceShader(params)
          , m_name(name)
          , m_last_geometry_version_id(InvalidVersionID)
          , m_last_asm_inst_version_id(InvalidVersionID)
        {
            extract_parameters();
        }

        virtual void release()
        {
            delete this;
        }

        virtual const char* get_model() const
        {
            return Model;
        }

        virtual const char* get_name() const
        {
            return m_name.c_str();
        }

        virtual void on_frame_begin(const Project& project)
        {
            const Scene& scene = *project.get_scene();

            // Rebuild the voxel tree if either the scene geometry or the transformations
            // of the assembly instances have changed since the last frame.
            if (scene.get_geometry_version_id() != m_last_geometry_version_id ||
                scene.get_assembly_instances_version_id() != m_last_asm_inst_version_id)
            {
                m_last_geometry_version_id = scene.get_geometry_version_id();
                m_last_asm_inst_version_id = scene.get_assembly_instances_version_id();

                // Build the voxel tree.
                m_voxel_tree.reset(
                    new AOVoxelTree(
                        scene,
                        m_max_voxel_extent));

                // Write the voxel tree to disk, if asked to.
                if (!m_output_filename.empty())
                {
                    m_voxel_tree->dump_solid_leaves_to_disk(m_output_filename);                            
/*
                    const boost::filesystem::path obj_file_path(m_output_filename);
                    const boost::filesystem::path bin_file_path =
                        boost::filesystem::change_extension(obj_file_path, ".bin");
                    m_voxel_tree->dump_tree_to_disk(bin_file_path.file_string());
*/
                }

                // Precompute the world space thresholds.
                m_diag_length = m_voxel_tree->get_max_diag_length() * (1.0 + 1.0e-5);
                m_classic_threshold = m_low_threshold * m_diag_length;
                m_fast_threshold = m_high_threshold * m_diag_length;

                m_half_samples = m_samples / 2;
                if (m_half_samples < 1)
                    m_half_samples = 1;

                // Create the voxel tree intersector.
                m_voxel_tree_intersector.reset(
                    new AOVoxelTreeIntersector(*m_voxel_tree));
            }
        }

        virtual void evaluate(
            SamplingContext&        sampling_context,
            const ShadingContext&   shading_context,
            const ShadingPoint&     shading_point,
            ShadingResult&          shading_result) const
        {
            assert(m_voxel_tree.get());
            assert(m_voxel_tree_intersector.get());

            // Set color space to linear RGB.
            shading_result.m_color_space = ColorSpaceLinearRGB;

            // Set alpha channel to full opacity.
            shading_result.m_alpha = Alpha(1.0);

            const Vector3d& geometric_normal = shading_point.get_geometric_normal();
            Vector3d safe_origin = shading_point.get_point();

            // Find the exit point of the hierarchy along the incoming ray.
            ShadingRay::RayType reverse_ray;
            reverse_ray.m_org = safe_origin;
            reverse_ray.m_dir = -shading_point.get_ray().m_dir;
            reverse_ray.m_tmin = 0.0;
            reverse_ray.m_tmax = shading_point.get_distance();
            double backtrack;
            const bool hit =
                m_voxel_tree_intersector->trace(
                    reverse_ray,
                    false,
                    backtrack);
            assert(hit);
            safe_origin += backtrack * (1.0 + 1.0e-5) * reverse_ray.m_dir;

            // Measure the clearance distance along the geometric normal.
            ShadingRay::RayType normal_ray;
            normal_ray.m_org = safe_origin;
            normal_ray.m_dir = geometric_normal;
            normal_ray.m_tmin = 0.0;
            normal_ray.m_tmax = m_fast_threshold;
            double clearance = m_fast_threshold;
            m_voxel_tree_intersector->trace(normal_ray, true, clearance);

            // Shift the origin along the geometric normal.
            safe_origin += m_diag_length * geometric_normal;

            double occlusion;

            if (clearance >= m_fast_threshold)
            {
                //
                // Clearance is high: use fast mode.
                //

                // Compute fast ambient occlusion.
                double min_distance;
                occlusion =
                    compute_fast_ambient_occlusion(
                        sampling_context,
                        *m_voxel_tree_intersector,
                        safe_origin,
                        shading_point.get_geometric_normal(),
                        shading_point.get_shading_basis(),
                        m_max_distance,
                        m_samples,
                        min_distance);

                if (m_enable_diagnostics)
                {
                    shading_result.m_color[0] = 0.0f;
                    shading_result.m_color[1] = 0.0f;
                    shading_result.m_color[2] = 1.0f;
                    return;
                }
            }
            else if (clearance < m_classic_threshold)
            {
                //
                // Clearance is low: use classic mode.
                //

                // Compute classic ambient occlusion.
                occlusion =
                    compute_ambient_occlusion(
                        sampling_context,
                        shading_context.get_intersector(),
                        shading_point.get_point(),
                        shading_point.get_geometric_normal(),
                        shading_point.get_shading_basis(),
                        m_max_distance,
                        m_samples,
                        &shading_point);

                if (m_enable_diagnostics)
                {
                    shading_result.m_color[0] = 1.0f;
                    shading_result.m_color[1] = 1.0f;
                    shading_result.m_color[2] = 0.0f;
                    return;
                }
            }
            else
            {
                //
                // Clearance is intermediate: blend between classic and fast modes.
                //

                // Compute classic ambient occlusion.
                const double classic_occlusion =
                    compute_ambient_occlusion(
                        sampling_context,
                        shading_context.get_intersector(),
                        shading_point.get_point(),
                        shading_point.get_geometric_normal(),
                        shading_point.get_shading_basis(),
                        m_max_distance,
                        m_half_samples,
                        &shading_point);

                // Compute fast ambient occlusion.
                double min_distance;
                const double fast_occlusion =
                    compute_fast_ambient_occlusion(
                        sampling_context,
                        *m_voxel_tree_intersector,
                        safe_origin,
                        shading_point.get_geometric_normal(),
                        shading_point.get_shading_basis(),
                        m_max_distance,
                        m_half_samples,
                        min_distance);

                // Linear interpolate between classic and fast occlusion values.
                // Using smoothstep() doesn't improve significantly the results.
                const double k =
                    linearstep(
                        m_classic_threshold,
                        m_fast_threshold,
                        clearance);
                occlusion = k * fast_occlusion + (1.0 - k) * classic_occlusion;

                if (m_enable_diagnostics)
                {
                    shading_result.m_color[0] = 1.0f - static_cast<float>(k);
                    shading_result.m_color[1] = 0.0f;
                    shading_result.m_color[2] = static_cast<float>(k);
                    return;
                }
            }

            // Return a gray scale value proportional to the accessibility.
            const float accessibility = static_cast<float>(1.0 - occlusion);
            shading_result.m_color[0] = accessibility;
            shading_result.m_color[1] = accessibility;
            shading_result.m_color[2] = accessibility;
        }

      private:
        const string                        m_name;

        size_t                              m_samples;
        double                              m_max_distance;
        GScalar                             m_max_voxel_extent;
        double                              m_low_threshold;
        double                              m_high_threshold;
        string                              m_output_filename;
        bool                                m_enable_diagnostics;

        VersionID                           m_last_geometry_version_id;
        VersionID                           m_last_asm_inst_version_id;
        auto_ptr<AOVoxelTree>               m_voxel_tree;
        auto_ptr<AOVoxelTreeIntersector>    m_voxel_tree_intersector;
        double                              m_diag_length;
        double                              m_classic_threshold;
        double                              m_fast_threshold;
        size_t                              m_half_samples;

        void extract_parameters()
        {
            const double DefaultLowThreshold = 2.0;
            const double DefaultHighThreshold = 4.0;

            m_samples = m_params.get_required<size_t>("samples", 16);
            m_max_distance = m_params.get_required<double>("max_distance", 1.0);
            m_max_voxel_extent = m_params.get_required<GScalar>("max_voxel_extent", GScalar(0.01));
            m_low_threshold = m_params.get_optional<double>("low_threshold", DefaultLowThreshold);
            m_high_threshold = m_params.get_optional<double>("high_threshold", DefaultHighThreshold);
            m_output_filename = m_params.get_optional<string>("output_filename", "");
            m_enable_diagnostics = m_params.get_optional<bool>("enable_diagnostics", false);

            if (m_low_threshold < 0.0 ||
                m_high_threshold < 0.0 ||
                m_high_threshold < m_low_threshold)
            {
                RENDERER_LOG_ERROR(
                    "invalid low and high threshold values, switching back to default %f and %f",
                    DefaultLowThreshold,
                    DefaultHighThreshold);

                m_low_threshold = DefaultLowThreshold;
                m_high_threshold = DefaultHighThreshold;
            }
        }
    };
}


//
// VoxelAOSurfaceShaderFactory class implementation.
//

const char* VoxelAOSurfaceShaderFactory::get_model() const
{
    return Model;
}

const char* VoxelAOSurfaceShaderFactory::get_human_readable_model() const
{
    return "Voxel-Based Ambient Occlusion (experimental)";
}

DictionaryArray VoxelAOSurfaceShaderFactory::get_widget_definitions() const
{
    DictionaryArray definitions;
    return definitions;
}

auto_release_ptr<SurfaceShader> VoxelAOSurfaceShaderFactory::create(
    const char*         name,
    const ParamArray&   params) const
{
    return
        auto_release_ptr<SurfaceShader>(
            new VoxelAOSurfaceShader(name, params));
}

}   // namespace renderer
