// Copyright (c) Orbbec Inc. All Rights Reserved.
// Licensed under the MIT License.

#include "VendorTCPClient.hpp"
#include "logger/Logger.hpp"
#include "utils/Utils.hpp"
#include "exception/ObException.hpp"

#include <thread>

namespace libobsensor {

VendorTCPClient::VendorTCPClient(std::string address, uint16_t port, uint32_t connectTimeout, uint32_t commTimeout)
    : address_(address), port_(port), socketFd_(INVALID_SOCKET), flushed_(false), CONNECT_TIMEOUT_MS(connectTimeout), COMM_TIMEOUT_MS(commTimeout) {
#if(defined(WIN32) || defined(_WIN32) || defined(WINCE))
    WSADATA wsaData;
    int     rst = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if(rst != 0) {
        throw libobsensor::invalid_value_exception(utils::string::to_string() << "Failed to load Winsock! err_code=" << GET_LAST_ERROR());
    }
#endif
// Due to network configuration changes causing socket connection pipeline errors, macOS throws a SIGPIPE exception to the application, leading to a crash (this does not occur on Linux). This exception needs to be filtered out.
#if(defined(OS_IOS) || defined(OS_MACOS))
    signal(SIGPIPE, SIG_IGN);
#endif
    socketConnect();
}

VendorTCPClient::~VendorTCPClient() noexcept{
    socketClose();
#if(defined(WIN32) || defined(_WIN32) || defined(WINCE))
    WSACleanup();
#endif
}

void VendorTCPClient::socketConnect() {
    int rst;
    socketFd_ = socket(AF_INET, SOCK_STREAM, 0);  // ipv4, tcp(Streaming)
    if(socketFd_ == INVALID_SOCKET) {
        throw libobsensor::io_exception(utils::string::to_string() << "create socket failed! err_code=" << GET_LAST_ERROR());
    }

#if(defined(WIN32) || defined(_WIN32) || defined(WINCE))
    uint32_t commTimeout = COMM_TIMEOUT_MS;
#else
    TIMEVAL commTimeout;
    commTimeout.tv_sec  = COMM_TIMEOUT_MS / 1000;
    commTimeout.tv_usec = COMM_TIMEOUT_MS % 1000 * 1000;
#endif

    setsockopt(socketFd_, SOL_SOCKET, SO_SNDTIMEO, (char *)&commTimeout, sizeof(commTimeout));  // Send timeout limit
    setsockopt(socketFd_, SOL_SOCKET, SO_RCVTIMEO, (char *)&commTimeout, sizeof(commTimeout));  // Receive timeout limit

    // Adjust the window size to resolve the network retransmission issue at any frame rate @LingYi
    // Adjust the window size
    // int sendBufSize = 1024 * 1024 * 2;  // 2MB
    // rst = setsockopt(socketFd_, SOL_SOCKET, SO_SNDBUF, (char*)&sendBufSize, sizeof(sendBufSize));
    // Receive window size
    // int recvBufSize = 1024 * 1024 * 2;  // 2MB
    // rst = setsockopt(socketFd_, SOL_SOCKET, SO_RCVBUF, (char*)&recvBufSize, sizeof(recvBufSize));

    unsigned long mode = 1;  // non-blocking mode
    // Set to non-blocking; handle connect timeout
    rst = ioctlsocket(socketFd_, FIONBIO, &mode);
    if(rst < 0) {
        throw libobsensor::invalid_value_exception(utils::string::to_string() << "VendorTCPClient: ioctlsocket to non-blocking mode failed! addr=" << address_
                                                                              << ", port=" << port_ << ", err_code=" << GET_LAST_ERROR());
    }

    struct sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;                                       // ipv4
    serverAddr.sin_port   = htons(port_);                                  // convert uint16_t from host to network byte sequence
    if(inet_pton(AF_INET, address_.c_str(), &serverAddr.sin_addr) <= 0) {  // address string to sin_addr
        throw libobsensor::invalid_value_exception("Invalid address!");
    }

    rst = connect(socketFd_, (struct sockaddr *)&serverAddr, sizeof(serverAddr));
    if(rst < 0) {
        rst = GET_LAST_ERROR();
#if(defined(WIN32) || defined(_WIN32) || defined(WINCE))
        if(rst != WSAEWOULDBLOCK) {
#else
        if(rst != EINPROGRESS) {  // EINPROGRESS due to non-blocking mode
#endif
            throw libobsensor::invalid_value_exception(utils::string::to_string() << "VendorTCPClient: Connect to server failed! addr=" << address_
                                                                                  << ", port=" << port_ << ", err_code=" << rst);
        }
    }

    TIMEVAL connTimeout;
#if(defined(OS_IOS) || defined(OS_MACOS) || defined(__ANDROID__))
    connTimeout.tv_sec  = 0;
    connTimeout.tv_usec = 100000;  // 100ms
#else
    connTimeout.tv_sec  = CONNECT_TIMEOUT_MS / 1000;
    connTimeout.tv_usec = CONNECT_TIMEOUT_MS % 1000 * 1000;
#endif

    fd_set write, err;
    FD_ZERO(&write);
    FD_ZERO(&err);
    FD_SET(socketFd_, &write);
    FD_SET(socketFd_, &err);

#if(defined(OS_IOS) || defined(OS_MACOS) || defined(__ANDROID__))
    // Since the traditional `fcntl` cannot be used on iOS to set socketFd as non-blocking, leading to select blocking, reduce connTimeout and use polling to avoid the issue.
    bool status = false;
    int  retry  = 5;
    do {
        // check if the socket is ready
        rst    = select(0, NULL, &write, &err, &connTimeout);
        status = FD_ISSET(socketFd_, &write);
    } while(!status && retry-- > 0);

    if(!status) {
        throw libobsensor::invalid_value_exception(utils::string::to_string() << "VendorTCPClient: Connect to server failed! addr=" << address_
                                                                              << ", port=" << port_ << ", err=socket is not ready & timeout");
    }
#else
    // check if the socket is ready
    rst = select(0, NULL, &write, &err, &connTimeout);
    if(!FD_ISSET(socketFd_, &write)) {
        throw libobsensor::invalid_value_exception(utils::string::to_string() << "VendorTCPClient: Connect to server failed! addr=" << address_
                                                                              << ", port=" << port_ << ", err=socket is not ready & timeout");
    }
#endif

    // Restore to blocking mode
    mode = 0;  // blocking mode
    rst  = ioctlsocket(socketFd_, FIONBIO, &mode);
    if(rst < 0) {
        throw libobsensor::invalid_value_exception(utils::string::to_string() << "VendorTCPClient: ioctlsocket to blocking mode failed! addr=" << address_
                                                                              << ", port=" << port_ << ", err_code=" << GET_LAST_ERROR());
    }
    LOG_DEBUG("TCP client socket created!, addr={0}, port={1}, socket={2}", address_, port_, socketFd_);
}

void VendorTCPClient::socketClose() {
    if(socketFd_ > 0) {
        auto rst = closesocket(socketFd_);
        if(rst < 0) {
            LOG_ERROR("close socket failed! socket={0}, err_code={1}", socketFd_, GET_LAST_ERROR());
        }
    }
    LOG_DEBUG("TCP client socket closed! socket={}", socketFd_);
    socketFd_ = INVALID_SOCKET;
}

void VendorTCPClient::socketReconnect() {
    LOG_INFO("TCP client socket reconnecting...");
    socketClose();
    socketConnect();
}

int VendorTCPClient::read(uint8_t *data, const uint32_t dataLen) {
    uint8_t retry = 2;
    while(retry-- && !flushed_) {
        int rst = recv(socketFd_, (char *)data, dataLen, 0);
        if(rst < 0) {
            rst = GET_LAST_ERROR();
#if(defined(WIN32) || defined(_WIN32) || defined(WINCE))
            if((rst == WSAECONNRESET || rst == WSAENOTCONN || rst == WSAETIMEDOUT) && retry >= 1) {
#else
            if((rst == EAGAIN || rst == EWOULDBLOCK) && retry >= 1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            else if((rst == ENOTCONN || rst == ECONNRESET || rst == ENETRESET) && retry >= 1) {
#endif
                socketReconnect();
                return -1;
            }
            else {
                throw libobsensor::io_exception(utils::string::to_string() << "VendorTCPClient read data failed! socket=" << socketFd_ << ", err_code=" << rst);
            }
        }
        else {
            return rst;
        }
    }

    return -1;
}

void VendorTCPClient::write(const uint8_t *data, const uint32_t dataLen) {
    uint8_t retry = 2;
    while(retry-- && !flushed_) {
        int rst = send(socketFd_, (const char *)data, dataLen, 0);
        if(rst < 0) {
            rst = GET_LAST_ERROR();
#if(defined(WIN32) || defined(_WIN32) || defined(WINCE))
            if((rst == WSAECONNRESET || rst == WSAENOTCONN || rst == WSAETIMEDOUT) && retry >= 1) {
#else
            if((rst == EAGAIN || rst == EWOULDBLOCK) && retry >= 1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            else if((rst == ENOTCONN || rst == ECONNRESET || rst == ENETRESET) && retry >= 1) {
#endif
                socketReconnect();
            }
            else {
                throw libobsensor::io_exception(utils::string::to_string()
                                                << "VendorTCPClient write data failed! socket=" << socketFd_ << ", err_code=" << rst);
            }
            continue;
        }
        break;
    }
}

void VendorTCPClient::flush() {
    if(socketFd_) {
        flushed_ = true;
        shutdown(socketFd_, SD_BOTH);
        socketClose();
    }
}


}  // namespace libobsensor

