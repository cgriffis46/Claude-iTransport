Abstract transport interface for microcontrollers to communicate with ICs using different transports (spi/uart/i2c) and make the connection as seamless as possible. 

For example, the BMP280 can communicate over either I2C or SPI. We try to implement both transports (SPI or I2c) using abstract methods, and reuse as much code as possible
by having the child classes implement the necessary I2C/SPI functions. Im trying to expand these transports to UART and implement more ethernet, wifi, and bluetooth controllers
using similar transports.


donations:
BTC: 3NHkEBTHdWWbvDBu8AXpmV8TFhnJPrqzBe

