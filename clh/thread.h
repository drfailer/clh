#ifndef CLH_THREAD
#define CLH_THREAD
#include <pthread.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *(*CLH_ThreadCallback)(void *);
typedef pthread_mutex_t CLH_Mutex;
typedef pthread_t       CLH_Thread;
typedef pthread_cond_t  CLH_ConditionalVariable;

CLH_Thread clh_thread_spawn(CLH_ThreadCallback cb, void *args);
void       clh_thread_join(CLH_Thread thread);

CLH_Mutex clh_mutex_create();
void      clh_mutex_destroy(CLH_Mutex *mutex);

void clh_mutex_lock(CLH_Mutex *mutex);
void clh_mutex_unlock(CLH_Mutex *mutex);

CLH_ConditionalVariable clh_conditional_variable_create();
void                    clh_conditional_variable_destroy(CLH_ConditionalVariable *cv);
void clh_conditional_variable_wait(CLH_ConditionalVariable *cv, CLH_Mutex *mutex);
void clh_conditional_variable_notify_one(CLH_ConditionalVariable *cv);
void clh_conditional_variable_notify_all(CLH_ConditionalVariable *cv);

bool clh_lock_region_init(CLH_Mutex *mutex);
bool clh_lock_region_end(CLH_Mutex *mutex);
bool clh_trylock_region_init(CLH_Mutex *mutex);
bool clh_trylock_region_end(CLH_Mutex *mutex);

#define CLH_LOCK_REGION(mutex_) \
    for (bool done = clh_lock_region_init(&mutex_); !done; done = clh_lock_region_end(&mutex_))
#define CLH_TRYLOCK_REGION(mutex_) \
    for (bool done = clh_trylock_region_init(&mutex_); !done; done = clh_lock_region_end(&mutex_))
#define CLH_EXIT_LOCK_REGION() continue

#ifdef __cplusplus
}
#endif

#endif // CLH_THREAD
