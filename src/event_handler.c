/*
 * event_handler.c
 *
 *  Created on: Nov 8, 2015
 *	  Author: pchero
 */

#include "asterisk.h"

#include <stdbool.h>
#include <event2/event.h>
#include <event2/thread.h>
#include <errno.h>

#include "asterisk/json.h"
#include "asterisk/utils.h"
#include "asterisk/cli.h"
#include "asterisk/uuid.h"
#include "asterisk/file.h"

#include "res_outbound.h"
#include "db_handler.h"
#include "ami_handler.h"
#include "event_handler.h"
#include "dialing_handler.h"
#include "campaign_handler.h"
#include "dl_handler.h"
#include "plan_handler.h"
#include "destination_handler.h"
#include "utils.h"

#define TEMP_FILENAME "/tmp/asterisk_outbound_tmp.txt"
#define DEF_EVENT_TIME_FAST "100000"
#define DEF_EVENT_TIME_SLOW "3000000"
#define DEF_ONE_SEC_IN_MICRO_SEC	1000000


struct event_base*  g_base = NULL;

static int init_outbound(void);

static void cb_campaign_start(__attribute__((unused)) int fd, __attribute__((unused)) short event, __attribute__((unused)) void *arg);
static void cb_campaign_starting(__attribute__((unused)) int fd, __attribute__((unused)) short event, __attribute__((unused)) void *arg);
static void cb_campaign_stopping(__attribute__((unused)) int fd, __attribute__((unused)) short event, __attribute__((unused)) void *arg);
static void cb_campaign_stopping_force(__attribute__((unused)) int fd, __attribute__((unused)) short event, __attribute__((unused)) void *arg);
static void cb_check_dialing_end(__attribute__((unused)) int fd, __attribute__((unused)) short event, __attribute__((unused)) void *arg);
static void cb_check_dialing_error(__attribute__((unused)) int fd, __attribute__((unused)) short event, __attribute__((unused)) void *arg);
static void cb_check_campaign_end(__attribute__((unused)) int fd, __attribute__((unused)) short event, __attribute__((unused)) void *arg);
static void cb_check_campaign_schedule_start(__attribute__((unused)) int fd, __attribute__((unused)) short event, __attribute__((unused)) void *arg);
static void cb_check_campaign_schedule_end(__attribute__((unused)) int fd, __attribute__((unused)) short event, __attribute__((unused)) void *arg);
//static void cb_campaign_schedule_stopping(__attribute__((unused)) int fd, __attribute__((unused)) short event, __attribute__((unused)) void *arg);



//static struct ast_json* get_queue_info(const char* uuid);

static void dial_desktop(const struct ast_json* j_camp, const struct ast_json* j_plan, const struct ast_json* j_dlma);
static void dial_power(const struct ast_json* j_camp, const struct ast_json* j_plan, const struct ast_json* j_dlma);
static void dial_predictive(struct ast_json* j_camp, struct ast_json* j_plan, struct ast_json* j_dlma, struct ast_json* j_dest);
static void dial_robo(const struct ast_json* j_camp, const struct ast_json* j_plan, const struct ast_json* j_dlma);
static void dial_redirect(const struct ast_json* j_camp, const struct ast_json* j_plan, const struct ast_json* j_dlma);

//struct ast_json* get_queue_summary(const char* name);
//struct ast_json* get_queue_param(const char* name);

static bool write_result_json(struct ast_json* j_res);

// todo
static int check_dial_avaiable_predictive(struct ast_json* j_camp, struct ast_json* j_plan, struct ast_json* j_dlma, struct ast_json* j_dest);

