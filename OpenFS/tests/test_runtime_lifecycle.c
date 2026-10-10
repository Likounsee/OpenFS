#include "openfs/runtime.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#if defined(_WIN32)
#include <windows.h>
#include <process.h>
typedef HANDLE test_thread_t;
#else
#include <pthread.h>
#include <sched.h>
typedef pthread_t test_thread_t;
#endif

static openfs_runtime_t runtime;
static atomic_int worker_entered;
static atomic_int release_worker;
static atomic_int shutdown_done;
static atomic_int shutdown_result;

#if defined(_WIN32)
static unsigned __stdcall active_worker(void *unused)
#else
static void *active_worker(void *unused)
#endif
{
    (void)unused;
    if (!openfs_runtime_enter(&runtime)) abort();
    /* A nested admission must not release the outer active-user pin. */
    if (!openfs_runtime_enter(&runtime)) abort();
    openfs_runtime_leave(&runtime);
    atomic_store_explicit(&worker_entered, 1, memory_order_release);
    while (atomic_load_explicit(&release_worker, memory_order_acquire) == 0) {
#if defined(_WIN32)
        Sleep(0);
#else
        sched_yield();
#endif
    }
    openfs_runtime_leave(&runtime);
#if defined(_WIN32)
    return 0U;
#else
    return NULL;
#endif
}

#if defined(_WIN32)
static unsigned __stdcall shutdown_worker(void *unused)
#else
static void *shutdown_worker(void *unused)
#endif
{
    (void)unused;
    atomic_store_explicit(&shutdown_result,
                          openfs_runtime_shutdown_if_unused(&runtime),
                          memory_order_release);
    atomic_store_explicit(&shutdown_done, 1, memory_order_release);
#if defined(_WIN32)
    return 0U;
#else
    return NULL;
#endif
}

static int start_thread(test_thread_t *thread,
#if defined(_WIN32)
                        unsigned (__stdcall *fn)(void *)
#else
                        void *(*fn)(void *)
#endif
)
{
#if defined(_WIN32)
    uintptr_t h = _beginthreadex(NULL, 0U, fn, NULL, 0U, NULL);
    *thread = (HANDLE)h;
    return h != 0U;
#else
    return pthread_create(thread, NULL, fn, NULL) == 0;
#endif
}

static void join_thread(test_thread_t thread)
{
#if defined(_WIN32)
    (void)WaitForSingleObject(thread, INFINITE);
    (void)CloseHandle(thread);
#else
    (void)pthread_join(thread, NULL);
#endif
}

int main(void)
{
    test_thread_t worker;
    test_thread_t shutdown;
    int admission_closed = 0;

    atomic_init(&worker_entered, 0);
    atomic_init(&release_worker, 0);
    atomic_init(&shutdown_done, 0);
    atomic_init(&shutdown_result, 0);
    if (!openfs_runtime_init(&runtime)) return 1;

    /*
     * An open handle must prevent unused-shutdown from destroying the
     * runtime. A rejected shutdown must also restore admission so the caller
     * can release the handle and continue using the runtime.
     */
    static int fake_device;
    if (!openfs_runtime_handle_acquire(&runtime, &fake_device, 1U, 1U)) {
        openfs_runtime_destroy(&runtime);
        return 1;
    }
    if (openfs_runtime_shutdown_if_unused(&runtime) != 0) {
        fprintf(stderr, "runtime shutdown succeeded while a handle was open\n");
        return 1;
    }
    if (!openfs_runtime_enter(&runtime)) {
        fprintf(stderr, "runtime admission was not restored after rejected shutdown\n");
        return 1;
    }
    int accepting = openfs_runtime_is_accepting(&runtime);
    openfs_runtime_leave(&runtime);
    if (!accepting ||
        openfs_runtime_handle_count(&runtime, &fake_device, 1U, 1U) != 1U ||
        !openfs_runtime_handle_release(&runtime, &fake_device, 1U, 1U) ||
        openfs_runtime_handle_count(&runtime, &fake_device, 1U, 1U) != 0U) {
        fprintf(stderr, "runtime handle accounting failed after rejected shutdown\n");
        openfs_runtime_destroy(&runtime);
        return 1;
    }

    if (!start_thread(&worker, active_worker)) {
        openfs_runtime_destroy(&runtime);
        return 1;
    }
    while (atomic_load_explicit(&worker_entered, memory_order_acquire) == 0) {
#if defined(_WIN32)
        Sleep(0);
#else
        sched_yield();
#endif
    }

    if (!start_thread(&shutdown, shutdown_worker)) {
        atomic_store_explicit(&release_worker, 1, memory_order_release);
        join_thread(worker);
        openfs_runtime_destroy(&runtime);
        return 1;
    }
    /*
     * Shutdown must stop admitting new users, then wait for the already
     * active worker. Probe admission until it is rejected before releasing
     * that worker; this proves shutdown actually reached its quiescence wait.
     */
    for (unsigned i = 0U; i < 10000000U; ++i) {
        if (!openfs_runtime_enter(&runtime)) {
            admission_closed = 1;
            break;
        }
        openfs_runtime_leave(&runtime);
#if defined(_WIN32)
        Sleep(0);
#else
        sched_yield();
#endif
    }

    atomic_store_explicit(&release_worker, 1, memory_order_release);
    join_thread(worker);
    join_thread(shutdown);

    if (!admission_closed ||
        atomic_load_explicit(&shutdown_done, memory_order_acquire) != 1 ||
        atomic_load_explicit(&shutdown_result, memory_order_acquire) != 1) {
        fprintf(stderr, "runtime shutdown did not close admission and drain active users\n");
        return 1;
    }
    puts("runtime lifecycle quiescence test passed");
    return 0;
}
