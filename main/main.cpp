#include "rpc_protocol.hpp"

#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr char kMainTag[] = "app_main";
rpc::RpcProtocol gProtocol;

bool EchoHandler(rpc::RpcArgumentView args,
                 std::array<uint8_t, rpc::kMaxTransportPayload>* response,
                 size_t* responseLength) {
  if (!response || !responseLength || args.size > response->size()) {
    return false;
  }
  std::memcpy(response->data(), args.data, args.size);
  *responseLength = args.size;
  return true;
}

}  // namespace

extern "C" void app_main(void) {
  rpc::RpcProtocolConfig config;
  config.uartPort = UART_NUM_1;
  config.txPin = GPIO_NUM_17;
  config.rxPin = GPIO_NUM_16;

  if (gProtocol.init(config) != ESP_OK) {
    ESP_LOGE(kMainTag, "Не удалось инициализировать стек RPC");
    return;
  }

  if (gProtocol.registerHandler("echo", EchoHandler) != ESP_OK) {
    ESP_LOGE(kMainTag, "Не удалось зарегистрировать обработчик echo");
    return;
  }

  ESP_LOGI(kMainTag, "Стек RPC запущен. Подключите UART1 к второй стороне.");
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
