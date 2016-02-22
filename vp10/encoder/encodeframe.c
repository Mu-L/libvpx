/*
 *  Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <limits.h>
#include <math.h>
#include <stdio.h>

#include "./vp10_rtcd.h"
#include "./vpx_dsp_rtcd.h"
#include "./vpx_config.h"

#include "vpx_dsp/vpx_dsp_common.h"
#include "vpx_ports/mem.h"
#include "vpx_ports/vpx_timer.h"
#include "vpx_ports/system_state.h"

#include "vp10/common/common.h"
#include "vp10/common/entropy.h"
#include "vp10/common/entropymode.h"
#include "vp10/common/idct.h"
#include "vp10/common/mvref_common.h"
#include "vp10/common/pred_common.h"
#include "vp10/common/quant_common.h"
#include "vp10/common/reconintra.h"
#include "vp10/common/reconinter.h"
#include "vp10/common/seg_common.h"
#include "vp10/common/tile_common.h"

#include "vp10/encoder/aq_complexity.h"
#include "vp10/encoder/aq_cyclicrefresh.h"
#include "vp10/encoder/aq_variance.h"
#if CONFIG_SUPERTX
#include "vp10/encoder/cost.h"
#endif
#include "vp10/encoder/encodeframe.h"
#include "vp10/encoder/encodemb.h"
#include "vp10/encoder/encodemv.h"
#include "vp10/encoder/ethread.h"
#include "vp10/encoder/extend.h"
#include "vp10/encoder/rd.h"
#include "vp10/encoder/rdopt.h"
#include "vp10/encoder/segmentation.h"
#include "vp10/encoder/tokenize.h"

static void encode_superblock(VP10_COMP *cpi, ThreadData * td,
                              TOKENEXTRA **t, int output_enabled,
                              int mi_row, int mi_col, BLOCK_SIZE bsize,
                              PICK_MODE_CONTEXT *ctx);

#if CONFIG_SUPERTX
static int check_intra_b(PICK_MODE_CONTEXT *ctx);

static int check_intra_sb(VP10_COMP *cpi, const TileInfo *const tile,
                          int mi_row, int mi_col, BLOCK_SIZE bsize,
                          PC_TREE *pc_tree);
static void predict_superblock(VP10_COMP *cpi, ThreadData *td,
                               int mi_row_pred, int mi_col_pred,
                               BLOCK_SIZE bsize_pred, int b_sub8x8, int block);
static int check_supertx_sb(BLOCK_SIZE bsize, TX_SIZE supertx_size,
                            PC_TREE *pc_tree);
static void predict_sb_complex(VP10_COMP *cpi, ThreadData *td,
                               const TileInfo *const tile,
                               int mi_row, int mi_col,
                               int mi_row_ori, int mi_col_ori,
                               int output_enabled, BLOCK_SIZE bsize,
                               BLOCK_SIZE top_bsize,
                               uint8_t *dst_buf[3], int dst_stride[3],
                               PC_TREE *pc_tree);
static void update_state_sb_supertx(VP10_COMP *cpi, ThreadData *td,
                                    const TileInfo *const tile,
                                    int mi_row, int mi_col,
                                    BLOCK_SIZE bsize,
                                    int output_enabled, PC_TREE *pc_tree);
static void rd_supertx_sb(VP10_COMP *cpi, ThreadData *td,
                          const TileInfo *const tile,
                          int mi_row, int mi_col, BLOCK_SIZE bsize,
                          int *tmp_rate, int64_t *tmp_dist,
                          TX_TYPE *best_tx,
                          PC_TREE *pc_tree);
#endif  // CONFIG_SUPERTX

// This is used as a reference when computing the source variance for the
//  purposes of activity masking.
// Eventually this should be replaced by custom no-reference routines,
//  which will be faster.
static const uint8_t VP9_VAR_OFFS[64] = {
    128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128
};

#if CONFIG_VP9_HIGHBITDEPTH
static const uint16_t VP9_HIGH_VAR_OFFS_8[64] = {
    128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128
};

static const uint16_t VP9_HIGH_VAR_OFFS_10[64] = {
    128*4, 128*4, 128*4, 128*4, 128*4, 128*4, 128*4, 128*4,
    128*4, 128*4, 128*4, 128*4, 128*4, 128*4, 128*4, 128*4,
    128*4, 128*4, 128*4, 128*4, 128*4, 128*4, 128*4, 128*4,
    128*4, 128*4, 128*4, 128*4, 128*4, 128*4, 128*4, 128*4,
    128*4, 128*4, 128*4, 128*4, 128*4, 128*4, 128*4, 128*4,
    128*4, 128*4, 128*4, 128*4, 128*4, 128*4, 128*4, 128*4,
    128*4, 128*4, 128*4, 128*4, 128*4, 128*4, 128*4, 128*4,
    128*4, 128*4, 128*4, 128*4, 128*4, 128*4, 128*4, 128*4
};

static const uint16_t VP9_HIGH_VAR_OFFS_12[64] = {
    128*16, 128*16, 128*16, 128*16, 128*16, 128*16, 128*16, 128*16,
    128*16, 128*16, 128*16, 128*16, 128*16, 128*16, 128*16, 128*16,
    128*16, 128*16, 128*16, 128*16, 128*16, 128*16, 128*16, 128*16,
    128*16, 128*16, 128*16, 128*16, 128*16, 128*16, 128*16, 128*16,
    128*16, 128*16, 128*16, 128*16, 128*16, 128*16, 128*16, 128*16,
    128*16, 128*16, 128*16, 128*16, 128*16, 128*16, 128*16, 128*16,
    128*16, 128*16, 128*16, 128*16, 128*16, 128*16, 128*16, 128*16,
    128*16, 128*16, 128*16, 128*16, 128*16, 128*16, 128*16, 128*16
};
#endif  // CONFIG_VP9_HIGHBITDEPTH

unsigned int vp10_get_sby_perpixel_variance(VP10_COMP *cpi,
                                           const struct buf_2d *ref,
                                           BLOCK_SIZE bs) {
  unsigned int sse;
  const unsigned int var = cpi->fn_ptr[bs].vf(ref->buf, ref->stride,
                                              VP9_VAR_OFFS, 0, &sse);
  return ROUND_POWER_OF_TWO(var, num_pels_log2_lookup[bs]);
}

#if CONFIG_VP9_HIGHBITDEPTH
unsigned int vp10_high_get_sby_perpixel_variance(
    VP10_COMP *cpi, const struct buf_2d *ref, BLOCK_SIZE bs, int bd) {
  unsigned int var, sse;
  switch (bd) {
    case 10:
      var = cpi->fn_ptr[bs].vf(ref->buf, ref->stride,
                               CONVERT_TO_BYTEPTR(VP9_HIGH_VAR_OFFS_10),
                               0, &sse);
      break;
    case 12:
      var = cpi->fn_ptr[bs].vf(ref->buf, ref->stride,
                               CONVERT_TO_BYTEPTR(VP9_HIGH_VAR_OFFS_12),
                               0, &sse);
      break;
    case 8:
    default:
      var = cpi->fn_ptr[bs].vf(ref->buf, ref->stride,
                               CONVERT_TO_BYTEPTR(VP9_HIGH_VAR_OFFS_8),
                               0, &sse);
      break;
  }
  return ROUND_POWER_OF_TWO(var, num_pels_log2_lookup[bs]);
}
#endif  // CONFIG_VP9_HIGHBITDEPTH

static unsigned int get_sby_perpixel_diff_variance(VP10_COMP *cpi,
                                                   const struct buf_2d *ref,
                                                   int mi_row, int mi_col,
                                                   BLOCK_SIZE bs) {
  unsigned int sse, var;
  uint8_t *last_y;
  const YV12_BUFFER_CONFIG *last = get_ref_frame_buffer(cpi, LAST_FRAME);

  assert(last != NULL);
  last_y =
      &last->y_buffer[mi_row * MI_SIZE * last->y_stride + mi_col * MI_SIZE];
  var = cpi->fn_ptr[bs].vf(ref->buf, ref->stride, last_y, last->y_stride, &sse);
  return ROUND_POWER_OF_TWO(var, num_pels_log2_lookup[bs]);
}

static BLOCK_SIZE get_rd_var_based_fixed_partition(VP10_COMP *cpi,
                                                   MACROBLOCK *x,
                                                   int mi_row,
                                                   int mi_col) {
  unsigned int var = get_sby_perpixel_diff_variance(cpi, &x->plane[0].src,
                                                    mi_row, mi_col,
                                                    BLOCK_64X64);
  if (var < 8)
    return BLOCK_64X64;
  else if (var < 128)
    return BLOCK_32X32;
  else if (var < 2048)
    return BLOCK_16X16;
  else
    return BLOCK_8X8;
}

// Lighter version of set_offsets that only sets the mode info
// pointers.
static void set_mode_info_offsets(VP10_COMP *const cpi,
                                  MACROBLOCK *const x,
                                  MACROBLOCKD *const xd,
                                  int mi_row,
                                  int mi_col) {
  VP10_COMMON *const cm = &cpi->common;
  const int idx_str = xd->mi_stride * mi_row + mi_col;
  xd->mi = cm->mi_grid_visible + idx_str;
  xd->mi[0] = cm->mi + idx_str;
  x->mbmi_ext = cpi->mbmi_ext_base + (mi_row * cm->mi_cols + mi_col);
}

static void set_offsets(VP10_COMP *cpi, const TileInfo *const tile,
                        MACROBLOCK *const x, int mi_row, int mi_col,
                        BLOCK_SIZE bsize) {
  VP10_COMMON *const cm = &cpi->common;
  MACROBLOCKD *const xd = &x->e_mbd;
  MB_MODE_INFO *mbmi;
  const int mi_width = num_8x8_blocks_wide_lookup[bsize];
  const int mi_height = num_8x8_blocks_high_lookup[bsize];
  const struct segmentation *const seg = &cm->seg;

  set_skip_context(xd, mi_row, mi_col);

  set_mode_info_offsets(cpi, x, xd, mi_row, mi_col);

#if CONFIG_VAR_TX
  xd->above_txfm_context = cm->above_txfm_context + mi_col;
  xd->left_txfm_context = xd->left_txfm_context_buffer + (mi_row & MI_MASK);
  xd->max_tx_size = max_txsize_lookup[bsize];
#endif

  mbmi = &xd->mi[0]->mbmi;

  // Set up destination pointers.
  vp10_setup_dst_planes(xd->plane, get_frame_new_buffer(cm), mi_row, mi_col);

  // Set up limit values for MV components.
  // Mv beyond the range do not produce new/different prediction block.
  x->mv_row_min = -(((mi_row + mi_height) * MI_SIZE) + VP9_INTERP_EXTEND);
  x->mv_col_min = -(((mi_col + mi_width) * MI_SIZE) + VP9_INTERP_EXTEND);
  x->mv_row_max = (cm->mi_rows - mi_row) * MI_SIZE + VP9_INTERP_EXTEND;
  x->mv_col_max = (cm->mi_cols - mi_col) * MI_SIZE + VP9_INTERP_EXTEND;

  // Set up distance of MB to edge of frame in 1/8th pel units.
  assert(!(mi_col & (mi_width - 1)) && !(mi_row & (mi_height - 1)));
  set_mi_row_col(xd, tile, mi_row, mi_height, mi_col, mi_width,
                 cm->mi_rows, cm->mi_cols);

  // Set up source buffers.
  vp10_setup_src_planes(x, cpi->Source, mi_row, mi_col);

  // R/D setup.
  x->rddiv = cpi->rd.RDDIV;
  x->rdmult = cpi->rd.RDMULT;

  // Setup segment ID.
  if (seg->enabled) {
    if (cpi->oxcf.aq_mode != VARIANCE_AQ) {
      const uint8_t *const map = seg->update_map ? cpi->segmentation_map
                                                 : cm->last_frame_seg_map;
      mbmi->segment_id = get_segment_id(cm, map, bsize, mi_row, mi_col);
    }
    vp10_init_plane_quantizers(cpi, x);

    x->encode_breakout = cpi->segment_encode_breakout[mbmi->segment_id];
  } else {
    mbmi->segment_id = 0;
    x->encode_breakout = cpi->encode_breakout;
  }

  // required by vp10_append_sub8x8_mvs_for_idx() and vp10_find_best_ref_mvs()
  xd->tile = *tile;
}

#if CONFIG_SUPERTX
static void set_offsets_supertx(VP10_COMP *cpi, ThreadData *td,
                                const TileInfo *const tile,
                                int mi_row, int mi_col, BLOCK_SIZE bsize) {
  MACROBLOCK *const x = &td->mb;
  VP10_COMMON *const cm = &cpi->common;
  MACROBLOCKD *const xd = &x->e_mbd;
  const int mi_width = num_8x8_blocks_wide_lookup[bsize];
  const int mi_height = num_8x8_blocks_high_lookup[bsize];

  set_mode_info_offsets(cpi, x, xd, mi_row, mi_col);

  // Set up distance of MB to edge of frame in 1/8th pel units.
  assert(!(mi_col & (mi_width - 1)) && !(mi_row & (mi_height - 1)));
  set_mi_row_col(xd, tile, mi_row, mi_height, mi_col, mi_width,
                 cm->mi_rows, cm->mi_cols);
}

static void set_offsets_extend(VP10_COMP *cpi, ThreadData *td,
                               const TileInfo *const tile,
                               int mi_row_pred, int mi_col_pred,
                               int mi_row_ori, int mi_col_ori,
                               BLOCK_SIZE bsize_pred, BLOCK_SIZE bsize_ori) {
  // Used in supertx
  // (mi_row_ori, mi_col_ori, bsize_ori): region for mv
  // (mi_row_pred, mi_col_pred, bsize_pred): region to predict
  MACROBLOCK *const x = &td->mb;
  VP10_COMMON *const cm = &cpi->common;
  MACROBLOCKD *const xd = &x->e_mbd;
  MB_MODE_INFO *mbmi;
  const int mi_width = num_8x8_blocks_wide_lookup[bsize_pred];
  const int mi_height = num_8x8_blocks_high_lookup[bsize_pred];
  const struct segmentation *const seg = &cm->seg;

  set_mode_info_offsets(cpi, x, xd, mi_row_ori, mi_col_ori);

  mbmi = &xd->mi[0]->mbmi;

  // Set up limit values for MV components.
  // Mv beyond the range do not produce new/different prediction block.
  x->mv_row_min = -(((mi_row_pred + mi_height) * MI_SIZE) + VP9_INTERP_EXTEND);
  x->mv_col_min = -(((mi_col_pred + mi_width) * MI_SIZE) + VP9_INTERP_EXTEND);
  x->mv_row_max = (cm->mi_rows - mi_row_pred) * MI_SIZE + VP9_INTERP_EXTEND;
  x->mv_col_max = (cm->mi_cols - mi_col_pred) * MI_SIZE + VP9_INTERP_EXTEND;

  // Set up distance of MB to edge of frame in 1/8th pel units.
  assert(!(mi_col_pred & (mi_width - 1)) && !(mi_row_pred & (mi_height - 1)));
  set_mi_row_col(xd, tile, mi_row_pred, mi_height, mi_col_pred, mi_width,
                 cm->mi_rows, cm->mi_cols);
  xd->up_available    = (mi_row_ori != 0);
  xd->left_available  = (mi_col_ori > tile->mi_col_start);

  // R/D setup.
  x->rddiv = cpi->rd.RDDIV;
  x->rdmult = cpi->rd.RDMULT;

  // Setup segment ID.
  if (seg->enabled) {
    if (cpi->oxcf.aq_mode != VARIANCE_AQ) {
      const uint8_t *const map = seg->update_map ? cpi->segmentation_map
                                                 : cm->last_frame_seg_map;
      mbmi->segment_id = get_segment_id(cm, map, bsize_ori,
                                        mi_row_ori, mi_col_ori);
    }
    vp10_init_plane_quantizers(cpi, x);

    x->encode_breakout = cpi->segment_encode_breakout[mbmi->segment_id];
  } else {
    mbmi->segment_id = 0;
    x->encode_breakout = cpi->encode_breakout;
  }
}
#endif  // CONFIG_SUPERTX

static void set_block_size(VP10_COMP * const cpi,
                           MACROBLOCK *const x,
                           MACROBLOCKD *const xd,
                           int mi_row, int mi_col,
                           BLOCK_SIZE bsize) {
  if (cpi->common.mi_cols > mi_col && cpi->common.mi_rows > mi_row) {
    set_mode_info_offsets(cpi, x, xd, mi_row, mi_col);
    xd->mi[0]->mbmi.sb_type = bsize;
  }
}

typedef struct {
  int64_t sum_square_error;
  int64_t sum_error;
  int log2_count;
  int variance;
} var;

typedef struct {
  var none;
  var horz[2];
  var vert[2];
} partition_variance;

typedef struct {
  partition_variance part_variances;
  var split[4];
} v4x4;

typedef struct {
  partition_variance part_variances;
  v4x4 split[4];
} v8x8;

typedef struct {
  partition_variance part_variances;
  v8x8 split[4];
} v16x16;

typedef struct {
  partition_variance part_variances;
  v16x16 split[4];
} v32x32;

typedef struct {
  partition_variance part_variances;
  v32x32 split[4];
} v64x64;

typedef struct {
  partition_variance *part_variances;
  var *split[4];
} variance_node;

typedef enum {
  V16X16,
  V32X32,
  V64X64,
} TREE_LEVEL;

static void tree_to_node(void *data, BLOCK_SIZE bsize, variance_node *node) {
  int i;
  node->part_variances = NULL;
  switch (bsize) {
    case BLOCK_64X64: {
      v64x64 *vt = (v64x64 *) data;
      node->part_variances = &vt->part_variances;
      for (i = 0; i < 4; i++)
        node->split[i] = &vt->split[i].part_variances.none;
      break;
    }
    case BLOCK_32X32: {
      v32x32 *vt = (v32x32 *) data;
      node->part_variances = &vt->part_variances;
      for (i = 0; i < 4; i++)
        node->split[i] = &vt->split[i].part_variances.none;
      break;
    }
    case BLOCK_16X16: {
      v16x16 *vt = (v16x16 *) data;
      node->part_variances = &vt->part_variances;
      for (i = 0; i < 4; i++)
        node->split[i] = &vt->split[i].part_variances.none;
      break;
    }
    case BLOCK_8X8: {
      v8x8 *vt = (v8x8 *) data;
      node->part_variances = &vt->part_variances;
      for (i = 0; i < 4; i++)
        node->split[i] = &vt->split[i].part_variances.none;
      break;
    }
    case BLOCK_4X4: {
      v4x4 *vt = (v4x4 *) data;
      node->part_variances = &vt->part_variances;
      for (i = 0; i < 4; i++)
        node->split[i] = &vt->split[i];
      break;
    }
    default: {
      assert(0);
      break;
    }
  }
}

// Set variance values given sum square error, sum error, count.
static void fill_variance(int64_t s2, int64_t s, int c, var *v) {
  v->sum_square_error = s2;
  v->sum_error = s;
  v->log2_count = c;
}

static void get_variance(var *v) {
  v->variance = (int)(256 * (v->sum_square_error -
      ((v->sum_error * v->sum_error) >> v->log2_count)) >> v->log2_count);
}

static void sum_2_variances(const var *a, const var *b, var *r) {
  assert(a->log2_count == b->log2_count);
  fill_variance(a->sum_square_error + b->sum_square_error,
                a->sum_error + b->sum_error, a->log2_count + 1, r);
}

static void fill_variance_tree(void *data, BLOCK_SIZE bsize) {
  variance_node node;
  memset(&node, 0, sizeof(node));
  tree_to_node(data, bsize, &node);
  sum_2_variances(node.split[0], node.split[1], &node.part_variances->horz[0]);
  sum_2_variances(node.split[2], node.split[3], &node.part_variances->horz[1]);
  sum_2_variances(node.split[0], node.split[2], &node.part_variances->vert[0]);
  sum_2_variances(node.split[1], node.split[3], &node.part_variances->vert[1]);
  sum_2_variances(&node.part_variances->vert[0], &node.part_variances->vert[1],
                  &node.part_variances->none);
}

static int set_vt_partitioning(VP10_COMP *cpi,
                               MACROBLOCK *const x,
                               MACROBLOCKD *const xd,
                               void *data,
                               BLOCK_SIZE bsize,
                               int mi_row,
                               int mi_col,
                               int64_t threshold,
                               BLOCK_SIZE bsize_min,
                               int force_split) {
  VP10_COMMON * const cm = &cpi->common;
  variance_node vt;
  const int block_width = num_8x8_blocks_wide_lookup[bsize];
  const int block_height = num_8x8_blocks_high_lookup[bsize];
  const int low_res = (cm->width <= 352 && cm->height <= 288);

  assert(block_height == block_width);
  tree_to_node(data, bsize, &vt);

  if (force_split == 1)
    return 0;

  // For bsize=bsize_min (16x16/8x8 for 8x8/4x4 downsampling), select if
  // variance is below threshold, otherwise split will be selected.
  // No check for vert/horiz split as too few samples for variance.
  if (bsize == bsize_min) {
    // Variance already computed to set the force_split.
    if (low_res || cm->frame_type == KEY_FRAME)
      get_variance(&vt.part_variances->none);
    if (mi_col + block_width / 2 < cm->mi_cols &&
        mi_row + block_height / 2 < cm->mi_rows &&
        vt.part_variances->none.variance < threshold) {
      set_block_size(cpi, x, xd, mi_row, mi_col, bsize);
      return 1;
    }
    return 0;
  } else if (bsize > bsize_min) {
    // Variance already computed to set the force_split.
    if (low_res || cm->frame_type == KEY_FRAME)
      get_variance(&vt.part_variances->none);
    // For key frame: take split for bsize above 32X32 or very high variance.
    if (cm->frame_type == KEY_FRAME &&
        (bsize > BLOCK_32X32 ||
        vt.part_variances->none.variance > (threshold << 4))) {
      return 0;
    }
    // If variance is low, take the bsize (no split).
    if (mi_col + block_width / 2 < cm->mi_cols &&
        mi_row + block_height / 2 < cm->mi_rows &&
        vt.part_variances->none.variance < threshold) {
      set_block_size(cpi, x, xd, mi_row, mi_col, bsize);
      return 1;
    }

    // Check vertical split.
    if (mi_row + block_height / 2 < cm->mi_rows) {
      BLOCK_SIZE subsize = get_subsize(bsize, PARTITION_VERT);
      get_variance(&vt.part_variances->vert[0]);
      get_variance(&vt.part_variances->vert[1]);
      if (vt.part_variances->vert[0].variance < threshold &&
          vt.part_variances->vert[1].variance < threshold &&
          get_plane_block_size(subsize, &xd->plane[1]) < BLOCK_INVALID) {
        set_block_size(cpi, x, xd, mi_row, mi_col, subsize);
        set_block_size(cpi, x, xd, mi_row, mi_col + block_width / 2, subsize);
        return 1;
      }
    }
    // Check horizontal split.
    if (mi_col + block_width / 2 < cm->mi_cols) {
      BLOCK_SIZE subsize = get_subsize(bsize, PARTITION_HORZ);
      get_variance(&vt.part_variances->horz[0]);
      get_variance(&vt.part_variances->horz[1]);
      if (vt.part_variances->horz[0].variance < threshold &&
          vt.part_variances->horz[1].variance < threshold &&
          get_plane_block_size(subsize, &xd->plane[1]) < BLOCK_INVALID) {
        set_block_size(cpi, x, xd, mi_row, mi_col, subsize);
        set_block_size(cpi, x, xd, mi_row + block_height / 2, mi_col, subsize);
        return 1;
      }
    }

    return 0;
  }
  return 0;
}

// Set the variance split thresholds for following the block sizes:
// 0 - threshold_64x64, 1 - threshold_32x32, 2 - threshold_16x16,
// 3 - vbp_threshold_8x8. vbp_threshold_8x8 (to split to 4x4 partition) is
// currently only used on key frame.
static void set_vbp_thresholds(VP10_COMP *cpi, int64_t thresholds[], int q) {
  VP10_COMMON *const cm = &cpi->common;
  const int is_key_frame = (cm->frame_type == KEY_FRAME);
  const int threshold_multiplier = is_key_frame ? 20 : 1;
  const int64_t threshold_base = (int64_t)(threshold_multiplier *
      cpi->y_dequant[q][1]);
  if (is_key_frame) {
    thresholds[0] = threshold_base;
    thresholds[1] = threshold_base >> 2;
    thresholds[2] = threshold_base >> 2;
    thresholds[3] = threshold_base << 2;
  } else {
    thresholds[1] = threshold_base;
    if (cm->width <= 352 && cm->height <= 288) {
      thresholds[0] = threshold_base >> 2;
      thresholds[2] = threshold_base << 3;
    } else {
      thresholds[0] = threshold_base;
      thresholds[1] = (5 * threshold_base) >> 2;
      if (cm->width >= 1920 && cm->height >= 1080)
        thresholds[1] = (7 * threshold_base) >> 2;
      thresholds[2] = threshold_base << cpi->oxcf.speed;
    }
  }
}

void vp10_set_variance_partition_thresholds(VP10_COMP *cpi, int q) {
  VP10_COMMON *const cm = &cpi->common;
  SPEED_FEATURES *const sf = &cpi->sf;
  const int is_key_frame = (cm->frame_type == KEY_FRAME);
  if (sf->partition_search_type != VAR_BASED_PARTITION &&
      sf->partition_search_type != REFERENCE_PARTITION) {
    return;
  } else {
    set_vbp_thresholds(cpi, cpi->vbp_thresholds, q);
    // The thresholds below are not changed locally.
    if (is_key_frame) {
      cpi->vbp_threshold_sad = 0;
      cpi->vbp_bsize_min = BLOCK_8X8;
    } else {
      if (cm->width <= 352 && cm->height <= 288)
        cpi->vbp_threshold_sad = 100;
      else
        cpi->vbp_threshold_sad = (cpi->y_dequant[q][1] << 1) > 1000 ?
            (cpi->y_dequant[q][1] << 1) : 1000;
      cpi->vbp_bsize_min = BLOCK_16X16;
    }
    cpi->vbp_threshold_minmax = 15 + (q >> 3);
  }
}

// Compute the minmax over the 8x8 subblocks.
static int compute_minmax_8x8(const uint8_t *s, int sp, const uint8_t *d,
                              int dp, int x16_idx, int y16_idx,
#if CONFIG_VP9_HIGHBITDEPTH
                              int highbd_flag,
#endif
                              int pixels_wide,
                              int pixels_high) {
  int k;
  int minmax_max = 0;
  int minmax_min = 255;
  // Loop over the 4 8x8 subblocks.
  for (k = 0; k < 4; k++) {
    int x8_idx = x16_idx + ((k & 1) << 3);
    int y8_idx = y16_idx + ((k >> 1) << 3);
    int min = 0;
    int max = 0;
    if (x8_idx < pixels_wide && y8_idx < pixels_high) {
#if CONFIG_VP9_HIGHBITDEPTH
      if (highbd_flag & YV12_FLAG_HIGHBITDEPTH) {
        vpx_highbd_minmax_8x8(s + y8_idx * sp + x8_idx, sp,
                              d + y8_idx * dp + x8_idx, dp,
                              &min, &max);
      } else {
        vpx_minmax_8x8(s + y8_idx * sp + x8_idx, sp,
                       d + y8_idx * dp + x8_idx, dp,
                       &min, &max);
      }
#else
      vpx_minmax_8x8(s + y8_idx * sp + x8_idx, sp,
                     d + y8_idx * dp + x8_idx, dp,
                     &min, &max);
#endif
      if ((max - min) > minmax_max)
        minmax_max = (max - min);
      if ((max - min) < minmax_min)
        minmax_min = (max - min);
    }
  }
  return (minmax_max - minmax_min);
}

static void fill_variance_4x4avg(const uint8_t *s, int sp, const uint8_t *d,
                                 int dp, int x8_idx, int y8_idx, v8x8 *vst,
#if CONFIG_VP9_HIGHBITDEPTH
                                 int highbd_flag,
#endif
                                 int pixels_wide,
                                 int pixels_high,
                                 int is_key_frame) {
  int k;
  for (k = 0; k < 4; k++) {
    int x4_idx = x8_idx + ((k & 1) << 2);
    int y4_idx = y8_idx + ((k >> 1) << 2);
    unsigned int sse = 0;
    int sum = 0;
    if (x4_idx < pixels_wide && y4_idx < pixels_high) {
      int s_avg;
      int d_avg = 128;
#if CONFIG_VP9_HIGHBITDEPTH
      if (highbd_flag & YV12_FLAG_HIGHBITDEPTH) {
        s_avg = vpx_highbd_avg_4x4(s + y4_idx * sp + x4_idx, sp);
        if (!is_key_frame)
          d_avg = vpx_highbd_avg_4x4(d + y4_idx * dp + x4_idx, dp);
      } else {
        s_avg = vpx_avg_4x4(s + y4_idx * sp + x4_idx, sp);
        if (!is_key_frame)
          d_avg = vpx_avg_4x4(d + y4_idx * dp + x4_idx, dp);
      }
#else
      s_avg = vpx_avg_4x4(s + y4_idx * sp + x4_idx, sp);
      if (!is_key_frame)
        d_avg = vpx_avg_4x4(d + y4_idx * dp + x4_idx, dp);
#endif
      sum = s_avg - d_avg;
      sse = sum * sum;
    }
    fill_variance(sse, sum, 0, &vst->split[k].part_variances.none);
  }
}

static void fill_variance_8x8avg(const uint8_t *s, int sp, const uint8_t *d,
                                 int dp, int x16_idx, int y16_idx, v16x16 *vst,
#if CONFIG_VP9_HIGHBITDEPTH
                                 int highbd_flag,
#endif
                                 int pixels_wide,
                                 int pixels_high,
                                 int is_key_frame) {
  int k;
  for (k = 0; k < 4; k++) {
    int x8_idx = x16_idx + ((k & 1) << 3);
    int y8_idx = y16_idx + ((k >> 1) << 3);
    unsigned int sse = 0;
    int sum = 0;
    if (x8_idx < pixels_wide && y8_idx < pixels_high) {
      int s_avg;
      int d_avg = 128;
#if CONFIG_VP9_HIGHBITDEPTH
      if (highbd_flag & YV12_FLAG_HIGHBITDEPTH) {
        s_avg = vpx_highbd_avg_8x8(s + y8_idx * sp + x8_idx, sp);
        if (!is_key_frame)
          d_avg = vpx_highbd_avg_8x8(d + y8_idx * dp + x8_idx, dp);
      } else {
        s_avg = vpx_avg_8x8(s + y8_idx * sp + x8_idx, sp);
        if (!is_key_frame)
          d_avg = vpx_avg_8x8(d + y8_idx * dp + x8_idx, dp);
      }
#else
      s_avg = vpx_avg_8x8(s + y8_idx * sp + x8_idx, sp);
      if (!is_key_frame)
        d_avg = vpx_avg_8x8(d + y8_idx * dp + x8_idx, dp);
#endif
      sum = s_avg - d_avg;
      sse = sum * sum;
    }
    fill_variance(sse, sum, 0, &vst->split[k].part_variances.none);
  }
}

// This function chooses partitioning based on the variance between source and
// reconstructed last, where variance is computed for down-sampled inputs.
static int choose_partitioning(VP10_COMP *cpi,
                                const TileInfo *const tile,
                                MACROBLOCK *x,
                                int mi_row, int mi_col) {
  VP10_COMMON * const cm = &cpi->common;
  MACROBLOCKD *xd = &x->e_mbd;
  int i, j, k, m;
  v64x64 vt;
  v16x16 vt2[16];
  int force_split[21];
  uint8_t *s;
  const uint8_t *d;
  int sp;
  int dp;
  int pixels_wide = 64, pixels_high = 64;
  int64_t thresholds[4] = {cpi->vbp_thresholds[0], cpi->vbp_thresholds[1],
      cpi->vbp_thresholds[2], cpi->vbp_thresholds[3]};

  // Always use 4x4 partition for key frame.
  const int is_key_frame = (cm->frame_type == KEY_FRAME);
  const int use_4x4_partition = is_key_frame;
  const int low_res = (cm->width <= 352 && cm->height <= 288);
  int variance4x4downsample[16];

  int segment_id = CR_SEGMENT_ID_BASE;
  if (cpi->oxcf.aq_mode == CYCLIC_REFRESH_AQ && cm->seg.enabled) {
    const uint8_t *const map = cm->seg.update_map ? cpi->segmentation_map :
                                                    cm->last_frame_seg_map;
    segment_id = get_segment_id(cm, map, BLOCK_64X64, mi_row, mi_col);

    if (cyclic_refresh_segment_id_boosted(segment_id)) {
      int q = vp10_get_qindex(&cm->seg, segment_id, cm->base_qindex);
      set_vbp_thresholds(cpi, thresholds, q);
    }
  }

  set_offsets(cpi, tile, x, mi_row, mi_col, BLOCK_64X64);

  if (xd->mb_to_right_edge < 0)
    pixels_wide += (xd->mb_to_right_edge >> 3);
  if (xd->mb_to_bottom_edge < 0)
    pixels_high += (xd->mb_to_bottom_edge >> 3);

  s = x->plane[0].src.buf;
  sp = x->plane[0].src.stride;

  if (!is_key_frame) {
    MB_MODE_INFO *mbmi = &xd->mi[0]->mbmi;
    unsigned int uv_sad;
    const YV12_BUFFER_CONFIG *yv12 = get_ref_frame_buffer(cpi, LAST_FRAME);

    const YV12_BUFFER_CONFIG *yv12_g = NULL;
    unsigned int y_sad, y_sad_g;
    const BLOCK_SIZE bsize = BLOCK_32X32
        + (mi_col + 4 < cm->mi_cols) * 2 + (mi_row + 4 < cm->mi_rows);

    assert(yv12 != NULL);
    yv12_g = get_ref_frame_buffer(cpi, GOLDEN_FRAME);

    if (yv12_g && yv12_g != yv12) {
      vp10_setup_pre_planes(xd, 0, yv12_g, mi_row, mi_col,
                           &cm->frame_refs[GOLDEN_FRAME - 1].sf);
      y_sad_g = cpi->fn_ptr[bsize].sdf(x->plane[0].src.buf,
                                       x->plane[0].src.stride,
                                       xd->plane[0].pre[0].buf,
                                       xd->plane[0].pre[0].stride);
    } else {
      y_sad_g = UINT_MAX;
    }

    vp10_setup_pre_planes(xd, 0, yv12, mi_row, mi_col,
                         &cm->frame_refs[LAST_FRAME - 1].sf);
    mbmi->ref_frame[0] = LAST_FRAME;
    mbmi->ref_frame[1] = NONE;
    mbmi->sb_type = BLOCK_64X64;
    mbmi->mv[0].as_int = 0;
    mbmi->interp_filter = BILINEAR;

    y_sad = vp10_int_pro_motion_estimation(cpi, x, bsize, mi_row, mi_col);
    if (y_sad_g < y_sad) {
      vp10_setup_pre_planes(xd, 0, yv12_g, mi_row, mi_col,
                           &cm->frame_refs[GOLDEN_FRAME - 1].sf);
      mbmi->ref_frame[0] = GOLDEN_FRAME;
      mbmi->mv[0].as_int = 0;
      y_sad = y_sad_g;
    } else {
      x->pred_mv[LAST_FRAME] = mbmi->mv[0].as_mv;
    }

    vp10_build_inter_predictors_sb(xd, mi_row, mi_col, BLOCK_64X64);

    for (i = 1; i <= 2; ++i) {
      struct macroblock_plane  *p = &x->plane[i];
      struct macroblockd_plane *pd = &xd->plane[i];
      const BLOCK_SIZE bs = get_plane_block_size(bsize, pd);

      if (bs == BLOCK_INVALID)
        uv_sad = UINT_MAX;
      else
        uv_sad = cpi->fn_ptr[bs].sdf(p->src.buf, p->src.stride,
                                     pd->dst.buf, pd->dst.stride);

      x->color_sensitivity[i - 1] = uv_sad > (y_sad >> 2);
    }

    d = xd->plane[0].dst.buf;
    dp = xd->plane[0].dst.stride;

    // If the y_sad is very small, take 64x64 as partition and exit.
    // Don't check on boosted segment for now, as 64x64 is suppressed there.
    if (segment_id == CR_SEGMENT_ID_BASE &&
        y_sad < cpi->vbp_threshold_sad) {
      const int block_width = num_8x8_blocks_wide_lookup[BLOCK_64X64];
      const int block_height = num_8x8_blocks_high_lookup[BLOCK_64X64];
      if (mi_col + block_width / 2 < cm->mi_cols &&
          mi_row + block_height / 2 < cm->mi_rows) {
        set_block_size(cpi, x, xd, mi_row, mi_col, BLOCK_64X64);
        return 0;
      }
    }
  } else {
    d = VP9_VAR_OFFS;
    dp = 0;
#if CONFIG_VP9_HIGHBITDEPTH
    if (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) {
      switch (xd->bd) {
        case 10:
          d = CONVERT_TO_BYTEPTR(VP9_HIGH_VAR_OFFS_10);
          break;
        case 12:
          d = CONVERT_TO_BYTEPTR(VP9_HIGH_VAR_OFFS_12);
          break;
        case 8:
        default:
          d = CONVERT_TO_BYTEPTR(VP9_HIGH_VAR_OFFS_8);
          break;
      }
    }
#endif  // CONFIG_VP9_HIGHBITDEPTH
  }

  // Index for force_split: 0 for 64x64, 1-4 for 32x32 blocks,
  // 5-20 for the 16x16 blocks.
  force_split[0] = 0;
  // Fill in the entire tree of 8x8 (or 4x4 under some conditions) variances
  // for splits.
  for (i = 0; i < 4; i++) {
    const int x32_idx = ((i & 1) << 5);
    const int y32_idx = ((i >> 1) << 5);
    const int i2 = i << 2;
    force_split[i + 1] = 0;
    for (j = 0; j < 4; j++) {
      const int x16_idx = x32_idx + ((j & 1) << 4);
      const int y16_idx = y32_idx + ((j >> 1) << 4);
      const int split_index = 5 + i2 + j;
      v16x16 *vst = &vt.split[i].split[j];
      force_split[split_index] = 0;
      variance4x4downsample[i2 + j] = 0;
      if (!is_key_frame) {
        fill_variance_8x8avg(s, sp, d, dp, x16_idx, y16_idx, vst,
#if CONFIG_VP9_HIGHBITDEPTH
                            xd->cur_buf->flags,
#endif
                            pixels_wide,
                            pixels_high,
                            is_key_frame);
        fill_variance_tree(&vt.split[i].split[j], BLOCK_16X16);
        get_variance(&vt.split[i].split[j].part_variances.none);
        if (vt.split[i].split[j].part_variances.none.variance >
            thresholds[2]) {
          // 16X16 variance is above threshold for split, so force split to 8x8
          // for this 16x16 block (this also forces splits for upper levels).
          force_split[split_index] = 1;
          force_split[i + 1] = 1;
          force_split[0] = 1;
        } else if (vt.split[i].split[j].part_variances.none.variance >
                   thresholds[1] &&
                   !cyclic_refresh_segment_id_boosted(segment_id)) {
          // We have some nominal amount of 16x16 variance (based on average),
          // compute the minmax over the 8x8 sub-blocks, and if above threshold,
          // force split to 8x8 block for this 16x16 block.
          int minmax = compute_minmax_8x8(s, sp, d, dp, x16_idx, y16_idx,
#if CONFIG_VP9_HIGHBITDEPTH
                                          xd->cur_buf->flags,
#endif
                                          pixels_wide, pixels_high);
          if (minmax > cpi->vbp_threshold_minmax) {
            force_split[split_index] = 1;
            force_split[i + 1] = 1;
            force_split[0] = 1;
          }
        }
      }
      if (is_key_frame || (low_res &&
          vt.split[i].split[j].part_variances.none.variance >
          (thresholds[1] << 1))) {
        force_split[split_index] = 0;
        // Go down to 4x4 down-sampling for variance.
        variance4x4downsample[i2 + j] = 1;
        for (k = 0; k < 4; k++) {
          int x8_idx = x16_idx + ((k & 1) << 3);
          int y8_idx = y16_idx + ((k >> 1) << 3);
          v8x8 *vst2 = is_key_frame ? &vst->split[k] :
              &vt2[i2 + j].split[k];
          fill_variance_4x4avg(s, sp, d, dp, x8_idx, y8_idx, vst2,
#if CONFIG_VP9_HIGHBITDEPTH
                               xd->cur_buf->flags,
#endif
                               pixels_wide,
                               pixels_high,
                               is_key_frame);
        }
      }
    }
  }

  // Fill the rest of the variance tree by summing split partition values.
  for (i = 0; i < 4; i++) {
    const int i2 = i << 2;
    for (j = 0; j < 4; j++) {
      if (variance4x4downsample[i2 + j] == 1) {
        v16x16 *vtemp = (!is_key_frame) ? &vt2[i2 + j] :
            &vt.split[i].split[j];
        for (m = 0; m < 4; m++)
          fill_variance_tree(&vtemp->split[m], BLOCK_8X8);
        fill_variance_tree(vtemp, BLOCK_16X16);
      }
    }
    fill_variance_tree(&vt.split[i], BLOCK_32X32);
    // If variance of this 32x32 block is above the threshold, force the block
    // to split. This also forces a split on the upper (64x64) level.
    if (!force_split[i + 1]) {
      get_variance(&vt.split[i].part_variances.none);
      if (vt.split[i].part_variances.none.variance > thresholds[1]) {
        force_split[i + 1] = 1;
        force_split[0] = 1;
      }
    }
  }
  if (!force_split[0]) {
    fill_variance_tree(&vt, BLOCK_64X64);
    get_variance(&vt.part_variances.none);
  }

  // Now go through the entire structure, splitting every block size until
  // we get to one that's got a variance lower than our threshold.
  if ( mi_col + 8 > cm->mi_cols || mi_row + 8 > cm->mi_rows ||
      !set_vt_partitioning(cpi, x, xd, &vt, BLOCK_64X64, mi_row, mi_col,
                           thresholds[0], BLOCK_16X16, force_split[0])) {
    for (i = 0; i < 4; ++i) {
      const int x32_idx = ((i & 1) << 2);
      const int y32_idx = ((i >> 1) << 2);
      const int i2 = i << 2;
      if (!set_vt_partitioning(cpi, x, xd, &vt.split[i], BLOCK_32X32,
                               (mi_row + y32_idx), (mi_col + x32_idx),
                               thresholds[1], BLOCK_16X16,
                               force_split[i + 1])) {
        for (j = 0; j < 4; ++j) {
          const int x16_idx = ((j & 1) << 1);
          const int y16_idx = ((j >> 1) << 1);
          // For inter frames: if variance4x4downsample[] == 1 for this 16x16
          // block, then the variance is based on 4x4 down-sampling, so use vt2
          // in set_vt_partioning(), otherwise use vt.
          v16x16 *vtemp = (!is_key_frame &&
                           variance4x4downsample[i2 + j] == 1) ?
                           &vt2[i2 + j] : &vt.split[i].split[j];
          if (!set_vt_partitioning(cpi, x, xd, vtemp, BLOCK_16X16,
                                   mi_row + y32_idx + y16_idx,
                                   mi_col + x32_idx + x16_idx,
                                   thresholds[2],
                                   cpi->vbp_bsize_min,
                                   force_split[5 + i2  + j])) {
            for (k = 0; k < 4; ++k) {
              const int x8_idx = (k & 1);
              const int y8_idx = (k >> 1);
              if (use_4x4_partition) {
                if (!set_vt_partitioning(cpi, x, xd, &vtemp->split[k],
                                         BLOCK_8X8,
                                         mi_row + y32_idx + y16_idx + y8_idx,
                                         mi_col + x32_idx + x16_idx + x8_idx,
                                         thresholds[3], BLOCK_8X8, 0)) {
                  set_block_size(cpi, x, xd,
                                 (mi_row + y32_idx + y16_idx + y8_idx),
                                 (mi_col + x32_idx + x16_idx + x8_idx),
                                 BLOCK_4X4);
                }
              } else {
                set_block_size(cpi, x, xd,
                               (mi_row + y32_idx + y16_idx + y8_idx),
                               (mi_col + x32_idx + x16_idx + x8_idx),
                               BLOCK_8X8);
              }
            }
          }
        }
      }
    }
  }
  return 0;
}

static void update_state(VP10_COMP *cpi, ThreadData *td,
                         PICK_MODE_CONTEXT *ctx,
                         int mi_row, int mi_col, BLOCK_SIZE bsize,
                         int output_enabled) {
  int i, x_idx, y;
  VP10_COMMON *const cm = &cpi->common;
  RD_COUNTS *const rdc = &td->rd_counts;
  MACROBLOCK *const x = &td->mb;
  MACROBLOCKD *const xd = &x->e_mbd;
  struct macroblock_plane *const p = x->plane;
  struct macroblockd_plane *const pd = xd->plane;
  MODE_INFO *mi = &ctx->mic;
  MB_MODE_INFO *const mbmi = &xd->mi[0]->mbmi;
  MODE_INFO *mi_addr = xd->mi[0];
  const struct segmentation *const seg = &cm->seg;
  const int bw = num_8x8_blocks_wide_lookup[mi->mbmi.sb_type];
  const int bh = num_8x8_blocks_high_lookup[mi->mbmi.sb_type];
  const int x_mis = VPXMIN(bw, cm->mi_cols - mi_col);
  const int y_mis = VPXMIN(bh, cm->mi_rows - mi_row);
  MV_REF *const frame_mvs =
      cm->cur_frame->mvs + mi_row * cm->mi_cols + mi_col;
  int w, h;

  const int mis = cm->mi_stride;
  const int mi_width = num_8x8_blocks_wide_lookup[bsize];
  const int mi_height = num_8x8_blocks_high_lookup[bsize];
  int max_plane;

#if !CONFIG_SUPERTX
  assert(mi->mbmi.sb_type == bsize);
#endif

  *mi_addr = *mi;
  *x->mbmi_ext = ctx->mbmi_ext;

  // If segmentation in use
  if (seg->enabled) {
    // For in frame complexity AQ copy the segment id from the segment map.
    if (cpi->oxcf.aq_mode == COMPLEXITY_AQ) {
      const uint8_t *const map = seg->update_map ? cpi->segmentation_map
                                                 : cm->last_frame_seg_map;
      mi_addr->mbmi.segment_id =
        get_segment_id(cm, map, bsize, mi_row, mi_col);
    }
    // Else for cyclic refresh mode update the segment map, set the segment id
    // and then update the quantizer.
    if (cpi->oxcf.aq_mode == CYCLIC_REFRESH_AQ) {
      vp10_cyclic_refresh_update_segment(cpi, &xd->mi[0]->mbmi, mi_row,
                                         mi_col, bsize, ctx->rate, ctx->dist,
                                         x->skip);
    }
  }

  max_plane = is_inter_block(mbmi) ? MAX_MB_PLANE : 1;
  for (i = 0; i < max_plane; ++i) {
    p[i].coeff = ctx->coeff_pbuf[i][1];
    p[i].qcoeff = ctx->qcoeff_pbuf[i][1];
    pd[i].dqcoeff = ctx->dqcoeff_pbuf[i][1];
    p[i].eobs = ctx->eobs_pbuf[i][1];
  }

  for (i = max_plane; i < MAX_MB_PLANE; ++i) {
    p[i].coeff = ctx->coeff_pbuf[i][2];
    p[i].qcoeff = ctx->qcoeff_pbuf[i][2];
    pd[i].dqcoeff = ctx->dqcoeff_pbuf[i][2];
    p[i].eobs = ctx->eobs_pbuf[i][2];
  }

  for (i = 0; i < 2; ++i)
    pd[i].color_index_map = ctx->color_index_map[i];

  // Restore the coding context of the MB to that that was in place
  // when the mode was picked for it
  for (y = 0; y < mi_height; y++)
    for (x_idx = 0; x_idx < mi_width; x_idx++)
      if ((xd->mb_to_right_edge >> (3 + MI_SIZE_LOG2)) + mi_width > x_idx
        && (xd->mb_to_bottom_edge >> (3 + MI_SIZE_LOG2)) + mi_height > y) {
        xd->mi[x_idx + y * mis] = mi_addr;
      }

  if (cpi->oxcf.aq_mode)
    vp10_init_plane_quantizers(cpi, x);

  if (is_inter_block(mbmi) && mbmi->sb_type < BLOCK_8X8) {
    mbmi->mv[0].as_int = mi->bmi[3].as_mv[0].as_int;
    mbmi->mv[1].as_int = mi->bmi[3].as_mv[1].as_int;
  }

  x->skip = ctx->skip;

#if CONFIG_VAR_TX
  for (i = 0; i < 1; ++i)
    memcpy(x->blk_skip[i], ctx->blk_skip[i],
           sizeof(uint8_t) * ctx->num_4x4_blk);
#endif
  memcpy(x->zcoeff_blk[mbmi->tx_size], ctx->zcoeff_blk,
         sizeof(ctx->zcoeff_blk[0]) * ctx->num_4x4_blk);

  if (!output_enabled)
    return;

#if CONFIG_INTERNAL_STATS
  if (frame_is_intra_only(cm)) {
    static const int kf_mode_index[] = {
      THR_DC        /*DC_PRED*/,
      THR_V_PRED    /*V_PRED*/,
      THR_H_PRED    /*H_PRED*/,
      THR_D45_PRED  /*D45_PRED*/,
      THR_D135_PRED /*D135_PRED*/,
      THR_D117_PRED /*D117_PRED*/,
      THR_D153_PRED /*D153_PRED*/,
      THR_D207_PRED /*D207_PRED*/,
      THR_D63_PRED  /*D63_PRED*/,
      THR_TM        /*TM_PRED*/,
    };
    ++cpi->mode_chosen_counts[kf_mode_index[mbmi->mode]];
  } else {
    // Note how often each mode chosen as best
    ++cpi->mode_chosen_counts[ctx->best_mode_index];
  }
