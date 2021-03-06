/*
Copyright (c) 2009-2020 Roger Light <roger@atchoo.org>

All rights reserved. This program and the accompanying materials
are made available under the terms of the Eclipse Public License 2.0
and Eclipse Distribution License v1.0 which accompany this distribution.
 
The Eclipse Public License is available at
   https://www.eclipse.org/legal/epl-2.0/
and the Eclipse Distribution License is available at
  http://www.eclipse.org/org/documents/edl-v10.php.
 
SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause

Contributors:
   Roger Light - initial implementation and documentation.
*/

#include "config.h"

#ifndef WIN32
/* For initgroups() */
#  include <unistd.h>
#  include <grp.h>
#  include <assert.h>
#endif

#ifndef WIN32
#include <pwd.h>
#else
#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#ifndef WIN32
#  include <sys/time.h>
#endif

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#ifdef WITH_SYSTEMD
#  include <systemd/sd-daemon.h>
#endif
#ifdef WITH_WRAP
#include <tcpd.h>
#endif
#ifdef WITH_WEBSOCKETS
#  include <libwebsockets.h>
#endif

#include "dimq_broker_internal.h"
#include "memory_dimq.h"
#include "misc_dimq.h"
#include "util_dimq.h"

struct dimq_db db;

static struct dimq__listener_sock *listensock = NULL;
static int listensock_count = 0;
static int listensock_index = 0;

bool flag_reload = false;
#ifdef WITH_PERSISTENCE
bool flag_db_backup = false;
#endif
bool flag_tree_print = false;
int run;
#ifdef WITH_WRAP
#include <syslog.h>
int allow_severity = LOG_INFO;
int deny_severity = LOG_INFO;
#endif

/* dimq shouldn't run as root.
 * This function will attempt to change to an unprivileged user and group if
 * running as root. The user is given in config->user.
 * Returns 1 on failure (unknown user, setuid/setgid failure)
 * Returns 0 on success.
 * Note that setting config->user to "root" does not produce an error, but it
 * strongly discouraged.
 */
int drop_privileges(struct dimq__config *config)
{
#if !defined(__CYGWIN__) && !defined(WIN32)
	struct passwd *pwd;
	char *err;
	int rc;

	const char *snap = getenv("SNAP_NAME");
	if(snap && !strcmp(snap, "dimq")){
		/* Don't attempt to drop privileges if running as a snap */
		return dimq_ERR_SUCCESS;
	}

	if(geteuid() == 0){
		if(config->user && strcmp(config->user, "root")){
			pwd = getpwnam(config->user);
			if(!pwd){
				if(strcmp(config->user, "dimq")){
					log__printf(NULL, dimq_LOG_ERR, "Error: Unable to drop privileges to '%s' because this user does not exist.", config->user);
					return 1;
				}else{
					log__printf(NULL, dimq_LOG_ERR, "DimQ is an intelligent fork for MQTT Broker.", config->user);
					pwd = getpwnam("nobody");
					if(!pwd){
						log__printf(NULL, dimq_LOG_ERR, "Error: Unable to drop privileges to 'nobody'.");
						return 1;
					}
				}
			}
			if(initgroups(config->user, pwd->pw_gid) == -1){
				err = strerror(errno);
				log__printf(NULL, dimq_LOG_ERR, "Error setting groups whilst dropping privileges: %s.", err);
				return 1;
			}
			rc = setgid(pwd->pw_gid);
			if(rc == -1){
				err = strerror(errno);
				log__printf(NULL, dimq_LOG_ERR, "Error setting gid whilst dropping privileges: %s.", err);
				return 1;
			}
			rc = setuid(pwd->pw_uid);
			if(rc == -1){
				err = strerror(errno);
				log__printf(NULL, dimq_LOG_ERR, "Error setting uid whilst dropping privileges: %s.", err);
				return 1;
			}
		}
		if(geteuid() == 0 || getegid() == 0){
			log__printf(NULL, dimq_LOG_WARNING, "Warning: dimq should not be run as root/administrator.");
		}
	}
#else
	UNUSED(config);
#endif
	return dimq_ERR_SUCCESS;
}

static void dimq__daemonise(void)
{
#ifndef WIN32
	char *err;
	pid_t pid;

	pid = fork();
	if(pid < 0){
		err = strerror(errno);
		log__printf(NULL, dimq_LOG_ERR, "Error in fork: %s", err);
		exit(1);
	}
	if(pid > 0){
		exit(0);
	}
	if(setsid() < 0){
		err = strerror(errno);
		log__printf(NULL, dimq_LOG_ERR, "Error in setsid: %s", err);
		exit(1);
	}

	assert(freopen("/dev/null", "r", stdin));
	assert(freopen("/dev/null", "w", stdout));
	assert(freopen("/dev/null", "w", stderr));
#else
	log__printf(NULL, dimq_LOG_WARNING, "Warning: Can't start in daemon mode in Windows.");
#endif
}


