# Milestone roadmap

## M0 — Repository and data contract (current)
- [x] Python package structure
- [x] Synthetic stream generator
- [x] Protocol parser and formatter
- [x] Seven protocol unit tests

## M1 — Host-side data tools
- [ ] Read a text stream and validate every record
- [ ] Write clean records to CSV
- [ ] Plot roll/pitch/yaw against time
- [ ] Report malformed lines and sequence gaps

## M2 — Sensor algorithm
- [ ] Implement accelerometer-based roll/pitch calculation
- [ ] Implement gyro integration
- [ ] Implement complementary filter
- [ ] Compare raw estimates with filtered angles and quantify error

## M3 — Physical sensor bring-up
- [ ] Create STM32CubeIDE project for STM32F103
- [ ] Confirm I2C communication with MPU9250 and read WHO_AM_I
- [ ] Read raw accelerometer/gyroscope/magnetometer values
- [ ] Check units, scale factors, sample rate, and sensor saturation

## M4 — Device-host integration
- [ ] Stream data through UART at a documented baud rate
- [ ] Connect actual serial input to parser
- [ ] Live plot and record an experiment
- [ ] Test disconnect/reconnect and malformed frames

## M5 — Engineering polish
- [ ] Configuration file and command-line options
- [ ] Logging and diagnostic counters
- [ ] CI running unit tests
- [ ] Architecture diagram, demo recording, reproducible test instructions
