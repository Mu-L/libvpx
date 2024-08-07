/*
 *  Copyright (c) 2014 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <tuple>

#include "gtest/gtest.h"
#include "test/acm_random.h"
#include "test/clear_system_state.h"
#include "test/register_state_check.h"
#include "test/util.h"

#include "vpx_scale/yv12config.h"
#include "vpx/vpx_integer.h"
#include "vp9/common/vp9_reconinter.h"
#include "vp9/encoder/vp9_context_tree.h"
#include "vp9/encoder/vp9_denoiser.h"
#include "vpx_config.h"

using libvpx_test::ACMRandom;

namespace {

const int kNumPixels = 64 * 64;

typedef int (*Vp9DenoiserFilterFunc)(const uint8_t *sig, int sig_stride,
                                     const uint8_t *mc_avg, int mc_avg_stride,
                                     uint8_t *avg, int avg_stride,
                                     int increase_denoising, BLOCK_SIZE bs,
                                     int motion_magnitude);
typedef std::tuple<Vp9DenoiserFilterFunc, BLOCK_SIZE> VP9DenoiserTestParam;

class VP9DenoiserTest
    : public ::testing::Test,
      public ::testing::WithParamInterface<VP9DenoiserTestParam> {
 public:
  ~VP9DenoiserTest() override = default;

  void SetUp() override { bs_ = GET_PARAM(1); }

  void TearDown() override { libvpx_test::ClearSystemState(); }

 protected:
  BLOCK_SIZE bs_;
};
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(VP9DenoiserTest);

TEST_P(VP9DenoiserTest, BitexactCheck) {
  ACMRandom rnd(ACMRandom::DeterministicSeed());
  const int count_test_block = 4000;

  // Allocate the space for input and output,
  // where sig_block is the block to be denoised,
  // mc_avg_block is the denoised reference block,
  // avg_block_c is the denoised result from C code,
  // avg_block_sse2 is the denoised result from SSE2 code.
  DECLARE_ALIGNED(16, uint8_t, sig_block[kNumPixels]);
  DECLARE_ALIGNED(16, uint8_t, mc_avg_block[kNumPixels]);
  DECLARE_ALIGNED(16, uint8_t, avg_block_c[kNumPixels]);
  DECLARE_ALIGNED(16, uint8_t, avg_block_sse2[kNumPixels]);

  for (int i = 0; i < count_test_block; ++i) {
    // Generate random motion magnitude, 20% of which exceed the threshold.
    const int motion_magnitude_random =
        rnd.Rand8() % static_cast<int>(MOTION_MAGNITUDE_THRESHOLD * 1.2);

    // Initialize a test block with random number in range [0, 255].
    for (int j = 0; j < kNumPixels; ++j) {
      int temp = 0;
      sig_block[j] = rnd.Rand8();
      // The pixels in mc_avg_block are generated by adding a random
      // number in range [-19, 19] to corresponding pixels in sig_block.
      temp =
          sig_block[j] + ((rnd.Rand8() % 2 == 0) ? -1 : 1) * (rnd.Rand8() % 20);
      // Clip.
      mc_avg_block[j] = (temp < 0) ? 0 : ((temp > 255) ? 255 : temp);
    }

    ASM_REGISTER_STATE_CHECK(vp9_denoiser_filter_c(sig_block, 64, mc_avg_block,
                                                   64, avg_block_c, 64, 0, bs_,
                                                   motion_magnitude_random));

    ASM_REGISTER_STATE_CHECK(GET_PARAM(0)(sig_block, 64, mc_avg_block, 64,
                                          avg_block_sse2, 64, 0, bs_,
                                          motion_magnitude_random));

    // Test bitexactness.
    for (int h = 0; h < (4 << b_height_log2_lookup[bs_]); ++h) {
      for (int w = 0; w < (4 << b_width_log2_lookup[bs_]); ++w) {
        EXPECT_EQ(avg_block_c[h * 64 + w], avg_block_sse2[h * 64 + w]);
      }
    }
  }
}

using std::make_tuple;

// Test for all block size.
#if HAVE_SSE2
INSTANTIATE_TEST_SUITE_P(
    SSE2, VP9DenoiserTest,
    ::testing::Values(make_tuple(&vp9_denoiser_filter_sse2, BLOCK_8X8),
                      make_tuple(&vp9_denoiser_filter_sse2, BLOCK_8X16),
                      make_tuple(&vp9_denoiser_filter_sse2, BLOCK_16X8),
                      make_tuple(&vp9_denoiser_filter_sse2, BLOCK_16X16),
                      make_tuple(&vp9_denoiser_filter_sse2, BLOCK_16X32),
                      make_tuple(&vp9_denoiser_filter_sse2, BLOCK_32X16),
                      make_tuple(&vp9_denoiser_filter_sse2, BLOCK_32X32),
                      make_tuple(&vp9_denoiser_filter_sse2, BLOCK_32X64),
                      make_tuple(&vp9_denoiser_filter_sse2, BLOCK_64X32),
                      make_tuple(&vp9_denoiser_filter_sse2, BLOCK_64X64)));
#endif  // HAVE_SSE2

#if HAVE_NEON
INSTANTIATE_TEST_SUITE_P(
    NEON, VP9DenoiserTest,
    ::testing::Values(make_tuple(&vp9_denoiser_filter_neon, BLOCK_8X8),
                      make_tuple(&vp9_denoiser_filter_neon, BLOCK_8X16),
                      make_tuple(&vp9_denoiser_filter_neon, BLOCK_16X8),
                      make_tuple(&vp9_denoiser_filter_neon, BLOCK_16X16),
                      make_tuple(&vp9_denoiser_filter_neon, BLOCK_16X32),
                      make_tuple(&vp9_denoiser_filter_neon, BLOCK_32X16),
                      make_tuple(&vp9_denoiser_filter_neon, BLOCK_32X32),
                      make_tuple(&vp9_denoiser_filter_neon, BLOCK_32X64),
                      make_tuple(&vp9_denoiser_filter_neon, BLOCK_64X32),
                      make_tuple(&vp9_denoiser_filter_neon, BLOCK_64X64)));
#endif
}  // namespace
