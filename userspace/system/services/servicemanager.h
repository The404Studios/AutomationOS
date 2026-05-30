// userspace/system/services/servicemanager.h - Service manager public API
#ifndef SERVICEMANAGER_H
#define SERVICEMANAGER_H

#include <stddef.h>

// Initialize service manager and load all service configurations
int service_manager_init(void);

// Start all enabled services (called during boot)
int service_manager_start_boot_services(void);

// Shutdown service manager and stop all services
int service_manager_shutdown(void);

// Individual service control
int service_start(const char *name);
int service_stop(const char *name);
int service_restart(const char *name);
int service_reload(const char *name);
int service_enable(const char *name);
int service_disable(const char *name);

// Service status
int service_status(const char *name, char *buffer, size_t size);
int service_list(void (*callback)(const char *name, const char *state, void *userdata), void *userdata);

#endif // SERVICEMANAGER_H
