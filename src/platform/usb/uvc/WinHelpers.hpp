// Copyright (c) Orbbec Inc. All Rights Reserved.
// Licensed under the MIT License.

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "usb/enumerator/UsbTypes.hpp"

#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cfgmgr32.h>

#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "setupapi.lib")

#define WAIT_FOR_MUTEX_TIME_OUT (5000)

namespace libobsensor {


#define CHECK_HR(x) check(#x, x);
#define LOG_HR(x) check(#x, x, false);

std::string wcharToString(const wchar_t *wText);

bool check(const char *call, HRESULT hr, bool to_throw = true);

std::string win_to_utf(const WCHAR *s);

bool is_win10_redstone2();

std::vector<std::string> tokenize(std::string string, char separator);

bool parse_usb_path_multiple_interface(uint16_t &vid, uint16_t &pid, uint16_t &mi, std::string &uid, const std::string &path, std::string &device_guid);
bool parse_usb_path_single_interface(uint16_t &vid, uint16_t &pid, std::string &serial, const std::string &path);
bool getUsbDescriptors(uint16_t device_vid, uint16_t device_pid, const std::string &device_uid, std::string &location, UsbSpec &spec, std::string &serial,
                       std::string &url);


/*
    Configuration Management (CM) tree node.
    See https://docs.microsoft.com/en-gb/windows/win32/api/cfgmgr32/

    A basic wrapper around DEVINST and the functionality we need around it, for ease of use.
*/
class cm_node {
    DEVINST _devinst;

public:
    cm_node() : _devinst(0) {}
    cm_node(DEVINST devinst) : _devinst(devinst) {}

    static cm_node from_instance_id(std::wstring const &instance_id);
    static cm_node from_device_path(LPCWSTR device_path);
    static cm_node root();

    DEVINST get() const {
        return _devinst;
    }
    bool valid() const {
        return get() != 0;
    }

    operator DEVINST() const {
        return _devinst;
    }

    operator DEVINST *() {
        return &_devinst;
    }
    operator DEVINST const *() const {
        return &_devinst;
    }

    std::string get_id() const;
    std::string get_uid() const;

    cm_node get_parent() const;
    cm_node get_sibling() const;
    cm_node get_child() const;

    std::string get_property(DEVPROPKEY const &property) const;

    /*
        Iterate through all nodes (children, grandchildren, etc.) under this one.
        A parent is visited before any children.
        An action (function) is run for each, which can return false to stop iteration:

        cm_node::root().foreach_node(
                [&]( cm_node device, size_t depth ) -> bool
                {
                    ...
                } );

        Returns whether iteration was stopped. False if the whole tree was finished.
    */
    template <class F> bool foreach_node(F fn, size_t depth = 1) {
        auto node = get_child();
        while(node.valid()) {
            if(!fn(node, depth))
                return true;
            if(node.foreach_node(fn, depth + 1))
                return true;
            node = node.get_sibling();
        }
        return false;
    }
};

class event_base {
public:
    virtual ~event_base() noexcept;
    virtual bool set();
    virtual bool wait(DWORD timeout) const;

    static event_base *wait(const std::vector<event_base *> &events, bool waitAll, int timeout);
    static event_base *wait_any(const std::vector<event_base *> &events, int timeout);
    static event_base *wait_all(const std::vector<event_base *> &events, int timeout);

    HANDLE get_handle() const {
        return _handle;
    }

protected:
    explicit event_base(HANDLE handle);

    HANDLE _handle;

private:
    event_base() = delete;

    // Disallow copy:
    event_base(const event_base &)            = delete;
    event_base &operator=(const event_base &) = delete;
};

class auto_reset_event : public event_base {
public:
    auto_reset_event();
};

class manual_reset_event : public event_base {
public:
    manual_reset_event();

    bool reset() const;
};

enum create_and_open_status { Mutex_Succeed, Mutex_TotalFailure, Mutex_AlreadyExist };


class winapi_error : public std::runtime_error {
public:
    explicit winapi_error(const char *message);

    static std::string last_error_string(DWORD lastError);
    static std::string generate_message(const std::string &message);
};

}  // namespace libobsensor

