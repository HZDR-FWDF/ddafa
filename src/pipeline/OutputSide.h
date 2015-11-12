/*
 * OutputSide.h
 *
 *  Created on: 12.11.2015
 *      Author: Jan Stephan
 *
 *      The OutputSide class specifies the output interface of a pipeline stage.
 */

#ifndef OUTPUTSIDE_H_
#define OUTPUTSIDE_H_

#include <memory>
#include <utility>

#include "Port.h"

namespace ddafa
{
	namespace pipeline
	{
		template <class OutputType>
		class OutputSide
		{
			public:
				void output(OutputType&& output)
				{
					if(port_ == nullptr)
						throw std::runtime_error("OutputSide: Missing port");

					port_.forward(std::forward<OutputType&&>(output));
				}

				void attach(Port* port) noexcept
				{
					port_.reset(port);
				}

			protected:
				std::unique_ptr<Port> port_;
		};
	}
}


#endif /* OUTPUTSIDE_H_ */