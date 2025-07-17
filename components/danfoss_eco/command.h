#pragma once

#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"

#include "properties.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace esphome
{
    namespace danfoss_eco
    {
        using namespace std;

        enum class CommandType
        {
            READ,
            WRITE
        };

        struct Command
        {
            Command(CommandType t, shared_ptr<DeviceProperty> const &p) : type(t), property(p) {}

            CommandType type; // 0 - read, 1 - write
            shared_ptr<DeviceProperty> property;

            bool execute(esphome::ble_client::BLEClient *client)
            {
                if (this->type == CommandType::WRITE)
                {
                    WritableProperty *wp = static_cast<WritableProperty *>(this->property.get());
                    return wp->write_request(client);
                }
                else
                    return this->property->read_request(client);
            }
        };

        // Start with 32 entries.
        // Log queue size/usage or add backpressure warnings.
        // Scale up to 64 only if you observe dropped or missing advertisements
        class CommandQueue
        {
        protected:
            QueueHandle_t queue_handle_;
            static constexpr size_t QUEUE_SIZE = 32;

        public:
            CommandQueue()
            {
                queue_handle_ = xQueueCreate(QUEUE_SIZE, sizeof(Command *));
            }

            ~CommandQueue()
            {
                if (queue_handle_ != nullptr)
                {
                    // Clean up any remaining commands
                    Command *cmd;
                    while (xQueueReceive(queue_handle_, &cmd, 0) == pdTRUE)
                    {
                        delete cmd;
                    }
                    vQueueDelete(queue_handle_);
                }
            }

            void push(Command *cmd)
            {
                xQueueSend(queue_handle_, &cmd, portMAX_DELAY);
            }

            Command *pop()
            {
                Command *cmd = nullptr;
                if (xQueueReceive(queue_handle_, &cmd, 0) == pdTRUE)
                {
                    return cmd;
                }
                return nullptr;
            }

            bool empty() const
            {
                return uxQueueMessagesWaiting(queue_handle_) == 0;
            }
        };
    } // namespace danfoss_eco
} // namespace esphome
