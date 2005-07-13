/*
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 ******************************************************************************
 * TODO: 
 *	1) Man page
 *	2) Add the "cl_respawn recover" function, for combining with recovery
 *	   manager. But what's its strategy ?
 * 	   The pid will passed by environment
 *	3) Add the function for "-l" option ?
 ******************************************************************************
 *
 * File: cl_respawn.c
 * Description: 
 * 	A small respawn tool which will start a program as a child process, and
 * unless it exits with the "magic" exit code, will restart the program again 
 * if it exits(dies).  It is intended that this respawn program should be usable
 * in resource agent scripts and other places.  The respawn tool should properly
 * log all restarts, and all exits which it doesn't respawn, and run itself as a
 * client of the apphb application heartbeating program, so that it can be 
 * restarted if the program it is monitoring dies.  
 * 
 * Author: Sun Jiang Dong <sunjd@cn.ibm.com>
 * Copyright (c) 2004 International Business Machines
 */ 

#include <portability.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <signal.h>
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <glib.h>
#include <clplumbing/cl_log.h>
#include <clplumbing/cl_signal.h>
#include <clplumbing/uids.h>
#include <clplumbing/lsb_exitcodes.h>
#include <clplumbing/GSource.h>
#include <clplumbing/proctrack.h>
#include <clplumbing/Gmain_timeout.h>
#include <apphb.h>

static const char * Simple_helpscreen =
"Usage cl_respawn [<options>] \"<monitored_program> [<arg1>] [<arg2>] ...\"\n"
"Options are as below:\n"
"-m magic_exit_code\n"
"	When monitored_program exit as this magic_exit_code, then cl_respawn\n"
"	will not try to respawn it.\n"
"-i interval\n"
"	Set the interval(ms) of application hearbeat.\n" 
"-w warntime\n"
"	Set the warning time (ms) of application heartbeat.\n"
"-r	Recover itself from crash. Only called by other monitor programs like"
"	recovery manager.\n"
"-l	List the program monitored by cl_respawn.\n"
"	Notice: donnot support yet.\n"
"-h	Display this simple help.\n";


static void become_daemon(void);
static int  run_client_as_child(void);
static gboolean emit_apphb(gpointer data);
static void cl_respawn_quit(int signo);
static int  deal_cmd_str(char * cmd_str, char * execv_argv[]);

/* Functions for handling the child quit/abort event
 */
static void monitoredProcessDied(ProcTrack* p, int status, int signo
            ,	int exitcode, int waslogged);
static void monitoredProcessRegistered(ProcTrack* p);
static const char * monitoredProcessName(ProcTrack* p);

static ProcTrack_ops MonitoredProcessTrackOps = {
        monitoredProcessDied,
        monitoredProcessRegistered,
        monitoredProcessName
};

static const int
	INSTANCE_NAME_LEN = 20,
	APPHB_INTVL_DETLA = 30;    /* Avoid the incorrect warning message */ 
	
static const unsigned long 
	DEFAULT_APPHB_INTERVAL 	= 2000, /* MS */
	DEFAULT_APPHB_WARNTIME  = 6000; /* MS */

static int MAGIC_EXIT_CODE = 100;

static const char * app_name = "cl_respawn";
static gboolean	REGTO_APPHBD = FALSE;

/* For the monitorred program */
static char * cmd_line = NULL;
#define	MAX_NUM_OF_PARAMETER 40
static char * execv_argv[MAX_NUM_OF_PARAMETER];

/* 
 * This pid will equal to the PID of the process who was ever the child of 
 * that dead cl_respawn.
 */
static pid_t monitored_PID = 0;

static const char * optstr = "rm:i:w:lh";
static GMainLoop * mainloop = NULL;
static gboolean IS_RECOVERY = FALSE;

