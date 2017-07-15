/*	$OpenBSD$	*/

/*
 * Copyright (c) 2005 Claudio Jeker <claudio@openbsd.org>
 * Copyright (c) 2004 Esben Norby <norby@openbsd.org>
 * Copyright (c) 2003, 2004 Henning Brauer <henning@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/syslog.h>
#include <sys/wait.h>

#include <errno.h>
#include <event.h>
#include <imsg.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include "log.h"
#include "proc.h"
#include "smtpfd.h"
#include "control.h"

__dead static void	usage(void);
__dead static void	priv_shutdown(void);
static void	priv_sig_handler(int, short, void *);
static void	priv_dispatch_frontend(struct imsgproc *, struct imsg*, void *);
static void	priv_dispatch_engine(struct imsgproc *, struct imsg*, void *);
static int	priv_reload(void);
static int	priv_send_config(struct smtpfd_conf *);
static void     priv_send_filter_proc(struct filter_conf *);
static void     priv_send_filter_conf(struct smtpfd_conf *, struct filter_conf *);
static void	config_print(struct smtpfd_conf *);

static char *conffile;
static char *csock;
static struct smtpfd_conf *env;

/* globals */
uint32_t cmd_opts;
struct imsgproc *p_engine;
struct imsgproc *p_frontend;
struct imsgproc *p_priv;

static void
priv_sig_handler(int sig, short event, void *arg)
{
	pid_t pid;
	int status;

	/*
	 * Normal signal handler rules don't apply because libevent
	 * decouples for us.
	 */

	switch (sig) {
	case SIGCHLD:
		do {
			pid = waitpid(-1, &status, WNOHANG);
			if (pid <= 0)
				continue;
			if (WIFSIGNALED(status))
				log_warnx("process %d terminated by signal %d",
				    (int)pid, WTERMSIG(status));
			else if (WIFEXITED(status) && WEXITSTATUS(status))
				log_warnx("process %d exited with status %d",
				    (int)pid, WEXITSTATUS(status));
			else if (WIFEXITED(status))
				log_debug("debug: process %d exited normally",
				    (int)pid);
			else
				/* WIFSTOPPED or WIFCONTINUED */
				continue;
		} while (pid > 0 || (pid == -1 && errno == EINTR));
		break;
	case SIGTERM:
	case SIGINT:
		priv_shutdown();
	case SIGHUP:
		if (priv_reload() == -1)
			log_warnx("configuration reload failed");
		else
			log_debug("configuration reloaded");
		break;
	default:
		fatalx("unexpected signal");
	}
}

__dead static void
usage(void)
{
	extern char *__progname;

	fprintf(stderr, "usage: %s [-dnv] [-f file] [-s socket]\n",
	    __progname);
	exit(1);
}

