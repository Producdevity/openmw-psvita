#include "apps/openmw/vita/VitaBenchTiming.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace
{
    void require(bool condition, const char* message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    void near(double actual, double expected, const char* message)
    {
        require(std::abs(actual - expected) < 1e-9, message);
    }

    void testClock()
    {
        Vita::Bench::FrameClock clock;
        require(!clock.sample(0, 0), "The first boundary must not create a frame");
        const auto normal = clock.sample(16667, 1000);
        require(normal && normal->mDurationUs == 16667, "A zero start timestamp must be valid");
        require(normal->residualUs() == 15667, "Subtract pacing only from the residual diagnostic");
        const auto stall = clock.sample(516667, 2000);
        require(stall && stall->mDurationUs == 500000, "Retain stalls beyond the 200ms simulation cap");
        const auto longStall = clock.sample(2016667, 0);
        require(longStall && longStall->mDurationUs == 1500000, "Retain stalls beyond one second");
        require(!clock.sample(2016667, 0), "Reject zero-length intervals");
        require(!clock.sample(2000000, 0), "Reject backward timestamps");
        require(clock.rejectedSamples() == 2, "Expose rejected samples");
        const auto recovered = clock.sample(2036667, 0);
        require(recovered && recovered->mDurationUs == 20000, "Invalid timestamps must not move the boundary");
        clock.start(4000000);
        require(clock.rejectedSamples() == 0, "A new window resets clock rejection counts");
        const auto restarted = clock.sample(4040000, 90000);
        require(restarted && restarted->mDurationUs == 40000, "Starting a new window excludes earlier time");
        require(restarted->mVblankWaitUs == 90000, "Keep raw pacing even when it exceeds the boundary interval");
        require(restarted->residualUs() == 0, "Pacing subtraction must not underflow");
    }

    void testStatistics()
    {
        std::vector<Vita::Bench::FrameSample> samples;
        for (std::uint64_t ms = 1; ms <= 100; ++ms)
            samples.push_back({ ms * 1000, 1000 });
        const auto stats = Vita::Bench::summarize(samples);
        require(stats.mCount == 100, "Summarize every sample");
        near(stats.mTotalMs, 5050, "Retain total observed duration");
        near(stats.mMeanMs, 50.5, "Mean frame duration");
        near(stats.mMedianMs, 50, "Median uses nearest rank");
        near(stats.mP90Ms, 90, "p90 uses nearest rank");
        near(stats.mP95Ms, 95, "p95 uses nearest rank");
        near(stats.mP99Ms, 99, "p99 uses nearest rank");
        near(stats.mMaxMs, 100, "Worst frame duration");
        near(stats.mMeanFps, 100000.0 / 5050, "Mean FPS is frame count divided by elapsed time");
        require(stats.mOver50Ms == 50 && stats.mOver100Ms == 0, "Hitch thresholds are strict");
        const auto residual = Vita::Bench::summarize(samples, true);
        near(residual.mTotalMs, 4950, "Residual diagnostic does not alter frame durations");
        near(residual.mMedianMs, 49, "Residual median");
        require(samples.front().mDurationUs == 1000, "Summarizing must preserve raw samples");
    }

    void testHitchesAndEdges()
    {
        const std::vector<Vita::Bench::FrameSample> samples{ { 20000, 0 }, { 500000, 0 }, { 1500000, 0 } };
        const auto stats = Vita::Bench::summarize(samples);
        near(stats.mTotalMs, 2020, "Long hitches must contribute to elapsed time");
        near(stats.mP95Ms, 1500, "Long hitches must appear in tail percentiles");
        near(stats.mMaxMs, 1500, "Long hitches must remain visible as worst frames");
        require(stats.mOver100Ms == 2, "Count long hitches without filtering");
        const auto empty = Vita::Bench::summarize({});
        require(empty.mCount == 0 && empty.mMeanFps == 0, "An empty run has no performance estimate");
        const auto one = Vita::Bench::summarize({ { 25000, 0 } });
        near(one.mMeanFps, 40, "A 25ms frame has a 40FPS cadence");
        near(one.mP99Ms, 25, "One-sample percentiles must remain in bounds");
    }
}

int main()
{
    testClock();
    testStatistics();
    testHitchesAndEdges();
    std::puts("Vita benchmark timing tests passed");
}
