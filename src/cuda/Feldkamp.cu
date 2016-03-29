#include <array>
#include <cstddef>
#include <cmath>
#include <fstream>
#include <locale>
#include <mutex>
#include <thread>
#include <string>
#include <utility>
#include <vector>

#include <boost/log/trivial.hpp>

#include <ddrf/Image.h>
#include <ddrf/cuda/Check.h>
#include <ddrf/cuda/Coordinates.h>
#include <ddrf/cuda/Launch.h>

#include "../common/Geometry.h"
#include "Feldkamp.h"
#include "FeldkampScheduler.h"

namespace ddafa
{
	namespace cuda
	{
		__global__ void init_volume(float* vol, std::size_t width, std::size_t height, std::size_t depth, std::size_t pitch)
		{
			auto x = ddrf::cuda::getX();
			auto y = ddrf::cuda::getY();
			auto z = ddrf::cuda::getZ();

			if((x < width) && (y < height) && (z < depth))
			{
				auto slice_pitch = pitch * height;
				auto slice = reinterpret_cast<char*>(vol) + z * slice_pitch;
				auto row = reinterpret_cast<float*>(slice + y * pitch);

				row[x] = 0.f;
			}
		}

		__global__ void check_volume(const float* vol, std::size_t width, std::size_t height, std::size_t depth, std::size_t pitch)
		{
			auto x = ddrf::cuda::getX();
			auto y = ddrf::cuda::getY();
			auto z = ddrf::cuda::getZ();

			if((x < width) && (y < height) && (z < depth))
			{
				auto slice_pitch = pitch * height;
				auto slice = reinterpret_cast<const char*>(vol) + z * slice_pitch;
				auto row = reinterpret_cast<const float*>(slice + y * pitch);

				if(x == 531 && y == 531 && z == 106)
				{
					printf("(531, 531, 106): %f\n", row[x]);
				}
			}
		}

		inline __device__ auto vol_centered_coordinate(unsigned int coord, std::size_t dim, float size) -> float
		{
			auto size2 = size / 2.f;
			return -(dim * size2) + size2 + coord * size;
		}

		inline __device__ auto proj_centered_coordinate(unsigned int coord, std::size_t dim, float size, float offset) -> float
		{
			auto size2 = size / 2.f;
			return size2 + coord * size + (-(dim * size2) - offset);
		}

		// round and cast as needed
		inline __device__ auto proj_real_coordinate(float coord, std::size_t dim, float size, float offset) -> float
		{
			auto size2 = size / 2.f;
			auto min = -(dim * size2) - offset;
			return (coord - size2 - min) / size;

		}

		template <class T>
		inline __device__ auto as_unsigned(T x) -> unsigned int
		{
			return static_cast<unsigned int>(x);
		}

