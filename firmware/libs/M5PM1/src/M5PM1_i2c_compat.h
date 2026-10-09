/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef __M5PM1_I2C_COMPAT_H__
#define __M5PM1_I2C_COMPAT_H__

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// ============================
// I2C 延迟配置 (可调整以解决总线冲突)
// I2C Delay Configuration (adjustable to resolve bus conflicts)
// 适用于所有平台和驱动 (Arduino Wire / M5Unified / i2c_master / i2c_bus / Legacy)
// Applies to all platforms and drivers (Arduino Wire / M5Unified / i2c_master / i2c_bus / Legacy)
// ============================
#ifndef M5PM1_I2C_WRITE_DELAY_US
#define M5PM1_I2C_WRITE_DELAY_US 500  // 写操作后延迟(微秒) / Delay after write (microseconds)
#endif

#ifndef M5PM1_I2C_READ_DELAY_US
#define M5PM1_I2C_READ_DELAY_US 500  // 读操作前/后延迟(微秒) / Delay around read (microseconds)
#endif

// 微秒级延迟宏 (平台无关) / Microsecond delay macro (platform-agnostic)
#ifdef ARDUINO
#define M5PM1_I2C_DELAY_US(us) delayMicroseconds(us)
#else
#define M5PM1_I2C_DELAY_US(us) esp_rom_delay_us(us)
#endif

#ifdef ARDUINO

#include "Wire.h"

// ============================
// Arduino I2C 功能
// Arduino I2C Functions
// ============================

#ifndef M5PM1_I2C_ARDUINO_READ_BYTE
static inline bool M5PM1_I2C_ARDUINO_READ_BYTE(TwoWire *wire, uint8_t addr, uint8_t reg, uint8_t *data)
{
    wire->beginTransmission(addr);
    wire->write(reg);
    if (wire->endTransmission(false) != 0) {
        return false;
    }
    delayMicroseconds(M5PM1_I2C_READ_DELAY_US);  // 增加延迟避免 I2C 总线冲突
    if (wire->requestFrom(addr, (uint8_t)1) != 1) {
        return false;
    }
    *data = wire->read();
    return true;
}
#endif

#ifndef M5PM1_I2C_ARDUINO_READ_BYTES
static inline bool M5PM1_I2C_ARDUINO_READ_BYTES(TwoWire *wire, uint8_t addr, uint8_t start_reg, size_t len,
                                                uint8_t *data)
{
    wire->beginTransmission(addr);
    wire->write(start_reg);
    if (wire->endTransmission(false) != 0) {
        return false;
    }
    delayMicroseconds(M5PM1_I2C_READ_DELAY_US);  // 增加延迟避免 I2C 总线冲突
    if (wire->requestFrom(addr, (uint8_t)len) != len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        data[i] = wire->read();
    }
    return true;
}
#endif

#ifndef M5PM1_I2C_ARDUINO_READ_REG16
static inline bool M5PM1_I2C_ARDUINO_READ_REG16(TwoWire *wire, uint8_t addr, uint8_t reg, uint16_t *data)
{
    uint8_t buf[2];
    if (!M5PM1_I2C_ARDUINO_READ_BYTES(wire, addr, reg, 2, buf)) {
        return false;
    }
    // 小端模式：低字节在前
    // Little-endian: low byte first
    *data = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    return true;
}
#endif

#ifndef M5PM1_I2C_ARDUINO_WRITE_BYTE
static inline bool M5PM1_I2C_ARDUINO_WRITE_BYTE(TwoWire *wire, uint8_t addr, uint8_t reg, uint8_t data)
{
    wire->beginTransmission(addr);
    wire->write(reg);
    wire->write(data);
    if (wire->endTransmission() != 0) {
        return false;
    }
    delayMicroseconds(M5PM1_I2C_WRITE_DELAY_US);  // 增加延迟给设备处理时间
    return true;
}
#endif

#ifndef M5PM1_I2C_ARDUINO_WRITE_BYTES
static inline bool M5PM1_I2C_ARDUINO_WRITE_BYTES(TwoWire *wire, uint8_t addr, uint8_t start_reg, size_t len,
                                                 const uint8_t *data)
{
    wire->beginTransmission(addr);
    wire->write(start_reg);
    for (size_t i = 0; i < len; i++) {
        wire->write(data[i]);
    }
    if (wire->endTransmission() != 0) {
        return false;
    }
    delayMicroseconds(M5PM1_I2C_WRITE_DELAY_US);  // 增加延迟给设备处理时间
    return true;
}
#endif

