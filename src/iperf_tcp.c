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
#include <arpa/inet.h>
#ifndef __WIN32__
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/select.h>
#endif
#include <sys/time.h>
#include <limits.h>

#include "iperf.h"
#include "iperf_api.h"
#include "iperf_tcp.h"
#include "iperf_util.h"
#include "net.h"
#include "cjson.h"

#if defined(HAVE_FLOWLABEL)
#include "flowlabel.h"
#endif /* HAVE_FLOWLABEL */

/* iperf_tcp_recv
 *
 * receives the data for TCP
 */
int
iperf_tcp_recv(struct iperf_stream *sp)
{
    int r;
    int sock_opt;

#if defined(HAVE_MSG_TRUNC)
    sock_opt = sp->test->settings->skip_rx_copy ? MSG_TRUNC : 0;
#else
    sock_opt = 0;
#endif /* HAVE_MSG_TRUNC */

    r = Nrecv_no_select(sp->socket, sp->buffer, sp->settings->blksize, Ptcp, sock_opt);


    if (r < 0) {
        if (sp->test->debug) // normal to hit this in tcp client receive mode.
            iperf_err(sp->test, "tcp-recv, failed (%s), socket: %d", STRERROR, sp->socket);
        return r;
    }

    /* Only count bytes received while we're in the correct state. */
    if (sp->test->state == TEST_RUNNING) {
	      sp->result->bytes_received += r;
	      sp->result->bytes_received_this_interval += r;
    }
    else {
	      if (sp->test->debug)
	          printf("Late receive, state = %d-%s\n", sp->test->state, state_to_text(sp->test->state));
    }

    return r;
}


/* iperf_tcp_send
 *
 * sends the data for TCP
 */
int
iperf_tcp_send(struct iperf_stream *sp)
{
    int r;
    int wsize = sp->settings->blksize;

    if (!sp->pending_size)
	      sp->pending_size = sp->settings->blksize;

    if (sp->test->zerocopy)
	      r = Nsendfile(sp->buffer_fd, sp->socket, sp->buffer, wsize);
    else {
#ifdef __WIN32__
        if (sp->settings->mss) {
            // Windows cannot do mss, send smaller block-sizes instead.
            if (sp->settings->mss < sp->settings->blksize)
                wsize = sp->settings->mss;
        }
#endif
        r = Nwrite(sp->socket, sp->buffer, sp->pending_size, Ptcp, sp->test);
    }
    if (r < 0)
        return r;

    sp->pending_size -= r;
    sp->result->bytes_sent += r;
    sp->result->bytes_sent_this_interval += r;

    if (sp->test->debug > 1)
	      printf("tcp: sent %d bytes of %d, total %llu\n", r, wsize, (long long unsigned)sp->result->bytes_sent);

    return r;
}


/* iperf_tcp_accept
 *
 * accept a new TCP stream connection
 */
int
iperf_tcp_accept(struct iperf_test * test)
{
    int     s;
    signed char rbuf = ACCESS_DENIED;
    char    cookie[COOKIE_SIZE] = {0};
    socklen_t len;
    struct sockaddr_storage addr;

    len = sizeof(addr);

    /* Wait a bit until the peer attempts a connection */
    waitSocketReadable(test->listener, ctrl_wait_ms);


    if ((s = accept(test->listener, (struct sockaddr *) &addr, &len)) < 0) {
        iperf_err(test, "tcp-accept, accept failed: %s\n", STRERROR);
        i_errno = IESTREAMCONNECT;
        return -1;
    }
#if defined(HAVE_SO_MAX_PACING_RATE)
    /* If fq socket pacing is specified, enable it. */

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

    if (waitRead(s, cookie, COOKIE_SIZE, Ptcp, test, ctrl_wait_ms) != COOKIE_SIZE) {
        i_errno = IERECVCOOKIE;
        iclosesocket(s, test);
        return -1;
    }

    if (strncmp(test->cookie, cookie, COOKIE_SIZE) != 0) {
        if (waitWrite(s, (char*) &rbuf, sizeof(rbuf), Ptcp, test, ctrl_wait_ms) != sizeof(rbuf)) {
            iperf_err(test, "failed to send access denied from busy server to new connecting client, errno = %d\n", errno);
            i_errno = IESENDMESSAGE;
        }
        iclosesocket(s, test);
        return -1;
    }

    return s;
}


/* iperf_tcp_listen
 *
 * start up a listener for TCP stream connections
 * Returns non-blocking socket.
 */
