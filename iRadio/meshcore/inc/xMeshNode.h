/*
 * xMeshNode.h
 *
 *  The MeshCore node's thread loop for CMSIS-RTOS2 (FreeRTOS): sleeps as
 *  long as Node::sleepHintMs() allows, and the radio's DIO interrupts
 *  wake it at once.
 *
 *      static rfm95<Stm32HalSPITransport> radio(rp, &hspi1, RFM_CS_GPIO_Port, RFM_CS_Pin, spi1Mutex);
 *      static meshcore::LocalIdentity id;
 *      static meshcore::Node node(radio, id, meshcore::defaultNodeParam());
 *      static meshcore::xMeshNode loop(node);
 *
 *      // EXTI on DIO0:  radio.onDio0(HAL_GetTick()); loop.wakeFromIsr();
 *
 *      void meshTask(void*) {
 *          id.fromSeed(seed);
 *          node.addChannel(key, 16);
 *          node.begin(osKernelGetTickCount());
 *          for (;;) {
 *              loop.step(osKernelGetTickCount());
 *              meshcore::NodeEvent e; uint8_t buf[184];
 *              while (node.takeEvent(&e, buf, sizeof buf)) ...
 *          }
 *      }
 *
 *  Everything that touches the node or the radio runs in this one thread;
 *  only onDio0() and wakeFromIsr() come from the interrupt.
 */

#ifndef XMESHNODE_H_
#define XMESHNODE_H_

#include "cmsis_os2.h"
#include "MeshNode.h"

namespace meshcore {

class xMeshNode {
public:
	// Not xrfm95's (0x04000000) nor xLoRaWanMac's (0x08000000).
	static const uint32_t kWakeFlag = 0x10000000u;

	explicit xMeshNode(Node& node) : _node(node) {}

	void step(uint32_t nowMs) {
		_thread = osThreadGetId();
		_node.main(nowMs);
		const uint32_t ms = _node.sleepHintMs(nowMs);
		if (ms) osThreadFlagsWait(kWakeFlag, osFlagsWaitAny, ms);
	}

	void wakeFromIsr() {
		osThreadId_t t = _thread;
		if (t != nullptr) osThreadFlagsSet(t, kWakeFlag);
	}

private:
	Node& _node;
	osThreadId_t volatile _thread = nullptr;
};

} // namespace meshcore

#endif /* XMESHNODE_H_ */