int run_outbound(void)
{
	int ret;
	int event_delay;
	const char* tmp_const;
	struct event* ev;
	struct timeval tm_fast;
	struct timeval tm_slow;

	// event delay fast.
	tmp_const = ast_json_string_get(ast_json_object_get(ast_json_object_get(g_app->j_conf, "general"), "event_time_fast"));
	if(tmp_const == NULL) {
		tmp_const = DEF_EVENT_TIME_FAST;
		ast_log(LOG_NOTICE, "Could not get correct event_time_fast value. Set default. event_time_fast[%s]\n", tmp_const);
	}
	ast_log(LOG_NOTICE, "Event delay time for fast event. event_time_fast[%s]\n", tmp_const);

	event_delay = atoi(tmp_const);
	tm_fast.tv_sec = event_delay / DEF_ONE_SEC_IN_MICRO_SEC;
	tm_fast.tv_usec = event_delay % DEF_ONE_SEC_IN_MICRO_SEC;

	// event delay slow.
	tmp_const = ast_json_string_get(ast_json_object_get(ast_json_object_get(g_app->j_conf, "general"), "event_time_slow"));
	if(tmp_const == NULL) {
		tmp_const = DEF_EVENT_TIME_SLOW;
		ast_log(LOG_NOTICE, "Could not get correct event_time_fast value. Set default. event_time_fast[%s]\n", tmp_const);
	}
	ast_log(LOG_NOTICE, "Event delay time for fast event. event_time_fast[%s]\n", tmp_const);

	event_delay = atoi(tmp_const);
	tm_slow.tv_sec = event_delay / DEF_ONE_SEC_IN_MICRO_SEC;
	tm_slow.tv_usec = event_delay % DEF_ONE_SEC_IN_MICRO_SEC;

	// init libevent
	ret = init_outbound();
	if(ret == false) {
		ast_log(LOG_ERROR, "Could not initiate outbound.\n");
		return false;
	}

	// check start.
	ev = event_new(g_base, -1, EV_TIMEOUT | EV_PERSIST, cb_campaign_start, NULL);
	event_add(ev, &tm_fast);

	// check starting
	ev = event_new(g_base, -1, EV_TIMEOUT | EV_PERSIST, cb_campaign_starting, NULL);
	event_add(ev, &tm_slow);

	// check stopping.
	ev = event_new(g_base, -1, EV_TIMEOUT | EV_PERSIST, cb_campaign_stopping, NULL);
	event_add(ev, &tm_slow);

	// check force stopping
	ev = event_new(g_base, -1, EV_TIMEOUT | EV_PERSIST, cb_campaign_stopping_force, NULL);
	event_add(ev, &tm_slow);

	// chceck dialing end
	ev = event_new(g_base, -1, EV_TIMEOUT | EV_PERSIST, cb_check_dialing_end, NULL);
	event_add(ev, &tm_fast);

	// check diaing error
	ev = event_new(g_base, -1, EV_TIMEOUT | EV_PERSIST, cb_check_dialing_error, NULL);
	event_add(ev, &tm_fast);

	// check end
	ev = event_new(g_base, -1, EV_TIMEOUT | EV_PERSIST, cb_check_campaign_end, NULL);
	event_add(ev, &tm_slow);

	// check campaign scheduling start
	ev = event_new(g_base, -1, EV_TIMEOUT | EV_PERSIST, cb_check_campaign_schedule_start, NULL);
	event_add(ev, &tm_slow);

	// check campaign scheduling end
	ev = event_new(g_base, -1, EV_TIMEOUT | EV_PERSIST, cb_check_campaign_schedule_end, NULL);
	event_add(ev, &tm_slow);

//	// check campaign scheduling for stop
//	ev = event_new(g_base, -1, EV_TIMEOUT | EV_PERSIST, cb_campaign_schedule_stopping, NULL);
//	event_add(ev, &tm_slow);

	event_base_loop(g_base, 0);

	return true;
}

static int init_outbound(void)
{
	int ret;
	struct ast_json* j_res;
	db_res_t* db_res;

	ret = evthread_use_pthreads();
	if(ret == -1){
		ast_log(LOG_ERROR, "Could not initiated event thread.");
	}

	// init libevent
	if(g_base == NULL) {
		ast_log(LOG_DEBUG, "event_base_new\n");
		g_base = event_base_new();
	}

	if(g_base == NULL) {
		ast_log(LOG_ERROR, "Could not initiate libevent. err[%d:%s]\n", errno, strerror(errno));
		return false;
	}

	// check database tables.
	db_res = db_query("select 1 from campaign limit 1;");
	if(db_res == NULL) {
		ast_log(LOG_ERROR, "Could not initiate libevent. Table is not ready.\n");
		return false;
	}

	j_res = db_get_record(db_res);
	db_free(db_res);
	AST_JSON_UNREF(j_res);

	ret = init_plan();
	if(ret == false) {
		ast_log(LOG_ERROR, "Could not initiate plan.\n");
		return false;
	}

	ast_log(LOG_NOTICE, "Initiated outbound.\n");

	return true;
}

void stop_outbound(void)
{
	struct timeval sec;

	sec.tv_sec = 0;
	sec.tv_usec = 0;

	event_base_loopexit(g_base, &sec);

	return;
}

/**
 *  @brief  Check start status campaign and trying to make a call.
 */
