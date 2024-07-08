#include "DeviceMonitor.hpp"
#include "protocol/Protocol.hpp"

namespace libobsensor {

DeviceMonitor::DeviceMonitor(IDevice *owner, std::shared_ptr<ISourcePort> dataPort)
    : DeviceComponentBase(owner), cbIdCounter_(0), heartbeatEnabled_(false), heartbeatPaused_(false), recvData_(1024), sendData_(1024) {
    vendorDataPort_ = std::dynamic_pointer_cast<IVendorDataPort>(dataPort);
    if(!vendorDataPort_) {
        throw std::runtime_error("DeviceMonitor: data port must be a vendor data port!");
    }
}

DeviceMonitor::~DeviceMonitor() noexcept {
    if(heartbeatEnabled_ && !heartbeatPaused_) {
        TRY_EXECUTE(disableHeartbeat());
    }

    if(heartbeatAndFetchStateThreadStarted_) {
        TRY_EXECUTE(stop());
    }
}

void DeviceMonitor::start() {
    if(heartbeatAndFetchStateThreadStarted_) {
        LOG_DEBUG("Heartbeat and fetch state thread already started!");
    }
    heartbeatAndFetchStateThreadStarted_ = true;
    heartbeatAndFetchStateThread_        = std::thread(&DeviceMonitor::poll, this);
}

void DeviceMonitor::stop() {
    if(!heartbeatAndFetchStateThreadStarted_) {
        LOG_DEBUG("Heartbeat and fetch state thread not started!");
        return;
    }
    heartbeatAndFetchStateThreadStarted_ = false;
    heartbeatAndFetchStateThreadCv_.notify_one();
    heartbeatAndFetchStateThread_.join();
    LOG_DEBUG("Heartbeat and fetch state thread stopped!");
}

void DeviceMonitor::poll() {
    const uint32_t HEARTBEAT_INTERVAL_MS = 3000;
    while(heartbeatAndFetchStateThreadStarted_) {
        std::unique_lock<std::mutex> lock(commMutex_);
        heartbeatAndFetchStateThreadCv_.wait_for(lock, std::chrono::milliseconds(HEARTBEAT_INTERVAL_MS),
                                                 [this]() { return !heartbeatAndFetchStateThreadStarted_; });
        if(!heartbeatAndFetchStateThreadStarted_) {
            break;
        }
        heartbeatAndFetchState();
    }
}

void DeviceMonitor::heartbeatAndFetchState() {
    bool emitNextHeartBeatImmediately = false;
    do {
        auto     req          = protocol::initHeartbeatAndStateReq(sendData_.data());
        uint16_t respDataSize = 1024;  // excepted size
        auto     res          = protocol::execute(vendorDataPort_, sendData_.data(), sizeof(req), recvData_.data(), &respDataSize);
        if(!protocol::checkStatus(res, false)) {
            utils::sleepMs(50);
            continue;
        }

        protocol::HeartbeatAndStateResp *resp;
        BEGIN_TRY_EXECUTE({ resp = protocol::parseHeartbeatAndStateResp(recvData_.data(), respDataSize); })
        CATCH_EXCEPTION_AND_EXECUTE({ continue; })

        // Heartbeat state value
        // ==================================================
        // |--[0, 31]bit --|--[32, 62]bit --|-- [63]bit ---|
        // |---------------|----------------|-------------|
        // |--ERROR msg  --|--WARNING msg --| CACHE flag  |
        // ==================================================

        // The highest bit is the device status information cache flag bit. If the device also caches other status information, the next heartbeat should be
        // executed immediately.
        emitNextHeartBeatImmediately = (bool)(resp->state >> 63);

        // Get status code (excluding cache flag 1 bit)
        devState_ = (OBDeviceState)(resp->state & 0x7FFFFFFFFFFFFFFF);

        // The remaining information is msg
        auto msgSize = resp->header.sizeInHalfWords * 2 - 10;  // Remove header.error and state, the remaining is msg

        // Callback when firmware returns non-0 status code
        if(0 != devState_ || msgSize) {
            auto msg = std::string(resp->message, msgSize);

            LOG_INFO("Firmware State/Log ({0}):\n{1}", devState_, msg);
            std::lock_guard<std::mutex> lock(stateChangedCallbacksMutex_);
            for(auto &callback: stateChangedCallbacks_) {
                callback.second(devState_, msg);
            }
        }
    } while(emitNextHeartBeatImmediately);
}

OBDeviceState DeviceMonitor::getCurrentDeviceState() const {
    if(!heartbeatAndFetchStateThreadStarted_) {
        LOG_WARN("Heartbeat and fetch state thread not started, the state may expired!");
    }
    return devState_;
}

int DeviceMonitor::registerStateChangedCallback(DeviceStateChangedCallback callback) {
    std::lock_guard<std::mutex> lock(stateChangedCallbacksMutex_);
    auto                        callbackId = cbIdCounter_++;
    stateChangedCallbacks_[callbackId]     = callback;
    if(!heartbeatAndFetchStateThreadStarted_) {
        start();
    }
    return callbackId;
}

void DeviceMonitor::unregisterStateChangedCallback(int callbackId) {
    std::lock_guard<std::mutex> lock(stateChangedCallbacksMutex_);
    stateChangedCallbacks_.erase(callbackId);
    if(stateChangedCallbacks_.empty() && !heartbeatPaused_ && heartbeatAndFetchStateThreadStarted_) {
        stop();
    }
}

void DeviceMonitor::enableHeartbeat() {
    if(heartbeatEnabled_) {
        LOG_DEBUG("Heartbeat already enabled!");
        return;
    }
    auto owner        = getOwner();
    auto propAccessor = owner->getPropertyAccessor();
    propAccessor->setPropertyValueT(OB_PROP_HEARTBEAT_BOOL, true);
    heartbeatEnabled_ = true;
    heartbeatPaused_  = false;
    if(!heartbeatAndFetchStateThreadStarted_) {
        start();
    }
    LOG_DEBUG("Heartbeat enabled!");
}

void DeviceMonitor::disableHeartbeat() {
    if(!heartbeatEnabled_) {
        LOG_DEBUG("Heartbeat already disabled!");
        return;
    }
    auto owner        = getOwner();
    auto propAccessor = owner->getPropertyAccessor();
    propAccessor->setPropertyValueT(OB_PROP_HEARTBEAT_BOOL, false);
    heartbeatEnabled_ = false;
    heartbeatPaused_  = false;
    if(heartbeatAndFetchStateThreadStarted_) {
        stop();
    }
    LOG_DEBUG("Heartbeat disabled!");
}

void DeviceMonitor::pauseHeartbeat() {
    if(heartbeatPaused_) {
        LOG_DEBUG("Heartbeat already paused!");
        return;
    }
    auto owner        = getOwner();
    auto propAccessor = owner->getPropertyAccessor();
    propAccessor->setPropertyValueT(OB_PROP_HEARTBEAT_BOOL, false);
    if(heartbeatAndFetchStateThreadStarted_) {
        stop();
    }
    heartbeatPaused_ = true;
    LOG_DEBUG("Heartbeat paused!");
}

void DeviceMonitor::resumeHeartbeat() {
    if(!heartbeatPaused_) {
        LOG_DEBUG("Heartbeat already resumed!");
        return;
    }

    if(heartbeatEnabled_) {
        // resume heartbeat enable state if it was enabled before
        auto owner        = getOwner();
        auto propAccessor = owner->getPropertyAccessor();
        propAccessor->setPropertyValueT(OB_PROP_HEARTBEAT_BOOL, true);
    }

    if(heartbeatEnabled_ || !stateChangedCallbacks_.empty()) {
        // resuming heartbeat thread if heartbeat is enabled or if there are state changed callbacks
        start();
    }
    heartbeatPaused_ = false;
    LOG_DEBUG("Heartbeat resumed!");
}

const std::vector<uint8_t> &DeviceMonitor::sendAndReceiveData(const std::vector<uint8_t> &data, uint32_t exceptedRecvLen) {
    std::lock_guard<std::mutex> lock(commMutex_);
    recvData_.resize(exceptedRecvLen);
    auto recvLen = vendorDataPort_->sendAndReceive(data.data(), static_cast<uint32_t>(data.size()), recvData_.data(), exceptedRecvLen);
    recvData_.resize(recvLen);
    return recvData_;
}

}  // namespace libobsensor