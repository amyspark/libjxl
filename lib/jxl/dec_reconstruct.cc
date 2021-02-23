// Copyright (c) the JPEG XL Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "lib/jxl/dec_reconstruct.h"

#include <atomic>
#include <utility>

#include "lib/jxl/filters.h"
#include "lib/jxl/image_ops.h"

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "lib/jxl/dec_reconstruct.cc"
#include <hwy/foreach_target.h>
#include <hwy/highway.h>

#include "lib/jxl/aux_out.h"
#include "lib/jxl/base/data_parallel.h"
#include "lib/jxl/base/profiler.h"
#include "lib/jxl/blending.h"
#include "lib/jxl/color_management.h"
#include "lib/jxl/common.h"
#include "lib/jxl/dec_noise.h"
#include "lib/jxl/dec_upsample.h"
#include "lib/jxl/dec_xyb-inl.h"
#include "lib/jxl/dec_xyb.h"
#include "lib/jxl/epf.h"
#include "lib/jxl/external_image.h"
#include "lib/jxl/frame_header.h"
#include "lib/jxl/gaborish.h"
#include "lib/jxl/loop_filter.h"
#include "lib/jxl/passes_state.h"
#include "lib/jxl/transfer_functions-inl.h"
HWY_BEFORE_NAMESPACE();
namespace jxl {
namespace HWY_NAMESPACE {

Status UndoXYBInPlace(Image3F* idct, const OpsinParams& opsin_params,
                      const Rect& rect, const ColorEncoding& target_encoding) {
  PROFILER_ZONE("UndoXYB");
  // The size of `rect` might not be a multiple of Lanes(d), but is guaranteed
  // to be a multiple of kBlockDim or at the margin of the image.
  for (size_t y = 0; y < rect.ysize(); y++) {
    float* JXL_RESTRICT row0 = rect.PlaneRow(idct, 0, y);
    float* JXL_RESTRICT row1 = rect.PlaneRow(idct, 1, y);
    float* JXL_RESTRICT row2 = rect.PlaneRow(idct, 2, y);

    const HWY_CAPPED(float, kBlockDim) d;

    if (target_encoding.IsLinearSRGB()) {
      for (size_t x = 0; x < rect.xsize(); x += Lanes(d)) {
        const auto in_opsin_x = Load(d, row0 + x);
        const auto in_opsin_y = Load(d, row1 + x);
        const auto in_opsin_b = Load(d, row2 + x);
        JXL_COMPILER_FENCE;
        auto linear_r = Undefined(d);
        auto linear_g = Undefined(d);
        auto linear_b = Undefined(d);
        XybToRgb(d, in_opsin_x, in_opsin_y, in_opsin_b, opsin_params, &linear_r,
                 &linear_g, &linear_b);

        Store(linear_r, d, row0 + x);
        Store(linear_g, d, row1 + x);
        Store(linear_b, d, row2 + x);
      }
    } else if (target_encoding.IsSRGB()) {
      for (size_t x = 0; x < rect.xsize(); x += Lanes(d)) {
        const auto in_opsin_x = Load(d, row0 + x);
        const auto in_opsin_y = Load(d, row1 + x);
        const auto in_opsin_b = Load(d, row2 + x);
        JXL_COMPILER_FENCE;
        auto linear_r = Undefined(d);
        auto linear_g = Undefined(d);
        auto linear_b = Undefined(d);
        XybToRgb(d, in_opsin_x, in_opsin_y, in_opsin_b, opsin_params, &linear_r,
                 &linear_g, &linear_b);

        Store(TF_SRGB().EncodedFromDisplay(d, linear_r), d, row0 + x);
        Store(TF_SRGB().EncodedFromDisplay(d, linear_g), d, row1 + x);
        Store(TF_SRGB().EncodedFromDisplay(d, linear_b), d, row2 + x);
      }
    } else {
      return JXL_FAILURE("Invalid target encoding");
    }
  }
  return true;
}

// NOLINTNEXTLINE(google-readability-namespace-comments)
}  // namespace HWY_NAMESPACE
}  // namespace jxl
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace jxl {

HWY_EXPORT(UndoXYBInPlace);

void EnsurePadding(const Image3F& src, const Rect& src_rect, Image3F* storage,
                   const Image3F** output, Rect* output_rect, size_t xpadding,
                   size_t ypadding, size_t xborder) {
  JXL_CHECK(xborder >= xpadding);
  if (src_rect.x0() >= xborder &&
      src_rect.x0() + src_rect.xsize() + xborder <= src.xsize() &&
      src_rect.y0() >= ypadding &&
      src_rect.y0() + src_rect.ysize() + ypadding <= src.ysize()) {
    // There is already enough border around `src:src_rect`, nothing to do.
    *output = &src;
    *output_rect = src_rect;
    return;
  }
  *output = storage;
  *output_rect = Rect(xborder, ypadding, src_rect.xsize(), src_rect.ysize());
  ssize_t src_x_start = src_rect.x0() - xpadding;
  ssize_t src_x_end = src_rect.x0() + src_rect.xsize() + xpadding;
  ssize_t storage_x_start = output_rect->x0() - xpadding;
  if (src_x_start + static_cast<ssize_t>(src.xsize()) >= 0 &&
      static_cast<size_t>(src_x_end) <= 2 * src.xsize()) {
    // Image is wide enough that a single Mirror() step is sufficient.
    for (size_t c = 0; c < 3; c++) {
      ssize_t y0 = src_rect.y0() - ypadding;
      ssize_t y1 = src_rect.y0() + src_rect.ysize() + ypadding;
      for (ssize_t y = y0; y < y1; y++) {
        float* JXL_RESTRICT row_out =
            storage->PlaneRow(c, y + output_rect->y0() - src_rect.y0());
        const float* JXL_RESTRICT row_in =
            src.ConstPlaneRow(c, Mirror(y, src.ysize()));
        // For x in [src_x_start, 0), we access the beginning of the row,
        // flipped.
        ssize_t x = src_x_start;
        for (; x < 0; x++) {
          row_out[x - src_x_start + storage_x_start] = row_in[-x - 1];
        }
        // From 0 to src_x_end or src.xsize(), we just copy directly.
        size_t num_direct = std::min<ssize_t>(src_x_end, src.xsize()) - x;
        memcpy(row_out + x - src_x_start + storage_x_start, row_in + x,
               num_direct * sizeof(float));
        x += num_direct;
        // From src.xsize() to src_x_end, we access the end of the row, flipped.
        for (; x < src_x_end; x++) {
          row_out[x - src_x_start + storage_x_start] =
              row_in[2 * src.xsize() - x - 1];
        }
      }
    }
  } else {
    // Slow case for small images.
    for (size_t c = 0; c < 3; c++) {
      ssize_t y0 = src_rect.y0() - ypadding;
      ssize_t y1 = src_rect.y0() + src_rect.ysize() + ypadding;
      for (ssize_t y = y0; y < y1; y++) {
        float* JXL_RESTRICT row_out =
            storage->PlaneRow(c, y + output_rect->y0() - src_rect.y0());
        const float* JXL_RESTRICT row_in =
            src.ConstPlaneRow(c, Mirror(y, src.ysize()));
        for (ssize_t x = src_x_start; x < src_x_end; x++) {
          row_out[x - src_x_start + storage_x_start] =
              row_in[Mirror(x, src.xsize())];
        }
      }
    }
  }
}

Status FinalizeImageRect(const Image3F& input_image, const Rect& input_rect,
                         PassesDecoderState* dec_state, size_t thread,
                         ImageBundle* JXL_RESTRICT output_image,
                         const Rect& output_rect) {
  const ImageFeatures& image_features = dec_state->shared->image_features;
  const FrameHeader& frame_header = dec_state->shared->frame_header;
  const OpsinParams& opsin_params = dec_state->shared->opsin_params;
  JXL_DASSERT(output_rect.xsize() <= kGroupDim);
  JXL_DASSERT(output_rect.ysize() <= kGroupDim);
  JXL_DASSERT(input_rect.xsize() == output_rect.xsize());
  JXL_DASSERT(input_rect.ysize() == output_rect.ysize());
  JXL_DASSERT(output_rect.x0() % kBlockDim == 0);
  JXL_DASSERT(output_rect.xsize() % kBlockDim == 0 ||
              output_rect.xsize() + output_rect.x0() ==
                  dec_state->shared->frame_dim.xsize);

  // This function operates in multiple steps:
  // - Apply EPF and/or Gaborish. This requires padding, and thus consumes a
  //   larger rect than it produces.
  // - Apply patches and splines (IF). This operates in-place.
  // - Apply upsampling. This does *not* operate in-place and requires 2 pixels
  //   of padding.
  // - Apply noise and color transforms. This operates in-place.

  // If we are applying upsampling, we need 2 more pixels around the actual rect
  // for border. Thus, we also need to apply patches and splines to those
  // pixels. We compute here
  // - The portion of image that corresponds to the area we are applying IF.
  //   (rect_for_if)
  // - The rect where that pixel data is stored in upsampling_input_storage.
  //   (rect_for_if_storage)
  // - The rect where the pixel data that we need to upsample is stored.
  //   (rect_for_upsampling)
  // - The source rect for the pixel data in `input_image`. It is assumed that,
  //   if `output_rect` is not on an image border, `input_image:input_rect` has
  //   enough border available. (rect_for_if_input)

  Image3F* storage_for_if = output_image->color();
  Rect rect_for_if = output_rect;
  Rect rect_for_if_storage = output_rect;
  Rect rect_for_upsampling = output_rect;
  Rect rect_for_if_input = input_rect;
  if (frame_header.upsampling != 1) {
    size_t ifbx0 = 0;
    size_t ifbx1 = 0;
    size_t ifby0 = 0;
    size_t ifby1 = 0;
    if (output_rect.x0() >= 2) {
      JXL_ASSERT(input_rect.x0() >= 2);
      ifbx0 = 2;
    }
    if (output_rect.y0() >= 2) {
      JXL_ASSERT(input_rect.y0() >= 2);
      ifby0 = 2;
    }
    if (output_rect.x0() + output_rect.xsize() + 2 <=
        dec_state->shared->frame_dim.xsize_padded) {
      JXL_ASSERT(input_rect.x0() + input_rect.xsize() + 2 <=
                 input_image.xsize());
      ifbx1 = 2;
    }
    if (output_rect.y0() + output_rect.ysize() + 2 <=
        dec_state->shared->frame_dim.ysize_padded) {
      JXL_ASSERT(input_rect.y0() + input_rect.ysize() + 2 <=
                 input_image.ysize());
      ifby1 = 2;
    }
    rect_for_if = Rect(output_rect.x0() - ifbx0, output_rect.y0() - ifby0,
                       output_rect.xsize() + ifbx0 + ifbx1,
                       output_rect.ysize() + ifby0 + ifby1);
    // Storage for pixel data does not necessarily start at (0, 0) as we need to
    // have the left border of upsampling_rect aligned to a multiple of
    // kBlockDim.
    rect_for_if_storage = Rect(RoundUpTo(ifbx0, kBlockDim) - ifbx0, 0,
                               rect_for_if.xsize(), rect_for_if.ysize());
    rect_for_upsampling = Rect(RoundUpTo(ifbx0, kBlockDim), ifby0,
                               output_rect.xsize(), output_rect.ysize());
    rect_for_if_input =
        Rect(input_rect.x0() - ifbx0, input_rect.y0() - ifby0,
             rect_for_if_storage.xsize(), rect_for_if_storage.ysize());
    storage_for_if = &dec_state->upsampling_input_storage[thread];
    // Ensures that image will be mirror-padded if needed.
    dec_state->upsampling_input_storage[thread].ShrinkTo(
        rect_for_if_storage.xsize() + rect_for_if_storage.x0(),
        rect_for_if_storage.ysize() + rect_for_if_storage.y0());
  }

  if (frame_header.loop_filter.epf_iters == 0 &&
      !frame_header.loop_filter.gab) {
    CopyImageTo(rect_for_if_input, input_image, rect_for_if_storage,
                storage_for_if);
  } else {
    const Image3F* filter_input;
    Rect filter_input_rect;
    size_t lf_padding = dec_state->shared->frame_header.loop_filter.Padding();
    // If `rect_for_if_input` does not start at a multiple of kBlockDim, we
    // extend the rect we run EPF on by one full block to ensure sigma is
    // handled correctly. We also extend the output and image rects accordingly.
    // To do this, we need 2 full blocks of border.
    size_t xborder = kBlockDim + (rect_for_if.x0() % kBlockDim);
    EnsurePadding(input_image, rect_for_if_input,
                  &dec_state->filter_input_storage[thread], &filter_input,
                  &filter_input_rect, lf_padding, lf_padding, xborder);
    size_t xextra = filter_input_rect.x0() % kBlockDim;
    Rect filter_input_padded_rect(
        filter_input_rect.x0() - xextra, filter_input_rect.y0(),
        filter_input_rect.xsize() + xextra, filter_input_rect.ysize());
    Rect image_padded_rect(rect_for_if.x0() - xextra, rect_for_if.y0(),
                           rect_for_if.xsize() + xextra, rect_for_if.ysize());
    Rect filter_output_padded_rect(
        rect_for_if_storage.x0() - xextra, rect_for_if_storage.y0(),
        rect_for_if_storage.xsize() + xextra, rect_for_if_storage.ysize());
    ApplyFilters(dec_state, image_padded_rect, *filter_input,
                 filter_input_padded_rect, thread, storage_for_if,
                 filter_output_padded_rect);
  }

  image_features.patches.AddTo(storage_for_if, rect_for_if_storage,
                               rect_for_if);
  JXL_RETURN_IF_ERROR(
      image_features.splines.AddTo(storage_for_if, rect_for_if_storage,
                                   rect_for_if, dec_state->shared->cmap));

  Rect upsampled_output_rect = output_rect;
  if (frame_header.upsampling != 1) {
    const Image3F* upsampling_input;
    Rect upsampling_input_rect;
    size_t xborder = kBlockDim;
    // We reuse filter_input_storage here as it is not currently in use.
    JXL_ASSERT(storage_for_if != &dec_state->filter_input_storage[thread]);
    EnsurePadding(*storage_for_if, rect_for_upsampling,
                  &dec_state->filter_input_storage[thread], &upsampling_input,
                  &upsampling_input_rect, 2, 2, xborder);
    upsampled_output_rect = Rect(output_rect.x0() * frame_header.upsampling,
                                 output_rect.y0() * frame_header.upsampling,
                                 output_rect.xsize() * frame_header.upsampling,
                                 output_rect.ysize() * frame_header.upsampling);
    dec_state->upsampler.UpsampleRect(*upsampling_input, upsampling_input_rect,
                                      output_image->color(),
                                      upsampled_output_rect);
  }
  // The image data is now unconditionally in
  // `output_image:upsampled_output_rect`.

  if (frame_header.flags & FrameHeader::kNoise) {
    PROFILER_ZONE("AddNoise");
    AddNoise(image_features.noise_params, upsampled_output_rect,
             dec_state->noise, upsampled_output_rect,
             dec_state->shared_storage.cmap, output_image->color());
  }

  if (dec_state->pre_color_transform_frame.xsize() != 0) {
    const Rect pre_color_output_rect =
        upsampled_output_rect.Crop(dec_state->pre_color_transform_frame);
    for (size_t c = 0; c < 3; c++) {
      for (size_t y = 0; y < pre_color_output_rect.ysize(); y++) {
        float* JXL_RESTRICT row_out = pre_color_output_rect.PlaneRow(
            &dec_state->pre_color_transform_frame, c, y);
        const float* JXL_RESTRICT row_in =
            pre_color_output_rect.ConstPlaneRow(*output_image->color(), c, y);
        memcpy(row_out, row_in,
               pre_color_output_rect.xsize() * sizeof(*row_in));
      }
    }
  }

  // We skip the color transform entirely if save_before_color_transform and
  // the frame is not supposed to be displayed.

  if (frame_header.needs_color_transform()) {
    if (frame_header.color_transform == ColorTransform::kXYB) {
      JXL_RETURN_IF_ERROR(HWY_DYNAMIC_DISPATCH(UndoXYBInPlace)(
          output_image->color(), opsin_params, upsampled_output_rect,
          dec_state->output_encoding));
    } else if (frame_header.color_transform == ColorTransform::kYCbCr) {
      YcbcrToRgb(*output_image->color(), output_image->color(),
                 upsampled_output_rect);
    }
  }

  // TODO(veluca): all blending should happen here.

  return true;
}

Status FinalizeFrameDecoding(ImageBundle* decoded,
                             PassesDecoderState* dec_state, ThreadPool* pool,
                             bool rerender, bool skip_blending) {
  std::vector<Rect> rects_to_process;

  const LoopFilter& lf = dec_state->shared->frame_header.loop_filter;
  const FrameHeader& frame_header = dec_state->shared->frame_header;
  const FrameDimensions& frame_dim = dec_state->shared->frame_dim;

  if (dec_state->FinalizeRectPadding() != 0 &&
      frame_header.chroma_subsampling.Is444() &&
      frame_header.encoding != FrameEncoding::kModular && !rerender) {
    size_t xsize = frame_dim.xsize_padded;
    size_t ysize = frame_dim.ysize_padded;
    size_t xsize_groups = frame_dim.xsize_groups;
    size_t ysize_groups = frame_dim.ysize_groups;
    size_t padx = RoundUpToBlockDim(dec_state->FinalizeRectPadding());
    size_t pady = dec_state->FinalizeRectPadding();
    // For every gap between groups, vertically, enqueue bottom gap with next
    // group ...
    for (size_t ygroup = 0; ygroup < ysize_groups - 1; ygroup++) {
      size_t gystart = ygroup * kGroupDim;
      size_t gyend = std::min(ysize, kGroupDim * (ygroup + 1));
      // Group is processed together with another group.
      if (gyend <= gystart + kBlockDim) continue;
      for (size_t xstart = 0; xstart < xsize;
           xstart += kApplyImageFeaturesTileDim) {
        rects_to_process.emplace_back(xstart, gyend - pady,
                                      kApplyImageFeaturesTileDim, 2 * pady,
                                      xsize, ysize);
      }
    }
    // For every gap between groups, horizontally, enqueue right gap with next
    // group, carefully avoiding overlaps with the horizontal gaps enqueued
    // before...
    for (size_t xgroup = 0; xgroup < xsize_groups - 1; xgroup++) {
      size_t gxstart = xgroup == 0 ? kBlockDim : xgroup * kGroupDim;
      size_t gxend = std::min(xsize, kGroupDim * (xgroup + 1));
      // Group is processed together with another group.
      if (gxend <= gxstart + kBlockDim) continue;
      for (size_t ygroup = 0; ygroup < ysize_groups; ygroup++) {
        size_t gystart = ygroup == 0 ? 0 : ygroup * kGroupDim + pady;
        size_t gyend = ygroup == ysize_groups - 1
                           ? ysize
                           : kGroupDim * (ygroup + 1) - pady;
        if (gyend <= gystart) continue;
        for (size_t ystart = gystart; ystart < gyend;
             ystart += kApplyImageFeaturesTileDim) {
          rects_to_process.emplace_back(gxend - padx, ystart, 2 * padx,
                                        kApplyImageFeaturesTileDim, xsize,
                                        gyend);
        }
      }
    }
  }
  // If we used chroma subsampling, we upsample chroma now and run
  // ApplyImageFeatures after.
  if (!frame_header.chroma_subsampling.Is444()) {
    for (size_t c = 0; c < 3; c++) {
      // This implementation of Upsample assumes that the image dimension in
      // xsize_padded, ysize_padded is a multiple of 8x8.
      JXL_DASSERT(frame_dim.xsize_padded %
                      (1u << frame_header.chroma_subsampling.HShift(c)) ==
                  0);
      JXL_DASSERT(frame_dim.ysize_padded %
                      (1u << frame_header.chroma_subsampling.VShift(c)) ==
                  0);
      ImageF& plane = const_cast<ImageF&>(dec_state->decoded.Plane(c));
      plane.ShrinkTo(
          (frame_dim.xsize_padded >> frame_header.chroma_subsampling.HShift(c)),
          frame_dim.ysize_padded >> frame_header.chroma_subsampling.VShift(c));
      for (size_t i = 0; i < frame_header.chroma_subsampling.HShift(c); i++) {
        plane.InitializePaddingForUnalignedAccesses();
        plane = UpsampleH2(plane, pool);
      }
      for (size_t i = 0; i < frame_header.chroma_subsampling.VShift(c); i++) {
        plane.InitializePaddingForUnalignedAccesses();
        plane = UpsampleV2(plane, pool);
      }
      JXL_DASSERT(SameSize(plane, dec_state->decoded));
    }
  }
  // ApplyImageFeatures was not yet run.
  if (frame_header.encoding == FrameEncoding::kModular ||
      !frame_header.chroma_subsampling.Is444() || rerender) {
    if (lf.epf_iters > 0 && frame_header.encoding == FrameEncoding::kModular) {
      FillImage(kInvSigmaNum / lf.epf_sigma_for_modular,
                &dec_state->filter_weights.sigma);
    }
    for (size_t y = 0; y < decoded->ysize(); y += kGroupDim) {
      for (size_t x = 0; x < decoded->xsize(); x += kGroupDim) {
        Rect rect(x, y, kGroupDim, kGroupDim, frame_dim.xsize, frame_dim.ysize);
        if (rect.xsize() == 0 || rect.ysize() == 0) continue;
        rects_to_process.push_back(rect);
      }
    }
  }
  const auto allocate_storage = [&](size_t num_threads) {
    dec_state->EnsureStorage(num_threads);
    return true;
  };

  std::atomic<bool> apply_features_ok{true};
  auto run_apply_features = [&](size_t rect_id, size_t thread) {
    if (!FinalizeImageRect(dec_state->decoded, rects_to_process[rect_id],
                           dec_state, thread, decoded,
                           rects_to_process[rect_id])) {
      apply_features_ok = false;
    }
  };

  RunOnPool(pool, 0, rects_to_process.size(), allocate_storage,
            run_apply_features, "ApplyFeatures");

  if (!apply_features_ok) {
    return JXL_FAILURE("FinalizeImageRect failed");
  }

  const size_t xsize = frame_dim.xsize_upsampled;
  const size_t ysize = frame_dim.ysize_upsampled;

  decoded->ShrinkTo(xsize, ysize);
  if (dec_state->pre_color_transform_frame.xsize() != 0) {
    dec_state->pre_color_transform_frame.ShrinkTo(xsize, ysize);
  }

  if (!skip_blending) {
    JXL_RETURN_IF_ERROR(DoBlending(dec_state, decoded));
  }

  return true;
}

}  // namespace jxl
#endif  // HWY_ONCE
