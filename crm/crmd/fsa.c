/* 
 * Copyright (C) 2004 Andrew Beekhof <andrew@beekhof.net>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include <crm/common/crm.h>
#include <crmd_fsa.h>
#include <fsa_matrix.h>
#include <fsa_proto.h>
#include <stdio.h>

#include <crm/common/xmlutils.h>
#include <crm/common/msgutils.h>
#include <crm/common/xmltags.h>
#include <crm/common/xmlvalues.h>
#include <clplumbing/Gmain_timeout.h>

#include <crmd_messages.h>
#include <string.h>
#include <time.h>

#include <crm/dmalloc_wrapper.h>

#define DOT_FSA_ACTIONS 1
#define DOT_ALL_FSA_INPUTS 1

enum crmd_fsa_input handle_message(xmlNodePtr stored_msg);


#ifdef DOT_FSA_ACTIONS
#define ELSEIF_FSA_ACTION(x,y)						\
  else if(is_set(actions,x)) {						\
/*	if(cur_input != I_DC_HEARTBEAT)				*/	\
	 fprintf(dot_strm, "\t\t// %s:\t%s\t(0x%.16llx) with data? %s\n",\
		fsa_input2string(cur_input), fsa_action2string(x), x,	\
		data==NULL?"no":"yes");					\
	fflush(dot_strm);						\
	CRM_DEBUG3("Invoking action %s (%.16llx)",			\
		fsa_action2string(x), x);				\
	actions = clear_bit(actions, x);				\
	next_input = y(x, cause, cur_state, last_input, data);		\
	CRM_DEBUG3("Result of action %s was %s",			\
		fsa_action2string(x), fsa_input2string(next_input));	\
  }
#else
#define ELSEIF_FSA_ACTION(x,y)						\
  else if(is_set(actions,x)) {						\
	CRM_DEBUG3("Invoking action %s (%.16llx)",			\
		fsa_action2string(x), x);				\
	actions = clear_bit(actions, x);				\
	next_input = y(x, cause, cur_state, last_input, data);		\
	CRM_DEBUG3("Result of action %s was %s",			\
		fsa_action2string(x), fsa_input2string(next_input));	\
  }
#endif

const char *dot_intro = "digraph \"g\" {\n"
"	size = \"30,30\"\n"
"	graph [\n"
"		fontsize = \"12\"\n"
"		fontname = \"Times-Roman\"\n"
"		fontcolor = \"black\"\n"
"		bb = \"0,0,398.922306,478.927856\"\n"
"		color = \"black\"\n"
"	]\n"
"	node [\n"
"		fontsize = \"12\"\n"
"		fontname = \"Times-Roman\"\n"
"		fontcolor = \"black\"\n"
"		shape = \"ellipse\"\n"
"		color = \"black\"\n"
"	]\n"
"	edge [\n"
"		fontsize = \"12\"\n"
"		fontname = \"Times-Roman\"\n"
"		fontcolor = \"black\"\n"
"		color = \"black\"\n"
"	]\n"
"// special nodes\n"
"	\"S_PENDING\" \n"
"	[\n"
"	 color = \"blue\"\n"
"	 fontcolor = \"blue\"\n"
"	 ]\n"
"	\"S_TERMINATE\" \n"
"	[\n"
"	 color = \"red\"\n"
"	 fontcolor = \"red\"\n"
"	 ]\n"
"\n"
"// DC only nodes\n"
"	\"S_RECOVERY_DC\" [ fontcolor = \"green\" ]\n"
"	\"S_INTEGRATION\" [ fontcolor = \"green\" ]\n"
"	\"S_POLICY_ENGINE\" [ fontcolor = \"green\" ]\n"
"	\"S_TRANSITION_ENGINE\" [ fontcolor = \"green\" ]\n"
"	\"S_RELEASE_DC\" [ fontcolor = \"green\" ]\n"
"	\"S_IDLE\" [ fontcolor = \"green\" ]\n";


static FILE *dot_strm = NULL;

enum crmd_fsa_state fsa_state;
oc_node_list_t *fsa_membership_copy;
ll_cluster_t   *fsa_cluster_connection;
long long       fsa_input_register;
const char     *fsa_our_uname;