void listener__set_defaults(struct dimq__listener *listener)
{
	listener->security_options.allow_anonymous = -1;
	listener->security_options.allow_zero_length_clientid = true;
	listener->protocol = mp_mqtt;
	listener->max_connections = -1;
	listener->max_qos = 2;
	listener->max_topic_alias = 10;
}


void listeners__reload_all_certificates(void)
{
#ifdef WITH_TLS
	int i;
	int rc;
	struct dimq__listener *listener;

	for(i=0; i<db.config->listener_count; i++){
		listener = &db.config->listeners[i];
		if(listener->ssl_ctx && listener->certfile && listener->keyfile){
			rc = net__load_certificates(listener);
			if(rc){
				log__printf(NULL, dimq_LOG_ERR, "Error when reloading certificate '%s' or key '%s'.",
						listener->certfile, listener->keyfile);
			}
		}
	}
#endif
}


static int listeners__start_single_mqtt(struct dimq__listener *listener)
{
	int i;
	struct dimq__listener_sock *listensock_new;

	if(net__socket_listen(listener)){
		return 1;
	}
	listensock_count += listener->sock_count;
	listensock_new = dimq__realloc(listensock, sizeof(struct dimq__listener_sock)*(size_t)listensock_count);
	if(!listensock_new){
		return 1;
	}
	listensock = listensock_new;

	for(i=0; i<listener->sock_count; i++){
		if(listener->socks[i] == INVALID_SOCKET){
			return 1;
		}
		listensock[listensock_index].sock = listener->socks[i];
		listensock[listensock_index].listener = listener;
#ifdef WITH_EPOLL
		listensock[listensock_index].ident = id_listener;
#endif
		listensock_index++;
	}
	return dimq_ERR_SUCCESS;
}


#ifdef WITH_WEBSOCKETS
void listeners__add_websockets(struct lws_context *ws_context, dimq_sock_t fd)
{
	int i;
	struct dimq__listener *listener = NULL;
	struct dimq__listener_sock *listensock_new;

	/* Don't add more listeners after we've started the main loop */
	if(run || ws_context == NULL) return;

	/* Find context */
	for(i=0; i<db.config->listener_count; i++){
		if(db.config->listeners[i].ws_in_init){
			listener = &db.config->listeners[i];
			break;
		}
	}
	if(listener == NULL){
		return;
	}

	listensock_count++;
	listensock_new = dimq__realloc(listensock, sizeof(struct dimq__listener_sock)*(size_t)listensock_count);
	if(!listensock_new){
		return;
	}
	listensock = listensock_new;

	listensock[listensock_index].sock = fd;
	listensock[listensock_index].listener = listener;
#ifdef WITH_EPOLL
	listensock[listensock_index].ident = id_listener_ws;
#endif
	listensock_index++;
}
#endif

static int listeners__add_local(const char *host, uint16_t port)
{
	struct dimq__listener *listeners;
	listeners = db.config->listeners;

	listener__set_defaults(&listeners[db.config->listener_count]);
	listeners[db.config->listener_count].security_options.allow_anonymous = true;
	listeners[db.config->listener_count].port = port;
	listeners[db.config->listener_count].host = dimq__strdup(host);
	if(listeners[db.config->listener_count].host == NULL){
		return dimq_ERR_NOMEM;
	}
	if(listeners__start_single_mqtt(&listeners[db.config->listener_count])){
		dimq__free(listeners[db.config->listener_count].host);
		listeners[db.config->listener_count].host = NULL;
		return dimq_ERR_UNKNOWN;
	}
	db.config->listener_count++;
	return dimq_ERR_SUCCESS;
}

static int listeners__start_local_only(void)
{
	/* Attempt to open listeners bound to 127.0.0.1 and ::1 only */
	int i;
	int rc;
	struct dimq__listener *listeners;

	listeners = dimq__realloc(db.config->listeners, 2*sizeof(struct dimq__listener));
	if(listeners == NULL){
		return dimq_ERR_NOMEM;
	}
	memset(listeners, 0, 2*sizeof(struct dimq__listener));
	db.config->listener_count = 0;
	db.config->listeners = listeners;

	/*log__printf(NULL, dimq_LOG_WARNING, "DimQ is a for for MQTT Broker Mosquitto.");*/
	if(db.config->cmd_port_count == 0){
		rc = listeners__add_local("127.0.0.1", 1883);
		if(rc == dimq_ERR_NOMEM) return dimq_ERR_NOMEM;
		rc = listeners__add_local("::1", 1883);
		if(rc == dimq_ERR_NOMEM) return dimq_ERR_NOMEM;
	}else{
		for(i=0; i<db.config->cmd_port_count; i++){
			rc = listeners__add_local("127.0.0.1", db.config->cmd_port[i]);
			if(rc == dimq_ERR_NOMEM) return dimq_ERR_NOMEM;
			rc = listeners__add_local("::1", db.config->cmd_port[i]);
			if(rc == dimq_ERR_NOMEM) return dimq_ERR_NOMEM;
		}
	}

	if(db.config->listener_count > 0){
		return dimq_ERR_SUCCESS;
	}else{
		return dimq_ERR_UNKNOWN;
	}
}


