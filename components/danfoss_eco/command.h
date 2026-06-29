#pragma once

#include "esphome/core/lock_free_queue.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"

#include "properties.h"

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

        // Command queue capacity. Command bursts are small (4 reads per poll, 1-2 writes per
        // climate control call), so 16 is ample. LockFreeQueue's SIZE template param is uint8_t.
        static constexpr uint8_t COMMAND_QUEUE_SIZE = 16;

        // esp32_ble_tracker::Queue<T> was removed from ESPHome; replace with the official
        // esphome::LockFreeQueue<T, SIZE>. It stores T* (pointers), which matches how device.cpp
        // uses the queue: push(new Command(...)) / Command* c = pop() / delete c. push() is
        // non-blocking and returns false (dropping the element) if full - unlike a FreeRTOS queue
        // with portMAX_DELAY, which would block the main loop and risk a watchdog reset.
        class CommandQueue : public esphome::LockFreeQueue<Command, COMMAND_QUEUE_SIZE>
        {
        public:
            bool is_empty() { return this->empty(); }
        };

    } // namespace danfoss_eco
} // namespace esphome