fsa_timer_t *election_trigger = NULL;		/*  */
fsa_timer_t *election_timeout = NULL;		/*  */
fsa_timer_t *shutdown_escalation_timmer = NULL;	/*  */
fsa_timer_t *integration_timer = NULL;
fsa_timer_t *dc_heartbeat = NULL;

long long
toggle_bit(long long action_list, long long action)
{
//	CRM_DEBUG2("Toggling bit %.16llx", action);
	action_list ^= action;
//	CRM_DEBUG2("Result %.16llx", action_list & action);
	return action_list;
}

long long
clear_bit(long long action_list, long long action)
{
//	CRM_DEBUG2("Clearing bit\t%.16llx", action);

	// ensure its set
	action_list |= action;

	// then toggle
	action_list = action_list ^ action;

	return action_list;
}

long long
set_bit(long long action_list, long long action)
{
//	CRM_DEBUG2("Adding bit\t%.16llx", action);
	action_list |= action;
	return action_list;
}

void
toggle_bit_inplace(long long *action_list, long long action)
{
	*action_list = toggle_bit(*action_list, action);
}

void
clear_bit_inplace(long long *action_list, long long action)
{
	*action_list = clear_bit(*action_list, action);
}

void
set_bit_inplace(long long *action_list, long long action)
{
	*action_list = set_bit(*action_list, action);
}



gboolean
is_set(long long action_list, long long action)
{
//	CRM_DEBUG2("Checking bit\t%.16llx", action);
	return ((action_list & action) == action);
}

long long
clear_flags(long long actions,
	    enum crmd_fsa_cause cause,
	    enum crmd_fsa_state cur_state,
	    enum crmd_fsa_input cur_input)
{
	return actions;
}

void
startTimer(fsa_timer_t *timer)
{
	if(((int)timer->source_id) < 0) {
		timer->source_id =
			Gmain_timeout_add(timer->period_ms,
					  timer->callback,
					  (void*)timer);
		CRM_DEBUG3("#!!#!!# Started %s timer (%d)",
			   fsa_input2string(timer->fsa_input),
			   timer->source_id);
	} else {
		CRM_DEBUG3("#!!#!!# Timer %s already running (%d)",
			   fsa_input2string(timer->fsa_input),
			   timer->source_id);
		
	}
}


void
stopTimer(fsa_timer_t *timer)
{
	if(((int)timer->source_id) > 0) {
		CRM_DEBUG3("#!!#!!# Stopping %s timer (%d)",
			   fsa_input2string(timer->fsa_input),
			   timer->source_id);
		g_source_remove(timer->source_id);
		timer->source_id = -2;

	} else {
		CRM_DEBUG3("#!!#!!# Timer %s already stopped (%d)",
			   fsa_input2string(timer->fsa_input),
			   timer->source_id);
		
	}
}

