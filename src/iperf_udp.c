/*
 * iperf, Copyright (c) 2014-2022, The Regents of the University of
 * California, through Lawrence Berkeley National Laboratory (subject
 * to receipt of any required approvals from the U.S. Dept. of
 * Energy).  All rights reserved.
 *
 * If you have questions about your rights to use or distribute this
 * software, please contact Berkeley Lab's Technology Transfer
 * Department at TTD@lbl.gov.
 *
 * NOTICE.  This software is owned by the U.S. Department of Energy.
 * As such, the U.S. Government has been granted for itself and others
 * acting on its behalf a paid-up, nonexclusive, irrevocable,
 * worldwide license in the Software to reproduce, prepare derivative
 * works, and perform publicly and display publicly.  Beginning five
 * (5) years after the date permission to assert copyright is obtained
 * from the U.S. Department of Energy, and subject to any subsequent
 * five (5) year renewals, the U.S. Government is granted for itself
 * and others acting on its behalf a paid-up, nonexclusive,
 * irrevocable, worldwide license in the Software to reproduce,
 * prepare derivative works, distribute copies to the public, perform
 * publicly and display publicly, and to permit others to do so.
 *
 * This code is distributed under a BSD style license, see the LICENSE
 * file for complete information.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <arpa/inet.h>
#ifndef __WIN32__
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/select.h>
#endif
#include <stdint.h>
#include <inttypes.h>
#include <sys/time.h>

#include "iperf.h"
#include "iperf_api.h"
#include "iperf_util.h"
#include "iperf_udp.h"
#include "timer.h"
#include "net.h"
#include "cjson.h"

/* iperf_udp_recv
 *
 * receives the data for UDP
 */