#endif
  if (!frame_is_intra_only(cm)) {
    if (is_inter_block(mbmi)) {
      vp10_update_mv_count(td);
      if (cm->interp_filter == SWITCHABLE
#if CONFIG_EXT_INTERP
          && vp10_is_interp_needed(xd)
#endif
          ) {
        const int ctx = vp10_get_pred_context_switchable_interp(xd);
        ++td->counts->switchable_interp[ctx][mbmi->interp_filter];
      }
    }

    rdc->comp_pred_diff[SINGLE_REFERENCE] += ctx->single_pred_diff;
    rdc->comp_pred_diff[COMPOUND_REFERENCE] += ctx->comp_pred_diff;
    rdc->comp_pred_diff[REFERENCE_MODE_SELECT] += ctx->hybrid_pred_diff;

    for (i = 0; i < SWITCHABLE_FILTER_CONTEXTS; ++i)
      rdc->filter_diff[i] += ctx->best_filter_diff[i];
  }

  for (h = 0; h < y_mis; ++h) {
    MV_REF *const frame_mv = frame_mvs + h * cm->mi_cols;
    for (w = 0; w < x_mis; ++w) {
      MV_REF *const mv = frame_mv + w;
      mv->ref_frame[0] = mi->mbmi.ref_frame[0];
      mv->ref_frame[1] = mi->mbmi.ref_frame[1];
      mv->mv[0].as_int = mi->mbmi.mv[0].as_int;
      mv->mv[1].as_int = mi->mbmi.mv[1].as_int;
    }
  }
}

#if CONFIG_SUPERTX
static void update_state_supertx(VP10_COMP *cpi, ThreadData *td,
                                 PICK_MODE_CONTEXT *ctx,
                                 int mi_row, int mi_col, BLOCK_SIZE bsize,
                                 int output_enabled) {
  int i, y, x_idx;
  VP10_COMMON *const cm = &cpi->common;
  RD_COUNTS *const rdc = &td->rd_counts;
  MACROBLOCK *const x = &td->mb;
  MACROBLOCKD *const xd = &x->e_mbd;
  MODE_INFO *mi = &ctx->mic;
  MB_MODE_INFO *const mbmi = &xd->mi[0]->mbmi;
  MODE_INFO *mi_addr = xd->mi[0];
  const struct segmentation *const seg = &cm->seg;
  const int mis = cm->mi_stride;
  const int mi_width = num_8x8_blocks_wide_lookup[bsize];
  const int mi_height = num_8x8_blocks_high_lookup[bsize];
  const int x_mis = VPXMIN(mi_width, cm->mi_cols - mi_col);
  const int y_mis = VPXMIN(mi_height, cm->mi_rows - mi_row);
  MV_REF *const frame_mvs =
      cm->cur_frame->mvs + mi_row * cm->mi_cols + mi_col;
  int w, h;

  *mi_addr = *mi;
  *x->mbmi_ext = ctx->mbmi_ext;
  assert(is_inter_block(mbmi));
  assert(mbmi->tx_size == ctx->mic.mbmi.tx_size);

  // If segmentation in use
  if (seg->enabled && output_enabled) {
    // For in frame complexity AQ copy the segment id from the segment map.
    if (cpi->oxcf.aq_mode == COMPLEXITY_AQ) {
      const uint8_t *const map = seg->update_map ? cpi->segmentation_map
                                                 : cm->last_frame_seg_map;
      mi_addr->mbmi.segment_id =
        get_segment_id(cm, map, bsize, mi_row, mi_col);
    } else if (cpi->oxcf.aq_mode == CYCLIC_REFRESH_AQ) {
      // Else for cyclic refresh mode update the segment map, set the segment id
      // and then update the quantizer.
      vp10_cyclic_refresh_update_segment(cpi, &xd->mi[0]->mbmi,
                                         mi_row, mi_col, bsize,
                                         ctx->rate, ctx->dist, 1);
      vp10_init_plane_quantizers(cpi, x);
    }
  }

  // Restore the coding context of the MB to that that was in place
  // when the mode was picked for it
  for (y = 0; y < mi_height; y++)
    for (x_idx = 0; x_idx < mi_width; x_idx++)
      if ((xd->mb_to_right_edge >> (3 + MI_SIZE_LOG2)) + mi_width > x_idx
        && (xd->mb_to_bottom_edge >> (3 + MI_SIZE_LOG2)) + mi_height > y) {
        xd->mi[x_idx + y * mis] = mi_addr;
      }

  if (cpi->oxcf.aq_mode)
    vp10_init_plane_quantizers(cpi, x);

  if (is_inter_block(mbmi) && mbmi->sb_type < BLOCK_8X8) {
    mbmi->mv[0].as_int = mi->bmi[3].as_mv[0].as_int;
    mbmi->mv[1].as_int = mi->bmi[3].as_mv[1].as_int;
  }

  x->skip = ctx->skip;

#if CONFIG_VAR_TX
  for (i = 0; i < 1; ++i)
    memcpy(x->blk_skip[i], ctx->blk_skip[i],
           sizeof(uint8_t) * ctx->num_4x4_blk);
#endif  // CONFIG_VAR_TX
  memcpy(x->zcoeff_blk[mbmi->tx_size], ctx->zcoeff_blk,
         sizeof(uint8_t) * ctx->num_4x4_blk);

#if CONFIG_VAR_TX
  {
    const TX_SIZE mtx = mbmi->tx_size;
    int idy, idx;
    for (idy = 0; idy < (1 << mtx) / 2; ++idy)
      for (idx = 0; idx < (1 << mtx) / 2; ++idx)
        mbmi->inter_tx_size[(idy << 3) + idx] = mbmi->tx_size;
  }
#endif  // CONFIG_VAR_TX

  if (!output_enabled)
    return;

  if (!frame_is_intra_only(cm)) {
    vp10_update_mv_count(td);

    if (cm->interp_filter == SWITCHABLE
#if CONFIG_EXT_INTERP
        && vp10_is_interp_needed(xd)
#endif
        ) {
      const int ctx = vp10_get_pred_context_switchable_interp(xd);
      ++td->counts->switchable_interp[ctx][mbmi->interp_filter];
    }

    rdc->comp_pred_diff[SINGLE_REFERENCE] += ctx->single_pred_diff;
    rdc->comp_pred_diff[COMPOUND_REFERENCE] += ctx->comp_pred_diff;
    rdc->comp_pred_diff[REFERENCE_MODE_SELECT] += ctx->hybrid_pred_diff;

    for (i = 0; i < SWITCHABLE_FILTER_CONTEXTS; ++i)
      rdc->filter_diff[i] += ctx->best_filter_diff[i];
  }

  for (h = 0; h < y_mis; ++h) {
    MV_REF *const frame_mv = frame_mvs + h * cm->mi_cols;
    for (w = 0; w < x_mis; ++w) {
      MV_REF *const mv = frame_mv + w;
      mv->ref_frame[0] = mi->mbmi.ref_frame[0];
      mv->ref_frame[1] = mi->mbmi.ref_frame[1];
      mv->mv[0].as_int = mi->mbmi.mv[0].as_int;
      mv->mv[1].as_int = mi->mbmi.mv[1].as_int;
    }
  }
}

static void update_state_sb_supertx(VP10_COMP *cpi, ThreadData *td,
                                    const TileInfo *const tile,
                                    int mi_row, int mi_col,
                                    BLOCK_SIZE bsize,
                                    int output_enabled, PC_TREE *pc_tree) {
  VP10_COMMON *const cm = &cpi->common;
  MACROBLOCK *const x = &td->mb;
  MACROBLOCKD *const xd = &x->e_mbd;
  struct macroblock_plane *const p = x->plane;
  struct macroblockd_plane *const pd = xd->plane;
  int bsl = b_width_log2_lookup[bsize], hbs = (1 << bsl) / 4;
  PARTITION_TYPE partition = pc_tree->partitioning;
  BLOCK_SIZE subsize = get_subsize(bsize, partition);
  int i;
  PICK_MODE_CONTEXT *pmc = NULL;

  if (mi_row >= cm->mi_rows || mi_col >= cm->mi_cols)
    return;

  switch (partition) {
    case PARTITION_NONE:
      set_offsets_supertx(cpi, td, tile, mi_row, mi_col, subsize);
      update_state_supertx(cpi, td, &pc_tree->none, mi_row, mi_col,
                           subsize, output_enabled);
      break;
    case PARTITION_VERT:
      set_offsets_supertx(cpi, td, tile, mi_row, mi_col, subsize);
      update_state_supertx(cpi, td, &pc_tree->vertical[0], mi_row, mi_col,
                           subsize, output_enabled);
      if (mi_col + hbs < cm->mi_cols && bsize > BLOCK_8X8) {
        set_offsets_supertx(cpi, td, tile, mi_row, mi_col + hbs, subsize);
        update_state_supertx(cpi, td, &pc_tree->vertical[1],
                             mi_row, mi_col + hbs, subsize, output_enabled);
      }
      pmc = &pc_tree->vertical_supertx;
      break;
    case PARTITION_HORZ:
      set_offsets_supertx(cpi, td, tile, mi_row, mi_col, subsize);
      update_state_supertx(cpi, td, &pc_tree->horizontal[0], mi_row, mi_col,
                           subsize, output_enabled);
      if (mi_row + hbs < cm->mi_rows && bsize > BLOCK_8X8) {
        set_offsets_supertx(cpi, td, tile, mi_row + hbs, mi_col, subsize);
        update_state_supertx(cpi, td, &pc_tree->horizontal[1], mi_row + hbs,
                             mi_col, subsize, output_enabled);
      }
      pmc = &pc_tree->horizontal_supertx;
      break;
    case PARTITION_SPLIT:
      if (bsize == BLOCK_8X8) {
        set_offsets_supertx(cpi, td, tile, mi_row, mi_col, subsize);
        update_state_supertx(cpi, td, pc_tree->leaf_split[0], mi_row, mi_col,
                             subsize, output_enabled);
      } else {
        set_offsets_supertx(cpi, td, tile, mi_row, mi_col, subsize);
        update_state_sb_supertx(cpi, td, tile, mi_row, mi_col, subsize,
                                output_enabled, pc_tree->split[0]);
        set_offsets_supertx(cpi, td, tile, mi_row, mi_col + hbs, subsize);
        update_state_sb_supertx(cpi, td, tile, mi_row, mi_col + hbs, subsize,
                                output_enabled, pc_tree->split[1]);
        set_offsets_supertx(cpi, td, tile, mi_row + hbs, mi_col, subsize);
        update_state_sb_supertx(cpi, td, tile, mi_row + hbs, mi_col, subsize,
                                output_enabled, pc_tree->split[2]);
        set_offsets_supertx(cpi, td, tile, mi_row + hbs, mi_col + hbs, subsize);
        update_state_sb_supertx(cpi, td, tile, mi_row + hbs, mi_col + hbs,
                                subsize, output_enabled, pc_tree->split[3]);
      }
      pmc = &pc_tree->split_supertx;
      break;
    default:
      assert(0);
  }

  for (i = 0; i < MAX_MB_PLANE; ++i) {
    if (pmc != NULL) {
      p[i].coeff = pmc->coeff_pbuf[i][1];
      p[i].qcoeff = pmc->qcoeff_pbuf[i][1];
      pd[i].dqcoeff = pmc->dqcoeff_pbuf[i][1];
      p[i].eobs = pmc->eobs_pbuf[i][1];
    } else {
      // These should never be used
      p[i].coeff = NULL;
      p[i].qcoeff = NULL;
      pd[i].dqcoeff = NULL;
      p[i].eobs = NULL;
    }
  }
}

