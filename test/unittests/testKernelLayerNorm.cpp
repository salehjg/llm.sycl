//
// Created by saleh on 17/05/24.
//

#include <gtest/gtest.h>
#include "testCommon.h"
#include "common/common.h"
#include "core/Tensor.h"
#include "kernels/Norm.h"
#include <sycl/sycl.hpp>

using namespace std;
using namespace llmsycl;

/*
 * float* out, float* mean, float* rstd,
                       const float* inp, const float* weight, const float* bias,
 * */
static void goldCpu(
        core::Tensor<float> &tnOut,
        size_t outOffset,
        core::Tensor<float> &tnMean,
        size_t meanOffset,
        core::Tensor<float> &tnRstd,
        size_t rstdOffset,
        const core::Tensor<float> &tnInp,
        size_t inpOffset,
        const core::Tensor<float> &tnWeight,
        size_t weightOffset,
        const core::Tensor<float> &tnBias,
        size_t biasOffset,
        int B, int T, int C) {

    auto accTnOut = tnOut.getHostBuffer() + outOffset;
    auto accTnMean = tnMean.getHostBuffer() + meanOffset;
    auto accTnRstd = tnRstd.getHostBuffer() + rstdOffset;
    auto accTnInp = tnInp.getHostBuffer() + inpOffset;
    auto accTnWeight = tnWeight.getHostBuffer() + weightOffset;
    auto accTnBias = tnBias.getHostBuffer() + biasOffset;

    float eps = 1e-5f;
    for (int b = 0; b < B; b++) {
        for (int t = 0; t < T; t++) {
            // calculate the mean
            float m = 0.0f;
            for (int i = 0; i < C; i++) {
                m += accTnInp[b * T * C + t * C + i];
            }
            m = m / C;
            // calculate the variance (without any bias correction)
            float v = 0.0f;
            for (int i = 0; i < C; i++) {
                float xshift = accTnInp[b * T * C + t * C + i] - m;
                v += xshift * xshift;
            }
            v = v / C;
            // calculate the rstd
            float s = 1.0f / std::sqrt(v + eps);
            // seek to the output position in out[b,t,:]

            for (int i = 0; i < C; i++) {
                float n = (s * (accTnInp[b * T * C + t * C + i] - m)); // normalized output
                float o = n * accTnWeight[i] + accTnBias[i]; // scale and shift it
                accTnOut[b * T * C + t * C + i] = o; // write
            }
            // cache the mean and rstd for the backward pass later
            accTnMean[b * T + t] = m;
            accTnRstd[b * T + t] = s;
        }
    }

}

static inline bool test() {
    sycl::queue q;
    prepareToTest(q);

    int B = 1;
    int T = 1024;
    int C = 768;

    // create tensors
    core::Tensor<float> tnOut(q, {(size_t) B * T * C});
    core::Tensor<float> tnOutGold(q, {(size_t) B * T * C});

    core::Tensor<float> tnMean(q, {(size_t) B * T});
    core::Tensor<float> tnMeanGold(q, {(size_t) B * T});

    core::Tensor<float> tnRstd(q, {(size_t) B * T});
    core::Tensor<float> tnRstdGold(q, {(size_t) B * T});

    core::Tensor<float> tnIn(q, {(size_t) B * T * C});
    core::Tensor<float> tnWeight(q, {(size_t) C});
    core::Tensor<float> tnBias(q, {(size_t) C});

    core::fillTensorWithRandomData(tnIn);
    core::fillTensorWith(tnWeight, 1);
    core::fillTensorWith(tnBias, 0);

    int blockSizes[] = {32, 64, 128, 256};
    goldCpu(
            tnOutGold, 0,
            tnMeanGold, 0,
            tnRstdGold, 0,
            tnIn, 0,
            tnWeight, 0,
            tnBias, 0,
            B, T, C
    );

    for (auto blockSize: blockSizes) {
        logger->info("Testing EncoderKernel with blockSize: {}", blockSize);

        kernels::Norm kernel(
                tnOut.getDeviceBuffer(),
                tnMean.getDeviceBuffer(),
                tnRstd.getDeviceBuffer(),
                tnIn.getDeviceBuffer(),
                tnWeight.getDeviceBuffer(),
                tnBias.getDeviceBuffer(),
                B, T, C
        );

        logger->info("BlockSize: {}, Device Time: {} ns", blockSize,
                     kernel.LaunchBlockingAndMeasureNanoSec(q, blockSize, {}));
        tnOut.syncBlockingD2H();
        tnMean.syncBlockingD2H();
        tnRstd.syncBlockingD2H();
        auto accTnOut = tnOut.getHostBuffer();
        auto accTnOutGold = tnOutGold.getHostBuffer();
        for (int i = 0; i < B * T * C; i++) {
            if (std::abs(accTnOut[i] - accTnOutGold[i]) > 1e-5) {
                logger->error("\tLayerNormKernel tnOut failed the verification test against the gold at index: {}, UUT: {}, GOLD: {}", i, accTnOut[i], accTnOutGold[i]);
                return false;
            }
        }


        auto accTnMean = tnMean.getHostBuffer();
        auto accTnMeanGold = tnMeanGold.getHostBuffer();
        for (int i = 0; i < B * T; i++) {
            if (std::abs(accTnMean[i] - accTnMeanGold[i]) > 1e-5) {
                logger->error("\tLayerNormKernel tnMean failed the verification test against the gold at index: {}", i);
                return false;
            }
        }

        auto accTnRstd = tnRstd.getHostBuffer();
        auto accTnRstdGold = tnRstdGold.getHostBuffer();
        for (int i = 0; i < B * T; i++) {
            if (std::abs(accTnRstd[i] - accTnRstdGold[i]) > 1e-5) {
                logger->error("\tLayerNormKernel tnRstd failed the verification test against the gold at index: {}", i);
                return false;
            }
        }
    }

    return true;
}

TEST(kernelLayerNorm, basic01) {
    EXPECT_TRUE(test());
}