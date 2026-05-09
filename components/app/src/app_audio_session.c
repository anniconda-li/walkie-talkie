/**
 * @file app_audio_session.c
 * @brief App 内部音频业务互斥状态。
 */
#include "app_audio_session.h"

#include "osal_mutex.h"

static osal_mutex_t s_audio_session_mutex = NULL;
static int s_audio_session_busy = 0;

int app_audio_session_init(void)
{
    if (s_audio_session_mutex != NULL) {
        return 0;
    }

    s_audio_session_mutex = osal_mutex_create();
    return s_audio_session_mutex == NULL ? -1 : 0;
}

int app_audio_session_try_begin(void)
{
    if (app_audio_session_init() != 0) {
        return -1;
    }

    if (osal_mutex_lock(s_audio_session_mutex, OSAL_WAIT_NONE) != 0) {
        return -2;
    }

    if (s_audio_session_busy) {
        osal_mutex_unlock(s_audio_session_mutex);
        return -3;
    }

    s_audio_session_busy = 1;
    osal_mutex_unlock(s_audio_session_mutex);
    return 0;
}

void app_audio_session_end(void)
{
    if (s_audio_session_mutex == NULL) {
        return;
    }

    if (osal_mutex_lock(s_audio_session_mutex, OSAL_WAIT_FOREVER) == 0) {
        s_audio_session_busy = 0;
        osal_mutex_unlock(s_audio_session_mutex);
    }
}

int app_audio_session_is_busy(void)
{
    int busy = 0;

    if (s_audio_session_mutex == NULL) {
        return 0;
    }

    if (osal_mutex_lock(s_audio_session_mutex, OSAL_WAIT_NONE) == 0) {
        busy = s_audio_session_busy;
        osal_mutex_unlock(s_audio_session_mutex);
    }

    return busy;
}