static void cb_campaign_start(__attribute__((unused)) int fd, __attribute__((unused)) short event, __attribute__((unused)) void *arg)
{
	struct ast_json* j_camp;
	struct ast_json* j_plan;
	struct ast_json* j_dlma;
	struct ast_json* j_dest;
	int dial_mode;

	j_camp = get_campaign_for_dialing();
	if(j_camp == NULL) {
		// Nothing.
		return;
	}
//	ast_log(LOG_DEBUG, "Get campaign info. camp_uuid[%s], camp_name[%s]\n",
//			ast_json_string_get(ast_json_object_get(j_camp, "uuid")),
//			ast_json_string_get(ast_json_object_get(j_camp, "name"))
//			);

	// get plan
	j_plan = get_plan(ast_json_string_get(ast_json_object_get(j_camp, "plan")));
	if(j_plan == NULL) {
		ast_log(LOG_WARNING, "Could not get plan info. Stopping campaign camp[%s], plan[%s]\n",
				ast_json_string_get(ast_json_object_get(j_camp, "uuid")),
				ast_json_string_get(ast_json_object_get(j_camp, "plan"))
				);
		update_campaign_status(ast_json_string_get(ast_json_object_get(j_camp, "uuid")), E_CAMP_STOPPING);
		AST_JSON_UNREF(j_camp);
		return;
	}

	// get destination
	j_dest = get_destination(ast_json_string_get(ast_json_object_get(j_camp, "dest")));
	if(j_dest == NULL) {
		ast_log(LOG_WARNING, "Could not get dest info. Stopping campaign camp[%s], dest[%s]\n",
						ast_json_string_get(ast_json_object_get(j_camp, "uuid"))? : "",
						ast_json_string_get(ast_json_object_get(j_camp, "dest"))? : ""
						);
		update_campaign_status(ast_json_string_get(ast_json_object_get(j_camp, "uuid")), E_CAMP_STOPPING);
		AST_JSON_UNREF(j_camp);
		AST_JSON_UNREF(j_plan);
		return;
	}

	// get dl_master_info
	j_dlma = get_dlma(ast_json_string_get(ast_json_object_get(j_camp, "dlma")));
	if(j_dlma == NULL)
	{
		ast_log(LOG_ERROR, "Could not find dial list master info. Stopping campaign. camp[%s], dlma[%s]\n",
				ast_json_string_get(ast_json_object_get(j_camp, "uuid")),
				ast_json_string_get(ast_json_object_get(j_camp, "dlma"))
				);
		update_campaign_status(ast_json_string_get(ast_json_object_get(j_camp, "uuid")), E_CAMP_STOPPING);
		AST_JSON_UNREF(j_camp);
		AST_JSON_UNREF(j_plan);
		AST_JSON_UNREF(j_dest);
		return;
	}
	ast_log(LOG_VERBOSE, "Get dlma info. dlma_uuid[%s], dlma_name[%s]\n",
			ast_json_string_get(ast_json_object_get(j_dlma, "uuid")),
			ast_json_string_get(ast_json_object_get(j_dlma, "name"))
			);

	// get dial_mode
	dial_mode = ast_json_integer_get(ast_json_object_get(j_plan, "dial_mode"));
	if(dial_mode == E_DIAL_MODE_NONE) {
		ast_log(LOG_ERROR, "Plan has no dial_mode. Stopping campaign. camp[%s], plan[%s]",
				ast_json_string_get(ast_json_object_get(j_camp, "uuid")),
				ast_json_string_get(ast_json_object_get(j_camp, "plan"))
				);

		update_campaign_status(ast_json_string_get(ast_json_object_get(j_camp, "uuid")), E_CAMP_STOPPING);
		AST_JSON_UNREF(j_camp);
		AST_JSON_UNREF(j_plan);
		AST_JSON_UNREF(j_dlma);
		AST_JSON_UNREF(j_dest);
		return;
	}

	switch(dial_mode) {
		case E_DIAL_MODE_PREDICTIVE: {
			dial_predictive(j_camp, j_plan, j_dlma, j_dest);
		}
		break;

		case E_DIAL_MODE_DESKTOP: {
			dial_desktop(j_camp, j_plan, j_dlma);
		}
		break;

		case E_DIAL_MODE_POWER: {
			dial_power(j_camp, j_plan, j_dlma);
		}
		break;

		case E_DIAL_MODE_ROBO: {
			dial_robo(j_camp, j_plan, j_dlma);
		}
		break;

		case E_DIAL_MODE_REDIRECT: {
			dial_redirect(j_camp, j_plan, j_dlma);
		}
		break;

		default: {
			ast_log(LOG_ERROR, "No match dial_mode. dial_mode[%d]\n", dial_mode);
		}
		break;
	}

	// release
	AST_JSON_UNREF(j_camp);
	AST_JSON_UNREF(j_plan);
	AST_JSON_UNREF(j_dlma);
	AST_JSON_UNREF(j_dest);

	return;
}

/**
 *  @brief  Check starting status campaign and update status to stopping or start.
 */
static void cb_campaign_starting(__attribute__((unused)) int fd, __attribute__((unused)) short event, __attribute__((unused)) void *arg)
{
	struct ast_json* j_camps;
	struct ast_json* j_camp;
	int size;
	int i;
	int ret;

	j_camps = get_campaigns_by_status(E_CAMP_STARTING);
	if(j_camps == NULL) {
		// Nothing.
		return;
	}

	size = ast_json_array_size(j_camps);
	for(i = 0; i < size; i++) {
		j_camp = ast_json_array_get(j_camps, i);

		// check startable
		ret = is_startable_campgain(j_camp);
		if(ret == false) {
			continue;
		}

		// update campaign status
		ast_log(LOG_NOTICE, "Update campaign status to start. camp_uuid[%s], camp_name[%s]\n",
				ast_json_string_get(ast_json_object_get(j_camp, "uuid")),
				ast_json_string_get(ast_json_object_get(j_camp, "name"))
				);
		ret = update_campaign_status(ast_json_string_get(ast_json_object_get(j_camp, "uuid")), E_CAMP_START);
		if(ret == false) {
			ast_log(LOG_ERROR, "Could not update campaign status to start. camp_uuid[%s], camp_name[%s]\n",
					ast_json_string_get(ast_json_object_get(j_camp, "uuid")),
					ast_json_string_get(ast_json_object_get(j_camp, "name"))
					);
		}
	}
	AST_JSON_UNREF(j_camps);
	return;
}

/**
 * Check Stopping status campaign, and update to stop.
 * @param fd
 * @param event
 * @param arg
 */
static void cb_campaign_stopping(__attribute__((unused)) int fd, __attribute__((unused)) short event, __attribute__((unused)) void *arg)
{
	struct ast_json* j_camps;
	struct ast_json* j_camp;
	int i;
	int size;
	int ret;

	j_camps = get_campaigns_by_status(E_CAMP_STOPPING);
	if(j_camps == NULL) {
		// Nothing.
		return;
	}

	size = ast_json_array_size(j_camps);
	for(i = 0; i < size; i++) {
		j_camp = ast_json_array_get(j_camps, i);

		// check stoppable campaign
		ret = is_stoppable_campgain(j_camp);
		if(ret == false) {
			continue;
		}
		
		// update status to stop
		ast_log(LOG_NOTICE, "Update campaign status to stop. camp_uuid[%s], camp_name[%s]\n",
				ast_json_string_get(ast_json_object_get(j_camp, "uuid")),
				ast_json_string_get(ast_json_object_get(j_camp, "name"))
				);
		ret = update_campaign_status(ast_json_string_get(ast_json_object_get(j_camp, "uuid")), E_CAMP_STOP);
		if(ret == false) {
			ast_log(LOG_ERROR, "Could not update campaign status to stop. camp_uuid[%s], camp_name[%s]\n",
				ast_json_string_get(ast_json_object_get(j_camp, "uuid")),
				ast_json_string_get(ast_json_object_get(j_camp, "name"))
				);
		}
	}

	AST_JSON_UNREF(j_camps);

}