#ifndef M5PM1_I2C_ARDUINO_WRITE_REG16
static inline bool M5PM1_I2C_ARDUINO_WRITE_REG16(TwoWire *wire, uint8_t addr, uint8_t reg, uint16_t data)
{
    uint8_t buf[2];
    // 小端模式：低字节在前
    // Little-endian: low byte first
    buf[0] = (uint8_t)(data & 0xFF);
    buf[1] = (uint8_t)((data >> 8) & 0xFF);
    return M5PM1_I2C_ARDUINO_WRITE_BYTES(wire, addr, reg, 2, buf);
}
#endif

// PM1睡眠模式唤醒信号
// Wake signal for PM1 sleep mode
#ifndef M5PM1_I2C_ARDUINO_SEND_WAKE
static inline void M5PM1_I2C_ARDUINO_SEND_WAKE(TwoWire *wire, uint8_t addr)
{
    // Send START signal to generate SDA falling edge for PM1 wake
    // PM1 uses SDA pin (PB4) falling edge to trigger EXTI4 interrupt for wakeup
    wire->beginTransmission(addr);
    wire->endTransmission();  // Send START with STOP
}
#endif

// ============================
// M5Unified I2C_Class 检测 (Arduino)
// M5Unified I2C_Class Detection (Arduino)
// ============================
#if defined(__cplusplus) && __has_include(<utility/I2C_Class.hpp>)
#define M5PM1_HAS_M5UNIFIED_I2C 1
#include <utility/I2C_Class.hpp>
#else
#define M5PM1_HAS_M5UNIFIED_I2C 0
#endif

// ============================
// M5Unified I2C_Class 通信封装 (Arduino)
// M5Unified I2C_Class Communication Wrappers (Arduino)
// (C++ only — m5::I2C_Class is a C++ class)
// ============================
#if M5PM1_HAS_M5UNIFIED_I2C

#ifndef M5PM1_M5UNIFIED_READ_BYTE
static inline bool M5PM1_M5UNIFIED_READ_BYTE(m5::I2C_Class *i2c, uint8_t addr, uint8_t reg, uint8_t *data,
                                             uint32_t freq)
{
    bool ok = i2c->readRegister(addr, reg, data, 1, freq);
    M5PM1_I2C_DELAY_US(M5PM1_I2C_READ_DELAY_US);
    return ok;
}
#endif

#ifndef M5PM1_M5UNIFIED_READ_BYTES
static inline bool M5PM1_M5UNIFIED_READ_BYTES(m5::I2C_Class *i2c, uint8_t addr, uint8_t reg, size_t len, uint8_t *data,
                                              uint32_t freq)
{
    bool ok = i2c->readRegister(addr, reg, data, len, freq);
    M5PM1_I2C_DELAY_US(M5PM1_I2C_READ_DELAY_US);
    return ok;
}
#endif

#ifndef M5PM1_M5UNIFIED_READ_REG16
static inline bool M5PM1_M5UNIFIED_READ_REG16(m5::I2C_Class *i2c, uint8_t addr, uint8_t reg, uint16_t *data,
                                              uint32_t freq)
{
    uint8_t buf[2];
    if (!i2c->readRegister(addr, reg, buf, 2, freq)) return false;
    M5PM1_I2C_DELAY_US(M5PM1_I2C_READ_DELAY_US);
    // 小端模式：低字节在前 / Little-endian: low byte first
    *data = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    return true;
}
#endif

#ifndef M5PM1_M5UNIFIED_WRITE_BYTE
static inline bool M5PM1_M5UNIFIED_WRITE_BYTE(m5::I2C_Class *i2c, uint8_t addr, uint8_t reg, uint8_t data,
                                              uint32_t freq)
{
    bool ok = i2c->writeRegister8(addr, reg, data, freq);
    M5PM1_I2C_DELAY_US(M5PM1_I2C_WRITE_DELAY_US);
    return ok;
}
#endif

#ifndef M5PM1_M5UNIFIED_WRITE_BYTES
static inline bool M5PM1_M5UNIFIED_WRITE_BYTES(m5::I2C_Class *i2c, uint8_t addr, uint8_t reg, size_t len,
                                               const uint8_t *data, uint32_t freq)
{
    bool ok = i2c->writeRegister(addr, reg, data, len, freq);
    M5PM1_I2C_DELAY_US(M5PM1_I2C_WRITE_DELAY_US);
    return ok;
}
#endif