		__device__ auto interpolate(float h, float v, const float* proj, std::size_t proj_width, std::size_t proj_height, std::size_t proj_pitch,
									float pixel_size_x, float pixel_size_y, float offset_x, float offset_y)
		-> float
		{
			auto k = ddrf::cuda::getX();
			auto l = ddrf::cuda::getY();
			auto m = ddrf::cuda::getZ();

			auto h_real = proj_real_coordinate(h, proj_width, pixel_size_x, offset_x);
			auto v_real = proj_real_coordinate(v, proj_height, pixel_size_y, offset_y);

			auto h_j0 = floorf(h_real);
			auto h_j1 = ceilf(h_real);
			auto v_i0 = floorf(v_real);
			auto v_i1 = ceilf(v_real);

			auto w_h0 = (h_real - h_j0) / (h_j1 - h_j0);
			auto w_v0 = (v_real - v_i0) / (v_i1 - v_i0);

			auto h_j0_ui = as_unsigned(h_j0);
			auto h_j1_ui = as_unsigned(h_j1);
			auto v_i0_ui = as_unsigned(v_i0);
			auto v_i1_ui = as_unsigned(v_i1);

			auto upper_row = reinterpret_cast<const float*>(reinterpret_cast<const char*>(proj) + v_i0_ui * proj_pitch);
			auto lower_row = reinterpret_cast<const float*>(reinterpret_cast<const char*>(proj) + v_i1_ui * proj_pitch);

			auto w_h1 = 1.f - w_h0;
			auto w_v1 = 1.f - w_v0;

			// bounds checking -- there has to be a more efficient way
			auto h_j0_valid = (h_j0 >= 0.f) && (h_j0 < static_cast<float>(proj_width));
			auto h_j1_valid = (h_j1 >= 0.f) && (h_j1 < static_cast<float>(proj_width));
			auto v_i0_valid = (v_i0 >= 0.f) && (v_i0 < static_cast<float>(proj_height));
			auto v_i1_valid = (v_i1 >= 0.f) && (v_i1 < static_cast<float>(proj_height));

			auto tl = 0.f;
			auto bl = 0.f;
			if(h_j0_valid)
			{
				if(v_i0_valid)
					tl = upper_row[h_j0_ui];
				if(v_i1_valid)
					bl = lower_row[h_j0_ui];
			}

			auto tr = 0.f;
			auto br = 0.f;
			if(h_j1_valid)
			{
				if(v_i0_valid)
					tr = upper_row[h_j1_ui];
				if(v_i1_valid)
					br = lower_row[h_j1_ui];
			}

			auto val = 	w_h1	* w_v1	* tl +
						w_h1	* w_v0	* bl +
						w_h0	* w_v1	* tr +
						w_h0	* w_v0	* br;

			return val;
		}

		__global__ void backproject(float* __restrict__ vol, std::size_t vol_w, std::size_t vol_h, std::size_t vol_d, std::size_t vol_pitch,
									float voxel_size_x, float voxel_size_y, float voxel_size_z,
									const float* __restrict__ proj, std::size_t proj_w, std::size_t proj_h, std::size_t proj_pitch,
									float pixel_size_x, float pixel_size_y, float pixel_offset_x, float pixel_offset_y,
									float angle_sin, float angle_cos, float dist_src, float dist_sd)
		{
			auto k = ddrf::cuda::getX();
			auto l = ddrf::cuda::getY();
			auto m = ddrf::cuda::getZ();

			if((k < vol_w) && (l < vol_h) && (m < vol_d))
			{
				auto slice_pitch = vol_pitch * vol_h;
				auto slice = reinterpret_cast<char*>(vol) + m * slice_pitch;
				auto row = reinterpret_cast<float*>(slice + l * vol_pitch);

				// get centered coordinates -- volume center is at (0, 0, 0)
				auto x_k = vol_centered_coordinate(k, vol_w, voxel_size_x);
				auto y_l = vol_centered_coordinate(l, vol_h, voxel_size_y);
				auto z_m = vol_centered_coordinate(m, vol_d, voxel_size_z);

				// rotate coordinates
				auto s = x_k * angle_cos + y_l * angle_sin;
				auto t = -x_k * angle_sin + y_l * angle_cos;
				auto z = z_m;

				// project rotated coordinates
				auto factor = dist_sd / (s - dist_src);
				auto h = t * factor;
				auto v = z * factor;

				// get projection value by interpolation
				auto det = interpolate(h, v, proj, proj_w, proj_h, proj_pitch, pixel_size_x, pixel_size_y, pixel_offset_x, pixel_offset_y);
				// auto det = 0.f;

				// backproject
				auto u = dist_src / (s - dist_src);
				row[k] += 0.5f * det * powf(u, 2.f);

				/*if(k == 531 && l == 531 && m == 106)
				{
					printf("det: %f\n", det);
					printf("u: %f\n", u);
					printf("(531, 531, 106): %f\n", row[k]);
				}

				__syncthreads();*/
			}
		}


