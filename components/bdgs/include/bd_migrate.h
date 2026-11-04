// One-way migration from the Arduino firmware's 8 MB flash layout into the
// layout this firmware is built for. See core/bd_migrate.cpp.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Call early in boot, after NVS is initialised and before any other subsystem
// touches flash. Returns only if no migration step had to reboot the station.
void bd_migrate_layout_if_needed(void);

#ifdef __cplusplus
}
#endif
