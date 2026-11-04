// Task set-up for the ground station.
//
// Replaces the Arduino loop(): every subsystem gets a task that blocks on a
// queue, a notification or a timeout, which is what allows the idle task to put
// the CPU into light sleep between LoRa packets.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void bd_app_start(void);

#ifdef __cplusplus
}
#endif
