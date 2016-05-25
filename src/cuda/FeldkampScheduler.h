#ifndef CUDA_FELDKAMPSCHEDULER_H_
#define CUDA_FELDKAMPSCHEDULER_H_

#include <cstddef>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include <boost/log/trivial.hpp>

#include <ddrf/cuda/Check.h>

#include "../common/Geometry.h"


namespace ddafa
{
	namespace cuda
	{
		// FIXME: Make me a "Modern C++ Design"-Singleton
		// FIXME: I am absolutely hideous. Replace me with something more elegant ASAP
		// FIXME: Remove template dependency
		template <typename T>
		class FeldkampScheduler
		{
			public:
				struct VolumeGeometry
				{
					std::size_t dim_x;
					std::size_t dim_y;
					std::size_t dim_z;

					float voxel_size_x;
					float voxel_size_y;
					float voxel_size_z;
				};

				~FeldkampScheduler() = default;

				static auto instance(const common::Geometry& geo) -> FeldkampScheduler<T>&
				{
					static FeldkampScheduler<T> instance(std::forward<const common::Geometry>(geo));
					return instance;
				}

				auto get_volume_num(int device) const noexcept -> std::uint32_t
				{
					try
					{
						return volumes_per_device_.at(device);
					}
					catch(const std::out_of_range&)
					{
						return 0u;
					}
				}

				auto get_volume_offset(int device, std::uint32_t index) const noexcept -> std::size_t
				{
					try
					{
						auto offset_map = offset_per_volume_.at(device);
						return offset_map.at(index);
					}
					catch(const std::out_of_range&)
					{
						return static_cast<std::size_t>(0);
					}
				}

				auto get_subproj_num(int device) const noexcept -> std::uint32_t
				{
					return get_volume_num(device);
				}

				auto get_subproj_dims(int device, std::size_t index) const noexcept -> std::pair<std::uint32_t, std::uint32_t>
				{
					try
					{
						auto vec = subprojs_.at(device);
						return vec.at(index);
					}
					catch(const std::out_of_range&)
					{
						return std::make_pair<std::uint32_t, std::uint32_t>(0u, 0u);
					}
				}

				auto get_subproj_offset(int device, std::uint32_t index) const noexcept -> std::size_t
				{
					try
					{
						auto offset_map = offset_per_subproj_.at(device);
						return offset_map.at(index);
					}
					catch(const std::out_of_range&)
					{
						return static_cast<std::size_t>(0);
					}
				}

				auto get_volume_geometry() const noexcept -> VolumeGeometry
				{
					return vol_geo_;
				}

			protected:
				FeldkampScheduler(const common::Geometry& geo)
				: vol_geo_{0}, volume_count_{0u}, dist_sd_{std::abs(geo.dist_det) + std::abs(geo.dist_src)}
				{
					CHECK(cudaGetDeviceCount(&devices_));

					calculate_volume_geo(geo);
					calculate_volume_height_mm();
					calculate_volume_bytes();
					calculate_volumes_per_device();
					calculate_subvolume_offsets();
					calculate_subprojection_borders(geo);
					distribute_subprojections();
					calculate_subprojection_offsets();
				}

			private:
				auto calculate_volume_geo(const common::Geometry& geo) -> void
				{
					// calculate volume dimensions -- x and y
					auto N_h = geo.det_pixels_row;
					auto d_h = geo.det_pixel_size_horiz;
					auto delta_h = geo.det_offset_horiz * d_h; // the offset is measured in pixels!
					auto alpha = std::atan((((N_h * d_h) / 2.f) + std::abs(delta_h)) / dist_sd_);
					auto r = std::abs(geo.dist_src) * std::sin(alpha);
					vol_geo_.voxel_size_x = r / ((((N_h * d_h) / 2.f) + std::abs(delta_h)) / d_h);
					vol_geo_.voxel_size_y = vol_geo_.voxel_size_x;
					vol_geo_.dim_x = static_cast<std::size_t>((2.f * r) / vol_geo_.voxel_size_x);
					vol_geo_.dim_y = vol_geo_.dim_x;

					// calculate volume dimensions -- z
					vol_geo_.voxel_size_z = vol_geo_.voxel_size_x;
					auto N_v = geo.det_pixels_column;
					auto d_v = geo.det_pixel_size_vert;
					auto delta_v = geo.det_offset_vert * d_v;
					vol_geo_.dim_z = static_cast<std::size_t>(((N_v * d_v) / 2.f + std::abs(delta_v)) * (std::abs(geo.dist_src) / dist_sd_) * (2.f / vol_geo_.voxel_size_z));

					BOOST_LOG_TRIVIAL(debug) << "Volume dimensions: " << vol_geo_.dim_x << "x" << vol_geo_.dim_y << "x" << vol_geo_.dim_z;
					BOOST_LOG_TRIVIAL(debug) << "Voxel size: " << vol_geo_.voxel_size_x << "x" << vol_geo_.voxel_size_y << "x" << vol_geo_.voxel_size_z;
				}

