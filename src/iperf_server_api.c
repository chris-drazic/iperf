/*
 * iperf, Copyright (c) 2014-2023 The Regents of the University of
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
/* iperf_server_api.c: Functions to be used by an iperf server
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <fcntl.h>
#ifndef __WIN32__
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/resource.h>
#endif
#include <stdint.h>
#include <sys/time.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>

#include "iperf.h"
#include "iperf_api.h"
#include "iperf_udp.h"
#include "iperf_tcp.h"
#include "iperf_util.h"
#include "timer.h"
#include "iperf_time.h"
#include "net.h"
#include "units.h"
#include "iperf_util.h"
#include "iperf_locale.h"

#if defined(HAVE_TCP_CONGESTION)
#if !defined(TCP_CA_NAME_MAX)
#define TCP_CA_NAME_MAX 16
#endif /* TCP_CA_NAME_MAX */
#endif /* HAVE_TCP_CONGESTION */

void *
iperf_server_worker_run(void *s) {
    struct iperf_stream *sp = (struct iperf_stream *) s;
    struct iperf_test *test = sp->test;

    /* Blocking signal to make sure that signal will be handled by main thread */
    sigset_t set;
    sigemptyset(&set);
#ifdef SIGTERM
    sigaddset(&set, SIGTERM);
#endif
#ifdef SIGHUP
    sigaddset(&set, SIGHUP);
#endif
#ifdef SIGINT
    sigaddset(&set, SIGINT);
#endif
    if (pthread_sigmask(SIG_BLOCK, &set, NULL) != 0) {
	    i_errno = IEPTHREADSIGMASK;
	    goto cleanup_and_fail;
    }

    /* Allow this thread to be cancelled even if it's in a syscall */
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

    while (! (test->done) && ! (sp->done)) {
        if (sp->sender) {
            if (iperf_send_mt(sp) < 0) {
                goto cleanup_and_fail;
            }
        }
        else {
            if (iperf_recv_mt(sp) < 0) {
                goto cleanup_and_fail;
            }
        }
    }
    return NULL;

  cleanup_and_fail:
    return NULL;
}

int
iperf_server_listen(struct iperf_test *test)
{
    retry:
    if((test->listener = netannounce(test->settings->domain, Ptcp, test->bind_address,
        test->bind_dev, test->server_port, test)) < 0) {
	if (errno == EAFNOSUPPORT && (test->settings->domain == AF_INET6 || test->settings->domain == AF_UNSPEC)) {
	    /* If we get "Address family not supported by protocol", that
	    ** probably means we were compiled with IPv6 but the running
	    ** kernel does not actually do IPv6.  This is not too unusual,
	    ** v6 support is and perhaps always will be spotty.
	    */
	    iperf_err(test, "this system does not seem to support IPv6 - trying IPv4");
	    test->settings->domain = AF_INET;
	    goto retry;
	} else {
	    i_errno = IELISTEN;
	    return -1;
	}
    }

    if (!test->json_output) {
        if (test->server_last_run_rc != 2)
            test->server_test_number +=1;
        if (test->debug || test->server_last_run_rc != 2) {
	    iperf_printf(test, "-----------------------------------------------------------\n");
	    iperf_printf(test, "Server listening on %s %s %d\n",
                     test->bind_dev ? test->bind_dev : "",
                     test->bind_address ? test->bind_address : "", test->server_port);
	    iperf_printf(test, "-----------------------------------------------------------\n");
	    if (test->forceflush)
	        iflush(test);
        }
    }
    setnonblocking(test->listener, 1);

    FD_ZERO(&test->read_set);
    FD_ZERO(&test->write_set);
    IFD_SET(test->listener, &test->read_set, test);

    return 0;
}