static void update_supertx_param(ThreadData *td,
                                 PICK_MODE_CONTEXT *ctx,
                                 int best_tx,
                                 TX_SIZE supertx_size) {
  MACROBLOCK *const x = &td->mb;
#if CONFIG_VAR_TX
  int i;

  for (i = 0; i < 1; ++i)
    memcpy(ctx->blk_skip[i], x->blk_skip[i],
           sizeof(uint8_t) * ctx->num_4x4_blk);
#endif  // CONFIG_VAR_TX
  memcpy(ctx->zcoeff_blk, x->zcoeff_blk[supertx_size],
         sizeof(uint8_t) * ctx->num_4x4_blk);
  ctx->mic.mbmi.tx_size = supertx_size;
  ctx->skip = x->skip;
  ctx->mic.mbmi.tx_type = best_tx;
}

static void update_supertx_param_sb(VP10_COMP *cpi, ThreadData *td,
                                    int mi_row, int mi_col,
                                    BLOCK_SIZE bsize,
                                    int best_tx,
                                    TX_SIZE supertx_size, PC_TREE *pc_tree) {
  VP10_COMMON *const cm = &cpi->common;
  int bsl = b_width_log2_lookup[bsize], hbs = (1 << bsl) / 4;
  PARTITION_TYPE partition = pc_tree->partitioning;
  BLOCK_SIZE subsize = get_subsize(bsize, partition);

  if (mi_row >= cm->mi_rows || mi_col >= cm->mi_cols)
    return;

  switch (partition) {
    case PARTITION_NONE:
      update_supertx_param(td, &pc_tree->none,
                           best_tx,
                           supertx_size);
      break;
    case PARTITION_VERT:
      update_supertx_param(td, &pc_tree->vertical[0],
                           best_tx,
                           supertx_size);
      if (mi_col + hbs < cm->mi_cols && bsize > BLOCK_8X8)
        update_supertx_param(td, &pc_tree->vertical[1],
                             best_tx,
                             supertx_size);
      break;
    case PARTITION_HORZ:
      update_supertx_param(td, &pc_tree->horizontal[0],
                           best_tx,
                           supertx_size);
      if (mi_row + hbs < cm->mi_rows && bsize > BLOCK_8X8)
        update_supertx_param(td, &pc_tree->horizontal[1],
                             best_tx,
                             supertx_size);
      break;
    case PARTITION_SPLIT:
      if (bsize == BLOCK_8X8) {
        update_supertx_param(td, pc_tree->leaf_split[0],
                             best_tx,
                             supertx_size);
      } else {
        update_supertx_param_sb(cpi, td, mi_row, mi_col, subsize,
                                best_tx,
                                supertx_size, pc_tree->split[0]);
        update_supertx_param_sb(cpi, td, mi_row, mi_col + hbs, subsize,
                                best_tx,
                                supertx_size, pc_tree->split[1]);
        update_supertx_param_sb(cpi, td, mi_row + hbs, mi_col, subsize,
                                best_tx,
                                supertx_size, pc_tree->split[2]);
        update_supertx_param_sb(cpi, td, mi_row + hbs, mi_col + hbs, subsize,
                                best_tx,
                                supertx_size, pc_tree->split[3]);
      }
      break;
    default:
      assert(0);
  }
}
#endif  // CONFIG_SUPERTX

void vp10_setup_src_planes(MACROBLOCK *x, const YV12_BUFFER_CONFIG *src,
                          int mi_row, int mi_col) {
  uint8_t *const buffers[3] = {src->y_buffer, src->u_buffer, src->v_buffer };
  const int strides[3] = {src->y_stride, src->uv_stride, src->uv_stride };
  int i;

  // Set current frame pointer.
  x->e_mbd.cur_buf = src;

  for (i = 0; i < MAX_MB_PLANE; i++)
    setup_pred_plane(&x->plane[i].src, buffers[i], strides[i], mi_row, mi_col,
                     NULL, x->e_mbd.plane[i].subsampling_x,
                     x->e_mbd.plane[i].subsampling_y);
}

static int set_segment_rdmult(VP10_COMP *const cpi,
                               MACROBLOCK *const x,
                               int8_t segment_id) {
  int segment_qindex;
  VP10_COMMON *const cm = &cpi->common;
  vp10_init_plane_quantizers(cpi, x);
  vpx_clear_system_state();
  segment_qindex = vp10_get_qindex(&cm->seg, segment_id,
                                  cm->base_qindex);
  return vp10_compute_rd_mult(cpi, segment_qindex + cm->y_dc_delta_q);
}

static void rd_pick_sb_modes(VP10_COMP *cpi,
                             TileDataEnc *tile_data,
                             MACROBLOCK *const x,
                             int mi_row, int mi_col, RD_COST *rd_cost,
#if CONFIG_SUPERTX
                             int *totalrate_nocoef,
#endif
                             BLOCK_SIZE bsize, PICK_MODE_CONTEXT *ctx,
                             int64_t best_rd) {
  VP10_COMMON *const cm = &cpi->common;
  TileInfo *const tile_info = &tile_data->tile_info;
  MACROBLOCKD *const xd = &x->e_mbd;
  MB_MODE_INFO *mbmi;
  struct macroblock_plane *const p = x->plane;
  struct macroblockd_plane *const pd = xd->plane;
  const AQ_MODE aq_mode = cpi->oxcf.aq_mode;
  int i, orig_rdmult;

  vpx_clear_system_state();

  // Use the lower precision, but faster, 32x32 fdct for mode selection.
  x->use_lp32x32fdct = 1;

  set_offsets(cpi, tile_info, x, mi_row, mi_col, bsize);
  mbmi = &xd->mi[0]->mbmi;
  mbmi->sb_type = bsize;
#if CONFIG_SUPERTX
  // We set tx_size here as skip blocks would otherwise not set it.
  // tx_size needs to be set at this point as supertx_enable in
  // write_modes_sb is computed based on this, and if the garbage in memory
  // just happens to be the supertx_size, then the packer will code this
  // block as a supertx block, even if rdopt did not pick it as such.
  mbmi->tx_size = max_txsize_lookup[bsize];
#endif

  for (i = 0; i < MAX_MB_PLANE; ++i) {
    p[i].coeff = ctx->coeff_pbuf[i][0];
    p[i].qcoeff = ctx->qcoeff_pbuf[i][0];
    pd[i].dqcoeff = ctx->dqcoeff_pbuf[i][0];
    p[i].eobs = ctx->eobs_pbuf[i][0];
  }

  if (cm->current_video_frame == 0 && cm->allow_screen_content_tools) {
    for (i = 0; i < 2; ++i) {
      if (ctx->color_index_map[i] == 0) {
        CHECK_MEM_ERROR(cm, ctx->color_index_map[i],
                        vpx_memalign(16, (ctx->num_4x4_blk << 4) *
                                     sizeof(*ctx->color_index_map[i])));
      }
    }
  }
  for (i = 0; i < 2; ++i)
    pd[i].color_index_map = ctx->color_index_map[i];

  ctx->is_coded = 0;
  ctx->skippable = 0;
  ctx->pred_pixel_ready = 0;
  x->skip_recode = 0;

  // Set to zero to make sure we do not use the previous encoded frame stats
  mbmi->skip = 0;

#if CONFIG_VP9_HIGHBITDEPTH
  if (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) {
    x->source_variance =
        vp10_high_get_sby_perpixel_variance(cpi, &x->plane[0].src,
                                            bsize, xd->bd);
  } else {
    x->source_variance =
      vp10_get_sby_perpixel_variance(cpi, &x->plane[0].src, bsize);
  }
#else
  x->source_variance =
    vp10_get_sby_perpixel_variance(cpi, &x->plane[0].src, bsize);
#endif  // CONFIG_VP9_HIGHBITDEPTH

  // Save rdmult before it might be changed, so it can be restored later.
  orig_rdmult = x->rdmult;

  if (aq_mode == VARIANCE_AQ) {
    const int energy = bsize <= BLOCK_16X16 ? x->mb_energy
                                            : vp10_block_energy(cpi, x, bsize);
    if (cm->frame_type == KEY_FRAME ||
        cpi->refresh_alt_ref_frame ||
        (cpi->refresh_golden_frame && !cpi->rc.is_src_frame_alt_ref)) {
      mbmi->segment_id = vp10_vaq_segment_id(energy);
    } else {
      const uint8_t *const map = cm->seg.update_map ? cpi->segmentation_map
                                                    : cm->last_frame_seg_map;
      mbmi->segment_id = get_segment_id(cm, map, bsize, mi_row, mi_col);
    }
    x->rdmult = set_segment_rdmult(cpi, x, mbmi->segment_id);
  } else if (aq_mode == COMPLEXITY_AQ) {
    x->rdmult = set_segment_rdmult(cpi, x, mbmi->segment_id);
  } else if (aq_mode == CYCLIC_REFRESH_AQ) {
    const uint8_t *const map = cm->seg.update_map ? cpi->segmentation_map
                                                  : cm->last_frame_seg_map;
    // If segment is boosted, use rdmult for that segment.
    if (cyclic_refresh_segment_id_boosted(
            get_segment_id(cm, map, bsize, mi_row, mi_col)))
      x->rdmult = vp10_cyclic_refresh_get_rdmult(cpi->cyclic_refresh);
  }

  // Find best coding mode & reconstruct the MB so it is available
  // as a predictor for MBs that follow in the SB
  if (frame_is_intra_only(cm)) {
    vp10_rd_pick_intra_mode_sb(cpi, x, rd_cost, bsize, ctx, best_rd);
#if CONFIG_SUPERTX
    *totalrate_nocoef = 0;
#endif  // CONFIG_SUPERTX
  } else {
    if (bsize >= BLOCK_8X8) {
      if (segfeature_active(&cm->seg, mbmi->segment_id, SEG_LVL_SKIP)) {
        vp10_rd_pick_inter_mode_sb_seg_skip(cpi, tile_data, x, rd_cost, bsize,
                                           ctx, best_rd);
#if CONFIG_SUPERTX
        *totalrate_nocoef = rd_cost->rate;
#endif  // CONFIG_SUPERTX
      } else {
        vp10_rd_pick_inter_mode_sb(cpi, tile_data, x, mi_row, mi_col, rd_cost,
#if CONFIG_SUPERTX
                                   totalrate_nocoef,
#endif  // CONFIG_SUPERTX
                                   bsize, ctx, best_rd);
      }
    } else {
      vp10_rd_pick_inter_mode_sub8x8(cpi, tile_data, x, mi_row, mi_col, rd_cost,
#if CONFIG_SUPERTX
                                     totalrate_nocoef,
#endif  // CONFIG_SUPERTX
                                     bsize, ctx, best_rd);
    }
  }


  // Examine the resulting rate and for AQ mode 2 make a segment choice.
  if ((rd_cost->rate != INT_MAX) &&
      (aq_mode == COMPLEXITY_AQ) && (bsize >= BLOCK_16X16) &&
      (cm->frame_type == KEY_FRAME ||
       cpi->refresh_alt_ref_frame ||
       (cpi->refresh_golden_frame && !cpi->rc.is_src_frame_alt_ref))) {
    vp10_caq_select_segment(cpi, x, bsize, mi_row, mi_col, rd_cost->rate);
  }

  x->rdmult = orig_rdmult;

  // TODO(jingning) The rate-distortion optimization flow needs to be
  // refactored to provide proper exit/return handle.
  if (rd_cost->rate == INT_MAX)
    rd_cost->rdcost = INT64_MAX;

  ctx->rate = rd_cost->rate;
  ctx->dist = rd_cost->dist;
}

