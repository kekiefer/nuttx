/****************************************************************************
 * arch/arm/src/stm32/stm32_tickless.c
 *
 *   Copyright (C) 2015 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *           David Sidrane <david_s5@nscdg.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/
/****************************************************************************
 * Tickless OS Support.
 *
 * When CONFIG_SCHED_TICKLESS is enabled, all support for timer interrupts
 * is suppressed and the platform specific code is expected to provide the
 * following custom functions.
 *
 *   void up_timer_initialize(void): Initializes the timer facilities.  Called
 *     early in the intialization sequence (by up_intialize()).
 *   int up_timer_gettime(FAR struct timespec *ts):  Returns the current
 *     time from the platform specific time source.
 *   int up_timer_cancel(void):  Cancels the interval timer.
 *   int up_timer_start(FAR const struct timespec *ts): Start (or re-starts)
 *     the interval timer.
 *
 * The RTOS will provide the following interfaces for use by the platform-
 * specific interval timer implementation:
 *
 *   void sched_timer_expiration(void):  Called by the platform-specific
 *     logic when the interval timer expires.
 *
 ****************************************************************************/
/****************************************************************************
 * STM32 Timer Usage
 *
 * This current implementation uses two timers:  A one-shot timer to provide
 * the timed events and a free running timer to provide the current time.
 * Since timers are a limited resource, that could be an issue on some
 * systems.
 *
 * We could do the job with a single timer if we were to keep the single
 * timer in a free-running at all times.  The STM32 timer/counters have
 * 32-bit counters with the capability to generate a compare interrupt when
 * the timer matches a compare value but also to continue counting without
 * stopping (giving another, different interrupt when the timer rolls over
 * from 0xffffffff to zero).  So we could potentially just set the compare
 * at the number of ticks you want PLUS the current value of timer.  Then
 * you could have both with a single timer:  An interval timer and a free-
 * running counter with the same timer!
 *
 * Patches are welcome!
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdbool.h>

#include <nuttx/arch.h>
#include <arch/board/board.h> // delete me with PROBES

#include "up_internal.h"
#include "stm32_tim.h"
#include "stm32_oneshot.h"
#include "stm32_freerun.h"

#ifdef CONFIG_SCHED_TICKLESS

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Debug ********************************************************************/
/* Non-standard debug that may be enabled just for testing the watchdog
 * timer
 */

#ifndef CONFIG_DEBUG
#  undef CONFIG_DEBUG_TIMER
#endif

#ifdef CONFIG_DEBUG_TIMER
#  define tcdbg                 dbg
#  define tclldbg               lldbg
#  ifdef CONFIG_DEBUG_VERBOSE
#    define tcvdbg              vdbg
#    define tcllvdbg            llvdbg
#  else
#    define tcvdbg(x...)
#    define tcllvdbg(x...)
#  endif
#else
#  define tcdbg(x...)
#  define tclldbg(x...)
#  define tcvdbg(x...)
#  define tcllvdbg(x...)
#endif

#ifndef CONFIG_STM32_ONESHOT
#  error CONFIG_STM32_ONESHOT must be selected for the Tickless OS option
#endif

#ifndef CONFIG_STM32_ONESHOT
#  error CONFIG_STM32_FREERUN must be selected for the Tickless OS option
#endif

#ifndef CONFIG_STM32_TICKLESS_FREERUN
#  error CONFIG_STM32_TICKLESS_FREERUN must be selected for the Tickless OS option
#endif

#ifndef CONFIG_STM32_TICKLESS_ONESHOT
#  error CONFIG_STM32_TICKLESS_ONESHOT must be selected for the Tickless OS option
#endif