enum crmd_fsa_state
s_crmd_fsa(enum crmd_fsa_cause cause,
	   enum crmd_fsa_input initial_input,
	   void *data)
{
	long long           actions = A_NOTHING, new_actions = A_NOTHING;
	enum crmd_fsa_input last_input = initial_input;
	enum crmd_fsa_input cur_input;
	enum crmd_fsa_input next_input;
	enum crmd_fsa_state cur_state, next_state, starting_state;
	
	FNIN();

	starting_state = fsa_state;
	cur_input = initial_input;
	next_input = initial_input;
	
	cur_state = starting_state;
	next_state = cur_state;

	CRM_DEBUG4("FSA invoked with Cause: %s\n\tState: %s, Input: %s",
		   fsa_cause2string(cause),
		   fsa_state2string(cur_state),
		   fsa_input2string(cur_input));

	/*
	 * Process actions in order of priority but do only one
	 * action at a time to avoid complicating the ordering.
	 *
	 * Actions may result in a new I_ event, these are added to
	 * (not replace) existing actions before the next iteration.
	 *
	 */
	while(next_input != I_NULL || actions != A_NOTHING) {

		if(next_input == I_WAIT_FOR_EVENT) {
			/* we may be waiting for an a-sync task to "happen"
			 * and until it does, we cant do anything else
			 */
			cl_log(LOG_INFO, "Wait until something else happens");
			break;
		}

		cur_input = next_input;

		CRM_DEBUG3("FSA while loop:\tState: %s, Input: %s",
			   fsa_state2string(cur_state),
			   fsa_input2string(cur_input));
		
		/* safe to do every time, I_NULL gets us to the same state */
		next_state = crmd_fsa_state[cur_input][cur_state];

		if(cur_input != I_NULL
		   && (cur_input != I_DC_HEARTBEAT || cur_state != S_NOT_DC)){
#ifndef DOT_ALL_FSA_INPUTS
			if(next_state != cur_state) {
#endif			
				const char *state_from =
					fsa_state2string(cur_state);
				const char *state_to =
					fsa_state2string(next_state);
				const char *input =
					fsa_input2string(cur_input);
				
				if(dot_strm == NULL) {
					dot_strm = fopen("/tmp/live.dot", "w");
					fprintf(dot_strm, "%s", dot_intro);
				}
				
				time_t now = time(NULL);
				
				fprintf(dot_strm,
					"\t\"%s\" -> \"%s\" [ label =\"%s\" ] // %s",
					state_from, state_to, input,
					asctime(localtime(&now)));
				fflush(dot_strm);
				
#ifndef DOT_ALL_FSA_INPUTS
			}
#endif
		}
		
		new_actions = crmd_fsa_actions[cur_input][cur_state];

		if(new_actions != A_NOTHING) {
			CRM_DEBUG2("Adding actions %.16llx", new_actions);
			actions |= new_actions;
		}
		

		if(cur_input != I_NULL)
			last_input = cur_input; // only for non I_NULL
	
		cur_state = next_state;
		fsa_state = cur_state;

#if 0
		if(fsa_state == S_RECOVERY || fsa_state == S_RECOVERY_DC) {
			set_bit(actions, A_RECOVER);
		}
#endif	
		
		/* this is always run, some inputs/states may make various
		 * actions irrelevant/invalid
		 */
		actions = clear_flags(actions, cause, cur_state, cur_input);
		
		/* regular action processing in order of action priority
		 * and/or ease of processing
		 */

		/*
		 * Make sure all actions that connect to required systems
		 * are performed first
		 */
		
		/* External connect
		 * These will drop existing connections if present
		 */
		if(actions == A_NOTHING) {

			cl_log(LOG_INFO, "Nothing to do");
			next_input = I_NULL;
		
			// check registers, see if anything is pending
			if(is_set(fsa_input_register, R_SHUTDOWN)) {
				CRM_DEBUG("(Re-)invoking shutdown");
				next_input = I_SHUTDOWN;
			} else if(is_set(fsa_input_register, R_INVOKE_PE)) {
				CRM_DEBUG("Invoke the PE somehow");
			}
		
		}
	
	/* logging */
	ELSEIF_FSA_ACTION(A_ERROR, do_log)
		ELSEIF_FSA_ACTION(A_WARN, do_log)
		ELSEIF_FSA_ACTION(A_LOG,  do_log)
		
		/* get out of here NOW! before anything worse happens */
		ELSEIF_FSA_ACTION(A_EXIT_1,	do_exit)
		
		ELSEIF_FSA_ACTION(A_STARTUP,	do_startup)
		
		ELSEIF_FSA_ACTION(A_CIB_START,  do_cib_control)
		ELSEIF_FSA_ACTION(A_HA_CONNECT, do_ha_register)
		ELSEIF_FSA_ACTION(A_LRM_CONNECT,do_lrm_register)
		ELSEIF_FSA_ACTION(A_CCM_CONNECT,do_ccm_register)
		ELSEIF_FSA_ACTION(A_ANNOUNCE,	do_announce)
		
		/* sub-system start */
		ELSEIF_FSA_ACTION(A_PE_START,	do_pe_control)
		ELSEIF_FSA_ACTION(A_TE_START,	do_te_control)
		
		/* sub-system restart
		 */
		ELSEIF_FSA_ACTION(O_CIB_RESTART,do_cib_control)
		ELSEIF_FSA_ACTION(O_PE_RESTART, do_pe_control)
		ELSEIF_FSA_ACTION(O_TE_RESTART, do_te_control)
		
		ELSEIF_FSA_ACTION(A_STARTED,	do_started)
		
		/* DC Timer */
		ELSEIF_FSA_ACTION(O_DC_TIMER_RESTART,	do_dc_timer_control)
		ELSEIF_FSA_ACTION(A_DC_TIMER_STOP,	do_dc_timer_control)
		ELSEIF_FSA_ACTION(A_DC_TIMER_START,	do_dc_timer_control)
		
		/*
		 * Highest priority actions
		 */
		ELSEIF_FSA_ACTION(A_MSG_ROUTE,		do_msg_route)
		ELSEIF_FSA_ACTION(A_RECOVER,		do_recover)
		ELSEIF_FSA_ACTION(A_ELECTION_VOTE,	do_election_vote)
		ELSEIF_FSA_ACTION(A_ELECTION_COUNT,	do_election_count_vote)
		ELSEIF_FSA_ACTION(A_ELECTION_TIMEOUT,	do_election_timeout)
		
		/*
		 * "Get this over with" actions
		 */
		ELSEIF_FSA_ACTION(A_MSG_STORE,		do_msg_store)
		ELSEIF_FSA_ACTION(A_NODE_BLOCK,		do_node_block)
		
		/*
		 * High priority actions
		 * Update the cache first
		 */
		ELSEIF_FSA_ACTION(A_CCM_UPDATE_CACHE,	do_ccm_update_cache)
		ELSEIF_FSA_ACTION(A_CCM_EVENT,		do_ccm_event)
		
		/*
		 * Medium priority actions
		 */
		ELSEIF_FSA_ACTION(A_DC_TAKEOVER,	do_dc_takeover)
		ELSEIF_FSA_ACTION(A_DC_RELEASE,		do_dc_release)
		ELSEIF_FSA_ACTION(A_JOIN_WELCOME_ALL,	do_send_welcome)
		ELSEIF_FSA_ACTION(A_JOIN_WELCOME,	do_send_welcome)
		ELSEIF_FSA_ACTION(A_JOIN_ACK,		do_ack_welcome)
		ELSEIF_FSA_ACTION(A_JOIN_PROCESS_ACK,	do_process_welcome_ack)
		
		/*
		 * Low(er) priority actions
		 * Make sure the CIB is always updated before invoking the
		 * PE, and the PE before the TE
		 */
		ELSEIF_FSA_ACTION(A_CIB_INVOKE,		do_cib_invoke)
		ELSEIF_FSA_ACTION(A_CIB_DISTRIBUTE,	do_cib_invoke)
		ELSEIF_FSA_ACTION(A_PE_INVOKE,		do_pe_invoke)
		ELSEIF_FSA_ACTION(A_TE_INVOKE,		do_te_invoke)
		
		/* sub-system stop */
		ELSEIF_FSA_ACTION(A_PE_STOP,	do_pe_control)
		ELSEIF_FSA_ACTION(A_TE_STOP,	do_te_control)
		ELSEIF_FSA_ACTION(A_DC_RELEASED,do_dc_release)
		ELSEIF_FSA_ACTION(A_CIB_STOP,	do_cib_control)
		
		/* time to go now... */
		
		/* Some of these can probably be consolidated */
		ELSEIF_FSA_ACTION(A_SHUTDOWN,   do_shutdown)
		ELSEIF_FSA_ACTION(A_STOP,	do_stop)
		
		/* exit gracefully */
		ELSEIF_FSA_ACTION(A_EXIT_0,	do_exit)

//		ELSEIF_FSA_ACTION(A_, do_)
		
		else if(actions & A_MSG_PROCESS) {
			xmlNodePtr stored_msg = NULL;
			
			/* any more queued messages? */
			if(is_message() == FALSE) {
				CRM_DEBUG("No more messages");
				actions = clear_bit(actions, A_MSG_PROCESS);
				continue;
			} else {
				data = get_message()->message;
				if(data == NULL) {
					cl_log(LOG_ERR,
					       "Invalid stored message");
				}
			}

#ifdef DOT_FSA_ACTIONS
			fprintf(dot_strm,
				"\t\t// %s:\t%s\t(0x%.16llx) with data? %s\n",
				fsa_input2string(cur_input),
				fsa_action2string(A_MSG_PROCESS),
				A_MSG_PROCESS,
				stored_msg==NULL?"no":"yes");
			fflush(dot_strm);
#endif
			CRM_DEBUG3("Invoking action %s (%.16llx)",
				   fsa_action2string(A_MSG_PROCESS),
				   A_MSG_PROCESS);

			stored_msg = (xmlNodePtr)data;
			xml_message_debug(stored_msg,
					  "FSA processing message:");

			next_input = handle_message(stored_msg);

			CRM_DEBUG3("Result of action %s was %s",
				   fsa_action2string(A_MSG_PROCESS),
				   fsa_input2string(next_input));	
	
			/* Error checking and reporting */
		} else if(cur_input != I_NULL && is_set(actions, A_NOTHING)) {
			cl_log(LOG_WARNING,
			       "No action specified for input,state (%s,%s)",
			       fsa_input2string(cur_input),
			       fsa_state2string(cur_state));
			
			next_input = I_NULL;
			
		} else if(cur_input == I_NULL && is_set(actions, A_NOTHING)) {
			cl_log(LOG_INFO, "Nothing left to do");
			
		} else {
			cl_log(LOG_ERR, "Action %s (0x%llx) not supported ",
			       fsa_action2string(actions), actions);
			next_input = I_ERROR;
		}
	}
	
	CRM_DEBUG2("################# Exiting the FSA (%s) ##################",
		  fsa_state2string(fsa_state));

	// cleanup inputs?
	
	FNRET(fsa_state);
}