		Feldkamp::Feldkamp(const common::Geometry& geo, const std::string& angles)
		: scheduler_{FeldkampScheduler<float>::instance(geo)}, done_{false}
		, geo_(geo), dist_sd_{geo_.dist_det + geo_.dist_src}, vol_geo_(scheduler_.volume_geometry())
		, input_num_{0u}, input_num_set_{false}, current_img_{0u}, current_angle_{0.f}
		{
			if(!angles.empty())
				parse_angles(angles);

			CHECK(cudaGetDeviceCount(&devices_));
			std::vector<std::thread> creation_threads;

			for(auto i = 0; i < devices_; ++i)
			{
				creation_threads.emplace_back(&Feldkamp::create_volumes, this, i);
				processor_threads_[i] = std::thread{&Feldkamp::processor, this, i};
			}

			for(auto&& t : creation_threads)
				t.join();
		}

		Feldkamp::~Feldkamp()
		{
			// this is the last stage in the pipeline, time to clean up CUDA
			cudaDeviceReset();
		}

		auto Feldkamp::parse_angles(const std::string& angles) -> void
		{
			auto&& file = std::ifstream{angles.c_str()};
			if(!file.is_open())
			{
				BOOST_LOG_TRIVIAL(warning) << "cuda::Feldkamp: Could not open angle file at " << angles << ", using default values.";
				return;
			}

			auto angle_string = std::string{""};
			std::getline(file, angle_string);

			auto loc = std::locale{};
			if(angle_string.find(',') != std::string::npos)
				loc = std::locale("de_DE.UTF-8");

			file.seekg(0, std::ios_base::beg);
			file.imbue(loc);

			while(file.tellg() != std::ios_base::end)
			{
				auto angle = 0.f;
				file >> angle;
				sin_tab_.emplace_back(std::sin(angle * M_PI / 180.f));
				cos_tab_.emplace_back(std::cos(angle * M_PI / 180.f));
			}

			angle_tabs_created_ = true;
		}

		auto Feldkamp::process(input_type&& img) -> void
		{
			if(img.valid())
				map_imgs_[img.device()].push(std::move(img));
			else
			{
				BOOST_LOG_TRIVIAL(debug) << "cuda::Feldkamp: Received poisonous pill, finishing...";
				for(auto i = 0; i < devices_; ++i)
					map_imgs_[i].push(input_type());

				for(auto i = 0; i < devices_; ++i)
					processor_threads_[i].join();

				merge_volumes();
				results_.push(output_type());
				done_ = true;
				BOOST_LOG_TRIVIAL(info) << "cuda::Feldkamp: Done.";
			}
		}

		auto Feldkamp::wait() -> output_type
		{
			while(!done_)
				std::this_thread::yield();

			return results_.take();
		}

		auto Feldkamp::processor(int device) -> void
		{
			CHECK(cudaSetDevice(device));
			while(true)
			{
				auto img = map_imgs_[device].take();
				if(!img.valid())
					break;

				BOOST_LOG_TRIVIAL(debug) << "cuda::Feldkamp: Processing image #" << img.index() << " on device #" << device;

				while(!input_num_set_)
					std::this_thread::yield();

				if(!angle_tabs_created_)
				{
					std::call_once(angle_flag_, [&](){
						sin_tab_.resize(input_num_);
						cos_tab_.resize(input_num_);

						auto angle_step = geo_.rot_angle;

						auto angle = 0.f;
						for(auto i = 0u; i < input_num_; ++i, angle += angle_step)
						{
							BOOST_LOG_TRIVIAL(debug) << "Creating angle tab entry for angle " << angle;
							auto angle_rad = static_cast<float>(angle * M_PI / 180.f);
							sin_tab_[i] = std::sin(angle_rad);
							BOOST_LOG_TRIVIAL(debug) << "Sin: " << sin_tab_[i];
							cos_tab_[i] = std::cos(angle_rad);
							BOOST_LOG_TRIVIAL(debug) << "Cos: " << cos_tab_[i];
						}
						angle_tabs_created_ = true;
					});
				}

				auto& volumes = volume_map_[device];
				for(auto& v : volumes) // FIXME: Fix this after testing
				{
					ddrf::cuda::launch(v.width(), v.height(), v.depth(),
										backproject,
										v.data(), v.width(), v.height(), v.depth(), v.pitch(),
										vol_geo_.voxel_size_x, vol_geo_.voxel_size_y, vol_geo_.voxel_size_z,
										static_cast<const float*>(img.data()), img.width(), img.height(), img.pitch(),
										geo_.det_pixel_size_horiz, geo_.det_pixel_size_vert, geo_.det_offset_horiz, geo_.det_offset_vert,
										sin_tab_[img.index()], cos_tab_[img.index()], geo_.dist_src, dist_sd_);
				}
			}

		}

