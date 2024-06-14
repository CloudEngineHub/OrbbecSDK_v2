
#if(defined(WIN32) || defined(_WIN32) || defined(WINCE))
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "Iphlpapi.lib")
#else
#include <netdb.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/time.h>
#endif

#include <thread>
#include <algorithm>

#include "GVCPClient.hpp"
#include "exception/ObException.hpp"

#include "logger/LoggerInterval.hpp"
#include "logger/LoggerHelper.hpp"

namespace libobsensor {


GVCPClient::GVCPClient() {
#if(defined(WIN32) || defined(_WIN32) || defined(WINCE))
    WSADATA wsaData;
    if(WSAStartup(MAKEWORD(2, 2), &wsaData)) {
        throw libobsensor::invalid_value_exception(utils::to_string() << "Failed to initialize WinSock! err_code=" << GET_LAST_ERROR());
    }
#endif
    openClientSockets();
}

GVCPClient::~GVCPClient() {
    std::lock_guard<std::mutex> lck(queryMtx_);
#if(defined(WIN32) || defined(_WIN32) || defined(WINCE))
    WSACleanup();
#endif
    closeClientSockets();
}

std::vector<NetDeviceInfo> GVCPClient::queryNetDeviceList() {
    std::lock_guard<std::mutex> lck(queryMtx_);
    devInfoList_.clear();

    checkAndUpdateSockets();
    std::vector<std::thread> threads;
    for(int i = 0; i < sockCount_; ++i) {
        auto func = std::bind(&GVCPClient::sendGVCPDiscovery, this, socks_[i]);
        threads.emplace_back(func, socks_[i]);
    }

    for(auto &thread: threads) {
        thread.join();
    }

    // devInfoList_去重
    if(!devInfoList_.empty()) {
        auto iter = devInfoList_.begin();
        while(iter != devInfoList_.end()) {
            auto it = std::find_if(iter + 1, devInfoList_.end(), [&](const NetDeviceInfo &other) { return *iter == other; });
            if(it != devInfoList_.end()) {
                devInfoList_.erase(it);
                continue;
            }
            iter++;
        }
    }

    LOG_TRACE("queryNetDevice completed ({}):", devInfoList_.size());
    for(auto &&info: devInfoList_) {
        // LOG_INFO("\t- mac:{}, ip:{}, sn:{}, pid:0x{:04x}", info.mac, info.ip, info.sn, info.pid);
        LOG_INTVL(LOG_INTVL_OBJECT_TAG + "queryNetDevice", DEF_MIN_LOG_INTVL, spdlog::level::info, "\t- mac:{}, ip:{}, sn:{}, pid:0x{:04x}", info.mac, info.ip,
                  info.sn, info.pid);
    }
    return devInfoList_;
}

bool GVCPClient::changeNetDeviceIpConfig(std::string mac, const OBNetIpConfig &config) {
    std::lock_guard<std::mutex> lck(queryMtx_);

    std::vector<std::thread> threads;

    for(int i = 0; i < sockCount_; ++i) {
        auto func = std::bind(&GVCPClient::sendGVCPForceIP, this, socks_[i], mac, config);
        threads.emplace_back(func, socks_[i]);
    }

    for(auto &thread: threads) {
        thread.join();
    }
    LOG_INFO("change net device ip config completed. mac:{}", mac);
    return true;
}

int GVCPClient::openClientSockets() {
#if(defined(WIN32) || defined(_WIN32) || defined(WINCE))
    DWORD                       rv, size;
    PIP_ADAPTER_ADDRESSES       adapter_addresses, aa;
    PIP_ADAPTER_UNICAST_ADDRESS ua;

    rv = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, NULL, NULL, &size);
    if(rv != ERROR_BUFFER_OVERFLOW) {
        fprintf(stderr, "GetAdaptersAddresses() failed...");
        return -1;
    }
    adapter_addresses = (PIP_ADAPTER_ADDRESSES)malloc(size);

    rv = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, NULL, adapter_addresses, &size);
    if(rv != ERROR_SUCCESS) {
        fprintf(stderr, "GetAdaptersAddresses() failed...");
        free(adapter_addresses);
        return -1;
    }

    int index = 0;

    for(aa = adapter_addresses; aa != NULL; aa = aa->Next) {
        for(ua = aa->FirstUnicastAddress; ua != NULL; ua = ua->Next) {
            SOCKADDR_IN addrSrv;
            addrSrv            = *(SOCKADDR_IN *)ua->Address.lpSockaddr;
            addrSrv.sin_family = AF_INET;
            addrSrv.sin_port   = htons(0);
            auto ipStr         = inet_ntoa(addrSrv.sin_addr);
            if(strncmp(ipStr, "169.254", 7) == 0 || strcmp(ipStr, "127.0.0.1") == 0) {
                continue;
            }
            socks_[index++] = openClientSocket(addrSrv);
        }
    }
    sockCount_ = index;