/*	A_NODE_BLOCK	*/
enum crmd_fsa_input
do_node_block(long long action,
	      enum crmd_fsa_cause cause,
	      enum crmd_fsa_state cur_state,
	      enum crmd_fsa_input current_input,
	      void *data)
{

	xmlNodePtr xml_message = (xmlNodePtr)data;
	const char *host_from  = xmlGetProp(xml_message, XML_ATTR_HOSTFROM);

	FNIN();
	
	(void)host_from;
	
	
	FNRET(I_NULL);
}


const char *
fsa_input2string(int input)
{
	gboolean found = TRUE;
	const char *inputAsText = NULL;
	
	switch(input){
		case I_NULL:
			inputAsText = "I_NULL";
			break;
		case I_CCM_EVENT:
			inputAsText = "I_CCM_EVENT";
			break;
		case I_CIB_UPDATE:
			inputAsText = "I_CIB_UPDATE";
			break;
		case I_DC_TIMEOUT:
			inputAsText = "I_DC_TIMEOUT";
			break;
		case I_ELECTION:
			inputAsText = "I_ELECTION";
			break;
		case I_RELEASE_DC:
			inputAsText = "I_RELEASE_DC";
			break;
		case I_ELECTION_DC:
			inputAsText = "I_ELECTION_DC";
			break;
		case I_ERROR:
			inputAsText = "I_ERROR";
			break;
		case I_FAIL:
			inputAsText = "I_FAIL";
			break;
		case I_INTEGRATION_TIMEOUT:
			inputAsText = "I_INTEGRATION_TIMEOUT";
			break;
		case I_NODE_JOIN:
			inputAsText = "I_NODE_JOIN";
			break;
		case I_NODE_LEFT:
			inputAsText = "I_NODE_LEFT";
			break;
		case I_NODE_LEAVING:
			inputAsText = "I_NODE_LEAVING";
			break;
		case I_NOT_DC:
			inputAsText = "I_NOT_DC";
			break;
		case I_RECOVERED:
			inputAsText = "I_RECOVERED";
			break;
		case I_RELEASE_FAIL:
			inputAsText = "I_RELEASE_FAIL";
			break;
		case I_RELEASE_SUCCESS:
			inputAsText = "I_RELEASE_SUCCESS";
			break;
		case I_RESTART:
			inputAsText = "I_RESTART";
			break;
		case I_REQUEST:
			inputAsText = "I_REQUEST";
			break;
		case I_ROUTER:
			inputAsText = "I_ROUTER";
			break;
		case I_SHUTDOWN:
			inputAsText = "I_SHUTDOWN";
			break;
		case I_STARTUP:
			inputAsText = "I_STARTUP";
			break;
		case I_SUCCESS:
			inputAsText = "I_SUCCESS";
			break;
		case I_WELCOME:
			inputAsText = "I_WELCOME";
			break;
		case I_WELCOME_ACK:
			inputAsText = "I_WELCOME_ACK";
			break;
		case I_DC_HEARTBEAT:
			inputAsText = "I_DC_HEARTBEAT";
			break;
		case I_ILLEGAL:
			inputAsText = "I_ILLEGAL";
			break;
		default:
			found = FALSE;
			inputAsText = "<UNKNOWN_INPUT>";
			break;
	}

	if(found == FALSE) {
		cl_log(LOG_ERR, "Input %d is unknown", input);
	}
	
	return inputAsText;
}

