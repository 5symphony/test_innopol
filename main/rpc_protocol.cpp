#include "rpc_protocol.hpp"

#include <algorithm>
#include <cstring>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/task.h"

namespace rpc {
namespace {

constexpr char kTag[] = "rpc_simple";
constexpr uint8_t kHeaderStart = 0xFA;
constexpr uint8_t kPayloadStart = 0xFB;
constexpr uint8_t kStopByte = 0xFE;
constexpr uint8_t kCrcPoly = 0x07;

uint8_t crc8(const uint8_t* data, size_t length) {
  uint8_t crc = 0x00;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      if (crc & 0x80) {
        crc = (crc << 1) ^ kCrcPoly;
      } else {
        crc <<= 1;
      }
    }
  }
  return crc;
}

size_t SafeStrnlen(const char* text, size_t maxLen) {
  if (!text) {
    return 0;
  }
  size_t len = 0;
  while (len < maxLen && text[len] != '\0') {
    ++len;
  }
  return len;
}

}  // namespace

esp_err_t RpcProtocol::init(const RpcProtocolConfig& config) {
  if (initialized_) {
    return ESP_OK;
  }
  config_ = config;

  transportTxQueue_ = xQueueCreate(config.queueDepth, sizeof(RpcMessage));
  channelTxQueue_ = xQueueCreate(config.queueDepth, sizeof(RpcMessage));
  channelRxQueue_ = xQueueCreate(config.queueDepth, sizeof(RpcMessage));
  physicalTxQueue_ = xQueueCreate(config.queueDepth, sizeof(RpcMessage));
  physicalRxQueue_ = xQueueCreate(config.queueDepth, sizeof(PhysicalChunk));
  if (!transportTxQueue_ || !channelTxQueue_ || !channelRxQueue_ || !physicalTxQueue_ || !physicalRxQueue_) {
    return ESP_ERR_NO_MEM;
  }

  uart_config_t uartConfig{};
  uartConfig.baud_rate = config_.baudRate;
  uartConfig.data_bits = UART_DATA_8_BITS;
  uartConfig.parity = UART_PARITY_DISABLE;
  uartConfig.stop_bits = UART_STOP_BITS_1;
  uartConfig.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uartConfig.source_clk = UART_SCLK_DEFAULT;

  ESP_RETURN_ON_ERROR(uart_param_config(config_.uartPort, &uartConfig), kTag, "uart_param_config failed");
  ESP_RETURN_ON_ERROR(uart_set_pin(config_.uartPort, config_.txPin, config_.rxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                      kTag, "uart_set_pin failed");

  const int driverBufferSize = static_cast<int>(kMaxFrameSize * 2);
  ESP_RETURN_ON_ERROR(
      uart_driver_install(config_.uartPort, driverBufferSize, driverBufferSize, config.queueDepth, &uartEventQueue_, 0),
      kTag, "uart_driver_install failed");

  if (xTaskCreate(&RpcProtocol::PhysicalTask, "rpc_phy", config_.physicalTaskStack, this, config_.physicalPriority, nullptr) !=
          pdPASS ||
      xTaskCreate(&RpcProtocol::ChannelTask, "rpc_channel", config_.channelTaskStack, this, config_.channelPriority,
                  nullptr) != pdPASS ||
      xTaskCreate(&RpcProtocol::TransportTask, "rpc_transport", config_.transportTaskStack, this,
                  config_.transportPriority, nullptr) != pdPASS) {
    ESP_LOGE(kTag, "Failed to create tasks");
    return ESP_FAIL;
  }

  initialized_ = true;
  ESP_LOGI(kTag, "RPC protocol initialized");
  return ESP_OK;
}

esp_err_t RpcProtocol::registerHandler(const char* name, RpcHandler handler) {
  if (!name || !handler) {
    return ESP_ERR_INVALID_ARG;
  }

  const size_t len = SafeStrnlen(name, kMaxFunctionName);
  if (len == 0 || len >= kMaxFunctionName) {
    return ESP_ERR_INVALID_ARG;
  }

  for (auto& record : handlers_) {
    if (!record.used) {
      record.used = true;
      std::fill(record.name.begin(), record.name.end(), 0);
      std::memcpy(record.name.data(), name, len);
      record.handler = handler;
      return ESP_OK;
    }
  }
  return ESP_ERR_NO_MEM;
}

esp_err_t RpcProtocol::sendRequest(const char* name, const uint8_t* payload, size_t length,
                                   std::array<uint8_t, kMaxTransportPayload>* responsePayload,
                                   size_t* responseLength, TickType_t timeout) {
  if (!initialized_ || !name || length > kMaxTransportPayload || (length > 0 && !payload)) {
    return ESP_ERR_INVALID_ARG;
  }
  const size_t nameLen = SafeStrnlen(name, kMaxFunctionName);
  if (nameLen == 0 || nameLen >= kMaxFunctionName) {
    return ESP_ERR_INVALID_ARG;
  }

  RpcMessage message;
  message.transport.type = TransportType::kRequest;
  message.transport.sequence = nextSequence_++;
  message.transport.nameLength = nameLen;
  std::fill(message.transport.name.begin(), message.transport.name.end(), 0);
  std::memcpy(message.transport.name.data(), name, nameLen);
  message.transport.payloadLength = length;
  if (length > 0) {
    std::memcpy(message.transport.payload.data(), payload, length);
  }

  PendingRequest* pending = allocPending(message.transport.sequence);
  if (!pending) {
    return ESP_ERR_NO_MEM;
  }
  pending->syncQueue = xQueueCreate(1, sizeof(RpcMessage));
  if (!pending->syncQueue) {
    pending->used = false;
    return ESP_ERR_NO_MEM;
  }

  if (!pushTransportMessage(message)) {
    vQueueDelete(pending->syncQueue);
    pending->used = false;
    return ESP_FAIL;
  }

  RpcMessage reply;
  const TickType_t waitTimeout = (timeout == portMAX_DELAY) ? config_.defaultTimeout : timeout;
  if (xQueueReceive(pending->syncQueue, &reply, waitTimeout) != pdTRUE) {
    vQueueDelete(pending->syncQueue);
    pending->used = false;
    return ESP_ERR_TIMEOUT;
  }

  if (responseLength) {
    *responseLength = reply.transport.payloadLength;
  }
  if (responsePayload && reply.transport.payloadLength > 0) {
    std::memcpy(responsePayload->data(), reply.transport.payload.data(), reply.transport.payloadLength);
  }

  vQueueDelete(pending->syncQueue);
  pending->used = false;
  return ESP_OK;
}

void RpcProtocol::PhysicalTask(void* arg) {
  static_cast<RpcProtocol*>(arg)->runPhysical();
}

void RpcProtocol::ChannelTask(void* arg) {
  static_cast<RpcProtocol*>(arg)->runChannel();
}

void RpcProtocol::TransportTask(void* arg) {
  static_cast<RpcProtocol*>(arg)->runTransport();
}

void RpcProtocol::runPhysical() {
  uart_event_t evt{};
  while (true) {
    if (xQueueReceive(uartEventQueue_, &evt, pdMS_TO_TICKS(20)) == pdTRUE) {
      if (evt.type == UART_DATA && evt.size > 0) {
        size_t remaining = evt.size;
        while (remaining > 0) {
          PhysicalChunk chunk;
          chunk.length = std::min(remaining, kMaxFrameSize);
          const int read =
              uart_read_bytes(config_.uartPort, chunk.data.data(), chunk.length, pdMS_TO_TICKS(20));
          if (read <= 0) {
            break;
          }
          chunk.length = read;
          remaining -= read;
          pushPhysicalChunk(chunk);
        }
      } else if (evt.type == UART_FIFO_OVF || evt.type == UART_BUFFER_FULL) {
        ESP_LOGW(kTag, "UART overflow %d", evt.type);
        uart_flush_input(config_.uartPort);
      }
    }

    RpcMessage outgoing;
    if (xQueueReceive(physicalTxQueue_, &outgoing, 0) == pdTRUE) {
      const auto* data = outgoing.channel.frame.data();
      size_t remaining = outgoing.channel.frameLength;
      while (remaining > 0) {
        const int written = uart_write_bytes(config_.uartPort, reinterpret_cast<const char*>(data), remaining);
        if (written < 0) {
          ESP_LOGE(kTag, "UART write failed");
          break;
        }
        remaining -= written;
        data += written;
      }
      uart_wait_tx_done(config_.uartPort, pdMS_TO_TICKS(20));
    }
  }
}

void RpcProtocol::runChannel() {
  while (true) {
    RpcMessage message;
    if (xQueueReceive(channelTxQueue_, &message, pdMS_TO_TICKS(20)) == pdTRUE) {
      if (encodeTransport(&message) && encodeChannel(&message)) {
        xQueueSend(physicalTxQueue_, &message, portMAX_DELAY);
      }
    }

    PhysicalChunk chunk;
    while (xQueueReceive(physicalRxQueue_, &chunk, 0) == pdTRUE) {
      appendChunk(chunk);
      RpcMessage incoming;
      while (extractFrame(&incoming)) {
        xQueueSend(channelRxQueue_, &incoming, portMAX_DELAY);
      }
    }
  }
}

void RpcProtocol::runTransport() {
  while (true) {
    RpcMessage outgoing;
    if (xQueueReceive(transportTxQueue_, &outgoing, pdMS_TO_TICKS(20)) == pdTRUE) {
      xQueueSend(channelTxQueue_, &outgoing, portMAX_DELAY);
    }

    RpcMessage incoming;
    while (xQueueReceive(channelRxQueue_, &incoming, 0) == pdTRUE) {
      handleIncoming(incoming);
    }
  }
}

bool RpcProtocol::pushTransportMessage(const RpcMessage& message) {
  return xQueueSend(transportTxQueue_, &message, portMAX_DELAY) == pdTRUE;
}

bool RpcProtocol::pushPhysicalChunk(const PhysicalChunk& chunk) {
  return xQueueSend(physicalRxQueue_, &chunk, portMAX_DELAY) == pdTRUE;
}

bool RpcProtocol::encodeTransport(RpcMessage* message) {
  auto& transport = message->transport;
  auto& payload = message->channel.payload;
  size_t index = 0;
  payload[index++] = static_cast<uint8_t>(transport.type);
  payload[index++] = transport.sequence;
  std::memcpy(&payload[index], transport.name.data(), transport.nameLength);
  index += transport.nameLength;
  payload[index++] = 0x00;
  std::memcpy(&payload[index], transport.payload.data(), transport.payloadLength);
  index += transport.payloadLength;
  message->channel.payloadLength = index;
  return true;
}

bool RpcProtocol::encodeChannel(RpcMessage* message) {
  auto& channel = message->channel;
  auto& frame = channel.frame;
  size_t index = 0;
  const uint16_t payloadLength = static_cast<uint16_t>(channel.payloadLength);
  frame[index++] = kHeaderStart;
  frame[index++] = payloadLength & 0xFF;
  frame[index++] = (payloadLength >> 8) & 0xFF;
  const uint8_t headerCrc = crc8(frame.data(), 3);
  frame[index++] = headerCrc;
  frame[index++] = kPayloadStart;
  std::memcpy(&frame[index], channel.payload.data(), channel.payloadLength);
  index += channel.payloadLength;
  const uint8_t payloadCrc = crc8(frame.data(), index);
  frame[index++] = payloadCrc;
  frame[index++] = kStopByte;
  channel.frameLength = index;
  return true;
}

bool RpcProtocol::appendChunk(const PhysicalChunk& chunk) {
  if (chunk.length == 0) {
    return false;
  }
  if (channelBufferLength_ + chunk.length > channelBuffer_.size()) {
    channelBufferLength_ = 0;
  }
  std::memcpy(channelBuffer_.data() + channelBufferLength_, chunk.data.data(), chunk.length);
  channelBufferLength_ += chunk.length;
  return true;
}

bool RpcProtocol::extractFrame(RpcMessage* message) {
  while (channelBufferLength_ >= 5) {
    if (channelBuffer_[0] != kHeaderStart) {
      std::memmove(channelBuffer_.data(), channelBuffer_.data() + 1, channelBufferLength_ - 1);
      --channelBufferLength_;
      continue;
    }

    if (channelBufferLength_ < 5) {
      return false;
    }

    const uint16_t payloadLen = static_cast<uint16_t>(channelBuffer_[1]) |
                                (static_cast<uint16_t>(channelBuffer_[2]) << 8);
    const size_t totalLen = payloadLen + 7;
    if (payloadLen > kMaxTransportBuffer) {
      channelBufferLength_ = 0;
      return false;
    }

    if (channelBufferLength_ < totalLen) {
      return false;
    }

    const uint8_t headerCrc = crc8(channelBuffer_.data(), 3);
    if (headerCrc != channelBuffer_[3] || channelBuffer_[4] != kPayloadStart) {
      std::memmove(channelBuffer_.data(), channelBuffer_.data() + 1, channelBufferLength_ - 1);
      --channelBufferLength_;
      continue;
    }

    const size_t payloadIndex = 5;
    const uint8_t packetCrc = channelBuffer_[payloadIndex + payloadLen];
    if (packetCrc != crc8(channelBuffer_.data(), payloadIndex + payloadLen)) {
      std::memmove(channelBuffer_.data(), channelBuffer_.data() + 1, channelBufferLength_ - 1);
      --channelBufferLength_;
      continue;
    }

    if (channelBuffer_[payloadIndex + payloadLen + 1] != kStopByte) {
      std::memmove(channelBuffer_.data(), channelBuffer_.data() + 1, channelBufferLength_ - 1);
      --channelBufferLength_;
      continue;
    }

    std::memcpy(message->channel.payload.data(), channelBuffer_.data() + payloadIndex, payloadLen);
    message->channel.payloadLength = payloadLen;

    const size_t remaining = channelBufferLength_ - totalLen;
    std::memmove(channelBuffer_.data(), channelBuffer_.data() + totalLen, remaining);
    channelBufferLength_ = remaining;

    const uint8_t* buffer = message->channel.payload.data();
    if (payloadLen < 3) {
      return false;
    }
    message->transport.type = static_cast<TransportType>(buffer[0]);
    message->transport.sequence = buffer[1];
    const uint8_t* namePtr = &buffer[2];
    const uint8_t* terminator =
        static_cast<const uint8_t*>(std::memchr(namePtr, 0x00, payloadLen - 2));
    if (!terminator) {
      return false;
    }
    const size_t nameLen = terminator - namePtr;
    message->transport.nameLength = std::min(nameLen, kMaxFunctionName - 1);
    std::fill(message->transport.name.begin(), message->transport.name.end(), 0);
    std::memcpy(message->transport.name.data(), namePtr, message->transport.nameLength);
    const size_t payloadOffset = 2 + nameLen + 1;
    if (payloadOffset > payloadLen) {
      return false;
    }
    message->transport.payloadLength = payloadLen - payloadOffset;
    std::memcpy(message->transport.payload.data(), buffer + payloadOffset, message->transport.payloadLength);
    return true;
  }
  return false;
}

void RpcProtocol::handleIncoming(const RpcMessage& message) {
  switch (message.transport.type) {
    case TransportType::kRequest:
      handleRequest(message);
      break;
    case TransportType::kResponse:
      handleResponse(message);
      break;
    case TransportType::kError:
      handleError(message);
      break;
    default:
      ESP_LOGW(kTag, "Unknown transport type %u", static_cast<unsigned>(message.transport.type));
      break;
  }
}

void RpcProtocol::handleRequest(const RpcMessage& message) {
  RpcHandler handler = findHandler(message.transport.name.data(), message.transport.nameLength);
  RpcMessage response = message;
  response.transport.payloadLength = 0;
  response.transport.type = TransportType::kError;

  if (!handler) {
    const char* text = "not found";
    response.transport.payloadLength = SafeStrnlen(text, kMaxTransportPayload);
    std::memcpy(response.transport.payload.data(), text, response.transport.payloadLength);
  } else {
    RpcArgumentView view{message.transport.payload.data(), message.transport.payloadLength};
    size_t resultLength = 0;
    std::array<uint8_t, kMaxTransportPayload> out{};
    if (handler(view, &out, &resultLength) && resultLength <= kMaxTransportPayload) {
      response.transport.type = TransportType::kResponse;
      response.transport.payloadLength = resultLength;
      if (resultLength > 0) {
        std::memcpy(response.transport.payload.data(), out.data(), resultLength);
      }
    } else {
      const char* text = "handler error";
      response.transport.payloadLength = SafeStrnlen(text, kMaxTransportPayload);
      std::memcpy(response.transport.payload.data(), text, response.transport.payloadLength);
    }
  }

  pushTransportMessage(response);
}

void RpcProtocol::handleResponse(const RpcMessage& message) {
  PendingRequest* pending = findPending(message.transport.sequence);
  if (!pending || !pending->syncQueue) {
    return;
  }
  xQueueSend(pending->syncQueue, &message, portMAX_DELAY);
}

void RpcProtocol::handleError(const RpcMessage& message) {
  handleResponse(message);
}

RpcHandler RpcProtocol::findHandler(const char* name, size_t nameLength) {
  for (const auto& record : handlers_) {
    if (!record.used || !record.handler) {
      continue;
    }
    if (std::strncmp(record.name.data(), name, kMaxFunctionName) == 0) {
      return record.handler;
    }
  }
  return nullptr;
}

RpcProtocol::PendingRequest* RpcProtocol::allocPending(uint8_t seq) {
  for (auto& item : pending_) {
    if (!item.used) {
      item.used = true;
      item.sequence = seq;
      item.syncQueue = nullptr;
      return &item;
    }
  }
  return nullptr;
}

RpcProtocol::PendingRequest* RpcProtocol::findPending(uint8_t seq) {
  for (auto& item : pending_) {
    if (item.used && item.sequence == seq) {
      return &item;
    }
  }
  return nullptr;
}

}  // namespace rpc