#ifndef M5PM1_M5UNIFIED_WRITE_REG16
static inline bool M5PM1_M5UNIFIED_WRITE_REG16(m5::I2C_Class *i2c, uint8_t addr, uint8_t reg, uint16_t data,
                                               uint32_t freq)
{
    uint8_t buf[2];
    // 小端模式：低字节在前 / Little-endian: low byte first
    buf[0]  = (uint8_t)(data & 0xFF);
    buf[1]  = (uint8_t)((data >> 8) & 0xFF);
    bool ok = i2c->writeRegister(addr, reg, buf, 2, freq);
    M5PM1_I2C_DELAY_US(M5PM1_I2C_WRITE_DELAY_US);
    return ok;
}
#endif

#ifndef M5PM1_M5UNIFIED_SEND_WAKE
// 产生 I2C START 信号以唤醒处于睡眠的 PM1
// Generate I2C START signal to wake PM1 from sleep
static inline bool M5PM1_M5UNIFIED_SEND_WAKE(m5::I2C_Class *i2c, uint8_t addr, uint32_t freq)
{
    i2c->start(addr, false, freq);
    i2c->stop();
    return true;
}
#endif

#endif  // M5PM1_HAS_M5UNIFIED_I2C

#else  // ESP-IDF

#include <esp_err.h>
#include <esp_idf_version.h>
#include <esp_rom_sys.h>  // esp_rom_delay_us

// ============================
// I2C 驱动检测
// I2C Driver Detection
// ============================

// 检测 i2c_bus 是否可用
// Detect if i2c_bus is available
//
// ESP-IDF < 5.3.0
//   Without BACKWARD_CONFIG: i2c_bus not supported; use legacy driver/i2c.h API.
//   With    BACKWARD_CONFIG: i2c_bus.h falls back to driver/i2c.h internally, safe to use.
//
// ESP-IDF >= 5.3.0
//   [Highest priority] M5Unified is present:
//     -> i2c_bus mode is not safe because M5Unified owns its I2C_Class bus.
//     Fix: use pm1.begin(&M5.In_I2C, addr, freq) instead.
//   M5GFX alone:
//     -> header presence is not enough to prove an I2C runtime conflict. Projects that use
//        M5GFX only for display/SPI can still use espressif/i2c_bus for external devices.
//        Do not call lgfx::i2c::init() on the same port while i2c_bus owns it.
//   With    BACKWARD_CONFIG (no M5Unified): i2c_bus.h uses driver/i2c.h internally, safe.
//   Without BACKWARD_CONFIG (no M5Unified):
//     _DRIVER_I2C_H_ defined: i2c_config_t typedef conflict risk, disabled.
//     _DRIVER_I2C_H_ not defined: no conflict risk, enable i2c_bus.
#if __has_include(<i2c_bus.h>)
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 3, 0)
#if defined(CONFIG_I2C_BUS_BACKWARD_CONFIG)
#define M5PM1_HAS_I2C_BUS 1  // IDF < 5.3.0 + BACKWARD_CONFIG：可用 / available
#else
#define M5PM1_HAS_I2C_BUS 0  // IDF < 5.3.0：默认 Legacy API / legacy API by default
#endif
#else
// IDF >= 5.3.0
//
// Runtime conflict (highest priority):
// M5Unified owns an I2C_Class instance. espressif__i2c_bus conflicts with it on both code paths:
//   BACKWARD_CONFIG=y -> i2c_bus.c calls i2c_driver_install() (legacy) -> runtime abort()
//   BACKWARD_CONFIG=n -> i2c_bus_v2.c calls i2c_new_master_bus() on the same port -> undefined behaviour
// Therefore, i2c_bus mode MUST be disabled whenever M5Unified is in the project.
//
// M5GFX alone is intentionally allowed here. The presence of M5GFX.h does not mean lgfx::i2c::init()
// is used, and SPI/display-only M5GFX projects can share the project with espressif__i2c_bus safely.
#if __has_include(<M5Unified.h>)
#if defined(_I2C_BUS_H_)
#error \
    "[M5PM1] i2c_bus cannot be used together with M5Unified. " \
    "M5Unified owns its I2C_Class bus. " \
    "BACKWARD_CONFIG=y: i2c_bus.c calls i2c_driver_install() (legacy driver) -> runtime abort() in check_i2c_driver_conflict(). " \
    "BACKWARD_CONFIG=n: i2c_bus_v2.c calls i2c_new_master_bus() on the same port -> undefined behaviour. " \
    "Fix: use pm1.begin(&M5.In_I2C, addr, freq) and set I2C_USE_MODE=0."
