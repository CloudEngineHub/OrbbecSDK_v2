#pragma once
#include "FilterBase.hpp"
#include <mutex>

namespace libobsensor {

class FormatConverter : public FilterBase {
public:
    FormatConverter(const std::string &name);
    virtual ~FormatConverter() noexcept;

    void               updateConfig(std::vector<std::string> &params) override;
    const std::string &getConfigSchema() const override;

private:
    std::shared_ptr<Frame> processFunc(std::shared_ptr<const Frame> frame) override;

    void yuyvToRgb(uint8_t *src, uint8_t *target, uint32_t width, uint32_t height);
    void yuyvToRgba(uint8_t *src, uint8_t *target, uint32_t width, uint32_t height);
    void yuyvToBgr(uint8_t *src, uint8_t *target, uint32_t width, uint32_t height);
    void yuyvToBgra(uint8_t *src, uint8_t *target, uint32_t width, uint32_t height);
    void yuyvToy16(uint8_t *src, uint8_t *target, uint32_t width, uint32_t height);
    void yuyvToy8(uint8_t *src, uint8_t *target, uint32_t width, uint32_t height);
    void uyvyToRgb(uint8_t *src, uint8_t *target, uint32_t width, uint32_t height);
    void i420ToRgb(uint8_t *src, uint8_t *target, uint32_t width, uint32_t height);
    void nv21ToRgb(uint8_t *src, uint8_t *target, uint32_t width, uint32_t height);
    void nv12ToRgb(uint8_t *src, uint8_t *target, uint32_t width, uint32_t height);
    void mjpgToI420(uint8_t *src, uint32_t src_len, uint8_t *target, uint32_t width, uint32_t height);
    void mjpgToNv21(uint8_t *src, uint32_t src_len, uint8_t *target, uint32_t width, uint32_t height);
    bool mjpgToRgb(uint8_t *src, uint32_t src_len, uint8_t *target, uint32_t width, uint32_t height);
    bool mjpgToBgr(uint8_t *src, uint32_t src_len, uint8_t *target, uint32_t width, uint32_t height);
    void exchangeRAndB(uint8_t *pucRgb, uint8_t *target, uint32_t width, uint32_t height, uint32_t pixelSize = 3);
    void mjpegToBgra(uint8_t *src, uint32_t src_len, uint8_t *target, uint32_t width, uint32_t height);
    void mjpgToNv12(uint8_t *src, uint32_t src_len, uint8_t *target, uint32_t width, uint32_t height);

protected:
    std::shared_ptr<const StreamProfile> currentStreamProfile_;
    std::shared_ptr<StreamProfile>       tarStreamProfile_;
    OBConvertFormat                      convertType_;
    uint8_t *                            tempDataBuf_     = nullptr;
    uint32_t                             tempDataBufSize_ = 0;

};

}  // namespace libobsensor
