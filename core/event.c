/*
 * Copyright (C) 2018-2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <schedule.h>
#include <event.h>
#include <logmsg.h>
#include <cpu.h>

/*
 * Scheduler event principle:
 *
 * sched_event is a tiny exclusive wait primitive for one scheduler thread. It
 * is not a counting semaphore and not a multi-waiter queue. The state is just:
 *
 *   +-------------------+
 *   | set flag          |
 *   | waiting_thread    |
 *   +-------------------+
 *
 * Wait path:
 *
 *   current thread
 *        |
 *        v
 *   event.lock
 *     - record waiting_thread
 *     - while !set: sleep_thread(waiter), drop lock, schedule()
 *        |
 *        v
 *   signal wakes waiter or leaves set=true for the waiter to observe
 *
 * Signal path:
 *
 *   signal_event()
 *        |
 *        v
 *   event.lock
 *     - set = true
 *     - wake waiting_thread if present
 *
 * The wakeup is not lost when signal_event() races with schedule(): the event
 * flag remains set under the event lock, and wake_thread() raises scheduler
 * state so the waiter or idle thread will re-check scheduling before continuing.
 */
void init_event(struct sched_event *event)
{
	spinlock_init(&event->lock);
	event->set = false;
	event->waiting_thread = NULL;
}

void reset_event(struct sched_event *event)
{
	uint64_t rflag;

	spinlock_irqsave_obtain(&event->lock, &rflag);
	event->set = false;
	event->waiting_thread = NULL;
	spinlock_irqrestore_release(&event->lock, rflag);
}

/* support exclusive waiting only
 *
 * During wait, the pCPU could be scheduled to run the idle thread when run queue
 * is empty. Signal_event() can happen when schedule() is in process.
 * This signal_event is not going to be lost, for the idle thread will always
 * check need_reschedule() after it is switched to at schedule().
 */
void wait_event(struct sched_event *event)
{
	uint64_t rflag;

	spinlock_irqsave_obtain(&event->lock, &rflag);
	ASSERT((event->waiting_thread == NULL), "only support exclusive waiting");
	event->waiting_thread = sched_get_current(get_pcpu_id());
	while (!event->set && (event->waiting_thread != NULL)) {
		sleep_thread(event->waiting_thread);
		spinlock_irqrestore_release(&event->lock, rflag);
		schedule();
		spinlock_irqsave_obtain(&event->lock, &rflag);
	}
	event->set = false;
	event->waiting_thread = NULL;
	spinlock_irqrestore_release(&event->lock, rflag);
}

void signal_event(struct sched_event *event)
{
	uint64_t rflag;

	spinlock_irqsave_obtain(&event->lock, &rflag);
	event->set = true;
	if (event->waiting_thread != NULL) {
		wake_thread(event->waiting_thread);
	}
	spinlock_irqrestore_release(&event->lock, rflag);
}