int
iperf_udp_recv(struct iperf_stream *sp)
{
    uint32_t  sec, usec;
    uint64_t  pcount;
    int       r;
    int       size = sp->settings->blksize;
    int       first_packet = 0;
    double    transit = 0, d = 0;
    struct iperf_time sent_time, arrival_time, temp_time;
    struct iperf_test *test = sp->test;	
    int sock_opt = 0;

#if defined(HAVE_MSG_TRUNC)
    // UDP recv() with MSG_TRUNC reads only the size bytes, but return the length of the full packet
    if (sp->test->settings->skip_rx_copy) {
        sock_opt = MSG_TRUNC;
        size = sizeof(sec) + sizeof(usec) + sizeof(pcount);
    }
#endif /* HAVE_MSG_TRUNC */

    r = Nrecv_no_select(sp->socket, sp->buffer, size, Pudp, sock_opt);

    /*
     * If we got an error in the read, or if we didn't read anything
     * because the underlying read(2) got a EAGAIN, then skip packet
     * processing.
     */
    if (r <= 0)
        return r;

    /* Only count bytes received while we're in the correct state. */
    if (test->state == TEST_RUNNING) {

	/*
	 * For jitter computation below, it's important to know if this
	 * packet is the first packet received.
	 */
	if (sp->result->bytes_received == 0) {
	    first_packet = 1;
	}

	sp->result->bytes_received += r;
	sp->result->bytes_received_this_interval += r;

	/* Dig the various counters out of the incoming UDP packet */
	if (test->udp_counters_64bit) {
	    memcpy(&sec, sp->buffer, sizeof(sec));
	    memcpy(&usec, sp->buffer+4, sizeof(usec));
	    memcpy(&pcount, sp->buffer+8, sizeof(pcount));
	    sec = ntohl(sec);
	    usec = ntohl(usec);
	    pcount = be64toh(pcount);
	    sent_time.secs = sec;
	    sent_time.usecs = usec;
	}
	else {
	    uint32_t pc;
	    memcpy(&sec, sp->buffer, sizeof(sec));
	    memcpy(&usec, sp->buffer+4, sizeof(usec));
	    memcpy(&pc, sp->buffer+8, sizeof(pc));
	    sec = ntohl(sec);
	    usec = ntohl(usec);
	    pcount = ntohl(pc);
	    sent_time.secs = sec;
	    sent_time.usecs = usec;
	}

	if (sp->test->debug > 1)
	    iperf_err(sp->test, "pcount %" PRIu64 " packet_count %" PRIu64 "\n", pcount, sp->packet_count);

	/*
	 * Try to handle out of order packets.  The way we do this
	 * uses a constant amount of storage but might not be
	 * correct in all cases.  In particular we seem to have the
	 * assumption that packets can't be duplicated in the network,
	 * because duplicate packets will possibly cause some problems here.
	 *
	 * First figure out if the sequence numbers are going forward.
	 * Note that pcount is the sequence number read from the packet,
	 * and sp->packet_count is the highest sequence number seen so
	 * far (so we're expecting to see the packet with sequence number
	 * sp->packet_count + 1 arrive next).
	 */
	if (pcount >= sp->packet_count + 1) {

	    /* Forward, but is there a gap in sequence numbers? */
	    if (pcount > sp->packet_count + 1) {
		/* There's a gap so count that as a loss. */
		sp->cnt_error += (pcount - 1) - sp->packet_count;
                if (test->debug_level >= DEBUG_LEVEL_INFO)
		    iperf_err(test, "LOST %" PRIu64 " PACKETS - received packet %" PRIu64 " but expected sequence %" PRIu64 " on stream %d\n", (pcount - sp->packet_count + 1), pcount, sp->packet_count + 1, sp->socket);
	    }
	    /* Update the highest sequence number seen so far. */
	    sp->packet_count = pcount;
	} else {

	    /*
	     * Sequence number went backward (or was stationary?!?).
	     * This counts as an out-of-order packet.
	     */
	    sp->outoforder_packets++;

	    /*
	     * If we have lost packets, then the fact that we are now
	     * seeing an out-of-order packet offsets a prior sequence
	     * number gap that was counted as a loss.  So we can take
	     * away a loss.
	     */
	    if (sp->cnt_error > 0)
		sp->cnt_error--;

	    /* Log the out-of-order packet */
	    if (test->debug_level >= DEBUG_LEVEL_INFO)
		iperf_err(test, "OUT OF ORDER - received packet %" PRIu64 " but expected sequence %" PRIu64 " on stream %d\n", pcount, sp->packet_count + 1, sp->socket);
	}

	/*
	 * jitter measurement
	 *
	 * This computation is based on RFC 1889 (specifically
	 * sections 6.3.1 and A.8).
	 *
	 * Note that synchronized clocks are not required since
	 * the source packet delta times are known.  Also this
	 * computation does not require knowing the round-trip
	 * time.
	 */
	iperf_time_now(&arrival_time);

	iperf_time_diff(&arrival_time, &sent_time, &temp_time);
	transit = iperf_time_in_secs(&temp_time);

	/* Hack to handle the first packet by initializing prev_transit. */
	if (first_packet)
	    sp->prev_transit = transit;

        if (sp->prev_transit) {
            d = transit - sp->prev_transit;
            if (d < 0)
                d = -d;
            sp->jitter += (d - sp->jitter) / 16.0;
        }
        else {
            sp->jitter = 0;
        }
        sp->prev_transit = transit;
    }
    else {
	if (test->debug_level >= DEBUG_LEVEL_INFO)
	    iperf_err(test, "Late receive, state = %d\n", test->state);
    }

    return r;
}


/* iperf_udp_send
 *
 * sends the data for UDP
 */