int
iperf_accept(struct iperf_test *test)
{
    int s = -1;
    int ret = -1;
    signed char rbuf = ACCESS_DENIED;
    socklen_t len;
    struct sockaddr_storage addr;

    if (test->debug) {
        iperf_err(test, "iperf-accept called.\n");
    }

    len = sizeof(addr);
    if ((s = accept(test->listener, (struct sockaddr *) &addr, &len)) < 0) {
        i_errno = IEACCEPT;
        return ret;
    }

    if (test->ctrl_sck == -1) {
        /* Server free, accept new client */
        test->ctrl_sck = s;
        // set TCP_NODELAY for lower latency on control messages
        int flag = 1;
        if (setsockopt(test->ctrl_sck, IPPROTO_TCP, TCP_NODELAY, (char *) &flag, sizeof(int))) {
            i_errno = IESETNODELAY;
            goto error_handling;
        }

#if defined(HAVE_TCP_USER_TIMEOUT)
        int opt;
        if ((opt = test->settings->snd_timeout)) {
            if (setsockopt(s, IPPROTO_TCP, TCP_USER_TIMEOUT, &opt, sizeof(opt)) < 0) {
                i_errno = IESETUSERTIMEOUT;
                goto error_handling;
            }
        }
#endif /* HAVE_TCP_USER_TIMEOUT */

#if defined (HAVE_TCP_KEEPALIVE)
        // Set Control Connection TCP Keepalive (especially useful for long UDP test sessions)
        if (iperf_set_control_keepalive(test) < 0)
            return -1;
#endif //HAVE_TCP_KEEPALIVE
            setnonblocking(s, 1);

            int rv = waitRead(test->ctrl_sck, test->cookie, COOKIE_SIZE, Ptcp, test, ctrl_wait_ms);
        if (rv != COOKIE_SIZE) {
            /*
             * Note this error covers both the case of a system error
             * or the inability to read the correct amount of data
             * (i.e. timed out).
             */
            iperf_err(test, "Accept problem, ctrl-sck: %d  s: %d  listener: %d waitRead rv: %d\n",
                      test->ctrl_sck, s, test->listener, rv);
            i_errno = IERECVCOOKIE;
            goto error_handling;
        }
        IFD_SET(test->ctrl_sck, &test->read_set, test);
        if (test->ctrl_sck > test->max_fd) test->max_fd = test->ctrl_sck;

        if (iperf_set_send_state(test, PARAM_EXCHANGE) != 0)
            goto error_handling;
        if (iperf_exchange_parameters(test) < 0)
            goto error_handling;
        if (test->server_affinity != -1) {
            if (iperf_setaffinity(test, test->server_affinity) != 0)
                goto error_handling;
        }
        if (test->on_connect)
            test->on_connect(test);
    } else {
        /*
         * Don't try to read from the socket.  It could block an ongoing test.
         * Just send ACCESS_DENIED, ignore any error, don't care if we cannot send the bytes immediately.
         * Also, if sending failed, don't return an error, as the request is not related
         * to the ongoing test, and returning an error will terminate the test.
         */
        Nwrite(s, (char*) &rbuf, sizeof(rbuf), Ptcp, test);
        iclosesocket(s, test);
    }
    return 0;
    error_handling:
    iclosesocket(s, test);
    return ret;
}


