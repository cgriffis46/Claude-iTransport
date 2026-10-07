#include "GpioSafeOutput.h"

GpioSafeOutput::GpioSafeOutput(GPIO_TypeDef* port, uint16_t pin)
    : port_(port), pin_(pin) {}

void GpioSafeOutput::setDesiredState(bool safe) {
    desiredState_ = safe;
    setSafe1State(safe);
    setSafe2State(safe);
    send();
}

bool GpioSafeOutput::send() {
    HAL_GPIO_WritePin(port_, pin_, desiredState_ ? GPIO_PIN_SET : GPIO_PIN_RESET);
    return true;
}