/**
 * Check Stopping status campaign, and update to stop.
 * @param fd
 * @param event
 * @param arg
 */
static void cb_campaign_stopping_force(__attribute__((unused)) int fd, __attribute__((unused)) short event, __attribute__((unused)) void *arg)
{
	struct ast_json* j_camps;
	struct ast_json* j_camp;
	struct ao2_iterator iter;
	rb_dialing* dialing;
	const char* tmp_const;
	int i;
	int size;

	j_camps = get_campaigns_by_status(E_CAMP_STOPPING_FORCE);
	if(j_camps == NULL) {
		// Nothing.
		return;
	}

	// find dialing info
	size = ast_json_array_size(j_camps);
	for(i = 0; i < size; i++) {
		j_camp = ast_json_array_get(j_camps, i);
		ast_log(LOG_DEBUG, "Force stop campaign info. camp_uuid[%s], camp_name[%s]\n",
				ast_json_string_get(ast_json_object_get(j_camp, "uuid")),
				ast_json_string_get(ast_json_object_get(j_camp, "name"))
				);

		iter = rb_dialing_iter_init();
		while(1) {
			dialing = rb_dialing_iter_next(&iter);
			if(dialing == NULL) {
				break;
			}

			tmp_const = ast_json_string_get(ast_json_object_get(dialing->j_dialing, "camp_uuid"));
			if(tmp_const == NULL) {
				continue;
			}

			if(strcmp(tmp_const, ast_json_string_get(ast_json_object_get(j_camp, "uuid"))) != 0) {
				continue;
			}

			// hang up the channel
			ast_log(LOG_NOTICE, "Hangup channel. uuid[%s], channel[%s]\n",
					dialing->uuid,
					ast_json_string_get(ast_json_object_get(dialing->j_event, "channel"))
					);
			ami_cmd_hangup(ast_json_string_get(ast_json_object_get(dialing->j_event, "channel")), AST_CAUSE_NORMAL_CLEARING);
		}
		rb_dialing_iter_destroy(&iter);

		// update status to stop
		update_campaign_status(ast_json_string_get(ast_json_object_get(j_camp, "uuid")), E_CAMP_STOP);
	}
	AST_JSON_UNREF(j_camps);
}


///**
// * Check Stopping status campaign, and update to stop.
// * @param fd
// * @param event
// * @param arg
// */
//static void cb_campaign_schedule_stopping(__attribute__((unused)) int fd, __attribute__((unused)) short event, __attribute__((unused)) void *arg)
//{
//	struct ast_json* j_camps;
//	struct ast_json* j_camp;
//	int i;
//	int size;
//	int ret;
//
//	j_camps = get_campaigns_by_status(E_CAMP_SCHEDULE_STOPPING);
//	if(j_camps == NULL) {
//		// Nothing.
//		return;
//	}
//
//	size = ast_json_array_size(j_camps);
//	for(i = 0; i < size; i++) {
//		j_camp = ast_json_array_get(j_camps, i);
//
//		// check stoppable campaign
//		ret = is_stoppable_campgain(j_camp);
//		if(ret == false) {
//			continue;
//		}
//
//		// update status to stop
//		ast_log(LOG_NOTICE, "Update campaign status to schedule stop. camp_uuid[%s], camp_name[%s]\n",
//				ast_json_string_get(ast_json_object_get(j_camp, "uuid")),
//				ast_json_string_get(ast_json_object_get(j_camp, "name"))
//				);
//		ret = update_campaign_status(ast_json_string_get(ast_json_object_get(j_camp, "uuid")), E_CAMP_SCHEDULE_STOP);
//		if(ret == false) {
//			ast_log(LOG_ERROR, "Could not update campaign status to schedule_stop. camp_uuid[%s], camp_name[%s]\n",
//				ast_json_string_get(ast_json_object_get(j_camp, "uuid")),
//				ast_json_string_get(ast_json_object_get(j_camp, "name"))
//				);
//		}
//	}
//
//	AST_JSON_UNREF(j_camps);
//
//}