int
main(int argc, char *argv[])
{
	struct event	 ev_sigint, ev_sigterm, ev_sighup, ev_sigchld;
	int		 ch, sp[2], rargc = 0;
	int		 debug = 0, engine_flag = 0, frontend_flag = 0;
	char		*saved_argv0;
	char		*rargv[7];

	conffile = CONF_FILE;
	csock = SMTPFD_SOCKET;

	log_init(1, LOG_DAEMON);	/* Log to stderr until daemonized. */
	log_setverbose(1);

	saved_argv0 = argv[0];
	if (saved_argv0 == NULL)
		saved_argv0 = "smtpfd";

	while ((ch = getopt(argc, argv, "D:dEFf:ns:v")) != -1) {
		switch (ch) {
		case 'D':
			if (cmdline_symset(optarg) < 0)
				log_warnx("could not parse macro definition %s",
				    optarg);
			break;
		case 'd':
			debug = 1;
			break;
		case 'E':
			engine_flag = 1;
			break;
		case 'F':
			frontend_flag = 1;
			break;
		case 'f':
			conffile = optarg;
			break;
		case 'n':
			cmd_opts |= OPT_NOACTION;
			break;
		case 's':
			csock = optarg;
			break;
		case 'v':
			if (cmd_opts & OPT_VERBOSE)
				cmd_opts |= OPT_VERBOSE2;
			cmd_opts |= OPT_VERBOSE;
			break;
		default:
			usage();
		}
	}

	argc -= optind;
	argv += optind;
	if (argc > 0 || (engine_flag && frontend_flag))
		usage();

	if (engine_flag)
		engine(debug, cmd_opts & OPT_VERBOSE);
	else if (frontend_flag)
		frontend(debug, cmd_opts & OPT_VERBOSE, csock);

	/* parse config file */
	if ((env = parse_config(conffile)) == NULL) {
		exit(1);
	}

	if (cmd_opts & OPT_NOACTION) {
		if (cmd_opts & OPT_VERBOSE)
			config_print(env);
		else
			fprintf(stderr, "configuration OK\n");
		exit(0);
	}

	/* Check for root privileges. */
	if (geteuid())
		fatalx("need root privileges");

	/* Check for assigned daemon user */
	if (getpwnam(SMTPFD_USER) == NULL)
		fatalx("unknown user %s", SMTPFD_USER);

	log_init(debug, LOG_DAEMON);
	log_setverbose(cmd_opts & OPT_VERBOSE);
	log_procinit("main");
	setproctitle("main");

	if (!debug)
		daemon(1, 0);

	log_info("startup");

	rargc = 0;
	rargv[rargc++] = saved_argv0;
	rargv[rargc++] = "-F";
	if (debug)
		rargv[rargc++] = "-d";
	if (cmd_opts & OPT_VERBOSE)
		rargv[rargc++] = "-v";
	rargv[rargc++] = "-s";
	rargv[rargc++] = csock;
	rargv[rargc++] = NULL;

	p_frontend = proc_exec(PROC_FRONTEND, rargv);
	proc_setcallback(p_frontend, priv_dispatch_frontend, NULL);
	rargv[1] = "-E";
	rargv[argc - 3] = NULL;
	p_engine = proc_exec(PROC_ENGINE, rargv);
	proc_setcallback(p_engine, priv_dispatch_engine, NULL);

	event_init();

	/* Setup signal handler. */
	signal_set(&ev_sigint, SIGINT, priv_sig_handler, NULL);
	signal_set(&ev_sigterm, SIGTERM, priv_sig_handler, NULL);
	signal_set(&ev_sighup, SIGHUP, priv_sig_handler, NULL);
	signal_set(&ev_sigchld, SIGCHLD, priv_sig_handler, NULL);
	signal_add(&ev_sigint, NULL);
	signal_add(&ev_sigterm, NULL);
	signal_add(&ev_sighup, NULL);
	signal_add(&ev_sigchld, NULL);
	signal(SIGPIPE, SIG_IGN);

	/* Start children */
	proc_enable(p_frontend);
	proc_enable(p_engine);

	/* Connect the two children */
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
	    PF_UNSPEC, sp) == -1)
		fatal("socketpair");
	if (proc_compose(p_frontend, IMSG_SOCKET_IPC, 0, 0, sp[0], NULL, 0)
	    == -1)
		fatal("proc_compose");
	if (proc_compose(p_engine, IMSG_SOCKET_IPC, 0, 0, sp[1], NULL, 0)
	    == -1)
		fatal("proc_compose");

	priv_send_config(env);

	if (pledge("rpath stdio sendfd cpath", NULL) == -1)
		fatal("pledge");

	event_dispatch();

	priv_shutdown();
	return (0);
}

__dead static void
priv_shutdown(void)
{
	pid_t	 pid;
	pid_t	 frontend_pid;
	pid_t	 engine_pid;
	int	 status;

	frontend_pid = proc_getpid(p_frontend);
	engine_pid = proc_getpid(p_frontend);

	/* Close pipes. */
	proc_free(p_frontend);
	proc_free(p_engine);

	config_clear(env);

	log_debug("waiting for children to terminate");
	do {
		pid = wait(&status);
		if (pid == -1) {
			if (errno != EINTR && errno != ECHILD)
				fatal("wait");
		} else if (WIFSIGNALED(status))
			log_warnx("%s terminated; signal %d",
			    (pid == engine_pid) ? "engine" :
			    "frontend", WTERMSIG(status));
	} while (pid != -1 || (pid == -1 && errno == EINTR));

	control_cleanup(csock);

	log_info("terminating");
	exit(0);
}

static void
priv_dispatch_frontend(struct imsgproc *p, struct imsg *imsg, void *arg)
{
	int verbose;

	if (imsg == NULL) {
		event_loopexit(NULL);
		return;
	}

	switch (imsg->hdr.type) {
	case IMSG_CTL_RELOAD:
		if (priv_reload() == -1)
			log_warnx("configuration reload failed");
		else
			log_warnx("configuration reloaded");
		break;
	case IMSG_CTL_LOG_VERBOSE:
		/* Already checked by frontend. */
		memcpy(&verbose, imsg->data, sizeof(verbose));
		log_setverbose(verbose);
		break;
	case IMSG_CTL_SHOW_MAIN_INFO:
		proc_compose(p, IMSG_CTL_END, 0, imsg->hdr.pid, -1, NULL, 0);
		break;
	default:
		log_debug("%s: error handling imsg %d", __func__,
		    imsg->hdr.type);
		break;
	}
}

