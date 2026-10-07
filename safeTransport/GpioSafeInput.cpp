#include "GpioSafeInput.h"

GpioSafeInput::GpioSafeInput(GPIO_TypeDef* port, uint16_t pin, bool activeHigh)
    : port_(port), pin_(pin), activeHigh_(activeHigh) {}

void GpioSafeInput::poll() {
    const GPIO_PinState raw = HAL_GPIO_ReadPin(port_, pin_);
    const bool safe = activeHigh_ ? (raw == GPIO_PIN_SET) : (raw == GPIO_PIN_RESET);
    setSafe1State(safe);
    setSafe2State(safe);
}