#if CONFIG_STM32_TICKLESS_ONESHOT == 1 && !defined(CONFIG_STM32_TIM1)
#  error CONFIG_STM32_TICKLESS_ONESHOT == 1 && CONFIG_STM32_TIM1 not selected
#elif CONFIG_STM32_TICKLESS_ONESHOT == 2 && !defined(CONFIG_STM32_TIM2)
#  error CONFIG_STM32_TICKLESS_ONESHOT == 2 && CONFIG_STM32_TIM2 not selected
#elif CONFIG_STM32_TICKLESS_ONESHOT == 3 && !defined(CONFIG_STM32_TIM3)
#  error CONFIG_STM32_TICKLESS_ONESHOT == 3 && CONFIG_STM32_TIM3 not selected
#elif CONFIG_STM32_TICKLESS_ONESHOT == 4 && !defined(CONFIG_STM32_TIM4)
#  error CONFIG_STM32_TICKLESS_ONESHOT == 4 && CONFIG_STM32_TIM4 not selected
#elif CONFIG_STM32_TICKLESS_ONESHOT == 5 && !defined(CONFIG_STM32_TIM5)
#  error CONFIG_STM32_TICKLESS_ONESHOT == 5 && CONFIG_STM32_TIM5 not selected
#elif CONFIG_STM32_TICKLESS_ONESHOT == 6 && !defined(CONFIG_STM32_TIM6)
#  error CONFIG_STM32_TICKLESS_ONESHOT == 6 && CONFIG_STM32_TIM6 not selected
#elif CONFIG_STM32_TICKLESS_ONESHOT == 7 && !defined(CONFIG_STM32_TIM7)
#  error CONFIG_STM32_TICKLESS_ONESHOT == 7 && CONFIG_STM32_TIM7 not selected
#elif CONFIG_STM32_TICKLESS_ONESHOT == 8 && !defined(CONFIG_STM32_TIM8)
#  error CONFIG_STM32_TICKLESS_ONESHOT == 8 && CONFIG_STM32_TIM8 not selected
#elif CONFIG_STM32_TICKLESS_ONESHOT == 9 && !defined(CONFIG_STM32_TIM9)
#  error CONFIG_STM32_TICKLESS_ONESHOT == 9 && CONFIG_STM32_TIM9 not selected
#elif CONFIG_STM32_TICKLESS_ONESHOT == 10 && !defined(CONFIG_STM32_TIM10)
#  error CONFIG_STM32_TICKLESS_ONESHOT == 10 && CONFIG_STM32_TIM10 not selected
#elif CONFIG_STM32_TICKLESS_ONESHOT == 11 && !defined(CONFIG_STM32_TIM11)
#  error CONFIG_STM32_TICKLESS_ONESHOT == 11 && CONFIG_STM32_TIM11 not selected
#elif CONFIG_STM32_TICKLESS_ONESHOT == 12 && !defined(CONFIG_STM32_TIM12)
#  error CONFIG_STM32_TICKLESS_ONESHOT == 12 && CONFIG_STM32_TIM12 not selected
#elif CONFIG_STM32_TICKLESS_ONESHOT == 13 && !defined(CONFIG_STM32_TIM13)
#  error CONFIG_STM32_TICKLESS_ONESHOT == 13 && CONFIG_STM32_TIM13 not selected
#elif CONFIG_STM32_TICKLESS_ONESHOT == 14 && !defined(CONFIG_STM32_TIM14)
#  error CONFIG_STM32_TICKLESS_ONESHOT == 14 && CONFIG_STM32_TIM14 not selected
#endif

#if CONFIG_STM32_TICKLESS_ONESHOT < 1 || CONFIG_STM32_TICKLESS_ONESHOT > 14
#  error CONFIG_STM32_TICKLESS_ONESHOT is not valid
#endif