#endif
#define M5PM1_HAS_I2C_BUS 0
#elif defined(CONFIG_I2C_BUS_BACKWARD_CONFIG)
#define M5PM1_HAS_I2C_BUS 1  // BACKWARD_CONFIG, no M5Unified: compatible
#elif defined(_DRIVER_I2C_H_)
#define M5PM1_HAS_I2C_BUS 0  // driver/i2c.h already included -> i2c_config_t typedef conflict risk, disabled
#else
#define M5PM1_HAS_I2C_BUS 1  // no conflict risk
#endif
#endif
#else
#define M5PM1_HAS_I2C_BUS 0
#endif

// 选择 I2C 驱动头文件
// Select I2C driver header
//
// ESP-IDF < 5.3.0：使用传统 Legacy I2C API（driver/i2c.h），与 i2c_bus 无关
// ESP-IDF < 5.3.0: use legacy I2C API (driver/i2c.h), independent of i2c_bus.
//
// ESP-IDF >= 5.3.0：优先使用新版 i2c_master 驱动
// ESP-IDF >= 5.3.0: prefer the new i2c_master driver.
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 3, 0)
#include <driver/i2c.h>
#elif __has_include(<driver/i2c_master.h>)
#include <driver/i2c_master.h>
#else
#include <driver/i2c.h>
#endif

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0) && __has_include(<driver/i2c_master.h>)
#define M5PM1_HAS_I2C_MASTER 1
#else
#define M5PM1_HAS_I2C_MASTER 0
#endif

// ============================
// M5Unified I2C_Class 检测
// M5Unified I2C_Class Detection
// ============================
#if M5PM1_HAS_I2C_BUS
#include <i2c_bus.h>
#else
// i2c_bus 桩类型
// i2c_bus stub types
typedef void *i2c_bus_handle_t;
typedef void *i2c_bus_device_handle_t;
#endif

//
// 仅 C++ 环境下检测 M5Unified 的 I2C_Class 是否可用。
// Detects whether M5Unified's I2C_Class is available (C++ only).
// 启用条件：__cplusplus 已定义（即编译单元为 C++）且 utility/I2C_Class.hpp 可被 include。
// Enabled when: __cplusplus is defined (i.e., C++ TU) AND utility/I2C_Class.hpp is reachable.
// 注意：i2c_bus.h 必须先于 I2C_Class.hpp 包含，避免 i2c_config_t typedef 冲突。
// Note: i2c_bus.h must be included before I2C_Class.hpp to avoid i2c_config_t typedef conflict.
#if defined(__cplusplus) && __has_include(<utility/I2C_Class.hpp>)
#define M5PM1_HAS_M5UNIFIED_I2C 1
#include <utility/I2C_Class.hpp>
#else
#define M5PM1_HAS_M5UNIFIED_I2C 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ============================
// I2C 驱动类型选择
// I2C Driver Type Selection
// ============================
typedef enum {
    M5PM1_I2C_DRIVER_NONE = 0,      // 未初始化 / Not initialized
    M5PM1_I2C_DRIVER_SELF_CREATED,  // 使用 i2c_port_t 自创建（IDF >= 5.3.0 用 i2c_master，否则用 Legacy）
                                    // Self-created using i2c_port_t (i2c_master on IDF >= 5.3.0, Legacy otherwise)
    M5PM1_I2C_DRIVER_MASTER,        // ESP-IDF 原生 i2c_master 驱动（仅 IDF >= 5.3.0）
                                    // ESP-IDF native i2c_master driver (IDF >= 5.3.0 only)
#if M5PM1_HAS_I2C_BUS
    M5PM1_I2C_DRIVER_BUS,  // esp-idf-lib i2c_bus 组件 / esp-idf-lib i2c_bus component
#endif
#if !M5PM1_HAS_I2C_MASTER && !M5PM1_HAS_I2C_BUS
    M5PM1_I2C_DRIVER_LEGACY,  // 传统 driver/i2c.h API（IDF < 5.3.0 且无 i2c_bus）
                              // Legacy driver/i2c.h API (IDF < 5.3.0 without i2c_bus)
#endif
#if M5PM1_HAS_M5UNIFIED_I2C
    M5PM1_I2C_DRIVER_M5UNIFIED,  // 借用 M5Unified I2C_Class 通信（不负责驱动安装/释放）
                                 // Borrow M5Unified I2C_Class for communication (no driver lifecycle)
#endif
} m5pm1_i2c_driver_t;

// ============================
// ESP-IDF I2C 函数 (i2c_bus)
// ESP-IDF I2C Functions (i2c_bus)
// ============================

#if M5PM1_HAS_I2C_BUS