#else
    struct ifaddrs *ifaddr, *ifa;
    int             family, s, n;
    char            host[NI_MAXHOST];

    if(getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        exit(EXIT_FAILURE);
    }

    int index = 0;

    for(ifa = ifaddr, n = 0; ifa != NULL; ifa = ifa->ifa_next, n++) {
        if(ifa->ifa_addr == NULL)
            continue;

        family = ifa->ifa_addr->sa_family;

        if(family == AF_INET) {
            s = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
            if(s != 0) {
                LOG_TRACE("getnameinfo() failed: {}", gai_strerror(s));
                exit(EXIT_FAILURE);
            }

            SOCKADDR_IN addrSrv;
            addrSrv            = *(SOCKADDR_IN *)ifa->ifa_addr;
            addrSrv.sin_family = AF_INET;
            addrSrv.sin_port   = htons(0);
            auto ipStr         = inet_ntoa(addrSrv.sin_addr);
            if(strcmp(ipStr, "127.0.0.1") == 0) {
                continue;
            }
            ipAddressStrSet_.insert(ipStr);
            socks_[index++] = openClientSocket(addrSrv);
        }
    }
    sockCount_ = index;
    freeifaddrs(ifaddr);
#endif

    return sockCount_;
}

void GVCPClient::closeClientSockets() {
    for(int i = 0; i < sockCount_; i++) {
        closesocket(socks_[i]);
    }
}

SOCKET GVCPClient::openClientSocket(SOCKADDR_IN addr) {
    // 创建套接字
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if(sock == INVALID_SOCKET) {
        throw libobsensor::invalid_value_exception(utils::to_string() << "Failed to create socket! err_code=" << GET_LAST_ERROR());
    }

    // 设置广播选项
    int bBroadcast = 1;
#if(defined(WIN32) || defined(_WIN32) || defined(WINCE))
    int err = setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (char *)&bBroadcast, sizeof(bBroadcast));
#else
    // int     err = setsockopt(sock, SOL_SOCKET, SO_BROADCAST | SO_REUSEADDR, (char *)&bBroadcast, sizeof(bBroadcast));
    int err = setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (char *)&bBroadcast, sizeof(bBroadcast));
    if(err == SOCKET_ERROR) {
        throw libobsensor::invalid_value_exception(utils::to_string() << "Failed to set socket boardcast option! err_code=" << GET_LAST_ERROR());
    }
    err = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&bBroadcast, sizeof(bBroadcast));
    if(err == SOCKET_ERROR) {
        throw libobsensor::invalid_value_exception(utils::to_string() << "Failed to set socket reuseaddr option! err_code=" << GET_LAST_ERROR());
    }
#endif

// 设置接收超时选项
#if(defined(WIN32) || defined(_WIN32) || defined(WINCE))
    uint32_t dwTimeout = 5000;
#else
    TIMEVAL dwTimeout;
    dwTimeout.tv_sec  = 5;
    dwTimeout.tv_usec = 0;
#endif
    err = setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&dwTimeout, sizeof(dwTimeout));
    if(err == SOCKET_ERROR) {
        throw libobsensor::invalid_value_exception(utils::to_string() << "Failed to set socket timeout option! err_code=" << GET_LAST_ERROR());
    }

#if(defined(__linux__) || defined(OS_IOS) || defined(OS_MACOS) || defined(__ANDROID__))
    addr.sin_addr.s_addr = inet_addr("0.0.0.0");
#endif
    LOG_INTVL(LOG_INTVL_OBJECT_TAG + "GVCP bind", MAX_LOG_INTERVAL, spdlog::level::debug, "bind {}:{}", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
    err = bind(sock, (SOCKADDR *)&addr, sizeof(SOCKADDR));
    if(err == SOCKET_ERROR) {
        return 0;
    }

    return sock;
}

