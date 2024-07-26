#include "G2StreamProfileFilter.hpp"

#include "utils/Utils.hpp"
#include "stream/StreamProfile.hpp"
#include "exception/ObException.hpp"
#include "property/InternalProperty.hpp"

namespace libobsensor {
G2StreamProfileFilter::G2StreamProfileFilter(IDevice *owner) : DeviceComponentBase(owner) {
    fetchEffectiveStreamProfiles();
}

bool isMatch(OBSensorType sensorType, std::shared_ptr<const VideoStreamProfile> videoProfile, OBEffectiveStreamProfile effProfile) {
    bool isSensorTypeEqual = sensorType == effProfile.sensorType;

    // Compatibility processing, normal mode has IR, but no IR_LEFT, IR_RIGHT; calibration mode has no IR, but has IR_LEFT, IR_RIGHT
    // IR and IR_LEFT both use the IR channel, so they are the same. IR_RIGHT uses the Depth channel, so IR_RIGHT must match accurately
    if(!isSensorTypeEqual && sensorType != OB_SENSOR_IR_RIGHT && effProfile.sensorType != OB_SENSOR_IR_RIGHT) {
        isSensorTypeEqual = isIRSensor(sensorType) && isIRSensor(effProfile.sensorType);
    }

    return isSensorTypeEqual && (videoProfile->getFormat() == effProfile.format) && (videoProfile->getFps() <= effProfile.maxFps)
           && (videoProfile->getWidth() == effProfile.width) && (videoProfile->getHeight() == effProfile.height);
}

StreamProfileList G2StreamProfileFilter::filter(const StreamProfileList &profiles) const {
    StreamProfileList filteredProfiles;

    for(const auto &profile: profiles) {
        if(!profile->is<VideoStreamProfile>()) {
            filteredProfiles.push_back(profile);
            continue;
        }
        auto videoProfile = profile->as<VideoStreamProfile>();
        auto streamType   = profile->getType();
        auto sensorType   = utils::mapStreamTypeToSensorType(streamType);

        for(auto &effProfile: effectiveStreamProfiles_) {
            if(isMatch(sensorType, videoProfile, effProfile)) {
                filteredProfiles.push_back(profile);
                continue;
            }
        }
    }

    return filteredProfiles;
}

std::vector<OBEffectiveStreamProfile> effectiveStreamProfilesParse(const std::vector<uint8_t> &data) {
    std::vector<OBEffectiveStreamProfile> output;
    const uint32_t                        typeSize  = sizeof(OBEffectiveStreamProfile);
    auto                                  itemCount = data.size() / typeSize;
    for(uint32_t i = 0; i < itemCount; i++) {
        auto profile = reinterpret_cast<const OBEffectiveStreamProfile *>(data.data() + i * typeSize);
        output.push_back(*profile);
    }
    return output;
}

void G2StreamProfileFilter::fetchEffectiveStreamProfiles() {
    auto owner      = getOwner();
    auto propServer = owner->getPropertyServer();

    std::vector<uint8_t> data;
    BEGIN_TRY_EXECUTE({
        propServer->getRawData(
            OB_RAW_DATA_EFFECTIVE_VIDEO_STREAM_PROFILE_LIST,
            [&](OBDataTranState state, OBDataChunk *dataChunk) {
                if(state == DATA_TRAN_STAT_TRANSFERRING) {
                    data.insert(data.end(), dataChunk->data, dataChunk->data + dataChunk->size);
                }
            },
            PROP_ACCESS_INTERNAL);
    })
    CATCH_EXCEPTION_AND_EXECUTE({
        LOG_ERROR("Get imu calibration params failed!");
        data.clear();
    })
    auto profiles            = effectiveStreamProfilesParse(data);
    effectiveStreamProfiles_ = profiles;
    for(auto &profile: profiles) {
        if(profile.sensorType == OB_SENSOR_DEPTH && profile.format == OB_FORMAT_RLE) {
            auto nerProfile   = profile;
            nerProfile.format = OB_FORMAT_Y16;
            effectiveStreamProfiles_.push_back(nerProfile);
        }
        else if(profile.sensorType == OB_SENSOR_COLOR && profile.format == OB_FORMAT_MJPG) {
            auto nerProfile   = profile;
            nerProfile.format = OB_FORMAT_RGB;
            effectiveStreamProfiles_.push_back(nerProfile);
            nerProfile.format = OB_FORMAT_BGR;
            effectiveStreamProfiles_.push_back(nerProfile);
            nerProfile.format = OB_FORMAT_BGRA;
            effectiveStreamProfiles_.push_back(nerProfile);
        }
    }
}

}  // namespace libobsensor