#if CONFIG_REF_MV
static void update_inter_mode_stats(FRAME_COUNTS *counts,
                                    PREDICTION_MODE mode,
#if CONFIG_EXT_INTER
                                    int is_compound,
#endif  // CONFIG_EXT_INTER
                                    int16_t mode_context) {
  int16_t mode_ctx = mode_context & NEWMV_CTX_MASK;
#if CONFIG_EXT_INTER
  if (mode == NEWMV || mode == NEWFROMNEARMV) {
    if (!is_compound)
      ++counts->new2mv_mode[mode == NEWFROMNEARMV];
#else
  if (mode == NEWMV) {
#endif  // CONFIG_EXT_INTER
    ++counts->newmv_mode[mode_ctx][0];
    return;
  } else {
    ++counts->newmv_mode[mode_ctx][1];

    if (mode_context & (1 << ALL_ZERO_FLAG_OFFSET)) {
      return;
    }

    mode_ctx = (mode_context >> ZEROMV_OFFSET) & ZEROMV_CTX_MASK;
    if (mode == ZEROMV) {
      ++counts->zeromv_mode[mode_ctx][0];
      return;
    } else {
      ++counts->zeromv_mode[mode_ctx][1];
      mode_ctx = (mode_context >> REFMV_OFFSET) & REFMV_CTX_MASK;

      if (mode_context & (1 << SKIP_NEARESTMV_OFFSET))
        mode_ctx = 6;
      if (mode_context & (1 << SKIP_NEARMV_OFFSET))
        mode_ctx = 7;
      if (mode_context & (1 << SKIP_NEARESTMV_SUB8X8_OFFSET))
        mode_ctx = 8;

      ++counts->refmv_mode[mode_ctx][mode != NEARESTMV];
    }
  }
}
#endif

static void update_stats(VP10_COMMON *cm, ThreadData *td
#if CONFIG_SUPERTX
                         , int supertx_enabled
#endif
                         ) {
  const MACROBLOCK *x = &td->mb;
  const MACROBLOCKD *const xd = &x->e_mbd;
  const MODE_INFO *const mi = xd->mi[0];
  const MB_MODE_INFO *const mbmi = &mi->mbmi;
  const MB_MODE_INFO_EXT *const mbmi_ext = x->mbmi_ext;
  const BLOCK_SIZE bsize = mbmi->sb_type;

  if (!frame_is_intra_only(cm)) {
    FRAME_COUNTS *const counts = td->counts;
    const int inter_block = is_inter_block(mbmi);
    const int seg_ref_active = segfeature_active(&cm->seg, mbmi->segment_id,
                                                 SEG_LVL_REF_FRAME);
    if (!seg_ref_active) {
#if CONFIG_SUPERTX
      if (!supertx_enabled)
#endif
      counts->intra_inter[vp10_get_intra_inter_context(xd)][inter_block]++;
      // If the segment reference feature is enabled we have only a single
      // reference frame allowed for the segment so exclude it from
      // the reference frame counts used to work out probabilities.
      if (inter_block) {
        const MV_REFERENCE_FRAME ref0 = mbmi->ref_frame[0];
        if (cm->reference_mode == REFERENCE_MODE_SELECT)
          counts->comp_inter[vp10_get_reference_mode_context(cm, xd)]
                            [has_second_ref(mbmi)]++;

        if (has_second_ref(mbmi)) {
#if CONFIG_EXT_REFS
          const int bit = (ref0 == GOLDEN_FRAME || ref0 == LAST3_FRAME ||
                           ref0 == LAST4_FRAME);
          counts->comp_ref[vp10_get_pred_context_comp_ref_p(cm, xd)][0][bit]++;
          if (!bit) {
            counts->comp_ref[vp10_get_pred_context_comp_ref_p1(cm, xd)][1]
                            [ref0 == LAST_FRAME]++;
          } else {
            counts->comp_ref[vp10_get_pred_context_comp_ref_p2(cm, xd)][2]
                            [ref0 == GOLDEN_FRAME]++;
            if (ref0 != GOLDEN_FRAME) {
              counts->comp_ref[vp10_get_pred_context_comp_ref_p3(cm, xd)][3]
                              [ref0 == LAST3_FRAME]++;
            }
          }
#else
          counts->comp_ref[vp10_get_pred_context_comp_ref_p(cm, xd)][0]
                          [ref0 == GOLDEN_FRAME]++;
#endif  // CONFIG_EXT_REFS
        } else {
#if CONFIG_EXT_REFS
          const int bit = (ref0 == ALTREF_FRAME || ref0 == GOLDEN_FRAME);
          counts->single_ref[vp10_get_pred_context_single_ref_p1(xd)][0][bit]++;
          if (bit) {
            counts->single_ref[vp10_get_pred_context_single_ref_p2(xd)][1]
                              [ref0 != GOLDEN_FRAME]++;
          } else {
            const int bit1 = !(ref0 == LAST2_FRAME || ref0 == LAST_FRAME);
            counts->single_ref[vp10_get_pred_context_single_ref_p3(xd)][2]
                              [bit1]++;
            if (!bit1) {
              counts->single_ref[vp10_get_pred_context_single_ref_p4(xd)][3]
                                [ref0 != LAST_FRAME]++;
            } else {
              counts->single_ref[vp10_get_pred_context_single_ref_p5(xd)][4]
                                [ref0 != LAST3_FRAME]++;
            }
          }
#else
          counts->single_ref[vp10_get_pred_context_single_ref_p1(xd)][0]
                            [ref0 != LAST_FRAME]++;
          if (ref0 != LAST_FRAME)
            counts->single_ref[vp10_get_pred_context_single_ref_p2(xd)][1]
                              [ref0 != GOLDEN_FRAME]++;
#endif  // CONFIG_EXT_REFS
        }
#if CONFIG_OBMC
#if CONFIG_SUPERTX
        if (!supertx_enabled)
#endif  // CONFIG_SUPERTX
        if (is_obmc_allowed(mbmi))
          counts->obmc[mbmi->sb_type][mbmi->obmc]++;
#endif  // CONFIG_OBMC
      }
    }

#if CONFIG_EXT_INTER
    if (cm->reference_mode != COMPOUND_REFERENCE &&
#if CONFIG_SUPERTX
       !supertx_enabled &&
#endif
       is_interintra_allowed(mbmi)) {
      if (mbmi->ref_frame[1] == INTRA_FRAME) {
        counts->y_mode[size_group_lookup[bsize]][mbmi->interintra_mode]++;
        counts->interintra[bsize][1]++;
      } else {
        counts->interintra[bsize][0]++;
      }
    }
#endif  // CONFIG_EXT_INTER

    if (inter_block &&
        !segfeature_active(&cm->seg, mbmi->segment_id, SEG_LVL_SKIP)) {
      int16_t mode_ctx = mbmi_ext->mode_context[mbmi->ref_frame[0]];
      if (bsize >= BLOCK_8X8) {
        const PREDICTION_MODE mode = mbmi->mode;
#if CONFIG_REF_MV
#if CONFIG_EXT_INTER
        if (has_second_ref(mbmi)) {
          mode_ctx = mbmi_ext->compound_mode_context[mbmi->ref_frame[0]];
          ++counts->inter_compound_mode[mode_ctx][INTER_COMPOUND_OFFSET(mode)];
        } else {
#endif  // CONFIG_EXT_INTER
        mode_ctx = vp10_mode_context_analyzer(mbmi_ext->mode_context,
                                              mbmi->ref_frame, bsize, -1);
        update_inter_mode_stats(counts, mode,
#if CONFIG_EXT_INTER
                                has_second_ref(mbmi),
#endif  // CONFIG_EXT_INTER
                                mode_ctx);

        if (mode == NEARMV) {
          uint8_t ref_frame_type = vp10_ref_frame_type(mbmi->ref_frame);
          if (mbmi_ext->ref_mv_count[ref_frame_type] > 2) {
            uint8_t drl0_ctx =
                vp10_drl_ctx(mbmi_ext->ref_mv_stack[ref_frame_type], 1);
            if (mbmi->ref_mv_idx == 0)
              ++counts->drl_mode0[drl0_ctx][0];
            else
              ++counts->drl_mode0[drl0_ctx][1];

            if (mbmi_ext->ref_mv_count[ref_frame_type] > 3 &&
                mbmi->ref_mv_idx > 0) {
              uint8_t drl1_ctx =
                  vp10_drl_ctx(mbmi_ext->ref_mv_stack[ref_frame_type], 2);
              if (mbmi->ref_mv_idx == 1)
                ++counts->drl_mode1[drl1_ctx][0];
              else
                ++counts->drl_mode1[drl1_ctx][1];
            }
          }
        }
#if CONFIG_EXT_INTER
        }
#endif  // CONFIG_EXT_INTER
#else
#if CONFIG_EXT_INTER
        if (is_inter_compound_mode(mode))
          ++counts->inter_compound_mode[mode_ctx][INTER_COMPOUND_OFFSET(mode)];
        else
#endif  // CONFIG_EXT_INTER
        ++counts->inter_mode[mode_ctx][INTER_OFFSET(mode)];
#endif
      } else {
        const int num_4x4_w = num_4x4_blocks_wide_lookup[bsize];
        const int num_4x4_h = num_4x4_blocks_high_lookup[bsize];
        int idx, idy;
        for (idy = 0; idy < 2; idy += num_4x4_h) {
          for (idx = 0; idx < 2; idx += num_4x4_w) {
            const int j = idy * 2 + idx;
            const PREDICTION_MODE b_mode = mi->bmi[j].as_mode;
#if CONFIG_REF_MV
#if CONFIG_EXT_INTER
          if (has_second_ref(mbmi)) {
            mode_ctx = mbmi_ext->compound_mode_context[mbmi->ref_frame[0]];
            ++counts->inter_compound_mode[mode_ctx]
                                         [INTER_COMPOUND_OFFSET(b_mode)];
          } else {
#endif  // CONFIG_EXT_INTER
            mode_ctx = vp10_mode_context_analyzer(mbmi_ext->mode_context,
                                                  mbmi->ref_frame, bsize, j);
            update_inter_mode_stats(counts, b_mode,
#if CONFIG_EXT_INTER
                                    has_second_ref(mbmi),
#endif  // CONFIG_EXT_INTER
                                    mode_ctx);
#if CONFIG_EXT_INTER
            }
#endif  // CONFIG_EXT_INTER
#else
#if CONFIG_EXT_INTER
            if (is_inter_compound_mode(b_mode))
              ++counts->inter_compound_mode[mode_ctx]
                                           [INTER_COMPOUND_OFFSET(b_mode)];
            else
#endif  // CONFIG_EXT_INTER
            ++counts->inter_mode[mode_ctx][INTER_OFFSET(b_mode)];
#endif
          }
        }
      }
    }
  }
}

static void restore_context(MACROBLOCK *const x, int mi_row, int mi_col,
                            ENTROPY_CONTEXT a[16 * MAX_MB_PLANE],
                            ENTROPY_CONTEXT l[16 * MAX_MB_PLANE],
                            PARTITION_CONTEXT sa[8], PARTITION_CONTEXT sl[8],
#if CONFIG_VAR_TX
                            TXFM_CONTEXT ta[8], TXFM_CONTEXT tl[8],
#endif
                            BLOCK_SIZE bsize) {
  MACROBLOCKD *const xd = &x->e_mbd;
  int p;
  const int num_4x4_blocks_wide = num_4x4_blocks_wide_lookup[bsize];
  const int num_4x4_blocks_high = num_4x4_blocks_high_lookup[bsize];
  int mi_width = num_8x8_blocks_wide_lookup[bsize];
  int mi_height = num_8x8_blocks_high_lookup[bsize];
  for (p = 0; p < MAX_MB_PLANE; p++) {
    memcpy(
        xd->above_context[p] + ((mi_col * 2) >> xd->plane[p].subsampling_x),
        a + num_4x4_blocks_wide * p,
        (sizeof(ENTROPY_CONTEXT) * num_4x4_blocks_wide) >>
        xd->plane[p].subsampling_x);
    memcpy(
        xd->left_context[p]
            + ((mi_row & MI_MASK) * 2 >> xd->plane[p].subsampling_y),
        l + num_4x4_blocks_high * p,
        (sizeof(ENTROPY_CONTEXT) * num_4x4_blocks_high) >>
        xd->plane[p].subsampling_y);
  }
  memcpy(xd->above_seg_context + mi_col, sa,
         sizeof(*xd->above_seg_context) * mi_width);
  memcpy(xd->left_seg_context + (mi_row & MI_MASK), sl,
         sizeof(xd->left_seg_context[0]) * mi_height);
#if CONFIG_VAR_TX
  memcpy(xd->above_txfm_context, ta,
         sizeof(*xd->above_txfm_context) * mi_width);
  memcpy(xd->left_txfm_context, tl,
         sizeof(*xd->left_txfm_context) * mi_height);
#endif
}

static void save_context(MACROBLOCK *const x, int mi_row, int mi_col,
                         ENTROPY_CONTEXT a[16 * MAX_MB_PLANE],
                         ENTROPY_CONTEXT l[16 * MAX_MB_PLANE],
                         PARTITION_CONTEXT sa[8], PARTITION_CONTEXT sl[8],
#if CONFIG_VAR_TX
                         TXFM_CONTEXT ta[8], TXFM_CONTEXT tl[8],
#endif
                         BLOCK_SIZE bsize) {
  const MACROBLOCKD *const xd = &x->e_mbd;
  int p;
  const int num_4x4_blocks_wide = num_4x4_blocks_wide_lookup[bsize];
  const int num_4x4_blocks_high = num_4x4_blocks_high_lookup[bsize];
  int mi_width = num_8x8_blocks_wide_lookup[bsize];
  int mi_height = num_8x8_blocks_high_lookup[bsize];

  // buffer the above/left context information of the block in search.
  for (p = 0; p < MAX_MB_PLANE; ++p) {
    memcpy(
        a + num_4x4_blocks_wide * p,
        xd->above_context[p] + (mi_col * 2 >> xd->plane[p].subsampling_x),
        (sizeof(ENTROPY_CONTEXT) * num_4x4_blocks_wide) >>
        xd->plane[p].subsampling_x);
    memcpy(
        l + num_4x4_blocks_high * p,
        xd->left_context[p]
            + ((mi_row & MI_MASK) * 2 >> xd->plane[p].subsampling_y),
        (sizeof(ENTROPY_CONTEXT) * num_4x4_blocks_high) >>
        xd->plane[p].subsampling_y);
  }
  memcpy(sa, xd->above_seg_context + mi_col,
         sizeof(*xd->above_seg_context) * mi_width);
  memcpy(sl, xd->left_seg_context + (mi_row & MI_MASK),
         sizeof(xd->left_seg_context[0]) * mi_height);
#if CONFIG_VAR_TX
  memcpy(ta, xd->above_txfm_context,
         sizeof(*xd->above_txfm_context) * mi_width);
  memcpy(tl, xd->left_txfm_context,
         sizeof(*xd->left_txfm_context) * mi_height);
#endif
}

static void encode_b(VP10_COMP *cpi, const TileInfo *const tile,
                     ThreadData *td,
                     TOKENEXTRA **tp, int mi_row, int mi_col,
                     int output_enabled, BLOCK_SIZE bsize,
                     PICK_MODE_CONTEXT *ctx) {
  MACROBLOCK *const x = &td->mb;
  set_offsets(cpi, tile, x, mi_row, mi_col, bsize);
  update_state(cpi, td, ctx, mi_row, mi_col, bsize, output_enabled);
  encode_superblock(cpi, td, tp, output_enabled, mi_row, mi_col, bsize, ctx);

  if (output_enabled) {
#if CONFIG_SUPERTX
    update_stats(&cpi->common, td, 0);
#else
    update_stats(&cpi->common, td);
#endif
  }
}

static void encode_sb(VP10_COMP *cpi, ThreadData *td,
                      const TileInfo *const tile,
                      TOKENEXTRA **tp, int mi_row, int mi_col,
                      int output_enabled, BLOCK_SIZE bsize,
                      PC_TREE *pc_tree) {
  VP10_COMMON *const cm = &cpi->common;
  MACROBLOCK *const x = &td->mb;
  MACROBLOCKD *const xd = &x->e_mbd;

  const int bsl = b_width_log2_lookup[bsize], hbs = (1 << bsl) / 4;
  int ctx;
  PARTITION_TYPE partition;
  BLOCK_SIZE subsize = bsize;

  if (mi_row >= cm->mi_rows || mi_col >= cm->mi_cols)
    return;

  if (bsize >= BLOCK_8X8) {
    ctx = partition_plane_context(xd, mi_row, mi_col, bsize);
    subsize = get_subsize(bsize, pc_tree->partitioning);
  } else {
    ctx = 0;
    subsize = BLOCK_4X4;
  }

  partition = partition_lookup[bsl][subsize];
  if (output_enabled && bsize != BLOCK_4X4)
    td->counts->partition[ctx][partition]++;

#if CONFIG_SUPERTX
  if (!frame_is_intra_only(cm) &&
      bsize <= MAX_SUPERTX_BLOCK_SIZE &&
      partition != PARTITION_NONE &&
      !xd->lossless[0]) {
    int supertx_enabled;
    TX_SIZE supertx_size = max_txsize_lookup[bsize];
    supertx_enabled = check_supertx_sb(bsize, supertx_size, pc_tree);
    if (supertx_enabled) {
      const int mi_width = num_8x8_blocks_wide_lookup[bsize];
      const int mi_height = num_8x8_blocks_high_lookup[bsize];
      int x_idx, y_idx, i;
      uint8_t *dst_buf[3];
      int dst_stride[3];
      set_skip_context(xd, mi_row, mi_col);
      set_mode_info_offsets(cpi, x, xd, mi_row, mi_col);
      update_state_sb_supertx(cpi, td, tile, mi_row, mi_col, bsize,
                              output_enabled, pc_tree);

      vp10_setup_dst_planes(xd->plane, get_frame_new_buffer(cm),
                           mi_row, mi_col);
      for (i = 0; i < MAX_MB_PLANE; i++) {
        dst_buf[i] = xd->plane[i].dst.buf;
        dst_stride[i] = xd->plane[i].dst.stride;
      }
      predict_sb_complex(cpi, td, tile, mi_row, mi_col, mi_row, mi_col,
                         output_enabled, bsize, bsize,
                         dst_buf, dst_stride, pc_tree);

      set_offsets(cpi, tile, x, mi_row, mi_col, bsize);
      if (!x->skip) {
        // TODO(geza.lore): Investigate if this can be relaxed
        x->skip_recode = 0;
        memset(x->skip_txfm, 0, sizeof(x->skip_txfm));

        x->skip_optimize = 0;
        x->use_lp32x32fdct = cpi->sf.use_lp32x32fdct;

        vp10_encode_sb_supertx(x, bsize);
        vp10_tokenize_sb_supertx(cpi, td, tp, !output_enabled, bsize);
      } else {
        xd->mi[0]->mbmi.skip = 1;
        if (output_enabled)
          td->counts->skip[vp10_get_skip_context(xd)][1]++;
        reset_skip_context(xd, bsize);
      }
      if (output_enabled) {
        for (y_idx = 0; y_idx < mi_height; y_idx++)
          for (x_idx = 0; x_idx < mi_width; x_idx++) {
            if ((xd->mb_to_right_edge >> (3 + MI_SIZE_LOG2)) + mi_width > x_idx
                && (xd->mb_to_bottom_edge >> (3 + MI_SIZE_LOG2)) + mi_height
                    > y_idx) {
              xd->mi[x_idx + y_idx * cm->mi_stride]->mbmi.skip =
                  xd->mi[0]->mbmi.skip;
            }
          }
        td->counts->supertx
            [partition_supertx_context_lookup[partition]][supertx_size][1]++;
        td->counts->supertx_size[supertx_size]++;
#if CONFIG_EXT_TX
        if (get_ext_tx_types(supertx_size, bsize, 1) > 1 &&
            !xd->mi[0]->mbmi.skip) {
          int eset = get_ext_tx_set(supertx_size, bsize, 1);
          if (eset > 0) {
            ++td->counts->inter_ext_tx[eset][supertx_size]
                                      [xd->mi[0]->mbmi.tx_type];
          }
        }
#else
        if (supertx_size < TX_32X32 &&
            !xd->mi[0]->mbmi.skip) {
          ++td->counts->inter_ext_tx[supertx_size][xd->mi[0]->mbmi.tx_type];
        }
#endif  // CONFIG_EXT_TX
      }
      if (partition != PARTITION_SPLIT || bsize == BLOCK_8X8)
        update_partition_context(xd, mi_row, mi_col, subsize, bsize);
#if CONFIG_VAR_TX
      set_txfm_ctx(xd->left_txfm_context, supertx_size, xd->n8_h);
      set_txfm_ctx(xd->above_txfm_context, supertx_size, mi_height);
#endif  // CONFIG_VAR_TX
      return;
    } else {
      if (output_enabled) {
        td->counts->supertx
            [partition_supertx_context_lookup[partition]][supertx_size][0]++;
      }
    }
  }
#endif  // CONFIG_SUPERTX

  switch (partition) {
    case PARTITION_NONE:
      encode_b(cpi, tile, td, tp, mi_row, mi_col, output_enabled, subsize,
               &pc_tree->none);
      break;
    case PARTITION_VERT:
      encode_b(cpi, tile, td, tp, mi_row, mi_col, output_enabled, subsize,
               &pc_tree->vertical[0]);
      if (mi_col + hbs < cm->mi_cols && bsize > BLOCK_8X8) {
        encode_b(cpi, tile, td, tp, mi_row, mi_col + hbs, output_enabled,
                 subsize, &pc_tree->vertical[1]);
      }
      break;
    case PARTITION_HORZ:
      encode_b(cpi, tile, td, tp, mi_row, mi_col, output_enabled, subsize,
               &pc_tree->horizontal[0]);
      if (mi_row + hbs < cm->mi_rows && bsize > BLOCK_8X8) {
        encode_b(cpi, tile, td, tp, mi_row + hbs, mi_col, output_enabled,
                 subsize, &pc_tree->horizontal[1]);
      }
      break;
    case PARTITION_SPLIT:
      if (bsize == BLOCK_8X8) {
        encode_b(cpi, tile, td, tp, mi_row, mi_col, output_enabled, subsize,
                 pc_tree->leaf_split[0]);
      } else {
        encode_sb(cpi, td, tile, tp, mi_row, mi_col, output_enabled, subsize,
                  pc_tree->split[0]);
        encode_sb(cpi, td, tile, tp, mi_row, mi_col + hbs, output_enabled,
                  subsize, pc_tree->split[1]);
        encode_sb(cpi, td, tile, tp, mi_row + hbs, mi_col, output_enabled,
                  subsize, pc_tree->split[2]);
        encode_sb(cpi, td, tile, tp, mi_row + hbs, mi_col + hbs, output_enabled,
                  subsize, pc_tree->split[3]);
      }
      break;
    default:
      assert(0 && "Invalid partition type.");
      break;
  }

  if (partition != PARTITION_SPLIT || bsize == BLOCK_8X8)
    update_partition_context(xd, mi_row, mi_col, subsize, bsize);
}

// Check to see if the given partition size is allowed for a specified number
// of 8x8 block rows and columns remaining in the image.
// If not then return the largest allowed partition size
static BLOCK_SIZE find_partition_size(BLOCK_SIZE bsize,
                                      int rows_left, int cols_left,
                                      int *bh, int *bw) {
  if (rows_left <= 0 || cols_left <= 0) {
    return VPXMIN(bsize, BLOCK_8X8);
  } else {
    for (; bsize > 0; bsize -= 3) {
      *bh = num_8x8_blocks_high_lookup[bsize];
      *bw = num_8x8_blocks_wide_lookup[bsize];
      if ((*bh <= rows_left) && (*bw <= cols_left)) {
        break;
      }
    }
  }
  return bsize;
}

static void set_partial_b64x64_partition(MODE_INFO *mi, int mis,
    int bh_in, int bw_in, int row8x8_remaining, int col8x8_remaining,
    BLOCK_SIZE bsize, MODE_INFO **mi_8x8) {
  int bh = bh_in;
  int r, c;
  for (r = 0; r < MI_BLOCK_SIZE; r += bh) {
    int bw = bw_in;
    for (c = 0; c < MI_BLOCK_SIZE; c += bw) {
      const int index = r * mis + c;
      mi_8x8[index] = mi + index;
      mi_8x8[index]->mbmi.sb_type = find_partition_size(bsize,
          row8x8_remaining - r, col8x8_remaining - c, &bh, &bw);
    }
  }
}

// This function attempts to set all mode info entries in a given SB64
// to the same block partition size.
// However, at the bottom and right borders of the image the requested size
// may not be allowed in which case this code attempts to choose the largest
// allowable partition.
static void set_fixed_partitioning(VP10_COMP *cpi, const TileInfo *const tile,
                                   MODE_INFO **mi_8x8, int mi_row, int mi_col,
                                   BLOCK_SIZE bsize) {
  VP10_COMMON *const cm = &cpi->common;
  const int mis = cm->mi_stride;
  const int row8x8_remaining = tile->mi_row_end - mi_row;
  const int col8x8_remaining = tile->mi_col_end - mi_col;
  int block_row, block_col;
  MODE_INFO *mi_upper_left = cm->mi + mi_row * mis + mi_col;
  int bh = num_8x8_blocks_high_lookup[bsize];
  int bw = num_8x8_blocks_wide_lookup[bsize];

  assert((row8x8_remaining > 0) && (col8x8_remaining > 0));

  // Apply the requested partition size to the SB64 if it is all "in image"
  if ((col8x8_remaining >= MI_BLOCK_SIZE) &&
      (row8x8_remaining >= MI_BLOCK_SIZE)) {
    for (block_row = 0; block_row < MI_BLOCK_SIZE; block_row += bh) {
      for (block_col = 0; block_col < MI_BLOCK_SIZE; block_col += bw) {
        int index = block_row * mis + block_col;
        mi_8x8[index] = mi_upper_left + index;
        mi_8x8[index]->mbmi.sb_type = bsize;
      }
    }
  } else {
    // Else this is a partial SB64.
    set_partial_b64x64_partition(mi_upper_left, mis, bh, bw, row8x8_remaining,
        col8x8_remaining, bsize, mi_8x8);
  }
}

static void rd_use_partition(VP10_COMP *cpi,
                             ThreadData *td,
                             TileDataEnc *tile_data,
                             MODE_INFO **mi_8x8, TOKENEXTRA **tp,
                             int mi_row, int mi_col,
                             BLOCK_SIZE bsize,
                             int *rate, int64_t *dist,
#if CONFIG_SUPERTX
                             int *rate_nocoef,
#endif
                             int do_recon, PC_TREE *pc_tree) {
  VP10_COMMON *const cm = &cpi->common;
  TileInfo *const tile_info = &tile_data->tile_info;
  MACROBLOCK *const x = &td->mb;
  MACROBLOCKD *const xd = &x->e_mbd;
  const int mis = cm->mi_stride;
  const int bsl = b_width_log2_lookup[bsize];
  const int mi_step = num_4x4_blocks_wide_lookup[bsize] / 2;
  const int bss = (1 << bsl) / 4;
  int i, pl;
  PARTITION_TYPE partition = PARTITION_NONE;
  BLOCK_SIZE subsize;
  ENTROPY_CONTEXT l[16 * MAX_MB_PLANE], a[16 * MAX_MB_PLANE];
  PARTITION_CONTEXT sl[8], sa[8];
#if CONFIG_VAR_TX
  TXFM_CONTEXT tl[8], ta[8];
#endif
  RD_COST last_part_rdc, none_rdc, chosen_rdc;
  BLOCK_SIZE sub_subsize = BLOCK_4X4;
  int splits_below = 0;
  BLOCK_SIZE bs_type = mi_8x8[0]->mbmi.sb_type;
  int do_partition_search = 1;
  PICK_MODE_CONTEXT *ctx = &pc_tree->none;
#if CONFIG_SUPERTX
  int last_part_rate_nocoef = INT_MAX;
  int none_rate_nocoef = INT_MAX;
  int chosen_rate_nocoef = INT_MAX;
#endif

  if (mi_row >= cm->mi_rows || mi_col >= cm->mi_cols)
    return;

  assert(num_4x4_blocks_wide_lookup[bsize] ==
         num_4x4_blocks_high_lookup[bsize]);

  vp10_rd_cost_reset(&last_part_rdc);
  vp10_rd_cost_reset(&none_rdc);
  vp10_rd_cost_reset(&chosen_rdc);

  partition = partition_lookup[bsl][bs_type];
  subsize = get_subsize(bsize, partition);

#if CONFIG_VAR_TX
  xd->above_txfm_context = cm->above_txfm_context + mi_col;
  xd->left_txfm_context = xd->left_txfm_context_buffer + (mi_row & MI_MASK);
#endif
  pc_tree->partitioning = partition;
  save_context(x, mi_row, mi_col, a, l, sa, sl,
#if CONFIG_VAR_TX
               ta, tl,
#endif
               bsize);

  if (bsize == BLOCK_16X16 && cpi->oxcf.aq_mode) {
    set_offsets(cpi, tile_info, x, mi_row, mi_col, bsize);
    x->mb_energy = vp10_block_energy(cpi, x, bsize);
  }

  if (do_partition_search &&
      cpi->sf.partition_search_type == SEARCH_PARTITION &&
      cpi->sf.adjust_partitioning_from_last_frame) {
    // Check if any of the sub blocks are further split.
    if (partition == PARTITION_SPLIT && subsize > BLOCK_8X8) {
      sub_subsize = get_subsize(subsize, PARTITION_SPLIT);
      splits_below = 1;
      for (i = 0; i < 4; i++) {
        int jj = i >> 1, ii = i & 0x01;
        MODE_INFO *this_mi = mi_8x8[jj * bss * mis + ii * bss];
        if (this_mi && this_mi->mbmi.sb_type >= sub_subsize) {
          splits_below = 0;
        }
      }
    }

    // If partition is not none try none unless each of the 4 splits are split
    // even further..
    if (partition != PARTITION_NONE && !splits_below &&
        mi_row + (mi_step >> 1) < cm->mi_rows &&
        mi_col + (mi_step >> 1) < cm->mi_cols) {
      pc_tree->partitioning = PARTITION_NONE;
      rd_pick_sb_modes(cpi, tile_data, x, mi_row, mi_col, &none_rdc,
#if CONFIG_SUPERTX
                       &none_rate_nocoef,
#endif
                       bsize, ctx, INT64_MAX);

      pl = partition_plane_context(xd, mi_row, mi_col, bsize);

      if (none_rdc.rate < INT_MAX) {
        none_rdc.rate += cpi->partition_cost[pl][PARTITION_NONE];
        none_rdc.rdcost = RDCOST(x->rdmult, x->rddiv, none_rdc.rate,
                                 none_rdc.dist);
#if CONFIG_SUPERTX
        none_rate_nocoef += cpi->partition_cost[pl][PARTITION_NONE];
#endif
      }

      restore_context(x, mi_row, mi_col, a, l, sa, sl,
#if CONFIG_VAR_TX
                      ta, tl,
#endif
                      bsize);
      mi_8x8[0]->mbmi.sb_type = bs_type;
      pc_tree->partitioning = partition;
    }
  }

  switch (partition) {
    case PARTITION_NONE:
      rd_pick_sb_modes(cpi, tile_data, x, mi_row, mi_col, &last_part_rdc,
#if CONFIG_SUPERTX
                       &last_part_rate_nocoef,
#endif
                       bsize, ctx, INT64_MAX);
      break;
    case PARTITION_HORZ:
      rd_pick_sb_modes(cpi, tile_data, x, mi_row, mi_col, &last_part_rdc,
#if CONFIG_SUPERTX
                       &last_part_rate_nocoef,
#endif
                       subsize, &pc_tree->horizontal[0],
                       INT64_MAX);
      if (last_part_rdc.rate != INT_MAX &&
          bsize >= BLOCK_8X8 && mi_row + (mi_step >> 1) < cm->mi_rows) {
        RD_COST tmp_rdc;
#if CONFIG_SUPERTX
        int rt_nocoef = 0;
#endif
        PICK_MODE_CONTEXT *ctx = &pc_tree->horizontal[0];
        vp10_rd_cost_init(&tmp_rdc);
        update_state(cpi, td, ctx, mi_row, mi_col, subsize, 0);
        encode_superblock(cpi, td, tp, 0, mi_row, mi_col, subsize, ctx);
        rd_pick_sb_modes(cpi, tile_data, x,
                         mi_row + (mi_step >> 1), mi_col, &tmp_rdc,
#if CONFIG_SUPERTX
                         &rt_nocoef,
#endif
                         subsize, &pc_tree->horizontal[1], INT64_MAX);
        if (tmp_rdc.rate == INT_MAX || tmp_rdc.dist == INT64_MAX) {
          vp10_rd_cost_reset(&last_part_rdc);
#if CONFIG_SUPERTX
          last_part_rate_nocoef = INT_MAX;
#endif
          break;
        }
        last_part_rdc.rate += tmp_rdc.rate;
        last_part_rdc.dist += tmp_rdc.dist;
        last_part_rdc.rdcost += tmp_rdc.rdcost;
#if CONFIG_SUPERTX
        last_part_rate_nocoef += rt_nocoef;
#endif
      }
      break;
    case PARTITION_VERT:
      rd_pick_sb_modes(cpi, tile_data, x, mi_row, mi_col, &last_part_rdc,
#if CONFIG_SUPERTX
                       &last_part_rate_nocoef,
#endif
                       subsize, &pc_tree->vertical[0], INT64_MAX);
      if (last_part_rdc.rate != INT_MAX &&
          bsize >= BLOCK_8X8 && mi_col + (mi_step >> 1) < cm->mi_cols) {
        RD_COST tmp_rdc;
#if CONFIG_SUPERTX
        int rt_nocoef = 0;
#endif
        PICK_MODE_CONTEXT *ctx = &pc_tree->vertical[0];
        vp10_rd_cost_init(&tmp_rdc);
        update_state(cpi, td, ctx, mi_row, mi_col, subsize, 0);
        encode_superblock(cpi, td, tp, 0, mi_row, mi_col, subsize, ctx);
        rd_pick_sb_modes(cpi, tile_data, x,
                         mi_row, mi_col + (mi_step >> 1), &tmp_rdc,
#if CONFIG_SUPERTX
                         &rt_nocoef,
#endif
                         subsize, &pc_tree->vertical[bsize > BLOCK_8X8],
                         INT64_MAX);
        if (tmp_rdc.rate == INT_MAX || tmp_rdc.dist == INT64_MAX) {
          vp10_rd_cost_reset(&last_part_rdc);
#if CONFIG_SUPERTX
          last_part_rate_nocoef = INT_MAX;
#endif
          break;
        }
        last_part_rdc.rate += tmp_rdc.rate;
        last_part_rdc.dist += tmp_rdc.dist;
        last_part_rdc.rdcost += tmp_rdc.rdcost;
#if CONFIG_SUPERTX
        last_part_rate_nocoef += rt_nocoef;
#endif
      }
      break;
    case PARTITION_SPLIT:
      if (bsize == BLOCK_8X8) {
        rd_pick_sb_modes(cpi, tile_data, x, mi_row, mi_col, &last_part_rdc,
#if CONFIG_SUPERTX
                         &last_part_rate_nocoef,
#endif
                         subsize, pc_tree->leaf_split[0], INT64_MAX);
        break;
      }
      last_part_rdc.rate = 0;
      last_part_rdc.dist = 0;
      last_part_rdc.rdcost = 0;
#if CONFIG_SUPERTX
      last_part_rate_nocoef = 0;
#endif
      for (i = 0; i < 4; i++) {
        int x_idx = (i & 1) * (mi_step >> 1);
        int y_idx = (i >> 1) * (mi_step >> 1);
        int jj = i >> 1, ii = i & 0x01;
        RD_COST tmp_rdc;
#if CONFIG_SUPERTX
        int rt_nocoef;
#endif
        if ((mi_row + y_idx >= cm->mi_rows) || (mi_col + x_idx >= cm->mi_cols))
          continue;

        vp10_rd_cost_init(&tmp_rdc);
        rd_use_partition(cpi, td, tile_data,
                         mi_8x8 + jj * bss * mis + ii * bss, tp,
                         mi_row + y_idx, mi_col + x_idx, subsize,
                         &tmp_rdc.rate, &tmp_rdc.dist,
#if CONFIG_SUPERTX
                         &rt_nocoef,
#endif
                         i != 3, pc_tree->split[i]);
        if (tmp_rdc.rate == INT_MAX || tmp_rdc.dist == INT64_MAX) {
          vp10_rd_cost_reset(&last_part_rdc);
#if CONFIG_SUPERTX
          last_part_rate_nocoef = INT_MAX;
#endif
          break;
        }
        last_part_rdc.rate += tmp_rdc.rate;
        last_part_rdc.dist += tmp_rdc.dist;
#if CONFIG_SUPERTX
        last_part_rate_nocoef += rt_nocoef;
#endif
      }
      break;
    default:
      assert(0);
      break;
  }

  pl = partition_plane_context(xd, mi_row, mi_col, bsize);
  if (last_part_rdc.rate < INT_MAX) {
    last_part_rdc.rate += cpi->partition_cost[pl][partition];
    last_part_rdc.rdcost = RDCOST(x->rdmult, x->rddiv,
                                  last_part_rdc.rate, last_part_rdc.dist);
#if CONFIG_SUPERTX
    last_part_rate_nocoef += cpi->partition_cost[pl][partition];
#endif
  }

  if (do_partition_search
      && cpi->sf.adjust_partitioning_from_last_frame
      && cpi->sf.partition_search_type == SEARCH_PARTITION
      && partition != PARTITION_SPLIT && bsize > BLOCK_8X8
      && (mi_row + mi_step < cm->mi_rows ||
          mi_row + (mi_step >> 1) == cm->mi_rows)
      && (mi_col + mi_step < cm->mi_cols ||
          mi_col + (mi_step >> 1) == cm->mi_cols)) {
    BLOCK_SIZE split_subsize = get_subsize(bsize, PARTITION_SPLIT);
    chosen_rdc.rate = 0;
    chosen_rdc.dist = 0;
#if CONFIG_SUPERTX
    chosen_rate_nocoef = 0;
#endif
    restore_context(x, mi_row, mi_col, a, l, sa, sl,
#if CONFIG_VAR_TX
                    ta, tl,
#endif
                    bsize);
    pc_tree->partitioning = PARTITION_SPLIT;

    // Split partition.
    for (i = 0; i < 4; i++) {
      int x_idx = (i & 1) * (mi_step >> 1);
      int y_idx = (i >> 1) * (mi_step >> 1);
      RD_COST tmp_rdc;
#if CONFIG_SUPERTX
      int rt_nocoef = 0;
#endif
      ENTROPY_CONTEXT l[16 * MAX_MB_PLANE], a[16 * MAX_MB_PLANE];
      PARTITION_CONTEXT sl[8], sa[8];
#if CONFIG_VAR_TX
      TXFM_CONTEXT tl[8], ta[8];
#endif

      if ((mi_row + y_idx >= cm->mi_rows) || (mi_col + x_idx >= cm->mi_cols))
        continue;

      save_context(x, mi_row, mi_col, a, l, sa, sl,
#if CONFIG_VAR_TX
                   ta, tl,
#endif
                   bsize);
      pc_tree->split[i]->partitioning = PARTITION_NONE;
      rd_pick_sb_modes(cpi, tile_data, x,
                       mi_row + y_idx, mi_col + x_idx, &tmp_rdc,
#if CONFIG_SUPERTX
                       &rt_nocoef,
#endif
                       split_subsize, &pc_tree->split[i]->none, INT64_MAX);

      restore_context(x, mi_row, mi_col, a, l, sa, sl,
#if CONFIG_VAR_TX
                      ta, tl,
#endif
                      bsize);

      if (tmp_rdc.rate == INT_MAX || tmp_rdc.dist == INT64_MAX) {
        vp10_rd_cost_reset(&chosen_rdc);
#if CONFIG_SUPERTX
        chosen_rate_nocoef = INT_MAX;
#endif
        break;
      }

      chosen_rdc.rate += tmp_rdc.rate;
      chosen_rdc.dist += tmp_rdc.dist;
#if CONFIG_SUPERTX
      chosen_rate_nocoef += rt_nocoef;
#endif

      if (i != 3)
        encode_sb(cpi, td, tile_info, tp,  mi_row + y_idx, mi_col + x_idx, 0,
                  split_subsize, pc_tree->split[i]);

      pl = partition_plane_context(xd, mi_row + y_idx, mi_col + x_idx,
                                   split_subsize);
      chosen_rdc.rate += cpi->partition_cost[pl][PARTITION_NONE];
#if CONFIG_SUPERTX
      chosen_rate_nocoef += cpi->partition_cost[pl][PARTITION_SPLIT];
#endif
    }
    pl = partition_plane_context(xd, mi_row, mi_col, bsize);
    if (chosen_rdc.rate < INT_MAX) {
      chosen_rdc.rate += cpi->partition_cost[pl][PARTITION_SPLIT];
      chosen_rdc.rdcost = RDCOST(x->rdmult, x->rddiv,
                                 chosen_rdc.rate, chosen_rdc.dist);
#if CONFIG_SUPERTX
      chosen_rate_nocoef += cpi->partition_cost[pl][PARTITION_NONE];
#endif
    }
  }

  // If last_part is better set the partitioning to that.
  if (last_part_rdc.rdcost < chosen_rdc.rdcost) {
    mi_8x8[0]->mbmi.sb_type = bsize;
    if (bsize >= BLOCK_8X8)
      pc_tree->partitioning = partition;
    chosen_rdc = last_part_rdc;
#if CONFIG_SUPERTX
    chosen_rate_nocoef = last_part_rate_nocoef;
#endif
  }
  // If none was better set the partitioning to that.
  if (none_rdc.rdcost < chosen_rdc.rdcost) {
    if (bsize >= BLOCK_8X8)
      pc_tree->partitioning = PARTITION_NONE;
    chosen_rdc = none_rdc;
#if CONFIG_SUPERTX
    chosen_rate_nocoef = none_rate_nocoef;
#endif
  }

#if CONFIG_VAR_TX
  xd->above_txfm_context = cm->above_txfm_context + mi_col;
  xd->left_txfm_context = xd->left_txfm_context_buffer + (mi_row & MI_MASK);
#endif
  restore_context(x, mi_row, mi_col, a, l, sa, sl,
#if CONFIG_VAR_TX
                  ta, tl,
#endif
                  bsize);

  // We must have chosen a partitioning and encoding or we'll fail later on.
  // No other opportunities for success.
  if (bsize == BLOCK_64X64)
    assert(chosen_rdc.rate < INT_MAX && chosen_rdc.dist < INT64_MAX);

  if (do_recon) {
    int output_enabled = (bsize == BLOCK_64X64);
    encode_sb(cpi, td, tile_info, tp, mi_row, mi_col, output_enabled, bsize,
              pc_tree);
  }

  *rate = chosen_rdc.rate;
  *dist = chosen_rdc.dist;
#if CONFIG_SUPERTX
  *rate_nocoef = chosen_rate_nocoef;
#endif
}

static const BLOCK_SIZE min_partition_size[BLOCK_SIZES] = {
  BLOCK_4X4,   BLOCK_4X4,   BLOCK_4X4,
  BLOCK_4X4,   BLOCK_4X4,   BLOCK_4X4,
  BLOCK_8X8,   BLOCK_8X8,   BLOCK_8X8,
  BLOCK_16X16, BLOCK_16X16, BLOCK_16X16,
  BLOCK_16X16
};

static const BLOCK_SIZE max_partition_size[BLOCK_SIZES] = {
  BLOCK_8X8,   BLOCK_16X16, BLOCK_16X16,
  BLOCK_16X16, BLOCK_32X32, BLOCK_32X32,
  BLOCK_32X32, BLOCK_64X64, BLOCK_64X64,
  BLOCK_64X64, BLOCK_64X64, BLOCK_64X64,
  BLOCK_64X64
};


// Look at all the mode_info entries for blocks that are part of this
// partition and find the min and max values for sb_type.
// At the moment this is designed to work on a 64x64 SB but could be
// adjusted to use a size parameter.
//
// The min and max are assumed to have been initialized prior to calling this
// function so repeat calls can accumulate a min and max of more than one sb64.
static void get_sb_partition_size_range(MACROBLOCKD *xd, MODE_INFO **mi_8x8,
                                        BLOCK_SIZE *min_block_size,
                                        BLOCK_SIZE *max_block_size,
                                        int bs_hist[BLOCK_SIZES]) {
  int sb_width_in_blocks = MI_BLOCK_SIZE;
  int sb_height_in_blocks  = MI_BLOCK_SIZE;
  int i, j;
  int index = 0;

  // Check the sb_type for each block that belongs to this region.
  for (i = 0; i < sb_height_in_blocks; ++i) {
    for (j = 0; j < sb_width_in_blocks; ++j) {
      MODE_INFO *mi = mi_8x8[index+j];
      BLOCK_SIZE sb_type = mi ? mi->mbmi.sb_type : 0;
      bs_hist[sb_type]++;
      *min_block_size = VPXMIN(*min_block_size, sb_type);
      *max_block_size = VPXMAX(*max_block_size, sb_type);
    }
    index += xd->mi_stride;
  }
}

// Next square block size less or equal than current block size.
static const BLOCK_SIZE next_square_size[BLOCK_SIZES] = {
  BLOCK_4X4, BLOCK_4X4, BLOCK_4X4,
  BLOCK_8X8, BLOCK_8X8, BLOCK_8X8,
  BLOCK_16X16, BLOCK_16X16, BLOCK_16X16,
  BLOCK_32X32, BLOCK_32X32, BLOCK_32X32,
  BLOCK_64X64
};

// Look at neighboring blocks and set a min and max partition size based on
// what they chose.
static void rd_auto_partition_range(VP10_COMP *cpi, const TileInfo *const tile,
                                    MACROBLOCKD *const xd,
                                    int mi_row, int mi_col,
                                    BLOCK_SIZE *min_block_size,
                                    BLOCK_SIZE *max_block_size) {
  VP10_COMMON *const cm = &cpi->common;
  MODE_INFO **mi = xd->mi;
  const int left_in_image = xd->left_available && mi[-1];
  const int above_in_image = xd->up_available && mi[-xd->mi_stride];
  const int row8x8_remaining = tile->mi_row_end - mi_row;
  const int col8x8_remaining = tile->mi_col_end - mi_col;
  int bh, bw;
  BLOCK_SIZE min_size = BLOCK_4X4;
  BLOCK_SIZE max_size = BLOCK_64X64;
  int bs_hist[BLOCK_SIZES] = {0};

  // Trap case where we do not have a prediction.
  if (left_in_image || above_in_image || cm->frame_type != KEY_FRAME) {
    // Default "min to max" and "max to min"
    min_size = BLOCK_64X64;
    max_size = BLOCK_4X4;

    // NOTE: each call to get_sb_partition_size_range() uses the previous
    // passed in values for min and max as a starting point.
    // Find the min and max partition used in previous frame at this location
    if (cm->frame_type != KEY_FRAME) {
      MODE_INFO **prev_mi =
          &cm->prev_mi_grid_visible[mi_row * xd->mi_stride + mi_col];
      get_sb_partition_size_range(xd, prev_mi, &min_size, &max_size, bs_hist);
    }
    // Find the min and max partition sizes used in the left SB64
    if (left_in_image) {
      MODE_INFO **left_sb64_mi = &mi[-MI_BLOCK_SIZE];
      get_sb_partition_size_range(xd, left_sb64_mi, &min_size, &max_size,
                                  bs_hist);
    }
    // Find the min and max partition sizes used in the above SB64.
    if (above_in_image) {
      MODE_INFO **above_sb64_mi = &mi[-xd->mi_stride * MI_BLOCK_SIZE];
      get_sb_partition_size_range(xd, above_sb64_mi, &min_size, &max_size,
                                  bs_hist);
    }

    // Adjust observed min and max for "relaxed" auto partition case.
    if (cpi->sf.auto_min_max_partition_size == RELAXED_NEIGHBORING_MIN_MAX) {
      min_size = min_partition_size[min_size];
      max_size = max_partition_size[max_size];
    }
  }

  // Check border cases where max and min from neighbors may not be legal.
  max_size = find_partition_size(max_size,
                                 row8x8_remaining, col8x8_remaining,
                                 &bh, &bw);
  // Test for blocks at the edge of the active image.
  // This may be the actual edge of the image or where there are formatting
  // bars.
  if (vp10_active_edge_sb(cpi, mi_row, mi_col)) {
    min_size = BLOCK_4X4;
  } else {
    min_size =
        VPXMIN(cpi->sf.rd_auto_partition_min_limit, VPXMIN(min_size, max_size));
  }

  // When use_square_partition_only is true, make sure at least one square
  // partition is allowed by selecting the next smaller square size as
  // *min_block_size.
  if (cpi->sf.use_square_partition_only &&
      next_square_size[max_size] < min_size) {
     min_size = next_square_size[max_size];
  }

  *min_block_size = min_size;
  *max_block_size = max_size;
}

// TODO(jingning) refactor functions setting partition search range
static void set_partition_range(VP10_COMMON *cm, MACROBLOCKD *xd,
                                int mi_row, int mi_col, BLOCK_SIZE bsize,
                                BLOCK_SIZE *min_bs, BLOCK_SIZE *max_bs) {
  int mi_width  = num_8x8_blocks_wide_lookup[bsize];
  int mi_height = num_8x8_blocks_high_lookup[bsize];
  int idx, idy;

  MODE_INFO *mi;
  const int idx_str = cm->mi_stride * mi_row + mi_col;
  MODE_INFO **prev_mi = &cm->prev_mi_grid_visible[idx_str];
  BLOCK_SIZE bs, min_size, max_size;

  min_size = BLOCK_64X64;
  max_size = BLOCK_4X4;

  if (prev_mi) {
    for (idy = 0; idy < mi_height; ++idy) {
      for (idx = 0; idx < mi_width; ++idx) {
        mi = prev_mi[idy * cm->mi_stride + idx];
        bs = mi ? mi->mbmi.sb_type : bsize;
        min_size = VPXMIN(min_size, bs);
        max_size = VPXMAX(max_size, bs);
      }
    }
  }

  if (xd->left_available) {
    for (idy = 0; idy < mi_height; ++idy) {
      mi = xd->mi[idy * cm->mi_stride - 1];
      bs = mi ? mi->mbmi.sb_type : bsize;
      min_size = VPXMIN(min_size, bs);
      max_size = VPXMAX(max_size, bs);
    }
  }

  if (xd->up_available) {
    for (idx = 0; idx < mi_width; ++idx) {
      mi = xd->mi[idx - cm->mi_stride];
      bs = mi ? mi->mbmi.sb_type : bsize;
      min_size = VPXMIN(min_size, bs);
      max_size = VPXMAX(max_size, bs);
    }
  }

  if (min_size == max_size) {
    min_size = min_partition_size[min_size];
    max_size = max_partition_size[max_size];
  }

  *min_bs = min_size;
  *max_bs = max_size;
}

static INLINE void store_pred_mv(MACROBLOCK *x, PICK_MODE_CONTEXT *ctx) {
  memcpy(ctx->pred_mv, x->pred_mv, sizeof(x->pred_mv));
}

static INLINE void load_pred_mv(MACROBLOCK *x, PICK_MODE_CONTEXT *ctx) {
  memcpy(x->pred_mv, ctx->pred_mv, sizeof(x->pred_mv));
}

#if CONFIG_FP_MB_STATS
const int num_16x16_blocks_wide_lookup[BLOCK_SIZES] =
  {1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 4, 4};
const int num_16x16_blocks_high_lookup[BLOCK_SIZES] =
  {1, 1, 1, 1, 1, 1, 1, 2, 1, 2, 4, 2, 4};
const int qindex_skip_threshold_lookup[BLOCK_SIZES] =
  {0, 10, 10, 30, 40, 40, 60, 80, 80, 90, 100, 100, 120};
const int qindex_split_threshold_lookup[BLOCK_SIZES] =
  {0, 3, 3, 7, 15, 15, 30, 40, 40, 60, 80, 80, 120};
const int complexity_16x16_blocks_threshold[BLOCK_SIZES] =
  {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 4, 4, 6};

typedef enum {
  MV_ZERO = 0,
  MV_LEFT = 1,
  MV_UP = 2,
  MV_RIGHT = 3,
  MV_DOWN = 4,
  MV_INVALID
} MOTION_DIRECTION;

static INLINE MOTION_DIRECTION get_motion_direction_fp(uint8_t fp_byte) {
  if (fp_byte & FPMB_MOTION_ZERO_MASK) {
    return MV_ZERO;
  } else if (fp_byte & FPMB_MOTION_LEFT_MASK) {
    return MV_LEFT;
  } else if (fp_byte & FPMB_MOTION_RIGHT_MASK) {
    return MV_RIGHT;
  } else if (fp_byte & FPMB_MOTION_UP_MASK) {
    return MV_UP;
  } else {
    return MV_DOWN;
  }
}

static INLINE int get_motion_inconsistency(MOTION_DIRECTION this_mv,
                                           MOTION_DIRECTION that_mv) {
  if (this_mv == that_mv) {
    return 0;
  } else {
    return abs(this_mv - that_mv) == 2 ? 2 : 1;
  }
}
#endif

// TODO(jingning,jimbankoski,rbultje): properly skip partition types that are
// unlikely to be selected depending on previous rate-distortion optimization
// results, for encoding speed-up.
static void rd_pick_partition(VP10_COMP *cpi, ThreadData *td,
                              TileDataEnc *tile_data,
                              TOKENEXTRA **tp, int mi_row, int mi_col,
                              BLOCK_SIZE bsize, RD_COST *rd_cost,
#if CONFIG_SUPERTX
                              int *rate_nocoef,
#endif
                              int64_t best_rd, PC_TREE *pc_tree) {
  VP10_COMMON *const cm = &cpi->common;
  TileInfo *const tile_info = &tile_data->tile_info;
  MACROBLOCK *const x = &td->mb;
  MACROBLOCKD *const xd = &x->e_mbd;
  const int mi_step = num_8x8_blocks_wide_lookup[bsize] / 2;
  ENTROPY_CONTEXT l[16 * MAX_MB_PLANE], a[16 * MAX_MB_PLANE];
  PARTITION_CONTEXT sl[8], sa[8];
#if CONFIG_VAR_TX
  TXFM_CONTEXT tl[8], ta[8];
#endif
  TOKENEXTRA *tp_orig = *tp;
  PICK_MODE_CONTEXT *ctx = &pc_tree->none;
  int i;
  const int pl = partition_plane_context(xd, mi_row, mi_col, bsize);
  BLOCK_SIZE subsize;
  RD_COST this_rdc, sum_rdc, best_rdc;
#if CONFIG_SUPERTX
  int this_rate_nocoef, sum_rate_nocoef = 0, best_rate_nocoef = INT_MAX;
  int tmp_rate;
  int abort_flag;
  int64_t tmp_dist, tmp_rd;
  PARTITION_TYPE best_partition;
#endif  // CONFIG_SUPERTX
  int do_split = bsize >= BLOCK_8X8;
  int do_rect = 1;

  // Override skipping rectangular partition operations for edge blocks
  const int force_horz_split = (mi_row + mi_step >= cm->mi_rows);
  const int force_vert_split = (mi_col + mi_step >= cm->mi_cols);
  const int xss = x->e_mbd.plane[1].subsampling_x;
  const int yss = x->e_mbd.plane[1].subsampling_y;

  BLOCK_SIZE min_size = x->min_partition_size;
  BLOCK_SIZE max_size = x->max_partition_size;

#if CONFIG_FP_MB_STATS
  unsigned int src_diff_var = UINT_MAX;
  int none_complexity = 0;
#endif

  int partition_none_allowed = !force_horz_split && !force_vert_split;
  int partition_horz_allowed = !force_vert_split && yss <= xss &&
                               bsize >= BLOCK_8X8;
  int partition_vert_allowed = !force_horz_split && xss <= yss &&
                               bsize >= BLOCK_8X8;
  (void) *tp_orig;

  assert(num_8x8_blocks_wide_lookup[bsize] ==
             num_8x8_blocks_high_lookup[bsize]);

  vp10_rd_cost_init(&this_rdc);
  vp10_rd_cost_init(&sum_rdc);
  vp10_rd_cost_reset(&best_rdc);
  best_rdc.rdcost = best_rd;

  set_offsets(cpi, tile_info, x, mi_row, mi_col, bsize);

  if (bsize == BLOCK_16X16 && cpi->oxcf.aq_mode)
    x->mb_energy = vp10_block_energy(cpi, x, bsize);

  if (cpi->sf.cb_partition_search && bsize == BLOCK_16X16) {
    int cb_partition_search_ctrl = ((pc_tree->index == 0 || pc_tree->index == 3)
        + get_chessboard_index(cm->current_video_frame)) & 0x1;

    if (cb_partition_search_ctrl && bsize > min_size && bsize < max_size)
      set_partition_range(cm, xd, mi_row, mi_col, bsize, &min_size, &max_size);
  }

  // Determine partition types in search according to the speed features.
  // The threshold set here has to be of square block size.
  if (cpi->sf.auto_min_max_partition_size) {
    partition_none_allowed &= (bsize <= max_size && bsize >= min_size);
    partition_horz_allowed &= ((bsize <= max_size && bsize > min_size) ||
                                force_horz_split);
    partition_vert_allowed &= ((bsize <= max_size && bsize > min_size) ||
                                force_vert_split);
    do_split &= bsize > min_size;
  }
  if (cpi->sf.use_square_partition_only) {
    partition_horz_allowed &= force_horz_split;
    partition_vert_allowed &= force_vert_split;
  }

#if CONFIG_VAR_TX
  xd->above_txfm_context = cm->above_txfm_context + mi_col;
  xd->left_txfm_context = xd->left_txfm_context_buffer + (mi_row & MI_MASK);
  save_context(x, mi_row, mi_col, a, l, sa, sl, ta, tl, bsize);
#else
  save_context(x, mi_row, mi_col, a, l, sa, sl, bsize);
#endif

#if CONFIG_FP_MB_STATS
  if (cpi->use_fp_mb_stats) {
    set_offsets(cpi, tile_info, x, mi_row, mi_col, bsize);
    src_diff_var = get_sby_perpixel_diff_variance(cpi, &x->plane[0].src,
                                                  mi_row, mi_col, bsize);
  }
#endif

#if CONFIG_FP_MB_STATS
  // Decide whether we shall split directly and skip searching NONE by using
  // the first pass block statistics
  if (cpi->use_fp_mb_stats && bsize >= BLOCK_32X32 && do_split &&
      partition_none_allowed && src_diff_var > 4 &&
      cm->base_qindex < qindex_split_threshold_lookup[bsize]) {
    int mb_row = mi_row >> 1;
    int mb_col = mi_col >> 1;
    int mb_row_end =
        VPXMIN(mb_row + num_16x16_blocks_high_lookup[bsize], cm->mb_rows);
    int mb_col_end =
        VPXMIN(mb_col + num_16x16_blocks_wide_lookup[bsize], cm->mb_cols);
    int r, c;

    // compute a complexity measure, basically measure inconsistency of motion
    // vectors obtained from the first pass in the current block
    for (r = mb_row; r < mb_row_end ; r++) {
      for (c = mb_col; c < mb_col_end; c++) {
        const int mb_index = r * cm->mb_cols + c;

        MOTION_DIRECTION this_mv;
        MOTION_DIRECTION right_mv;
        MOTION_DIRECTION bottom_mv;

        this_mv =
            get_motion_direction_fp(cpi->twopass.this_frame_mb_stats[mb_index]);

        // to its right
        if (c != mb_col_end - 1) {
          right_mv = get_motion_direction_fp(
              cpi->twopass.this_frame_mb_stats[mb_index + 1]);
          none_complexity += get_motion_inconsistency(this_mv, right_mv);
        }

        // to its bottom
        if (r != mb_row_end - 1) {
          bottom_mv = get_motion_direction_fp(
              cpi->twopass.this_frame_mb_stats[mb_index + cm->mb_cols]);
          none_complexity += get_motion_inconsistency(this_mv, bottom_mv);
        }

        // do not count its left and top neighbors to avoid double counting
      }
    }

    if (none_complexity > complexity_16x16_blocks_threshold[bsize]) {
      partition_none_allowed = 0;
    }
  }
#endif

  // PARTITION_NONE
  if (partition_none_allowed) {
    rd_pick_sb_modes(cpi, tile_data, x, mi_row, mi_col, &this_rdc,
#if CONFIG_SUPERTX
                     &this_rate_nocoef,
#endif
                     bsize, ctx, best_rdc.rdcost);
    if (this_rdc.rate != INT_MAX) {
      if (bsize >= BLOCK_8X8) {
        this_rdc.rate += cpi->partition_cost[pl][PARTITION_NONE];
        this_rdc.rdcost = RDCOST(x->rdmult, x->rddiv,
                                 this_rdc.rate, this_rdc.dist);
#if CONFIG_SUPERTX
        this_rate_nocoef += cpi->partition_cost[pl][PARTITION_NONE];
#endif
      }

      if (this_rdc.rdcost < best_rdc.rdcost) {
        int64_t dist_breakout_thr = cpi->sf.partition_search_breakout_dist_thr;
        int rate_breakout_thr = cpi->sf.partition_search_breakout_rate_thr;

        best_rdc = this_rdc;
#if CONFIG_SUPERTX
        best_rate_nocoef = this_rate_nocoef;
        assert(best_rate_nocoef >= 0);
#endif
        if (bsize >= BLOCK_8X8)
          pc_tree->partitioning = PARTITION_NONE;

        // Adjust dist breakout threshold according to the partition size.
        dist_breakout_thr >>= 8 - (b_width_log2_lookup[bsize] +
            b_height_log2_lookup[bsize]);

        rate_breakout_thr *= num_pels_log2_lookup[bsize];

        // If all y, u, v transform blocks in this partition are skippable, and
        // the dist & rate are within the thresholds, the partition search is
        // terminated for current branch of the partition search tree.
        // The dist & rate thresholds are set to 0 at speed 0 to disable the
        // early termination at that speed.
        if (!x->e_mbd.lossless[xd->mi[0]->mbmi.segment_id] &&
            (ctx->skippable && best_rdc.dist < dist_breakout_thr &&
            best_rdc.rate < rate_breakout_thr)) {
          do_split = 0;
          do_rect = 0;
        }

#if CONFIG_FP_MB_STATS
        // Check if every 16x16 first pass block statistics has zero
        // motion and the corresponding first pass residue is small enough.
        // If that is the case, check the difference variance between the
        // current frame and the last frame. If the variance is small enough,
        // stop further splitting in RD optimization
        if (cpi->use_fp_mb_stats && do_split != 0 &&
            cm->base_qindex > qindex_skip_threshold_lookup[bsize]) {
          int mb_row = mi_row >> 1;
          int mb_col = mi_col >> 1;
          int mb_row_end =
              VPXMIN(mb_row + num_16x16_blocks_high_lookup[bsize], cm->mb_rows);
          int mb_col_end =
              VPXMIN(mb_col + num_16x16_blocks_wide_lookup[bsize], cm->mb_cols);
          int r, c;

          int skip = 1;
          for (r = mb_row; r < mb_row_end; r++) {
            for (c = mb_col; c < mb_col_end; c++) {
              const int mb_index = r * cm->mb_cols + c;
              if (!(cpi->twopass.this_frame_mb_stats[mb_index] &
                    FPMB_MOTION_ZERO_MASK) ||
                  !(cpi->twopass.this_frame_mb_stats[mb_index] &
                    FPMB_ERROR_SMALL_MASK)) {
                skip = 0;
                break;
              }
            }
            if (skip == 0) {
              break;
            }
          }
          if (skip) {
            if (src_diff_var == UINT_MAX) {
              set_offsets(cpi, tile_info, x, mi_row, mi_col, bsize);
              src_diff_var = get_sby_perpixel_diff_variance(
                  cpi, &x->plane[0].src, mi_row, mi_col, bsize);
            }
            if (src_diff_var < 8) {
              do_split = 0;
              do_rect = 0;
            }
          }
        }
#endif
      }
    }
#if CONFIG_VAR_TX
    xd->above_txfm_context = cm->above_txfm_context + mi_col;
    xd->left_txfm_context = xd->left_txfm_context_buffer + (mi_row & MI_MASK);
    restore_context(x, mi_row, mi_col, a, l, sa, sl, ta, tl, bsize);
#else
    restore_context(x, mi_row, mi_col, a, l, sa, sl, bsize);
#endif  // CONFIG_VAR_TX
  }

  // store estimated motion vector
  if (cpi->sf.adaptive_motion_search)
    store_pred_mv(x, ctx);

  // PARTITION_SPLIT
  // TODO(jingning): use the motion vectors given by the above search as
  // the starting point of motion search in the following partition type check.
  if (do_split) {
    subsize = get_subsize(bsize, PARTITION_SPLIT);
    if (bsize == BLOCK_8X8) {
      i = 4;
      if (cpi->sf.adaptive_pred_interp_filter && partition_none_allowed)
        pc_tree->leaf_split[0]->pred_interp_filter =
            ctx->mic.mbmi.interp_filter;
#if CONFIG_SUPERTX
      rd_pick_sb_modes(cpi, tile_data, x, mi_row, mi_col, &sum_rdc,
                       &sum_rate_nocoef, subsize, pc_tree->leaf_split[0],
                       INT64_MAX);
#else
      rd_pick_sb_modes(cpi, tile_data, x, mi_row, mi_col, &sum_rdc, subsize,
                       pc_tree->leaf_split[0], best_rdc.rdcost);
#endif  // CONFIG_SUPERTX
      if (sum_rdc.rate == INT_MAX) {
        sum_rdc.rdcost = INT64_MAX;
#if CONFIG_SUPERTX
        sum_rate_nocoef = INT_MAX;
#endif
      }
#if CONFIG_SUPERTX
      if (!frame_is_intra_only(cm) && sum_rdc.rdcost < INT64_MAX &&
          !xd->lossless[0]) {
        TX_SIZE supertx_size = max_txsize_lookup[bsize];
        best_partition = pc_tree->partitioning;
        pc_tree->partitioning = PARTITION_SPLIT;

        sum_rdc.rate += vp10_cost_bit(
            cm->fc->supertx_prob
            [partition_supertx_context_lookup[PARTITION_SPLIT]][supertx_size],
            0);
        sum_rdc.rdcost =
            RDCOST(x->rdmult, x->rddiv, sum_rdc.rate, sum_rdc.dist);
        if (is_inter_mode(pc_tree->leaf_split[0]->mic.mbmi.mode)) {
          TX_TYPE best_tx = DCT_DCT;
          tmp_rate = sum_rate_nocoef;
          tmp_dist = 0;
#if CONFIG_VAR_TX
          xd->above_txfm_context = cm->above_txfm_context + mi_col;
          xd->left_txfm_context =
              xd->left_txfm_context_buffer + (mi_row & MI_MASK);
          restore_context(x, mi_row, mi_col, a, l, sa, sl, ta, tl, bsize);
#else
          restore_context(x, mi_row, mi_col, a, l, sa, sl, bsize);
#endif  // CONFIG_VAR_TX
          rd_supertx_sb(cpi, td, tile_info, mi_row, mi_col, bsize,
                        &tmp_rate, &tmp_dist,
                        &best_tx,
                        pc_tree);

          tmp_rate += vp10_cost_bit(
              cm->fc->supertx_prob
              [partition_supertx_context_lookup[PARTITION_SPLIT]][supertx_size],
              1);
          tmp_rd = RDCOST(x->rdmult, x->rddiv, tmp_rate, tmp_dist);
          if (tmp_rd < sum_rdc.rdcost) {
            sum_rdc.rdcost = tmp_rd;
            sum_rdc.rate = tmp_rate;
            sum_rdc.dist = tmp_dist;
            update_supertx_param_sb(cpi, td, mi_row, mi_col, bsize,
                                    best_tx,
                                    supertx_size, pc_tree);
          }
        }
        pc_tree->partitioning = best_partition;
      }
#endif  // CONFIG_SUPERTX
    } else {
#if CONFIG_SUPERTX
      for (i = 0; i < 4 && sum_rdc.rdcost < INT64_MAX; ++i) {
#else
      for (i = 0; i < 4 && sum_rdc.rdcost < best_rdc.rdcost; ++i) {
#endif  // CONFIG_SUPERTX
        const int x_idx = (i & 1) * mi_step;
        const int y_idx = (i >> 1) * mi_step;

        if (mi_row + y_idx >= cm->mi_rows || mi_col + x_idx >= cm->mi_cols)
          continue;

        if (cpi->sf.adaptive_motion_search)
          load_pred_mv(x, ctx);

        pc_tree->split[i]->index = i;
#if CONFIG_SUPERTX
        rd_pick_partition(cpi, td, tile_data, tp,
                          mi_row + y_idx, mi_col + x_idx,
                          subsize, &this_rdc, &this_rate_nocoef,
                          INT64_MAX - sum_rdc.rdcost, pc_tree->split[i]);
#else
        rd_pick_partition(cpi, td, tile_data, tp,
                          mi_row + y_idx, mi_col + x_idx,
                          subsize, &this_rdc,
                          best_rdc.rdcost - sum_rdc.rdcost, pc_tree->split[i]);
#endif  // CONFIG_SUPERTX

        if (this_rdc.rate == INT_MAX) {
          sum_rdc.rdcost = INT64_MAX;
#if CONFIG_SUPERTX
          sum_rate_nocoef = INT_MAX;
#endif  // CONFIG_SUPERTX
          break;
        } else {
          sum_rdc.rate += this_rdc.rate;
          sum_rdc.dist += this_rdc.dist;
          sum_rdc.rdcost += this_rdc.rdcost;
#if CONFIG_SUPERTX
          sum_rate_nocoef += this_rate_nocoef;
#endif  // CONFIG_SUPERTX
        }
      }
#if CONFIG_SUPERTX
      if (!frame_is_intra_only(cm) &&
          sum_rdc.rdcost < INT64_MAX &&
          i == 4 && bsize <= MAX_SUPERTX_BLOCK_SIZE &&
          !xd->lossless[0]) {
        TX_SIZE supertx_size = max_txsize_lookup[bsize];
        best_partition = pc_tree->partitioning;
        pc_tree->partitioning = PARTITION_SPLIT;

        sum_rdc.rate += vp10_cost_bit(
            cm->fc->supertx_prob
            [partition_supertx_context_lookup[PARTITION_SPLIT]][supertx_size],
            0);
        sum_rdc.rdcost =
            RDCOST(x->rdmult, x->rddiv, sum_rdc.rate, sum_rdc.dist);

        if (!check_intra_sb(cpi, tile_info, mi_row, mi_col, bsize, pc_tree)) {
          TX_TYPE best_tx = DCT_DCT;

          tmp_rate = sum_rate_nocoef;
          tmp_dist = 0;
#if CONFIG_VAR_TX
          xd->above_txfm_context = cm->above_txfm_context + mi_col;
          xd->left_txfm_context =
              xd->left_txfm_context_buffer + (mi_row & MI_MASK);
          restore_context(x, mi_row, mi_col, a, l, sa, sl, ta, tl, bsize);
#else
          restore_context(x, mi_row, mi_col, a, l, sa, sl, bsize);
#endif  // CONFIG_VAR_TX
          rd_supertx_sb(cpi, td, tile_info, mi_row, mi_col, bsize,
                        &tmp_rate, &tmp_dist,
                        &best_tx,
                        pc_tree);

          tmp_rate += vp10_cost_bit(
              cm->fc->supertx_prob
              [partition_supertx_context_lookup[PARTITION_SPLIT]][supertx_size],
              1);
          tmp_rd = RDCOST(x->rdmult, x->rddiv, tmp_rate, tmp_dist);
          if (tmp_rd < sum_rdc.rdcost) {
            sum_rdc.rdcost = tmp_rd;
            sum_rdc.rate = tmp_rate;
            sum_rdc.dist = tmp_dist;
            update_supertx_param_sb(cpi, td, mi_row, mi_col, bsize,
                                    best_tx,
                                    supertx_size, pc_tree);
          }
        }
        pc_tree->partitioning = best_partition;
      }
#endif  // CONFIG_SUPERTX
    }

    if (sum_rdc.rdcost < best_rdc.rdcost && i == 4) {
      sum_rdc.rate += cpi->partition_cost[pl][PARTITION_SPLIT];
      sum_rdc.rdcost = RDCOST(x->rdmult, x->rddiv,
                              sum_rdc.rate, sum_rdc.dist);
#if CONFIG_SUPERTX
      sum_rate_nocoef += cpi->partition_cost[pl][PARTITION_SPLIT];
#endif  // CONFIG_SUPERTX

      if (sum_rdc.rdcost < best_rdc.rdcost) {
        best_rdc = sum_rdc;
#if CONFIG_SUPERTX
        best_rate_nocoef = sum_rate_nocoef;
        assert(best_rate_nocoef >= 0);
#endif  // CONFIG_SUPERTX
        pc_tree->partitioning = PARTITION_SPLIT;
      }
    } else {
      // skip rectangular partition test when larger block size
      // gives better rd cost
      if (cpi->sf.less_rectangular_check)
        do_rect &= !partition_none_allowed;
    }
#if CONFIG_VAR_TX
    xd->above_txfm_context = cm->above_txfm_context + mi_col;
    xd->left_txfm_context = xd->left_txfm_context_buffer + (mi_row & MI_MASK);
    restore_context(x, mi_row, mi_col, a, l, sa, sl, ta, tl, bsize);
#else
    restore_context(x, mi_row, mi_col, a, l, sa, sl, bsize);
#endif
  }  // if (do_split)

  // PARTITION_HORZ
  if (partition_horz_allowed &&
      (do_rect || vp10_active_h_edge(cpi, mi_row, mi_step))) {
    subsize = get_subsize(bsize, PARTITION_HORZ);
    if (cpi->sf.adaptive_motion_search)
      load_pred_mv(x, ctx);
    if (cpi->sf.adaptive_pred_interp_filter && bsize == BLOCK_8X8 &&
        partition_none_allowed)
      pc_tree->horizontal[0].pred_interp_filter =
          ctx->mic.mbmi.interp_filter;
    rd_pick_sb_modes(cpi, tile_data, x, mi_row, mi_col, &sum_rdc,
#if CONFIG_SUPERTX
                     &sum_rate_nocoef,
#endif  // CONFIG_SUPERTX
                     subsize, &pc_tree->horizontal[0], best_rdc.rdcost);

#if CONFIG_SUPERTX
    abort_flag = (sum_rdc.rdcost >= best_rd && bsize > BLOCK_8X8) ||
        (sum_rdc.rate == INT_MAX && bsize == BLOCK_8X8);
    if (sum_rdc.rdcost < INT64_MAX &&
#else
    if (sum_rdc.rdcost < best_rdc.rdcost &&
#endif  // CONFIG_SUPERTX
        mi_row + mi_step < cm->mi_rows &&
        bsize > BLOCK_8X8) {
      PICK_MODE_CONTEXT *ctx = &pc_tree->horizontal[0];
      update_state(cpi, td, ctx, mi_row, mi_col, subsize, 0);
      encode_superblock(cpi, td, tp, 0, mi_row, mi_col, subsize, ctx);

      if (cpi->sf.adaptive_motion_search)
        load_pred_mv(x, ctx);
      if (cpi->sf.adaptive_pred_interp_filter && bsize == BLOCK_8X8 &&
          partition_none_allowed)
        pc_tree->horizontal[1].pred_interp_filter =
            ctx->mic.mbmi.interp_filter;
#if CONFIG_SUPERTX
      rd_pick_sb_modes(cpi, tile_data, x, mi_row + mi_step, mi_col,
                       &this_rdc, &this_rate_nocoef,
                       subsize, &pc_tree->horizontal[1],
                       INT64_MAX);
#else
      rd_pick_sb_modes(cpi, tile_data, x, mi_row + mi_step, mi_col,
                       &this_rdc, subsize, &pc_tree->horizontal[1],
                       best_rdc.rdcost - sum_rdc.rdcost);
#endif  // CONFIG_SUPERTX
      if (this_rdc.rate == INT_MAX) {
        sum_rdc.rdcost = INT64_MAX;
#if CONFIG_SUPERTX
        sum_rate_nocoef = INT_MAX;
#endif  // CONFIG_SUPERTX
      } else {
        sum_rdc.rate += this_rdc.rate;
        sum_rdc.dist += this_rdc.dist;
        sum_rdc.rdcost += this_rdc.rdcost;
#if CONFIG_SUPERTX
        sum_rate_nocoef += this_rate_nocoef;
#endif  // CONFIG_SUPERTX
      }
    }

#if CONFIG_SUPERTX
    if (!frame_is_intra_only(cm) && !abort_flag &&
        sum_rdc.rdcost < INT64_MAX && bsize <= MAX_SUPERTX_BLOCK_SIZE &&
        !xd->lossless[0]) {
      TX_SIZE supertx_size = max_txsize_lookup[bsize];
      best_partition = pc_tree->partitioning;
      pc_tree->partitioning = PARTITION_HORZ;

      sum_rdc.rate += vp10_cost_bit(
          cm->fc->supertx_prob[partition_supertx_context_lookup[PARTITION_HORZ]]
          [supertx_size], 0);
      sum_rdc.rdcost = RDCOST(x->rdmult, x->rddiv, sum_rdc.rate, sum_rdc.dist);

      if (!check_intra_sb(cpi, tile_info, mi_row, mi_col, bsize, pc_tree)) {
        TX_TYPE best_tx = DCT_DCT;
        tmp_rate = sum_rate_nocoef;
        tmp_dist = 0;
#if CONFIG_VAR_TX
        xd->above_txfm_context = cm->above_txfm_context + mi_col;
        xd->left_txfm_context = xd->left_txfm_context_buffer + (mi_row & 0x07);
        restore_context(x, mi_row, mi_col, a, l, sa, sl, ta, tl, bsize);
#else
        restore_context(x, mi_row, mi_col, a, l, sa, sl, bsize);
#endif  // CONFIG_VAR_TX
        rd_supertx_sb(cpi, td, tile_info, mi_row, mi_col, bsize,
                      &tmp_rate, &tmp_dist,
                      &best_tx,
                      pc_tree);

        tmp_rate += vp10_cost_bit(
            cm->fc->supertx_prob
            [partition_supertx_context_lookup[PARTITION_HORZ]][supertx_size],
            1);
        tmp_rd = RDCOST(x->rdmult, x->rddiv, tmp_rate, tmp_dist);
        if (tmp_rd < sum_rdc.rdcost) {
          sum_rdc.rdcost = tmp_rd;
          sum_rdc.rate = tmp_rate;
          sum_rdc.dist = tmp_dist;
          update_supertx_param_sb(cpi, td, mi_row, mi_col, bsize,
                                  best_tx,
                                  supertx_size, pc_tree);
        }
      }
      pc_tree->partitioning = best_partition;
    }
#endif  // CONFIG_SUPERTX

    if (sum_rdc.rdcost < best_rdc.rdcost) {
      sum_rdc.rate += cpi->partition_cost[pl][PARTITION_HORZ];
      sum_rdc.rdcost = RDCOST(x->rdmult, x->rddiv, sum_rdc.rate, sum_rdc.dist);
#if CONFIG_SUPERTX
      sum_rate_nocoef += cpi->partition_cost[pl][PARTITION_HORZ];
#endif  // CONFIG_SUPERTX
      if (sum_rdc.rdcost < best_rdc.rdcost) {
        best_rdc = sum_rdc;
#if CONFIG_SUPERTX
        best_rate_nocoef = sum_rate_nocoef;
        assert(best_rate_nocoef >= 0);
#endif  // CONFIG_SUPERTX
        pc_tree->partitioning = PARTITION_HORZ;
      }
    }
#if CONFIG_VAR_TX
    xd->above_txfm_context = cm->above_txfm_context + mi_col;
    xd->left_txfm_context = xd->left_txfm_context_buffer + (mi_row & MI_MASK);
    restore_context(x, mi_row, mi_col, a, l, sa, sl, ta, tl, bsize);
#else
    restore_context(x, mi_row, mi_col, a, l, sa, sl, bsize);
#endif
  }
  // PARTITION_VERT
  if (partition_vert_allowed &&
      (do_rect || vp10_active_v_edge(cpi, mi_col, mi_step))) {
    subsize = get_subsize(bsize, PARTITION_VERT);

    if (cpi->sf.adaptive_motion_search)
      load_pred_mv(x, ctx);
    if (cpi->sf.adaptive_pred_interp_filter && bsize == BLOCK_8X8 &&
        partition_none_allowed)
      pc_tree->vertical[0].pred_interp_filter =
          ctx->mic.mbmi.interp_filter;
    rd_pick_sb_modes(cpi, tile_data, x, mi_row, mi_col, &sum_rdc,
#if CONFIG_SUPERTX
                     &sum_rate_nocoef,
#endif  // CONFIG_SUPERTX
                     subsize, &pc_tree->vertical[0], best_rdc.rdcost);
#if CONFIG_SUPERTX
    abort_flag = (sum_rdc.rdcost >= best_rd && bsize > BLOCK_8X8) ||
                 (sum_rdc.rate == INT_MAX && bsize == BLOCK_8X8);
    if (sum_rdc.rdcost < INT64_MAX &&
#else
    if (sum_rdc.rdcost < best_rdc.rdcost &&
#endif  // CONFIG_SUPERTX
        mi_col + mi_step < cm->mi_cols &&
        bsize > BLOCK_8X8) {
      update_state(cpi, td, &pc_tree->vertical[0], mi_row, mi_col, subsize, 0);
      encode_superblock(cpi, td, tp, 0, mi_row, mi_col, subsize,
                        &pc_tree->vertical[0]);

      if (cpi->sf.adaptive_motion_search)
        load_pred_mv(x, ctx);
      if (cpi->sf.adaptive_pred_interp_filter && bsize == BLOCK_8X8 &&
          partition_none_allowed)
        pc_tree->vertical[1].pred_interp_filter =
            ctx->mic.mbmi.interp_filter;
#if CONFIG_SUPERTX
      rd_pick_sb_modes(cpi, tile_data, x, mi_row, mi_col + mi_step, &this_rdc,
                       &this_rate_nocoef, subsize, &pc_tree->vertical[1],
                       INT64_MAX - sum_rdc.rdcost);
#else
      rd_pick_sb_modes(cpi, tile_data, x, mi_row, mi_col + mi_step,
                       &this_rdc, subsize,
                       &pc_tree->vertical[1], best_rdc.rdcost - sum_rdc.rdcost);
#endif  // CONFIG_SUPERTX
      if (this_rdc.rate == INT_MAX) {
        sum_rdc.rdcost = INT64_MAX;
#if CONFIG_SUPERTX
        sum_rate_nocoef = INT_MAX;
#endif  // CONFIG_SUPERTX
      } else {
        sum_rdc.rate += this_rdc.rate;
        sum_rdc.dist += this_rdc.dist;
        sum_rdc.rdcost += this_rdc.rdcost;
#if CONFIG_SUPERTX
        sum_rate_nocoef += this_rate_nocoef;
#endif  // CONFIG_SUPERTX
      }
    }
#if CONFIG_SUPERTX
    if (!frame_is_intra_only(cm) && !abort_flag &&
        sum_rdc.rdcost < INT64_MAX && bsize <= MAX_SUPERTX_BLOCK_SIZE &&
        !xd->lossless[0]) {
      TX_SIZE supertx_size = max_txsize_lookup[bsize];
      best_partition = pc_tree->partitioning;
      pc_tree->partitioning = PARTITION_VERT;
      sum_rdc.rate += vp10_cost_bit(
          cm->fc->supertx_prob[partition_supertx_context_lookup[PARTITION_VERT]]
                              [supertx_size], 0);
      sum_rdc.rdcost = RDCOST(x->rdmult, x->rddiv, sum_rdc.rate, sum_rdc.dist);

      if (!check_intra_sb(cpi, tile_info, mi_row, mi_col, bsize, pc_tree)) {
        TX_TYPE best_tx = DCT_DCT;

        tmp_rate = sum_rate_nocoef;
        tmp_dist = 0;
#if CONFIG_VAR_TX
        xd->above_txfm_context = cm->above_txfm_context + mi_col;
        xd->left_txfm_context = xd->left_txfm_context_buffer + (mi_row & 0x07);
        restore_context(x, mi_row, mi_col, a, l, sa, sl, ta, tl, bsize);
#else
        restore_context(x, mi_row, mi_col, a, l, sa, sl, bsize);
#endif  // CONFIG_VAR_TX
        rd_supertx_sb(cpi, td, tile_info, mi_row, mi_col, bsize,
                      &tmp_rate, &tmp_dist,
                      &best_tx,
                      pc_tree);

        tmp_rate += vp10_cost_bit(
            cm->fc->supertx_prob
            [partition_supertx_context_lookup[PARTITION_VERT]][supertx_size],
            1);
        tmp_rd = RDCOST(x->rdmult, x->rddiv, tmp_rate, tmp_dist);
        if (tmp_rd < sum_rdc.rdcost) {
          sum_rdc.rdcost = tmp_rd;
          sum_rdc.rate = tmp_rate;
          sum_rdc.dist = tmp_dist;
          update_supertx_param_sb(cpi, td, mi_row, mi_col, bsize,
                                  best_tx,
                                  supertx_size, pc_tree);
        }
      }
      pc_tree->partitioning = best_partition;
    }
#endif  // CONFIG_SUPERTX

    if (sum_rdc.rdcost < best_rdc.rdcost) {
      sum_rdc.rate += cpi->partition_cost[pl][PARTITION_VERT];
      sum_rdc.rdcost = RDCOST(x->rdmult, x->rddiv,
                              sum_rdc.rate, sum_rdc.dist);
#if CONFIG_SUPERTX
      sum_rate_nocoef += cpi->partition_cost[pl][PARTITION_VERT];
#endif  // CONFIG_SUPERTX
      if (sum_rdc.rdcost < best_rdc.rdcost) {
        best_rdc = sum_rdc;
#if CONFIG_SUPERTX
        best_rate_nocoef = sum_rate_nocoef;
        assert(best_rate_nocoef >= 0);
#endif  // CONFIG_SUPERTX
        pc_tree->partitioning = PARTITION_VERT;
      }
    }
#if CONFIG_VAR_TX
    xd->above_txfm_context = cm->above_txfm_context + mi_col;
    xd->left_txfm_context = xd->left_txfm_context_buffer + (mi_row & MI_MASK);
    restore_context(x, mi_row, mi_col, a, l, sa, sl, ta, tl, bsize);
#else
    restore_context(x, mi_row, mi_col, a, l, sa, sl, bsize);
#endif
  }

  // TODO(jbb): This code added so that we avoid static analysis
  // warning related to the fact that best_rd isn't used after this
  // point.  This code should be refactored so that the duplicate
  // checks occur in some sub function and thus are used...
  (void) best_rd;
  *rd_cost = best_rdc;
#if CONFIG_SUPERTX
  *rate_nocoef = best_rate_nocoef;
#endif  // CONFIG_SUPERTX

  if (best_rdc.rate < INT_MAX && best_rdc.dist < INT64_MAX &&
      pc_tree->index != 3) {
    int output_enabled = (bsize == BLOCK_64X64);
    encode_sb(cpi, td, tile_info, tp, mi_row, mi_col, output_enabled,
              bsize, pc_tree);
  }

  if (bsize == BLOCK_64X64) {
    assert(tp_orig < *tp || (tp_orig == *tp && xd->mi[0]->mbmi.skip));
    assert(best_rdc.rate < INT_MAX);
    assert(best_rdc.dist < INT64_MAX);
  } else {
    assert(tp_orig == *tp);
  }
}

static void encode_rd_sb_row(VP10_COMP *cpi,
                             ThreadData *td,
                             TileDataEnc *tile_data,
                             int mi_row,
                             TOKENEXTRA **tp) {
  VP10_COMMON *const cm = &cpi->common;
  TileInfo *const tile_info = &tile_data->tile_info;
  MACROBLOCK *const x = &td->mb;
  MACROBLOCKD *const xd = &x->e_mbd;
  SPEED_FEATURES *const sf = &cpi->sf;
  int mi_col;

  // Initialize the left context for the new SB row
  memset(&xd->left_context, 0, sizeof(xd->left_context));
  memset(xd->left_seg_context, 0, sizeof(xd->left_seg_context));
#if CONFIG_VAR_TX
  memset(xd->left_txfm_context_buffer, 0,
         sizeof(xd->left_txfm_context_buffer));
#endif
  // Code each SB in the row
  for (mi_col = tile_info->mi_col_start; mi_col < tile_info->mi_col_end;
       mi_col += MI_BLOCK_SIZE) {
    const struct segmentation *const seg = &cm->seg;
    int dummy_rate;
    int64_t dummy_dist;
    RD_COST dummy_rdc;
#if CONFIG_SUPERTX
    int dummy_rate_nocoef;
#endif  // CONFIG_SUPERTX
    int i;
    int seg_skip = 0;

    const int idx_str = cm->mi_stride * mi_row + mi_col;
    MODE_INFO **mi = cm->mi_grid_visible + idx_str;

    if (sf->adaptive_pred_interp_filter) {
      for (i = 0; i < 64; ++i)
        td->leaf_tree[i].pred_interp_filter = SWITCHABLE;

      for (i = 0; i < 64; ++i) {
        td->pc_tree[i].vertical[0].pred_interp_filter = SWITCHABLE;
        td->pc_tree[i].vertical[1].pred_interp_filter = SWITCHABLE;
        td->pc_tree[i].horizontal[0].pred_interp_filter = SWITCHABLE;
        td->pc_tree[i].horizontal[1].pred_interp_filter = SWITCHABLE;
      }
    }

    vp10_zero(x->pred_mv);
    td->pc_root->index = 0;

    if (seg->enabled) {
      const uint8_t *const map = seg->update_map ? cpi->segmentation_map
                                                 : cm->last_frame_seg_map;
      int segment_id = get_segment_id(cm, map, BLOCK_64X64, mi_row, mi_col);
      seg_skip = segfeature_active(seg, segment_id, SEG_LVL_SKIP);
    }

    x->source_variance = UINT_MAX;
    if (sf->partition_search_type == FIXED_PARTITION || seg_skip) {
      const BLOCK_SIZE bsize =
          seg_skip ? BLOCK_64X64 : sf->always_this_block_size;
      set_offsets(cpi, tile_info, x, mi_row, mi_col, BLOCK_64X64);
      set_fixed_partitioning(cpi, tile_info, mi, mi_row, mi_col, bsize);
      rd_use_partition(cpi, td, tile_data, mi, tp, mi_row, mi_col,
                       BLOCK_64X64, &dummy_rate, &dummy_dist,
#if CONFIG_SUPERTX
                       &dummy_rate_nocoef,
#endif  // CONFIG_SUPERTX
                       1, td->pc_root);
    } else if (cpi->partition_search_skippable_frame) {
      BLOCK_SIZE bsize;
      set_offsets(cpi, tile_info, x, mi_row, mi_col, BLOCK_64X64);
      bsize = get_rd_var_based_fixed_partition(cpi, x, mi_row, mi_col);
      set_fixed_partitioning(cpi, tile_info, mi, mi_row, mi_col, bsize);
      rd_use_partition(cpi, td, tile_data, mi, tp, mi_row, mi_col,
                       BLOCK_64X64, &dummy_rate, &dummy_dist,
#if CONFIG_SUPERTX
                       &dummy_rate_nocoef,
#endif  // CONFIG_SUPERTX
                       1, td->pc_root);
    } else if (sf->partition_search_type == VAR_BASED_PARTITION &&
               cm->frame_type != KEY_FRAME) {
      choose_partitioning(cpi, tile_info, x, mi_row, mi_col);
      rd_use_partition(cpi, td, tile_data, mi, tp, mi_row, mi_col,
                       BLOCK_64X64, &dummy_rate, &dummy_dist,
#if CONFIG_SUPERTX
                       &dummy_rate_nocoef,
#endif  // CONFIG_SUPERTX
                       1, td->pc_root);
    } else {
      // If required set upper and lower partition size limits
      if (sf->auto_min_max_partition_size) {
        set_offsets(cpi, tile_info, x, mi_row, mi_col, BLOCK_64X64);
        rd_auto_partition_range(cpi, tile_info, xd, mi_row, mi_col,
                                &x->min_partition_size,
                                &x->max_partition_size);
      }
      rd_pick_partition(cpi, td, tile_data, tp, mi_row, mi_col, BLOCK_64X64,
                        &dummy_rdc,
#if CONFIG_SUPERTX
                        &dummy_rate_nocoef,
#endif  // CONFIG_SUPERTX
                        INT64_MAX, td->pc_root);
    }
  }
}

static void init_encode_frame_mb_context(VP10_COMP *cpi) {
  MACROBLOCK *const x = &cpi->td.mb;
  VP10_COMMON *const cm = &cpi->common;
  MACROBLOCKD *const xd = &x->e_mbd;
  const int aligned_mi_cols = mi_cols_aligned_to_sb(cm->mi_cols);

  // Copy data over into macro block data structures.
  vp10_setup_src_planes(x, cpi->Source, 0, 0);

  vp10_setup_block_planes(&x->e_mbd, cm->subsampling_x, cm->subsampling_y);

  // Note: this memset assumes above_context[0], [1] and [2]
  // are allocated as part of the same buffer.
  memset(xd->above_context[0], 0,
         sizeof(*xd->above_context[0]) *
         2 * aligned_mi_cols * MAX_MB_PLANE);
  memset(xd->above_seg_context, 0,
         sizeof(*xd->above_seg_context) * aligned_mi_cols);
#if CONFIG_VAR_TX
  memset(cm->above_txfm_context, 0,
         sizeof(*xd->above_txfm_context) * aligned_mi_cols);
#endif
}

static int check_dual_ref_flags(VP10_COMP *cpi) {
  const int ref_flags = cpi->ref_frame_flags;

  if (segfeature_active(&cpi->common.seg, 1, SEG_LVL_REF_FRAME)) {
    return 0;
  } else {
    return (!!(ref_flags & VP9_GOLD_FLAG) +
            !!(ref_flags & VP9_LAST_FLAG) +
#if CONFIG_EXT_REFS
            !!(ref_flags & VP9_LAST2_FLAG) +
            !!(ref_flags & VP9_LAST3_FLAG) +
            !!(ref_flags & VP9_LAST4_FLAG) +
#endif  // CONFIG_EXT_REFS
            !!(ref_flags & VP9_ALT_FLAG)) >= 2;
  }
}

#if !CONFIG_VAR_TX
static void reset_skip_tx_size(VP10_COMMON *cm, TX_SIZE max_tx_size) {
  int mi_row, mi_col;
  const int mis = cm->mi_stride;
  MODE_INFO **mi_ptr = cm->mi_grid_visible;

  for (mi_row = 0; mi_row < cm->mi_rows; ++mi_row, mi_ptr += mis) {
    for (mi_col = 0; mi_col < cm->mi_cols; ++mi_col) {
      if (mi_ptr[mi_col]->mbmi.tx_size > max_tx_size)
        mi_ptr[mi_col]->mbmi.tx_size = max_tx_size;
    }
  }
}
#endif

static MV_REFERENCE_FRAME get_frame_type(const VP10_COMP *cpi) {
  if (frame_is_intra_only(&cpi->common))
    return INTRA_FRAME;
  else if (cpi->rc.is_src_frame_alt_ref && cpi->refresh_golden_frame)
    return ALTREF_FRAME;
  else if (cpi->refresh_golden_frame || cpi->refresh_alt_ref_frame)
    return GOLDEN_FRAME;
  else
    // TODO(zoeliu): TO investigate whether a frame_type other than
    // INTRA/ALTREF/GOLDEN/LAST needs to be specified seperately.
    return LAST_FRAME;
}

static TX_MODE select_tx_mode(const VP10_COMP *cpi, MACROBLOCKD *const xd) {
  if (xd->lossless[0])
    return ONLY_4X4;
  if (cpi->sf.tx_size_search_method == USE_LARGESTALL)
    return ALLOW_32X32;
  else if (cpi->sf.tx_size_search_method == USE_FULL_RD||
           cpi->sf.tx_size_search_method == USE_TX_8X8)
    return TX_MODE_SELECT;
  else
    return cpi->common.tx_mode;
}

void vp10_init_tile_data(VP10_COMP *cpi) {
  VP10_COMMON *const cm = &cpi->common;
  const int tile_cols = 1 << cm->log2_tile_cols;
  const int tile_rows = 1 << cm->log2_tile_rows;
  int tile_col, tile_row;
  TOKENEXTRA *pre_tok = cpi->tile_tok[0][0];
  int tile_tok = 0;

  if (cpi->tile_data == NULL || cpi->allocated_tiles < tile_cols * tile_rows) {
    if (cpi->tile_data != NULL)
      vpx_free(cpi->tile_data);
    CHECK_MEM_ERROR(cm, cpi->tile_data,
        vpx_malloc(tile_cols * tile_rows * sizeof(*cpi->tile_data)));
    cpi->allocated_tiles = tile_cols * tile_rows;

    for (tile_row = 0; tile_row < tile_rows; ++tile_row)
      for (tile_col = 0; tile_col < tile_cols; ++tile_col) {
        TileDataEnc *tile_data =
            &cpi->tile_data[tile_row * tile_cols + tile_col];
        int i, j;
        for (i = 0; i < BLOCK_SIZES; ++i) {
          for (j = 0; j < MAX_MODES; ++j) {
            tile_data->thresh_freq_fact[i][j] = 32;
            tile_data->mode_map[i][j] = j;
          }
        }
      }
  }

  for (tile_row = 0; tile_row < tile_rows; ++tile_row) {
    for (tile_col = 0; tile_col < tile_cols; ++tile_col) {
      TileInfo *tile_info =
          &cpi->tile_data[tile_row * tile_cols + tile_col].tile_info;
      vp10_tile_init(tile_info, cm, tile_row, tile_col);

      cpi->tile_tok[tile_row][tile_col] = pre_tok + tile_tok;
      pre_tok = cpi->tile_tok[tile_row][tile_col];
      tile_tok = allocated_tokens(*tile_info);
    }
  }
}

void vp10_encode_tile(VP10_COMP *cpi, ThreadData *td,
                     int tile_row, int tile_col) {
  VP10_COMMON *const cm = &cpi->common;
  const int tile_cols = 1 << cm->log2_tile_cols;
  TileDataEnc *this_tile =
      &cpi->tile_data[tile_row * tile_cols + tile_col];
  const TileInfo * const tile_info = &this_tile->tile_info;
  TOKENEXTRA *tok = cpi->tile_tok[tile_row][tile_col];
  int mi_row;

  // Set up pointers to per thread motion search counters.
  td->mb.m_search_count_ptr = &td->rd_counts.m_search_count;
  td->mb.ex_search_count_ptr = &td->rd_counts.ex_search_count;

  for (mi_row = tile_info->mi_row_start; mi_row < tile_info->mi_row_end;
       mi_row += MI_BLOCK_SIZE) {
    encode_rd_sb_row(cpi, td, this_tile, mi_row, &tok);
  }
  cpi->tok_count[tile_row][tile_col] =
      (unsigned int)(tok - cpi->tile_tok[tile_row][tile_col]);
  assert(tok - cpi->tile_tok[tile_row][tile_col] <=
      allocated_tokens(*tile_info));
}

static void encode_tiles(VP10_COMP *cpi) {
  VP10_COMMON *const cm = &cpi->common;
  const int tile_cols = 1 << cm->log2_tile_cols;
  const int tile_rows = 1 << cm->log2_tile_rows;
  int tile_col, tile_row;

  vp10_init_tile_data(cpi);

  for (tile_row = 0; tile_row < tile_rows; ++tile_row)
    for (tile_col = 0; tile_col < tile_cols; ++tile_col)
      vp10_encode_tile(cpi, &cpi->td, tile_row, tile_col);
}

#if CONFIG_FP_MB_STATS
static int input_fpmb_stats(FIRSTPASS_MB_STATS *firstpass_mb_stats,
                            VP10_COMMON *cm, uint8_t **this_frame_mb_stats) {
  uint8_t *mb_stats_in = firstpass_mb_stats->mb_stats_start +
      cm->current_video_frame * cm->MBs * sizeof(uint8_t);

  if (mb_stats_in > firstpass_mb_stats->mb_stats_end)
    return EOF;

  *this_frame_mb_stats = mb_stats_in;

  return 1;
}
#endif

static void encode_frame_internal(VP10_COMP *cpi) {
  ThreadData *const td = &cpi->td;
  MACROBLOCK *const x = &td->mb;
  VP10_COMMON *const cm = &cpi->common;
  MACROBLOCKD *const xd = &x->e_mbd;
  RD_COUNTS *const rdc = &cpi->td.rd_counts;
  int i;

  xd->mi = cm->mi_grid_visible;
  xd->mi[0] = cm->mi;

  vp10_zero(*td->counts);
  vp10_zero(rdc->coef_counts);
  vp10_zero(rdc->comp_pred_diff);
  vp10_zero(rdc->filter_diff);
  rdc->m_search_count = 0;   // Count of motion search hits.
  rdc->ex_search_count = 0;  // Exhaustive mesh search hits.

  for (i = 0; i < MAX_SEGMENTS; ++i) {
    const int qindex = cm->seg.enabled ?
        vp10_get_qindex(&cm->seg, i, cm->base_qindex) : cm->base_qindex;
    xd->lossless[i] = qindex == 0 &&
                      cm->y_dc_delta_q == 0 &&
                      cm->uv_dc_delta_q == 0 &&
                      cm->uv_ac_delta_q == 0;
  }

  if (!cm->seg.enabled && xd->lossless[0])
    x->optimize = 0;

  cm->tx_mode = select_tx_mode(cpi, xd);

  vp10_frame_init_quantizer(cpi);

  vp10_initialize_rd_consts(cpi);
  vp10_initialize_me_consts(cpi, x, cm->base_qindex);
  init_encode_frame_mb_context(cpi);
  cm->use_prev_frame_mvs = !cm->error_resilient_mode &&
                           cm->width == cm->last_width &&
                           cm->height == cm->last_height &&
                           !cm->intra_only &&
                           cm->last_show_frame;
  // Special case: set prev_mi to NULL when the previous mode info
  // context cannot be used.
  cm->prev_mi = cm->use_prev_frame_mvs ?
                cm->prev_mip + cm->mi_stride + 1 : NULL;

  x->quant_fp = cpi->sf.use_quant_fp;
  vp10_zero(x->skip_txfm);
#if CONFIG_VAR_TX
  vp10_zero(x->blk_skip);
#endif

  {
    struct vpx_usec_timer emr_timer;
    vpx_usec_timer_start(&emr_timer);

#if CONFIG_FP_MB_STATS
  if (cpi->use_fp_mb_stats) {
    input_fpmb_stats(&cpi->twopass.firstpass_mb_stats, cm,
                     &cpi->twopass.this_frame_mb_stats);
  }
#endif

    // If allowed, encoding tiles in parallel with one thread handling one tile.
    if (VPXMIN(cpi->oxcf.max_threads, 1 << cm->log2_tile_cols) > 1)
      vp10_encode_tiles_mt(cpi);
    else
      encode_tiles(cpi);

    vpx_usec_timer_mark(&emr_timer);
    cpi->time_encode_sb_row += vpx_usec_timer_elapsed(&emr_timer);
  }

#if 0
  // Keep record of the total distortion this time around for future use
  cpi->last_frame_distortion = cpi->frame_distortion;
#endif
}

static INTERP_FILTER get_interp_filter(
    const int64_t threshes[SWITCHABLE_FILTER_CONTEXTS], int is_alt_ref) {
#if CONFIG_EXT_INTERP
  if (!is_alt_ref &&
      threshes[EIGHTTAP_SMOOTH2] > threshes[EIGHTTAP_SMOOTH] &&
      threshes[EIGHTTAP_SMOOTH2] > threshes[EIGHTTAP] &&
      threshes[EIGHTTAP_SMOOTH2] > threshes[EIGHTTAP_SHARP] &&
      threshes[EIGHTTAP_SMOOTH2] > threshes[SWITCHABLE - 1]) {
    return EIGHTTAP_SMOOTH2;
  }
#endif  // CONFIG_EXT_INTERP
  if (!is_alt_ref &&
      threshes[EIGHTTAP_SMOOTH] > threshes[EIGHTTAP] &&
      threshes[EIGHTTAP_SMOOTH] > threshes[EIGHTTAP_SHARP] &&
      threshes[EIGHTTAP_SMOOTH] > threshes[SWITCHABLE - 1]) {
    return EIGHTTAP_SMOOTH;
  } else if (threshes[EIGHTTAP_SHARP] > threshes[EIGHTTAP] &&
             threshes[EIGHTTAP_SHARP] > threshes[SWITCHABLE - 1]) {
    return EIGHTTAP_SHARP;
  } else if (threshes[EIGHTTAP] > threshes[SWITCHABLE - 1]) {
    return EIGHTTAP;
  } else {
    return SWITCHABLE;
  }
}

void vp10_encode_frame(VP10_COMP *cpi) {
  VP10_COMMON *const cm = &cpi->common;

  // In the longer term the encoder should be generalized to match the
  // decoder such that we allow compound where one of the 3 buffers has a
  // different sign bias and that buffer is then the fixed ref. However, this
  // requires further work in the rd loop. For now the only supported encoder
  // side behavior is where the ALT ref buffer has opposite sign bias to
  // the other two.
  if (!frame_is_intra_only(cm)) {
    if ((cm->ref_frame_sign_bias[ALTREF_FRAME] ==
             cm->ref_frame_sign_bias[GOLDEN_FRAME]) ||
        (cm->ref_frame_sign_bias[ALTREF_FRAME] ==
             cm->ref_frame_sign_bias[LAST_FRAME])) {
      cpi->allow_comp_inter_inter = 0;
    } else {
      cpi->allow_comp_inter_inter = 1;
      cm->comp_fixed_ref = ALTREF_FRAME;
      cm->comp_var_ref[0] = LAST_FRAME;
#if CONFIG_EXT_REFS
      cm->comp_var_ref[1] = LAST2_FRAME;
      cm->comp_var_ref[2] = LAST3_FRAME;
      cm->comp_var_ref[3] = LAST4_FRAME;
      cm->comp_var_ref[4] = GOLDEN_FRAME;
#else
      cm->comp_var_ref[1] = GOLDEN_FRAME;
#endif  // CONFIG_EXT_REFS
    }
  } else {
    cpi->allow_comp_inter_inter = 0;
  }

  if (cpi->sf.frame_parameter_update) {
    int i;
    RD_OPT *const rd_opt = &cpi->rd;
    FRAME_COUNTS *counts = cpi->td.counts;
    RD_COUNTS *const rdc = &cpi->td.rd_counts;

    // This code does a single RD pass over the whole frame assuming
    // either compound, single or hybrid prediction as per whatever has
    // worked best for that type of frame in the past.
    // It also predicts whether another coding mode would have worked
    // better than this coding mode. If that is the case, it remembers
    // that for subsequent frames.
    // It does the same analysis for transform size selection also.
    //
    // TODO(zoeliu): TO investigate whether a frame_type other than
    // INTRA/ALTREF/GOLDEN/LAST needs to be specified seperately.
    const MV_REFERENCE_FRAME frame_type = get_frame_type(cpi);
    int64_t *const mode_thrs = rd_opt->prediction_type_threshes[frame_type];
    int64_t *const filter_thrs = rd_opt->filter_threshes[frame_type];
    const int is_alt_ref = frame_type == ALTREF_FRAME;

    /* prediction (compound, single or hybrid) mode selection */
    if (is_alt_ref || !cpi->allow_comp_inter_inter)
      cm->reference_mode = SINGLE_REFERENCE;
    else if (mode_thrs[COMPOUND_REFERENCE] > mode_thrs[SINGLE_REFERENCE] &&
             mode_thrs[COMPOUND_REFERENCE] >
                 mode_thrs[REFERENCE_MODE_SELECT] &&
             check_dual_ref_flags(cpi) &&
             cpi->static_mb_pct == 100)
      cm->reference_mode = COMPOUND_REFERENCE;
    else if (mode_thrs[SINGLE_REFERENCE] > mode_thrs[REFERENCE_MODE_SELECT])
      cm->reference_mode = SINGLE_REFERENCE;
    else
      cm->reference_mode = REFERENCE_MODE_SELECT;

    if (cm->interp_filter == SWITCHABLE) {
      cm->interp_filter = get_interp_filter(filter_thrs, is_alt_ref);
    }

    encode_frame_internal(cpi);

    for (i = 0; i < REFERENCE_MODES; ++i)
      mode_thrs[i] = (mode_thrs[i] + rdc->comp_pred_diff[i] / cm->MBs) / 2;

    for (i = 0; i < SWITCHABLE_FILTER_CONTEXTS; ++i)
      filter_thrs[i] = (filter_thrs[i] + rdc->filter_diff[i] / cm->MBs) / 2;

    if (cm->reference_mode == REFERENCE_MODE_SELECT) {
      int single_count_zero = 0;
      int comp_count_zero = 0;

      for (i = 0; i < COMP_INTER_CONTEXTS; i++) {
        single_count_zero += counts->comp_inter[i][0];
        comp_count_zero += counts->comp_inter[i][1];
      }

      if (comp_count_zero == 0) {
        cm->reference_mode = SINGLE_REFERENCE;
        vp10_zero(counts->comp_inter);
      } else if (single_count_zero == 0) {
        cm->reference_mode = COMPOUND_REFERENCE;
        vp10_zero(counts->comp_inter);
      }
    }

#if !CONFIG_VAR_TX
    if (cm->tx_mode == TX_MODE_SELECT) {
      int count4x4 = 0;
      int count8x8_lp = 0, count8x8_8x8p = 0;
      int count16x16_16x16p = 0, count16x16_lp = 0;
      int count32x32 = 0;

      for (i = 0; i < TX_SIZE_CONTEXTS; ++i) {
        count4x4 += counts->tx.p32x32[i][TX_4X4];
        count4x4 += counts->tx.p16x16[i][TX_4X4];
        count4x4 += counts->tx.p8x8[i][TX_4X4];

        count8x8_lp += counts->tx.p32x32[i][TX_8X8];
        count8x8_lp += counts->tx.p16x16[i][TX_8X8];
        count8x8_8x8p += counts->tx.p8x8[i][TX_8X8];

        count16x16_16x16p += counts->tx.p16x16[i][TX_16X16];
        count16x16_lp += counts->tx.p32x32[i][TX_16X16];
        count32x32 += counts->tx.p32x32[i][TX_32X32];
      }
      if (count4x4 == 0 && count16x16_lp == 0 && count16x16_16x16p == 0 &&
#if CONFIG_SUPERTX
          cm->counts.supertx_size[TX_16X16] == 0 &&
          cm->counts.supertx_size[TX_32X32] == 0 &&
#endif  // CONFIG_SUPERTX
          count32x32 == 0) {
        cm->tx_mode = ALLOW_8X8;
        reset_skip_tx_size(cm, TX_8X8);
      } else if (count8x8_8x8p == 0 && count16x16_16x16p == 0 &&
                 count8x8_lp == 0 && count16x16_lp == 0 &&
#if CONFIG_SUPERTX
                 cm->counts.supertx_size[TX_8X8] == 0 &&
                 cm->counts.supertx_size[TX_16X16] == 0 &&
                 cm->counts.supertx_size[TX_32X32] == 0 &&
#endif  // CONFIG_SUPERTX
                 count32x32 == 0) {
        cm->tx_mode = ONLY_4X4;
        reset_skip_tx_size(cm, TX_4X4);
      } else if (count8x8_lp == 0 && count16x16_lp == 0 &&
                 count4x4 == 0) {
        cm->tx_mode = ALLOW_32X32;
      } else if (count32x32 == 0 && count8x8_lp == 0 &&
#if CONFIG_SUPERTX
                 cm->counts.supertx_size[TX_32X32] == 0 &&
#endif  // CONFIG_SUPERTX
                 count4x4 == 0) {
        cm->tx_mode = ALLOW_16X16;
        reset_skip_tx_size(cm, TX_16X16);
      }
    }
#endif
  } else {
    cm->reference_mode = SINGLE_REFERENCE;
    encode_frame_internal(cpi);
  }
}

static void sum_intra_stats(FRAME_COUNTS *counts, const MODE_INFO *mi,
                            const MODE_INFO *above_mi, const MODE_INFO *left_mi,
                            const int intraonly) {
  const PREDICTION_MODE y_mode = mi->mbmi.mode;
  const PREDICTION_MODE uv_mode = mi->mbmi.uv_mode;
  const BLOCK_SIZE bsize = mi->mbmi.sb_type;

  if (bsize < BLOCK_8X8) {
    int idx, idy;
    const int num_4x4_w = num_4x4_blocks_wide_lookup[bsize];
    const int num_4x4_h = num_4x4_blocks_high_lookup[bsize];
    for (idy = 0; idy < 2; idy += num_4x4_h)
      for (idx = 0; idx < 2; idx += num_4x4_w) {
        const int bidx = idy * 2 + idx;
        const PREDICTION_MODE bmode = mi->bmi[bidx].as_mode;
        if (intraonly) {
          const PREDICTION_MODE a = vp10_above_block_mode(mi, above_mi, bidx);
          const PREDICTION_MODE l = vp10_left_block_mode(mi, left_mi, bidx);
          ++counts->kf_y_mode[a][l][bmode];
        } else {
          ++counts->y_mode[0][bmode];
        }
      }
  } else {
    if (intraonly) {
      const PREDICTION_MODE above = vp10_above_block_mode(mi, above_mi, 0);
      const PREDICTION_MODE left = vp10_left_block_mode(mi, left_mi, 0);
      ++counts->kf_y_mode[above][left][y_mode];
    } else {
      ++counts->y_mode[size_group_lookup[bsize]][y_mode];
    }
  }

  ++counts->uv_mode[y_mode][uv_mode];
}

#if CONFIG_VAR_TX
static void update_txfm_count(MACROBLOCKD *xd,
                              FRAME_COUNTS *counts,
                              TX_SIZE tx_size, int blk_row, int blk_col) {
  MB_MODE_INFO *mbmi = &xd->mi[0]->mbmi;
  int tx_idx = (blk_row >> 1) * 8 + (blk_col >> 1);
  int max_blocks_high = num_4x4_blocks_high_lookup[mbmi->sb_type];
  int max_blocks_wide = num_4x4_blocks_wide_lookup[mbmi->sb_type];
  int ctx = txfm_partition_context(xd->above_txfm_context + (blk_col >> 1),
                                   xd->left_txfm_context + (blk_row >> 1),
                                   tx_size);
  TX_SIZE plane_tx_size = mbmi->inter_tx_size[tx_idx];

  if (xd->mb_to_bottom_edge < 0)
    max_blocks_high += xd->mb_to_bottom_edge >> 5;
  if (xd->mb_to_right_edge < 0)
    max_blocks_wide += xd->mb_to_right_edge >> 5;

  if (blk_row >= max_blocks_high || blk_col >= max_blocks_wide)
    return;

  if (tx_size == plane_tx_size) {
    ++counts->txfm_partition[ctx][0];
    mbmi->tx_size = tx_size;
    txfm_partition_update(xd->above_txfm_context + (blk_col >> 1),
                          xd->left_txfm_context + (blk_row >> 1), tx_size);
  } else {
    BLOCK_SIZE bsize = txsize_to_bsize[tx_size];
    int bh = num_4x4_blocks_high_lookup[bsize];
    int i;
    ++counts->txfm_partition[ctx][1];

    if (tx_size == TX_8X8) {
      mbmi->inter_tx_size[tx_idx] = TX_4X4;
      mbmi->tx_size = TX_4X4;
      txfm_partition_update(xd->above_txfm_context + (blk_col >> 1),
                            xd->left_txfm_context + (blk_row >> 1), TX_4X4);
      return;
    }

    for (i = 0; i < 4; ++i) {
      int offsetr = (i >> 1) * bh / 2;
      int offsetc = (i & 0x01) * bh / 2;
      update_txfm_count(xd, counts, tx_size - 1,
                        blk_row + offsetr, blk_col + offsetc);
    }
  }
}

static void tx_partition_count_update(VP10_COMMON *cm,
                                      MACROBLOCKD *xd,
                                      BLOCK_SIZE plane_bsize,
                                      int mi_row, int mi_col,
                                      FRAME_COUNTS *td_counts) {
  const int mi_width = num_4x4_blocks_wide_lookup[plane_bsize];
  const int mi_height = num_4x4_blocks_high_lookup[plane_bsize];
  TX_SIZE max_tx_size = max_txsize_lookup[plane_bsize];
  BLOCK_SIZE txb_size = txsize_to_bsize[max_tx_size];
  int bh = num_4x4_blocks_wide_lookup[txb_size];
  int idx, idy;

  xd->above_txfm_context = cm->above_txfm_context + mi_col;
  xd->left_txfm_context = xd->left_txfm_context_buffer + (mi_row & MI_MASK);

  for (idy = 0; idy < mi_height; idy += bh)
    for (idx = 0; idx < mi_width; idx += bh)
      update_txfm_count(xd, td_counts, max_tx_size, idy, idx);
}

static void set_txfm_context(MACROBLOCKD *xd, TX_SIZE tx_size,
                             int blk_row, int blk_col) {
  MB_MODE_INFO *mbmi = &xd->mi[0]->mbmi;
  int tx_idx = (blk_row >> 1) * 8 + (blk_col >> 1);
  int max_blocks_high = num_4x4_blocks_high_lookup[mbmi->sb_type];
  int max_blocks_wide = num_4x4_blocks_wide_lookup[mbmi->sb_type];
  TX_SIZE plane_tx_size = mbmi->inter_tx_size[tx_idx];

  if (xd->mb_to_bottom_edge < 0)
    max_blocks_high += xd->mb_to_bottom_edge >> 5;
  if (xd->mb_to_right_edge < 0)
    max_blocks_wide += xd->mb_to_right_edge >> 5;

  if (blk_row >= max_blocks_high || blk_col >= max_blocks_wide)
    return;

  if (tx_size == plane_tx_size) {
    mbmi->tx_size = tx_size;
    txfm_partition_update(xd->above_txfm_context + (blk_col >> 1),
                          xd->left_txfm_context + (blk_row >> 1), tx_size);

  } else {
    BLOCK_SIZE bsize = txsize_to_bsize[tx_size];
    int bsl = b_width_log2_lookup[bsize];
    int i;

    if (tx_size == TX_8X8) {
      mbmi->inter_tx_size[tx_idx] = TX_4X4;
      mbmi->tx_size = TX_4X4;
      txfm_partition_update(xd->above_txfm_context + (blk_col >> 1),
                            xd->left_txfm_context + (blk_row >> 1), TX_4X4);
      return;
    }

    assert(bsl > 0);
    --bsl;
    for (i = 0; i < 4; ++i) {
      int offsetr = (i >> 1) << bsl;
      int offsetc = (i & 0x01) << bsl;
      set_txfm_context(xd, tx_size - 1,
                       blk_row + offsetr, blk_col + offsetc);
    }
  }
}

static void tx_partition_set_contexts(VP10_COMMON *cm,
                                      MACROBLOCKD *xd,
                                      BLOCK_SIZE plane_bsize,
                                      int mi_row, int mi_col) {
  const int mi_width = num_4x4_blocks_wide_lookup[plane_bsize];
  const int mi_height = num_4x4_blocks_high_lookup[plane_bsize];
  TX_SIZE max_tx_size = max_txsize_lookup[plane_bsize];
  BLOCK_SIZE txb_size = txsize_to_bsize[max_tx_size];
  int bh = num_4x4_blocks_wide_lookup[txb_size];
  int idx, idy;

  xd->above_txfm_context = cm->above_txfm_context + mi_col;
  xd->left_txfm_context = xd->left_txfm_context_buffer + (mi_row & MI_MASK);

  for (idy = 0; idy < mi_height; idy += bh)
    for (idx = 0; idx < mi_width; idx += bh)
      set_txfm_context(xd, max_tx_size, idy, idx);
}
#endif

static void encode_superblock(VP10_COMP *cpi, ThreadData *td,
                              TOKENEXTRA **t, int output_enabled,
                              int mi_row, int mi_col, BLOCK_SIZE bsize,
                              PICK_MODE_CONTEXT *ctx) {
  VP10_COMMON *const cm = &cpi->common;
  MACROBLOCK *const x = &td->mb;
  MACROBLOCKD *const xd = &x->e_mbd;
  MODE_INFO **mi_8x8 = xd->mi;
  MODE_INFO *mi = mi_8x8[0];
  MB_MODE_INFO *mbmi = &mi->mbmi;
  const int seg_skip = segfeature_active(&cm->seg, mbmi->segment_id,
                                         SEG_LVL_SKIP);
  const int mis = cm->mi_stride;
  const int mi_width = num_8x8_blocks_wide_lookup[bsize];
  const int mi_height = num_8x8_blocks_high_lookup[bsize];

  x->skip_recode = !x->select_tx_size && mbmi->sb_type >= BLOCK_8X8 &&
#if CONFIG_OBMC
                   !(is_inter_block(mbmi) && is_obmc_allowed(mbmi)) &&
#endif  // CONFIG_OBMC
                   cpi->oxcf.aq_mode != COMPLEXITY_AQ &&
                   cpi->oxcf.aq_mode != CYCLIC_REFRESH_AQ &&
                   cpi->sf.allow_skip_recode;

  if (!x->skip_recode)
    memset(x->skip_txfm, 0, sizeof(x->skip_txfm));

  x->skip_optimize = ctx->is_coded;
  ctx->is_coded = 1;
  x->use_lp32x32fdct = cpi->sf.use_lp32x32fdct;

  if (!is_inter_block(mbmi)) {
    int plane;
    mbmi->skip = 1;
    for (plane = 0; plane < MAX_MB_PLANE; ++plane)
      vp10_encode_intra_block_plane(x, VPXMAX(bsize, BLOCK_8X8), plane);
    if (output_enabled)
      sum_intra_stats(td->counts, mi, xd->above_mi, xd->left_mi,
                      frame_is_intra_only(cm));

#if CONFIG_EXT_INTRA
    if (output_enabled && bsize >= BLOCK_8X8) {
      FRAME_COUNTS *counts = td->counts;
      if (mbmi->mode == DC_PRED)
        ++counts->ext_intra[0][mbmi->ext_intra_mode_info.use_ext_intra_mode[0]];
      if (mbmi->uv_mode == DC_PRED)
        ++counts->ext_intra[1][mbmi->ext_intra_mode_info.use_ext_intra_mode[1]];
      if (mbmi->mode != DC_PRED && mbmi->mode != TM_PRED) {
        int p_angle;
        const int intra_filter_ctx = vp10_get_pred_context_intra_interp(xd);
        p_angle = mode_to_angle_map[mbmi->mode] +
            mbmi->angle_delta[0] * ANGLE_STEP;
        if (pick_intra_filter(p_angle))
          ++counts->intra_filter[intra_filter_ctx][mbmi->intra_filter];
      }
    }
#endif  // CONFIG_EXT_INTRA

    if (bsize >= BLOCK_8X8 && output_enabled) {
      if (mbmi->palette_mode_info.palette_size[0] > 0) {
        mbmi->palette_mode_info.palette_first_color_idx[0] =
            xd->plane[0].color_index_map[0];
        // TODO(huisu): this increases the use of token buffer. Needs stretch
        // test to verify.
        vp10_tokenize_palette_sb(td, bsize, 0, t);
      }
    }
    vp10_tokenize_sb(cpi, td, t, !output_enabled, VPXMAX(bsize, BLOCK_8X8));
  } else {
    int ref;
    const int is_compound = has_second_ref(mbmi);
    set_ref_ptrs(cm, xd, mbmi->ref_frame[0], mbmi->ref_frame[1]);
    for (ref = 0; ref < 1 + is_compound; ++ref) {
      YV12_BUFFER_CONFIG *cfg = get_ref_frame_buffer(cpi,
                                                     mbmi->ref_frame[ref]);
      assert(cfg != NULL);
      vp10_setup_pre_planes(xd, ref, cfg, mi_row, mi_col,
                           &xd->block_refs[ref]->sf);
    }
    if (!(cpi->sf.reuse_inter_pred_sby && ctx->pred_pixel_ready) || seg_skip)
      vp10_build_inter_predictors_sby(xd, mi_row, mi_col,
                                      VPXMAX(bsize, BLOCK_8X8));

    vp10_build_inter_predictors_sbuv(xd, mi_row, mi_col,
                                     VPXMAX(bsize, BLOCK_8X8));

#if CONFIG_OBMC
    if (mbmi->obmc) {
#if CONFIG_VP9_HIGHBITDEPTH
      DECLARE_ALIGNED(16, uint8_t, tmp_buf1[2 * MAX_MB_PLANE * 64 * 64]);
      DECLARE_ALIGNED(16, uint8_t, tmp_buf2[2 * MAX_MB_PLANE * 64 * 64]);
#else
      DECLARE_ALIGNED(16, uint8_t, tmp_buf1[MAX_MB_PLANE * 64 * 64]);
      DECLARE_ALIGNED(16, uint8_t, tmp_buf2[MAX_MB_PLANE * 64 * 64]);
#endif  // CONFIG_VP9_HIGHBITDEPTH
      uint8_t *dst_buf1[MAX_MB_PLANE], *dst_buf2[MAX_MB_PLANE];
      int dst_stride1[MAX_MB_PLANE] = {64, 64, 64};
      int dst_stride2[MAX_MB_PLANE] = {64, 64, 64};

      assert(mbmi->sb_type >= BLOCK_8X8);

#if CONFIG_VP9_HIGHBITDEPTH
      if (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) {
        int len = sizeof(uint16_t);
        dst_buf1[0] = CONVERT_TO_BYTEPTR(tmp_buf1);
        dst_buf1[1] = CONVERT_TO_BYTEPTR(tmp_buf1 + 4096 * len);
        dst_buf1[2] = CONVERT_TO_BYTEPTR(tmp_buf1 + 8192 * len);
        dst_buf2[0] = CONVERT_TO_BYTEPTR(tmp_buf2);
        dst_buf2[1] = CONVERT_TO_BYTEPTR(tmp_buf2 + 4096 * len);
        dst_buf2[2] = CONVERT_TO_BYTEPTR(tmp_buf2 + 8192 * len);
      } else {
#endif  // CONFIG_VP9_HIGHBITDEPTH
      dst_buf1[0] = tmp_buf1;
      dst_buf1[1] = tmp_buf1 + 4096;
      dst_buf1[2] = tmp_buf1 + 8192;
      dst_buf2[0] = tmp_buf2;
      dst_buf2[1] = tmp_buf2 + 4096;
      dst_buf2[2] = tmp_buf2 + 8192;
#if CONFIG_VP9_HIGHBITDEPTH
      }
#endif  // CONFIG_VP9_HIGHBITDEPTH

      vp10_build_prediction_by_above_preds(cpi, xd, mi_row, mi_col, dst_buf1,
                                           dst_stride1);
      vp10_build_prediction_by_left_preds(cpi, xd, mi_row, mi_col, dst_buf2,
                                          dst_stride2);
      vp10_setup_dst_planes(xd->plane, get_frame_new_buffer(cm),
                            mi_row, mi_col);
      vp10_build_obmc_inter_prediction(cm, xd, mi_row, mi_col, 0, NULL, NULL,
                                       dst_buf1, dst_stride1,
                                       dst_buf2, dst_stride2);
    }

#endif  // CONFIG_OBMC

    vp10_encode_sb(x, VPXMAX(bsize, BLOCK_8X8));
#if CONFIG_VAR_TX
    vp10_tokenize_sb_inter(cpi, td, t, !output_enabled,
                           mi_row, mi_col, VPXMAX(bsize, BLOCK_8X8));
#else
    vp10_tokenize_sb(cpi, td, t, !output_enabled, VPXMAX(bsize, BLOCK_8X8));
#endif
  }

  if (output_enabled) {
    if (cm->tx_mode == TX_MODE_SELECT &&
        mbmi->sb_type >= BLOCK_8X8  &&
        !(is_inter_block(mbmi) && (mbmi->skip || seg_skip))) {
#if CONFIG_VAR_TX
      if (is_inter_block(mbmi))
        tx_partition_count_update(cm, xd, bsize, mi_row, mi_col, td->counts);
#endif
      ++get_tx_counts(max_txsize_lookup[bsize], get_tx_size_context(xd),
                      &td->counts->tx)[mbmi->tx_size];
    } else {
      int x, y;
      TX_SIZE tx_size;
      // The new intra coding scheme requires no change of transform size
      if (is_inter_block(&mi->mbmi))
        tx_size = VPXMIN(tx_mode_to_biggest_tx_size[cm->tx_mode],
                         max_txsize_lookup[bsize]);
      else
        tx_size = (bsize >= BLOCK_8X8) ? mbmi->tx_size : TX_4X4;

      for (y = 0; y < mi_height; y++)
        for (x = 0; x < mi_width; x++)
          if (mi_col + x < cm->mi_cols && mi_row + y < cm->mi_rows)
            mi_8x8[mis * y + x]->mbmi.tx_size = tx_size;
    }
    ++td->counts->tx.tx_totals[mbmi->tx_size];
    ++td->counts->tx.tx_totals[get_uv_tx_size(mbmi, &xd->plane[1])];
#if CONFIG_EXT_TX
    if (get_ext_tx_types(mbmi->tx_size, bsize, is_inter_block(mbmi)) > 1 &&
        cm->base_qindex > 0 && !mbmi->skip &&
        !segfeature_active(&cm->seg, mbmi->segment_id, SEG_LVL_SKIP)) {
      int eset = get_ext_tx_set(mbmi->tx_size, bsize,
                                is_inter_block(mbmi));
      if (eset > 0) {
        if (is_inter_block(mbmi)) {
          ++td->counts->inter_ext_tx[eset][mbmi->tx_size][mbmi->tx_type];
        } else {
          ++td->counts->intra_ext_tx[eset][mbmi->tx_size][mbmi->mode]
              [mbmi->tx_type];
        }
      }
    }
#else
    if (mbmi->tx_size < TX_32X32 &&
        cm->base_qindex > 0 && !mbmi->skip &&
        !segfeature_active(&cm->seg, mbmi->segment_id, SEG_LVL_SKIP)) {
      if (is_inter_block(mbmi)) {
        ++td->counts->inter_ext_tx[mbmi->tx_size][mbmi->tx_type];
      } else {
        ++td->counts->intra_ext_tx[mbmi->tx_size]
                                  [intra_mode_to_tx_type_context[mbmi->mode]]
                                  [mbmi->tx_type];
      }
    }
#endif  // CONFIG_EXT_TX
  }

#if CONFIG_VAR_TX
  if (cm->tx_mode == TX_MODE_SELECT && mbmi->sb_type >= BLOCK_8X8 &&
      is_inter_block(mbmi) && !(mbmi->skip || seg_skip)) {
    if (!output_enabled)
      tx_partition_set_contexts(cm, xd, bsize, mi_row, mi_col);
  } else {
    TX_SIZE tx_size;
    // The new intra coding scheme requires no change of transform size
    if (is_inter_block(mbmi))
      tx_size = VPXMIN(tx_mode_to_biggest_tx_size[cm->tx_mode],
                       max_txsize_lookup[bsize]);
    else
      tx_size = (bsize >= BLOCK_8X8) ? mbmi->tx_size : TX_4X4;
    mbmi->tx_size = tx_size;
    set_txfm_ctx(xd->left_txfm_context, tx_size, xd->n8_h);
    set_txfm_ctx(xd->above_txfm_context, tx_size, xd->n8_w);
  }
#endif
}

#if CONFIG_SUPERTX
static int check_intra_b(PICK_MODE_CONTEXT *ctx) {
  if (!is_inter_mode((&ctx->mic)->mbmi.mode))
    return 1;
#if CONFIG_EXT_INTER
  if (ctx->mic.mbmi.ref_frame[1] == INTRA_FRAME)
    return 1;
#endif  // CONFIG_EXT_INTER
  return 0;
}

static int check_intra_sb(VP10_COMP *cpi, const TileInfo *const tile,
                          int mi_row, int mi_col, BLOCK_SIZE bsize,
                          PC_TREE *pc_tree) {
  VP10_COMMON *const cm = &cpi->common;

  const int bsl = b_width_log2_lookup[bsize], hbs = (1 << bsl) / 4;
  PARTITION_TYPE partition;
  BLOCK_SIZE subsize = bsize;

  if (mi_row >= cm->mi_rows || mi_col >= cm->mi_cols)
    return 1;

  if (bsize >= BLOCK_8X8)
    subsize = get_subsize(bsize, pc_tree->partitioning);
  else
    subsize = BLOCK_4X4;

  partition = partition_lookup[bsl][subsize];

  switch (partition) {
    case PARTITION_NONE:
      return check_intra_b(&pc_tree->none);
      break;
    case PARTITION_VERT:
      if (check_intra_b(&pc_tree->vertical[0]))
        return 1;
      if (mi_col + hbs < cm->mi_cols && bsize > BLOCK_8X8) {
        if (check_intra_b(&pc_tree->vertical[1]))
          return 1;
      }
      break;
    case PARTITION_HORZ:
      if (check_intra_b(&pc_tree->horizontal[0]))
        return 1;
      if (mi_row + hbs < cm->mi_rows && bsize > BLOCK_8X8) {
        if (check_intra_b(&pc_tree->horizontal[1]))
          return 1;
      }
      break;
    case PARTITION_SPLIT:
      if (bsize == BLOCK_8X8) {
        if (check_intra_b(pc_tree->leaf_split[0]))
          return 1;
      } else {
        if (check_intra_sb(cpi, tile, mi_row, mi_col, subsize,
                           pc_tree->split[0]))
          return 1;
        if (check_intra_sb(cpi, tile, mi_row, mi_col + hbs, subsize,
                           pc_tree->split[1]))
          return 1;
        if (check_intra_sb(cpi, tile, mi_row + hbs, mi_col, subsize,
                           pc_tree->split[2]))
          return 1;
        if (check_intra_sb(cpi, tile, mi_row + hbs, mi_col + hbs, subsize,
                           pc_tree->split[3]))
          return 1;
      }
      break;
    default:
      assert(0);
  }
  return 0;
}

static int check_supertx_b(TX_SIZE supertx_size, PICK_MODE_CONTEXT *ctx) {
  return ctx->mic.mbmi.tx_size == supertx_size;
}

static int check_supertx_sb(BLOCK_SIZE bsize, TX_SIZE supertx_size,
                            PC_TREE *pc_tree) {
  PARTITION_TYPE partition;
  BLOCK_SIZE subsize;

  partition = pc_tree->partitioning;
  subsize = get_subsize(bsize, partition);
  switch (partition) {
    case PARTITION_NONE:
      return check_supertx_b(supertx_size, &pc_tree->none);
    case PARTITION_VERT:
      return check_supertx_b(supertx_size, &pc_tree->vertical[0]);
    case PARTITION_HORZ:
      return check_supertx_b(supertx_size, &pc_tree->horizontal[0]);
    case PARTITION_SPLIT:
      if (bsize == BLOCK_8X8)
        return check_supertx_b(supertx_size, pc_tree->leaf_split[0]);
      else
        return check_supertx_sb(subsize, supertx_size, pc_tree->split[0]);
    default:
      assert(0);
      return 0;
  }
}

static void predict_superblock(VP10_COMP *cpi, ThreadData *td,
                               int mi_row_pred, int mi_col_pred,
                               BLOCK_SIZE bsize_pred, int b_sub8x8, int block) {
  // Used in supertx
  // (mi_row_ori, mi_col_ori): location for mv
  // (mi_row_pred, mi_col_pred, bsize_pred): region to predict
  VP10_COMMON *const cm = &cpi->common;
  MACROBLOCK *const x = &td->mb;
  MACROBLOCKD *const xd = &x->e_mbd;
  MODE_INFO *mi_8x8 = xd->mi[0];
  MODE_INFO *mi = mi_8x8;
  MB_MODE_INFO *mbmi = &mi->mbmi;
  int ref;
  const int is_compound = has_second_ref(mbmi);

  set_ref_ptrs(cm, xd, mbmi->ref_frame[0], mbmi->ref_frame[1]);

  for (ref = 0; ref < 1 + is_compound; ++ref) {
    YV12_BUFFER_CONFIG *cfg = get_ref_frame_buffer(cpi,
                                                   mbmi->ref_frame[ref]);
    vp10_setup_pre_planes(xd, ref, cfg, mi_row_pred, mi_col_pred,
                         &xd->block_refs[ref]->sf);
  }

  if (!b_sub8x8)
    vp10_build_inter_predictors_sb(xd, mi_row_pred, mi_col_pred, bsize_pred);
  else
    vp10_build_inter_predictors_sb_sub8x8(xd, mi_row_pred, mi_col_pred,
                                          bsize_pred, block);
}

static void predict_b_extend(VP10_COMP *cpi, ThreadData *td,
                             const TileInfo *const tile,
                             int block,
                             int mi_row_ori, int mi_col_ori,
                             int mi_row_pred, int mi_col_pred,
                             int mi_row_top, int mi_col_top,
                             uint8_t * dst_buf[3], int dst_stride[3],
                             BLOCK_SIZE bsize_ori, BLOCK_SIZE bsize_top,
                             BLOCK_SIZE bsize_pred, int output_enabled,
                             int b_sub8x8, int bextend) {
  // Used in supertx
  // (mi_row_ori, mi_col_ori): location for mv
  // (mi_row_pred, mi_col_pred, bsize_pred): region to predict
  // (mi_row_top, mi_col_top, bsize_top): region of the top partition size
  // block: sub location of sub8x8 blocks
  // b_sub8x8: 1: ori is sub8x8; 0: ori is not sub8x8
  // bextend: 1: region to predict is an extension of ori; 0: not

  MACROBLOCK *const x = &td->mb;
  VP10_COMMON *const cm = &cpi->common;
  MACROBLOCKD *const xd = &x->e_mbd;
  int r = (mi_row_pred - mi_row_top) * MI_SIZE;
  int c = (mi_col_pred - mi_col_top) * MI_SIZE;
  const int mi_width_top = num_8x8_blocks_wide_lookup[bsize_top];
  const int mi_height_top = num_8x8_blocks_high_lookup[bsize_top];

  if (mi_row_pred < mi_row_top || mi_col_pred < mi_col_top ||
      mi_row_pred >= mi_row_top + mi_height_top ||
      mi_col_pred >= mi_col_top + mi_width_top ||
      mi_row_pred >= cm->mi_rows || mi_col_pred >= cm->mi_cols)
    return;

  set_offsets_extend(cpi, td, tile, mi_row_pred, mi_col_pred,
                     mi_row_ori, mi_col_ori, bsize_pred, bsize_ori);
  xd->plane[0].dst.stride = dst_stride[0];
  xd->plane[1].dst.stride = dst_stride[1];
  xd->plane[2].dst.stride = dst_stride[2];
  xd->plane[0].dst.buf = dst_buf[0] +
                         (r >> xd->plane[0].subsampling_y) * dst_stride[0] +
                         (c >> xd->plane[0].subsampling_x);
  xd->plane[1].dst.buf = dst_buf[1] +
                         (r >> xd->plane[1].subsampling_y) * dst_stride[1] +
                         (c >> xd->plane[1].subsampling_x);
  xd->plane[2].dst.buf = dst_buf[2] +
                         (r >> xd->plane[2].subsampling_y) * dst_stride[2] +
                         (c >> xd->plane[2].subsampling_x);

  predict_superblock(cpi, td,
                     mi_row_pred, mi_col_pred, bsize_pred,
                     b_sub8x8, block);

  if (output_enabled && !bextend)
    update_stats(&cpi->common, td, 1);
}

static void extend_dir(VP10_COMP *cpi, ThreadData *td,
                       const TileInfo *const tile,
                       int block, BLOCK_SIZE bsize, BLOCK_SIZE top_bsize,
                       int mi_row, int mi_col,
                       int mi_row_top, int mi_col_top,
                       int output_enabled,
                       uint8_t * dst_buf[3], int dst_stride[3], int dir) {
  // dir: 0-lower, 1-upper, 2-left, 3-right
  //      4-lowerleft, 5-upperleft, 6-lowerright, 7-upperright
  MACROBLOCKD *xd = &td->mb.e_mbd;
  const int mi_width = num_8x8_blocks_wide_lookup[bsize];
  const int mi_height = num_8x8_blocks_high_lookup[bsize];
  int xss = xd->plane[1].subsampling_x;
  int yss = xd->plane[1].subsampling_y;
  int b_sub8x8 = (bsize < BLOCK_8X8) ? 1 : 0;

  BLOCK_SIZE extend_bsize;
  int unit, mi_row_pred, mi_col_pred;

  if (dir == 0 || dir == 1) {  // lower and upper
    extend_bsize = (mi_width == 1 || bsize < BLOCK_8X8 || xss < yss) ?
                   BLOCK_8X8 : BLOCK_16X8;
    unit = num_8x8_blocks_wide_lookup[extend_bsize];
    mi_row_pred = mi_row + ((dir == 0) ? mi_height : -1);
    mi_col_pred = mi_col;

    predict_b_extend(cpi, td, tile, block, mi_row, mi_col,
                     mi_row_pred, mi_col_pred,
                     mi_row_top, mi_col_top, dst_buf, dst_stride,
                     bsize, top_bsize, extend_bsize,
                     output_enabled, b_sub8x8, 1);

    if (mi_width > unit) {
      int i;
      for (i = 0; i < mi_width/unit - 1; i++) {
        mi_col_pred += unit;
        predict_b_extend(cpi, td, tile, block, mi_row, mi_col,
                         mi_row_pred, mi_col_pred, mi_row_top, mi_col_top,
                         dst_buf, dst_stride, bsize, top_bsize, extend_bsize,
                         output_enabled, b_sub8x8, 1);
      }
    }
  } else if (dir == 2 || dir == 3) {  // left and right
    extend_bsize = (mi_height == 1 || bsize < BLOCK_8X8 || yss < xss) ?
                   BLOCK_8X8 : BLOCK_8X16;
    unit = num_8x8_blocks_high_lookup[extend_bsize];
    mi_row_pred = mi_row;
    mi_col_pred = mi_col + ((dir == 3) ? mi_width : -1);

    predict_b_extend(cpi, td, tile, block, mi_row, mi_col,
                     mi_row_pred, mi_col_pred, mi_row_top, mi_col_top,
                     dst_buf, dst_stride, bsize, top_bsize, extend_bsize,
                     output_enabled, b_sub8x8, 1);

    if (mi_height > unit) {
      int i;
      for (i = 0; i < mi_height/unit - 1; i++) {
        mi_row_pred += unit;
        predict_b_extend(cpi, td, tile, block, mi_row, mi_col,
                         mi_row_pred, mi_col_pred, mi_row_top, mi_col_top,
                         dst_buf, dst_stride, bsize, top_bsize, extend_bsize,
                         output_enabled, b_sub8x8, 1);
      }
    }
  } else {
    extend_bsize = BLOCK_8X8;
    mi_row_pred = mi_row + ((dir == 4 || dir == 6) ? mi_height : -1);
    mi_col_pred = mi_col + ((dir == 6 || dir == 7) ? mi_width : -1);

    predict_b_extend(cpi, td, tile, block, mi_row, mi_col,
                     mi_row_pred, mi_col_pred, mi_row_top, mi_col_top,
                     dst_buf, dst_stride, bsize, top_bsize, extend_bsize,
                     output_enabled, b_sub8x8, 1);
  }
}

static void extend_all(VP10_COMP *cpi, ThreadData *td,
                       const TileInfo *const tile,
                       int block,
                       BLOCK_SIZE bsize, BLOCK_SIZE top_bsize,
                       int mi_row, int mi_col,
                       int mi_row_top, int mi_col_top,
                       int output_enabled,
                       uint8_t * dst_buf[3], int dst_stride[3]) {
  assert(block >= 0 && block < 4);
  extend_dir(cpi, td, tile, block, bsize, top_bsize, mi_row, mi_col,
             mi_row_top, mi_col_top, output_enabled, dst_buf, dst_stride, 0);
  extend_dir(cpi, td, tile, block, bsize, top_bsize, mi_row, mi_col,
             mi_row_top, mi_col_top, output_enabled, dst_buf, dst_stride, 1);
  extend_dir(cpi, td, tile, block, bsize, top_bsize, mi_row, mi_col,
             mi_row_top, mi_col_top, output_enabled, dst_buf, dst_stride, 2);
  extend_dir(cpi, td, tile, block, bsize, top_bsize, mi_row, mi_col,
             mi_row_top, mi_col_top, output_enabled, dst_buf, dst_stride, 3);
  extend_dir(cpi, td, tile, block, bsize, top_bsize, mi_row, mi_col,
             mi_row_top, mi_col_top, output_enabled, dst_buf, dst_stride, 4);
  extend_dir(cpi, td, tile, block, bsize, top_bsize, mi_row, mi_col,
             mi_row_top, mi_col_top, output_enabled, dst_buf, dst_stride, 5);
  extend_dir(cpi, td, tile, block, bsize, top_bsize, mi_row, mi_col,
             mi_row_top, mi_col_top, output_enabled, dst_buf, dst_stride, 6);
  extend_dir(cpi, td, tile, block, bsize, top_bsize, mi_row, mi_col,
             mi_row_top, mi_col_top, output_enabled, dst_buf, dst_stride, 7);
}


// This function generates prediction for multiple blocks, between which
// discontinuity around boundary is reduced by smoothing masks. The basic
// smoothing mask is a soft step function along horz/vert direction. In more
// complicated case when a block is split into 4 subblocks, the basic mask is
// first applied to neighboring subblocks (2 pairs) in horizontal direction and
// then applied to the 2 masked prediction mentioned above in vertical direction
// If the block is split into more than one level, at every stage, masked
// prediction is stored in dst_buf[] passed from higher level.
static void predict_sb_complex(VP10_COMP *cpi, ThreadData *td,
                               const TileInfo *const tile,
                               int mi_row, int mi_col,
                               int mi_row_top, int mi_col_top,
                               int output_enabled, BLOCK_SIZE bsize,
                               BLOCK_SIZE top_bsize,
                               uint8_t *dst_buf[3], int dst_stride[3],
                               PC_TREE *pc_tree) {
  VP10_COMMON *const cm = &cpi->common;
  MACROBLOCK *const x = &td->mb;
  MACROBLOCKD *const xd = &x->e_mbd;

  const int bsl = b_width_log2_lookup[bsize], hbs = (1 << bsl) / 4;
  PARTITION_TYPE partition;
  BLOCK_SIZE subsize;

  int i, ctx;
  uint8_t *dst_buf1[3], *dst_buf2[3], *dst_buf3[3];
  DECLARE_ALIGNED(16, uint8_t,
                  tmp_buf1[MAX_MB_PLANE * MAXTXLEN * MAXTXLEN * 2]);
  DECLARE_ALIGNED(16, uint8_t,
                  tmp_buf2[MAX_MB_PLANE * MAXTXLEN * MAXTXLEN * 2]);
  DECLARE_ALIGNED(16, uint8_t,
                  tmp_buf3[MAX_MB_PLANE * MAXTXLEN * MAXTXLEN * 2]);
  int dst_stride1[3] = {MAXTXLEN, MAXTXLEN, MAXTXLEN};
  int dst_stride2[3] = {MAXTXLEN, MAXTXLEN, MAXTXLEN};
  int dst_stride3[3] = {MAXTXLEN, MAXTXLEN, MAXTXLEN};
#if CONFIG_VP9_HIGHBITDEPTH
  if (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) {
    int len = sizeof(uint16_t);
    dst_buf1[0] = CONVERT_TO_BYTEPTR(tmp_buf1);
    dst_buf1[1] = CONVERT_TO_BYTEPTR(tmp_buf1 + MAXTXLEN * MAXTXLEN * len);
    dst_buf1[2] = CONVERT_TO_BYTEPTR(tmp_buf1 + 2 * MAXTXLEN * MAXTXLEN * len);
    dst_buf2[0] = CONVERT_TO_BYTEPTR(tmp_buf2);
    dst_buf2[1] = CONVERT_TO_BYTEPTR(tmp_buf2 + MAXTXLEN * MAXTXLEN * len);
    dst_buf2[2] = CONVERT_TO_BYTEPTR(tmp_buf2 + 2 * MAXTXLEN * MAXTXLEN * len);
    dst_buf3[0] = CONVERT_TO_BYTEPTR(tmp_buf3);
    dst_buf3[1] = CONVERT_TO_BYTEPTR(tmp_buf3 + MAXTXLEN * MAXTXLEN * len);
    dst_buf3[2] = CONVERT_TO_BYTEPTR(tmp_buf3 + 2 * MAXTXLEN * MAXTXLEN * len);
  } else {
#endif  // CONFIG_VP9_HIGHBITDEPTH
    dst_buf1[0] = tmp_buf1;
    dst_buf1[1] = tmp_buf1 + MAXTXLEN * MAXTXLEN;
    dst_buf1[2] = tmp_buf1 + 2 * MAXTXLEN * MAXTXLEN;
    dst_buf2[0] = tmp_buf2;
    dst_buf2[1] = tmp_buf2 + MAXTXLEN * MAXTXLEN;
    dst_buf2[2] = tmp_buf2 + 2 * MAXTXLEN * MAXTXLEN;
    dst_buf3[0] = tmp_buf3;
    dst_buf3[1] = tmp_buf3 + MAXTXLEN * MAXTXLEN;
    dst_buf3[2] = tmp_buf3 + 2 * MAXTXLEN * MAXTXLEN;
#if CONFIG_VP9_HIGHBITDEPTH
  }
#endif  // CONFIG_VP9_HIGHBITDEPTH

  if (mi_row >= cm->mi_rows || mi_col >= cm->mi_cols)
    return;

  if (bsize >= BLOCK_8X8) {
    ctx = partition_plane_context(xd, mi_row, mi_col, bsize);
    subsize = get_subsize(bsize, pc_tree->partitioning);
  } else {
    ctx = 0;
    subsize = BLOCK_4X4;
  }
  partition = partition_lookup[bsl][subsize];
  if (output_enabled && bsize != BLOCK_4X4 && bsize < top_bsize)
      cm->counts.partition[ctx][partition]++;

  for (i = 0; i < MAX_MB_PLANE; i++) {
    xd->plane[i].dst.buf = dst_buf[i];
    xd->plane[i].dst.stride = dst_stride[i];
  }

  switch (partition) {
    case PARTITION_NONE:
      assert(bsize < top_bsize);
      predict_b_extend(cpi, td, tile, 0, mi_row, mi_col, mi_row, mi_col,
                       mi_row_top, mi_col_top, dst_buf, dst_stride,
                       bsize, top_bsize, bsize, output_enabled, 0, 0);
      extend_all(cpi, td, tile, 0, bsize, top_bsize, mi_row, mi_col,
                 mi_row_top, mi_col_top, output_enabled, dst_buf, dst_stride);
      break;
    case PARTITION_HORZ:
      if (bsize == BLOCK_8X8) {
        // Fisrt half
        predict_b_extend(cpi, td, tile, 0, mi_row, mi_col, mi_row, mi_col,
                         mi_row_top, mi_col_top, dst_buf, dst_stride,
                         subsize, top_bsize, BLOCK_8X8, output_enabled, 1, 0);
        if (bsize < top_bsize)
          extend_all(cpi, td, tile, 0, subsize, top_bsize, mi_row, mi_col,
                     mi_row_top, mi_col_top, output_enabled,
                     dst_buf, dst_stride);

        // Second half
        predict_b_extend(cpi, td, tile, 2, mi_row, mi_col, mi_row, mi_col,
                         mi_row_top, mi_col_top, dst_buf1, dst_stride1,
                         subsize, top_bsize, BLOCK_8X8, output_enabled, 1, 1);
        if (bsize < top_bsize)
          extend_all(cpi, td, tile, 2, subsize, top_bsize, mi_row, mi_col,
                     mi_row_top, mi_col_top, output_enabled,
                     dst_buf1, dst_stride1);

        // Smooth
        xd->plane[0].dst.buf = dst_buf[0];
        xd->plane[0].dst.stride = dst_stride[0];
        vp10_build_masked_inter_predictor_complex(xd,
                                                  dst_buf[0], dst_stride[0],
                                                  dst_buf1[0], dst_stride1[0],
                                                  &xd->plane[0],
                                                  mi_row, mi_col,
                                                  mi_row_top, mi_col_top,
                                                  bsize, top_bsize,
                                                  PARTITION_HORZ, 0);
      }  else {
        // First half
        predict_b_extend(cpi, td, tile, 0, mi_row, mi_col, mi_row, mi_col,
                         mi_row_top, mi_col_top, dst_buf, dst_stride,
                         subsize, top_bsize, subsize, output_enabled, 0, 0);
        if (bsize < top_bsize)
          extend_all(cpi, td, tile, 0, subsize, top_bsize, mi_row, mi_col,
                     mi_row_top, mi_col_top, output_enabled,
                     dst_buf, dst_stride);
        else
          extend_dir(cpi, td, tile, 0, subsize, top_bsize, mi_row, mi_col,
                     mi_row_top, mi_col_top, output_enabled,
                     dst_buf, dst_stride, 0);

        if (mi_row + hbs < cm->mi_rows) {
          // Second half
          predict_b_extend(cpi, td, tile, 0, mi_row + hbs, mi_col,
                           mi_row + hbs, mi_col, mi_row_top, mi_col_top,
                           dst_buf1, dst_stride1, subsize, top_bsize, subsize,
                           output_enabled, 0, 0);
          if (bsize < top_bsize)
            extend_all(cpi, td, tile, 0, subsize, top_bsize, mi_row + hbs,
                       mi_col, mi_row_top, mi_col_top, output_enabled,
                       dst_buf1, dst_stride1);
          else
            extend_dir(cpi, td, tile, 0, subsize, top_bsize, mi_row + hbs,
                       mi_col, mi_row_top, mi_col_top, output_enabled,
                       dst_buf1, dst_stride1, 1);

          // Smooth
          for (i = 0; i < MAX_MB_PLANE; i++) {
            xd->plane[i].dst.buf = dst_buf[i];
            xd->plane[i].dst.stride = dst_stride[i];
            vp10_build_masked_inter_predictor_complex(
                xd, dst_buf[i], dst_stride[i], dst_buf1[i], dst_stride1[i],
                &xd->plane[i], mi_row, mi_col, mi_row_top, mi_col_top,
                bsize, top_bsize, PARTITION_HORZ, i);
          }
        }
      }
      break;
    case PARTITION_VERT:
      if (bsize == BLOCK_8X8) {
        // First half
        predict_b_extend(cpi, td, tile, 0, mi_row, mi_col, mi_row, mi_col,
                         mi_row_top, mi_col_top, dst_buf, dst_stride,
                         subsize, top_bsize, BLOCK_8X8, output_enabled, 1, 0);
        if (bsize < top_bsize)
          extend_all(cpi, td, tile, 0, subsize, top_bsize, mi_row, mi_col,
                     mi_row_top, mi_col_top, output_enabled,
                     dst_buf, dst_stride);

        // Second half
        predict_b_extend(cpi, td, tile, 1, mi_row, mi_col, mi_row, mi_col,
                         mi_row_top, mi_col_top, dst_buf1, dst_stride1,
                         subsize, top_bsize, BLOCK_8X8, output_enabled, 1, 1);
        if (bsize < top_bsize)
          extend_all(cpi, td, tile, 1, subsize, top_bsize, mi_row, mi_col,
                     mi_row_top, mi_col_top, output_enabled,
                     dst_buf1, dst_stride1);

        // Smooth
        xd->plane[0].dst.buf = dst_buf[0];
        xd->plane[0].dst.stride = dst_stride[0];
        vp10_build_masked_inter_predictor_complex(xd,
                                                  dst_buf[0], dst_stride[0],
                                                  dst_buf1[0], dst_stride1[0],
                                                  &xd->plane[0],
                                                  mi_row, mi_col,
                                                  mi_row_top, mi_col_top,
                                                  bsize, top_bsize,
                                                  PARTITION_VERT, 0);
      } else {
        // bsize: not important, not useful
        predict_b_extend(cpi, td, tile, 0, mi_row, mi_col, mi_row, mi_col,
                         mi_row_top, mi_col_top, dst_buf, dst_stride,
                         subsize, top_bsize, subsize, output_enabled, 0, 0);
        if (bsize < top_bsize)
          extend_all(cpi, td, tile, 0, subsize, top_bsize, mi_row, mi_col,
                     mi_row_top, mi_col_top, output_enabled,
                     dst_buf, dst_stride);
        else
          extend_dir(cpi, td, tile, 0, subsize, top_bsize, mi_row, mi_col,
                     mi_row_top, mi_col_top, output_enabled,
                     dst_buf, dst_stride, 3);


        if (mi_col + hbs < cm->mi_cols) {
          predict_b_extend(cpi, td, tile, 0, mi_row, mi_col + hbs,
                           mi_row, mi_col + hbs, mi_row_top, mi_col_top,
                           dst_buf1, dst_stride1, subsize, top_bsize, subsize,
                           output_enabled, 0, 0);
          if (bsize < top_bsize)
            extend_all(cpi, td, tile, 0, subsize, top_bsize, mi_row,
                       mi_col + hbs, mi_row_top, mi_col_top, output_enabled,
                       dst_buf1, dst_stride1);
          else
            extend_dir(cpi, td, tile, 0, subsize, top_bsize, mi_row,
                       mi_col + hbs, mi_row_top, mi_col_top, output_enabled,
                       dst_buf1, dst_stride1, 2);

          for (i = 0; i < MAX_MB_PLANE; i++) {
            xd->plane[i].dst.buf = dst_buf[i];
            xd->plane[i].dst.stride = dst_stride[i];
            vp10_build_masked_inter_predictor_complex(
                xd, dst_buf[i], dst_stride[i], dst_buf1[i], dst_stride1[i],
                &xd->plane[i], mi_row, mi_col, mi_row_top, mi_col_top,
                bsize, top_bsize, PARTITION_VERT, i);
          }
        }
      }
      break;
    case PARTITION_SPLIT:
      if (bsize == BLOCK_8X8) {
        predict_b_extend(cpi, td, tile, 0, mi_row, mi_col, mi_row, mi_col,
                         mi_row_top, mi_col_top, dst_buf, dst_stride,
                         subsize, top_bsize, BLOCK_8X8, output_enabled, 1, 0);
        predict_b_extend(cpi, td, tile, 1, mi_row, mi_col, mi_row, mi_col,
                         mi_row_top, mi_col_top, dst_buf1, dst_stride1,
                         subsize, top_bsize, BLOCK_8X8, output_enabled, 1, 1);
        predict_b_extend(cpi, td, tile, 2, mi_row, mi_col, mi_row, mi_col,
                         mi_row_top, mi_col_top, dst_buf2, dst_stride2,
                         subsize, top_bsize, BLOCK_8X8, output_enabled, 1, 1);
        predict_b_extend(cpi, td, tile, 3, mi_row, mi_col, mi_row, mi_col,
                         mi_row_top, mi_col_top, dst_buf3, dst_stride3,
                         subsize, top_bsize, BLOCK_8X8, output_enabled, 1, 1);

        if (bsize < top_bsize) {
          extend_all(cpi, td, tile, 0, subsize, top_bsize, mi_row, mi_col,
                     mi_row_top, mi_col_top, output_enabled,
                     dst_buf, dst_stride);
          extend_all(cpi, td, tile, 1, subsize, top_bsize, mi_row, mi_col,
                     mi_row_top, mi_col_top, output_enabled,
                     dst_buf1, dst_stride1);
          extend_all(cpi, td, tile, 2, subsize, top_bsize, mi_row, mi_col,
                     mi_row_top, mi_col_top, output_enabled,
                     dst_buf2, dst_stride2);
          extend_all(cpi, td, tile, 3, subsize, top_bsize, mi_row, mi_col,
                     mi_row_top, mi_col_top, output_enabled,
                     dst_buf3, dst_stride3);
        }
      } else {
        predict_sb_complex(cpi, td, tile, mi_row, mi_col,
                           mi_row_top, mi_col_top, output_enabled, subsize,
                           top_bsize, dst_buf, dst_stride,
                           pc_tree->split[0]);
        if (mi_row < cm->mi_rows && mi_col + hbs < cm->mi_cols)
          predict_sb_complex(cpi, td, tile, mi_row, mi_col + hbs,
                             mi_row_top, mi_col_top, output_enabled, subsize,
                             top_bsize, dst_buf1, dst_stride1,
                             pc_tree->split[1]);
        if (mi_row + hbs < cm->mi_rows && mi_col < cm->mi_cols)
          predict_sb_complex(cpi, td, tile, mi_row + hbs, mi_col,
                             mi_row_top, mi_col_top, output_enabled, subsize,
                             top_bsize, dst_buf2, dst_stride2,
                             pc_tree->split[2]);
        if (mi_row + hbs < cm->mi_rows && mi_col + hbs < cm->mi_cols)
          predict_sb_complex(cpi, td, tile, mi_row + hbs, mi_col + hbs,
                             mi_row_top, mi_col_top, output_enabled, subsize,
                             top_bsize, dst_buf3, dst_stride3,
                             pc_tree->split[3]);
      }
        for (i = 0; i < MAX_MB_PLANE; i++) {
          if (bsize == BLOCK_8X8 && i != 0)
            continue;  // Skip <4x4 chroma smoothing
          if (mi_row < cm->mi_rows && mi_col + hbs < cm->mi_cols) {
            vp10_build_masked_inter_predictor_complex(xd,
                                                      dst_buf[i],
                                                      dst_stride[i],
                                                      dst_buf1[i],
                                                      dst_stride1[i],
                                                      &xd->plane[i],
                                                      mi_row, mi_col,
                                                      mi_row_top, mi_col_top,
                                                      bsize, top_bsize,
                                                      PARTITION_VERT, i);
            if (mi_row + hbs < cm->mi_rows) {
              vp10_build_masked_inter_predictor_complex(xd,
                                                        dst_buf2[i],
                                                        dst_stride2[i],
                                                        dst_buf3[i],
                                                        dst_stride3[i],
                                                        &xd->plane[i],
                                                        mi_row, mi_col,
                                                        mi_row_top, mi_col_top,
                                                        bsize, top_bsize,
                                                        PARTITION_VERT, i);
              vp10_build_masked_inter_predictor_complex(xd,
                                                        dst_buf[i],
                                                        dst_stride[i],
                                                       dst_buf2[i],
                                                       dst_stride2[i],
                                                       &xd->plane[i],
                                                       mi_row, mi_col,
                                                       mi_row_top, mi_col_top,
                                                       bsize, top_bsize,
                                                       PARTITION_HORZ, i);
            }
          } else if (mi_row + hbs < cm->mi_rows && mi_col < cm->mi_cols) {
            vp10_build_masked_inter_predictor_complex(xd,
                                                      dst_buf[i],
                                                      dst_stride[i],
                                                      dst_buf2[i],
                                                      dst_stride2[i],
                                                      &xd->plane[i],
                                                      mi_row, mi_col,
                                                      mi_row_top, mi_col_top,
                                                      bsize, top_bsize,
                                                      PARTITION_HORZ, i);
          }
        }
        break;
    default:
        assert(0);
  }


  if (bsize < top_bsize && (partition != PARTITION_SPLIT || bsize == BLOCK_8X8))
    update_partition_context(xd, mi_row, mi_col, subsize, bsize);
}

static void rd_supertx_sb(VP10_COMP *cpi, ThreadData *td,
                          const TileInfo *const tile,
                          int mi_row, int mi_col, BLOCK_SIZE bsize,
                          int *tmp_rate, int64_t *tmp_dist,
                          TX_TYPE *best_tx,
                          PC_TREE *pc_tree) {
  VP10_COMMON *const cm = &cpi->common;
  MACROBLOCK *const x = &td->mb;
  MACROBLOCKD *const xd = &x->e_mbd;
  int plane, pnskip, skippable, skippable_uv, rate_uv, this_rate,
      base_rate = *tmp_rate;
  int64_t sse, pnsse, sse_uv, this_dist, dist_uv;
  uint8_t *dst_buf[3];
  int dst_stride[3];
  TX_SIZE tx_size;
  MB_MODE_INFO *mbmi;
  TX_TYPE tx_type, best_tx_nostx;
#if CONFIG_EXT_TX
  int ext_tx_set;
#endif  // CONFIG_EXT_TX
  int tmp_rate_tx = 0, skip_tx = 0;
  int64_t tmp_dist_tx = 0, rd_tx, bestrd_tx = INT64_MAX;
  uint8_t tmp_zcoeff_blk = 0;

  set_skip_context(xd, mi_row, mi_col);
  set_mode_info_offsets(cpi, x, xd, mi_row, mi_col);
  update_state_sb_supertx(cpi, td, tile, mi_row, mi_col, bsize,
                          0, pc_tree);
  vp10_setup_dst_planes(xd->plane, get_frame_new_buffer(cm),
                        mi_row, mi_col);
  for (plane = 0; plane < MAX_MB_PLANE; plane++) {
    dst_buf[plane] = xd->plane[plane].dst.buf;
    dst_stride[plane] = xd->plane[plane].dst.stride;
  }
  predict_sb_complex(cpi, td, tile, mi_row, mi_col, mi_row, mi_col,
                     0, bsize, bsize, dst_buf, dst_stride, pc_tree);

  set_offsets(cpi, tile, x, mi_row, mi_col, bsize);

  // These skip_txfm flags are previously set by the non-supertx RD search.
  // vp10_txfm_rd_in_plane_supertx calls block_rd_txfm, which checks these
  // to reuse distortion values from the RD estimation, so we reset these
  // flags here before evaluating RD for supertx coding.
  for (plane = 0 ; plane < MAX_MB_PLANE ; plane++)
    x->skip_txfm[plane << 2] = SKIP_TXFM_NONE;

  mbmi = &xd->mi[0]->mbmi;
  best_tx_nostx = mbmi->tx_type;

  *best_tx = DCT_DCT;

  // chroma
  skippable_uv = 1;
  rate_uv = 0;
  dist_uv = 0;
  sse_uv = 0;
  for (plane = 1; plane < MAX_MB_PLANE; ++plane) {
#if CONFIG_VAR_TX
    ENTROPY_CONTEXT ctxa[16], ctxl[16];
    const struct macroblockd_plane *const pd = &xd->plane[plane];
    int coeff_ctx = 1;

    this_rate = 0;
    this_dist = 0;
    pnsse = 0;
    pnskip = 1;

    tx_size = max_txsize_lookup[bsize];
    tx_size = get_uv_tx_size_impl(tx_size, bsize,
                                  cm->subsampling_x, cm->subsampling_y);
    vp10_get_entropy_contexts(bsize, tx_size, pd, ctxa, ctxl);
    coeff_ctx = combine_entropy_contexts(ctxa[0], ctxl[0]);

    vp10_subtract_plane(x, bsize, plane);
    vp10_tx_block_rd_b(cpi, x, tx_size,
                       0, 0, plane, 0,
                       get_plane_block_size(bsize, pd), coeff_ctx,
                       &this_rate, &this_dist, &pnsse, &pnskip);
#else
    tx_size = max_txsize_lookup[bsize];
    tx_size = get_uv_tx_size_impl(tx_size, bsize,
                                  cm->subsampling_x, cm->subsampling_y);
    vp10_subtract_plane(x, bsize, plane);
    vp10_txfm_rd_in_plane_supertx(x, cpi, &this_rate, &this_dist,
                                  &pnskip, &pnsse,
                                  INT64_MAX, plane, bsize, tx_size, 0);
#endif  // CONFIG_VAR_TX

    rate_uv += this_rate;
    dist_uv += this_dist;
    sse_uv += pnsse;
    skippable_uv &= pnskip;
  }

  // luma
  tx_size = max_txsize_lookup[bsize];
  vp10_subtract_plane(x, bsize, 0);
#if CONFIG_EXT_TX
  ext_tx_set = get_ext_tx_set(tx_size, bsize, 1);
#endif  // CONFIG_EXT_TX
  for (tx_type = DCT_DCT; tx_type < TX_TYPES; ++tx_type) {
#if CONFIG_VAR_TX
    ENTROPY_CONTEXT ctxa[16], ctxl[16];
    const struct macroblockd_plane *const pd = &xd->plane[0];
    int coeff_ctx = 1;
#endif  // CONFIG_VAR_TX
#if CONFIG_EXT_TX
    if (!ext_tx_used_inter[ext_tx_set][tx_type])
      continue;
    if (ext_tx_set == 1 &&
        tx_type >= DST_ADST && tx_type < IDTX && *best_tx == DCT_DCT) {
      tx_type = IDTX - 1;
      continue;
    }
#else
    if (tx_size >= TX_32X32 && tx_type != DCT_DCT)
      continue;
#endif  // CONFIG_EXT_TX
    mbmi->tx_type = tx_type;

#if CONFIG_VAR_TX
    this_rate = 0;
    this_dist = 0;
    pnsse = 0;
    pnskip = 1;

    vp10_get_entropy_contexts(bsize, tx_size, pd, ctxa, ctxl);
    coeff_ctx = combine_entropy_contexts(ctxa[0], ctxl[0]);
    vp10_tx_block_rd_b(cpi, x, tx_size,
                       0, 0, 0, 0,
                       bsize, coeff_ctx,
                       &this_rate, &this_dist, &pnsse, &pnskip);
#else
    vp10_txfm_rd_in_plane_supertx(x, cpi, &this_rate, &this_dist, &pnskip,
                                  &pnsse, INT64_MAX, 0, bsize, tx_size, 0);
#endif  // CONFIG_VAR_TX

#if CONFIG_EXT_TX
    if (get_ext_tx_types(tx_size, bsize, 1) > 1 &&
        !xd->lossless[xd->mi[0]->mbmi.segment_id] &&
        this_rate != INT_MAX) {
      if (ext_tx_set > 0)
        this_rate += cpi->inter_tx_type_costs[ext_tx_set]
            [mbmi->tx_size][mbmi->tx_type];
    }
#else
    if (tx_size < TX_32X32 &&
        !xd->lossless[xd->mi[0]->mbmi.segment_id] &&
        this_rate != INT_MAX) {
      this_rate += cpi->inter_tx_type_costs[tx_size][mbmi->tx_type];
    }
#endif  // CONFIG_EXT_TX
    *tmp_rate = rate_uv + this_rate;
    *tmp_dist = dist_uv + this_dist;
    sse = sse_uv + pnsse;
    skippable = skippable_uv && pnskip;
    if (skippable) {
      *tmp_rate = vp10_cost_bit(vp10_get_skip_prob(cm, xd), 1);
      x->skip = 1;
    } else {
      if (RDCOST(x->rdmult, x->rddiv, *tmp_rate, *tmp_dist)
          < RDCOST(x->rdmult, x->rddiv, 0, sse)) {
        *tmp_rate += vp10_cost_bit(vp10_get_skip_prob(cm, xd), 0);
        x->skip = 0;
      } else {
        *tmp_dist = sse;
        *tmp_rate = vp10_cost_bit(vp10_get_skip_prob(cm, xd), 1);
        x->skip = 1;
      }
    }
    *tmp_rate += base_rate;
    rd_tx = RDCOST(x->rdmult, x->rddiv, *tmp_rate, *tmp_dist);
    if (rd_tx < bestrd_tx * 0.99 || tx_type == DCT_DCT) {
      *best_tx = tx_type;
      bestrd_tx = rd_tx;
      tmp_rate_tx = *tmp_rate;
      tmp_dist_tx = *tmp_dist;
      skip_tx = x->skip;
      tmp_zcoeff_blk = x->zcoeff_blk[tx_size][0];
    }
  }
  x->zcoeff_blk[tx_size][0] = tmp_zcoeff_blk;
  *tmp_rate = tmp_rate_tx;
  *tmp_dist = tmp_dist_tx;
  x->skip = skip_tx;
#if CONFIG_VAR_TX
  for (plane = 0; plane < 1; ++plane)
    memset(x->blk_skip[plane], x->skip,
           sizeof(uint8_t) * pc_tree->none.num_4x4_blk);
#endif  // CONFIG_VAR_TX
  xd->mi[0]->mbmi.tx_type = best_tx_nostx;
}
#endif  // CONFIG_SUPERTX