/**************************************************************************/
int
iperf_handle_message_server(struct iperf_test *test)
{
    int rval;
    struct iperf_stream *sp;
    signed char s;

    // if (test->debug) {
    //     iperf_err(test, "Reading new State from the Client - current state is %d-%s\n", test->state, state_to_text(test->state));
    // }

    // XXX: Need to rethink how this behaves to fit API
    if ((rval = waitRead(test->ctrl_sck, (char*) &s, sizeof(s), Ptcp, test, ctrl_wait_ms)) != sizeof(s)) {
        iperf_err(test, "The client has unexpectedly closed the connection (handle-message-server): %s  rval: %d",
                  STRERROR, rval);
        if (rval == 0 || (rval == NET_HANGUP)) {
            i_errno = IECTRLCLOSE;
            return -1;
        } else {
            i_errno = IERECVMESSAGE;
            return -1;
        }
    }
    else {
        iperf_set_state(test, s, __FUNCTION__);
    }

    if (test->debug_level >= DEBUG_LEVEL_INFO) {
        iperf_printf(test, "State change: server received and changed State to %d-%s\n", test->state, state_to_text(test->state));
    }

    switch(test->state) {
        case TEST_START:
            break;
        case TEST_END:
            test->done = 1;
            cpu_util(test->cpu_util);
            test->stats_callback(test);
            SLIST_FOREACH(sp, &test->streams, streams) {
                iclosesocket(sp->socket, test);
                sp->socket = -1;
            }
            if (iperf_set_send_state(test, EXCHANGE_RESULTS) != 0)
                return -1;
            if (iperf_exchange_results(test) < 0)
                return -1;
            if (iperf_set_send_state(test, DISPLAY_RESULTS) != 0)
                return -1;
            if (test->on_test_finish)
                test->on_test_finish(test);
            test->reporter_callback(test);
            break;
        case IPERF_DONE:
            break;
        case CLIENT_TERMINATE:
            i_errno = IECLIENTTERM;

	    // Temporarily be in DISPLAY_RESULTS phase so we can get
	    // ending summary statistics.
	    signed char oldstate = test->state;
	    cpu_util(test->cpu_util);
            iperf_set_state(test, DISPLAY_RESULTS, __FUNCTION__);
	    test->reporter_callback(test);
            iperf_set_state(test, oldstate, __FUNCTION__);

            // XXX: Remove this line below!
	    iperf_err(test, "the client has terminated");
            SLIST_FOREACH(sp, &test->streams, streams) {
                iclosesocket(sp->socket, test);
                sp->socket = -1;
            }
            iperf_set_state(test, IPERF_DONE, __FUNCTION__);
            break;
        default:
            i_errno = IEMESSAGE;
            return -1;
    }

    return 0;
}

static void
server_timer_proc(TimerClientData client_data, struct iperf_time *nowP)
{
    struct iperf_test *test = client_data.p;
    struct iperf_stream *sp;

    test->timer = NULL;
    if (test->done)
        return;
    test->done = 1;
    /* Free streams */
    while (!SLIST_EMPTY(&test->streams)) {
        sp = SLIST_FIRST(&test->streams);
        SLIST_REMOVE_HEAD(&test->streams, streams);
        iclosesocket(sp->socket, test);
        iperf_free_stream(sp);
    }
    iclosesocket(test->ctrl_sck, test);
}

static void
server_stats_timer_proc(TimerClientData client_data, struct iperf_time *nowP)
{
    struct iperf_test *test = client_data.p;

    if (test->done)
        return;
    if (test->stats_callback)
	test->stats_callback(test);
}

static void
server_reporter_timer_proc(TimerClientData client_data, struct iperf_time *nowP)
{
    struct iperf_test *test = client_data.p;

    if (test->done)
        return;
    if (test->reporter_callback)
	test->reporter_callback(test);
}

static int
create_server_timers(struct iperf_test * test)
{
    struct iperf_time now;
    TimerClientData cd;
    int max_rtt = 4; /* seconds */
    int state_transitions = 10; /* number of state transitions in iperf3 */
    int grace_period = max_rtt * state_transitions;

    if (iperf_time_now(&now) < 0) {
	i_errno = IEINITTEST;
	return -1;
    }
    cd.p = test;
    test->timer = test->stats_timer = test->reporter_timer = NULL;
    if (test->duration != 0 ) {
        test->done = 0;
        test->timer = tmr_create(&now, server_timer_proc, cd, (test->duration + test->omit + grace_period) * SEC_TO_US, 0);
        if (test->timer == NULL) {
            i_errno = IEINITTEST;
            return -1;
        }
    }

    test->stats_timer = test->reporter_timer = NULL;
    if (test->stats_interval != 0) {
        test->stats_timer = tmr_create(&now, server_stats_timer_proc, cd, test->stats_interval * SEC_TO_US, 1);
        if (test->stats_timer == NULL) {
            i_errno = IEINITTEST;
            return -1;
	}
    }
    if (test->reporter_interval != 0) {
        test->reporter_timer = tmr_create(&now, server_reporter_timer_proc, cd, test->reporter_interval * SEC_TO_US, 1);
        if (test->reporter_timer == NULL) {
            i_errno = IEINITTEST;
            return -1;
	}
    }
    return 0;
}

