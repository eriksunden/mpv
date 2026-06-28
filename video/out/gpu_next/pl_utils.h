/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <libplacebo/utils/upload.h>
#include "video/img_format.h"

// Returns the number of planes on success, 0 if the format is unsupported.
// out_data[].pixels and .row_stride are not set; the caller fills them.
int plane_data_from_imgfmt(struct pl_plane_data out_data[4],
                           struct pl_bit_encoding *out_bits,
                           enum mp_imgfmt imgfmt, bool use_uint);
