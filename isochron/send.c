// SPDX-License-Identifier: GPL-2.0
/* Copyright 2019 NXP Semiconductors */
/* This file contains code snippets from:
 * - The Linux kernel
 * - The linuxptp project
 * Initial prototype based on:
 * https://gist.github.com/austinmarton/1922600
 * https://sourceforge.net/p/linuxptp/mailman/message/31998404/
 */
#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/ether.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
/* For va_start and va_end */
#include <stdarg.h>
#include "common.h"

#define BUF_SIZ		1522
#define LOGBUF_SIZ	(10 * 1024 * 1024) /* 10 MiB */

struct app_private {
	struct sockaddr *sockaddr;
	char *sendbuf;
	int data_fd;
	int tx_len;
};

struct prog_data {
	__u8 dest_mac[ETH_ALEN];
	__u8 src_mac[ETH_ALEN];
	char if_name[IFNAMSIZ];
	char sendbuf[BUF_SIZ];
	struct sockaddr_ll socket_address;
	long timestamped;
	long iterations;
	clockid_t clkid;
	__s64 advance_time;
	__s64 shift_time;
	__s64 cycle_time;
	__s64 base_time;
	long priority;
	int log_buf_len;
	char *log_buf;
	long tx_len;
	int data_fd;
	long vid;
	bool do_ts;
	struct app_private priv;
};

static int rtprintf(struct prog_data *prog, char *fmt, ...)
{
	char *buf = prog->log_buf + prog->log_buf_len + 1;
	va_list args;
	int rc;

	va_start(args, fmt);

	rc = vsnprintf(buf, LOGBUF_SIZ - prog->log_buf_len, fmt, args);
	prog->log_buf_len += (rc + 1);

	va_end(args);

	return rc;
}

static void rtflush(struct prog_data *prog)
{
	int rc, i = 0;

	while (i < prog->log_buf_len && prog->log_buf[i]) {
		rc = printf("%s", prog->log_buf + i);
		i += (rc + 1);
	}
}

static void process_txtstamp(struct prog_data *prog, const char *buf,
			     struct timestamp *tstamp)
{
	char scheduled_buf[TIMESPEC_BUFSIZ];
	char hwts_buf[TIMESPEC_BUFSIZ];
	char swts_buf[TIMESPEC_BUFSIZ];
	struct app_header *app_hdr;
	__s64 hwts, swts;

	app_hdr = (struct app_header *)(buf + sizeof(struct vlan_ethhdr));

	hwts = timespec_to_ns(&tstamp->hw);
	swts = timespec_to_ns(&tstamp->sw);

	ns_sprintf(scheduled_buf, __be64_to_cpu(app_hdr->tx_time));
	ns_sprintf(hwts_buf, hwts);
	ns_sprintf(swts_buf, swts);

	rtprintf(prog, "[%s] seqid %d txtstamp %s swts %s\n",
		 scheduled_buf, ntohs(app_hdr->seqid), hwts_buf, swts_buf);
	prog->timestamped++;
}

static void print_no_tstamp(struct prog_data *prog, const char *buf)
{
	char scheduled_buf[TIMESPEC_BUFSIZ];
	struct app_header *app_hdr;

	app_hdr = (struct app_header *)(buf + sizeof(struct vlan_ethhdr));

	ns_sprintf(scheduled_buf, __be64_to_cpu(app_hdr->tx_time));

	rtprintf(prog, "[%s] seqid %d\n", scheduled_buf, ntohs(app_hdr->seqid));
}

static int do_work(struct prog_data *prog, int iteration, __s64 scheduled,
		   clockid_t clkid)
{
	struct app_private *priv = &prog->priv;
	unsigned char err_pkt[BUF_SIZ];
	struct timestamp tstamp = {0};
	struct app_header *app_hdr;
	struct timespec now_ts;
	int rc;

	clock_gettime(clkid, &now_ts);
	app_hdr = (struct app_header *)(priv->sendbuf +
					sizeof(struct vlan_ethhdr));
	app_hdr->tx_time = __cpu_to_be64(scheduled);
	app_hdr->seqid = htons(iteration);

	/* Send packet */
	rc = sendto(priv->data_fd, priv->sendbuf, priv->tx_len, 0,
		    priv->sockaddr, sizeof(struct sockaddr_ll));
	if (rc < 0) {
		perror("send\n");
		return rc;
	}
	if (prog->do_ts) {
		rc = sk_receive(prog->data_fd, err_pkt, BUF_SIZ, &tstamp,
				MSG_ERRQUEUE, 0);
		if (rc == -EAGAIN)
			return 0;
		if (rc < 0)
			return rc;

		/* If a timestamp becomes available, process it now
		 * (don't wait for later)
		 */
		process_txtstamp(prog, err_pkt, &tstamp);
	} else {
		print_no_tstamp(prog, priv->sendbuf);
	}

	return 0;
}

