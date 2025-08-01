/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <faiss/gpu/test/TestUtils.h>
#include <faiss/utils/random.h>
#include <gtest/gtest.h>
#include <time.h>
#include <cmath>
#include <set>
#include <sstream>
#include <unordered_map>

namespace faiss {
namespace gpu {

inline float half2float(const unsigned short h) {
    unsigned int sign = ((static_cast<unsigned int>(h) >> 15U) & 1U);
    unsigned int exponent = ((static_cast<unsigned int>(h) >> 10U) & 0x1fU);
    unsigned int mantissa = ((static_cast<unsigned int>(h) & 0x3ffU) << 13U);
    float f;
    if (exponent == 0x1fU) { /* NaN or Inf */
        /* discard sign of a NaN */
        sign = ((mantissa != 0U) ? (sign >> 1U) : sign);
        mantissa = ((mantissa != 0U) ? 0x7fffffU : 0U);
        exponent = 0xffU;
    } else if (exponent == 0U) { /* Denorm or Zero */
        if (mantissa != 0U) {
            unsigned int msb;
            exponent = 0x71U;
            do {
                msb = (mantissa & 0x400000U);
                mantissa <<= 1U; /* normalize */
                --exponent;
            } while (msb == 0U);
            mantissa &= 0x7fffffU; /* 1.mantissa is implicit */
        }
    } else {
        exponent += 0x70U;
    }
    const unsigned int u = ((sign << 31U) | (exponent << 23U) | mantissa);
    std::memcpy(&f, &u, sizeof(u));
    return f;
}

unsigned short float2half(const float f) {
    unsigned int sign;
    unsigned int remainder;
    unsigned int x;
    unsigned int u;
    unsigned int result;
    (void)std::memcpy(&x, &f, sizeof(f));

    u = (x & 0x7fffffffU);
    sign = ((x >> 16U) & 0x8000U);
    // NaN/+Inf/-Inf
    if (u >= 0x7f800000U) {
        remainder = 0U;
        result = ((u == 0x7f800000U) ? (sign | 0x7c00U) : 0x7fffU);
    } else if (u > 0x477fefffU) { // Overflows
        remainder = 0x80000000U;
        result = (sign | 0x7bffU);
    } else if (u >= 0x38800000U) { // Normal numbers
        remainder = u << 19U;
        u -= 0x38000000U;
        result = (sign | (u >> 13U));
    } else if (u < 0x33000001U) { // +0/-0
        remainder = u;
        result = sign;
    } else { // Denormal numbers
        const unsigned int exponent = u >> 23U;
        const unsigned int shift = 0x7eU - exponent;
        unsigned int mantissa = (u & 0x7fffffU);
        mantissa |= 0x800000U;
        remainder = mantissa << (32U - shift);
        result = (sign | (mantissa >> shift));
        result &= 0x0000FFFFU;
    }

    if ((remainder > 0x80000000U) ||
        ((remainder == 0x80000000U) && ((result & 0x1U) != 0U))) {
        return static_cast<unsigned short>(result) + 1;
    } else {
        return static_cast<unsigned short>(result);
    }
}

inline float relativeError(float a, float b) {
    return std::abs(a - b) / (0.5f * (std::abs(a) + std::abs(b)));
}

// This seed is also used for the faiss float_rand API; in a test it
// is all within a single thread, so it is ok
long s_seed = 1;
std::mt19937 rng(1);
std::uniform_int_distribution<> distrib;

void newTestSeed() {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);

