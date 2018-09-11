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
        int                sock;
        struct sockaddr_un unix_sock_addr;
        int                len;
} uhyve_monitor_sock_t;

typedef struct uhyve_monitor_event {
	struct event accept_ev;
	struct event_base *evbase;
} uhyve_monitor_event_t;

static uhyve_monitor_sock_t uhyve_monitor_sock;
static uhyve_monitor_event_t uhyve_monitor_event;

/**
 * \brief The uyve monitor callback
 *
 * This is the listener callback that is used to receive json request to:
 * - migrate
 * - create/restore checkpoints
 * - start an application
 * - modify the guest configuration
 */
void
uhyve_monitor_event_callback(int fd, short ev, void *arg)
{
        fprintf(stderr, "[WARNING] The event loop ist not implemented yet.");
}

/**
 * \brief Initializes the event socket
 */
static void
uhyve_monitor_init_ev_sock(void)
{
        // cleanup old socket
        unlink(UHYVE_SOCK_PATH);

        // create the uhyve monitor socket
        if ((uhyve_monitor_sock.sock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
                perror("[ERROR] Could not create the uhyve monitor socket");
                exit(EXIT_FAILURE);
        }

        // bind the socket to UHYVE_SOCK_PATH
        uhyve_monitor_sock.unix_sock_addr.sun_family = AF_UNIX;
        strcpy(uhyve_monitor_sock.unix_sock_addr.sun_path, UHYVE_SOCK_PATH);

        uhyve_monitor_sock.len =
            sizeof(uhyve_monitor_sock.unix_sock_addr.sun_family) +
            strlen(uhyve_monitor_sock.unix_sock_addr.sun_path);

        if (bind(uhyve_monitor_sock.sock,
                 &(uhyve_monitor_sock.unix_sock_addr),
                 uhyve_monitor_sock.len) < 0) {
                perror("[ERROR] Could not bind the uhyve monitor socket.");
                exit(EXIT_FAILURE);
        }

        // make it accessible for everyone
        chmod(UHYVE_SOCK_PATH, S_IRWXU | S_IROTH | S_IWOTH);

        // start listening (one connection at a time)
        if (listen(uhyve_monitor_sock.sock, 1) < 0) {
                perror("[ERROR] Could not listen on the uhyve monitor socket.");
                exit(EXIT_FAILURE);
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
	uhyve_monitor_event.evbase = event_base_new();

        // initialize the event socket
        uhyve_monitor_init_ev_sock();

        // set callback function
        if (event_assign(&uhyve_monitor_event.accept_ev,
                         uhyve_monitor_event.evbase,
                         uhyve_monitor_sock.sock,
                         (EV_READ | EV_PERSIST),
                         uhyve_monitor_event_callback,
                         NULL) < 0) {
                err(1, "[ERROR] Could not set the event callback.");
        }

	// add the event to set of pending events
        if (event_add(&uhyve_monitor_event.accept_ev, NULL) < 0) {
                err(1, "[ERROR] Could not add the event to the set of pending events.");
        }

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

	// delete the event
	if (event_del(&uhyve_monitor_event.accept_ev) < 0) {
		err(1, "[ERROR] Could not delete the accept event.");
	}

	// exit the loop
	if (event_base_loopexit(uhyve_monitor_event.evbase, NULL) < 0) {
		err(1, "[ERROR] Could not exit the event loop.");
	}
}
