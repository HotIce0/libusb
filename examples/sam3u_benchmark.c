/*
 * libusb example program to measure Atmel SAM3U isochronous performance
 * Copyright (C) 2012 Harald Welte <laforge@gnumonks.org>
 *
 * Copied with the author's permission under LGPL-2.1 from
 * http://git.gnumonks.org/cgi-bin/gitweb.cgi?p=sam3u-tests.git;a=blob;f=usb-benchmark-project/host/benchmark.c;h=74959f7ee88f1597286cd435f312a8ff52c56b7e
 *
 * An Atmel SAM3U test firmware is also available in the above repository.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <config.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#include <time.h>

#include "libusb.h"

#if !defined(NSEC_PER_SEC)
#define NSEC_PER_SEC	1000000000L
#endif

#define EP_DATA_IN	0x82
#define EP_ISO_IN	0x86

static volatile sig_atomic_t do_exit = 0;
static struct libusb_device_handle *devh = NULL;

static unsigned long num_bytes = 0, num_xfer = 0;
static struct timeval tv_start;

static void get_timestamp(struct timeval *tv)
{
#if defined(PLATFORM_WINDOWS)
	static LARGE_INTEGER frequency;
	LARGE_INTEGER counter;

	if (!frequency.QuadPart)
		QueryPerformanceFrequency(&frequency);

	QueryPerformanceCounter(&counter);
	counter.QuadPart *= 1000000;
	counter.QuadPart /= frequency.QuadPart;

	tv->tv_sec = (long)(counter.QuadPart / 1000000ULL);
	tv->tv_usec = (long)(counter.QuadPart % 1000000ULL);
#elif defined(HAVE_CLOCK_GETTIME)
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	tv->tv_sec = ts.tv_sec;
	tv->tv_usec = (int)(ts.tv_nsec / 1000L);
#else
	gettimeofday(tv, NULL);
#endif
}

static void LIBUSB_CALL cb_xfr(struct libusb_transfer *xfr)
{
	int i;

	if (xfr->status != LIBUSB_TRANSFER_COMPLETED) {
		fprintf(stderr, "transfer status %d\n", xfr->status);
		libusb_free_transfer(xfr);
		exit(3);
	}

	if (xfr->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
		for (i = 0; i < xfr->num_iso_packets; i++) {
			struct libusb_iso_packet_descriptor *pack = &xfr->iso_packet_desc[i];

			if (pack->status != LIBUSB_TRANSFER_COMPLETED) {
				fprintf(stderr, "Error: pack %d status %d\n", i, pack->status);
				exit(5);
			}

			printf("pack%d length:%u, actual_length:%u\n", i, pack->length, pack->actual_length);
		}
	}

	printf("length:%u, actual_length:%u\n", xfr->length, xfr->actual_length);
	for (i = 0; i < xfr->actual_length; i++) {
		printf("%02x", xfr->buffer[i]);
		if (i % 16)
			printf("\n");
		else if (i % 8)
			printf("  ");
		else
			printf(" ");
	}
	num_bytes += xfr->actual_length;
	num_xfer++;

	if (libusb_submit_transfer(xfr) < 0) {
		fprintf(stderr, "error re-submitting URB\n");
		exit(1);
	}
}

static int benchmark_in(uint8_t ep)
{
	static uint8_t buf[2048];
	static struct libusb_transfer *xfr;
	int num_iso_pack = 0;

	if (ep == EP_ISO_IN)
		num_iso_pack = 16;

	xfr = libusb_alloc_transfer(num_iso_pack);
	if (!xfr) {
		errno = ENOMEM;
		return -1;
	}

	if (ep == EP_ISO_IN) {
		libusb_fill_iso_transfer(xfr, devh, ep, buf,
				sizeof(buf), num_iso_pack, cb_xfr, NULL, 0);
		libusb_set_iso_packet_lengths(xfr, sizeof(buf)/num_iso_pack);
	} else
		libusb_fill_bulk_transfer(xfr, devh, ep, buf,
				sizeof(buf), cb_xfr, NULL, 0);

	get_timestamp(&tv_start);

	/* NOTE: To reach maximum possible performance the program must
	 * submit *multiple* transfers here, not just one.
	 *
	 * When only one transfer is submitted there is a gap in the bus
	 * schedule from when the transfer completes until a new transfer
	 * is submitted by the callback. This causes some jitter for
	 * isochronous transfers and loss of throughput for bulk transfers.
	 *
	 * This is avoided by queueing multiple transfers in advance, so
	 * that the host controller is always kept busy, and will schedule
	 * more transfers on the bus while the callback is running for
	 * transfers which have completed on the bus.
	 */

	return libusb_submit_transfer(xfr);
}

static void measure(void)
{
	struct timeval tv_stop;
	unsigned long diff_msec;

	get_timestamp(&tv_stop);

	long diff_sec = tv_stop.tv_sec - tv_start.tv_sec;
	long diff_usec = tv_stop.tv_usec - tv_start.tv_usec;
	if (diff_usec < 0) {
		diff_usec += NSEC_PER_SEC;
		diff_sec--;
	}
	diff_msec = (unsigned long)diff_sec * 1000UL;
	diff_msec += (unsigned long)diff_usec / 1000UL;

	printf("%lu transfers (total %lu bytes) in %lu milliseconds => %lu bytes/sec\n",
		num_xfer, num_bytes, diff_msec, (num_bytes * 1000L) / diff_msec);
}

static void sig_hdlr(int signum)
{
	(void)signum;

	measure();
	do_exit = 1;
}

int main(void)
{
	int rc;

#if defined(PLATFORM_POSIX)
	struct sigaction sigact;

	sigact.sa_handler = sig_hdlr;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	(void)sigaction(SIGINT, &sigact, NULL);
#else
	(void)signal(SIGINT, sig_hdlr);
#endif

	rc = libusb_init_context(/*ctx=*/NULL, /*options=*/NULL, /*num_options=*/0);
	if (rc < 0) {
		fprintf(stderr, "Error initializing libusb: %s\n", libusb_error_name(rc));
		exit(1);
	}

	devh = libusb_open_device_with_vid_pid(NULL, 0x16c0, 0x0763);
	if (!devh) {
		fprintf(stderr, "Error finding USB device\n");
		goto out;
	}

	rc = libusb_claim_interface(devh, 2);
	if (rc < 0) {
		fprintf(stderr, "Error claiming interface: %s\n", libusb_error_name(rc));
		goto out;
	}

	benchmark_in(EP_ISO_IN);

	while (!do_exit) {
		rc = libusb_handle_events(NULL);
		if (rc != LIBUSB_SUCCESS)
			break;
	}

	/* Measurement has already been done by the signal handler. */

	libusb_release_interface(devh, 2);
out:
	if (devh)
		libusb_close(devh);
	libusb_exit(NULL);
	return rc;
}
