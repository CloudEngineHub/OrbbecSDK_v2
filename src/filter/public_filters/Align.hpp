#pragma once
#include "FilterBase.hpp"
#include "AlignImpl.hpp"

namespace libobsensor {

/**
 * @brief Aligh depth to color or vice verse
 */
class Align : public FilterBase {
public:
    Align(const std::string &name, OBStreamType align_to_stream);
    virtual ~Align() noexcept;

    OBStreamType getAlignToStreamType() {
        return align_to_stream_;
    }

    void               updateConfig(std::vector<std::string> &params) override;
    const std::string &getConfigSchema() const override;

    virtual void reset() override;

private:
    std::shared_ptr<Frame> processFunc(std::shared_ptr<const Frame> frame) override;

protected:
    OBFrameType getFrameType();

    void alignFrames(std::shared_ptr<Frame> aligned, const std::shared_ptr<Frame> from, const std::shared_ptr<Frame> to);

    std::shared_ptr<VideoStreamProfile> createAlignedProfile(std::shared_ptr<const VideoStreamProfile> original_profile,
                                                             std::shared_ptr<const VideoStreamProfile> to_profile);

    virtual void resetCache(){};

    std::pair<const VideoStreamProfile *, const VideoStreamProfile *> align_streams_;
    std::shared_ptr<VideoStreamProfile>                               target_stream_profile_;

    OBStreamType       align_to_stream_;

    std::recursive_mutex alignMutex_;

private:
    AlignImpl *pImpl;
    float              depth_unit_mm_;
    bool               add_target_distortion_;
    bool               gap_fill_copy_;
    OBCameraIntrinsic  from_intrin_;
    OBCameraDistortion from_disto_;
    OBCameraIntrinsic  to_intrin_;
    OBCameraDistortion to_disto_;
    OBExtrinsic        from_to_extrin_;
};

}  // namespace libobsensor