static int wait_for_txtimestamps(struct prog_data *prog)
{
	unsigned char err_pkt[BUF_SIZ];
	struct timestamp tstamp;
	int rc;

	if (!prog->do_ts)
		return 0;

	while (prog->timestamped < prog->iterations) {
		rc = sk_receive(prog->data_fd, err_pkt, BUF_SIZ, &tstamp,
				MSG_ERRQUEUE, TXTSTAMP_TIMEOUT_MS);
		if (rc < 0) {
			fprintf(stderr,
				"Timed out waiting for TX timestamp: %d (%s)\n",
				rc, strerror(-rc));
			fprintf(stderr, "%ld timestamps unacknowledged\n",
				prog->iterations - prog->timestamped);
			return rc;
		}

		/* If a timestamp becomes available, process it now
		 * (don't wait for later)
		 */
		process_txtstamp(prog, err_pkt, &tstamp);
	}

	return 0;
}

static int run_nanosleep(struct prog_data *prog)
{
	char cycle_time_buf[TIMESPEC_BUFSIZ];
	char base_time_buf[TIMESPEC_BUFSIZ];
	__s64 wakeup = prog->base_time;
	__s64 scheduled;
	int rc;
	long i;

	ns_sprintf(base_time_buf, prog->base_time);
	ns_sprintf(cycle_time_buf, prog->cycle_time);
	fprintf(stderr, "%10s: %s\n", "Base time", base_time_buf);
	fprintf(stderr, "%10s: %s\n", "Cycle time", cycle_time_buf);

	/* Play nice with awk's array indexing */
	for (i = 1; i <= prog->iterations; i++) {
		struct timespec wakeup_ts = ns_to_timespec(wakeup);

		rc = clock_nanosleep(prog->clkid, TIMER_ABSTIME,
				     &wakeup_ts, NULL);
		switch (rc) {
		case 0:
			scheduled = wakeup + prog->advance_time;

			rc = do_work(prog, i, scheduled, prog->clkid);
			if (rc < 0)
				break;

			wakeup += prog->cycle_time;
			break;
		case EINTR:
			continue;
		default:
			fprintf(stderr, "clock_nanosleep returned %d: %s\n",
				rc, strerror(rc));
			break;
		}
	}

	return wait_for_txtimestamps(prog);
}

static void app_init(void *data)
{
	int i = sizeof(struct vlan_ethhdr);
	struct app_private *priv = data;

	/* Packet data */
	while (i < priv->tx_len) {
		priv->sendbuf[i++] = 0xde;
		priv->sendbuf[i++] = 0xad;
		priv->sendbuf[i++] = 0xbe;
		priv->sendbuf[i++] = 0xef;
	}
}

/* Calculate the first base_time in the future that satisfies this
 * relationship:
 *
 * future_base_time = base_time + N x cycle_time >= now, or
 *
 *      now - base_time
 * N >= ---------------
 *         cycle_time
 */
static __s64 future_base_time(__s64 base_time, __s64 cycle_time, __s64 now)
{
	__s64 n;

	if (base_time >= now)
		return base_time;

	n = (now - base_time) / cycle_time;

	return base_time + (n + 1) * cycle_time;
}