static void
priv_dispatch_engine(struct imsgproc *p, struct imsg *imsg, void *arg)
{
	if (imsg == NULL) {
		event_loopexit(NULL);
		return;
	}

	switch (imsg->hdr.type) {
	default:
		log_debug("%s: error handling imsg %d", __func__,
		    imsg->hdr.type);
		break;
	}
}

static int
priv_reload(void)
{
	struct smtpfd_conf *xconf;

	if ((xconf = parse_config(conffile)) == NULL)
		return (-1);

	if (priv_send_config(xconf) == -1)
		return (-1);

	config_clear(env);
	env = xconf;

	return (0);
}

static int
priv_send_config(struct smtpfd_conf *xconf)
{
	struct filter_conf *f;

	/* Send fixed part of config to engine. */
	if (proc_compose(p_engine, IMSG_RECONF_CONF, 0, 0, -1, NULL, 0) == -1)
		return (-1);

	TAILQ_FOREACH(f, &xconf->filters, entry) {
		if (f->chain)
			continue;
		priv_send_filter_proc(f);
	}

	TAILQ_FOREACH(f, &xconf->filters, entry) {
		proc_compose(p_engine, IMSG_RECONF_FILTER, 0, 0, -1, f->name,
		    strlen(f->name) + 1);
		priv_send_filter_conf(xconf, f);
	}

	/* Tell children the revised config is now complete. */
	if (proc_compose(p_engine, IMSG_RECONF_END, 0, 0, -1, NULL, 0) == -1)
		return (-1);

	return (0);
}

static void
priv_send_filter_proc(struct filter_conf *f)
{
	int sp[2];
	pid_t pid;

	if (socketpair(AF_UNIX, SOCK_STREAM|SOCK_NONBLOCK, PF_UNSPEC, sp) == -1)
		fatal("socketpair");

	switch (pid = fork()) {
	case -1:
		fatal("fork");
	case 0:
		break;
	default:
		close(sp[0]);
		log_debug("forked filter %s as pid %d", f->name, (int)pid);
		proc_compose(p_engine, IMSG_RECONF_FILTER_PROC, 0, pid, sp[1],
		    f->name, strlen(f->name) + 1);
		return;
	}

	if (dup2(sp[0], 3) == -1)
		fatal("dup2");

	if (closefrom(4) == -1)
		fatal("closefrom");

	execvp(f->argv[0], f->argv+1);
	fatal("proc_exec: execvp: %s", f->argv[0]);
}

static void
priv_send_filter_conf(struct smtpfd_conf *conf, struct filter_conf *f)
{
	struct filter_conf *tmp;
	int i;

	if (f->chain) {
		for (i = 0; i < f->argc; i++) {
			TAILQ_FOREACH(tmp, &conf->filters, entry)
				if (!strcmp(f->argv[i], tmp->name)) {
					priv_send_filter_conf(conf, tmp);
					break;
				}
		}
	}
	else {
		proc_compose(p_engine, IMSG_RECONF_FILTER_NODE, 0, 0, -1,
		    f->name, strlen(f->name) + 1);
	}
}

struct smtpfd_conf *
config_new_empty(void)
{
	struct smtpfd_conf	*conf;

	conf = calloc(1, sizeof(*conf));
	if (conf == NULL)
		fatal(NULL);

	TAILQ_INIT(&conf->filters);

	return (conf);
}

void
config_clear(struct smtpfd_conf *conf)
{
	struct filter_conf *f;
	int i;

	while ((f = TAILQ_FIRST(&conf->filters))) {
		TAILQ_REMOVE(&conf->filters, f, entry);
		free(f->name);
		for (i = 0; i < f->argc; i++)
			free(f->argv[i]);
		free(f);
	}

	free(conf);
}

void
config_print(struct smtpfd_conf *conf)
{
	struct filter_conf *f;
	int i;

	TAILQ_FOREACH(f, &conf->filters, entry) {
		printf("%s %s", f->chain ? "chain":"filter", f->name);
		for (i = 0; i < f->argc; i++)
			printf(" %s", f->argv[i]);
		printf("\n");
	}
}