int
iperf_tcp_listen(struct iperf_test *test)
{
    int s, opt;
    socklen_t optlen;
    int saved_errno;
    int rcvbuf_actual, sndbuf_actual;

    s = test->listener;

    /*
     * If certain parameters are specified (such as socket buffer
     * size), then throw away the listening socket (the one for which
     * we just accepted the control connection) and recreate it with
     * those parameters.  That way, when new data connections are
     * set, they'll have all the correct parameters in place.
     *
     * It's not clear whether this is a requirement or a convenience.
     */
    if (test->no_delay || test->mptcp || test->settings->mss || test->settings->socket_bufsize) {
        struct addrinfo hints, *res;
        char portstr[6];
        int proto = 0;

        iclosesocket(s, test);

        snprintf(portstr, 6, "%d", test->server_port);
        memset(&hints, 0, sizeof(hints));

	/*
	 * If binding to the wildcard address with no explicit address
	 * family specified, then force us to get an AF_INET6 socket.
	 * More details in the comments in netanounce().
	 */
	if (test->settings->domain == AF_UNSPEC && !test->bind_address) {
	    hints.ai_family = AF_INET6;
	}
	else {
	    hints.ai_family = test->settings->domain;
	}
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;
        if ((gerror = getaddrinfo(test->bind_address, portstr, &hints, &res)) != 0) {
            iperf_err(test, "tcp-listen, getaddrinfo failed: %s\n", STRERROR);
            i_errno = IESTREAMLISTEN;
            return -1;
        }

        setnonblocking(s, 1);

#if defined(HAVE_IPPROTO_MPTCP)
        if (test->mptcp)
	    proto = IPPROTO_MPTCP;
#endif

        if ((s = socket(res->ai_family, SOCK_STREAM, proto)) < 0) {
            iperf_err(test, "tcp-listen, socket() failed: %s\n", STRERROR);
	        freeaddrinfo(res);
            i_errno = IESTREAMLISTEN;
            return -1;
        }

        if (test->no_delay) {
            opt = 1;
            if (setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&opt, sizeof(opt)) < 0) {
		saved_errno = errno;
		iclosesocket(s, test);
		freeaddrinfo(res);
		errno = saved_errno;
                i_errno = IESETNODELAY;
                return -1;
            }
        }
        // XXX: Setting MSS is very buggy!
#ifndef __WIN32__
        if ((opt = test->settings->mss)) {
            if (setsockopt(s, IPPROTO_TCP, TCP_MAXSEG, &opt, sizeof(opt)) < 0) {
                saved_errno = errno;
                iclosesocket(s, test);
                freeaddrinfo(res);
                errno = saved_errno;
                i_errno = IESETMSS;
                return -1;
            }
        }
#endif
        if ((opt = test->settings->socket_bufsize)) {
            if (setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&opt, sizeof(opt)) < 0) {
		saved_errno = errno;
		iclosesocket(s, test);
		freeaddrinfo(res);
		errno = saved_errno;
                i_errno = IESETBUF;
                return -1;
            }
            if (setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&opt, sizeof(opt)) < 0) {
		saved_errno = errno;
		iclosesocket(s, test);
		freeaddrinfo(res);
		errno = saved_errno;
                i_errno = IESETBUF;
                return -1;
            }
        }
    {
	unsigned int rate = test->settings->rate / 8;
	if (rate > 0) {
	    if (test->debug) {
		printf("Setting application pacing to %u\n", rate);
	    }
	}
    }
        opt = 1;
        if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt)) < 0) {
	        saved_errno = errno;
            iclosesocket(s, test);
            freeaddrinfo(res);
            errno = saved_errno;
            i_errno = IEREUSEADDR;
            return -1;
        }

	/*
	 * If we got an IPv6 socket, figure out if it should accept IPv4
	 * connections as well.  See documentation in netannounce() for
	 * more details.
	 */
#if defined(IPV6_V6ONLY) && !defined(__OpenBSD__)
	if (res->ai_family == AF_INET6 && (test->settings->domain == AF_UNSPEC || test->settings->domain == AF_INET)) {
	    if (test->settings->domain == AF_UNSPEC)
		opt = 0;
	    else
		opt = 1;
	    if (setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY,
			   (const char *) &opt, sizeof(opt)) < 0) {
		saved_errno = errno;
		iclosesocket(s, test);
		freeaddrinfo(res);
		errno = saved_errno;
		i_errno = IEV6ONLY;
		return -1;
	    }
	}