void GVCPClient::sendGVCPDiscovery(SOCKET sock) {
    LOG_TRACE("send gvcp discovery {}", sock);
    gvcp_discover_cmd discoverCmd;
    gvcp_discover_ack discoverAck;

    SOCKADDR_IN destAddr;
    destAddr.sin_family      = AF_INET;
    destAddr.sin_addr.s_addr = INADDR_BROADCAST;  // 广播地址
    destAddr.sin_port        = htons(GVCP_PORT);

    // 设备发现
    gvcp_cmd_header cmdHeader;
    cmdHeader.cMsgKeyCode = GVCP_VERSION;
    cmdHeader.cFlag       = GVCP_DISCOVERY_FLAGS;
    cmdHeader.wCmd        = htons(GVCP_DISCOVERY_CMD);
    cmdHeader.wLen        = htons(0);
    cmdHeader.wReqID      = htons(1);

    discoverCmd.header = cmdHeader;

    ////获取本地的ip
    // struct sockaddr_in addr;
    // socklen_t          addrLen = sizeof(addr);

    //// 获取套接字的地址信息
    // if(getsockname(sock, (struct sockaddr *)&addr, &addrLen) == 0) {
    //     LOG_INFO("cur addr {}:{}", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
    // }

    // 发送数据
    int err = sendto(sock, (const char *)&discoverCmd, sizeof(discoverCmd), 0, (SOCKADDR *)&destAddr, sizeof(destAddr));
    if(err == SOCKET_ERROR) {
        LOG_INTVL(LOG_INTVL_OBJECT_TAG + "GVCP sendto", MAX_LOG_INTERVAL, spdlog::level::debug, "sendto failed with error:{}", GET_LAST_ERROR());
    }
    // LOG_INFO("sendto get info with error:{}", GET_LAST_ERROR());
    char recvBuf[1024];
    memset(recvBuf, 0, sizeof(recvBuf));

    int res;
    int failedCount = 100;
    do {
        struct timeval timeout;
        timeout.tv_sec  = 1;
        timeout.tv_usec = 0;

        int    nfds = 0;
        fd_set readfs;
        FD_ZERO(&readfs);
        nfds = sock + 1;
        FD_SET(sock, &readfs);

        res = select(nfds, &readfs, 0, 0, &timeout);
        if(res > 0) {
            if(FD_ISSET(sock, &readfs)) {
                // 接收数据
                SOCKADDR_IN srcAddr;
                socklen_t   srcAddrLen = sizeof(srcAddr);

                err = recvfrom(sock, recvBuf, sizeof(recvBuf), 0, (SOCKADDR *)&srcAddr, &srcAddrLen);
                if(err == SOCKET_ERROR) {
                    LOG_INTVL(LOG_INTVL_OBJECT_TAG + "GVCP recvfrom", DEF_MIN_LOG_INTVL, spdlog::level::err, "recvfrom failed with error: {}",
                              GET_LAST_ERROR());

                    if(failedCount-- < 0) {
                        LOG_WARN("GVCP recvfrom failed!!!");
                        break;
                    }
                }

                // 解析响应数据
                gvcp_ack_header ackHeader = {};
                memcpy(&ackHeader, recvBuf, sizeof(gvcp_ack_header));

                uint16_t status = ntohs(ackHeader.wStatus);
                uint16_t ack    = ntohs(ackHeader.wAck);
                uint16_t len    = ntohs(ackHeader.wLen);
                uint16_t reqID  = ntohs(ackHeader.wReqID);

                LOG_INTVL(LOG_INTVL_OBJECT_TAG + "GVCP get info", DEF_MIN_LOG_INTVL, spdlog::level::info, "{}, {}, {}, {}", status, ack, len, reqID);

                discoverAck.header = ackHeader;

                if(status == GEV_STATUS_SUCCESS && ack == GVCP_DISCOVERY_ACK && reqID == GVCP_REQUEST_ID) {
                    gvcp_ack_payload ackPayload = {};
                    memcpy(&ackPayload, recvBuf + sizeof(gvcp_ack_header), sizeof(gvcp_ack_payload));

                    uint16_t specVer  = ntohl(ackPayload.dwSpecVer);
                    uint16_t devMode  = ntohl(ackPayload.dwDevMode);
                    uint16_t supIpSet = ntohl(ackPayload.dwSupIpSet);
                    uint16_t curIpSet = ntohl(ackPayload.dwCurIpSet);
                    uint32_t curPID   = ntohl(ackPayload.dwPID);

                    // 获取Mac地址
                    char macStr[18];
                    sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X", ackPayload.Mac[2], ackPayload.Mac[3], ackPayload.Mac[4], ackPayload.Mac[5],
                            ackPayload.Mac[6], ackPayload.Mac[7]);

                    // 读取 CurIP 字段
                    uint32_t curIP = *((uint32_t *)&ackPayload.CurIP[12]);
                    char     curIPStr[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &curIP, curIPStr, INET_ADDRSTRLEN);

                    // 读取 SubMask 字段
                    uint32_t subMask = *((uint32_t *)&ackPayload.SubMask[12]);
                    char     subMaskStr[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &subMask, subMaskStr, INET_ADDRSTRLEN);

                    // 读取 Gateway 字段
                    uint32_t gateway = *((uint32_t *)&ackPayload.Gateway[12]);
                    char     gatewayStr[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &gateway, gatewayStr, INET_ADDRSTRLEN);

                    // LOG_INFO("{},{}, {},{}, {},{}, {}, {}, {}, {}, {}, {}, {}, {}, {}", specVer, devMode, macStr, supIpSet, curIpSet, curPID, curIPStr,
                    //          subMaskStr, gatewayStr, ackPayload.szFacName, ackPayload.szModelName, ackPayload.szDevVer, ackPayload.szFacInfo,
                    //          ackPayload.szSerial, ackPayload.szUserName);

                    LOG_INTVL(LOG_INTVL_OBJECT_TAG + "GVCP get ack info", DEF_MIN_LOG_INTVL, spdlog::level::info,
                              "{},{}, {},{}, {},{}, {}, {}, {}, {}, {}, {}, {}, {}, {}", specVer, devMode, std::string(macStr), supIpSet, curIpSet, curPID,
                              std::string(curIPStr), std::string(subMaskStr), std::string(gatewayStr), std::string(ackPayload.szFacName),
                              std::string(ackPayload.szModelName), std::string(ackPayload.szDevVer), std::string(ackPayload.szFacInfo),
                              std::string(ackPayload.szSerial), std::string(ackPayload.szUserName));

                    discoverAck.payload = ackPayload;

                    // 过滤非Orbbec设备
                    if(strcmp(ackPayload.szFacName, "Orbbec") != 0)
                        continue;

                    NetDeviceInfo info;
                    info.mac     = macStr;
                    info.ip      = curIPStr;
                    info.mask    = subMaskStr;
                    info.gateway = gatewayStr;
                    info.sn      = ackPayload.szSerial;
                    info.name    = ackPayload.szModelName;
                    info.pid     = curPID;
                    // info.manufacturer = ackPayload.szFacName;
                    // info.version      = ackPayload.szDevVer;

                    std::lock_guard<std::mutex> lock(devInfoListMtx_);
                    devInfoList_.push_back(info);
                }
            }
            FD_SET(sock, &readfs);
        }
    } while(res > 0);
    // LOG_INFO("finish gvcp discovery {}", sock);
}