#ifndef M5PM1_I2C_BUS_READ_BYTE
static inline esp_err_t M5PM1_I2C_BUS_READ_BYTE(i2c_bus_device_handle_t dev, uint8_t reg, uint8_t *data)
{
    esp_err_t ret = i2c_bus_read_byte(dev, reg, data);
    M5PM1_I2C_DELAY_US(M5PM1_I2C_READ_DELAY_US);
    return ret;
}
#endif

#ifndef M5PM1_I2C_BUS_READ_BYTES
static inline esp_err_t M5PM1_I2C_BUS_READ_BYTES(i2c_bus_device_handle_t dev, uint8_t start_reg, size_t len,
                                                 uint8_t *data)
{
    esp_err_t ret = i2c_bus_read_bytes(dev, start_reg, len, data);
    M5PM1_I2C_DELAY_US(M5PM1_I2C_READ_DELAY_US);
    return ret;
}
#endif

#ifndef M5PM1_I2C_BUS_READ_REG16
static inline esp_err_t M5PM1_I2C_BUS_READ_REG16(i2c_bus_device_handle_t dev, uint8_t reg, uint16_t *data)
{
    uint8_t buf[2];
    esp_err_t ret = i2c_bus_read_bytes(dev, reg, 2, buf);
    M5PM1_I2C_DELAY_US(M5PM1_I2C_READ_DELAY_US);
    if (ret == ESP_OK) {
        // 小端模式：低字节在前
        // Little-endian: low byte first
        *data = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    }
    return ret;
}
#endif

#ifndef M5PM1_I2C_BUS_WRITE_BYTE
static inline esp_err_t M5PM1_I2C_BUS_WRITE_BYTE(i2c_bus_device_handle_t dev, uint8_t reg, uint8_t data)
{
    esp_err_t ret = i2c_bus_write_byte(dev, reg, data);
    M5PM1_I2C_DELAY_US(M5PM1_I2C_WRITE_DELAY_US);
    return ret;
}
#endif

#ifndef M5PM1_I2C_BUS_WRITE_BYTES
static inline esp_err_t M5PM1_I2C_BUS_WRITE_BYTES(i2c_bus_device_handle_t dev, uint8_t start_reg, size_t len,
                                                  const uint8_t *data)
{
    esp_err_t ret = i2c_bus_write_bytes(dev, start_reg, len, (uint8_t *)data);
    M5PM1_I2C_DELAY_US(M5PM1_I2C_WRITE_DELAY_US);
    return ret;
}
#endif

#ifndef M5PM1_I2C_BUS_WRITE_REG16
static inline esp_err_t M5PM1_I2C_BUS_WRITE_REG16(i2c_bus_device_handle_t dev, uint8_t reg, uint16_t data)
{
    uint8_t buf[2];
    // 小端模式：低字节在前
    // Little-endian: low byte first
    buf[0]        = (uint8_t)(data & 0xFF);
    buf[1]        = (uint8_t)((data >> 8) & 0xFF);
    esp_err_t ret = i2c_bus_write_bytes(dev, reg, 2, buf);
    M5PM1_I2C_DELAY_US(M5PM1_I2C_WRITE_DELAY_US);
    return ret;
}
#endif

// PM1睡眠模式唤醒信号 (i2c_bus)
// Wake signal for PM1 sleep mode (i2c_bus)
#ifndef M5PM1_I2C_BUS_SEND_WAKE
static inline esp_err_t M5PM1_I2C_BUS_SEND_WAKE(i2c_bus_device_handle_t dev, uint8_t reg)
{
    // Read any register to generate I2C start signal for wake
    uint8_t dummy;
    return i2c_bus_read_byte(dev, reg, &dummy);
}
#endif

#else  // !M5PM1_HAS_I2C_BUS

// ============================
// ESP-IDF I2C 函数 (Legacy - driver/i2c.h，仅 IDF < 5.3.0 且无 i2c_bus)
// ESP-IDF I2C Functions (Legacy - driver/i2c.h, IDF < 5.3.0 without i2c_bus only)
// 使用 IDF 4.3+ 引入的简单 API：i2c_master_write_to_device / i2c_master_write_read_device
// Uses simple API introduced in IDF 4.3+: i2c_master_write_to_device / i2c_master_write_read_device
// ============================

#if !M5PM1_HAS_I2C_MASTER

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// 传统 I2C 超时，单位毫秒 / Legacy I2C timeout in milliseconds
#ifndef M5PM1_I2C_LEGACY_TIMEOUT_MS
#define M5PM1_I2C_LEGACY_TIMEOUT_MS 100
#endif

