#include "thread.h"
#include <errno.h>

CLH_Thread clh_thread_spawn(CLH_ThreadCallback cb, void *args)
{
    CLH_Thread thread;

    pthread_create(&thread, NULL, cb, args);
    return thread;
}

void clh_thread_join(CLH_Thread thread)
{
    pthread_join(thread, NULL);
}

CLH_Mutex clh_mutex_create()
{
    CLH_Mutex mutex;
    pthread_mutex_init(&mutex, NULL);
    return mutex;
}

void clh_mutex_destroy(CLH_Mutex *mutex)
{
    pthread_mutex_destroy(mutex);
}

void clh_mutex_lock(CLH_Mutex *mutex)
{
    pthread_mutex_lock(mutex);
}

void clh_mutex_unlock(CLH_Mutex *mutex)
{
    pthread_mutex_unlock(mutex);
}

CLH_ConditionalVariable clh_conditional_variable_create()
{
    CLH_ConditionalVariable cv;
    pthread_cond_init(&cv, NULL);
    return cv;
}

void clh_conditional_variable_destroy(CLH_ConditionalVariable *cv)
{
    pthread_cond_destroy(cv);
}

void clh_conditional_variable_wait(CLH_ConditionalVariable *cv, CLH_Mutex *mutex)
{
    pthread_cond_wait(cv, mutex);
}

void clh_conditional_variable_notify_one(CLH_ConditionalVariable *cv)
{
    pthread_cond_signal(cv);
}

void clh_conditional_variable_notify_all(CLH_ConditionalVariable *cv)
{
    pthread_cond_broadcast(cv);
}

bool clh_lock_region_init(CLH_Mutex *mutex)
{
    clh_mutex_lock(mutex);
    return false;
}

bool clh_lock_region_end(CLH_Mutex *mutex)
{
    clh_mutex_unlock(mutex);
    return true;
}

bool clh_trylock_region_init(CLH_Mutex *mutex)
{
    if (EBUSY == pthread_mutex_trylock(mutex)) {
        return true;
    }
    return false;
}
