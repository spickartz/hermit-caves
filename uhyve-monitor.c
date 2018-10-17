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
#include <semaphore.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "uhyve-json.h"
#include "uhyve-monitor.h"
#include "uhyve.h"

#define MIN(a, b) (a) < (b) ? (a) : (b)
#define UHYVE_SOCK_PATH "/tmp/uhyve.sock"
#define JSON_TASK_STR "task"

static uint32_t uhyve_monitor_handle_start_app(json_value *json_task);
static uint32_t uhyve_monitor_handle_create_checkpoint(json_value *json_task);
static uint32_t uhyve_monitor_handle_load_checkpoint(json_value *json_task);
static uint32_t uhyve_monitor_handle_migrate(json_value *json_task);

extern uint8_t *guest_mem;
extern sem_t    monitor_sem;

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
static uint8_t               uhyve_monitor_initialized = 0;

typedef uint32_t (*task_handler_t)(json_value *json_task);
typedef struct _task_to_handler_elem {
	const char *   name;
	task_handler_t handler;
} task_to_handler_elem_t;

static const task_to_handler_elem_t task_to_handler[] = {
    {"start app", uhyve_monitor_handle_start_app},
    {"create checkpoint", uhyve_monitor_handle_create_checkpoint},
    {"load checkpoint", uhyve_monitor_handle_load_checkpoint},
    {"migrate", uhyve_monitor_handle_migrate},
};

static const int task_to_handler_len =
    sizeof(task_to_handler) / sizeof(task_to_handler[0]);

static int32_t
find_json_field(const char *field_name, json_value *json_task)
{
	uint32_t i = 0;
	for (i = 0; i < json_task->u.object.length; ++i) {
		const json_char *entry_name =
		    json_task->u.object.values[i].name;
		const size_t entry_name_length =
		    json_task->u.object.values[i].name_length;
		size_t max_n = MIN(entry_name_length, strlen(field_name));

		if (strncmp(entry_name, field_name, max_n) == 0)
			return i;
	}

	return -1;
}

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
 * \param length length of the task string
 *
 * This is the task handler that processes the json request to:
 * - migrate
 * - create/restore checkpoints
 * - start an application
 * - modify the guest configuration
 */
static uint32_t
uhyve_monitor_task_handler(void *task, size_t length)
{
	uint32_t status_code = 0;

	// parse the json task
	json_value *json_task = json_parse((const json_char *)task, length);

	// find task field
	int32_t task_index = -1;
	if ((task_index = find_json_field(JSON_TASK_STR, json_task)) < 0) {
		fprintf(
		    stderr,
		    "[ERROR] Json string does not contain a '%s' field. Abort!\n",
		    JSON_TASK_STR);
		return 400;
	}

	// determine task
	const json_char *task_name =
	    json_task->u.object.values[task_index].value->u.string.ptr;
	const size_t task_name_length =
	    json_task->u.object.values[task_index].value->u.string.length;

	uint32_t i = 0;
	for (i = 0; i < task_to_handler_len; ++i) {
		const size_t max_n =
		    MIN(task_name_length, strlen(task_to_handler[i].name));
		if (strncmp(task_name, task_to_handler[i].name, max_n) == 0) {
			status_code = task_to_handler[i].handler(json_task);
			break;
		}
	}

	// task not found -> return 'Not Implemented'
	if (i == task_to_handler_len) {
		fprintf(stderr,
			"[WARNING] Task '%s' not implemented.\n",
			task_name);
		status_code = 501;
	}

	return status_code;
}

/**
 * \brief Task handler for: application start
 */
static uint32_t
uhyve_monitor_handle_start_app(json_value *json_task)
{
	fprintf(stderr, "[INFO] Handling an application start event!\n");

	// find path field
	int32_t path_index = -1;
	if ((path_index = find_json_field("path", json_task)) < 0) {
		fprintf(
		    stderr,
		    "[ERROR] Start task is missing the 'path' field. Abort!\n");
		return 400;
	}

	// load the given application
	char *path = json_task->u.object.values[path_index].value->u.string.ptr;
	load_kernel(guest_mem, path);
	sem_post(&monitor_sem);

	return 200;
}

/**
 * \brief Task handler for: checkpoint
 */
static uint32_t
uhyve_monitor_handle_create_checkpoint(json_value *json_task)
{
	fprintf(stderr, "[INFO] Handling a checkpoint event!\n");
	return 501;
}

/**
 * \brief Task handler for: restore
 */
static uint32_t
uhyve_monitor_handle_load_checkpoint(json_value *json_task)
{
	fprintf(stderr, "[INFO] Handling a restore event!\n");
	return 501;
}

/**
 * \brief Task handler for: migration
 */
static uint32_t
uhyve_monitor_handle_migrate(json_value *json_task)
{
	fprintf(stderr, "[INFO] Handling a migration event!\n");
	return 501;
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
	uint32_t status_code = uhyve_monitor_task_handler(msg, bytes_to_read);
	free(msg);

	// return the status code to the requesting entity
	char status_code_str[4];
	sprintf(status_code_str, "%u", status_code);
	if (bufferevent_write(bev, status_code_str, 4) < 0) {
		err(1, "[ERROR] Could write to the event buffer.");
	}
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
	bufferevent_enable(bev, EV_READ | EV_WRITE);
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
	// did we already initialize?
	if (uhyve_monitor_initialized)
		return;

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

	uhyve_monitor_initialized = 1;
}

/**
 * \brief Frees monitor-related resources
 */
void
uhyve_monitor_destroy(void)
{
	// did we initialize the monitor?
	if (!uhyve_monitor_initialized)
		return;

	fprintf(stderr, "[INFO] Shutting down  the uhyve monitor ...\n");

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