#ifndef M5PM1_I2C_LEGACY_READ_BYTE
static inline esp_err_t M5PM1_I2C_LEGACY_READ_BYTE(i2c_port_t port, uint8_t addr, uint8_t reg, uint8_t *data)
{
    esp_err_t ret =
        i2c_master_write_read_device(port, addr, &reg, 1, data, 1, pdMS_TO_TICKS(M5PM1_I2C_LEGACY_TIMEOUT_MS));
    M5PM1_I2C_DELAY_US(M5PM1_I2C_READ_DELAY_US);
    return ret;
}
#endif

#ifndef M5PM1_I2C_LEGACY_READ_BYTES
static inline esp_err_t M5PM1_I2C_LEGACY_READ_BYTES(i2c_port_t port, uint8_t addr, uint8_t start_reg, size_t len,
                                                    uint8_t *data)
{
    esp_err_t ret =
        i2c_master_write_read_device(port, addr, &start_reg, 1, data, len, pdMS_TO_TICKS(M5PM1_I2C_LEGACY_TIMEOUT_MS));
    M5PM1_I2C_DELAY_US(M5PM1_I2C_READ_DELAY_US);
    return ret;
}
#endif

#ifndef M5PM1_I2C_LEGACY_READ_REG16
static inline esp_err_t M5PM1_I2C_LEGACY_READ_REG16(i2c_port_t port, uint8_t addr, uint8_t reg, uint16_t *data)
{
    uint8_t buf[2];
    esp_err_t ret =
        i2c_master_write_read_device(port, addr, &reg, 1, buf, 2, pdMS_TO_TICKS(M5PM1_I2C_LEGACY_TIMEOUT_MS));
    M5PM1_I2C_DELAY_US(M5PM1_I2C_READ_DELAY_US);
    if (ret == ESP_OK) {
        // 小端模式：低字节在前
        // Little-endian: low byte first
        *data = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    }
    return ret;
}
#endif

#ifndef M5PM1_I2C_LEGACY_WRITE_BYTE
static inline esp_err_t M5PM1_I2C_LEGACY_WRITE_BYTE(i2c_port_t port, uint8_t addr, uint8_t reg, uint8_t data)
{
    uint8_t buf[2] = {reg, data};
    esp_err_t ret  = i2c_master_write_to_device(port, addr, buf, 2, pdMS_TO_TICKS(M5PM1_I2C_LEGACY_TIMEOUT_MS));
    M5PM1_I2C_DELAY_US(M5PM1_I2C_WRITE_DELAY_US);
    return ret;
}
#endif

#ifndef M5PM1_I2C_LEGACY_WRITE_BYTES
static inline esp_err_t M5PM1_I2C_LEGACY_WRITE_BYTES(i2c_port_t port, uint8_t addr, uint8_t start_reg, size_t len,
                                                     const uint8_t *data)
{
    uint8_t *buf = (uint8_t *)malloc(len + 1);
    if (buf == NULL) return ESP_ERR_NO_MEM;
    buf[0] = start_reg;
    memcpy(buf + 1, data, len);
    esp_err_t ret = i2c_master_write_to_device(port, addr, buf, len + 1, pdMS_TO_TICKS(M5PM1_I2C_LEGACY_TIMEOUT_MS));
    free(buf);
    M5PM1_I2C_DELAY_US(M5PM1_I2C_WRITE_DELAY_US);
    return ret;
}
#endif

#ifndef M5PM1_I2C_LEGACY_WRITE_REG16
static inline esp_err_t M5PM1_I2C_LEGACY_WRITE_REG16(i2c_port_t port, uint8_t addr, uint8_t reg, uint16_t data)
{
    uint8_t buf[3];
    buf[0] = reg;
    // 小端模式：低字节在前
    // Little-endian: low byte first
    buf[1]        = (uint8_t)(data & 0xFF);
    buf[2]        = (uint8_t)((data >> 8) & 0xFF);
    esp_err_t ret = i2c_master_write_to_device(port, addr, buf, 3, pdMS_TO_TICKS(M5PM1_I2C_LEGACY_TIMEOUT_MS));
    M5PM1_I2C_DELAY_US(M5PM1_I2C_WRITE_DELAY_US);
    return ret;
}
#endif

