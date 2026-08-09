#ifndef KMASK_RUN_H
#define KMASK_RUN_H

#include "kilix_mask_edit.h"

/*
 * Run the interactive editor on this terminal until the operator quits.
 *
 * The mask is borrowed; saving happens through `path`, which may be NULL
 * for a session that can only be looked at.  `rect_cap` is the consumer's
 * obstacle budget, shown against the decomposition count and flagged when
 * exceeded, or 0 when there is none - the module has no opinion about
 * what a reasonable budget is, only about reporting it.
 *
 * Returns a process exit status: 0 for a clean quit, 1 for a terminal
 * that could not be used.
 */
int kmask_run(kmaskedit *editor, kmask *mask, const char *path, int rect_cap);

#endif /* KMASK_RUN_H */
