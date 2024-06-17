#pragma once
// #include "core/frame/process/FrameProcessingBlock.hpp"
#include "openobsdk/h/ObTypes.h"
#include "frame/Frame.hpp"
#include "pipeline/Config.hpp"

#include <map>
#include <queue>
#include <mutex>
#include <memory>
#include <algorithm>

namespace libobsensor {

struct SourceFrameQueue {
    std::queue<std::shared_ptr<const Frame>> queue;
    uint32_t                                 maxSyncQueueSize_;
    uint32_t                                 halfTspGap;
};

class FrameAggregator {
public:
    FrameAggregator();
    ~FrameAggregator() noexcept;

    void updateConfig(std::shared_ptr<const Config> config, const bool matchingRateFirst);
    void pushFrame(std::shared_ptr<const Frame> frame);
    void enableFrameSync(bool enable);
    void setCallback(FrameCallback callback);

    void clearFrameQueue(OBFrameType frameType);
    void clearAllFrameQueue();

private:
    void outputFrameset(std::shared_ptr<const FrameSet> frameSet);
    void reset();
    void tryAggregator();

private:
    bool                                    frameSync_;
    std::map<OBFrameType, SourceFrameQueue> srcFrameQueueMap_;
    std::recursive_mutex                    srcFrameQueueMutex_;
    FrameCallback                           FrameSetCallbackFunc_;
    uint64_t                                miniTimeStamp_;
    OBFrameType                             miniTimeStampFrameType_;
    bool                                    withOverflowQueue_;
    OBFrameType                             withOverflowQueueFrameType_;
    bool                                    withEmptyQueue_;
    OBFrameAggregateOutputMode              frameAggregateOutputMode_;
    uint32_t                                frameCnt_;
    bool                                    withColorFrame_;
    bool                                    matchingRateFirst_;
};
}  // namespace libobsensor