				auto calculate_volume_height_mm() -> void
				{
					volume_height_ = vol_geo_.dim_z * vol_geo_.voxel_size_z;
					BOOST_LOG_TRIVIAL(debug) << "Volume is " << volume_height_ << " mm high.";
				}

				auto calculate_volume_bytes() -> void
				{
					volume_bytes_ = vol_geo_.dim_x * vol_geo_.dim_y * vol_geo_.dim_z * sizeof(T);
					BOOST_LOG_TRIVIAL(debug) << "Volume needs " << volume_bytes_ << " Bytes.";
				}

				auto calculate_volumes_per_device() -> void
				{
					// split volume up if it doesn't fit into device memory
					volume_bytes_ /= static_cast<unsigned int>(devices_);
					for(auto i = 0; i < devices_; ++i)
					{
						auto vol_size_dev = volume_bytes_;
						auto vol_count_dev = 1u;
						CHECK(cudaSetDevice(i));
						auto properties = cudaDeviceProp{};
						CHECK(cudaGetDeviceProperties(&properties, i));

						// divide size by 2 until it fits in memory
						auto calcVolumeSizePerDev = std::function<std::size_t(std::size_t, std::uint32_t*, std::size_t)>();
						calcVolumeSizePerDev = [&calcVolumeSizePerDev](std::size_t volume_size, std::uint32_t* volume_count, std::size_t dev_mem)
						{
							if(volume_size >= dev_mem)
							{
								volume_size /= 2;
								*volume_count *= 2;
								return calcVolumeSizePerDev(volume_size, volume_count, dev_mem);
							}
							else
								return volume_size;
						};

						vol_size_dev = calcVolumeSizePerDev(vol_size_dev, &vol_count_dev, properties.totalGlobalMem);
						volume_count_ += vol_count_dev;
						auto chunk_str = std::string(vol_count_dev > 1 ? "chunks" : "chunk");
						BOOST_LOG_TRIVIAL(debug) << "Need " << vol_count_dev << " " << chunk_str << " with " << vol_size_dev
							<< " Bytes on device #" << i;
						volumes_per_device_.emplace(std::make_pair(i, vol_count_dev));
					}
				}

				auto calculate_subvolume_offsets() -> void
				{
					for(auto i = 0; i < devices_; ++i)
					{
						if(volumes_per_device_.count(i) == 0)
							continue;

						auto vol_offset = (vol_geo_.dim_z / volume_count_);
						auto vol_count_dev = volumes_per_device_[i];

						for(auto c = 0u; c < vol_count_dev; ++c)
							offset_per_volume_[i][c] = i * vol_count_dev * vol_offset + c * vol_offset;
					}
				}