static void
server_omit_timer_proc(TimerClientData client_data, struct iperf_time *nowP)
{
    struct iperf_test *test = client_data.p;

    test->omit_timer = NULL;
    test->omitting = 0;
    iperf_reset_stats(test);
    if (test->verbose && !test->json_output && test->reporter_interval == 0)
	iperf_printf(test, "%s", report_omit_done);

    /* Reset the timers. */
    if (test->stats_timer != NULL)
	tmr_reset(nowP, test->stats_timer);
    if (test->reporter_timer != NULL)
	tmr_reset(nowP, test->reporter_timer);
}

static int
create_server_omit_timer(struct iperf_test * test)
{
    struct iperf_time now;
    TimerClientData cd;

    if (test->omit == 0) {
	test->omit_timer = NULL;
	test->omitting = 0;
    } else {
	if (iperf_time_now(&now) < 0) {
	    i_errno = IEINITTEST;
	    return -1;
	}
	test->omitting = 1;
	cd.p = test;
	test->omit_timer = tmr_create(&now, server_omit_timer_proc, cd, test->omit * SEC_TO_US, 0);
	if (test->omit_timer == NULL) {
	    i_errno = IEINITTEST;
	    return -1;
	}
    }

    return 0;
}

void cleanup_server(struct iperf_test *test)
{
    struct iperf_stream *sp;

    /* Cancel outstanding threads */
    int i_errno_save = i_errno;
    SLIST_FOREACH(sp, &test->streams, streams) {
        int rc;
        sp->done = 1;
        if (sp->thread_created == 1) {
            rc = pthread_cancel(sp->thr);
            if (rc != 0 && rc != ESRCH) {
                i_errno = IEPTHREADCANCEL;
                errno = rc;
                iperf_err(test, "cleanup_server in pthread_cancel - %s", iperf_strerror(i_errno));
            }
            rc = pthread_join(sp->thr, NULL);
            if (rc != 0 && rc != ESRCH) {
                i_errno = IEPTHREADJOIN;
                errno = rc;
                iperf_err(test, "cleanup_server in pthread_join - %s", iperf_strerror(i_errno));
            }
            if (test->debug_level >= DEBUG_LEVEL_INFO) {
                iperf_printf(test, "Thread FD %d stopped\n", sp->socket);
            }
            sp->thread_created = 0;
        }
    }
    i_errno = i_errno_save;

    if (test->debug_level >= DEBUG_LEVEL_INFO) {
        iperf_printf(test, "All threads stopped\n");
    }

    /* Close open streams */
    SLIST_FOREACH(sp, &test->streams, streams) {
	if (sp->socket > -1) {
            IFD_CLR(sp->socket, &test->read_set, test);
            IFD_CLR(sp->socket, &test->write_set, test);
            close(sp->socket);
            sp->socket = -1;
	}
    }

    /* Close open test sockets */
    iclosesocket(test->ctrl_sck, test);
    iclosesocket(test->listener, test);
    if (test->prot_listener != -1) {     // May remain open if create socket failed
	    iclosesocket(test->prot_listener, test);
        test->prot_listener = -1;
    }

    /* Free streams */
    while (!SLIST_EMPTY(&test->streams)) {
        sp = SLIST_FIRST(&test->streams);
        SLIST_REMOVE_HEAD(&test->streams, streams);
        iclosesocket(sp->socket, test);
        iperf_free_stream(sp);
    }

    /* Cancel any remaining timers. */
    if (test->stats_timer != NULL) {
        tmr_cancel(test->stats_timer);
        test->stats_timer = NULL;
    }
    if (test->reporter_timer != NULL) {
        tmr_cancel(test->reporter_timer);
        test->reporter_timer = NULL;
    }
    if (test->omit_timer != NULL) {
        tmr_cancel(test->omit_timer);
        test->omit_timer = NULL;
    }
    if (test->congestion_used != NULL) {
        free(test->congestion_used);
	    test->congestion_used = NULL;
    }
    if (test->timer != NULL) {
        tmr_cancel(test->timer);
        test->timer = NULL;
    }
    iperf_set_state(test, IPERF_DONE, __FUNCTION__);
}


