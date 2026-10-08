/*
 * xdavis_rfm69.h
 *
 *  The Davis receiver in its own FreeRTOS task. The state machine is
 *  davis_rfm69's, unchanged. What this adds is how data gets in and out:
 *
 *    packets()   a queue of DavisPacket, one per good packet, for the
 *                task that uses the readings (decode() / DavisWeather).
 *                Sent with no wait: when it is full the packet is
 *                dropped and stats().dropped counts it.
 *    log()       optional stream buffer of text, one line per packet,
 *                for a task that writes it to a UART, a file or a socket:
 *
 *                  <rx ms> <transmitter 1-8> <channel> <rssi dBm> <fei Hz> <10 raw bytes in hex>[ R]\n
 *
 *                R marks a repeated packet. A line that does not fit is
 *                left out whole, never cut, and logDropped() counts it.
 *    commands    postStationActive(), postBand() and postResync() from
 *                any task go through a queue, and wake the radio task.
 *    DIO0        onDio0FromISR() from the pin's interrupt notes the time
 *                and wakes the task, which is otherwise asleep until
 *                the next packet is due.
 *
 *  The task sleeps in ulTaskNotifyTake() for as long as the state
 *  machine has nothing to do (sleep() below), so it costs nothing
 *  between packets, and the DIO0 interrupt or a command cuts the sleep
 *  short.
 *
 *      static xdavis_rfm69<Stm32HalSPITransport> radio(param, &hspi1, RFM_CS_GPIO_Port, RFM_CS_Pin, spi1Mutex);
 *      radio.start();                       // before or after vTaskStartScheduler()
 *
 *      void HAL_GPIO_EXTI_Callback(uint16_t pin) {
 *          if (pin == RFM_DIO0_Pin) radio.onDio0FromISR();
 *      }
 *
 *      DavisPacket p;
 *      if (xQueueReceive(radio.packets(), &p, portMAX_DELAY) == pdPASS) { ... }
 *
 *  Native FreeRTOS (queues, stream buffers, task notifications); CMSIS-
 *  RTOS2 has no stream buffer. Times are ms: ticks x portTICK_PERIOD_MS.
 */

#ifndef XDAVIS_RFM69_H_
#define XDAVIS_RFM69_H_

#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "stream_buffer.h"
#include "davis_rfm69.h"

namespace DAVIS {

typedef enum davis_command_type_t{
	davis_cmd_station_active = 0,	// id, value: 1 listen / 0 stop
	davis_cmd_band,					// value: davis_band_t
	davis_cmd_resync
}davis_command_type_t;

typedef struct davis_command_t{
	davis_command_type_t type;
	uint8_t id;
	uint8_t value;
}davis_command_t;

struct xdavis_config_t {
	UBaseType_t packetDepth = 8;		// DavisPackets the queue holds
	size_t logBytes = 0;				// stream buffer size; 0: no log
	UBaseType_t commandDepth = 4;
};

template <typename TTransport>
class xdavis_rfm69 : public davis_rfm69<TTransport> {
	using Base = davis_rfm69<TTransport>;

public:
	typedef xdavis_config_t Config;

	using Base::Base;

	// Creates the queues and the task. False if any could not be.
	bool start(const char* name = "davis", uint16_t stackWords = 384,
			   UBaseType_t priority = tskIDLE_PRIORITY + 3, const Config& cfg = Config());

	// Creates only the queues: for running run() or step() from a task
	// of your own.
	bool begin(const Config& cfg = Config());

	QueueHandle_t packets() const { return packets_; }
	StreamBufferHandle_t log() const { return log_; }
	uint32_t logDropped() const { return log_dropped; }

	// From any task. False if the command queue stayed full.
	bool postStationActive(uint8_t id, bool active, TickType_t wait = 0) {
		davis_command_t c = { davis_cmd_station_active, (uint8_t)(id & 7), (uint8_t)(active ? 1 : 0) };
		return post(c, wait);
	}
	bool postBand(davis_band_t band, TickType_t wait = 0) {
		davis_command_t c = { davis_cmd_band, 0, (uint8_t)band };
		return post(c, wait);
	}
	bool postResync(TickType_t wait = 0) {
		davis_command_t c = { davis_cmd_resync, 0, 0 };
		return post(c, wait);
	}

