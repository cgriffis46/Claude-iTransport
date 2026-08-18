#include <cstdio>
#include "cmsis_os2.h"
#include "Stm32HalI2CTransport.h"
#include "SPITransport.h"
#include "xBMP280.h"

extern I2C_HandleTypeDef hi2c1;
extern UART_HandleTypeDef huart2;

// Called from SensorBase::State::NotifyData, right after each reading
// completes — an alternative to polling hasNewReading() from loop().
// Non-blocking: HAL_UART_Transmit_IT() only *issues* the transfer.
static void onBmpReading(SensorBase& sensor, void* /*context*/) {
    auto& bmp = static_cast<xBMP280&>(sensor);

    // buf must stay untouched until any in-flight transfer finishes —
    // overwriting it mid-transmit would corrupt what's currently being
    // sent. If UART2 is still busy with the previous reading, just
    // drop this one rather than block bmp's own task waiting for it.
    if (HAL_UART_GetState(&huart2) != HAL_UART_STATE_READY) return;

    static char buf[48];
    int len = snprintf(buf, sizeof(buf), "T=%.2fC P=%.2fPa\r\n",
                        static_cast<double>(bmp.lastTemperatureC()),
                        static_cast<double>(bmp.lastPressurePa()));
    if (len <= 0) return;
    if (static_cast<size_t>(len) >= sizeof(buf)) len = sizeof(buf) - 1;

    HAL_UART_Transmit_IT(&huart2, reinterpret_cast<uint8_t*>(buf), static_cast<uint16_t>(len));
}

// This project's actual entry point. Assumes HAL_Init() and clock
// config (the usual CubeMX-generated boilerplate) have already run
// ahead of this — the one hard requirement is osKernelInitialize()
// before any RTOS object (mutex, thread, ...) gets created.
//
// Uses xBMP280, not plain BMP280Sensor — this file assumes FreeRTOS
// is available (osKernelInitialize()/osKernelStart() below), and only
// xBMP280 overrides startThread()/sleep() to actually use it. Plain
// BMP280Sensor would still compile here, but begin() would be a no-op
// (its startThread() is SensorBase's default), and nothing would ever
// call bmp.main() — this file has no manual loop() of its own, by
// design, since it hands off to the scheduler permanently below.
int main() {
    osKernelInitialize(); // must come before osMutexNew()/begin()'s osThreadNew()

    static osMutexId_t  i2c1BusMutex = osMutexNew(nullptr); // shared by every
                                                              // I2CTransport on this bus
    static Stm32HalI2CTransport bmpBus(&hi2c1, 0x76, i2c1BusMutex);
    static xBMP280 bmp(bmpBus); // construction alone does nothing RTOS-related

    bmp.setNewDataCallback(&onBmpReading); // optional — omit this line and
                                             // NotifyData is just a no-op step

    bmp.begin(); // starts bmp's own dedicated task (xBMP280::startThread())

    osKernelStart(); // hands off to the scheduler — does not return
    for (;;) {}       // unreachable
}