static void cb_check_dialing_end(__attribute__((unused)) int fd, __attribute__((unused)) short event, __attribute__((unused)) void *arg)
{
	struct ao2_iterator iter;
	rb_dialing* dialing;
	struct ast_json* j_tmp;
	int ret;
	char* timestamp;

	iter = rb_dialing_iter_init();
	while(1) {
		dialing = rb_dialing_iter_next(&iter);
		if(dialing == NULL) {
			break;
		}

		if(dialing->status != E_DIALING_HANGUP) {
			continue;
		}

		// create dl_list for update
		timestamp = get_utc_timestamp();
		j_tmp = ast_json_pack("{s:s, s:i, s:O, s:O, s:O, s:s}",
				"uuid",				 ast_json_string_get(ast_json_object_get(dialing->j_dialing, "dl_list_uuid")),
				"status",			   E_DL_IDLE,
				"dialing_uuid",		 ast_json_null(),
				"dialing_camp_uuid",	ast_json_null(),
				"dialing_plan_uuid",	ast_json_null(),
				"tm_last_hangup",			timestamp
				);
		ast_json_object_set(j_tmp, "res_hangup", ast_json_ref(ast_json_object_get(dialing->j_dialing, "res_hangup")));
		ast_json_object_set(j_tmp, "res_dial", ast_json_ref(ast_json_object_get(dialing->j_dialing, "res_dial")));
		ast_free(timestamp);
		if(j_tmp == NULL) {
			ast_log(LOG_ERROR, "Could not create update dl_list json. dl_list_uuid[%s], res_hangup[%"PRIdMAX"], res_dial[%"PRIdMAX"]\n",
					ast_json_string_get(ast_json_object_get(dialing->j_dialing, "dl_list_uuid")),
					ast_json_integer_get(ast_json_object_get(dialing->j_dialing, "res_hangup")),
					ast_json_integer_get(ast_json_object_get(dialing->j_dialing, "res_dial"))
					);
		}

		// update dl_list
		ret = update_dl_list(j_tmp);
		AST_JSON_UNREF(j_tmp);
		if(ret == false) {
			ast_log(LOG_WARNING, "Could not update dialing result. dialing_uuid[%s], dl_list_uuid[%s]\n",
					dialing->uuid, ast_json_string_get(ast_json_object_get(dialing->j_dialing, "dl_list_uuid")));
			continue;
		}

		// create result data
		j_tmp = create_json_for_dl_result(dialing);
		ast_log(LOG_DEBUG, "Check result value. dial_channel[%s], dial_addr[%s], dial_index[%"PRIdMAX"], dial_trycnt[%"PRIdMAX"], dial_timeout[%"PRIdMAX"], dial_type[%"PRIdMAX"], dial_exten[%s], res_dial[%"PRIdMAX"], res_hangup[%"PRIdMAX"], res_hangup_detail[%s]\n",

				// dial
				ast_json_string_get(ast_json_object_get(j_tmp, "dial_channel")),
				ast_json_string_get(ast_json_object_get(j_tmp, "dial_addr")),
				ast_json_integer_get(ast_json_object_get(j_tmp, "dial_index")),
				ast_json_integer_get(ast_json_object_get(j_tmp, "dial_trycnt")),
				ast_json_integer_get(ast_json_object_get(j_tmp, "dial_timeout")),
				ast_json_integer_get(ast_json_object_get(j_tmp, "dial_type")),
				ast_json_string_get(ast_json_object_get(j_tmp, "dial_exten")),

				// result
				ast_json_integer_get(ast_json_object_get(j_tmp, "res_dial")),
				ast_json_integer_get(ast_json_object_get(j_tmp, "res_hangup")),
				ast_json_string_get(ast_json_object_get(j_tmp, "res_hangup_detail"))
				);

//		db_insert("dl_result", j_tmp);
		ret = write_result_json(j_tmp);
		AST_JSON_UNREF(j_tmp);
		if(ret == false) {
			ast_log(LOG_ERROR, "Could not write result correctly.\n");
			continue;
		}

		rb_dialing_destory(dialing);
		ast_log(LOG_DEBUG, "Destroyed dialing info.\n");

	}
	ao2_iterator_destroy(&iter);

	return;
}

static void cb_check_dialing_error(__attribute__((unused)) int fd, __attribute__((unused)) short event, __attribute__((unused)) void *arg)
{
	struct ao2_iterator iter;
	rb_dialing* dialing;
	struct ast_json* j_tmp;
	int ret;
	char* timestamp;

	iter = rb_dialing_iter_init();
	while(1) {
		dialing = rb_dialing_iter_next(&iter);
		if(dialing == NULL) {
			break;
		}

		if(dialing->status != E_DIALING_ERROR) {
			continue;
		}


		// create dl_list for update
		timestamp = get_utc_timestamp();
		j_tmp = ast_json_pack("{s:s, s:i, s:O, s:O, s:O, s:s}",
				"uuid",				 ast_json_string_get(ast_json_object_get(dialing->j_dialing, "dl_list_uuid")),
				"status",			   E_DL_IDLE,
				"dialing_uuid",		 ast_json_null(),
				"dialing_camp_uuid",	ast_json_null(),
				"dialing_plan_uuid",	ast_json_null(),
				"tm_last_hangup",			timestamp
				);
		ast_json_object_set(j_tmp, "res_hangup", ast_json_ref(ast_json_object_get(dialing->j_dialing, "res_hangup")));
		ast_json_object_set(j_tmp, "res_dial", ast_json_ref(ast_json_object_get(dialing->j_dialing, "res_dial")));
		ast_free(timestamp);
		if(j_tmp == NULL) {
			ast_log(LOG_ERROR, "Could not create update dl_list json. dl_list_uuid[%s], res_hangup[%"PRIdMAX"], res_dial[%"PRIdMAX"]\n",
					ast_json_string_get(ast_json_object_get(dialing->j_dialing, "dl_list_uuid")),
					ast_json_integer_get(ast_json_object_get(dialing->j_dialing, "res_hangup")),
					ast_json_integer_get(ast_json_object_get(dialing->j_dialing, "res_dial"))
					);
		}

		// update dl_list
		ret = update_dl_list(j_tmp);
		AST_JSON_UNREF(j_tmp);
		if(ret == false) {
			ast_log(LOG_WARNING, "Could not update dialing result. dialing_uuid[%s], dl_list_uuid[%s]\n",
					dialing->uuid, ast_json_string_get(ast_json_object_get(dialing->j_dialing, "dl_list_uuid")));
			continue;
		}

		// create result data
		j_tmp = create_json_for_dl_result(dialing);
		ast_log(LOG_DEBUG, "Check result value. dial_channel[%s], dial_addr[%s], dial_index[%"PRIdMAX"], dial_trycnt[%"PRIdMAX"], dial_timeout[%"PRIdMAX"], dial_type[%"PRIdMAX"], dial_exten[%s], res_dial[%"PRIdMAX"], res_hangup[%"PRIdMAX"], res_hangup_detail[%s]\n",

				// dial
				ast_json_string_get(ast_json_object_get(j_tmp, "dial_channel")),
				ast_json_string_get(ast_json_object_get(j_tmp, "dial_addr")),
				ast_json_integer_get(ast_json_object_get(j_tmp, "dial_index")),
				ast_json_integer_get(ast_json_object_get(j_tmp, "dial_trycnt")),
				ast_json_integer_get(ast_json_object_get(j_tmp, "dial_timeout")),
				ast_json_integer_get(ast_json_object_get(j_tmp, "dial_type")),
				ast_json_string_get(ast_json_object_get(j_tmp, "dial_exten")),

				// result
				ast_json_integer_get(ast_json_object_get(j_tmp, "res_dial")),
				ast_json_integer_get(ast_json_object_get(j_tmp, "res_hangup")),
				ast_json_string_get(ast_json_object_get(j_tmp, "res_hangup_detail"))
				);

		ret = write_result_json(j_tmp);
		AST_JSON_UNREF(j_tmp);

		rb_dialing_destory(dialing);
		ast_log(LOG_DEBUG, "Destroyed!\n");

	}
	ao2_iterator_destroy(&iter);

	return;
}