// 唤醒信号发送 (Legacy) / Wake signal send (Legacy)
#ifndef M5PM1_I2C_LEGACY_SEND_WAKE
static inline esp_err_t M5PM1_I2C_LEGACY_SEND_WAKE(i2c_port_t port, uint8_t addr)
{
    uint8_t dummy = 0;
    uint8_t reg   = 0x00;
    // 忽略错误：唤醒时设备可能处于睡眠状态，ACK 可能超时
    // Ignore error: device may be asleep during wake, ACK may timeout
    i2c_master_write_read_device(port, addr, &reg, 1, &dummy, 1, pdMS_TO_TICKS(10));
    return ESP_OK;
}
#endif

#endif  // !M5PM1_HAS_I2C_MASTER

#endif  // M5PM1_HAS_I2C_BUS

// ============================
// ESP-IDF I2C 函数 (i2c_master - 原生驱动)
// ESP-IDF I2C Functions (i2c_master - native driver)
// 仅 ESP-IDF >= 5.3.0 支持（driver/i2c_master.h 引入于 5.1，API 稳定于 5.3）
// Available on ESP-IDF >= 5.3.0 only (driver/i2c_master.h introduced in 5.1, stable in 5.3)
// ============================

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)

#ifndef M5PM1_I2C_MASTER_READ_BYTE
static inline esp_err_t M5PM1_I2C_MASTER_READ_BYTE(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *data)
{
    esp_err_t ret = i2c_master_transmit_receive(dev, &reg, 1, data, 1, -1);
    M5PM1_I2C_DELAY_US(M5PM1_I2C_READ_DELAY_US);
    return ret;
}
#endif

#ifndef M5PM1_I2C_MASTER_READ_BYTES
static inline esp_err_t M5PM1_I2C_MASTER_READ_BYTES(i2c_master_dev_handle_t dev, uint8_t start_reg, size_t len,
                                                    uint8_t *data)
{
    esp_err_t ret = i2c_master_transmit_receive(dev, &start_reg, 1, data, len, -1);
    M5PM1_I2C_DELAY_US(M5PM1_I2C_READ_DELAY_US);
    return ret;
}
#endif

#ifndef M5PM1_I2C_MASTER_READ_REG16
static inline esp_err_t M5PM1_I2C_MASTER_READ_REG16(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t *data)
{
    uint8_t buf[2];
    esp_err_t ret = i2c_master_transmit_receive(dev, &reg, 1, buf, 2, -1);
    M5PM1_I2C_DELAY_US(M5PM1_I2C_READ_DELAY_US);
    if (ret == ESP_OK) {
        // 小端模式：低字节在前
        // Little-endian: low byte first
        *data = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    }
    return ret;
}
#endif

#ifndef M5PM1_I2C_MASTER_WRITE_BYTE
static inline esp_err_t M5PM1_I2C_MASTER_WRITE_BYTE(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t data)
{
    uint8_t buf[2] = {reg, data};
    esp_err_t ret  = i2c_master_transmit(dev, buf, 2, -1);
    M5PM1_I2C_DELAY_US(M5PM1_I2C_WRITE_DELAY_US);
    return ret;
}
#endif

#ifndef M5PM1_I2C_MASTER_WRITE_BYTES
static inline esp_err_t M5PM1_I2C_MASTER_WRITE_BYTES(i2c_master_dev_handle_t dev, uint8_t start_reg, size_t len,
                                                     const uint8_t *data)
{
    // 需要在数据前添加寄存器地址
    // Need to prepend register address
    uint8_t *buf = (uint8_t *)malloc(len + 1);
    if (buf == NULL) return ESP_ERR_NO_MEM;
    buf[0] = start_reg;
    memcpy(buf + 1, data, len);
    esp_err_t ret = i2c_master_transmit(dev, buf, len + 1, -1);
    free(buf);
    M5PM1_I2C_DELAY_US(M5PM1_I2C_WRITE_DELAY_US);
    return ret;
}
#endif

#ifndef M5PM1_I2C_MASTER_WRITE_REG16
static inline esp_err_t M5PM1_I2C_MASTER_WRITE_REG16(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t data)
{
    uint8_t buf[3];
    buf[0] = reg;
    // 小端模式：低字节在前
    // Little-endian: low byte first
    buf[1]        = (uint8_t)(data & 0xFF);
    buf[2]        = (uint8_t)((data >> 8) & 0xFF);
    esp_err_t ret = i2c_master_transmit(dev, buf, 3, -1);
    M5PM1_I2C_DELAY_US(M5PM1_I2C_WRITE_DELAY_US);
    return ret;
}
#endif

