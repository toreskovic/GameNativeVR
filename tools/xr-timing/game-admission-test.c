/* Production Unix admission gate, with no Vulkan work or game process. */
#include "../../app/src/main/windows/openxr_runtime/unix/gamenative_openxr_unix.c"
#include <assert.h>
static atomic_int entered, finished;
static void *admit(void *unused) {
    (void)unused;
    atomic_store(&entered,1);
    unsigned in_flight; uint64_t budget;
    assert(wait_game_budget(1000000000ull,&in_flight,&budget));
    atomic_store(&finished,1);
    return NULL;
}
int main(void) {
    unsigned in_flight; uint64_t budget;
    pending_submission_count=1;
    assert(wait_game_budget(1000000,&in_flight,&budget));
    assert(in_flight==1 && budget==0); // CPU can overlap one prior GPU frame
    pending_submission_count=2;
    assert(!wait_game_budget(1000000,&in_flight,&budget)); // no silent bypass on timeout
    pthread_t thread; assert(!pthread_create(&thread,NULL,admit,NULL));
    while(!atomic_load(&entered)) sched_yield();
    struct timespec delay={0,10000000}; nanosleep(&delay,NULL);
    assert(!atomic_load(&finished));
    // The condition wait releases the mutex; GPU retirement can always progress.
    pthread_mutex_lock(&submit_mutex);
    pending_submission_count=1;
    pthread_cond_broadcast(&submit_space_cond);
    pthread_mutex_unlock(&submit_mutex);
    pthread_join(thread,NULL); assert(atomic_load(&finished));
    submission_gpu_failed=1; assert(!wait_game_budget(1000000,NULL,NULL));
    submission_gpu_failed=0; transport_ownership_failed=1;
    assert(!wait_game_budget(1000000,NULL,NULL));
    transport_ownership_failed=0;
    game_starts[0].target=200; game_starts[0].started_ns=1234;
    assert(!consume_game_start_locked(300));
    assert(consume_game_start_locked(200)==1234 && !consume_game_start_locked(200));
    puts("One-frame overlap, two-submission backpressure, asynchronous retirement wakeup, timeout/failure and exact target matching passed");
}