const char *
fsa_state2string(int state)
{
	gboolean found = TRUE;
	const char *stateAsText = NULL;
	
	switch(state){
		case S_IDLE:
			stateAsText = "S_IDLE";
			break;
		case S_ELECTION:
			stateAsText = "S_ELECTION";
			break;
		case S_INTEGRATION:
			stateAsText = "S_INTEGRATION";
			break;
		case S_NOT_DC:
			stateAsText = "S_NOT_DC";
			break;
		case S_POLICY_ENGINE:
			stateAsText = "S_POLICY_ENGINE";
			break;
		case S_RECOVERY:
			stateAsText = "S_RECOVERY";
			break;
		case S_RECOVERY_DC:
			stateAsText = "S_RECOVERY_DC";
			break;
		case S_RELEASE_DC:
			stateAsText = "S_RELEASE_DC";
			break;
		case S_PENDING:
			stateAsText = "S_PENDING";
			break;
		case S_STOPPING:
			stateAsText = "S_STOPPING";
			break;
		case S_TERMINATE:
			stateAsText = "S_TERMINATE";
			break;
		case S_TRANSITION_ENGINE:
			stateAsText = "S_TRANSITION_ENGINE";
			break;
		case S_ILLEGAL:
			stateAsText = "S_ILLEGAL";
			break;
		default:
			found = FALSE;
			stateAsText = "<UNKNOWN_STATE>";
			break;
	}

	if(found == FALSE) {
		cl_log(LOG_ERR, "State %d is unknown", state);
	}
	
	return stateAsText;
}

