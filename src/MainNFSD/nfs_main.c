// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright CEA/DAM/DIF  (2008)
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * ---------------------------------------
 */

/**
 * @file nfs_main.c
 * @brief The file that contain the 'main' routine for the nfsd.
 *
 */
#include "config.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <signal.h> /* for sigaction */
#include <errno.h>
#include "fsal.h"
#include "log.h"
#include "gsh_rpc.h"
#include "nfs_init.h"
#include "nfs_exports.h"
#include "pnfs_utils.h"
#include "config_parsing.h"
#include "conf_url.h"
#include "sal_functions.h"

#ifdef USE_MONITORING
#include "monitoring.h"
#endif /* USE_MONITORING */

#ifdef LINUX
#include <sys/prctl.h>
#ifndef PR_SET_IO_FLUSHER
#define PR_SET_IO_FLUSHER 57
#endif
#endif

#include "ff_api.h"

/* parameters for NFSd startup and default values */

static nfs_start_info_t my_nfs_start_info = { .dump_default_config = false,
					      .lw_mark_trigger = false,
					      .drop_caps = true };

config_file_t nfs_config_struct;
char *nfs_host_name = "localhost";
bool config_errors_fatal;

/* command line syntax */

#ifdef USE_LTTNG
#define LTTNG_OPTION "G"
#else
#define LTTNG_OPTION
#endif /* USE_LTTNG */

static const char options[] = "v@L:N:S:f:p:FRTE:ChI:x" LTTNG_OPTION;
static const char usage[] =
	"Usage: %s [-hd][-L <logfile>][-N <dbg_lvl>][-f <config_file>]\n"
	"\t[-v]                display version information\n"
	"\t[-L <logfile>]      set the default logfile for the daemon\n"
	"\t[-N <dbg_lvl>]      set the verbosity level\n"
	"\t[-f <config_file>]  set the config file to be used\n"
	"\t[-p <pid_file>]     set the pid file\n"
	"\t[-F]                the program stays in foreground\n"
	"\t[-R]                daemon will manage RPCSEC_GSS (default is no RPCSEC_GSS)\n"
	"\t[-S <size>]         set the default thread stack size (in K) to be used\n"
	"\t[-T]                dump the default configuration on stdout\n"
	"\t[-E <epoch>]        overrides ServerBootTime for ServerEpoch\n"
	"\t[-I <nodeid>]       cluster nodeid\n"
	"\t[-C]                dump trace when segfault\n"
	"\t[-x]                fatal exit if there are config errors on startup\n"
	"\t[-h]                display this help\n"
#ifdef USE_LTTNG
	"\t[-G]                Load LTTNG traces\n"
#endif /* USE_LTTNG */
	"----------------- Signals ----------------\n"
	"SIGHUP     : Reload LOG and EXPORT config\n"
	"SIGTERM    : Cleanly terminate the program\n"
	"------------- Default Values -------------\n"
	"LogFile    : SYSLOG\n"
	"PidFile    : " GANESHA_PIDFILE_PATH "\n"
	"DebugLevel : NIV_EVENT\n"
	"ConfigFile : " GANESHA_CONFIG_PATH "\n";

static inline char *main_strdup(const char *var, const char *str)
{
	char *s = strdup(str);

	if (s == NULL) {
		fprintf(stderr, "strdup failed for %s value %s\n", var, str);
		abort();
	}

	return s;
}

static int valid_stack_size(unsigned long stack_size)
{
	static unsigned long valid_sizes[] = { 16,  32,	  64,	128,  256,
					       512, 1024, 2048, 4096, 8192 };
	for (unsigned int i = 0;
	     i < sizeof(valid_sizes) / sizeof(valid_sizes[0]); i++)
		if (valid_sizes[i] == stack_size)
			return 1;
	return 0;
}

#ifdef USE_LTTNG

