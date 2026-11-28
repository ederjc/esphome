#pragma once

#include <cstddef>
#include <cstdint>

struct device {
  bool ready{true};
};
inline bool device_is_ready(const device *dev) { return dev->ready; }
using spi_operation_t = uint32_t;
inline constexpr spi_operation_t SPI_OP_MODE_MASTER = 0;
inline constexpr spi_operation_t SPI_MODE_CPOL = 1;
inline constexpr spi_operation_t SPI_MODE_CPHA = 2;
inline constexpr spi_operation_t SPI_TRANSFER_LSB = 4;
inline constexpr spi_operation_t SPI_TRANSFER_MSB = 0;
inline constexpr spi_operation_t SPI_LINES_QUAD = 8;
inline constexpr spi_operation_t SPI_LINES_OCTAL = 16;
inline constexpr spi_operation_t SPI_WORD_SET(unsigned bits) { return bits << 8; }
struct spi_config {
  uint32_t frequency;
  spi_operation_t operation;
};
struct spi_buf {
  void *buf;
  size_t len;
};
struct spi_buf_set {
  const spi_buf *buffers;
  size_t count;
};
int spi_transceive(const device *dev, const spi_config *config, const spi_buf_set *tx, const spi_buf_set *rx);