    setTestSeed(t.tv_nsec);
}

void setTestSeed(long seed) {
    printf("testing with random seed %ld\n", seed);

    rng = std::mt19937(seed);
    s_seed = seed;
}

int randVal(int a, int b) {
    EXPECT_GE(a, 0);
    EXPECT_LE(a, b);

    return a + (distrib(rng) % (b + 1 - a));
}

bool randBool() {
    return randSelect<bool>({true, false});
}

std::vector<float> randVecs(size_t num, size_t dim) {
    std::vector<float> v(num * dim);

    faiss::float_rand(v.data(), v.size(), s_seed);
    // unfortunately we generate separate sets of vectors, and don't
    // want the same values
    ++s_seed;

    return v;
}

std::vector<unsigned char> randBinaryVecs(size_t num, size_t dim) {
    std::vector<unsigned char> v(num * (dim / 8));

    faiss::byte_rand(v.data(), v.size(), s_seed);
    // unfortunately we generate separate sets of vectors, and don't
    // want the same values
    ++s_seed;

    return v;
}

std::vector<float> roundToHalf(const std::vector<float>& v) {
    auto out = std::vector<float>(v.size());
    for (int i = 0; i < v.size(); ++i) {
        out[i] = half2float(float2half(v[i]));
    }

    return out;
}

void compareIndices(
        const std::vector<float>& queryVecs,
        faiss::Index& refIndex,
        faiss::Index& testIndex,
        int numQuery,
        int /*dim*/,
        int k,
        const std::string& configMsg,
        float maxRelativeError,
        float pctMaxDiff1,
        float pctMaxDiffN) {
    // Compare
    std::vector<float> refDistance(numQuery * k, 0);
    std::vector<faiss::idx_t> refIndices(numQuery * k, -1);
    refIndex.search(
            numQuery,
            queryVecs.data(),
            k,
            refDistance.data(),
            refIndices.data());

    std::vector<float> testDistance(numQuery * k, 0);
    std::vector<faiss::idx_t> testIndices(numQuery * k, -1);
    testIndex.search(
            numQuery,
            queryVecs.data(),
            k,
            testDistance.data(),
            testIndices.data());

    faiss::gpu::compareLists(
            refDistance.data(),
            refIndices.data(),
            testDistance.data(),
            testIndices.data(),
            numQuery,
            k,
            configMsg,
            true,
            false,
            true,
            maxRelativeError,
            pctMaxDiff1,
            pctMaxDiffN);
}

void compareIndices(
        faiss::Index& refIndex,
        faiss::Index& testIndex,
        int numQuery,
        int dim,
        int k,
        const std::string& configMsg,
        float maxRelativeError,
        float pctMaxDiff1,
        float pctMaxDiffN) {
    auto queryVecs = faiss::gpu::randVecs(numQuery, dim);

    compareIndices(
            queryVecs,
            refIndex,
            testIndex,
            numQuery,
            dim,
            k,
            configMsg,
            maxRelativeError,
            pctMaxDiff1,
            pctMaxDiffN);
}

template <typename T>
inline T lookup(const T* p, int i, int j, int /*dim1*/, int dim2) {
    return p[i * dim2 + j];
}

void compareLists(
        const float* refDist,
        const faiss::idx_t* refInd,
        const float* testDist,
        const faiss::idx_t* testInd,
        int dim1,
        int dim2,
        const std::string& configMsg,
        bool printBasicStats,
        bool printDiffs,
        bool assertOnErr,
        float maxRelativeError,
        float pctMaxDiff1,
        float pctMaxDiffN) {
    float maxAbsErr = 0.0f;
    for (int i = 0; i < dim1 * dim2; ++i) {
        maxAbsErr = std::max(maxAbsErr, std::abs(refDist[i] - testDist[i]));
    }
    int numResults = dim1 * dim2;

    // query -> {index -> result position}
    std::vector<std::unordered_map<faiss::idx_t, int>> refIndexMap;

    for (int query = 0; query < dim1; ++query) {
        std::unordered_map<faiss::idx_t, int> indices;

        for (int result = 0; result < dim2; ++result) {
            indices[lookup(refInd, query, result, dim1, dim2)] = result;
        }

        refIndexMap.emplace_back(std::move(indices));
    }

    // See how far off the indices are
    // Keep track of the difference for each entry
    std::vector<std::vector<int>> indexDiffs;

    int diff1 = 0;   // index differs by 1
    int diffN = 0;   // index differs by >1
    int diffInf = 0; // index not found in the other
    int nonUniqueIndices = 0;

    double avgDiff = 0.0;
    int maxDiff = 0;
    float maxRelErr = 0.0f;

    for (int query = 0; query < dim1; ++query) {
        std::vector<int> diffs;
        std::set<faiss::idx_t> uniqueIndices;

        auto& indices = refIndexMap[query];

        for (int result = 0; result < dim2; ++result) {
            auto t = lookup(testInd, query, result, dim1, dim2);

            // All indices reported within a query should be unique; this is
            // a serious error if is otherwise the case.
            // If -1 is reported (no result due to IVF partitioning or not
            // enough entries in the index), then duplicates are allowed, but
            // both the reference and test must have -1 in the same position.
            if (t == -1) {
                EXPECT_EQ(lookup(refInd, query, result, dim1, dim2), t);
            } else {
                bool uniqueIndex = !uniqueIndices.contains(t);
                if (assertOnErr) {
                    EXPECT_TRUE(uniqueIndex) << configMsg << " " << query << " "
                                             << result << " " << t;
                }

                if (!uniqueIndex) {
                    ++nonUniqueIndices;
                } else {
                    uniqueIndices.insert(t);
                }

                auto it = indices.find(t);
                if (it != indices.end()) {
                    int diff = std::abs(result - it->second);
                    diffs.push_back(diff);

                    if (diff == 1) {
                        ++diff1;
                        maxDiff = std::max(diff, maxDiff);
                    } else if (diff > 1) {
                        ++diffN;
                        maxDiff = std::max(diff, maxDiff);
                    }

                    avgDiff += (double)diff;
                } else {
                    ++diffInf;
                    diffs.push_back(-1);
                    // don't count this for maxDiff
                }
            }

            auto refD = lookup(refDist, query, result, dim1, dim2);
            auto testD = lookup(testDist, query, result, dim1, dim2);

            float relErr = relativeError(refD, testD);

            if (assertOnErr) {
                EXPECT_LE(relErr, maxRelativeError)
                        << configMsg << " (" << query << ", " << result
                        << ") refD: " << refD << " testD: " << testD;
            }

            maxRelErr = std::max(maxRelErr, relErr);
        }

        indexDiffs.emplace_back(std::move(diffs));
    }

    if (assertOnErr) {
        EXPECT_LE(
                (float)(diff1 + diffN + diffInf),
                (float)numResults * pctMaxDiff1)
                << configMsg;

        // Don't count diffInf because that could be diff1 as far as we
        // know
        EXPECT_LE((float)diffN, (float)numResults * pctMaxDiffN) << configMsg;
    }

    avgDiff /= (double)numResults;

    if (printBasicStats) {
        if (!configMsg.empty()) {
            printf("Config\n"
                   "----------------------------\n"
                   "%s\n",
                   configMsg.c_str());
        }

        printf("Result error and differences\n"
               "----------------------------\n"
               "max abs diff %.7f rel diff %.7f\n"
               "idx diff avg: %.5g max: %d\n"
               "idx diff of 1:      %d (%.3f%% of queries)\n"
               "idx diff of >1:     %d (%.3f%% of queries)\n"
               "idx diff not found: %d (%.3f%% of queries)"
               " [typically a last element inversion]\n"
               "non-unique indices: %d (a serious error if >0)\n",
               maxAbsErr,
               maxRelErr,
               avgDiff,
               maxDiff,
               diff1,
               100.0f * (float)diff1 / (float)numResults,
               diffN,
               100.0f * (float)diffN / (float)numResults,
               diffInf,
               100.0f * (float)diffInf / (float)numResults,
               nonUniqueIndices);
    }

    if (printDiffs) {
        printf("differences:\n");
        printf("==================\n");
        for (int query = 0; query < dim1; ++query) {
            for (int result = 0; result < dim2; ++result) {
                long refI = lookup(refInd, query, result, dim1, dim2);
                long testI = lookup(testInd, query, result, dim1, dim2);

                if (refI != testI) {
                    float refD = lookup(refDist, query, result, dim1, dim2);
                    float testD = lookup(testDist, query, result, dim1, dim2);

                    float maxDist = std::max(refD, testD);
                    float delta = std::abs(refD - testD);

                    float relErr = delta / maxDist;

                    if (refD == testD) {
                        printf("(%d, %d [%d]) (ref %ld tst %ld dist ==)\n",
                               query,
                               result,
                               indexDiffs[query][result],
                               refI,
                               testI);
                    } else {
                        printf("(%d, %d [%d]) (ref %ld tst %ld abs %.8f "
                               "rel %.8f ref %a tst %a)\n",
                               query,
                               result,
                               indexDiffs[query][result],
                               refI,
                               testI,
                               delta,
                               relErr,
                               refD,
                               testD);
                    }
                }
            }
        }
    }
}

} // namespace gpu
} // namespace faiss
