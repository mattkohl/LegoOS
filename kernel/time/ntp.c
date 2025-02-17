/*
 * Copyright (c) 2016-2019 Wuklab, Purdue University. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <lego/time.h>
#include <lego/timex.h>
#include <lego/ktime.h>
#include <lego/kernel.h>
#include <lego/jiffies.h>
#include <lego/spinlock.h>
#include <lego/timekeeping.h>
#include <lego/clocksource.h>

/*
 * NTP timekeeping variables:
 *
 * Note: All of the NTP state is protected by the timekeeping locks.
 */

/* USER_HZ period (usecs): */
unsigned long			tick_usec = TICK_USEC;

/* SHIFTED_HZ period (nsecs): */
unsigned long			tick_nsec;

static u64			tick_length;
static u64			tick_length_base;

#define SECS_PER_DAY		86400
#define MAX_TICKADJ		500LL		/* usecs */
#define MAX_TICKADJ_SCALED \
	(((MAX_TICKADJ * NSEC_PER_USEC) << NTP_SCALE_SHIFT) / NTP_INTERVAL_FREQ)

/*
 * phase-lock loop variables
 */

/*
 * clock synchronization status
 *
 * (TIME_ERROR prevents overwriting the CMOS clock)
 */
static int			time_state = TIME_OK;

/* clock status bits:							*/
static int			time_status = STA_UNSYNC;

/* time adjustment (nsecs):						*/
static s64			time_offset;

/* pll time constant:							*/
static long			time_constant = 2;

/* maximum error (usecs):						*/
static long			time_maxerror = NTP_PHASE_LIMIT;

/* estimated error (usecs):						*/
static long			time_esterror = NTP_PHASE_LIMIT;

/* frequency offset (scaled nsecs/secs):				*/
static s64			time_freq;

/* time at last adjustment (secs):					*/
static time_t			time_reftime;

static long			time_adjust;

/* constant (boot-param configurable) NTP tick adjustment (upscaled)	*/
static s64			ntp_tick_adj;

/* second value of the next pending leapsecond, or TIME_MAX if no leap */
static time_t			ntp_next_leap_sec = TIME_MAX;

/*
 * X86 does not config NTP_PPS
 */

static inline s64 ntp_offset_chunk(s64 offset)
{
	return shift_right(offset, SHIFT_PLL + time_constant);
}

static inline void pps_reset_freq_interval(void) {}
static inline void pps_clear(void) {}
static inline void pps_dec_valid(void) {}
static inline void pps_set_freq(s64 freq) {}

static inline int is_error_status(int status)
{
	return status & (STA_UNSYNC|STA_CLOCKERR);
}

static inline void pps_fill_timex(struct timex *txc)
{
	/* PPS is not implemented, so these are zero */
	txc->ppsfreq	   = 0;
	txc->jitter	   = 0;
	txc->shift	   = 0;
	txc->stabil	   = 0;
	txc->jitcnt	   = 0;
	txc->calcnt	   = 0;
	txc->errcnt	   = 0;
	txc->stbcnt	   = 0;
}

/**
 * ntp_synced - Returns 1 if the NTP status is not UNSYNC
 *
 */
static inline int ntp_synced(void)
{
	return !(time_status & STA_UNSYNC);
}

/*
 * NTP methods:
 */

/*
 * Update (tick_length, tick_length_base, tick_nsec), based
 * on (tick_usec, ntp_tick_adj, time_freq):
 */
static void ntp_update_frequency(void)
{
	u64 second_length;
	u64 new_base;

	second_length		 = (u64)(tick_usec * NSEC_PER_USEC * USER_HZ)
						<< NTP_SCALE_SHIFT;

	second_length		+= ntp_tick_adj;
	second_length		+= time_freq;

	tick_nsec		 = div_u64(second_length, HZ) >> NTP_SCALE_SHIFT;
	new_base		 = div_u64(second_length, NTP_INTERVAL_FREQ);

	/*
	 * Don't wait for the next second_overflow, apply
	 * the change to the tick length immediately:
	 */
	tick_length		+= new_base - tick_length_base;
	tick_length_base	 = new_base;
}

static inline s64 ntp_update_offset_fll(s64 offset64, long secs)
{
	time_status &= ~STA_MODE;

	if (secs < MINSEC)
		return 0;

	if (!(time_status & STA_FLL) && (secs <= MAXSEC))
		return 0;

	time_status |= STA_MODE;

	return div64_long(offset64 << (NTP_SCALE_SHIFT - SHIFT_FLL), secs);
}