int main(int argc, char * argv[])
{
	char app_instance[INSTANCE_NAME_LEN];
	int option_char;
	int apphb_interval = DEFAULT_APPHB_INTERVAL;
	int apphb_warntime = DEFAULT_APPHB_WARNTIME;
	int ret_value_tmp;
	pid_t child_tmp = 0;

	cl_log_set_entity(app_name);
	cl_log_enable_stderr(TRUE);
	cl_log_set_facility(LOG_DAEMON);

	if (argc == 1) { /* no arguments */
		printf("%s\n",Simple_helpscreen);
	}
	
	/* code for debug */
	/*
	int j;
	cl_log(LOG_INFO, "Called arg");
	for (j=0; j< argc; j++) {
		cl_log(LOG_INFO, "argv[%d]: %s", j, argv[j]);
	}
	*/

	do {
		option_char = getopt(argc, argv, optstr);

		if (option_char == -1) {
			break;
		}

		switch (option_char) {
			case 'r':
				IS_RECOVERY = TRUE;
				break;

			case 'm':
				if (optarg) {
					MAGIC_EXIT_CODE = atoi(optarg); 
				}
				break;

			case 'i':
				if (optarg) {
					apphb_interval = atoi(optarg);
				}
				break;

			case 'w':
				if (optarg) {
					apphb_warntime = atoi(optarg);
				}
				break;

			case 'l':
				break;
				/* information */
				return LSB_EXIT_OK;

			case 'h':
				printf("%s\n",Simple_helpscreen);
				return LSB_EXIT_OK;

			default:
				cl_log(LOG_ERR, "Error:getopt returned" 
					"character code %c.", option_char);
				printf("%s\n",Simple_helpscreen);
				return LSB_EXIT_EINVAL;
		}
	} while (1);


	if ( (IS_RECOVERY == FALSE) && (argv[optind] == NULL) ) {
		cl_log(LOG_ERR, "Please give the program name which will be " 
			"runned as a child process of cl_respawn.");
		return LSB_EXIT_EINVAL;
	}

	if ((IS_RECOVERY == TRUE ) && (argv[optind] == NULL)) {
		/* 
		 * From the environment variables to acquire the necessary
		 * information set by other daemons like recovery manager.
		 * RSP_PID:  the PID of the process which need to be monitored.
		 * RSP_CMD:  the command line to restart the program, which is
		 * the same as the input in command line as above. 
		 */
		if ( getenv("RSP_PID") == NULL ) {
			cl_log(LOG_ERR, "cannot get monitoring PID from the" 
				"environment variable in recovery process.");
			return LSB_EXIT_EINVAL;
		}
		monitored_PID = atoi(getenv("RSP_PID"));
		/* 
		 * "argv[optind] == NULL" indicate no client program passed as 
		 * a parameter by others such as recovery manager, so expect it
		 * will be passed by environment variable RSP_CMD, see as below
		 * If cannot get it, quit
		 */
		if (argv[optind] == NULL) {
			cmd_line = getenv("RSP_CMD");
		}
			
	} else {
		cmd_line = argv[optind];
	}
	
	ret_value_tmp = deal_cmd_str(cmd_line, execv_argv);
	if ( ret_value_tmp != 0 ) {
		cl_log(LOG_ERR, "No a valid command line parameter.");
		return ret_value_tmp;
	}

	/* Not use daemon call since it's not a POSIX's */ 
	become_daemon();

	/* Code for debug
	int k = 0;
	do {
		cl_log(LOG_INFO,"%s", execv_argv[k]);
	} while (execv_argv[++k] != NULL); 
	*/

	set_sigchld_proctrack(G_PRIORITY_HIGH);

	if ( IS_RECOVERY == FALSE ) {
		child_tmp = run_client_as_child();
		if (child_tmp > 0 ) {
			cl_log(LOG_NOTICE, "started the monitored program %s, whose"
			 " PID is %d", execv_argv[0], child_tmp); 
		} else {
			exit(LSB_EXIT_GENERIC);
		}
	}

	snprintf(app_instance, INSTANCE_NAME_LEN, "%s_%ldd"
		, app_name, (long)getpid());

	if (apphb_register(app_name, app_instance) != 0) {
		cl_log(LOG_WARNING, "Failed to register to apphbd.");
		cl_log(LOG_WARNING, "Maybe apphd isnot running.");
		REGTO_APPHBD = FALSE;
	} else {
		REGTO_APPHBD = TRUE;
		cl_log(LOG_INFO, "Registered to apphbd.");
		apphb_setinterval(apphb_interval);
		apphb_setwarn(apphb_warntime);
		Gmain_timeout_add(apphb_interval - APPHB_INTVL_DETLA
			,	emit_apphb, NULL);
		/* To avoid the warning when app_interval is very small. */
		apphb_hb();
	}

	mainloop = g_main_new(FALSE);
	g_main_run(mainloop);

	if ( REGTO_APPHBD == TRUE ) {
		apphb_hb();
		apphb_unregister();
	}
	
	return LSB_EXIT_OK;
}

static int
run_client_as_child(void)
{
	long	pid;
	int	i;

	if (execv_argv[0] == NULL) {
		cl_log(LOG_ERR, "Null pointer to program name which need to" 
			"be executed.");
		return LSB_EXIT_EINVAL;
	}

	pid = fork();

	if (pid < 0) {
		cl_log(LOG_ERR, "cannot start monitor program %s.", 
			execv_argv[0]);
		return -1;
	} else if (pid > 0) { /* in the parent process */
		NewTrackedProc( pid, 1, PT_LOGVERBOSE
			, g_strdup(execv_argv[0]), &MonitoredProcessTrackOps);
		return pid;
	}
	
 	/* Now in child process */
	execvp(execv_argv[0], execv_argv);
	/* if go here, there must be something wrong */
	cl_log(LOG_ERR, "%s",strerror(errno));
	cl_log(LOG_ERR, "execving monitored program %s failed.", execv_argv[0]);

	i = 0;
	do {
		free(execv_argv[i]);
	} while (execv_argv[++i] != NULL); 

	/* Since parameter error, donnot need to be respawned */
	exit(MAGIC_EXIT_CODE);
}

