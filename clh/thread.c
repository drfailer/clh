#include "thread.h"

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
