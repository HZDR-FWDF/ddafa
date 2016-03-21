#ifndef CUDA_TOHOSTIMAGE_H_
#define CUDA_TOHOSTIMAGE_H_

#include <thread>
#include <vector>

#include <ddrf/Image.h>
#include <ddrf/Queue.h>
#include <ddrf/cuda/DeviceMemoryManager.h>
#include <ddrf/cuda/HostMemoryManager.h>
#include <ddrf/cuda/Memory.h>

namespace ddafa
{
	namespace cuda
	{
		class ToHostImage
		{
			public:
				using input_type = ddrf::Image<ddrf::cuda::DeviceMemoryManager<float>>;
				using output_type = ddrf::Image<ddrf::cuda::HostMemoryManager<float>>;

				ToHostImage();
				auto process(input_type&&) -> void;
				auto wait() -> output_type;

			protected:
				~ToHostImage();

			private:
				auto processor(input_type&&) -> void;
				auto finish() -> void;

			private:
				ddrf::Queue<output_type> results_;
				std::vector<std::thread> processor_threads_;
				int devices_;

		};
	}
}


#endif /* CUDA_TOHOSTIMAGE_H_ */