/* 
 * Notes: Since the work dir is changed to "/", the client name should include
 * pathname or it's located in the system PATH
*/
static void
become_daemon(void)
{
	pid_t pid;
	int j;

	pid = fork();

	if (pid < 0) {
		cl_log(LOG_ERR, "cannot start daemon.");
		exit(LSB_EXIT_GENERIC);
	} else if (pid > 0) {
		exit(LSB_EXIT_OK);
	}

	if (chdir("/") < 0) {
		cl_log(LOG_ERR, "cannot chroot to /.");
		exit(LSB_EXIT_GENERIC);
	}
	
	umask(022);
	setsid();

	for (j=0; j < 3; ++j) {
		close(j);
		(void)open("/dev/null", j == 0 ? O_RDONLY : O_RDONLY);
	}

	CL_IGNORE_SIG(SIGINT);
	CL_IGNORE_SIG(SIGHUP);
	CL_SIGNAL(SIGTERM, cl_respawn_quit);
}

static gboolean
emit_apphb(gpointer data)
{
	pid_t new_pid;

	if ( REGTO_APPHBD == TRUE ) {
		apphb_hb();
	}
	/* cl_log(LOG_NOTICE,"donnot emit hb for test."); */
	if ( IS_RECOVERY == TRUE  && !(CL_PID_EXISTS(monitored_PID)) ) {
		cl_log(LOG_INFO, "process %d exited.", monitored_PID);

		new_pid = run_client_as_child();
		if (new_pid > 0 ) {
			cl_log(LOG_NOTICE, "restart the monitored program %s,"
				" whose PID is %d", execv_argv[0], new_pid); 
		} else { 
			/* 
			 * donnot let recovery manager restart me again, avoid
			 * infinite loop 
			*/
			cl_log(LOG_ERR, "failed to restart the monitored "
				"program %s, will exit.", execv_argv[0] );
			cl_respawn_quit(3);		
		}
	}

	return TRUE;
}

static void
cl_respawn_quit(int signo)
{
	if (mainloop != NULL && g_main_is_running(mainloop)) {
		DisableProcLogging();
		g_main_quit(mainloop);
	} else {
		apphb_unregister();
		DisableProcLogging();
		exit(LSB_EXIT_OK);
	}
}

static int 
deal_cmd_str(char * cmd_str, char * execv_argv[])
{
	char *pre, *next;
	int index = 0;
	int i, len_tmp;
	
	if (cmd_str == NULL) {
		return LSB_EXIT_EINVAL;
	}

	pre = cmd_str;
	do {
		next = strchr(pre,' ');

		if (next == NULL) {
			len_tmp = strnlen(pre, 80);	
			execv_argv[index] = calloc(len_tmp+1, sizeof(char));
			if ((execv_argv[index]) == NULL ) {
				cl_perror("deal_cmd_str: calloc: ");
				return LSB_EXIT_GENERIC;
			}
			strncpy(execv_argv[index], pre, len_tmp);
			break;
		}

		execv_argv[index] = calloc(next-pre+1, sizeof(char));
		if ((execv_argv[index]) == NULL ) {
			cl_perror("deal_cmd_str: calloc: ");
			return LSB_EXIT_GENERIC;
		}
		strncpy(execv_argv[index], pre, next-pre);

		/* remove redundant spaces between parametes */
		while ((char)(*next)==' ') {
			next++;
		}

		pre = next;
		if (++index >= MAX_NUM_OF_PARAMETER - 1) {
			break; 
		}
	} while (1==1);
	
	if (index >= MAX_NUM_OF_PARAMETER - 1) {
		for (i=0; i<MAX_NUM_OF_PARAMETER; i++) {
			free(execv_argv[i]);
		} 
		return LSB_EXIT_EINVAL; 
	}

	execv_argv[index+1] = NULL;

	return 0;
}

static void
monitoredProcessDied(ProcTrack* p, int status, int signo
			, int exitcode, int waslogged)
{
	pid_t new_pid;
	const char * pname = p->ops->proctype(p);

	if ( exitcode == MAGIC_EXIT_CODE) {
		cl_log(LOG_INFO, "Not restartng monitored program"
			" %s [%d], since got a magic exit code."
			, execv_argv[0], p->pid);
        	g_free(p->privatedata);	
		cl_respawn_quit(3);		
	}

	cl_log(LOG_INFO, "process %s[%d] exited, and its exit code is %d"
		, pname, p->pid, exitcode);
	if ( 0 < (new_pid = run_client_as_child()) ) {
		cl_log(LOG_NOTICE, "restarted the monitored program, whose PID "
			" is %d", new_pid); 
	} else {
		cl_log(LOG_ERR, "failed to restart the monitored program %s ,"
			"will exit.", pname );
        	g_free(p->privatedata);	
		cl_respawn_quit(3);
	}

        g_free(p->privatedata);	
	p->privatedata = NULL;
}

static void
monitoredProcessRegistered(ProcTrack* p)
{
	cl_log(LOG_INFO, "Child process [%s] started [ pid: %d ]."
			, p->ops->proctype(p), p->pid);
}

static const char *
monitoredProcessName(ProcTrack* p)
{
	gchar * process_name = p->privatedata;
	return  process_name;
}