#if CONFIG_STM32_TICKLESS_FREERUN == 1 && !defined(CONFIG_STM32_TIM1)
#  error CONFIG_STM32_TICKLESS_FREERUN == 1 && CONFIG_STM32_TIM1 not selected
#elif CONFIG_STM32_TICKLESS_FREERUN == 2 && !defined(CONFIG_STM32_TIM2)
#  error CONFIG_STM32_TICKLESS_FREERUN == 2 && CONFIG_STM32_TIM2 not selected
#elif CONFIG_STM32_TICKLESS_FREERUN == 3 && !defined(CONFIG_STM32_TIM3)
#  error CONFIG_STM32_TICKLESS_FREERUN == 3 && CONFIG_STM32_TIM3 not selected
#elif CONFIG_STM32_TICKLESS_FREERUN == 4 && !defined(CONFIG_STM32_TIM4)
#  error CONFIG_STM32_TICKLESS_FREERUN == 4 && CONFIG_STM32_TIM4 not selected
#elif CONFIG_STM32_TICKLESS_FREERUN == 5 && !defined(CONFIG_STM32_TIM5)
#  error CONFIG_STM32_TICKLESS_FREERUN == 5 && CONFIG_STM32_TIM5 not selected
#elif CONFIG_STM32_TICKLESS_FREERUN == 6 && !defined(CONFIG_STM32_TIM6)
#  error CONFIG_STM32_TICKLESS_FREERUN == 6 && CONFIG_STM32_TIM6 not selected
#elif CONFIG_STM32_TICKLESS_FREERUN == 7 && !defined(CONFIG_STM32_TIM7)
#  error CONFIG_STM32_TICKLESS_FREERUN == 7 && CONFIG_STM32_TIM7 not selected
#elif CONFIG_STM32_TICKLESS_FREERUN == 8 && !defined(CONFIG_STM32_TIM8)
#  error CONFIG_STM32_TICKLESS_FREERUN == 8 && CONFIG_STM32_TIM8 not selected
#elif CONFIG_STM32_TICKLESS_FREERUN == 9 && !defined(CONFIG_STM32_TIM9)
#  error CONFIG_STM32_TICKLESS_FREERUN == 9 && CONFIG_STM32_TIM9 not selected
#elif CONFIG_STM32_TICKLESS_FREERUN == 10 && !defined(CONFIG_STM32_TIM10)
#  error CONFIG_STM32_TICKLESS_FREERUN == 10 && CONFIG_STM32_TIM10 not selected
#elif CONFIG_STM32_TICKLESS_FREERUN == 11 && !defined(CONFIG_STM32_TIM11)
#  error CONFIG_STM32_TICKLESS_FREERUN == 11 && CONFIG_STM32_TIM11 not selected
#elif CONFIG_STM32_TICKLESS_FREERUN == 12 && !defined(CONFIG_STM32_TIM12)
#  error CONFIG_STM32_TICKLESS_FREERUN == 12 && CONFIG_STM32_TIM12 not selected
#elif CONFIG_STM32_TICKLESS_FREERUN == 13 && !defined(CONFIG_STM32_TIM13)
#  error CONFIG_STM32_TICKLESS_FREERUN == 13 && CONFIG_STM32_TIM13 not selected
#elif CONFIG_STM32_TICKLESS_FREERUN == 14 && !defined(CONFIG_STM32_TIM14)
#  error CONFIG_STM32_TICKLESS_FREERUN == 14 && CONFIG_STM32_TIM14 not selected
#endif


#if CONFIG_STM32_TICKLESS_FREERUN < 1 || CONFIG_STM32_TICKLESS_FREERUN > 14
#  error CONFIG_STM32_TICKLESS_FREERUN is not valid
#endif