void GVCPClient::sendGVCPForceIP(SOCKET sock, std::string mac, const OBNetIpConfig &config) {
    gvcp_forceip_cmd forceIPCmd;
    // gvcp_forceip_ack forceIPAck;

    SOCKADDR_IN destAddr;
    destAddr.sin_family      = AF_INET;
    destAddr.sin_addr.s_addr = INADDR_BROADCAST;  // 广播地址
    destAddr.sin_port        = htons(GVCP_PORT);

    gvcp_cmd_header cmdHeader = {};
    cmdHeader.cMsgKeyCode     = GVCP_VERSION;
    cmdHeader.cFlag           = GVCP_FORCEIP_FLAGS;
    cmdHeader.wCmd            = htons(GVCP_FORCEIP_CMD);
    cmdHeader.wLen            = htons(sizeof(gvcp_forceip_payload));
    cmdHeader.wReqID          = htons(1);

    forceIPCmd.header = cmdHeader;

    gvcp_forceip_payload payload = {};
    sscanf(mac.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &payload.Mac[2], &payload.Mac[3], &payload.Mac[4], &payload.Mac[5], &payload.Mac[6], &payload.Mac[7]);
    // memcpy(&payload.Mac[2], &discoverAck.payload.Mac[2], 6);

    // 要修改的IP
    // char m_szLocalIp[32];
    // strcpy(m_szLocalIp, config.address);
    // char m_szLocalMask[32];
    // strcpy(m_szLocalMask, config.mask);
    // char m_szLocalGateway[32];
    // strcpy(m_szLocalGateway, config.gateway);

    // *((uint32_t *)&payload.CurIP[12])   = inet_addr(m_szLocalIp);       // last 4 byte
    // *((uint32_t *)&payload.SubMask[12]) = inet_addr(m_szLocalMask);     // last 4 byte
    // *((uint32_t *)&payload.Gateway[12]) = inet_addr(m_szLocalGateway);  // last 4 byte

    memcpy(payload.CurIP, config.address, 4);
    memcpy(payload.SubMask, config.mask, 4);
    memcpy(payload.Gateway, config.gateway, 4);

    forceIPCmd.payload = payload;

    // 发送数据
    int err = sendto(sock, (const char *)&forceIPCmd, sizeof(forceIPCmd), 0, (SOCKADDR *)&destAddr, sizeof(destAddr));
    if(err == SOCKET_ERROR) {
        LOG_TRACE("sendto failed with error: {}", GET_LAST_ERROR());
    }
}