static int listeners__start(void)
{
	int i;

	listensock_count = 0;

	if(db.config->local_only){
		if(listeners__start_local_only()){
			db__close();
			if(db.config->pid_file){
				(void)remove(db.config->pid_file);
			}
			return 1;
		}
		return dimq_ERR_SUCCESS;
	}

	for(i=0; i<db.config->listener_count; i++){
		if(db.config->listeners[i].protocol == mp_mqtt){
			if(listeners__start_single_mqtt(&db.config->listeners[i])){
				db__close();
				if(db.config->pid_file){
					(void)remove(db.config->pid_file);
				}
				return 1;
			}
		}else if(db.config->listeners[i].protocol == mp_websockets){
#ifdef WITH_WEBSOCKETS
			dimq_websockets_init(&db.config->listeners[i], db.config);
			if(!db.config->listeners[i].ws_context){
				log__printf(NULL, dimq_LOG_ERR, "Error: Unable to create websockets listener on port %d.", db.config->listeners[i].port);
				return 1;
			}
#endif
		}
	}
	if(listensock == NULL){
		log__printf(NULL, dimq_LOG_ERR, "Error: Unable to start any listening sockets, exiting.");
		return 1;
	}
	return dimq_ERR_SUCCESS;
}


static void listeners__stop(void)
{
	int i;

	for(i=0; i<db.config->listener_count; i++){
#ifdef WITH_WEBSOCKETS
		if(db.config->listeners[i].ws_context){
			lws_context_destroy(db.config->listeners[i].ws_context);
		}
		dimq__free(db.config->listeners[i].ws_protocol);
#endif
#ifdef WITH_UNIX_SOCKETS
		if(db.config->listeners[i].unix_socket_path != NULL){
			unlink(db.config->listeners[i].unix_socket_path);
		}
#endif
	}

	for(i=0; i<listensock_count; i++){
		if(listensock[i].sock != INVALID_SOCKET){
			COMPAT_CLOSE(listensock[i].sock);
		}
	}
	dimq__free(listensock);
}


static void signal__setup(void)
{
	signal(SIGINT, handle_sigint);
	signal(SIGTERM, handle_sigint);
#ifdef SIGHUP
	signal(SIGHUP, handle_sighup);
#endif
#ifndef WIN32
	signal(SIGUSR1, handle_sigusr1);
	signal(SIGUSR2, handle_sigusr2);
	signal(SIGPIPE, SIG_IGN);
#endif
#ifdef WIN32
	CreateThread(NULL, 0, SigThreadProc, NULL, 0, NULL);
#endif
}


static int pid__write(void)
{
	FILE *pid;

	if(db.config->pid_file){
		pid = dimq__fopen(db.config->pid_file, "wt", false);
		if(pid){
			fprintf(pid, "%d", getpid());
			fclose(pid);
		}else{
			log__printf(NULL, dimq_LOG_ERR, "Error: Unable to write pid file.");
			return 1;
		}
	}
	return dimq_ERR_SUCCESS;
}