static void load_lttng(void)
{
	void *dl = NULL;
#if defined(LINUX) && !defined(SANITIZE_ADDRESS)
	dl = dlopen("libganesha_trace.so",
		    RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
#elif defined(BSDBASED) || defined(SANITIZE_ADDRESS)
	dl = dlopen("libganesha_trace.so", RTLD_NOW | RTLD_LOCAL);
#endif
	if (dl == NULL) {
		fprintf(stderr, "Failed to load libganesha_trace.so\n");
		exit(1);
	}

#if defined(LINUX) && !defined(SANITIZE_ADDRESS)
	dl = dlopen("libntirpc_tracepoints.so",
		    RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
#elif defined(BSDBASED) || defined(SANITIZE_ADDRESS)
	dl = dlopen("libntirpc_tracepoints.so", RTLD_NOW | RTLD_LOCAL);
#endif
	if (dl == NULL) {
		fprintf(stderr, "Failed to load libntirpc_tracepoints.so\n");
		exit(1);
	}
}

#endif /* USE_LTTNG */

/**
 * main: simply the main function.
 *
 * The 'main' function as in every C program.
 *
 * @param argc number of arguments
 * @param argv array of arguments
 *
 * @return status to calling program by calling the exit(3C) function.
 *
 */

int main(int argc, char *argv[])
{
	/*
	char *ff_argv[4] = {
		"./ganesha.nfsd",
		"--conf=/data/f-stack/config.ini",
		"--proc-type=primary",
		"--proc-id=0"
	};
	ff_init(4, ff_argv);
	*/
	fprintf(stderr, "Officially start nfs...\n");
	char *tempo_exec_name = NULL;
	char localmachine[MAXHOSTNAMELEN + 1];
	int c;
	int dsc;
	int rc;
	int pidfile = -1; /* fd for file to store pid */
	unsigned long stack_size = 8388608; /* 8M, glibc's default */
	char *log_path = NULL;
	char *exec_name = "nfs-ganesha";
	int debug_level = -1;
	int detach_flag = true;
	bool dump_trace = false;
#ifndef HAVE_DAEMON
	int dev_null_fd = 0;
	pid_t son_pid;
#endif
	sigset_t signals_to_block;
	struct config_error_type err_type;

	FILE *logfile = fopen("/tmp/log.lyp", "w");

	/* Set the server's boot time and epoch */
	now(&nfs_ServerBootTime);
	nfs_ServerEpoch = (time_t)nfs_ServerBootTime.tv_sec;
	srand(nfs_ServerEpoch);

	tempo_exec_name = strrchr(argv[0], '/');
	if (tempo_exec_name != NULL)
		exec_name = main_strdup("exec_name", tempo_exec_name + 1);

	if (*exec_name == '\0')
		exec_name = argv[0];

	/* get host name */
	if (gethostname(localmachine, sizeof(localmachine)) != 0) {
		fprintf(stderr, "Could not get local host name, exiting...\n");
		exit(1);
	} else {
		nfs_host_name = main_strdup("host_name", localmachine);
	}

	optind = 1;
	/* now parsing options with getopt */
	while ((c = getopt(argc, argv, options)) != EOF) {
		fprintf(logfile, "c=%c\n", c);
		switch (c) {
		case 'v':
		case '@':
			printf("NFS-Ganesha Release = V%s\n", GANESHA_VERSION);
#if !GANESHA_BUILD_RELEASE
			/* A little backdoor to keep track of binary versions */
			printf("%s compiled on %s at %s\n", exec_name, __DATE__,
			       __TIME__);
			printf("Release comment = %s\n", VERSION_COMMENT);
			printf("Git HEAD = %s\n", _GIT_HEAD_COMMIT);
			printf("Git Describe = %s\n", _GIT_DESCRIBE);
#endif
			exit(0);
			break;

		case 'L':
			/* Default Log */
			log_path = main_strdup("log_path", optarg);
			fprintf(logfile, "log_path=%s\n", log_path);
			fflush(logfile);
			break;
#ifdef USE_LTTNG
		case 'G':
			load_lttng();
			break;
#endif /* USE_LTTNG */

		case 'N':
			/* debug level */
			debug_level = ReturnLevelAscii(optarg);
			if (debug_level == -1) {
				fprintf(stderr,
					"Invalid value for option 'N': NIV_NULL, NIV_MAJ, NIV_CRIT, NIV_EVENT, NIV_DEBUG, NIV_MID_DEBUG or NIV_FULL_DEBUG expected.\n");
				exit(1);
			}
			break;

		case 'S':
			/* default thread stack size */
			stack_size = strtoul(optarg, NULL, 10);
			if (!valid_stack_size(stack_size)) {
				fprintf(stderr,
					"Invalid value for option 'S': valid choices are 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192\n");
				exit(1);
			}
			stack_size *= 1024;
			break;

		case 'f':
			/* config file */

			nfs_config_path = main_strdup("config_path", optarg);
			fprintf(stderr, "config_path=%s\n", nfs_config_path);
			break;

		case 'p':
			/* PID file */
			nfs_pidfile_path = main_strdup("pidfile_path", optarg);
			break;

		case 'F':
			/* Don't detach, foreground mode */
			detach_flag = false;
			break;

		case 'R':
			/* Shall we manage  RPCSEC_GSS ? */
			fprintf(stderr,
				"\n\nThe -R flag is deprecated, use this syntax in the configuration file instead:\n\n");
			fprintf(stderr, "NFS_KRB5\n");
			fprintf(stderr, "{\n");
			fprintf(stderr,
				"\tPrincipalName = nfs@<your_host> ;\n");
			fprintf(stderr, "\tKeytabPath = /etc/krb5.keytab ;\n");
			fprintf(stderr, "\tActive_krb5 = true ;\n");
			fprintf(stderr, "}\n\n\n");
			exit(1);
			break;

		case 'T':
			/* Dump the default configuration on stdout */
			my_nfs_start_info.dump_default_config = true;
			break;

		case 'C':
			dump_trace = true;
			break;

		case 'E':
			nfs_ServerEpoch = (time_t)atoll(optarg);
			break;

		case 'I':
			g_nodeid = atoi(optarg);
			break;

		case 'x':
			config_errors_fatal = true;
			break;

		case 'h':
			fprintf(stderr, usage, exec_name);
			exit(0);

		default: /* '?' */
			fprintf(stderr, "Try '%s -h' for usage\n", exec_name);
			exit(1);
		}
	}

	fprintf(logfile, "Finish parsing arguments\n");
	fflush(logfile);

	/* initialize memory and logging */
	nfs_prereq_init(exec_name, nfs_host_name, debug_level, log_path,
			dump_trace, stack_size);
#if GANESHA_BUILD_RELEASE
	LogEvent(COMPONENT_MAIN, "%s Starting: Ganesha Version %s", exec_name,
		 GANESHA_VERSION);
#else
	LogEvent(COMPONENT_MAIN, "%s Starting: %s", exec_name,
		 "Ganesha Version " _GIT_DESCRIBE ", built at " __DATE__
		 " " __TIME__ " on " BUILD_HOST);
#endif

	/* initialize nfs_init */
	nfs_init_init();
        fprintf(logfile, "Finish nfs init init\n");
	fflush(logfile);
	nfs_check_malloc();

	/* Start in background, if wanted */
	if (detach_flag) {
#ifdef HAVE_DAEMON
		fprintf(logfile, "HAVE_DAEMON\n");
		/* daemonize the process (fork, close xterm fds,
		 * detach from parent process) */
		if (daemon(0, 0))
			LogFatal(COMPONENT_MAIN,
				 "Error detaching process from parent: %s",
				 strerror(errno));

		/* In the child process, change the log header
		 * if not, the header will contain the parent's pid */
		set_const_log_str();
#else
		/* Step 1: forking a service process */
		switch (son_pid = fork()) {
		case -1:
			/* Fork failed */
			LogFatal(
				COMPONENT_MAIN,
				"Could not start nfs daemon (fork error %d (%s)",
				errno, strerror(errno));
			break;

		case 0:
			/* This code is within the son (that will actually work)
			 * Let's make it the leader of its group of process */
			if (setsid() == -1) {
				LogFatal(
					COMPONENT_MAIN,
					"Could not start nfs daemon (setsid error %d (%s)",
					errno, strerror(errno));
			}

			/* stdin, stdout and stderr should not refer to a tty
			 * I close 0, 1 & 2  and redirect them to /dev/null */
			dev_null_fd = open("/dev/null", O_RDWR);
			if (dev_null_fd < 0)
				LogFatal(COMPONENT_MAIN,
					 "Could not open /dev/null: %d (%s)",
					 errno, strerror(errno));

			if (close(STDIN_FILENO) == -1)
				LogEvent(COMPONENT_MAIN,
					 "Error while closing stdin: %d (%s)",
					 errno, strerror(errno));
			else {
				LogEvent(COMPONENT_MAIN, "stdin closed");
				dup(dev_null_fd);
			}

			if (close(STDOUT_FILENO) == -1)
				LogEvent(COMPONENT_MAIN,
					 "Error while closing stdout: %d (%s)",
					 errno, strerror(errno));
			else {
				LogEvent(COMPONENT_MAIN, "stdout closed");
				dup(dev_null_fd);
			}

			if (close(STDERR_FILENO) == -1)
				LogEvent(COMPONENT_MAIN,
					 "Error while closing stderr: %d (%s)",
					 errno, strerror(errno));
			else {
				LogEvent(COMPONENT_MAIN, "stderr closed");
				dup(dev_null_fd);
			}

			if (close(dev_null_fd) == -1)
				LogFatal(
					COMPONENT_MAIN,
					"Could not close tmp fd to /dev/null: %d (%s)",
					errno, strerror(errno));

			/* In the child process, change the log header
			 * if not, the header will contain the parent's pid */
			set_const_log_str();
			break;

		default:
			/* This code is within the parent process,
			 * it is useless, it must die */
			LogFullDebug(COMPONENT_MAIN,
				     "Starting a child of pid %d", son_pid);
			exit(0);
			break;
		}
#endif
	}

	/* Make sure Linux file i/o will return with error
	 * if file size is exceeded. */
#ifdef _LINUX
	signal(SIGXFSZ, SIG_IGN);
#endif

	/* Echo our PID into pidfile: this serves as a lock to prevent */
	/* multiple instances from starting, so any failure creating   */
	/* this file is a fatal error.                                 */
	pidfile = open(nfs_pidfile_path, O_CREAT | O_RDWR, 0644);
	fprintf(logfile, "just opened pidfile\n");
	fprintf(logfile, "pid = %d", getpid());
	fflush(logfile);

	if (pidfile == -1) {
		LogFatal(
			COMPONENT_MAIN,
			"open(%s, O_CREAT | O_RDWR, 0644) failed for pid file, errno was: %s (%d)",
			nfs_pidfile_path, strerror(errno), errno);
		goto fatal_die;
	} else {
		struct flock lk;

		/* Try to obtain a lock on the file: if we cannot lock it, */
		/* Ganesha may already be running.                         */
		lk.l_type = F_WRLCK;
		lk.l_whence = SEEK_SET;
		lk.l_start = (off_t)0;
		lk.l_len = (off_t)0;
		if (fcntl(pidfile, F_SETLK, &lk) == -1) {
			LogFatal(COMPONENT_MAIN,
				 "fcntl(%d) failed, Ganesha already started",
				 pidfile);
			goto fatal_die;
		}

		if (ftruncate(pidfile, 0) == -1) {
			LogFatal(COMPONENT_MAIN,
				 "ftruncate(%d) failed, errno was: %s (%d)",
				 pidfile, strerror(errno), errno);
			goto fatal_die;
		}

		/* Put our pid into the file, then explicitly sync it */
		/* to ensure it winds up on the disk.                 */
		if (dprintf(pidfile, "%u\n", getpid()) < 0 ||
		    fsync(pidfile) < 0) {
			LogFatal(
				COMPONENT_MAIN,
				"dprintf() or fsync() failed trying to write pid to file %s errno was: %s (%d)",
				nfs_pidfile_path, strerror(errno), errno);
			goto fatal_die;
		}
	}

	fprintf(logfile, "finish pidfile\n");
	fflush(logfile);

	/* Set up for the signal handler.
	 * Blocks the signals the signal handler will handle.
	 */
	sigemptyset(&signals_to_block);
	sigaddset(&signals_to_block, SIGTERM);
	sigaddset(&signals_to_block, SIGHUP);
	sigaddset(&signals_to_block, SIGPIPE);
	if (pthread_sigmask(SIG_BLOCK, &signals_to_block, NULL) != 0) {
		LogFatal(COMPONENT_MAIN,
			 "Could not start nfs daemon, pthread_sigmask failed");
		goto fatal_die;
	}

	/* init URL package */
	config_url_init();

	/* Create a memstream for parser+processing error messages */
	if (!init_error_type(&err_type))
		goto fatal_die;

	/* Parse the configuration file so we all know what is going on. */

	if (nfs_config_path == NULL || nfs_config_path[0] == '\0') {
		LogWarn(COMPONENT_INIT, "No configuration file named.");
		nfs_config_struct = NULL;
	} else
		nfs_config_struct =
			config_ParseFile(nfs_config_path, &err_type);

	fprintf(logfile, "Finish parsing nfs config\n");
	fflush(logfile);

	if (!config_error_no_error(&err_type)) {
		char *errstr = err_type_str(&err_type);

		if (!config_error_is_harmless(&err_type)) {
			LogCrit(COMPONENT_INIT, "Error %s while parsing (%s)",
				errstr != NULL ? errstr : "unknown",
				nfs_config_path);
			if (errstr != NULL)
				gsh_free(errstr);
			goto fatal_die;
		} else
			LogWarn(COMPONENT_INIT, "Error %s while parsing (%s)",
				errstr != NULL ? errstr : "unknown",
				nfs_config_path);
		if (errstr != NULL)
			gsh_free(errstr);
	}

	if (read_log_config(nfs_config_struct, &err_type) < 0) {
		LogCrit(COMPONENT_INIT,
			"Error while parsing log configuration");
		goto fatal_die;
	}

	/* We need all the fsal modules loaded so we can have
	 * the list available at exports parsing time.
	 */
	if (start_fsals(nfs_config_struct, &err_type) < 0) {
		LogCrit(COMPONENT_INIT, "Error starting FSALs.");
		goto fatal_die;
	}

	/* parse configuration file */

	if (nfs_set_param_from_conf(nfs_config_struct, &my_nfs_start_info,
				    &err_type)) {
		LogCrit(COMPONENT_INIT,
			"Error setting parameters from configuration file.");
		goto fatal_die;
	}

#ifdef LINUX
	/* Set thread I/O flusher, see
	 * https://git.kernel.org/torvalds/p/8d19f1c8e1937baf74e1962aae9f90fa3aeab463
	 */
	if (prctl(PR_SET_IO_FLUSHER, 1, 0, 0, 0) == -1) {
		if (errno == EPERM) {
			if (nfs_param.core_param.allow_set_io_flusher_fail)
				LogWarn(COMPONENT_MAIN,
					"Failed to set PR_SET_IO_FLUSHER due to EPERM, ignoring...");
			else {
				LogFatal(
					COMPONENT_MAIN,
					"Failed to PR_SET_IO_FLUSHER with EPERM. Take a look at config option allow_set_io_flusher_fail to see if you should allow it");
				goto fatal_die;
			}
		} else if (errno != EINVAL) {
			LogFatal(
				COMPONENT_MAIN,
				"Error setting prctl PR_SET_IO_FLUSHER flag: %s",
				strerror(errno));
			goto fatal_die;
		}
	}

#endif

#ifdef USE_MONITORING
	monitoring__init(nfs_param.core_param.monitoring_port,
			 nfs_param.core_param.enable_dynamic_metrics);
#endif /* USE_MONITORING */

	/* initialize core subsystems and data structures */
	if (init_server_pkgs() != 0) {
		LogCrit(COMPONENT_INIT, "Failed to initialize server packages");
		goto fatal_die;
	}
	/* Load Data Server entries from parsed file
	 * returns the number of DS entries.
	 */
	dsc = ReadDataServers(nfs_config_struct, &err_type);
	if (dsc < 0) {
		LogCrit(COMPONENT_INIT, "Error while parsing DS entries");
		goto fatal_die;
	}

	/* Create stable storage directory, this needs to be done before
	 * starting the recovery thread.
	 */
	rc = nfs4_recovery_init();
	if (rc) {
		LogCrit(COMPONENT_INIT,
			"Recovery backend initialization failed!");
		goto fatal_die;
	}

	/* Start grace period */
	nfs_start_grace(NULL);

	/* Wait for enforcement to begin */
	nfs_wait_for_grace_enforcement();

	/* Load export entries from parsed file
	 * returns the number of export entries.
	 */
	rc = ReadExports(nfs_config_struct, &err_type);
	if (rc < 0) {
		LogCrit(COMPONENT_INIT, "Error while parsing export entries");
		goto fatal_die;
	}
	if (rc == 0 && dsc == 0)
		LogWarn(COMPONENT_INIT,
			"No export entries found in configuration file !!!");

	find_unused_blocks(nfs_config_struct, &err_type);

	rc = report_config_errors(&err_type, NULL, config_errs_to_log);

	if (config_errors_fatal && rc > 0)
		goto fatal_die;

	/* freeing syntax tree : */

	config_Free(nfs_config_struct);

	fprintf(logfile, "Just about to nfs_start\n");
	fflush(logfile);
	/* Everything seems to be OK! We can now start service threads */
	nfs_start(&my_nfs_start_info);

	if (tempo_exec_name)
		free(exec_name);
	if (log_path)
		free(log_path);

	if (pidfile != -1)
		close(pidfile);

	nfs_prereq_destroy();

	return 0;

fatal_die:
	(void)report_config_errors(&err_type, NULL, config_errs_to_log);

	if (tempo_exec_name)
		free(exec_name);
	if (log_path)
		free(log_path);
	if (pidfile != -1)
		close(pidfile);

	/* systemd journal won't display our errors without this */
	sleep(1);

	LogFatal(COMPONENT_INIT, "Fatal errors.  Server exiting...");
	/* NOT REACHED */
	return 2;
}
