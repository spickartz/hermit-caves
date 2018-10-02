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
#include <event2/listener.h>
#include <event2/thread.h>
#include <pthread.h>
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
static pthread_t             uhyve_monitor_thread;

static void
uhyve_monitor_on_conn_event(struct bufferevent *bev, short events,
			    void *user_data)
{
	if (events & BEV_EVENT_EOF) {
		// free the event buffer
		bufferevent_free(bev);
	} else if (events & BEV_EVENT_ERROR) {
		perror("Got an error on the connection");
	}
}

/**
 * \brief The uyve task handler
 *
 * \param task A json string encoding the task
 *
 * This is the task handler that processes the json request to:
 * - migrate
 * - create/restore checkpoints
 * - start an application
 * - modify the guest configuration
 */
static void
uhyve_monitor_task_handler(void *task)
{
	// parse the json task
	json_value *json_task = json_parse((const json_char*)
	free(task);
}

/**
 * \brief Get a task string out of the event buffer
 */
static void
uhyve_monitor_receive_task(struct bufferevent *bev, void *user_data)
{
	// get the message out of the buffer
	struct evbuffer *input         = bufferevent_get_input(bev);
	size_t           bytes_to_read = evbuffer_get_length(input);
	void *           msg           = malloc(bytes_to_read);
	bufferevent_read(bev, msg, bytes_to_read);

	// pass the message to the task handler
	uhyve_monitor_task_handler(msg, bytes_to_read);
}

/**
 * \brief This callback is invoked once a client connects to the monitor
 */
static void
uhyve_monitor_on_accept(struct evconnlistener *listener, evutil_socket_t fd,
			struct sockaddr *sa, int socklen, void *user_data)
{

	// create a new buffer event socket and register callbacks
	struct bufferevent *bev;
	if ((bev = bufferevent_socket_new(
		 uhyve_monitor_event.evbase, fd, BEV_OPT_CLOSE_ON_FREE)) < 0) {
		err(1, "[ERROR] Could not construct bufferevent.");
	}
	bufferevent_setcb(bev,
			  uhyve_monitor_receive_task,
			  NULL,
			  uhyve_monitor_on_conn_event,
			  NULL);
	bufferevent_disable(bev, EV_WRITE);
	bufferevent_enable(bev, EV_READ);
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
	    uhyve_monitor_on_accept,
	    NULL,
	    LEV_OPT_REUSEABLE | LEV_OPT_CLOSE_ON_FREE,
	    -1,
	    (struct sockaddr *)&uhyve_monitor_sock.unix_sock_addr,
	    sizeof(uhyve_monitor_sock.unix_sock_addr));

	if (uhyve_monitor_sock.listener == NULL) {
		err(1, "[ERROR] Could not create the event listener.");
	}
}

/*
 * \brief The uhyve monitor event loop
 */
void *
uhyve_monitor_event_loop(void *args)
{
	if (event_base_dispatch(uhyve_monitor_event.evbase) < 0) {
		perror("[ERROR] Could not start the uhyve monitor event loop.");
	}
}

/**
 * \brief Initializes the uhyve monitor and starts the event  loop
 */
void
uhyve_monitor_init(void)
{
	fprintf(stderr, "[INFO] Initializing the uhyve monitor ...\n");

	// setup libevent to suppor threading
	if (evthread_use_pthreads() < 0) {
		err(1, "[ERROR] Could not enable thread support for libevent.");
	}

	// create the event base
	if ((uhyve_monitor_event.evbase = event_base_new()) == 0) {
		err(1, "[ERROR] Could not initialize libevent.");
	}

	// initialize the event socket
	uhyve_monitor_init_evconnlistener();

	// start the event loop
	if (pthread_create(
		&uhyve_monitor_thread, NULL, uhyve_monitor_event_loop, NULL)) {
		err(1, "[ERROR] Could not create the uhyve monitor event loop");
	}
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

	// wait for the monitor thread
	pthread_join(uhyve_monitor_thread, NULL);
}
