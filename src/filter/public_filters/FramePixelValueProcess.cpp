#include "FramePixelValueProcess.hpp"
#include "exception/ObException.hpp"
#include "logger/LoggerInterval.hpp"
#include "frame/FrameFactory.hpp"

namespace libobsensor {

template <typename T> void imagePixelValueScale(const T *src, T *dst, uint32_t width, uint32_t height, float scale_) {
    const T *srcPixel = src;
    T       *dstPixel = dst;
    for(uint32_t h = 0; h < height; h++) {
        for(uint32_t w = 0; w < width; w++) {
            *dstPixel = (T)(*srcPixel * scale_);
            srcPixel++;
            dstPixel++;
        }
    }
}

template <typename T> void imagePixelValueOffset(T *src, T *dst, uint32_t width, uint32_t height, int8_t offset) {
    T *dstPixel = dst;
    T *srcPixel = src;
    if(offset > 0) {
        uint8_t shiftCount = offset;
        for(uint32_t h = 0; h < height; h++) {
            for(uint32_t w = 0; w < width; w++) {
                *dstPixel = *srcPixel >> shiftCount;
                srcPixel++;
                dstPixel++;
            }
        }
    }
    else if(offset < 0) {
        uint8_t shiftCount = -offset;
        for(uint32_t h = 0; h < height; h++) {
            for(uint32_t w = 0; w < width; w++) {
                *dstPixel = *srcPixel << shiftCount;
                srcPixel++;
                dstPixel++;
            }
        }
    }
}

template <typename T> void imagePixelValueCutOff(T *src, T *dst, uint32_t width, uint32_t height, uint32_t min, uint32_t max) {
    T *dstPixel = dst;
    T *srcPixel = src;
    if(min >= max) {
        for(uint32_t h = 0; h < height; h++) {
            for(uint32_t w = 0; w < width; w++) {
                *dstPixel = 0;
                srcPixel++;
                dstPixel++;
            }
        }
    }
    else {
        for(uint32_t h = 0; h < height; h++) {
            for(uint32_t w = 0; w < width; w++) {
                *dstPixel = (*srcPixel < min) ? 0 : ((*srcPixel > max) ? 0 : *srcPixel);
                srcPixel++;
                dstPixel++;
            }
        }
    }
}

PixelValueScaler::PixelValueScaler(const std::string &name) : FilterBase(name) {}
PixelValueScaler::~PixelValueScaler() noexcept {}

void PixelValueScaler::updateConfig(std::vector<std::string> &params) {
    if(params.size() != 1) {
        throw invalid_value_exception("PixelValueScaler config error: params size not match");
    }
    try {
        scale_ = std::stof(params[0]);
    }
    catch(const std::exception &e) {
        throw invalid_value_exception("PixelValueScaler config error: " + std::string(e.what()));
    }
}

const std::string &PixelValueScaler::getConfigSchema() const {
    // csv format: name，type， min，max，step，default，description
    static const std::string schema = "scale, float, 0.01, 100.0, 0.01, 1.0, value scale factor";
    return schema;
}

std::shared_ptr<Frame> PixelValueScaler::processFunc(std::shared_ptr<const Frame> frame) {
    if(frame->getType() != OB_FRAME_DEPTH) {
        LOG_WARN_INTVL("PixelValueScaler unsupported to process this frame type: {}", frame->getType());
        return FrameFactory::cloneFrame(frame);
    }

    auto depthFrame = frame->as<DepthFrame>();

    auto outFrame = FrameFactory::cloneFrame(frame);

    switch(frame->getFormat()) {
    case OB_FORMAT_Y16:
        imagePixelValueScale<uint16_t>((uint16_t *)frame->getData(), (uint16_t *)outFrame->getData(), depthFrame->getWidth(), depthFrame->getHeight(), scale_);
        break;
    default:
        LOG_ERROR_INTVL("PixelValueScaler: unsupported format: {}", frame->getFormat());
        break;
    }

    auto outDepthFrame = outFrame->as<DepthFrame>();
    auto valueScale    = outDepthFrame->getValueScale();
    outDepthFrame->setValueScale(valueScale * scale_);

    return outFrame;
}

PixelValueCutOff::PixelValueCutOff(const std::string &name) : FilterBase(name) {}
PixelValueCutOff::~PixelValueCutOff() noexcept {}

void PixelValueCutOff::updateConfig(std::vector<std::string> &params) {
    if(params.size() != 2) {
        throw invalid_value_exception("PixelValueCutOff config error: params size not match");
    }
    try {
        int min = std::stoi(params[0]);
        if(min >= 0 && min <= 16000) {
            min_ = min;
        }

        int max = std::stoi(params[1]);
        if(max >= 0 && max <= 16000) {
            max_ = max;
        }
    }
    catch(const std::exception &e) {
        throw invalid_value_exception("PixelValueCutOff config error: " + std::string(e.what()));
    }
}

const std::string &PixelValueCutOff::getConfigSchema() const {
    // csv format: name，type， min，max，step，default，description
    static const std::string schema = "min, int, 0, 16000, 1, 0, min depth range\n"
                                      "max, int, 0, 16000, 1, 16000, max depth range";
    return schema;
}

std::shared_ptr<Frame> PixelValueCutOff::processFunc(std::shared_ptr<const Frame> frame) {
    if(!frame) {
        return nullptr;
    }

    auto  videoFrame = frame->as<VideoFrame>();
    auto  outFrame   = FrameFactory::cloneFrame(frame);
    float scale      = 1.0f;
    if(frame->is<DepthFrame>()) {
        scale = frame->as<DepthFrame>()->getValueScale();
    }

    if(max_ != 65535) {
        switch(frame->getFormat()) {
        case OB_FORMAT_Y16:
            imagePixelValueCutOff((uint16_t *)frame->getData(), (uint16_t *)outFrame->getData(), videoFrame->getWidth(), videoFrame->getHeight(),
                                  (uint32_t)(min_ / scale), (uint32_t)(max_ / scale));
            break;
        case OB_FORMAT_Y8:
            imagePixelValueCutOff((uint8_t *)frame->getData(), (uint8_t *)outFrame->getData(), videoFrame->getWidth(), videoFrame->getHeight(),
                                  (uint32_t)(min_ / scale), (uint32_t)(max_ / scale));
            break;
        default:
            LOG_ERROR_INTVL("PixelValueCutOff: unsupported format: {}", frame->getFormat());
            break;
        }
    }

    return outFrame;
}

PixelValueOffset::PixelValueOffset(const std::string &name) : FilterBase(name) {}
PixelValueOffset::~PixelValueOffset() noexcept {}

void PixelValueOffset::updateConfig(std::vector<std::string> &params) {
    if(params.size() != 1) {
        throw invalid_value_exception("PixelValueOffset config error: params size not match");
    }
    try {
        offset_ = static_cast<int8_t>(std::stoi(params[0]));
    }
    catch(const std::exception &e) {
        throw invalid_value_exception("PixelValueOffset config error: " + std::string(e.what()));
    }
}

const std::string &PixelValueOffset::getConfigSchema() const {
    // csv format: name，type， min，max，step，default，description
    static const std::string schema = "offset, int, -16, 16, 1, 0, value offset factor";
    return schema;
}

std::shared_ptr<Frame> PixelValueOffset::processFunc(std::shared_ptr<const Frame> frame) {
    if(!frame) {
        return nullptr;
    }

    auto videoFrame = frame->as<VideoFrame>();
    auto outFrame   = FrameFactory::cloneFrame(frame);
    if(offset_ != 0) {
        switch(frame->getFormat()) {
        case OB_FORMAT_Y16:
            imagePixelValueOffset((uint16_t *)frame->getData(), (uint16_t *)outFrame->getData(), videoFrame->getWidth(), videoFrame->getHeight(), offset_);
            break;
        case OB_FORMAT_Y8:
            imagePixelValueOffset((uint8_t *)frame->getData(), (uint8_t *)outFrame->getData(), videoFrame->getWidth(), videoFrame->getHeight(), offset_);
            break;
        default:
            LOG_ERROR_INTVL("PixelValueOffset: unsupported format: {}", frame->getFormat());
            break;
        }

        uint8_t bitSize = frame->as<VideoFrame>()->getPixelAvailableBitSize();
        outFrame->as<VideoFrame>()->setPixelAvailableBitSize(bitSize - offset_);
    }
    return outFrame;
}

}  // namespace libobsensor