	// From DIO0's rising edge.
	void onDio0FromISR() {
		this->onDio0((uint32_t)(xTaskGetTickCountFromISR() * portTICK_PERIOD_MS));
		if (task_ != nullptr) {
			BaseType_t woken = pdFALSE;
			vTaskNotifyGiveFromISR(task_, &woken);
			portYIELD_FROM_ISR(woken);
		}
	}

	// One pass: the commands waiting, then main() once (which sleeps when
	// there is nothing to do). run() is this for ever.
	void step();
	void run();

protected:
	void sleep(uint32_t ms) override {
		(void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms));
	}
	void deliver(const DavisPacket& p) override;

private:
	static void taskEntry(void* arg) { static_cast<xdavis_rfm69*>(arg)->run(); }
	bool post(const davis_command_t& c, TickType_t wait);

	QueueHandle_t packets_ = nullptr;
	QueueHandle_t commands_ = nullptr;
	StreamBufferHandle_t log_ = nullptr;
	TaskHandle_t task_ = nullptr;
	uint32_t log_dropped = 0;
};

template <typename TTransport>
bool xdavis_rfm69<TTransport>::begin(const Config& cfg) {
	if (packets_ == nullptr) packets_ = xQueueCreate(cfg.packetDepth ? cfg.packetDepth : 1, sizeof(DavisPacket));
	if (commands_ == nullptr) commands_ = xQueueCreate(cfg.commandDepth ? cfg.commandDepth : 1, sizeof(davis_command_t));
	if (log_ == nullptr && cfg.logBytes > 0) log_ = xStreamBufferCreate(cfg.logBytes, 1);
	return packets_ != nullptr && commands_ != nullptr && (cfg.logBytes == 0 || log_ != nullptr);
}

template <typename TTransport>
bool xdavis_rfm69<TTransport>::start(const char* name, uint16_t stackWords, UBaseType_t priority, const Config& cfg) {
	if (!begin(cfg)) return false;
	if (task_ != nullptr) return true;
	return xTaskCreate(taskEntry, name, stackWords, this, priority, &task_) == pdPASS;
}

template <typename TTransport>
bool xdavis_rfm69<TTransport>::post(const davis_command_t& c, TickType_t wait) {
	if (commands_ == nullptr) return false;
	if (xQueueSend(commands_, &c, wait) != pdPASS) return false;
	if (task_ != nullptr) xTaskNotifyGive(task_);
	return true;
}

template <typename TTransport>
void xdavis_rfm69<TTransport>::step() {
	if (task_ == nullptr) task_ = xTaskGetCurrentTaskHandle();
	davis_command_t c;
	while (commands_ != nullptr && xQueueReceive(commands_, &c, 0) == pdPASS) {
		switch (c.type) {
		case davis_cmd_station_active: this->setStationActive(c.id, c.value != 0); break;
		case davis_cmd_band:           this->setBand((davis_band_t)c.value); break;
		case davis_cmd_resync:         this->resync(); break;
		}
	}
	this->main((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
}

template <typename TTransport>
void xdavis_rfm69<TTransport>::run() {
	task_ = xTaskGetCurrentTaskHandle();
	for (;;) step();
}

template <typename TTransport>
void xdavis_rfm69<TTransport>::deliver(const DavisPacket& p) {
	if (packets_ == nullptr || xQueueSend(packets_, &p, 0) != pdPASS) ++this->st.dropped;
	if (log_ == nullptr) return;

	char line[96];
	int n = snprintf(line, sizeof line, "%lu %u %u %d %ld",
		(unsigned long)p.rxMs, (unsigned)(p.station + 1), (unsigned)p.channel, (int)p.rssi, (long)p.feiHz);
	for (uint8_t i = 0; i < kPacketLen && n > 0 && n < (int)sizeof line; ++i)
		n += snprintf(line + n, sizeof line - (size_t)n, " %02X", p.raw[i]);
	if (n > 0 && n < (int)sizeof line - 3) {
		if (p.viaRepeater) { line[n++] = ' '; line[n++] = 'R'; }
		line[n++] = '\n';
	} else {
		++log_dropped;
		return;
	}
	if (xStreamBufferSpacesAvailable(log_) < (size_t)n) {
		++log_dropped;
		return;
	}
	(void)xStreamBufferSend(log_, line, (size_t)n, 0);
}

} /* namespace DAVIS */

#endif /* XDAVIS_RFM69_H_ */