static int prog_init(struct prog_data *prog)
{
	char now_buf[TIMESPEC_BUFSIZ];
	struct vlan_ethhdr *hdr;
	struct timespec now_ts;
	struct ifreq if_idx;
	struct ifreq if_mac;
	__s64 now;
	int rc;

	prog->clkid = CLOCK_REALTIME;
	/* Convert negative logic from cmdline to positive */
	prog->do_ts = !prog->do_ts;

	/* Open RAW socket to send on */
	prog->data_fd = socket(AF_PACKET, SOCK_RAW, IPPROTO_RAW);
	if (prog->data_fd < 0) {
		perror("socket");
		return -EINVAL;
	}

	rc = setsockopt(prog->data_fd, SOL_SOCKET, SO_PRIORITY, &prog->priority,
			sizeof(int));
	if (rc < 0) {
		perror("setsockopt");
		close(prog->data_fd);
		return rc;
	}

	/* Get the index of the interface to send on */
	memset(&if_idx, 0, sizeof(struct ifreq));
	strncpy(if_idx.ifr_name, prog->if_name, IFNAMSIZ - 1);
	if (ioctl(prog->data_fd, SIOCGIFINDEX, &if_idx) < 0) {
		perror("SIOCGIFINDEX");
		close(prog->data_fd);
		return rc;
	}

	/* Get the MAC address of the interface to send on */
	memset(&if_mac, 0, sizeof(struct ifreq));
	strncpy(if_mac.ifr_name, prog->if_name, IFNAMSIZ - 1);
	if (ioctl(prog->data_fd, SIOCGIFHWADDR, &if_mac) < 0) {
		perror("SIOCGIFHWADDR");
		close(prog->data_fd);
		return rc;
	}

	if (!ether_addr_to_u64(prog->src_mac))
		memcpy(prog->src_mac, &if_mac.ifr_hwaddr.sa_data, ETH_ALEN);

	/* Construct the Ethernet header */
	memset(prog->sendbuf, 0, BUF_SIZ);
	/* Ethernet header */
	hdr = (struct vlan_ethhdr *)prog->sendbuf;
	memcpy(hdr->h_source, prog->src_mac, ETH_ALEN);
	memcpy(hdr->h_dest, prog->dest_mac, ETH_ALEN);
	hdr->h_vlan_proto = htons(ETH_P_8021Q);
	/* Ethertype field */
	hdr->h_vlan_encapsulated_proto = htons(ETH_P_TSN);
	hdr->h_vlan_TCI = htons((prog->priority << VLAN_PRIO_SHIFT) |
				(prog->vid & VLAN_VID_MASK));

	/* Index of the network device */
	prog->socket_address.sll_ifindex = if_idx.ifr_ifindex;
	/* Address length*/
	prog->socket_address.sll_halen = ETH_ALEN;
	/* Destination MAC */
	memcpy(prog->socket_address.sll_addr, prog->dest_mac, ETH_ALEN);

	rc = clock_gettime(prog->clkid, &now_ts);
	if (rc < 0) {
		perror("clock_gettime");
		close(prog->data_fd);
		return rc;
	}

	now = timespec_to_ns(&now_ts);
	prog->base_time += prog->shift_time;
	prog->base_time -= prog->advance_time;

	/* Make sure we get enough sleep at the beginning */
	now += NSEC_PER_SEC;

	if (prog->base_time < now) {
		char base_time_buf[TIMESPEC_BUFSIZ];

		ns_sprintf(base_time_buf, prog->base_time);
		fprintf(stderr,
			"Base time %s is in the past, winding it into the future\n",
			base_time_buf);

		prog->base_time = future_base_time(prog->base_time,
						   prog->cycle_time,
						   now);
	}

	ns_sprintf(now_buf, now);
	fprintf(stderr, "%10s: %s\n", "Now", now_buf);

	prog->log_buf = calloc(sizeof(char), LOGBUF_SIZ);
	if (!prog->log_buf)
		return -ENOMEM;
	prog->log_buf_len = -1;

	/* Prevent the process's virtual memory from being swapped out, by
	 * locking all current and future pages
	 */
	rc = mlockall(MCL_CURRENT | MCL_FUTURE);
	if (rc < 0) {
		fprintf(stderr, "mlockall returned %d: %s\n",
			errno, strerror(errno));
		return rc;
	}

	if (prog->do_ts)
		return sk_timestamping_init(prog->data_fd, prog->if_name, 1);

	return 0;
}

static int prog_teardown(struct prog_data *prog)
{
	rtflush(prog);
	free(prog->log_buf);

	return 0;
}