#endif /* IPV6_V6ONLY */

        if (bind(s, (struct sockaddr *) res->ai_addr, res->ai_addrlen) < 0) {
            iperf_err(test, "tcp-listen, bind2() failed: %s\n", STRERROR);
	        saved_errno = errno;
            iclosesocket(s, test);
            freeaddrinfo(res);
	        errno = saved_errno;
            i_errno = IESTREAMLISTEN;
            return -1;
        }

        freeaddrinfo(res);

        if (listen(s, INT_MAX) < 0) {
            i_errno = IESTREAMLISTEN;
            saved_errno = errno;
            iclosesocket(s, test);
            errno = saved_errno;
            return -1;
        }

        test->listener = s;
    }

    /* Read back and verify the sender socket buffer size */
    optlen = sizeof(sndbuf_actual);
    if (getsockopt(s, SOL_SOCKET, SO_SNDBUF, (char*)&sndbuf_actual, &optlen) < 0) {
	    saved_errno = errno;
	    iclosesocket(s, test);
        test->listener = -1;
	    errno = saved_errno;
	    i_errno = IESETBUF;
	    return -1;
    }
    
    if (test->debug) {
	printf("SNDBUF is %u, expecting %u\n", sndbuf_actual, test->settings->socket_bufsize);
    }
    if (test->settings->socket_bufsize && test->settings->socket_bufsize > sndbuf_actual) {
	    i_errno = IESETBUF2;
        iclosesocket(s, test);
	    return -1;
    }

    /* Read back and verify the receiver socket buffer size */
    optlen = sizeof(rcvbuf_actual);
    if (getsockopt(s, SOL_SOCKET, SO_RCVBUF, (char *)&rcvbuf_actual, &optlen) < 0) {
	    saved_errno = errno;
	    iclosesocket(s, test);
        test->listener = -1;
	    errno = saved_errno;
        i_errno = IESETBUF;
	    return -1;
    }
    if (test->debug) {
	printf("RCVBUF is %u, expecting %u\n", rcvbuf_actual, test->settings->socket_bufsize);
    }
    if (test->settings->socket_bufsize && test->settings->socket_bufsize > rcvbuf_actual) {
	    i_errno = IESETBUF2;
        iclosesocket(s, test);
	    return -1;
    }

    if (test->json_output) {
        cJSON_AddNumberToObject(test->json_start, "sock_bufsize", test->settings->socket_bufsize);
        cJSON_AddNumberToObject(test->json_start, "sndbuf_actual", sndbuf_actual);
        cJSON_AddNumberToObject(test->json_start, "rcvbuf_actual", rcvbuf_actual);
    }

    return s;
}


/* iperf_tcp_connect
 *
 * connect to a TCP stream listener
 * This function is roughly similar to netdial(), and may indeed have
 * been derived from it at some point, but it sets many TCP-specific
 * options between socket creation and connection.
 * Returns non-blocking socket
 */
int
iperf_tcp_connect(struct iperf_test *test)
{
    struct addrinfo *server_res;
    int s, opt;
    socklen_t optlen;
    int saved_errno;
    int rcvbuf_actual, sndbuf_actual;
    int proto = 0;

#if defined(HAVE_IPPROTO_MPTCP)
    if (test->mptcp)
        proto = IPPROTO_MPTCP;
#endif

    s = create_socket(test->settings->domain, SOCK_STREAM, proto, test->bind_address, test->bind_dev, test->bind_port, test->server_hostname, test->server_port, test, &server_res);
    if (s < 0) {
        iperf_err(test, "tcp-connect, create_socket failed: %s\n", STRERROR);
        i_errno = IESTREAMCONNECT;
        return -1;
    }

    /* Set socket options */
    if (test->no_delay) {
        opt = 1;
        if (setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&opt, sizeof(opt)) < 0) {
            saved_errno = errno;
            iclosesocket(s, test);
            freeaddrinfo(server_res);
            errno = saved_errno;
            i_errno = IESETNODELAY;
            return -1;
        }
    }
#ifndef __WIN32__
    if ((opt = test->settings->mss)) {
        if (setsockopt(s, IPPROTO_TCP, TCP_MAXSEG, &opt, sizeof(opt)) < 0) {
            saved_errno = errno;
            iclosesocket(s, test);
            freeaddrinfo(server_res);
            errno = saved_errno;
            i_errno = IESETMSS;
            return -1;
        }
    }
#endif
    if ((opt = test->settings->socket_bufsize)) {
       if (setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&opt, sizeof(opt)) < 0) {
            saved_errno = errno;
            iclosesocket(s, test);
            freeaddrinfo(server_res);
            errno = saved_errno;
            i_errno = IESETBUF;
            return -1;
        }
       if (setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&opt, sizeof(opt)) < 0) {
            saved_errno = errno;
            iclosesocket(s, test);
            freeaddrinfo(server_res);
            errno = saved_errno;
            i_errno = IESETBUF;
            return -1;
        }
    }
#if defined(HAVE_TCP_USER_TIMEOUT)
    if ((opt = test->settings->snd_timeout)) {
        if (setsockopt(s, IPPROTO_TCP, TCP_USER_TIMEOUT, &opt, sizeof(opt)) < 0) {
            saved_errno = errno;
            iclosesocket(s, test);
            freeaddrinfo(server_res);
            errno = saved_errno;
            i_errno = IESETUSERTIMEOUT;
            return -1;
        }
    }