const char *
fsa_cause2string(int cause)
{
	gboolean found = TRUE;
	const char *causeAsText = NULL;
	
	switch(cause){
		case C_UNKNOWN:
			causeAsText = "C_UNKNOWN";
			break;
		case C_STARTUP:
			causeAsText = "C_STARTUP";
			break;
		case C_IPC_MESSAGE:
			causeAsText = "C_IPC_MESSAGE";
			break;
		case C_HA_MESSAGE:
			causeAsText = "C_HA_MESSAGE";
			break;
		case C_CCM_CALLBACK:
			causeAsText = "C_CCM_CALLBACK";
			break;
		case C_TIMER_POPPED:
			causeAsText = "C_TIMER_POPPED";
			break;
		case C_SHUTDOWN:
			causeAsText = "C_SHUTDOWN";
			break;
		case C_HEARTBEAT_FAILED:
			causeAsText = "C_HEARTBEAT_FAILED";
			break;
		case C_ILLEGAL:
			causeAsText = "C_ILLEGAL";
			break;
		default:
			found = FALSE;
			causeAsText = "<UNKNOWN_CAUSE>";
			break;
	}

	if(found == FALSE) {
		cl_log(LOG_ERR, "Cause %d is unknown", cause);
	}
	
	return causeAsText;
}

