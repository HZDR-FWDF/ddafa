/*
 * This file is part of the ddafa reconstruction program.
 *
 * Copyright (C) 2016 Helmholtz-Zentrum Dresden-Rossendorf
 *
 * Licensed under the EUPL, Version 1.1 or - as soon they will be approved by
 * the European Commission - subsequent version of the EUPL (the "Licence");
 * You may not use this work except in compliance with the Licence.
 * You may obtain a copy of the Licence at:
 *
 * http://ec.europa.eu/idabc/eupl
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the Licence is distributed on an "AS IS" basis,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the Licence for the specific language governing permissions and
 * limitations under the Licence.
 *
 * Date: 09 September 2016
 * Authors: Jan Stephan
 */

#ifndef DDAFA_VOLUME_H_
#define DDAFA_VOLUME_H_

#include <cstddef>
#include <cstdint>
#include <utility>

namespace ddafa
{
    template <class Ptr>
    struct volume
    {
        volume() noexcept = default;

        volume(Ptr p, std::uint32_t w, std::uint32_t h, std::uint32_t d, std::uint32_t o, bool v, int dev) noexcept
        : ptr{std::move(p)}, width{w}, height{h}, depth{d}, offset{o}, valid{v}, device{dev}
        {}

        Ptr ptr = nullptr;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t depth = 0;
        std::uint32_t offset = 0;
        bool valid = false;
        int device = 0;
    };
}



#endif /* DDAFA_VOLUME_H_ */
