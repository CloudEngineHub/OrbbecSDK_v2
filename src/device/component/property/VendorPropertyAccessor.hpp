#pragma once
#include "IProperty.hpp"
#include "ISourcePort.hpp"
#include "IDevice.hpp"
#include "IDeviceComponent.hpp"
#include <mutex>

namespace libobsensor {
class VendorPropertyAccessor : public IDeviceComponent,
                               public IBasicPropertyAccessor,
                               public IStructureDataAccessor,
                               public IStructureDataAccessorV1_1,
                               public IRawDataAccessor {
public:
    explicit VendorPropertyAccessor(IDevice *owner, const std::shared_ptr<ISourcePort> &backend);
    ~VendorPropertyAccessor() noexcept override = default;

    IDevice *getOwner() const override;

    void setPropertyValue(uint32_t propertyId, const OBPropertyValue &value) override;
    void getPropertyValue(uint32_t propertyId, OBPropertyValue *value) override;
    void getPropertyRange(uint32_t propertyId, OBPropertyRange *range) override;

    void                        setStructureData(uint32_t propertyId, const std::vector<uint8_t> &data) override;
    const std::vector<uint8_t> &getStructureData(uint32_t propertyId) override;

    void getRawData(uint32_t propertyId, GetDataCallback callback) override;

    uint16_t                    getCmdVersionProtoV1_1(uint32_t propertyId) override;
    const std::vector<uint8_t> &getStructureDataProtoV1_1(uint32_t propertyId, uint16_t cmdVersion) override;
    void                        setStructureDataProtoV1_1(uint32_t propertyId, const std::vector<uint8_t> &data, uint16_t cmdVersion) override;
    const std::vector<uint8_t> &getStructureDataListProtoV1_1(uint32_t propertyId, uint16_t cmdVersion) override;

private:
    void clearBuffers();

private:
    IDevice                          *owner_;
    std::shared_ptr<ISourcePort>      backend_;
    std::mutex                        mutex_;
    std::vector<uint8_t>              recvData_;
    std::vector<uint8_t>              sendData_;
    std::vector<uint8_t>              outputData_;
    std::vector<std::vector<uint8_t>> structureDataList_;  // for cmd version 1.1
};
}  // namespace libobsensor