int
iperf_run_server(struct iperf_test *test)
{
    int result, s;
    int send_streams_accepted, rec_streams_accepted;
    int streams_to_send = 0, streams_to_rec = 0;
#if defined(HAVE_TCP_CONGESTION)
    int saved_errno;
#endif /* HAVE_TCP_CONGESTION */
    fd_set read_set, write_set;
    struct iperf_stream *sp;
    struct iperf_time now;
    struct iperf_time last_receive_time;
    struct iperf_time diff_time;
    struct timeval* timeout;
    struct timeval used_timeout;
    iperf_size_t last_receive_blocks;
    int flag;
    int64_t t_usecs;
    int64_t timeout_us;
    int64_t rcv_timeout_us;
    unsigned long last_dbg = 0;

    if (test->logfile) {
        if (iperf_open_logfile(test) < 0)
            return -2;
    }

    if (test->affinity != -1) {
	if (iperf_setaffinity(test, test->affinity) != 0) {
            cleanup_server(test);
	    return -2;
        }
    }

    if (test->json_output) {
	if (iperf_json_start(test) < 0) {
            cleanup_server(test);
	    return -2;
        }
    }

    if (test->json_output) {
	cJSON_AddItemToObject(test->json_start, "version", cJSON_CreateString(version));
	cJSON_AddItemToObject(test->json_start, "system_info", cJSON_CreateString(get_system_info()));
    } else if (test->verbose) {
	iperf_printf(test, "%s\n", version);
	iperf_printf(test, "%s", "");
	iperf_printf(test, "%s\n", get_system_info());
	iflush(test);
    }

    // Open socket and listen
    if (iperf_server_listen(test) < 0) {
	cleanup_server(test);
        return -2;
    }

    iperf_time_now(&last_receive_time); // Initialize last time something was received
    last_receive_blocks = 0;

    iperf_set_state(test, IPERF_START, __FUNCTION__);
    send_streams_accepted = 0;
    rec_streams_accepted = 0;
    rcv_timeout_us = (test->settings->rcv_timeout.secs * SEC_TO_US) + test->settings->rcv_timeout.usecs;

    while (test->state != IPERF_DONE) {

        // Check if average transfer rate was exceeded (condition set in the callback routines)
	if (test->bitrate_limit_exceeded) {
	    cleanup_server(test);
            i_errno = IETOTALRATE;
            return -1;
	}

        memcpy(&read_set, &test->read_set, sizeof(fd_set));
        memcpy(&write_set, &test->write_set, sizeof(fd_set));

	iperf_time_now(&now);
	timeout = tmr_timeout(&now);

        // Ensure select() will timeout to allow handling error cases that require server restart
        if (test->state == IPERF_START) {       // In idle mode server may need to restart
            if (timeout == NULL && test->settings->idle_timeout > 0) {
                used_timeout.tv_sec = test->settings->idle_timeout;
                used_timeout.tv_usec = 0;
                timeout = &used_timeout;
            }
        } else if (test->mode != SENDER) {     // In non-reverse active mode server ensures data is received
            timeout_us = -1;
            if (timeout != NULL) {
                used_timeout.tv_sec = timeout->tv_sec;
                used_timeout.tv_usec = timeout->tv_usec;
                timeout_us = (timeout->tv_sec * SEC_TO_US) + timeout->tv_usec;
            }
            /* Cap the maximum select timeout at 1 second */
            if (timeout_us > SEC_TO_US) {
                timeout_us = SEC_TO_US;
            }
            if (timeout_us < 0 || timeout_us > rcv_timeout_us) {
                used_timeout.tv_sec = test->settings->rcv_timeout.secs;
                used_timeout.tv_usec = test->settings->rcv_timeout.usecs;
            }
            timeout = &used_timeout;
        }

        if (test->debug > 1 || (test->debug && (last_dbg != now.secs))) {
            if (timeout)
                iperf_err(test, "timeout: %ld.%06ld  max-fd: %d state: %d (%s)",
                          (long)(timeout->tv_sec), (long)(timeout->tv_usec), test->max_fd,
                          test->state, iperf_get_state_str(test->state));
            else
                iperf_err(test, "timeout NULL, max-fd: %d state: %d(%s)", test->max_fd,
                          test->state, iperf_get_state_str(test->state));
            print_fdset(test->max_fd, &read_set, &write_set, test);
        }

        result = select(test->max_fd + 1, &read_set, &write_set, NULL, timeout);
        if (test->debug > 1 || (test->debug && (last_dbg != now.secs))) {
            iperf_err(test, "select result: %d, listener: %d  ISSET-listener: %d  test-state: %d(%s)\n",
                    result, test->listener, test->listener >= 0 ? FD_ISSET(test->listener, &read_set) : -1, test->state,
                    iperf_get_state_str(test->state));
            iperf_err(test, "prot-listener: %d  ISSET: %d  max-fd: %d",
                    test->prot_listener, test->prot_listener >= 0 ? FD_ISSET(test->prot_listener, &read_set) : -1, test->max_fd);
            print_fdset(test->max_fd, &read_set, &write_set, test);
            last_dbg = now.secs;
        }
        if (result < 0 && errno != EINTR) {
            iperf_err(stderr, "Cleaning server, select had error: %s\n", STRERROR);
            i_errno = IESELECT;
            return -1;
        } else if (result == 0) {
            /*
             * If nothing was received during the specified time (per
             * state) then probably something got stuck either at the
             * client, server or network, and test should be forced to
             * end.
            */
            iperf_time_now(&now);
            t_usecs = 0;
            if (iperf_time_diff(&now, &last_receive_time, &diff_time) == 0) {
                t_usecs = iperf_time_in_usecs(&diff_time);

                /* We're in the state where we're still accepting connections */
                if (test->state == IPERF_START) {
                    if (test->settings->idle_timeout > 0 && t_usecs >= test->settings->idle_timeout * SEC_TO_US) {
                        test->server_forced_idle_restarts_count += 1;
                        if (test->debug)
                            printf("Server restart (#%d) in idle state as no connection request was received for %d sec\n",
                                test->server_forced_idle_restarts_count, test->settings->idle_timeout);
                        cleanup_server(test);
			if ( iperf_get_test_one_off(test) ) {
			  if (test->debug)
                            printf("No connection request was received for %d sec in one-off mode; exiting.\n",
				   test->settings->idle_timeout);
			  exit(0);
			}

                        return 2;
                    }
                }

                /*
                 * Running a test. If we're receiving, be sure we're making
                 * progress (sender hasn't died/crashed).
                 */
                else if (test->mode != SENDER && t_usecs > rcv_timeout_us) {
                    /* Idle timeout if no new blocks received */
                    if (test->blocks_received == last_receive_blocks) {
                        test->server_forced_no_msg_restarts_count += 1;
                        i_errno = IENOMSG;
                        if (iperf_get_verbose(test))
                            iperf_err(test, "Server restart (#%d) during active test due to idle timeout for receiving data",
                                      test->server_forced_no_msg_restarts_count);
                        cleanup_server(test);
                        return -1;
                    }
                }
            }
        }

        /* See if the test is making progress */
        if (test->blocks_received > last_receive_blocks) {
            last_receive_blocks = test->blocks_received;
            last_receive_time = now;
        }

	if (result > 0) {
            // Check listener socket
            if (test->listener >= 0 && FD_ISSET(test->listener, &read_set)) {
                if (test->state != CREATE_STREAMS) {
                    if (iperf_accept(test) < 0) {
                        return -1;
                    }

                    // Set streams number
                    if (test->mode == BIDIRECTIONAL) {
                        streams_to_send = test->num_streams;
                        streams_to_rec = test->num_streams;
                    } else if (test->mode == RECEIVER) {
                        streams_to_rec = test->num_streams;
                        streams_to_send = 0;
                    } else {
                        streams_to_send = test->num_streams;
                        streams_to_rec = 0;
                    }
                }
            }
            // Check control socket
            if (test->ctrl_sck >= 0 && FD_ISSET(test->ctrl_sck, &read_set)) {
                if (iperf_handle_message_server(test) < 0) {
                    return -1;
		        }
            }

            if (test->state == CREATE_STREAMS) {
                if (test->prot_listener >= 0 && FD_ISSET(test->prot_listener, &read_set)) {

                    if ((s = test->protocol->accept(test)) < 0) {
                        return -1;
		    }
            /* Use non-blocking IO so we don't accidentally end up
            * hanging on socket operations. */
            setnonblocking(s, 1);
            
            if (test->debug) {
                iperf_err(stderr, "create-streams, accepted socket: %d\n", s);
            }

		    /* apply other common socket options */
                    if (iperf_common_sockopts(test, s) < 0)
                    {
                        cleanup_server(test);
                        return -1;
                    }

                    if (!is_closed(s)) {

#if defined(HAVE_TCP_USER_TIMEOUT)
		    if (test->protocol->id == Ptcp) {
                        int opt;
                        if ((opt = test->settings->snd_timeout)) {
                            if (setsockopt(s, IPPROTO_TCP, TCP_USER_TIMEOUT, &opt, sizeof(opt)) < 0) {
                                saved_errno = errno;
                                close(s);
                                cleanup_server(test);
                                errno = saved_errno;
                                i_errno = IESETUSERTIMEOUT;
                                return -1;
                            }
                        }
                    }
#endif /* HAVE_TCP_USER_TIMEOUT */

#if defined(HAVE_TCP_CONGESTION)
		    if (test->protocol->id == Ptcp) {
			if (test->congestion) {
			    if (setsockopt(s, IPPROTO_TCP, TCP_CONGESTION, test->congestion, strlen(test->congestion)) < 0) {
				/*
				 * ENOENT means we tried to set the
				 * congestion algorithm but the algorithm
				 * specified doesn't exist.  This can happen
				 * if the client and server have different
				 * congestion algorithms available.  In this
				 * case, print a warning, but otherwise
				 * continue.
				 */
				if (errno == ENOENT) {
				    iperf_err(test, "TCP congestion control algorithm not supported");
				}
				else {
				    saved_errno = errno;
				    iclosesocket(s, test);
				    errno = saved_errno;
				    i_errno = IESETCONGESTION;
				    return -1;
				}
			    }
			}
			{
			    socklen_t len = TCP_CA_NAME_MAX;
			    char ca[TCP_CA_NAME_MAX + 1];
                            int rc;
			    rc = getsockopt(s, IPPROTO_TCP, TCP_CONGESTION, ca, &len);
                            if (rc < 0 && test->congestion) {
				saved_errno = errno;
				iclosesocket(s, test);
				errno = saved_errno;
				i_errno = IESETCONGESTION;
				return -1;
			    }
                            /*
                             * If not the first connection, discard prior
                             * congestion algorithm name so we don't leak
                             * duplicated strings.  We probably don't need
                             * the old string anyway.
                             */
                            if (test->congestion_used != NULL) {
                                free(test->congestion_used);
                            }
                            // Set actual used congestion alg, or set to unknown if could not get it
                            if (rc < 0)
                                test->congestion_used = strdup("unknown");
                            else
                                test->congestion_used = strdup(ca);
			    if (test->debug) {
				printf("Congestion algorithm is %s\n", test->congestion_used);
			    }
			}
		    }
#endif /* HAVE_TCP_CONGESTION */

                        if (rec_streams_accepted != streams_to_rec) {
                            flag = 0;
                            ++rec_streams_accepted;
                        } else if (send_streams_accepted != streams_to_send) {
                            flag = 1;
                            ++send_streams_accepted;
                        }

                        if (flag != -1) {
                            sp = iperf_new_stream(test, s, flag);
                            if (!sp) {
                                return -1;
                            }

                            if (s > test->max_fd) test->max_fd = s;

                            if (test->on_new_stream)
                                test->on_new_stream(sp);

                            flag = -1;
                        }
                    }
                }


                if (rec_streams_accepted == streams_to_rec && send_streams_accepted == streams_to_send) {
                    if (test->protocol->id != Ptcp) {
                        // Stop listening for more protocol connections, we are full.
                        iclosesocket(test->prot_listener, test);
                    } else {
                        if (test->no_delay || test->settings->mss || test->settings->socket_bufsize) {
                            // Re-open protocol listener socket, I am not sure why. --Ben
                            iclosesocket(test->prot_listener, test);
                            if ((s = netannounce(test->settings->domain, Ptcp, test->bind_address, test->bind_dev, test->server_port, test)) < 0) {
                                i_errno = IELISTEN;
                                return -1;
                            }

                            setnonblocking(s, 1);

                            test->listener = s;
                            IFD_SET(test->listener, &test->read_set, test);
                        }
                    }
                    test->prot_listener = -1;

		    /* Ensure that total requested data rate is not above limit */
		    iperf_size_t total_requested_rate = test->num_streams * test->settings->rate * (test->mode == BIDIRECTIONAL? 2 : 1);
		    if (test->settings->bitrate_limit > 0 && total_requested_rate > test->settings->bitrate_limit) {
                        if (iperf_get_verbose(test))
                            iperf_err(test, "Client total requested throughput rate of %" PRIu64 " bps exceeded %" PRIu64 " bps limit",
                                      total_requested_rate, test->settings->bitrate_limit);
			cleanup_server(test);
			i_errno = IETOTALRATE;
			return -1;
		    }

		    // Begin calculating CPU utilization
		    cpu_util(NULL);

		    if (iperf_set_send_state(test, TEST_START) != 0) {
                        return -1;
		    }
                    if (iperf_init_test(test) < 0) {
                        return -1;
		    }
		    if (create_server_timers(test) < 0) {
                        return -1;
		    }
		    if (create_server_omit_timer(test) < 0) {
                        return -1;
		    }
		    if (test->mode != RECEIVER)
			if (iperf_create_send_timers(test) < 0) {
			    return -1;
			}
		    if (iperf_set_send_state(test, TEST_RUNNING) != 0) {
                        return -1;
		    }

                    /* Create and spin up threads */
                    pthread_attr_t attr;
                    if (pthread_attr_init(&attr) != 0) {
                        i_errno = IEPTHREADATTRINIT;
                        cleanup_server(test);
                    };

                    SLIST_FOREACH(sp, &test->streams, streams) {
                        if (pthread_create(&(sp->thr), &attr, &iperf_server_worker_run, sp) != 0) {
                            i_errno = IEPTHREADCREATE;
                            cleanup_server(test);
                            return -1;
                        }
                        sp->thread_created = 1;
                        if (test->debug_level >= DEBUG_LEVEL_INFO) {
                            iperf_printf(test, "Thread FD %d created\n", sp->socket);
                        }
                    }
                    if (test->debug_level >= DEBUG_LEVEL_INFO) {
                        iperf_printf(test, "All threads created\n");
                    }
                    if (pthread_attr_destroy(&attr) != 0) {
                        i_errno = IEPTHREADATTRDESTROY;
                        cleanup_server(test);
                    };
                }
            } /* if test is running state */
        } /* if some file descriptor has data to read/write */

	if (result == 0 ||
	    (timeout != NULL && timeout->tv_sec == 0 && timeout->tv_usec == 0)) {
	    /* Run the timers. */
        if (test->debug > 1)
            iperf_err(test, "Running timers..\n");
	    iperf_time_now(&now);
	    tmr_run(&now);
        if (test->debug > 1)
            iperf_err(test, "Done with timers..\n");
	}
    }

    if (test->state == CREATE_STREAMS) {
        // If it has been too long, then consider the test a failure and return.
        if (test->create_streams_state_at + 5000 < getCurMs()) {
            iperf_err(test, "Test has been in create-streams state for: %llums, aborting.\n",
                        (unsigned long long)(getCurMs() - test->create_streams_state_at));
            return -1;
        }
    }


    if (test->json_output) {
	if (iperf_json_finish(test) < 0)
	    return -1;
    }

    iflush(test);
    if (test->debug)
        iperf_err(test, "Done with server loop, cleaning up server.\n");

    if (test->server_affinity != -1)
	if (iperf_clearaffinity(test) != 0)
	    return -1;

    return 0;
}