/**
 * Check ended campaign. If the campaign is end-able, set the status to STOPPING.
 * \param fd
 * \param event
 * \param arg
 */
static void cb_check_campaign_end(__attribute__((unused)) int fd, __attribute__((unused)) short event, __attribute__((unused)) void *arg)
{
	struct ast_json* j_camps;
	struct ast_json* j_camp;
	struct ast_json* j_plan;
	struct ast_json* j_dlma;
	int i;
	int size;
	int ret;

	j_camps = get_campaigns_by_status(E_CAMP_START);
	size = ast_json_array_size(j_camps);
	for(i = 0; i < size; i++) {
		j_camp = ast_json_array_get(j_camps, i);
		if(j_camp == NULL) {
			continue;
		}

		j_plan = get_plan(ast_json_string_get(ast_json_object_get(j_camp, "plan")));
		if(j_plan == NULL) {
			update_campaign_status(ast_json_string_get(ast_json_object_get(j_camp, "uuid")), E_CAMP_STOPPING);
			continue;
		}

		j_dlma = get_dlma(ast_json_string_get(ast_json_object_get(j_camp, "dlma")));
		if(j_dlma == NULL) {
			update_campaign_status(ast_json_string_get(ast_json_object_get(j_camp, "uuid")), E_CAMP_STOPPING);
			AST_JSON_UNREF(j_plan);
			continue;
		}

		// check end-able.
		ret = is_endable_plan(j_plan);
		ret &= is_endable_dl_list(j_dlma, j_plan);
		if(ret == true) {
			ast_log(LOG_NOTICE, "The campaign ended. Stopping campaign. uuid[%s], name[%s]\n",
					ast_json_string_get(ast_json_object_get(j_camp, "uuid")),
					ast_json_string_get(ast_json_object_get(j_camp, "name"))
					);
			update_campaign_status(ast_json_string_get(ast_json_object_get(j_camp, "uuid")), E_CAMP_STOPPING);
		}
		AST_JSON_UNREF(j_plan);
		AST_JSON_UNREF(j_dlma);
	}
	AST_JSON_UNREF(j_camps);
	return;
}

/**
 * Check campaign schedule. If the campaign is start-able, set the status to STARTING.
 * \param fd
 * \param event
 * \param arg
 */
static void cb_check_campaign_schedule_start(__attribute__((unused)) int fd, __attribute__((unused)) short event, __attribute__((unused)) void *arg)
{
	struct ast_json* j_camps;
	struct ast_json* j_camp;
	int size;
	int i;
	int ret;

	j_camps = get_campaigns_schedule_start();
	size = ast_json_array_size(j_camps);
	for(i = 0; i < size; i++) {
		j_camp = ast_json_array_get(j_camps, i);
		if(j_camp == NULL) {
			continue;
		}

		ast_log(LOG_NOTICE, "Update campaign status to starting by scheduling. camp_uuid[%s], camp_name[%s]\n",
				ast_json_string_get(ast_json_object_get(j_camp, "uuid"))? : "",
				ast_json_string_get(ast_json_object_get(j_camp, "name"))? : ""
				);
		ret = update_campaign_status(ast_json_string_get(ast_json_object_get(j_camp, "uuid")), E_CAMP_STARTING);
		if(ret == false) {
			ast_log(LOG_ERROR, "Could not update campaign status to starting. camp_uuid[%s], camp_name[%s]\n",
					ast_json_string_get(ast_json_object_get(j_camp, "uuid"))? : "",
					ast_json_string_get(ast_json_object_get(j_camp, "name"))? : ""
					);
		}
	}

	AST_JSON_UNREF(j_camps);
}