				auto calculate_subprojection_borders(const common::Geometry& geo) -> void
				{
					auto delta_v = geo.det_offset_vert * geo.det_pixel_size_vert;
					auto d_v = geo.det_pixel_size_vert;
					auto N_v = geo.det_pixels_column;
					auto N = volume_count_;
					auto d_src = geo.dist_src;
					auto r_max = (vol_geo_.dim_x * vol_geo_.voxel_size_x) / 2.f;

					for(auto n = 0u; n < volume_count_; ++n)
					{
						auto top = -(volume_height_ / 2.f) + (static_cast<float>(n) / N) * volume_height_;
						auto bottom = -(volume_height_ / 2.f) + (static_cast<float>(n + 1) / N) * volume_height_;

						auto top_proj_virt = top * (dist_sd_) / (std::abs(d_src) + (top < 0.f ? -r_max : r_max));
						auto bottom_proj_virt = bottom * (dist_sd_) / (std::abs(d_src) + (bottom < 0.f ? r_max : -r_max));

						auto top_proj_real = 0.f - ((N_v * d_v) / 2.f) - delta_v + (d_v / 2.f);
						auto bottom_proj_real = top_proj_real + N_v * d_v - d_v;

						auto top_proj = float{};
						if(top_proj_virt > bottom_proj_real)
							top_proj = bottom_proj_real;
						else if(top_proj_virt < top_proj_real)
							top_proj = top_proj_real;
						else
							top_proj = top_proj_virt;

						auto bottom_proj = float{};
						if(bottom_proj_virt < top_proj_real)
							bottom_proj = top_proj_real;
						else if(bottom_proj_virt > bottom_proj_real)
							bottom_proj = bottom_proj_real;
						else
							bottom_proj = bottom_proj_virt;

						auto start_row = std::floor((((top_proj) + ((N_v * d_v) / 2.f) + delta_v) / d_v) - (1.f / 2.f));
						auto bottom_row = std::ceil((((bottom_proj) + ((N_v * d_v) / 2.f) + delta_v) / d_v) - (1.f / 2.f));

						if(start_row < 0.f)
							start_row = 0.f;
						if(bottom_row >= N_v)
							bottom_row = static_cast<float>(N_v) - 1.f;

						subproj_dims_.emplace_back(std::make_pair(static_cast<std::uint32_t>(start_row), static_cast<std::uint32_t>(bottom_row)));

						BOOST_LOG_TRIVIAL(debug) << "For volume #" << n << ": ";
						BOOST_LOG_TRIVIAL(debug) << "(top, bottom) = (" << top << ", " << bottom << ")";
						BOOST_LOG_TRIVIAL(debug) << "(top_proj_virt, bottom_proj_virt) = (" << top_proj_virt << ", " << bottom_proj_virt << ")";
						BOOST_LOG_TRIVIAL(debug) << "(top_proj_real, bottom_proj_real) = (" << top_proj_real << ", " << bottom_proj_real << ")";
						BOOST_LOG_TRIVIAL(debug) << "(top_proj, bottom_proj) = (" << top_proj << ", " << bottom_proj << ")";
						BOOST_LOG_TRIVIAL(debug) << "(start_row, bottom_row) = (" << start_row << ", " << bottom_row << ")";
					}
				}

				auto distribute_subprojections() -> void
				{
					auto subprojs_begin = std::begin(subproj_dims_);
					for(auto i = 0; i < devices_; ++i)
					{
						auto subprojs_count = get_volume_num(i);
						subprojs_.emplace(std::make_pair(i,
							std::vector<std::pair<std::uint32_t, std::uint32_t>>(subprojs_begin, subprojs_begin + subprojs_count)));
						subprojs_begin += subprojs_count;

						BOOST_LOG_TRIVIAL(debug) << "Device #" << i << " will process the following subprojection(s):";
						auto vec = subprojs_.at(i);
						for(auto& p : vec)
							BOOST_LOG_TRIVIAL(debug) << "(" << p.first << "," << p.second << ")";
					}
				}

				auto calculate_subprojection_offsets() -> void
				{
					for(auto i = 0; i < devices_; ++i)
					{
						auto subprojs_count = get_volume_num(i);
						if(subprojs_.count(i) == 0)
							continue;

						auto vec = subprojs_[i];
						for(auto c = 0u; c < subprojs_count; ++c)
							offset_per_subproj_[i][c] = vec[c].first;
					}
				}

			private:
				float volume_height_;
				std::size_t volume_bytes_;
				VolumeGeometry vol_geo_;
				int devices_;
				std::uint32_t volume_count_;
				std::map<int, std::uint32_t> volumes_per_device_;
				std::map<int, std::map<std::uint32_t, std::size_t>> offset_per_volume_;
				float dist_sd_;
				std::vector<std::pair<std::uint32_t, std::uint32_t>> subproj_dims_;
				std::map<int, std::vector<std::pair<std::uint32_t, std::uint32_t>>> subprojs_;
				std::map<int, std::map<std::uint32_t, std::size_t>> offset_per_subproj_;
		};
	}
}



#endif /* CUDA_FELDKAMPSCHEDULER_H_ */
