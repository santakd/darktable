/*
    This file is part of darktable,
    Copyright (C) 2017-2023 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include <stdint.h>
#include <stdlib.h>
#include "common/darktable.h"

typedef void((*eaw_decompose_t)(float *const restrict out, const float *const restrict in, float *const restrict detail,
                                const int scale, const float sharpen, const int32_t width, const int32_t height));

typedef void((*eaw_synthesize_t)(float *const out, const float *const in, const float *const restrict detail,
                                 const float *const restrict thrsf, const float *const restrict boostf,
                                 const int32_t width, const int32_t height));

void eaw_decompose_and_synthesize(float *const restrict out,
                                  const float *const restrict in,
                                  float *const restrict accum,
                                  const int scale,
                                  const float sharpen,
                                  const dt_aligned_pixel_t threshold,
                                  const dt_aligned_pixel_t boost,
                                  const ssize_t width,
                                  const ssize_t height) ;
void eaw_synthesize(float *const restrict out,
                    const float *const restrict in,
                    const float *const restrict detail,
                    const float *const restrict thrsf,
                    const float *const restrict boostf,
                    const int32_t width,
                    const int32_t height);

typedef void((*eaw_dn_decompose_t)(float *const restrict out, const float *const restrict in, float *const restrict detail,
                                   dt_aligned_pixel_t sum_squared, const int scale, const float inv_sigma2,
                                   const int32_t width, const int32_t height));

void eaw_dn_decompose(float *const restrict out, const float *const restrict in, float *const restrict detail,
                      dt_aligned_pixel_t sum_squared, const int scale, const float inv_sigma2,
                      const int32_t width, const int32_t height);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