/**
 * Check campaign schedule. If the campaign is end-able, set the status to STOPPING.
 * \param fd
 * \param event
 * \param arg
 */
static void cb_check_campaign_schedule_end(__attribute__((unused)) int fd, __attribute__((unused)) short event, __attribute__((unused)) void *arg)
{
	struct ast_json* j_camps;
	struct ast_json* j_camp;
	int size;
	int i;
	int ret;

	j_camps = get_campaigns_schedule_end();
	size = ast_json_array_size(j_camps);
	for(i = 0; i < size; i++) {
		j_camp = ast_json_array_get(j_camps, i);
		if(j_camp == NULL) {
			continue;
		}

		ast_log(LOG_NOTICE, "Update campaign status to stopping by scheduling. camp_uuid[%s], camp_name[%s]\n",
				ast_json_string_get(ast_json_object_get(j_camp, "uuid")),
				ast_json_string_get(ast_json_object_get(j_camp, "name"))
				);
		ret = update_campaign_status(ast_json_string_get(ast_json_object_get(j_camp, "uuid")), E_CAMP_STOPPING);
		if(ret == false) {
			ast_log(LOG_ERROR, "Could not update campaign status to schedule_stopping. camp_uuid[%s], camp_name[%s]\n",
					ast_json_string_get(ast_json_object_get(j_camp, "uuid")),
					ast_json_string_get(ast_json_object_get(j_camp, "name"))
					);
		}
	}

	AST_JSON_UNREF(j_camps);
}



/**
 *
 * @param j_camp
 * @param j_plan
 */
static void dial_desktop(const struct ast_json* j_camp, const struct ast_json* j_plan, const struct ast_json* j_dlma)
{
	return;
}

/**
 *
 * @param j_camp
 * @param j_plan
 */
static void dial_power(const struct ast_json* j_camp, const struct ast_json* j_plan, const struct ast_json* j_dlma)
{
	return;
}

/**
 *  Make a call by predictive algorithms.
 *  Currently, just consider ready agent only.
 * @param j_camp	campaign info
 * @param j_plan	plan info
 * @param j_dlma	dial list master info
 */
static void dial_predictive(struct ast_json* j_camp, struct ast_json* j_plan, struct ast_json* j_dlma, struct ast_json* j_dest)
{
	int ret;
	struct ast_json* j_dl_list;
	struct ast_json* j_dial;
	struct ast_json* j_res;
	rb_dialing* dialing;
	char* tmp;
	E_DESTINATION_TYPE dial_type;

	// get dl_list info to dial.
	j_dl_list = get_dl_available_predictive(j_dlma, j_plan);
	if(j_dl_list == NULL) {
		// No available list
//		ast_log(LOG_VERBOSE, "No more dialing list. stopping the campaign.\n");
//		update_campaign_info_status(ast_json_string_get(ast_json_object_get(j_camp, "uuid")), E_CAMP_STOPPING);
		return;
	}

	// check available outgoing call.
	ret = check_dial_avaiable_predictive(j_camp, j_plan, j_dlma, j_dest);
	if(ret == -1) {
		// something was wrong. stop the campaign.
		update_campaign_status(ast_json_string_get(ast_json_object_get(j_camp, "uuid")), E_CAMP_STOPPING);
		AST_JSON_UNREF(j_dl_list);
		return;
	}
	else if(ret == 0) {
		// Too much calls already outgoing.
		AST_JSON_UNREF(j_dl_list);
		return;
	}

	// creating dialing info
	j_dial = create_dial_info(j_plan, j_dl_list, j_dest);
	if(j_dial == NULL) {
		AST_JSON_UNREF(j_dl_list);
		ast_log(LOG_DEBUG, "Could not create dialing info.");
		return;
	}
	ast_log(LOG_NOTICE, "Originating. camp_uuid[%s], camp_name[%s], channel[%s], chan_id[%s], timeout[%"PRIdMAX"], dial_index[%"PRIdMAX"], dial_trycnt[%"PRIdMAX"], dial_type[%"PRIdMAX"]\n",
			ast_json_string_get(ast_json_object_get(j_camp, "uuid")),
			ast_json_string_get(ast_json_object_get(j_camp, "name")),
			ast_json_string_get(ast_json_object_get(j_dial, "dial_channel")),
			ast_json_string_get(ast_json_object_get(j_dial, "channelid")),
			ast_json_integer_get(ast_json_object_get(j_dial, "dial_timeout")),
			ast_json_integer_get(ast_json_object_get(j_dial, "dial_index")),
			ast_json_integer_get(ast_json_object_get(j_dial, "dial_trycnt")),
			ast_json_integer_get(ast_json_object_get(j_dial, "dial_type"))
			);

	// create rbtree
	dialing = rb_dialing_create(
			ast_json_string_get(ast_json_object_get(j_dial, "channelid")),
			j_camp,
			j_plan,
			j_dlma,
			j_dest,
			j_dl_list,
			j_dial
			);
	AST_JSON_UNREF(j_dl_list);
	if(dialing == NULL) {
		ast_log(LOG_WARNING, "Could not create rbtree object.\n");
		AST_JSON_UNREF(j_dial);
		return;
	}

	// update dl list using dialing info
	ret = update_dl_list_after_create_dialing_info(dialing);
	if(ret == false) {
		rb_dialing_destory(dialing);
		clear_dl_list_dialing(ast_json_string_get(ast_json_object_get(dialing->j_dialing, "dl_list_uuid")));
		ast_log(LOG_ERROR, "Could not update dial list info.\n");
		return;
	}

	// dial to customer
	dial_type = ast_json_integer_get(ast_json_object_get(j_dial, "dial_type"));
	switch(dial_type) {
		case DESTINATION_EXTEN: {
			j_res = ami_cmd_originate_to_exten(j_dial);
			AST_JSON_UNREF(j_dial);
		}
		break;

		case DESTINATION_APPLICATION: {
			j_res = ami_cmd_originate_to_application(j_dial);
			AST_JSON_UNREF(j_dial);
		}
		break;

		default: {
			AST_JSON_UNREF(j_dial);
			ast_log(LOG_ERROR, "Unsupported dialing type.");
			clear_dl_list_dialing(ast_json_string_get(ast_json_object_get(dialing->j_dialing, "dl_list_uuid")));
			rb_dialing_destory(dialing);
			return;
		}
		break;
	}

	if(j_res == NULL) {
		ast_log(LOG_WARNING, "Originating has been failed.\n");
		clear_dl_list_dialing(ast_json_string_get(ast_json_object_get(dialing->j_dialing, "dl_list_uuid")));
		rb_dialing_destory(dialing);
		return;
	}

	tmp = ast_json_dump_string_format(j_res, 0);
	ast_log(LOG_DEBUG, "Check value. tmp[%s]\n", tmp);
	ast_json_free(tmp);
	AST_JSON_UNREF(j_res);

	// update dialing status
	rb_dialing_update_status(dialing, E_DIALING_ORIGINATE_REQUEST);

	return;
}

