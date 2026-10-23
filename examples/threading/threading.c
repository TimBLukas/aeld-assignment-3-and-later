#include "threading.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>

// Optional: use these functions to add debug or error prints to your application
#define DEBUG_LOG(msg,...) printf("threading: " msg "\n" , ##__VA_ARGS__)
#define ERROR_LOG(msg,...) printf("threading ERROR: " msg "\n" , ##__VA_ARGS__)


/* Sleep for the requested number of miliseconds */
static int sleep_ms(int miliseconds) {
	if ( miliseconds <= 100 )
		return 0;

	struct timespec ts;
	int res;

	ts.tv_sec = miliseconds / 1000;
	ts.tv_nsec = (miliseconds % 1000) * 1000000;

	do {
		res = nanosleep(&ts, &ts);
	} while ( res && errno == EINTR);

	return res;
}
	

	


void* threadfunc(void* thread_param)
{

    // TODO: wait, obtain mutex, wait, release mutex as described by thread_data structure
    // hint: use a cast like the one below to obtain thread arguments from your parameter
    //struct thread_data* thread_func_args = (struct thread_data *) thread_param;
    	struct thread_data* thread_func_args  = (struct thread_data*) thread_param;

	int obtain_sleep_rc = sleep_ms(thread_func_args->wait_to_obtain_ms);
	if ( obtain_sleep_rc != 0 ) {
		ERROR_LOG("Error during execution of sleep before obtaining mutex lock");
		return thread_param;
	}

	// obtain lock
	int acquire_rc = pthread_mutex_lock(thread_func_args->mutex);
	if ( acquire_rc != 0 ) {
		ERROR_LOG("Unable to obtain lock for mutex");
		return thread_param;
	}


	int release_sleep_rc = sleep_ms(thread_func_args->wait_to_release_ms);
	if ( release_sleep_rc != 0 ) {
		ERROR_LOG("Error during execution of sleep before releasing mutex lock");
		return thread_param;
	}

	int release_rc = pthread_mutex_unlock(thread_func_args->mutex);
	if ( release_rc != 0 ) {
		ERROR_LOG("Error while trying to release mutex lock");
		return thread_param;
	}

	if ( obtain_sleep_rc == 0 &&
			acquire_rc == 0 &&
			release_sleep_rc == 0 &&
			release_rc == 0 ) {

		thread_func_args->thread_complete_success = true;
	}


    	return thread_param;
}


bool start_thread_obtaining_mutex(pthread_t *thread, pthread_mutex_t *mutex,int wait_to_obtain_ms, int wait_to_release_ms)
{
	/**
	 * TODO: allocate memory for thread_data, setup mutex and wait arguments, pass thread_data to created thread
	 * using threadfunc() as entry point.
	 *
	 * return true if successful.
	 *
	 * See implementation details in threading.h file comment block
	 */
    	struct thread_data* thread_func_args = malloc(sizeof(struct thread_data));
	if ( thread_func_args == NULL ) {
		ERROR_LOG("Unable to allocate memory for thread arguments");
		return false;
	}


	thread_func_args->mutex = mutex;
	thread_func_args->wait_to_obtain_ms = wait_to_obtain_ms;
	thread_func_args->wait_to_release_ms = wait_to_release_ms;
	thread_func_args->thread_complete_success = false;


	int rc = pthread_create(thread, NULL, threadfunc, thread_func_args);
	if ( rc != 0 ) {
		ERROR_LOG("Unable to create new thread");
		free(thread_func_args);
		return false;
	}


	DEBUG_LOG("Successfully created thread");
    	return true;
}