const char *
fsa_action2string(long long action)
{
	gboolean found = TRUE;
	const char *actionAsText = NULL;
	
	switch(action){

		case A_NOTHING:
			actionAsText = "A_NOTHING";
			break;
		case O_SHUTDOWN:
			actionAsText = "O_SHUTDOWN";
			break;
		case O_RELEASE:
			actionAsText = "O_RELEASE";
			break;
		case A_STARTUP:
			actionAsText = "A_STARTUP";
			break;
		case A_STARTED:
			actionAsText = "A_STARTED";
			break;
		case A_HA_CONNECT:
			actionAsText = "A_HA_CONNECT";
			break;
		case A_LRM_CONNECT:
			actionAsText = "A_LRM_CONNECT";
			break;
		case O_DC_TIMER_RESTART:
			actionAsText = "O_DC_TIMER_RESTART";
			break;
		case A_DC_TIMER_STOP:
			actionAsText = "A_DC_TIMER_STOP";
			break;
		case A_DC_TIMER_START:
			actionAsText = "A_DC_TIMER_START";
			break;
		case A_ELECTION_COUNT:
			actionAsText = "A_ELECTION_COUNT";
			break;
		case A_ELECTION_TIMEOUT:
			actionAsText = "A_ELECTION_TIMEOUT";
			break;
		case A_ELECTION_VOTE:
			actionAsText = "A_ELECTION_VOTE";
			break;
		case A_ANNOUNCE:
			actionAsText = "A_ANNOUNCE";
			break;
		case A_JOIN_ACK:
			actionAsText = "A_JOIN_ACK";
			break;
		case A_JOIN_WELCOME:
			actionAsText = "A_JOIN_WELCOME";
			break;
		case A_JOIN_WELCOME_ALL:
			actionAsText = "A_JOIN_WELCOME_ALL";
			break;
		case A_JOIN_PROCESS_ACK:
			actionAsText = "A_JOIN_PROCESS_ACK";
			break;
		case A_MSG_PROCESS:
			actionAsText = "A_MSG_PROCESS";
			break;
		case A_MSG_ROUTE:
			actionAsText = "A_MSG_ROUTE";
			break;
		case A_MSG_STORE:
			actionAsText = "A_MSG_STORE";
			break;
		case A_RECOVER:
			actionAsText = "A_RECOVER";
			break;
		case A_DC_RELEASE:
			actionAsText = "A_DC_RELEASE";
			break;
		case A_DC_RELEASED:
			actionAsText = "A_DC_RELEASED";
			break;
		case A_DC_TAKEOVER:
			actionAsText = "A_DC_TAKEOVER";
			break;
		case A_SHUTDOWN:
			actionAsText = "A_SHUTDOWN";
			break;
		case A_STOP:
			actionAsText = "A_STOP";
			break;
		case A_EXIT_0:
			actionAsText = "A_EXIT_0";
			break;
		case A_EXIT_1:
			actionAsText = "A_EXIT_1";
			break;
		case A_CCM_CONNECT:
			actionAsText = "A_CCM_CONNECT";
			break;
		case A_CCM_EVENT:
			actionAsText = "A_CCM_EVENT";
			break;
		case A_CCM_UPDATE_CACHE:
			actionAsText = "A_CCM_UPDATE_CACHE";
			break;
		case A_CIB_DISTRIBUTE:
			actionAsText = "A_CIB_DISTRIBUTE";
			break;
		case A_CIB_INVOKE:
			actionAsText = "A_CIB_INVOKE";
			break;
		case O_CIB_RESTART:
			actionAsText = "O_CIB_RESTART";
			break;
		case A_CIB_START:
			actionAsText = "A_CIB_START";
			break;
		case A_CIB_STOP:
			actionAsText = "A_CIB_STOP";
			break;
		case A_TE_INVOKE:
			actionAsText = "A_TE_INVOKE";
			break;
		case O_TE_RESTART:
			actionAsText = "O_TE_RESTART";
			break;
		case A_TE_START:
			actionAsText = "A_TE_START";
			break;
		case A_TE_STOP:
			actionAsText = "A_TE_STOP";
			break;
		case A_PE_INVOKE:
			actionAsText = "A_PE_INVOKE";
			break;
		case O_PE_RESTART:
			actionAsText = "O_PE_RESTART";
			break;
		case A_PE_START:
			actionAsText = "A_PE_START";
			break;
		case A_PE_STOP:
			actionAsText = "A_PE_STOP";
			break;
		case A_NODE_BLOCK:
			actionAsText = "A_NODE_BLOCK";
			break;
		case A_LOG:
			actionAsText = "A_LOG";
			break;
		case A_ERROR:
			actionAsText = "A_ERROR";
			break;
		case A_WARN:
			actionAsText = "A_WARN";
			break;
		default:
			found = FALSE;
			actionAsText = "<UNKNOWN_ACTION>";
			break;
	}

	if(found == FALSE) {
		cl_log(LOG_ERR, "Action %.16llx is unknown", action);
	}
	
	return actionAsText;
}


