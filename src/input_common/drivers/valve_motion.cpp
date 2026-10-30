// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>

#include "common/controller_console_state.h"
#include "common/logging.h"
#include "common/settings.h"
#include "input_common/drivers/valve_motion.h"

#ifdef __linux__
#include <fcntl.h>
#include <linux/hidraw.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace InputCommon {

namespace {

constexpr u16 VendorValve = 0x28DE;

enum Family : int {
    FamilyNone = 0,
    FamilyDeck = 1,         ///< Steam Deck: unnumbered reports, header type 9
    FamilyController = 2,   ///< Steam Controller: unnumbered reports, header type 1
    FamilyController26 = 3, ///< Steam Controller (2026): numbered reports 0x42, 0x45, 0x47
};

[[maybe_unused]] s16 ReadS16(const u8* data, std::size_t at) {
    return static_cast<s16>(data[at] | (data[at + 1] << 8));
}

} // Anonymous namespace

int ValveMotion::Family(u16 product) {
    switch (product) {
    case 0x1205:
        return FamilyDeck;
    case 0x1102: // wired
    case 0x1142: // wireless dongle
        return FamilyController;
    case 0x1302: // wired
    case 0x1304: // wireless dongle (Proteus)
    case 0x1305: // wireless dongle (Nereid)
        return FamilyController26;
    default:
        return FamilyNone;
    }
}

bool ValveMotion::Enabled() {
#ifdef __linux__
    const auto mode = Settings::values.controller_applet_mode.GetValue();
    const bool automatic = mode == Settings::AppletMode::ConsoleMode ||
                           mode == Settings::AppletMode::HandheldConsoleMode;
    // (Bypass Steam Input gives SDL the controllers, motion included: nothing to read here)
    static const bool enabled = Common::RunningUnderSteam() && automatic &&
                                Settings::values.console_valve_reader.GetValue() &&
                                !Settings::values.console_bypass_steam.GetValue();
    return enabled;
#else
    return false;
#endif
}

ValveMotion::ValveMotion(std::string input_engine_) : InputEngine(std::move(input_engine_)) {
    if (!Enabled()) {
        return;
    }
    LOG_INFO(Input, "Valve controllers' motion read directly (experimental)");
    scanner = std::jthread([this](std::stop_token stop) { Scan(stop); });
}

ValveMotion::~ValveMotion() = default;

std::vector<ValveMotion::Device> ValveMotion::GetDevices() const {
    std::scoped_lock lock{mutex};
    std::vector<Device> out;
    const auto now = std::chrono::steady_clock::now();
    for (const auto& [path, device] : devices) {
        // (A controller reports hundreds of times a second while on)
        if (now - device.seen < std::chrono::seconds{2}) {
            out.push_back(device);
        }
    }
    return out;
}