void GVCPClient::checkAndUpdateSockets() {
#if(defined(WIN32) || defined(_WIN32) || defined(WINCE))
    DWORD                       rv, size;
    PIP_ADAPTER_ADDRESSES       adapter_addresses, aa;
    PIP_ADAPTER_UNICAST_ADDRESS ua;

    rv = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, NULL, NULL, &size);
    if(rv != ERROR_BUFFER_OVERFLOW) {
        LOG_ERROR("GetAdaptersAddresses() failed...");
        return;
    }
    adapter_addresses = (PIP_ADAPTER_ADDRESSES)malloc(size);

    rv = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, NULL, adapter_addresses, &size);
    if(rv != ERROR_SUCCESS) {
        LOG_ERROR("GetAdaptersAddresses() failed...");
        free(adapter_addresses);
        return;
    }

    int index = sockCount_;

    for(aa = adapter_addresses; aa != NULL; aa = aa->Next) {
        for(ua = aa->FirstUnicastAddress; ua != NULL; ua = ua->Next) {
            SOCKADDR_IN addrSrv;
            addrSrv            = *(SOCKADDR_IN *)ua->Address.lpSockaddr;
            addrSrv.sin_family = AF_INET;
            addrSrv.sin_port   = htons(0);
            std::string ipStr  = inet_ntoa(addrSrv.sin_addr);

            if(strncmp(ipStr.c_str(), "169.254", 7) == 0 || strcmp(ipStr.c_str(), "127.0.0.1") == 0) {
                continue;
            }

            bool found = false;
            for(auto sock: socks_) {
                if(sock == 0) {
                    continue;
                }

                SOCKADDR_IN addr;
                socklen_t   addrLen = sizeof(addr);
                if(getsockname(sock, (struct sockaddr *)&addr, &addrLen) == 0) {
                    std::string sockIp = inet_ntoa(addr.sin_addr);
                    if(ipStr == sockIp) {
                        found = true;
                        break;
                    }
                }
                else {
                    LOG_INFO("get socket ip addr failed,{}", ipStr);
                }
            }

            if(!found) {
                int socketFd = openClientSocket(addrSrv);
                if(socketFd != 0) {
                    socks_[index++] = socketFd;
                    LOG_INFO("new ip segment found,new ip addr:{}", ipStr);
                }
            }
        }
    }
    sockCount_ = index;
#else
    struct ifaddrs *ifaddr, *ifa;
    int             family, s, n;
    char            host[NI_MAXHOST];

    if(getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        exit(EXIT_FAILURE);
    }

    int index = sockCount_;

    for(ifa = ifaddr, n = 0; ifa != NULL; ifa = ifa->ifa_next, n++) {
        if(ifa->ifa_addr == NULL)
            continue;

        family = ifa->ifa_addr->sa_family;

        if(family == AF_INET) {
            s = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
            if(s != 0) {
                LOG_TRACE("getnameinfo() failed: {}", gai_strerror(s));
                exit(EXIT_FAILURE);
            }

            SOCKADDR_IN addrSrv;
            addrSrv            = *(SOCKADDR_IN *)ifa->ifa_addr;
            addrSrv.sin_family = AF_INET;
            addrSrv.sin_port   = htons(0);
            std::string ipStr  = inet_ntoa(addrSrv.sin_addr);
            if(strcmp(ipStr.c_str(), "127.0.0.1") == 0) {
                continue;
            }

            bool found = false;
            for(auto sockIp: ipAddressStrSet_) {
                if(ipStr == sockIp) {
                    found = true;
                    break;
                }
            }

            if(!found) {
                closeClientSockets();
                index           = 0;
                socks_[index++] = openClientSocket(addrSrv);
                ipAddressStrSet_.insert(ipStr);
                LOG_INFO("new ip segment found,new ip addr:{}", ipStr);
            }
        }
    }
    sockCount_ = index;
    freeifaddrs(ifaddr);
#endif
}


}  // namespace libobsensor