/**
 *
 * @param j_camp
 * @param j_plan
 */
static void dial_robo(const struct ast_json* j_camp, const struct ast_json* j_plan, const struct ast_json* j_dlma)
{
	return;
}

/**
 *  Redirect call to other dialplan.
 * @param j_camp	campaign info
 * @param j_plan	plan info
 * @param j_dlma	dial list master info
 */
static void dial_redirect(const struct ast_json* j_camp, const struct ast_json* j_plan, const struct ast_json* j_dlma)
{
	return;
}




/**
 * Return dialing availability.
 * todo: need something more here.. currently, just compare dial numbers..
 * If can dialing returns true, if not returns false.
 * @param j_camp
 * @param j_plan
 * @return 1:OK, 0:NO, -1:ERROR
 */
static int check_dial_avaiable_predictive(
		struct ast_json* j_camp,
		struct ast_json* j_plan,
		struct ast_json* j_dlma,
		struct ast_json* j_dest
		)
{
	int cnt_current_dialing;
	int plan_service_level;
	int cnt_avail;
	int ret;

	// get available destination count
	cnt_avail = get_destination_available_count(j_dest);
	if(cnt_avail == DEF_DESTINATION_AVAIL_CNT_UNLIMITED) {
		ast_log(LOG_DEBUG, "Available destination count is unlimited. cnt[%d]\n", cnt_avail);
		return 1;
	}
	ast_log(LOG_DEBUG, "Available destination count. cnt[%d]\n", cnt_avail);

	// get service level
	plan_service_level = ast_json_integer_get(ast_json_object_get(j_plan, "service_level"));
	if(plan_service_level < 0) {
		plan_service_level = 0;
	}
	ast_log(LOG_DEBUG, "Service level. level[%d]\n", plan_service_level);

	// get current dialing count
	cnt_current_dialing = rb_dialing_get_count_by_camp_uuid(ast_json_string_get(ast_json_object_get(j_camp, "uuid")));
	if(cnt_current_dialing == -1) {
		ast_log(LOG_ERROR, "Could not get current dialing count info. camp_uuid[%s]\n",
				ast_json_string_get(ast_json_object_get(j_camp, "uuid"))
				);
		return -1;
	}
	ast_log(LOG_DEBUG, "Current dialing count. count[%d]\n", cnt_current_dialing);

	ret = (cnt_avail + plan_service_level) - cnt_current_dialing;

	if(ret <= 0) {
		return 0;
	}

	return 1;
}

static bool write_result_json(struct ast_json* j_res)
{
	FILE* fp;
	struct ast_json* j_general;
	const char* filename;
	char* tmp;

	if(j_res == NULL) {
		ast_log(LOG_ERROR, "Wrong input parameter.\n");
		return false;
	}

	// open json file
	j_general = ast_json_object_get(g_app->j_conf, "general");
	filename = ast_json_string_get(ast_json_object_get(j_general, "result_filename"));
	if(filename == NULL) {
		ast_log(LOG_ERROR, "Could not get option value. option[%s]\n", "result_filename");
		return false;
	}

	// open file
	fp = fopen(filename, "a");
	if(fp == NULL) {
		ast_log(LOG_ERROR, "Could not open result file. filename[%s], err[%s]\n",
				filename, strerror(errno)
				);
		return false;
	}

	tmp = ast_json_dump_string_format(j_res, AST_JSON_COMPACT);
	if(tmp == NULL) {
		ast_log(LOG_ERROR, "Could not get result string to the file. filename[%s], err[%s]\n",
				filename, strerror(errno)
				);
		fclose(fp);
		return false;
	}

	fprintf(fp, "%s\n", tmp);
	ast_json_free(tmp);
	fclose(fp);

	ast_log(LOG_VERBOSE, "Result write succeed.\n");

	return true;
}
