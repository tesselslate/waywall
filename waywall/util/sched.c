#include "util/log.h"
#include <pthread.h>

/*
 * This code is based on sway's code for managing scheduler priority.
 *
 * Copyright (c) 2016-2017 Drew DeVault
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

static int orig_sched_scheduler;
static struct sched_param orig_sched_params;

static void
on_fork() {
    int ret = pthread_setschedparam(pthread_self(), orig_sched_scheduler, &orig_sched_params);
    if (ret != 0) {
        ww_log_errno(LOG_ERROR, "failed to reset scheduler priority for child process");
    }
}

void
util_sched_realtime() {
    int ret = pthread_getschedparam(pthread_self(), &orig_sched_scheduler, &orig_sched_params);
    if (ret != 0) {
        ww_log_errno(LOG_ERROR, "failed to get original scheduling policy");
        return;
    }

    ww_log(LOG_INFO, "original scheduling policy: %d (priority %d)", orig_sched_scheduler,
           orig_sched_params.sched_priority);

    int priority = sched_get_priority_min(SCHED_RR);
    ww_assert(priority != -1);

    const struct sched_param param = {.sched_priority = priority};
    if (pthread_setschedparam(pthread_self(), SCHED_RR, &param) != 0) {
        ww_log_errno(LOG_WARN, "failed to set scheduler priority");
        return;
    }

    ww_log(LOG_INFO, "using SCHED_RR with priority %d", priority);
    pthread_atfork(NULL, NULL, on_fork);
}
