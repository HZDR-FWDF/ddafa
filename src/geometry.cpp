/*
 * This file is part of the PARIS reconstruction program.
 *
 * Copyright (C) 2016 Helmholtz-Zentrum Dresden-Rossendorf
 *
 * PARIS is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * PARIS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with PARIS. If not, see <http://www.gnu.org/licenses/>.
 *
 * Date: 27 October 2016
 * Authors: Jan Stephan <j.stephan@hzdr.de>
 */

#include <cstdint>
#include <cmath>
#include <iomanip>

#include <boost/log/trivial.hpp>

#include "geometry.h"
#include "region_of_interest.h"

namespace paris
{
    namespace
    {
        auto make_volume_geometry(const detector_geometry& det_geo) noexcept -> volume_geometry
        {
            auto vol_geo = volume_geometry{};

            /* Casts and removing some boilerplate code from the calculations below*/
            const auto n_row = static_cast<float>(det_geo.n_row);
            const auto l_px_row = det_geo.l_px_row;
            const auto delta_s = std::abs(det_geo.delta_s * l_px_row); // the offset in det_geo is measured in pixels!

            const auto n_col = static_cast<float>(det_geo.n_col);
            const auto l_px_col = det_geo.l_px_col;
            const auto delta_t = std::abs(det_geo.delta_t * l_px_col);

            const auto d_so = std::abs(det_geo.d_so);
            const auto d_sd = std::abs(det_geo.d_od) + d_so;

            /* Calculate slice dimensions */
            const auto alpha = std::atan((((n_row * l_px_row) / 2.f) + delta_s) / d_sd);
            const auto r = d_so * std::sin(alpha);

            vol_geo.l_vx_x = r / ((((n_row * l_px_row) / 2.f) + delta_s) / l_px_row);
            vol_geo.l_vx_y = vol_geo.l_vx_x;

            vol_geo.dim_x = static_cast<std::uint32_t>((2.f * r) / vol_geo.l_vx_x);
            vol_geo.dim_y = vol_geo.dim_x;

            /* Calculate number of slices */
            vol_geo.l_vx_z = vol_geo.l_vx_x;
            vol_geo.dim_z = static_cast<std::uint32_t>(((n_col * l_px_col / 2.f) + delta_t) * (d_so / d_sd) * (2.f / vol_geo.l_vx_z));

            return vol_geo;
        }

    }

    auto calculate_volume_geometry(const detector_geometry& det_geo) noexcept -> volume_geometry
    {
        auto vol_geo = make_volume_geometry(det_geo);

        const auto dim_x_mm = static_cast<float>(vol_geo.dim_x) * vol_geo.l_vx_x;
        const auto dim_y_mm = static_cast<float>(vol_geo.dim_y) * vol_geo.l_vx_y;
        const auto dim_z_mm = static_cast<float>(vol_geo.dim_z) * vol_geo.l_vx_z;

        BOOST_LOG_TRIVIAL(info) << "Volume dimensions [vx]: " << vol_geo.dim_x << " x " << vol_geo.dim_y << " x " << vol_geo.dim_z;
        BOOST_LOG_TRIVIAL(info) << "Volume dimensions [mm]: " << dim_x_mm << " x " << dim_y_mm  << " x " << dim_z_mm;
        BOOST_LOG_TRIVIAL(info) << "Voxel size [mm]: " << std::setprecision(4) << vol_geo.l_vx_x << " x " << vol_geo.l_vx_y << " x " << vol_geo.l_vx_z;

        return vol_geo;
    }

    auto apply_roi(const volume_geometry& vol_geo,
                    std::uint32_t x1, std::uint32_t x2,
                    std::uint32_t y1, std::uint32_t y2,
                    std::uint32_t z1, std::uint32_t z2) noexcept -> volume_geometry
    {
        auto roi_geo = vol_geo;

        auto check_coords = [](std::uint32_t low, std::uint32_t high) { return low < high; };
        auto check_dims = [](std::uint32_t updated, std::uint32_t old) { return updated <= old; };

        if(check_coords(x1, x2) && check_coords(y1, y2) && check_coords(z1, z2))
        {
            auto dim_x = x2 - x1;
            auto dim_y = y2 - y1;
            auto dim_z = z2 - z1;

            if(x1 == 0)
                ++dim_x;
            if(y1 == 0)
                ++dim_y;
            if(z1 == 0)
                ++dim_z;

            if(check_dims(dim_x, vol_geo.dim_x) && check_dims(dim_y, vol_geo.dim_y) && check_dims(dim_z, vol_geo.dim_z))
            {
                roi_geo.dim_x = dim_x;
                roi_geo.dim_y = dim_y;
                roi_geo.dim_z = dim_z;

                const auto dim_x_mm = static_cast<float>(roi_geo.dim_x) * roi_geo.l_vx_x;
                const auto dim_y_mm = static_cast<float>(roi_geo.dim_y) * roi_geo.l_vx_y;
                const auto dim_z_mm = static_cast<float>(roi_geo.dim_z) * roi_geo.l_vx_z;

                BOOST_LOG_TRIVIAL(info) << "Applied region of interest.";
                BOOST_LOG_TRIVIAL(info) << "Updated volume dimensions [vx]: " << roi_geo.dim_x << " x " << roi_geo.dim_y << " x " << roi_geo.dim_z;
                BOOST_LOG_TRIVIAL(info) << "Updated volume dimensions [mm]: " << dim_x_mm << " x " << dim_y_mm  << " x " << dim_z_mm;
            }
            else
                BOOST_LOG_TRIVIAL(warning) << "New volume dimensions exceed old volume dimensions. ROI NOT applied.";
        }
        else
            BOOST_LOG_TRIVIAL(warning) << "Invalid ROI coordinates. ROI NOT applied.";

        return roi_geo;
    }
}


