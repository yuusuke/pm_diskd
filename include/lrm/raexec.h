/*
 * raexec.h: The universal interface of RA Execution Plugin
 *
 * Author: Sun Jiang Dong <sunjd@cn.ibm.com>
 * Copyright (c) 2004 International Business Machines
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#ifndef RAEXEC_H
#define RAEXEC_H
#include <glib.h>

typedef enum UNIFORM_RET_EXECRA uniform_ret_execra_t;
/* Uniform return value of executing RA */
enum UNIFORM_RET_EXECRA {
	EXECRA_EXEC_UNKNOWN_ERROR = -200,
	EXECRA_NO_RA = -100,
	EXECRA_OK = 0,
	EXECRA_UNKNOWN_ERROR = 1,
	EXECRA_INVALID_PARAM = 2,
	EXECRA_UNIMPLEMENT_FEATURE = 3,
	EXECRA_INSUFFICIENT_PRIV = 4,
	EXECRA_NOT_INSTALLED = 5,
	EXECRA_NOT_CONFIGURED = 6,
	EXECRA_NOT_RUNNING = 7,
		
	/* For status command only */
	EXECRA_RA_DEAMON_DEAD1 = 11,
	EXECRA_RA_DEAMON_DEAD2 = 12,
	EXECRA_RA_DEAMON_STOPPED = 13,
	EXECRA_STATUS_UNKNOWN = 14
};

const int RA_MAX_DIRNAME_LENGTH  = 200,
	  RA_MAX_BASENAME_LENGTH = 40; 


typedef struct {
	gchar * rsc_type;
	/* As for version, no definite definition yet */
	gchar * version;	
} rsc_info_t;

/* 
 * RA Execution Interfaces 
 * The plugin usage is divided into two step. First to send out a command to
 * execute a resource agency via calling function execra. Execra is a unblock
 * function, always return at once. Then to call function post_query_result to
 * get the RA exection result.     
*/
struct RAExecOps { 
        /* 
	 * Description: 
	 * 	Launch a exection of a resource agency -- normally is a script
	 *
	 * Parameters:
	 * 	ra_name:  The basename of a RA.
	 *	op:	  The operation that hope RA to do, such as "start",
	 *		    "stop" and so on.
	 *	cmd_params: The command line parameters need to be passed to 
	 *		      the RA for a execution. 
	 *	env_params: The enviroment parameters need to be set for 
	 *		      affecting the action of a RA execution. As for 
	 *		      OCF style RA, it's the only way to pass 
	 *		      parameter to the RA.
	 *
	 * Return Value:
	 *	0:  RA execution is ok, while the exec_key is a valid value.
	 *	-1: The RA don't exist.
	 * 	-2: There are invalid command line parameters.
	 *	-3: Other unkown error when launching the execution.
	 */
	int (*execra)(
		const char * ra_name,	
		const char * op,
		GHashTable * cmd_params,
		GHashTable * env_params);

	/*
	 * Description:
	 *	Map the specific ret value to a uniform value.
	 *
	 * Parameters:
	 *	ret_execra: the RA type specific ret value. 
	 *	
	 * Return Value:
	 *	A uniform value without regarding RA type.
	 */
	uniform_ret_execra_t (*map_ra_retvalue)(int ret_execra,
				const char * op);

	/*
	 * Description:
	 * 	List all resource info of this class ( OCF )	
	 *
	 * Parameters:
	 *	rsc_info: a GList which item data type is rsc_info_t as 
	 *		  defined above, containing all resource info of
	 *		  this class in the local machine.
	 *
	 * Return Value:
	 *	0 : succeed
	 *	-1: failed due to invalid RA directory such as not existing.
	 *	-2: failed due to other factors
	 */
	int (*get_resource_list)(GList ** rsc_info);
};

#define RA_EXEC_TYPE	RAExec
#define RA_EXEC_TYPE_S	MKSTRING(RAExec)

#endif /* RAEXEC_H */