#if CONFIG_STM32_TICKLESS_FREERUN == CONFIG_STM32_TICKLESS_ONESHOT
#  error CONFIG_STM32_TICKLESS_FREERUN is the same as CONFIG_STM32_TICKLESS_ONESHOT
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct stm32_tickless_s
{
  struct stm32_oneshot_s oneshot;
  struct stm32_freerun_s freerun;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Private Data
 ****************************************************************************/

struct stm32_tickless_s g_tickless;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: stm32_oneshot_handler
 *
 * Description:
 *   Called when the one shot timer expires
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Called early in the initialization sequence before any special
 *   concurrency protections are required.
 *
 ****************************************************************************/
static volatile int test_cnt = 0;
static void stm32_oneshot_handler(void *arg)
{
  tcllvdbg("Expired...\n");
  test_cnt++;
  sched_timer_expiration();
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: up_timer_initialize
 *
 * Description:
 *   Initializes all platform-specific timer facilities.  This function is
 *   called early in the initialization sequence by up_intialize().
 *   On return, the current up-time should be available from
 *   up_timer_gettime() and the interval timer is ready for use (but not
 *   actively timing.
 *
 *   Provided by platform-specific code and called from the architecture-
 *   specific logic.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Called early in the initialization sequence before any special
 *   concurrency protections are required.
 *
 ****************************************************************************/

void up_timer_initialize(void)
{
  int ret;

  /* Initialize the one-shot timer */

  ret = stm32_oneshot_initialize(&g_tickless.oneshot,
                               CONFIG_STM32_TICKLESS_ONESHOT,
                               CONFIG_USEC_PER_TICK);
  if (ret < 0)
    {
      tclldbg("ERROR: stm32_oneshot_initialize failed\n");
      PANIC();
    }

  /* Initialize the free-running timer */

  ret = stm32_freerun_initialize(&g_tickless.freerun,
                               CONFIG_STM32_TICKLESS_FREERUN,
                               CONFIG_USEC_PER_TICK);
  if (ret < 0)
    {
      tclldbg("ERROR: stm32_freerun_initialize failed\n");
      PANIC();
    }

  hrt_init();
  volatile uint64_t bk = 0;
  volatile uint64_t st = 0;
  volatile uint64_t delay = 0;
  volatile struct timespec ts;
  volatile struct timespec remain;
  volatile uint64_t Ts;
  volatile uint64_t Tn;
  volatile int s;
  volatile uint64_t D;
  volatile uint64_t Dt;
  for ( s=0; s < 5; s++) {

      for (int i=50; i < 15000; i += 100) {

          int j = 2;

          while(j--) {
              ts.tv_nsec = i * 1000;
              ts.tv_sec = s;

              int cnt = test_cnt;
              PROBE(3,true);
              delay = ts.tv_nsec/1000 + (ts.tv_sec * 1000000);
              up_timer_start(&ts);
              Ts = hrt_absolute_time();
              while(cnt == test_cnt){
                  Tn = hrt_absolute_time();
                  D = Tn - Ts;
                  if (D >= (delay/2)) {
                    up_timer_cancel(&remain);
                    bk = remain.tv_nsec/1000 + (remain.tv_sec * 1000000);
                    Dt = (bk - delay/2);
                    if (bk == delay/2) {
                        st++;
                    } else {
                        st--;
                    }
                    break;

                  }
              }
              PROBE(3,false);
          }
       }
    }
}

/****************************************************************************
 * Name: up_timer_gettime
 *
 * Description:
 *   Return the elapsed time since power-up (or, more correctly, since
 *   up_timer_initialize() was called).  This function is functionally
 *   equivalent to:
 *
 *      int clock_gettime(clockid_t clockid, FAR struct timespec *ts);
 *
 *   when clockid is CLOCK_MONOTONIC.
 *
 *   This function provides the basis for reporting the current time and
 *   also is used to eliminate error build-up from small erros in interval
 *   time calculations.
 *
 *   Provided by platform-specific code and called from the RTOS base code.
 *
 * Input Parameters:
 *   ts - Provides the location in which to return the up-time.
 *
 * Returned Value:
 *   Zero (OK) is returned on success; a negated errno value is returned on
 *   any failure.
 *
 * Assumptions:
 *   Called from the the normal tasking context.  The implementation must
 *   provide whatever mutual exclusion is necessary for correct operation.
 *   This can include disabling interrupts in order to assure atomic register
 *   operations.
 *
 ****************************************************************************/

int up_timer_gettime(FAR struct timespec *ts)
{
  return stm32_freerun_counter(&g_tickless.freerun, ts);
}

/****************************************************************************
 * Name: up_timer_cancel
 *
 * Description:
 *   Cancel the interval timer and return the time remaining on the timer.
 *   These two steps need to be as nearly atomic as possible.
 *   sched_timer_expiration() will not be called unless the timer is
 *   restarted with up_timer_start().
 *
 *   If, as a race condition, the timer has already expired when this
 *   function is called, then that pending interrupt must be cleared so
 *   that up_timer_start() and the remaining time of zero should be
 *   returned.
 *
 *   NOTE: This function may execute at a high rate with no timer running (as
 *   when pre-emption is enabled and disabled).
 *
 *   Provided by platform-specific code and called from the RTOS base code.
 *
 * Input Parameters:
 *   ts - Location to return the remaining time.  Zero should be returned
 *        if the timer is not active.  ts may be zero in which case the
 *        time remaining is not returned.
 *
 * Returned Value:
 *   Zero (OK) is returned on success.  A call to up_timer_cancel() when
 *   the timer is not active should also return success; a negated errno
 *   value is returned on any failure.
 *
 * Assumptions:
 *   May be called from interrupt level handling or from the normal tasking
 *   level.  Interrupts may need to be disabled internally to assure
 *   non-reentrancy.
 *
 ****************************************************************************/

int up_timer_cancel(FAR struct timespec *ts)
{
  return stm32_oneshot_cancel(&g_tickless.oneshot, ts);
}

/****************************************************************************
 * Name: up_timer_start
 *
 * Description:
 *   Start the interval timer.  sched_timer_expiration() will be
 *   called at the completion of the timeout (unless up_timer_cancel
 *   is called to stop the timing.
 *
 *   Provided by platform-specific code and called from the RTOS base code.
 *
 * Input Parameters:
 *   ts - Provides the time interval until sched_timer_expiration() is
 *        called.
 *
 * Returned Value:
 *   Zero (OK) is returned on success; a negated errno value is returned on
 *   any failure.
 *
 * Assumptions:
 *   May be called from interrupt level handling or from the normal tasking
 *   level.  Interrupts may need to be disabled internally to assure
 *   non-reentrancy.
 *
 ****************************************************************************/

int up_timer_start(FAR const struct timespec *ts)
{
  return stm32_oneshot_start(&g_tickless.oneshot, stm32_oneshot_handler, NULL, ts);
}
#endif /* CONFIG_SCHED_TICKLESS */