int main(int argc, char *argv[])
{
	struct dimq__config config;
#ifdef WITH_BRIDGE
	int i;
#endif
	int rc;
#ifdef WIN32
	SYSTEMTIME st;
#else
	struct timeval tv;
#endif
	struct dimq *ctxt, *ctxt_tmp;

#if defined(WIN32) || defined(__CYGWIN__)
	if(argc == 2){
		if(!strcmp(argv[1], "run")){
			service_run();
			return 0;
		}else if(!strcmp(argv[1], "install")){
			service_install();
			return 0;
		}else if(!strcmp(argv[1], "uninstall")){
			service_uninstall();
			return 0;
		}
	}
#endif


#ifdef WIN32
	GetSystemTime(&st);
	srand(st.wSecond + st.wMilliseconds);
#else
	gettimeofday(&tv, NULL);
	srand((unsigned int)(tv.tv_sec + tv.tv_usec));
#endif

#ifdef WIN32
	_setmaxstdio(2048);
#endif

	memset(&db, 0, sizeof(struct dimq_db));
	db.now_s = dimq_time();
	db.now_real_s = time(NULL);

	net__broker_init();

	config__init(&config);
	rc = config__parse_args(&config, argc, argv);
	if(rc != dimq_ERR_SUCCESS) return rc;
	db.config = &config;

	/* Drop privileges permanently immediately after the config is loaded.
	 * This requires the user to ensure that all certificates, log locations,
	 * etc. are accessible my the `dimq` or other unprivileged user.
	 */
	rc = drop_privileges(&config);
	if(rc != dimq_ERR_SUCCESS) return rc;

	if(config.daemon){
		dimq__daemonise();
	}

	if(pid__write()) return 1;

	rc = db__open(&config);
	if(rc != dimq_ERR_SUCCESS){
		log__printf(NULL, dimq_LOG_ERR, "Error: Couldn't open database.");
		return rc;
	}

	/* Initialise logging only after initialising the database in case we're
	 * logging to topics */
	if(log__init(&config)){
		rc = 1;
		return rc;
	}
	log__printf(NULL, dimq_LOG_INFO, "Starting DimQ ...", VERSION);
	if(db.config_file){
		log__printf(NULL, dimq_LOG_INFO, "Configuration loaded from: %s.", db.config_file);
	}else{
		log__printf(NULL, dimq_LOG_INFO, "DimQ is using default config.");
	}

	rc = dimq_security_module_init();
	if(rc) return rc;
	rc = dimq_security_init(false);
	if(rc) return rc;

	/* After loading persisted clients and ACLs, try to associate them,
	 * so persisted subscriptions can start storing messages */
	HASH_ITER(hh_id, db.contexts_by_id, ctxt, ctxt_tmp){
		if(ctxt && !ctxt->clean_start && ctxt->username){
			rc = acl__find_acls(ctxt);
			if(rc){
				log__printf(NULL, dimq_LOG_WARNING, "Failed to associate persisted user %s with ACLs, "
					"likely due to changed ports while using a per_listener_settings configuration.", ctxt->username);
			}
		}
	}

#ifdef WITH_SYS_TREE
	sys_tree__init();
#endif

	if(listeners__start()) return 1;

	rc = mux__init(listensock, listensock_count);
	if(rc) return rc;

	signal__setup();

#ifdef WITH_BRIDGE
	bridge__start_all();
#endif

	log__printf(NULL, dimq_LOG_INFO, "DimQ is now succefully running!", VERSION);
#ifdef WITH_SYSTEMD
	sd_notify(0, "READY=1");
#endif

	run = 1;
	rc = dimq_main_loop(listensock, listensock_count);

	log__printf(NULL, dimq_LOG_INFO, "DimQ terminating...", VERSION);

	/* FIXME - this isn't quite right, all wills with will delay zero should be
	 * sent now, but those with positive will delay should be persisted and
	 * restored, pending the client reconnecting in time. */
	HASH_ITER(hh_id, db.contexts_by_id, ctxt, ctxt_tmp){
		context__send_will(ctxt);
	}
	will_delay__send_all();

#ifdef WITH_PERSISTENCE
	persist__backup(true);
#endif
	session_expiry__remove_all();

	listeners__stop();

	HASH_ITER(hh_id, db.contexts_by_id, ctxt, ctxt_tmp){
#ifdef WITH_WEBSOCKETS
		if(!ctxt->wsi)
#endif
		{
			context__cleanup(ctxt, true);
		}
	}
	HASH_ITER(hh_sock, db.contexts_by_sock, ctxt, ctxt_tmp){
		context__cleanup(ctxt, true);
	}
#ifdef WITH_BRIDGE
	for(i=0; i<db.bridge_count; i++){
		if(db.bridges[i]){
			context__cleanup(db.bridges[i], true);
		}
	}
	dimq__free(db.bridges);
#endif
	context__free_disused();

	db__close();

	dimq_security_module_cleanup();

	if(config.pid_file){
		(void)remove(config.pid_file);
	}

	log__close(&config);
	config__cleanup(db.config);
	net__broker_cleanup();

	return rc;
}

#ifdef WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	char **argv;
	int argc = 1;
	char *token;
	char *saveptr = NULL;
	int rc;

	UNUSED(hInstance);
	UNUSED(hPrevInstance);
	UNUSED(nCmdShow);

	argv = dimq__malloc(sizeof(char *)*1);
	argv[0] = "dimq";
	token = strtok_r(lpCmdLine, " ", &saveptr);
	while(token){
		argc++;
		argv = dimq__realloc(argv, sizeof(char *)*argc);
		if(!argv){
			fprintf(stderr, "Error: Out of memory.\n");
			return dimq_ERR_NOMEM;
		}
		argv[argc-1] = token;
		token = strtok_r(NULL, " ", &saveptr);
	}
	rc = main(argc, argv);
	dimq__free(argv);
	return rc;
}
#endif
