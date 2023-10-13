#ifndef WAYWALL_RESET_COUNTER_H
#define WAYWALL_RESET_COUNTER_H

#include <stdbool.h>

/*
 *  Counts the number of resets the user has performed.
 */
struct reset_counter;

/*
 *  Attempts to change which file the reset_counter uses. If successful, the current count is
 *  committed to the previous file and the reset count from the new file is used in its place.
 *
 *  Returns true on success, false on failure.
 */
bool reset_counter_change_file(struct reset_counter *counter, const char *path);

/*
 *  Attempts to commit any buffered reset count updates to disk.
 */
void reset_counter_commit_writes(struct reset_counter *counter);

/*
 *  Frees all resources associated with the reset_counter, at which point it is invalid.
 */
void reset_counter_destroy(struct reset_counter *counter);

/*
 *  Attempts to create a new reset_counter from the given file path. Returns NULL on failure.
 */
struct reset_counter *reset_counter_from_file(const char *path);

/*
 *  Returns the current number of resets tracked by the reset_counter.
 */
int reset_counter_get_count(struct reset_counter *counter);

/*
 *  Increments the reset_counter. If write queueing is disabled, the change will be written to disk
 *  immediately; otherwise, it will be written when reset_counter_commit_writes is next called
 * (which may be automatically by the reset_counter if needed.)
 */
int reset_counter_increment(struct reset_counter *counter);

/*
 *  Temporarily disables immediate writes to disk. This can be used to prevent needless IO syscalls
 *  when several resets may happen in quick succession. Immediate writes can be reenabled with
 *  reset_counter_commit_writes, which may be called automatically by the reset_counter if needed.
 */
void reset_counter_queue_writes(struct reset_counter *counter);

#endif
