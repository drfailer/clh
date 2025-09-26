#ifndef CLH_THREAD
#define CLH_THREAD
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *(*CLH_ThreadCallback)(void*);
typedef pthread_mutex_t CLH_Mutex;
typedef pthread_t CLH_Thread;

CLH_Thread clh_thread_spawn(CLH_ThreadCallback cb, void *args);
void       clh_thread_join(CLH_Thread thread);

CLH_Mutex clh_mutex_create();
void      clh_mutex_destroy(CLH_Mutex *mutex);

void clh_mutex_lock(CLH_Mutex *mutex);
void clh_mutex_unlock(CLH_Mutex *mutex);

#ifdef __cplusplus
}
#endif

#endif // CLH_THREAD
