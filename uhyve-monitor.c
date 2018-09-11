/*
 * Copyright (c) 2018, Simon Pickartz, RWTH Aachen University
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *    * Neither the name of the University nor the names of its contributors
 *      may be used to endorse or promote products derived from this
 *      software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define _GNU_SOURCE

#include <err.h>
#include <event.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "uhyve-monitor.h"

#define UHYVE_SOCK_PATH "/tmp/uhyve.sock"

typedef struct uhyve_monitor_sock {
	struct evconnlistener *listener;
	int                    sock;
	struct sockaddr_un     unix_sock_addr;
	int                    len;
} uhyve_monitor_sock_t;

typedef struct uhyve_monitor_event {
	struct event       accept_ev;
	struct event_base *evbase;
} uhyve_monitor_event_t;

static uhyve_monitor_sock_t  uhyve_monitor_sock;
static uhyve_monitor_event_t uhyve_monitor_event;

static void
conn_eventcb(struct bufferevent *bev, short events, void *user_data)
{
	if (events & BEV_EVENT_EOF) {
		printf("Connection closed.\n");
	} else if (events & BEV_EVENT_ERROR) {
		printf("Got an error on the connection: %s\n",
		       strerror(errno)); /*XXX win32*/
	}
	/* None of the other events can happen here, since we haven't enabled
	 * timeouts */
	bufferevent_free(bev);
}

static void
conn_readcb(struct bufferevent *bev, void *user_data)
{
	struct evbuffer *output = bufferevent_get_output(bev);
	if (evbuffer_get_length(output) == 0) {
		printf("flushed answer\n");
		bufferevent_free(bev);
	}
}

/**
 * \brief The uyve monitor callback
 *
 * This is the listener callback that is used to receive json request to:
 * - migrate
 * - create/restore checkpoints
 * - start an application
 * - modify the guest configuration
 */
static void
uhyve_monitor_event_callback(struct evconnlistener *listener,
			     evutil_socket_t fd, struct sockaddr *sa,
			     int socklen, void *user_data)
{
	struct bufferevent *bev;

	if ((bev = bufferevent_socket_new(
		 uhyve_monitor_event.evbase, fd, BEV_OPT_CLOSE_ON_FREE)) < 0) {
		err(1, "[ERROR] Could not construct bufferevent.");
	}
	bufferevent_setcb(bev, conn_readcb, NULL, conn_eventcb, NULL);
	bufferevent_disable(bev, EV_WRITE);
	bufferevent_enable(bev, EV_READ);

	ssize_t num_bytes = bufferevent_get_max_to_read(bev);
	void *  msg       = malloc(num_bytes);
	bufferevent_read(bev, msg, num_bytes);

	printf("'s'\n", msg);
	free(msg);
}

/**
 * \brief Initializes the event socket
 */
static void
uhyve_monitor_init_evconnlistener(void)
{
	// cleanup old socket
	unlink(UHYVE_SOCK_PATH);

	memset(&uhyve_monitor_sock.unix_sock_addr,
	       0,
	       sizeof(&uhyve_monitor_sock.unix_sock_addr));
	uhyve_monitor_sock.unix_sock_addr.sun_family = AF_UNIX;
	strncpy(uhyve_monitor_sock.unix_sock_addr.sun_path,
		UHYVE_SOCK_PATH,
		sizeof(uhyve_monitor_sock.unix_sock_addr.sun_path) - 1);
	uhyve_monitor_sock.listener = evconnlistener_new_bind(
	    uhyve_monitor_event.evbase,
	    uhyve_monitor_event_callback,
	    NULL,
	    LEV_OPT_REUSEABLE | LEV_OPT_CLOSE_ON_FREE,
	    -1,
	    (struct sockaddr *)&uhyve_monitor_sock.unix_sock_addr,
	    sizeof(uhyve_monitor_sock.unix_sock_addr));

	if (listener == NULL) {
		err(1, "[ERROR] Could not create the event listener.")
	}
}

/**
 * \brief Initializes the uhyve monitor and starts the event  loop
 */
void
uhyve_monitor_init(void)
{
	fprintf(stderr, "[INFO] Initializing the uhyve monitor ...\n");

	// create the event base
	if ((uhyve_monitor_event.evbase = event_base_new()) == 0) {
		err("[ERROR] Could not initialize libevent.")
	}

	// initialize the event socket
	uhyve_monitor_init_evconnlistener();

	// start the event loop
	if (event_base_dispatch(uhyve_monitor_event.evbase) < 0) {
		perror("[ERROR] Could not start the uhyve monitor event loop.");
	}

	return;
}

/**
 * \brief Frees monitor-related resources
 */
void
uhyve_monitor_destroy(void)
{
	// close the uhyve monitor socket
	close(uhyve_monitor_sock.sock);

	// cleanup socket path
	unlink(UHYVE_SOCK_PATH);

	// exit the loop
	if (event_base_loopexit(uhyve_monitor_event.evbase, NULL) < 0) {
		err(1, "[ERROR] Could not exit the event loop.");
	}
}
