#pragma once
#include "FilterBase.hpp"
#include "libobsensor/h/ObTypes.h"
// #include "IProperty.hpp"
#include "InternalTypes.hpp"

namespace libobsensor {

class IMUCorrector : public FilterBase {
public:
    static OBIMUCalibrateParams parserIMUCalibrationParamsRaw(uint8_t *data, uint32_t size);
    static OBIMUCalibrateParams getDefaultImuCalibParam();
    static float                calculateAccelGravity(int16_t accelValue, uint8_t accelFSR);
    static float                calculateGyroDPS(int16_t gyroValue, uint8_t gyroFSR);
    static float                calculateRegisterTemperature(int16_t tempValue);

public:
    IMUCorrector(const std::string &name);

    virtual ~IMUCorrector() = default;

    // Config
    void               updateConfig(std::vector<std::string> &params) override;
    const std::string &getConfigSchema() const override;

private:
    std::shared_ptr<Frame> processFunc(std::shared_ptr<const Frame> frame) override;

    OBAccelValue correctAccel(const OBAccelValue &accelValue, OBAccelIntrinsic *intrinsic);
    OBGyroValue  correctGyro(const OBGyroValue &gyroValue, OBGyroIntrinsic *intrinsic);

protected:
    static constexpr float GYRO_MAX  = 32800 / 0.017453293f;
    static constexpr float ACCEL_MAX = 32768 / 9.80f;
};

}  // namespace libobsensor
