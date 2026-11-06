#ifndef OPENMW_VITA_BENCH_TIMING_H
#define OPENMW_VITA_BENCH_TIMING_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace Vita::Bench
{
    struct FrameSample
    {
        std::uint64_t mDurationUs;
        std::uint64_t mVblankWaitUs;

        std::uint64_t residualUs() const
        {
            return mDurationUs - std::min(mDurationUs, mVblankWaitUs);
        }
    };

    class FrameClock
    {
    public:
        void start(std::uint64_t nowUs)
        {
            mPreviousUs = nowUs;
            mRejectedSamples = 0;
        }

        std::optional<FrameSample> sample(std::uint64_t nowUs, std::uint64_t vblankWaitUs)
        {
            if (!mPreviousUs)
            {
                start(nowUs);
                return std::nullopt;
            }
            if (nowUs <= *mPreviousUs)
            {
                ++mRejectedSamples;
                return std::nullopt;
            }
            const FrameSample result{ nowUs - *mPreviousUs, vblankWaitUs };
            mPreviousUs = nowUs;
            return result;
        }

        std::size_t rejectedSamples() const { return mRejectedSamples; }

    private:
        std::optional<std::uint64_t> mPreviousUs;
        std::size_t mRejectedSamples = 0;
    };

    struct Distribution
    {
        std::size_t mCount = 0;
        double mTotalMs = 0;
        double mMeanMs = 0;
        double mMedianMs = 0;
        double mP90Ms = 0;
        double mP95Ms = 0;
        double mP99Ms = 0;
        double mMaxMs = 0;
        double mMeanFps = 0;
        std::size_t mOver50Ms = 0;
        std::size_t mOver100Ms = 0;
    };

    inline Distribution summarize(const std::vector<FrameSample>& samples, bool residual = false)
    {
        Distribution result;
        if (samples.empty())
            return result;
        std::vector<double> milliseconds;
        milliseconds.reserve(samples.size());
        for (const FrameSample& sample : samples)
        {
            const double ms = (residual ? sample.residualUs() : sample.mDurationUs) / 1000.0;
            milliseconds.push_back(ms);
            result.mTotalMs += ms;
            result.mOver50Ms += ms > 50.0;
            result.mOver100Ms += ms > 100.0;
        }
        std::sort(milliseconds.begin(), milliseconds.end());
        const auto percentile = [&](double quantile) {
            const std::size_t rank = static_cast<std::size_t>(std::ceil(quantile * milliseconds.size()));
            return milliseconds[std::clamp(rank, std::size_t{ 1 }, milliseconds.size()) - 1];
        };
        result.mCount = samples.size();
        result.mMeanMs = result.mTotalMs / result.mCount;
        result.mMedianMs = percentile(0.5);
        result.mP90Ms = percentile(0.9);
        result.mP95Ms = percentile(0.95);
        result.mP99Ms = percentile(0.99);
        result.mMaxMs = milliseconds.back();
        if (result.mTotalMs > 0)
            result.mMeanFps = result.mCount * 1000.0 / result.mTotalMs;
        return result;
    }
}

#endif