int
iperf_udp_send(struct iperf_stream *sp)
{
    int r;
    int       size = sp->settings->blksize;
    struct iperf_time before;

    iperf_time_now(&before);

    ++sp->packet_count;

    if (sp->test->udp_counters_64bit) {

	uint32_t  sec, usec;
	uint64_t  pcount;

	sec = htonl(before.secs);
	usec = htonl(before.usecs);
	pcount = htobe64(sp->packet_count);

	memcpy(sp->buffer, &sec, sizeof(sec));
	memcpy(sp->buffer+4, &usec, sizeof(usec));
	memcpy(sp->buffer+8, &pcount, sizeof(pcount));

    }
    else {

	uint32_t  sec, usec, pcount;

	sec = htonl(before.secs);
	usec = htonl(before.usecs);
	pcount = htonl(sp->packet_count);

	memcpy(sp->buffer, &sec, sizeof(sec));
	memcpy(sp->buffer+4, &usec, sizeof(usec));
	memcpy(sp->buffer+8, &pcount, sizeof(pcount));

    }

    r = Nwrite(sp->socket, sp->buffer, size, Pudp, sp->test);

    if (r <= 0) {
        --sp->packet_count;     /* Don't count messages that no data was sent from them.
                                 * Allows "resending" a massage with the same numbering */
        if (r < 0) {
            if (r == NET_SOFTERROR && sp->test->debug_level >= DEBUG_LEVEL_INFO)
                iperf_err(sp->test, "UDP send failed on NET_SOFTERROR. errno=%s\n", strerror(errno));
            return r;
        }
    }

    sp->result->bytes_sent += r;
    sp->result->bytes_sent_this_interval += r;

    if (sp->test->debug_level > 1)
	    iperf_err(sp->test, "sent %d bytes of %d, total %" PRIu64 "\n", r, sp->settings->blksize, sp->result->bytes_sent);

    return r;
}


/**************************************************************************/

/*
 * The following functions all have to do with managing UDP data sockets.
 * UDP of course is connectionless, so there isn't really a concept of
 * setting up a connection, although connect(2) can (and is) used to
 * bind the remote end of sockets.  We need to simulate some of the
 * connection management that is built-in to TCP so that each side of the
 * connection knows about each other before the real data transfers begin.
 */

/*
 * Set and verify socket buffer sizes.
 * Return 0 if no error, -1 if an error, +1 if socket buffers are
 * potentially too small to hold a message.
 */
int
iperf_udp_buffercheck(struct iperf_test *test, int s)
{
    int rc = 0;
    int sndbuf_actual, rcvbuf_actual;

    /*
     * Set socket buffer size if requested.  Do this for both sending and
     * receiving so that we can cover both normal and --reverse operation.
     */
    int opt;
    socklen_t optlen;

    if ((opt = test->settings->socket_bufsize)) {
        if (setsockopt(s, SOL_SOCKET, SO_RCVBUF, (char*)&opt, sizeof(opt)) < 0) {
            i_errno = IESETBUF;
            return -1;
        }
        if (setsockopt(s, SOL_SOCKET, SO_SNDBUF, (char*)&opt, sizeof(opt)) < 0) {
            i_errno = IESETBUF;
            return -1;
        }
    }

    /* Read back and verify the sender socket buffer size */
    optlen = sizeof(sndbuf_actual);
    if (getsockopt(s, SOL_SOCKET, SO_SNDBUF, (char*)&sndbuf_actual, &optlen) < 0) {
        i_errno = IESETBUF;
        return -1;
    }
    if (test->debug) {
	    printf("SNDBUF is %u, expecting %u, fd: %d\n", sndbuf_actual, test->settings->socket_bufsize, s);
    }
    if (test->settings->socket_bufsize && test->settings->socket_bufsize > sndbuf_actual) {
        i_errno = IESETBUF2;
        return -1;
    }
    if (test->settings->blksize > sndbuf_actual) {
	    iperf_err(test, "Block size %d > sending socket buffer size %d", test->settings->blksize, sndbuf_actual);
	    rc = 1;
    }

    /* Read back and verify the receiver socket buffer size */
    optlen = sizeof(rcvbuf_actual);
    if (getsockopt(s, SOL_SOCKET, SO_RCVBUF, (char*)&rcvbuf_actual, &optlen) < 0) {
        i_errno = IESETBUF;
        return -1;
    }
    if (test->debug) {
	    iperf_err(test, "RCVBUF is %u, expecting %u fd: %d\n", rcvbuf_actual, test->settings->socket_bufsize, s);
    }
    if (test->settings->socket_bufsize && test->settings->socket_bufsize > rcvbuf_actual) {
        i_errno = IESETBUF2;
        return -1;
    }
    if (test->settings->blksize > rcvbuf_actual) {
	    iperf_err(test, "Block size %d > receiving socket buffer size %d",test->settings->blksize, rcvbuf_actual);
	    rc = 1;
    }

    if (test->json_output) {
        cJSON *sock_bufsize_item = cJSON_GetObjectItem(test->json_start, "sock_bufsize");
    if (sock_bufsize_item == NULL) {
        cJSON_AddNumberToObject(test->json_start, "sock_bufsize", test->settings->socket_bufsize);
    }

    cJSON *sndbuf_actual_item = cJSON_GetObjectItem(test->json_start, "sndbuf_actual");
    if (sndbuf_actual_item == NULL) {
	    cJSON_AddNumberToObject(test->json_start, "sndbuf_actual", sndbuf_actual);
    }
        
    cJSON *rcvbuf_actual_item = cJSON_GetObjectItem(test->json_start, "rcvbuf_actual");
    if (rcvbuf_actual_item == NULL) {
	    cJSON_AddNumberToObject(test->json_start, "rcvbuf_actual", rcvbuf_actual);
    }
    }

    return rc;
}

