#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "driver/uart.h"
#include "hal/gpio_types.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace rpc {

constexpr size_t kMaxTransportPayload = 192;
constexpr size_t kMaxFunctionName = 24;
constexpr size_t kMaxTransportBuffer = kMaxTransportPayload + kMaxFunctionName + 3;
constexpr size_t kMaxFrameSize = kMaxTransportPayload + 7 + kMaxFunctionName + 3;
constexpr size_t kMaxPendingRequests = 6;
constexpr size_t kMaxHandlers = 8;

enum class TransportType : uint8_t {
  kRequest = 0x0B,
  kStream = 0x0C,
  kResponse = 0x16,
  kError = 0x21,
};

struct RpcArgumentView {
  const uint8_t* data;
  size_t size;
};

using RpcHandler = bool (*)(RpcArgumentView args,
                            std::array<uint8_t, kMaxTransportPayload>* responsePayload,
                            size_t* responseLength);

struct RpcProtocolConfig {
  uart_port_t uartPort{UART_NUM_1};
  int baudRate{115200};
  gpio_num_t txPin{GPIO_NUM_17};
  gpio_num_t rxPin{GPIO_NUM_16};
  size_t queueDepth{6};
  uint32_t transportTaskStack{4096};
  uint32_t channelTaskStack{4096};
  uint32_t physicalTaskStack{4096};
  UBaseType_t transportPriority{tskIDLE_PRIORITY + 3};
  UBaseType_t channelPriority{tskIDLE_PRIORITY + 2};
  UBaseType_t physicalPriority{tskIDLE_PRIORITY + 2};
  TickType_t defaultTimeout{pdMS_TO_TICKS(1500)};
};

struct RpcMessage {
  struct TransportLayer {
    TransportType type{TransportType::kRequest};
    uint8_t sequence{0};
    size_t nameLength{0};
    std::array<char, kMaxFunctionName> name{};
    size_t payloadLength{0};
    std::array<uint8_t, kMaxTransportPayload> payload{};
  } transport;

  struct ChannelLayer {
    size_t payloadLength{0};
    std::array<uint8_t, kMaxTransportBuffer> payload{};
    size_t frameLength{0};
    std::array<uint8_t, kMaxFrameSize> frame{};
  } channel;
};

class RpcProtocol {
 public:
  esp_err_t init(const RpcProtocolConfig& config);
  esp_err_t registerHandler(const char* name, RpcHandler handler);
  esp_err_t sendRequest(const char* name, const uint8_t* payload, size_t length,
                        std::array<uint8_t, kMaxTransportPayload>* responsePayload,
                        size_t* responseLength,
                        TickType_t timeout = portMAX_DELAY);

 private:
  struct HandlerRecord {
    bool used{false};
    std::array<char, kMaxFunctionName> name{};
    RpcHandler handler{nullptr};
  };

  struct PendingRequest {
    bool used{false};
    uint8_t sequence{0};
    QueueHandle_t syncQueue{nullptr};
  };

  struct PhysicalChunk {
    size_t length{0};
    std::array<uint8_t, kMaxFrameSize> data{};
  };

  static void PhysicalTask(void* arg);
  static void ChannelTask(void* arg);
  static void TransportTask(void* arg);

  void runPhysical();
  void runChannel();
  void runTransport();

  bool pushTransportMessage(const RpcMessage& message);
  bool pushPhysicalChunk(const PhysicalChunk& chunk);

  bool encodeTransport(RpcMessage* message);
  bool encodeChannel(RpcMessage* message);
  bool appendChunk(const PhysicalChunk& chunk);
  bool extractFrame(RpcMessage* message);

  void handleIncoming(const RpcMessage& message);
  void handleRequest(const RpcMessage& message);
  void handleResponse(const RpcMessage& message);
  void handleError(const RpcMessage& message);

  RpcHandler findHandler(const char* name, size_t nameLength);
  PendingRequest* allocPending(uint8_t seq);
  PendingRequest* findPending(uint8_t seq);

  RpcProtocolConfig config_{};
  QueueHandle_t transportTxQueue_{nullptr};
  QueueHandle_t channelTxQueue_{nullptr};
  QueueHandle_t channelRxQueue_{nullptr};
  QueueHandle_t physicalTxQueue_{nullptr};
  QueueHandle_t physicalRxQueue_{nullptr};
  QueueHandle_t uartEventQueue_{nullptr};
  std::array<HandlerRecord, kMaxHandlers> handlers_{};
  std::array<PendingRequest, kMaxPendingRequests> pending_{};
  std::array<uint8_t, kMaxFrameSize * 2> channelBuffer_{};
  size_t channelBufferLength_{0};
  uint8_t nextSequence_{0};
  bool initialized_{false};
};

}  // namespace rpc