static void ntp_update_offset(long offset)
{
	s64 freq_adj;
	s64 offset64;
	long secs;

	if (!(time_status & STA_PLL))
		return;

	if (!(time_status & STA_NANO)) {
		/* Make sure the multiplication below won't overflow */
		offset = clamp(offset, -USEC_PER_SEC, USEC_PER_SEC);
		offset *= NSEC_PER_USEC;
	}

	/*
	 * Scale the phase adjustment and
	 * clamp to the operating range.
	 */
	offset = clamp(offset, -MAXPHASE, MAXPHASE);

	/*
	 * Select how the frequency is to be controlled
	 * and in which mode (PLL or FLL).
	 */
	secs = (long)(__ktime_get_real_seconds() - time_reftime);
	if (unlikely(time_status & STA_FREQHOLD))
		secs = 0;

	time_reftime = __ktime_get_real_seconds();

	offset64    = offset;
	freq_adj    = ntp_update_offset_fll(offset64, secs);

	/*
	 * Clamp update interval to reduce PLL gain with low
	 * sampling rate (e.g. intermittent network connection)
	 * to avoid instability.
	 */
	if (unlikely(secs > 1 << (SHIFT_PLL + 1 + time_constant)))
		secs = 1 << (SHIFT_PLL + 1 + time_constant);

	freq_adj    += (offset64 * secs) <<
			(NTP_SCALE_SHIFT - 2 * (SHIFT_PLL + 2 + time_constant));

	freq_adj    = min(freq_adj + time_freq, MAXFREQ_SCALED);

	time_freq   = max(freq_adj, -MAXFREQ_SCALED);

	time_offset = div_s64(offset64 << NTP_SCALE_SHIFT, NTP_INTERVAL_FREQ);
}

/**
 * ntp_clear - Clears the NTP state variables
 */
void ntp_clear(void)
{
	time_adjust	= 0;		/* stop active adjtime() */
	time_status	|= STA_UNSYNC;
	time_maxerror	= NTP_PHASE_LIMIT;
	time_esterror	= NTP_PHASE_LIMIT;

	ntp_update_frequency();

	tick_length	= tick_length_base;
	time_offset	= 0;

	ntp_next_leap_sec = TIME_MAX;
	/* Clear PPS state variables */
	pps_clear();
}

u64 ntp_tick_length(void)
{
	return tick_length;
}

/**
 * ntp_get_next_leap - Returns the next leapsecond in CLOCK_REALTIME ktime_t
 *
 * Provides the time of the next leapsecond against CLOCK_REALTIME in
 * a ktime_t format. Returns KTIME_MAX if no leapsecond is pending.
 */
ktime_t ntp_get_next_leap(void)
{
	ktime_t ret;

	if ((time_state == TIME_INS) && (time_status & STA_INS))
		return ktime_set(ntp_next_leap_sec, 0);
	ret = KTIME_MAX;
	return ret;
}

/*
 * this routine handles the overflow of the microsecond field
 *
 * The tricky bits of code to handle the accurate clock support
 * were provided by Dave Mills (Mills@UDEL.EDU) of NTP fame.
 * They were originally developed for SUN and DEC kernels.
 * All the kudos should go to Dave for this stuff.
 *
 * Also handles leap second processing, and returns leap offset
 */
int second_overflow(time_t secs)
{
	s64 delta;
	int leap = 0;
	s32 rem;

	/*
	 * Leap second processing. If in leap-insert state at the end of the
	 * day, the system clock is set back one second; if in leap-delete
	 * state, the system clock is set ahead one second.
	 */
	switch (time_state) {
	case TIME_OK:
		if (time_status & STA_INS) {
			time_state = TIME_INS;
			div_s64_rem(secs, SECS_PER_DAY, &rem);
			ntp_next_leap_sec = secs + SECS_PER_DAY - rem;
		} else if (time_status & STA_DEL) {
			time_state = TIME_DEL;
			div_s64_rem(secs + 1, SECS_PER_DAY, &rem);
			ntp_next_leap_sec = secs + SECS_PER_DAY - rem;
		}
		break;
	case TIME_INS:
		if (!(time_status & STA_INS)) {
			ntp_next_leap_sec = TIME_MAX;
			time_state = TIME_OK;
		} else if (secs == ntp_next_leap_sec) {
			leap = -1;
			time_state = TIME_OOP;
			printk(KERN_NOTICE
				"Clock: inserting leap second 23:59:60 UTC\n");
		}
		break;
	case TIME_DEL:
		if (!(time_status & STA_DEL)) {
			ntp_next_leap_sec = TIME_MAX;
			time_state = TIME_OK;
		} else if (secs == ntp_next_leap_sec) {
			leap = 1;
			ntp_next_leap_sec = TIME_MAX;
			time_state = TIME_WAIT;
			printk(KERN_NOTICE
				"Clock: deleting leap second 23:59:59 UTC\n");
		}
		break;
	case TIME_OOP:
		ntp_next_leap_sec = TIME_MAX;
		time_state = TIME_WAIT;
		break;
	case TIME_WAIT:
		if (!(time_status & (STA_INS | STA_DEL)))
			time_state = TIME_OK;
		break;
	}


	/* Bump the maxerror field */
	time_maxerror += MAXFREQ / NSEC_PER_USEC;
	if (time_maxerror > NTP_PHASE_LIMIT) {
		time_maxerror = NTP_PHASE_LIMIT;
		time_status |= STA_UNSYNC;
	}

	/* Compute the phase adjustment for the next second */
	tick_length	 = tick_length_base;

	delta		 = ntp_offset_chunk(time_offset);
	time_offset	-= delta;
	tick_length	+= delta;

	/* Check PPS signal */
	pps_dec_valid();

	if (!time_adjust)
		goto out;

	if (time_adjust > MAX_TICKADJ) {
		time_adjust -= MAX_TICKADJ;
		tick_length += MAX_TICKADJ_SCALED;
		goto out;
	}

	if (time_adjust < -MAX_TICKADJ) {
		time_adjust += MAX_TICKADJ;
		tick_length -= MAX_TICKADJ_SCALED;
		goto out;
	}

	tick_length += (s64)(time_adjust * NSEC_PER_USEC / NTP_INTERVAL_FREQ)
							 << NTP_SCALE_SHIFT;
	time_adjust = 0;

out:
	return leap;
}