// PM1睡眠模式唤醒信号 (i2c_master)
// Wake signal for PM1 sleep mode (i2c_master)
#ifndef M5PM1_I2C_MASTER_SEND_WAKE
static inline esp_err_t M5PM1_I2C_MASTER_SEND_WAKE(i2c_master_bus_handle_t bus, uint8_t addr)
{
    // Use i2c_master_probe to generate START signal for wake
    return i2c_master_probe(bus, addr, 10);
}
#endif

#endif  // ESP_IDF_VERSION >= 5.3.0

#ifdef __cplusplus
}
#endif

// ============================
// M5Unified I2C_Class 通信封装
// M5Unified I2C_Class Communication Wrappers
// (C++ only — m5::I2C_Class is a C++ class)
// ============================
#if M5PM1_HAS_M5UNIFIED_I2C
#ifdef __cplusplus

#ifndef M5PM1_M5UNIFIED_READ_BYTE
static inline bool M5PM1_M5UNIFIED_READ_BYTE(m5::I2C_Class *i2c, uint8_t addr, uint8_t reg, uint8_t *data,
                                             uint32_t freq)
{
    bool ok = i2c->readRegister(addr, reg, data, 1, freq);
    M5PM1_I2C_DELAY_US(M5PM1_I2C_READ_DELAY_US);
    return ok;
}
#endif

#ifndef M5PM1_M5UNIFIED_READ_BYTES
static inline bool M5PM1_M5UNIFIED_READ_BYTES(m5::I2C_Class *i2c, uint8_t addr, uint8_t reg, size_t len, uint8_t *data,
                                              uint32_t freq)
{
    bool ok = i2c->readRegister(addr, reg, data, len, freq);
    M5PM1_I2C_DELAY_US(M5PM1_I2C_READ_DELAY_US);
    return ok;
}
#endif

#ifndef M5PM1_M5UNIFIED_READ_REG16
static inline bool M5PM1_M5UNIFIED_READ_REG16(m5::I2C_Class *i2c, uint8_t addr, uint8_t reg, uint16_t *data,
                                              uint32_t freq)
{
    uint8_t buf[2];
    if (!i2c->readRegister(addr, reg, buf, 2, freq)) return false;
    M5PM1_I2C_DELAY_US(M5PM1_I2C_READ_DELAY_US);
    // 小端模式：低字节在前 / Little-endian: low byte first
    *data = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    return true;
}
#endif

#ifndef M5PM1_M5UNIFIED_WRITE_BYTE
static inline bool M5PM1_M5UNIFIED_WRITE_BYTE(m5::I2C_Class *i2c, uint8_t addr, uint8_t reg, uint8_t data,
                                              uint32_t freq)
{
    bool ok = i2c->writeRegister8(addr, reg, data, freq);
    M5PM1_I2C_DELAY_US(M5PM1_I2C_WRITE_DELAY_US);
    return ok;
}
#endif

#ifndef M5PM1_M5UNIFIED_WRITE_BYTES
static inline bool M5PM1_M5UNIFIED_WRITE_BYTES(m5::I2C_Class *i2c, uint8_t addr, uint8_t reg, size_t len,
                                               const uint8_t *data, uint32_t freq)
{
    bool ok = i2c->writeRegister(addr, reg, data, len, freq);
    M5PM1_I2C_DELAY_US(M5PM1_I2C_WRITE_DELAY_US);
    return ok;
}
#endif

#ifndef M5PM1_M5UNIFIED_WRITE_REG16
static inline bool M5PM1_M5UNIFIED_WRITE_REG16(m5::I2C_Class *i2c, uint8_t addr, uint8_t reg, uint16_t data,
                                               uint32_t freq)
{
    uint8_t buf[2];
    // 小端模式：低字节在前 / Little-endian: low byte first
    buf[0]  = (uint8_t)(data & 0xFF);
    buf[1]  = (uint8_t)((data >> 8) & 0xFF);
    bool ok = i2c->writeRegister(addr, reg, buf, 2, freq);
    M5PM1_I2C_DELAY_US(M5PM1_I2C_WRITE_DELAY_US);
    return ok;
}
#endif

#ifndef M5PM1_M5UNIFIED_SEND_WAKE
// 产生 I2C START 信号以唤醒处于睡眠的 PM1
// Generate I2C START signal to wake PM1 from sleep
static inline bool M5PM1_M5UNIFIED_SEND_WAKE(m5::I2C_Class *i2c, uint8_t addr, uint32_t freq)
{
    i2c->start(addr, false, freq);
    i2c->stop();
    return true;
}
#endif

#endif  // __cplusplus
#endif  // M5PM1_HAS_M5UNIFIED_I2C

#endif  // ARDUINO

#endif  // __M5PM1_I2C_COMPAT_H__