enum crmd_fsa_input
handle_message(xmlNodePtr stored_msg)
{
	enum crmd_fsa_input next_input = I_NULL;
	xmlNodePtr options = find_xml_node(stored_msg, XML_TAG_OPTIONS);

	const char *sys_to   = xmlGetProp(stored_msg, XML_ATTR_SYSTO);
	const char *sys_from = xmlGetProp(stored_msg, XML_ATTR_SYSFROM);
	const char *type     = xmlGetProp(stored_msg, XML_ATTR_MSGTYPE);
	const char *op = xmlGetProp(options, XML_ATTR_OP);

	if(type == NULL || op == NULL) {
		cl_log(LOG_ERR, "Ignoring message (type=%s), (op=%s)",
		       type, op);
		xml_message_debug(stored_msg, "Bad message");
		
	} else if(strcmp(type, XML_ATTR_REQUEST) == 0){
		if(strcmp(op, "vote") == 0) {
			next_input = I_ELECTION;
				
		} else if(strcmp(op, "beat") == 0) {
			next_input = I_DC_HEARTBEAT;
				
		} else if(strcmp(op, "welcome") == 0) {
			next_input = I_WELCOME;
				
		} else if(strcmp(op, "announce") == 0) {
			next_input = I_NODE_JOIN;
				
		} else if(strcmp(op, "store") == 0 && AM_I_DC) {
			next_input = I_RELEASE_DC;
				
		} else if(strcmp(op, "store") == 0
			  || strcmp(op, "create") == 0
			  || strcmp(op, "update") == 0
			  || strcmp(op, "delete") == 0
			  || strcmp(op, "erase") == 0) {
			next_input = I_CIB_UPDATE;
				
		} else if(strcmp(op, "ping") == 0) {
			/* eventually do some stuff to figure out
			 * if we /are/ ok
			 */
			xmlNodePtr ping =
				createPingAnswerFragment(sys_to, "ok");
				
			xmlNodePtr wrapper = create_reply(stored_msg, ping);
			relay_message(wrapper, TRUE);
			free_xml(wrapper);
				
		} else {
			cl_log(LOG_ERR,
			       "Unexpected (request) operation %s", op);
		}
		
	} else if(strcmp(type, XML_ATTR_RESPONSE) == 0) {

		if(strcmp(op, "welcome") == 0) {
			next_input = I_WELCOME;
				
		} else if(strcmp(op, "join_ack") == 0) {
			next_input = I_WELCOME_ACK;
				
		} else if(strcmp(op, "create") == 0
			  || strcmp(op, "update") == 0
			  || strcmp(op, "delete") == 0
			  || strcmp(op, "erase") == 0) {

			// perhaps we should do somethign with these replies

		} else if(strcmp(op, "bump") == 0) {

			xmlNodePtr data = find_xml_node(stored_msg,
							XML_TAG_FRAGMENT);
			
			send_request(NULL, data, "store",
				     NULL, CRM_SYSTEM_CRMD);

		} else if (strcmp(op, "store") == 0) {

			if(AM_I_DC == FALSE) {
				
				/* we need to merge these results back
				 * into the DC
				 */
				xmlNodePtr data =
					find_xml_node(stored_msg,
						      XML_TAG_FRAGMENT);
				
				if(data != NULL) {
					send_request(NULL, data, "update",
						     NULL, CRM_SYSTEM_CIB);
				}
			}

		} else if(strcmp(sys_from, CRM_SYSTEM_CIB) == 0
		   && strcmp(op, "forward") == 0
		   && AM_I_DC) {
				
			/* this is a reply to our earlier command
			 * Send it to the relevant node(s)
			 */
			const char *uname = xmlGetProp(options,
						       "forward_to");

			xmlNodePtr data = find_xml_node(stored_msg,
							XML_TAG_FRAGMENT);

			xmlNodePtr local_options =
				create_xml_node(NULL, XML_TAG_OPTIONS);

			/* If this is part of join request processing,
			 * ask for the status section, otherwise just blast
			 * it down to them.
			 */
			if(uname != NULL) {
				set_xml_property_copy(local_options,
						      XML_ATTR_VERBOSE,
						      "true");
			}
			
			send_request(local_options, data, "store",
				     uname, CRM_SYSTEM_CRMD);
				
		} else {
			cl_log(LOG_ERR,
			       "Unexpected (response) operation %s", op);
		}
	} else {
			
	}
		
	return next_input;
		
}