#endif /* HAVE_TCP_USER_TIMEOUT */

    /* Read back and verify the sender socket buffer size */
    optlen = sizeof(sndbuf_actual);
    if (getsockopt(s, SOL_SOCKET, SO_SNDBUF, (char*)&sndbuf_actual, &optlen) < 0) {
        saved_errno = errno;
        iclosesocket(s, test);
        freeaddrinfo(server_res);
        errno = saved_errno;
        i_errno = IESETBUF;
        return -1;
    }
    if (test->debug) {
	printf("SNDBUF is %u, expecting %u\n", sndbuf_actual, test->settings->socket_bufsize);
    }
    if (test->settings->socket_bufsize && test->settings->socket_bufsize > sndbuf_actual) {
        iclosesocket(s, test);
        freeaddrinfo(server_res);
        i_errno = IESETBUF2;
        return -1;
    }

    /* Read back and verify the receiver socket buffer size */
    optlen = sizeof(rcvbuf_actual);
    if (getsockopt(s, SOL_SOCKET, SO_RCVBUF, (char*)&rcvbuf_actual, &optlen) < 0) {
        saved_errno = errno;
        iclosesocket(s, test);
        freeaddrinfo(server_res);
        errno = saved_errno;
        i_errno = IESETBUF;
        return -1;
    }
    if (test->debug) {
	printf("RCVBUF is %u, expecting %u\n", rcvbuf_actual, test->settings->socket_bufsize);
    }
    if (test->settings->socket_bufsize && test->settings->socket_bufsize > rcvbuf_actual) {
        iclosesocket(s, test);
        freeaddrinfo(server_res);
        i_errno = IESETBUF2;
        return -1;
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

#if defined(HAVE_FLOWLABEL)
    if (test->settings->flowlabel) {
        if (server_res->ai_addr->sa_family != AF_INET6) {
            saved_errno = errno;
            iclosesocket(s, test);
            freeaddrinfo(server_res);
            errno = saved_errno;
            i_errno = IESETFLOW;
            return -1;
	} else {
	    struct sockaddr_in6* sa6P = (struct sockaddr_in6*) server_res->ai_addr;
            char freq_buf[sizeof(struct in6_flowlabel_req)];
            struct in6_flowlabel_req *freq = (struct in6_flowlabel_req *)freq_buf;
            int freq_len = sizeof(*freq);

            memset(freq, 0, sizeof(*freq));
            freq->flr_label = htonl(test->settings->flowlabel & IPV6_FLOWINFO_FLOWLABEL);
            freq->flr_action = IPV6_FL_A_GET;
            freq->flr_flags = IPV6_FL_F_CREATE;
            freq->flr_share = IPV6_FL_S_ANY;
            memcpy(&freq->flr_dst, &sa6P->sin6_addr, 16);

            if (setsockopt(s, IPPROTO_IPV6, IPV6_FLOWLABEL_MGR, freq, freq_len) < 0) {
		saved_errno = errno;
                iclosesocket(s, test);
                freeaddrinfo(server_res);
		errno = saved_errno;
                i_errno = IESETFLOW;
                return -1;
            }
            sa6P->sin6_flowinfo = freq->flr_label;

            opt = 1;
            if (setsockopt(s, IPPROTO_IPV6, IPV6_FLOWINFO_SEND, &opt, sizeof(opt)) < 0) {
		saved_errno = errno;
                iclosesocket(s, test);
                freeaddrinfo(server_res);
		errno = saved_errno;
                i_errno = IESETFLOW;
                return -1;
            }
	}
    }
#endif /* HAVE_FLOWLABEL */

#if defined(HAVE_SO_MAX_PACING_RATE)
    /* If socket pacing is specified try to enable it. */
    if (test->settings->fqrate) {
        /* Convert bits per second to bytes per second */
        unsigned int fqrate = test->settings->fqrate / 8;
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

    /* Set common socket options */
    iperf_common_sockopts(test, s);

    if (timeout_connect(s, (struct sockaddr *) server_res->ai_addr, server_res->ai_addrlen, test->settings->connect_timeout) < 0) {
        iperf_err(test, "tcp-connect, connect() failed: %s\n", STRERROR);
        saved_errno = errno;
        iclosesocket(s, test);
        freeaddrinfo(server_res);
        errno = saved_errno;
        i_errno = IESTREAMCONNECT;
        return -1;
    }

    freeaddrinfo(server_res);

    /* Send cookie for verification */
    if (waitWrite(s, test->cookie, COOKIE_SIZE, Ptcp, test, ctrl_wait_ms) != COOKIE_SIZE) {
	saved_errno = errno;
	iclosesocket(s, test);
	errno = saved_errno;
        i_errno = IESENDCOOKIE;
        return -1;
    }

    return s;
}
