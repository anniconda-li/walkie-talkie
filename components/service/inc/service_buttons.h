/**
 * @file service_buttons.h
 * @brief Board button service for debounced volume key events.
 */
#ifndef SERVICE_BUTTONS_H
#define SERVICE_BUTTONS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SERVICE_BUTTONS_LEVEL_LOW = 0,
    SERVICE_BUTTONS_LEVEL_HIGH = 1,
} service_buttons_level_t;

typedef struct {
    int (*read_volume_up)(service_buttons_level_t *level);
    int (*read_volume_down)(service_buttons_level_t *level);
    service_buttons_level_t active_level;
} service_buttons_config_t;

typedef struct {
    void (*volume_step)(int step);
} service_buttons_callbacks_t;

int service_buttons_init(const service_buttons_config_t *cfg);
int service_buttons_deinit(void);
int service_buttons_start(const service_buttons_callbacks_t *callbacks);

#ifdef __cplusplus
}
#endif

#endif /* SERVICE_BUTTONS_H */