/*
 * iperf_udp_accept
 *
 * Accepts a new UDP "connection"
 */
int
iperf_udp_accept(struct iperf_test *test)
{
    struct sockaddr_storage sa_peer;
    unsigned int buf;
    socklen_t len;
    int       sz, s;
    int	      rc;
    int i;

    /*
     * Get the current outstanding socket.  This socket will be used to handle
     * data transfers and a new "listening" socket will be created.
     */
    s = test->prot_listener;

    for (i = 0; i<30; i++) {
        fd_set read_fds;

        /* Don't block forever if the peer messed up or cannot otherwise send frame
         * this direction.
         */
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        FD_ZERO(&read_fds);
        FD_SET(s, &read_fds);

        int select_ret = select(test->prot_listener + 1, &read_fds, NULL, NULL, &tv);
        if (select_ret == 1) {
            /*
             * Grab the UDP packet sent by the client.  From that we can extract the
             * client's address, and then use that information to bind the remote side
             * of the socket to the client.
             */
            len = sizeof(sa_peer);
            if ((sz = recvfrom(test->prot_listener, (char*)&buf, sizeof(buf), 0, (struct sockaddr *) &sa_peer, &len)) < 0) {
                i_errno = IESTREAMACCEPT;
                return -1;
            }
            goto got_response;
        }
        else {
            if (test->debug) {
                iperf_err(test, "Did not receive response, try %d / 30, in udp-accept.", i);
            }
        }
    }

    /* If here, we did not get a response in time. */
    iperf_err(test, "Did not receive frame within 30 seconds in udp-accept.");
    i_errno = IESTREAMACCEPT;
    return -1;

got_response:
    if (connect(s, (struct sockaddr *) &sa_peer, len) < 0) {
        i_errno = IESTREAMACCEPT;
        return -1;
    }

    /* Check and set socket buffer sizes */
    rc = iperf_udp_buffercheck(test, s);
    if (rc < 0)
	/* error */
	return rc;
    /*
     * If the socket buffer was too small, but it was the default
     * size, then try explicitly setting it to something larger.
     */
    if (rc > 0) {
	if (test->settings->socket_bufsize == 0) {
	    int bufsize = test->settings->blksize + UDP_BUFFER_EXTRA;
	    iperf_err(test, "Increasing socket buffer size to %d", bufsize);
	    test->settings->socket_bufsize = bufsize;
	    rc = iperf_udp_buffercheck(test, s);
	    if (rc < 0)
		    return rc;
	}
    }

#if defined(HAVE_SO_MAX_PACING_RATE)
    /* If socket pacing is specified, try it. */
    if (test->settings->fqrate) {
	/* Convert bits per second to bytes per second */
	uint64_t fqrate = test->settings->fqrate / 8;
	if (fqrate > 0) {
	    if (test->debug) {
		    printf("Setting fair-queue socket pacing to %"PRIu64"\n", fqrate);
	    }
	    if (setsockopt(s, SOL_SOCKET, SO_MAX_PACING_RATE, &fqrate, sizeof(fqrate)) < 0) {
		    iperf_err(test, "Unable to set socket pacing");
	    }
	}
    }
#endif /* HAVE_SO_MAX_PACING_RATE */
    {
	unsigned int rate = test->settings->rate / 8;
	if (rate > 0) {
	    if (test->debug) {
		printf("Setting application pacing to %u\n", rate);
	    }
	}
    }

    /*
     * Create a new "listening" socket to replace the one we were using before.
     */
    FD_CLR(test->prot_listener, &test->read_set); // No control messages from old listener
    test->prot_listener = netannounce(test->settings->domain, Pudp, test->bind_address, test->bind_dev, test->server_port, test);
    if (test->debug) {
        fprintf(stderr, "udp-accept, new prot-listener socket: %d\n", test->prot_listener);
    }
    if (test->prot_listener < 0) {
        i_errno = IESTREAMLISTEN;
        iclosesocket(s, test);
        return -1;
    }

    IFD_SET(test->prot_listener, &test->read_set, test);

    /* Let the client know we're ready "accept" another UDP "stream" */
    buf = UDP_CONNECT_REPLY;
#ifndef __WIN32__
    if (send(s, (const char*)&buf, sizeof(buf), 0) < 0)
#else
    if (write(s, &buf, sizeof(buf)) < 0)
#endif
        {
        i_errno = IESTREAMWRITE;
        iclosesocket(s, test);
        return -1;
    }

    return s;
}


