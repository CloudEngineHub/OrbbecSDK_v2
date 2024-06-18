#pragma once
#include "IDeviceEnumerator.hpp"
#include "DeviceWatcher.hpp"
#include "ObPal.hpp"

#include <memory>

namespace libobsensor {
class UsbDeviceEnumerator : public IDeviceEnumerator, public std::enable_shared_from_this<IDeviceEnumerator> {
public:
    UsbDeviceEnumerator(DeviceChangedCallback callback);
    ~UsbDeviceEnumerator() noexcept override;
    DeviceEnumInfoList       getDeviceInfoList() override;
    void                     setDeviceChangedCallback(DeviceChangedCallback callback) override;

private:
    void               onPalDeviceChanged(OBDeviceChangedType changeType, std::string devUid);
    DeviceEnumInfoList queryRemovedDevice(std::string rmDevUid);
    DeviceEnumInfoList queryArrivalDevice();

    void deviceArrivalHandleThreadFunc();

    static DeviceEnumInfoList usbDeviceInfoMatch(const SourcePortInfoList infoList);

private:
    std::shared_ptr<ObPal> obPal_;
    bool                   destroy_ = false;

    std::shared_ptr<DeviceWatcher> deviceWatcher_;

    DeviceChangedCallback devChangedCallback_ = nullptr;
    std::thread           devChangedCallbackThread_;

    SourcePortInfoList      currentUsbPortInfoList_;
    bool                    newUsbPortArrival_ = false;
    std::condition_variable newUsbPortArrivalCV_;
    std::thread             deviceArrivalHandleThread_;

    DeviceEnumInfoList   deviceInfoList_;
    std::recursive_mutex deviceInfoListMutex_;

    std::mutex callbackMutex_;
};
}  // namespace libobsensor