static int prog_parse_args(int argc, char **argv, struct prog_data *prog)
{
	struct prog_arg args[] = {
		{
			.short_opt = "-i",
			.long_opt = "--interface",
			.type = PROG_ARG_STRING,
			.string = {
				.buf = prog->if_name,
				.size = IFNAMSIZ - 1,
			},
		}, {
			.short_opt = "-d",
			.long_opt = "--dmac",
			.type = PROG_ARG_MAC_ADDR,
			.mac = {
				.buf = prog->dest_mac,
			},
		}, {
			.short_opt = "-A",
			.long_opt = "--smac",
			.type = PROG_ARG_MAC_ADDR,
			.mac = {
				.buf = prog->src_mac,
			},
			.optional = true,
		}, {
			.short_opt = "-p",
			.long_opt = "--priority",
			.type = PROG_ARG_LONG,
			.long_ptr = {
				.ptr = &prog->priority,
			},
		}, {
			.short_opt = "-b",
			.long_opt = "--base-time",
			.type = PROG_ARG_TIME,
			.time = {
				.clkid = CLOCK_REALTIME,
				.ns = &prog->base_time,
			},
		}, {
			.short_opt = "-a",
			.long_opt = "--advance-time",
			.type = PROG_ARG_TIME,
			.time = {
				.clkid = CLOCK_REALTIME,
				.ns = &prog->advance_time,
			},
			.optional = true,
		}, {
			.short_opt = "-S",
			.long_opt = "--shift-time",
			.type = PROG_ARG_TIME,
			.time = {
				.clkid = CLOCK_REALTIME,
				.ns = &prog->shift_time,
			},
			.optional = true,
		}, {
			.short_opt = "-c",
			.long_opt = "--cycle-time",
			.type = PROG_ARG_TIME,
			.time = {
				.clkid = CLOCK_REALTIME,
				.ns = &prog->cycle_time,
			},
		}, {
			.short_opt = "-n",
			.long_opt = "--num-frames",
			.type = PROG_ARG_LONG,
			.long_ptr = {
				.ptr = &prog->iterations,
			},
		}, {
			.short_opt = "-s",
			.long_opt = "--frame-size",
			.type = PROG_ARG_LONG,
			.long_ptr = {
				.ptr = &prog->tx_len,
			},
		}, {
			.short_opt = "-T",
			.long_opt = "--no-ts",
			.type = PROG_ARG_BOOL,
			.boolean_ptr = {
			        .ptr = &prog->do_ts,
			},
			.optional = true,
		}, {
			.short_opt = "-v",
			.long_opt = "--vid",
			.type = PROG_ARG_LONG,
			.long_ptr = {
				.ptr = &prog->vid,
			},
			.optional = true,
		},
	};
	int rc;

	rc = prog_parse_np_args(argc, argv, args, ARRAY_SIZE(args));

	/* Non-positional arguments left unconsumed */
	if (rc < 0) {
		fprintf(stderr, "Parsing returned %d: %s\n",
			-rc, strerror(-rc));
		return rc;
	} else if (rc < argc) {
		fprintf(stderr, "%d unconsumed arguments. First: %s\n",
			argc - rc, argv[rc]);
		prog_usage("isochron-send", args, ARRAY_SIZE(args));
		return -1;
	}

	/* No point in leaving this one's default to zero, if we know that
	 * means it will always be late for its gate event.
	 */
	if (!prog->advance_time)
		prog->advance_time = prog->cycle_time;

	if (prog->advance_time > prog->cycle_time) {
		fprintf(stderr,
			"Advance time cannot be higher than cycle time\n");
		return -EINVAL;
	}
	if (prog->shift_time > prog->cycle_time) {
		fprintf(stderr,
			"Shift time cannot be higher than cycle time\n");
		return -EINVAL;
	}

	return 0;
}

int isochron_send_main(int argc, char *argv[])
{
	struct prog_data prog = {0};
	struct app_private *priv = &prog.priv;
	int rc_save, rc;

	rc = prog_parse_args(argc, argv, &prog);
	if (rc < 0)
		return rc;

	rc = prog_init(&prog);
	if (rc < 0)
		return rc;

	priv->sockaddr = (struct sockaddr *)&prog.socket_address;
	priv->sendbuf = prog.sendbuf;
	priv->data_fd = prog.data_fd;
	priv->tx_len = prog.tx_len;

	app_init(priv);

	rc_save = run_nanosleep(&prog);

	rc = prog_teardown(&prog);
	if (rc < 0)
		return rc;

	return rc_save;
}