/*
 * iperf_udp_listen
 *
 * Start up a listener for UDP stream connections.  Unlike for TCP,
 * there is no listen(2) for UDP.  This socket will however accept
 * a UDP datagram from a client (indicating the client's presence).
 */
int
iperf_udp_listen(struct iperf_test *test)
{
    int s;

    if ((s = netannounce(test->settings->domain, Pudp, test->bind_address, test->bind_dev, test->server_port, test)) < 0) {
        i_errno = IESTREAMLISTEN;
        return -1;
    }

    if (test->debug) {
        fprintf(stderr, "iperf-udp-listen, fd: %d\n", s);
    }

    /*
     * The caller will put this value into test->prot_listener.
     */
    return s;
}


/*
 * iperf_udp_connect
 *
 * "Connect" to a UDP stream listener.
 */
int
iperf_udp_connect(struct iperf_test *test)
{
    int s, sz;
    unsigned int buf;
#ifdef SO_RCVTIMEO
    struct timeval tv;
#endif
    int rc;
    int i, max_len_wait_for_reply;

    if (test->debug) {
        fprintf(stderr, "udp-connect called\n");
    }

    /* Create and bind our local socket. */
    if ((s = netdial(test->settings->domain, Pudp, test->bind_address, test->bind_dev, 
                     test->bind_port, test->server_hostname, test->server_port, -1, test)) < 0) {
        fprintf(stderr, "udp-connect, netdial() failed: %s\n", STRERROR);
        i_errno = IESTREAMCONNECT;
        return -1;
    }

    /* Check and set socket buffer sizes */
    rc = iperf_udp_buffercheck(test, s);
    if (rc < 0) {
        /* error */
        iclosesocket(s, test);
        return rc;
    }
    /*
     * If the socket buffer was too small, but it was the default
     * size, then try explicitly setting it to something larger.
     */
    if (rc > 0) {
        if (test->settings->socket_bufsize == 0) {
                char str[WARN_STR_LEN];
            int bufsize = test->settings->blksize + UDP_BUFFER_EXTRA;
            snprintf(str, sizeof(str), "Increasing socket buffer size to %d",
                    bufsize);
            warning(str);
            test->settings->socket_bufsize = bufsize;
            rc = iperf_udp_buffercheck(test, s);
            if (rc < 0) {
                iclosesocket(s, test);
                return rc;
            }
        }
    }

#if defined(HAVE_SO_MAX_PACING_RATE)
    /* If socket pacing is available and not disabled, try it. */
    if (test->settings->fqrate) {
	/* Convert bits per second to bytes per second */
	uint64_t fqrate = test->settings->fqrate / 8;
	if (fqrate > 0) {
	    if (test->debug) {
		printf("Setting fair-queue socket pacing to %"PRIu64"\n", fqrate);
	    }
	    if (setsockopt(s, SOL_SOCKET, SO_MAX_PACING_RATE, &fqrate, sizeof(fqrate)) < 0) {
		warning("Unable to set socket pacing");
	    }
	}
    }
#endif /* HAVE_SO_MAX_PACING_RATE */
    {
	unsigned int rate = test->settings->rate / 8;
	if (rate > 0) {
	    if (test->debug) {
		    printf("Setting application pacing to %u\n", rate);
	    }
	}
    }

    /* Set common socket options */
    iperf_common_sockopts(test, s);

#ifdef SO_RCVTIMEO
    /* 30 sec timeout for a case when there is a network problem. */
    tv.tv_sec = 30;
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(struct timeval));
#endif

    /*
     * Write a datagram to the UDP stream to let the server know we're here.
     * The server learns our address by obtaining its peer's address.
     */
    buf = UDP_CONNECT_MSG;
    if (test->debug) {
        iperf_err(test, "sending '0x39383736' to peer to let them know we are here: %s",
                hexdump((const unsigned char*)(&buf), sizeof(buf), 1, 1));
    }
    for (i = 0; i<30; i++) {
        fd_set read_fds;

        /* UDP messages can be lost and will not automatically be retransmitted.
         * so we will retry and use select to not block forever.
         */
#ifdef __WIN32__
        if (send(s, (const char*)&buf, sizeof(buf), 0) < 0)
#else
        if (write(s, &buf, sizeof(buf)) < 0)
#endif        
        {
            // XXX: Should this be changed to IESTREAMCONNECT? 
            i_errno = IESTREAMWRITE;
            iclosesocket(s, test);
            return -1;
        }

        if (test->debug) {
            iperf_err(test, "waiting to receive response from server");
        }

        /*
         * Wait until the server replies back to us.
         */
        //We are going to use select to wait for the socket to connect
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        FD_ZERO(&read_fds);
        FD_SET(s, &read_fds);

        int select_ret = select(s + 1, &read_fds, NULL, NULL, &tv);
        if (select_ret == 1) {
            if ((sz = recv(s, (char*)&buf, sizeof(buf), 0)) < 0) {
                iperf_err(test, "Failed recv: %s  socket: %d", STRERROR, s);
                iclosesocket(s, test);
                i_errno = IESTREAMREAD;
                return -1;
            }

            if (test->debug) {
                iperf_err(test, "Received response from server: %s", hexdump((const unsigned char*)(&buf), sizeof(buf), 1, 1));
            }
            return s;
        }
        else {
            if (test->debug) {
                iperf_err(test, "No response from server, will retry: %d / 30", i);
            }
        }
    }

    /* if here, we could not get a response in time. */
    iperf_err(test, "Did not receive UDP connect response in time.");
    iclosesocket(s, test);
    i_errno = IESTREAMREAD;
    return -1;
}


/* iperf_udp_init
 *
 * initializer for UDP streams in TEST_START
 */
int
iperf_udp_init(struct iperf_test *test)
{
    return 0;
}