int __weak update_persistent_clock(struct timespec now)
{
	return -ENODEV;
}

/*
 * Propagate a new txc->status value into the NTP state:
 */
static inline void process_adj_status(struct timex *txc, struct timespec *ts)
{
	if ((time_status & STA_PLL) && !(txc->status & STA_PLL)) {
		time_state = TIME_OK;
		time_status = STA_UNSYNC;
		ntp_next_leap_sec = TIME_MAX;
		/* restart PPS frequency calibration */
		pps_reset_freq_interval();
	}

	/*
	 * If we turn on PLL adjustments then reset the
	 * reference time to current time.
	 */
	if (!(time_status & STA_PLL) && (txc->status & STA_PLL))
		time_reftime = __ktime_get_real_seconds();

	/* only set allowed bits */
	time_status &= STA_RONLY;
	time_status |= txc->status & ~STA_RONLY;
}

static inline void process_adjtimex_modes(struct timex *txc,
						struct timespec *ts,
						s32 *time_tai)
{
	if (txc->modes & ADJ_STATUS)
		process_adj_status(txc, ts);

	if (txc->modes & ADJ_NANO)
		time_status |= STA_NANO;

	if (txc->modes & ADJ_MICRO)
		time_status &= ~STA_NANO;

	if (txc->modes & ADJ_FREQUENCY) {
		time_freq = txc->freq * PPM_SCALE;
		time_freq = min(time_freq, MAXFREQ_SCALED);
		time_freq = max(time_freq, -MAXFREQ_SCALED);
		/* update pps_freq */
		pps_set_freq(time_freq);
	}

	if (txc->modes & ADJ_MAXERROR)
		time_maxerror = txc->maxerror;

	if (txc->modes & ADJ_ESTERROR)
		time_esterror = txc->esterror;

	if (txc->modes & ADJ_TIMECONST) {
		time_constant = txc->constant;
		if (!(time_status & STA_NANO))
			time_constant += 4;
		time_constant = min(time_constant, (long)MAXTC);
		time_constant = max(time_constant, 0l);
	}

	if (txc->modes & ADJ_TAI && txc->constant > 0)
		*time_tai = txc->constant;

	if (txc->modes & ADJ_OFFSET)
		ntp_update_offset(txc->offset);

	if (txc->modes & ADJ_TICK)
		tick_usec = txc->tick;

	if (txc->modes & (ADJ_TICK|ADJ_FREQUENCY|ADJ_OFFSET))
		ntp_update_frequency();
}

/**
 * ntp_validate_timex - Ensures the timex is ok for use in do_adjtimex
 */
int ntp_validate_timex(struct timex *txc)
{
	if (txc->modes & ADJ_ADJTIME) {
		/* singleshot must not be used with any other mode bits */
		if (!(txc->modes & ADJ_OFFSET_SINGLESHOT))
			return -EINVAL;
		if (!(txc->modes & ADJ_OFFSET_READONLY))
			return -EPERM;
	} else {
		/* In order to modify anything, you gotta be super-user! */
		 if (txc->modes)
			return -EPERM;
		/*
		 * if the quartz is off by more than 10% then
		 * something is VERY wrong!
		 */
		if (txc->modes & ADJ_TICK &&
		    (txc->tick <  900000/USER_HZ ||
		     txc->tick > 1100000/USER_HZ))
			return -EINVAL;
	}

	if (txc->modes & ADJ_SETOFFSET) {
		if (txc->modes & ADJ_NANO) {
			struct timespec ts;

			ts.tv_sec = txc->time.tv_sec;
			ts.tv_nsec = txc->time.tv_usec;
			if (!timespec_inject_offset_valid(&ts))
				return -EINVAL;

		} else {
			if (!timeval_inject_offset_valid(&txc->time))
				return -EINVAL;
		}
	}

	/*
	 * Check for potential multiplication overflows that can
	 * only happen on 64-bit systems:
	 */
	if ((txc->modes & ADJ_FREQUENCY) && (BITS_PER_LONG == 64)) {
		if (LLONG_MIN / PPM_SCALE > txc->freq)
			return -EINVAL;
		if (LLONG_MAX / PPM_SCALE < txc->freq)
			return -EINVAL;
	}

	return 0;
}

void __init ntp_init(void)
{
	ntp_clear();
}
