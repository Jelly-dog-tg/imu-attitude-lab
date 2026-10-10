# STM32F103 firmware (later milestone)

This folder reserves the firmware component. The CubeIDE/CubeMX-generated project and hardware-specific code will be added after the PC-side protocol tests are stable.

Planned modules:

- `bsp/imu_mpu9250.c/.h`: I2C register access and sensor identity checks
- `drivers/uart_protocol.c/.h`: record formatter and UART transmit queue
- `algorithm/attitude.c/.h`: calibration, gyro integration, complementary filter
- `app/main.c`: scheduled sampling and fault handling

Do not claim this firmware works on hardware until it has been built, flashed, and tested on the actual board. Record board revision, sensor breakout, wiring, I2C address, UART baud rate, compiler version, and test evidence in the eventual release notes.