		auto Feldkamp::set_input_num(std::uint32_t num) noexcept -> void
		{
			input_num_ = num;
			input_num_set_ = true;
		}

		auto Feldkamp::create_volumes(int device) -> void
		{
			BOOST_LOG_TRIVIAL(debug) << "cuda::Feldkamp: Creating volumes on device #" << device;
			CHECK(cudaSetDevice(device));

			auto vol_dev_size = vol_geo_.dim_z / static_cast<std::size_t>(devices_);

			auto vol_on_device = scheduler_.volumes_per_device()[device];
			for(auto i = 0u; i < vol_on_device; ++i)
			{
				auto subvol_dev_size = vol_dev_size / vol_on_device;
				auto ptr = ddrf::cuda::make_device_ptr<float>(vol_geo_.dim_x, vol_geo_.dim_y, subvol_dev_size);
				BOOST_LOG_TRIVIAL(debug) << "cuda::Feldkamp: Creating " << vol_geo_.dim_x << "x" << vol_geo_.dim_y << "x" << subvol_dev_size << " volume on device #" << device;
				ddrf::cuda::launch(ptr.width(), ptr.height(), ptr.depth(),
									init_volume,
									ptr.get(), ptr.width(), ptr.height(), ptr.depth(), ptr.pitch());

				volume_map_[device].emplace_back(ptr.width(), ptr.height(), ptr.depth(), std::move(ptr));
			}
		}

		auto Feldkamp::merge_volumes() -> void
		{
			// FIXME: The following code (and the scheduler itself) is absolutely hideous. Replace ASAP.
			BOOST_LOG_TRIVIAL(debug) << "cuda::Feldkamp: Merging volumes";
			auto output = output_type{vol_geo_.dim_x, vol_geo_.dim_y, vol_geo_.dim_z};
			for(auto i = 0; i < devices_; ++i)
			{
				CHECK(cudaSetDevice(i));
				auto vol_dev_size = output.depth() / static_cast<std::size_t>(devices_);
				auto offset = static_cast<std::size_t>(i) * vol_dev_size;
				auto vol_on_device = scheduler_.volumes_per_device()[i];
				auto subvol_counter = 0u;
				for(auto& v : volume_map_[i])
				{
					auto subvol_dev_size = vol_dev_size / vol_on_device;
					auto first_row = offset + subvol_counter * subvol_dev_size;
					auto output_start = output.data() + first_row * output.width() * output.height();
					BOOST_LOG_TRIVIAL(debug) << "cuda::Feldkamp: Copying to row " << first_row;

					auto parms = cudaMemcpy3DParms{0};
					auto uchar_width = output.width() * sizeof(float)/sizeof(unsigned char);
					auto height = output.height();
					parms.srcPtr = make_cudaPitchedPtr(reinterpret_cast<unsigned char*>(v.data()), v.pitch(), uchar_width, height);
					parms.dstPtr = make_cudaPitchedPtr(reinterpret_cast<unsigned char*>(output_start), output.pitch(), uchar_width, height);
					parms.extent = make_cudaExtent(uchar_width, height, v.depth());
					parms.kind = cudaMemcpyDeviceToHost;
					CHECK(cudaMemcpy3D(&parms));
				}
			}
			results_.push(std::move(output));
		}
	}
}