void ValveMotion::Scan(std::stop_token stop) {
#ifdef __linux__
    namespace fs = std::filesystem;
    while (!stop.stop_requested()) {
        std::error_code error;
        for (const auto& entry : fs::directory_iterator{"/sys/class/hidraw", error}) {
            std::ifstream uevent{entry.path() / "device" / "uevent"};
            unsigned bus = 0, vendor = 0, product = 0;
            for (std::string line; std::getline(uevent, line);) {
                if (line.starts_with("HID_ID=")) {
                    std::sscanf(line.c_str() + 7, "%x:%x:%x", &bus, &vendor, &product);
                }
            }
            if (vendor != VendorValve || Family(static_cast<u16>(product)) == FamilyNone) {
                continue;
            }
            const std::string path = "/dev/" + entry.path().filename().string();
            std::scoped_lock lock{mutex};
            if (reading.insert(path).second) {
                const std::size_t port = next_port++;
                readers.emplace_back([this, path, port, product](std::stop_token reader_stop) {
                    Read(reader_stop, path, port, static_cast<u16>(product));
                });
            }
        }
        for (int i = 0; i < 20 && !stop.stop_requested(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
    }
#endif
}

void ValveMotion::Read(std::stop_token stop, std::string path, std::size_t port, u16 product) {
#ifdef __linux__
    const int family = Family(product);
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        std::scoped_lock lock{mutex};
        reading.erase(path);
        return;
    }
    const PadIdentifier identifier{.guid = Common::UUID{}, .port = port, .pad = 0};
    bool known = false;
    int silent = 0;
    auto last_sample = std::chrono::steady_clock::time_point{};
    auto last_request = std::chrono::steady_clock::time_point{};
    std::array<u8, 128> report{};
    while (!stop.stop_requested()) {
        pollfd waiting{.fd = fd, .events = POLLIN, .revents = 0};
        if (::poll(&waiting, 1, 200) <= 0) {
            continue;
        }
        const ssize_t size = ::read(fd, report.data(), report.size());
        if (size <= 0) {
            break; // (unplugged, or turned off)
        }
        const u8* data = report.data();
        std::size_t accel = 0;
        std::size_t gyro = 0;
        bool held = false;
        bool flip_gyro_y = false;
        u8 feature_report_id = 0;
        if (family == FamilyDeck && size >= 56 && data[0] == 0x01 && data[2] == 0x09) {
            accel = 24;
            gyro = 30;
            for (std::size_t i = 8; i < 16; ++i) {
                held |= data[i] != 0;
            }
        } else if (family == FamilyController && size >= 40 && data[0] == 0x01 && data[2] == 0x01) {
            accel = 28;
            gyro = 34;
            // (Bytes 11 and 12 are the triggers)
            for (const std::size_t i : {8, 9, 10, 13, 14, 15}) {
                held |= data[i] != 0;
            }
            flip_gyro_y = true;
        } else if (family == FamilyController26 && size >= 46 &&
                   (data[0] == 0x42 || data[0] == 0x45 || data[0] == 0x47)) {
            accel = 34;
            gyro = 40;
            held = (data[2] | data[3] | data[4] | data[5]) != 0;
            feature_report_id = 0x01;
        } else {
            continue; // (another interface's report)
        }
        if (!known) {
            known = true;
            PreSetController(identifier);
            PreSetMotion(identifier, 0);
            LOG_INFO(Input, "Valve controller {:04x}:{:04x} read as port {} ({})", VendorValve,
                     product, port, path);
        }

        // As SDL reads them (2000 deg/s and 2 g full scale), in the units and axes Eden's SDL
        // motion has: rotations per second, and g
        constexpr float gyro_scale = 2000.0f / 360.0f / 32768.0f;
        constexpr float accel_scale = 2.0f / 32768.0f;
        BasicMotion motion{};
        motion.gyro_x = ReadS16(data, gyro) * gyro_scale;
        motion.gyro_y = ReadS16(data, gyro + 2) * gyro_scale * (flip_gyro_y ? -1.0f : 1.0f);
        motion.gyro_z = ReadS16(data, gyro + 4) * gyro_scale;
        motion.accel_x = -ReadS16(data, accel) * accel_scale;
        motion.accel_y = -ReadS16(data, accel + 2) * accel_scale;
        motion.accel_z = -ReadS16(data, accel + 4) * accel_scale;
        const bool zero = motion.accel_x == 0 && motion.accel_y == 0 && motion.accel_z == 0;

        const auto now = std::chrono::steady_clock::now();
        {
            std::scoped_lock lock{mutex};
            auto& device = devices[path];
            device.port = port;
            device.path = path;
            device.product = product;
            device.button_held = held;
            device.moving = !zero;
            device.seen = now;
        }

        // Silent (Steam keeps the sensors off unless its layout uses the gyro): ask for them,
        // motion setting only, at most every 2 seconds
        silent = zero ? silent + 1 : 0;
        if (silent >= 50 && now - last_request > std::chrono::seconds{2}) {
            last_request = now;
            if (const int control = ::open(path.c_str(), O_RDWR | O_CLOEXEC); control >= 0) {
                std::array<u8, 65> request{feature_report_id, 0x87, 0x03, 0x30, 0x18, 0x00};
                const std::size_t length = feature_report_id ? 64 : 65;
                const bool sent = ioctl(control, HIDIOCSFEATURE(length), request.data()) >= 0;
                ::close(control);
                if (Common::ControllerProbes()) {
                    LOG_INFO(Input, "Valve controller port {} silent: motion asked for ({})", port,
                             sent ? "sent" : "failed");
                }
            }
        }
        if (zero) {
            last_sample = {}; // (the first sample after a silence has no time step)
            continue;
        }
        if (last_sample.time_since_epoch().count() != 0) {
            motion.delta_timestamp = static_cast<u64>(
                std::chrono::duration_cast<std::chrono::microseconds>(now - last_sample).count());
            SetMotion(identifier, 0, motion);
        }
        last_sample = now;
    }
    ::close(fd);
    // (Only a controller interface was a device: the keyboard and mouse ones never were)
    if (known) {
        LOG_INFO(Input, "Valve controller port {} gone ({})", port, path);
    }
    std::scoped_lock lock{mutex};
    devices.erase(path);
    reading.erase(path);
#endif
}

} // namespace InputCommon
