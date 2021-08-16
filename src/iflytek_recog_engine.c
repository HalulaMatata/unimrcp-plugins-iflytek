/*
 * Copyright 2021-2031 pengc
 *
 * Use iFLYTEK SDK
 *
 */

/* 
 * Mandatory rules concerning plugin implementation.
 * 1. Each plugin MUST implement a plugin/engine creator function
 *    with the exact signature and name (the main entry point)
 *        MRCP_PLUGIN_DECLARE(mrcp_engine_t*) mrcp_plugin_create(apr_pool_t *pool)
 * 2. Each plugin MUST declare its version number
 *        MRCP_PLUGIN_VERSION_DECLARE
 * 3. One and only one response MUST be sent back to the received request.
 * 4. Methods (callbacks) of the MRCP engine channel MUST not block.
 *   (asynchronous response can be sent from the context of other thread)
 * 5. Methods (callbacks) of the MPF engine stream MUST not block.
 */

#include "mrcp_recog_engine.h"
#include "mpf_activity_detector.h"
#include "apt_consumer_task.h"
#include "apt_log.h"

#ifdef _GNU_SOURCE
#include <unistd.h>
#else
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#define gettid() syscall(SYS_gettid)
#endif

#define RECOG_ENGINE_TASK_NAME "iFLYTEK Recog Engine"

typedef struct iflytek_recog_engine_t iflytek_recog_engine_t;
typedef struct iflytek_recog_channel_t iflytek_recog_channel_t;
typedef struct iflytek_recog_msg_t iflytek_recog_msg_t;

/** Declaration of recognizer engine methods */
static apt_bool_t iflytek_recog_engine_destroy(mrcp_engine_t *engine);
static apt_bool_t iflytek_recog_engine_open(mrcp_engine_t *engine);
static apt_bool_t iflytek_recog_engine_close(mrcp_engine_t *engine);
static mrcp_engine_channel_t* iflytek_recog_engine_channel_create(mrcp_engine_t *engine, apr_pool_t *pool);

static const struct mrcp_engine_method_vtable_t engine_vtable = {
	iflytek_recog_engine_destroy,
	iflytek_recog_engine_open,
	iflytek_recog_engine_close,
	iflytek_recog_engine_channel_create
};


/** Declaration of recognizer channel methods */
static apt_bool_t iflytek_recog_channel_destroy(mrcp_engine_channel_t *channel);
static apt_bool_t iflytek_recog_channel_open(mrcp_engine_channel_t *channel);
static apt_bool_t iflytek_recog_channel_close(mrcp_engine_channel_t *channel);
static apt_bool_t iflytek_recog_channel_request_process(mrcp_engine_channel_t *channel, mrcp_message_t *request);

static const struct mrcp_engine_channel_method_vtable_t channel_vtable = {
	iflytek_recog_channel_destroy,
	iflytek_recog_channel_open,
	iflytek_recog_channel_close,
	iflytek_recog_channel_request_process
};

/** Declaration of recognizer audio stream methods */
static apt_bool_t iflytek_recog_stream_destroy(mpf_audio_stream_t *stream);
static apt_bool_t iflytek_recog_stream_open(mpf_audio_stream_t *stream, mpf_codec_t *codec);
static apt_bool_t iflytek_recog_stream_close(mpf_audio_stream_t *stream);
static apt_bool_t iflytek_recog_stream_write(mpf_audio_stream_t *stream, const mpf_frame_t *frame);

static const mpf_audio_stream_vtable_t audio_stream_vtable = {
	iflytek_recog_stream_destroy,
	NULL,
	NULL,
	NULL,
	iflytek_recog_stream_open,
	iflytek_recog_stream_close,
	iflytek_recog_stream_write,
	NULL
};

/** Declaration of demo recognizer engine */
struct iflytek_recog_engine_t {
	apt_consumer_task_t    *task;
};

/** Declaration of demo recognizer channel */
struct iflytek_recog_channel_t {
	/** Back pointer to engine */
	iflytek_recog_engine_t     *demo_engine;
	/** Engine channel base */
	mrcp_engine_channel_t   *channel;

	/** Active (in-progress) recognition request */
	mrcp_message_t          *recog_request;
	/** Pending stop response */
	mrcp_message_t          *stop_response;
	/** Indicates whether input timers are started */
	apt_bool_t               timers_started;
	/** Voice activity detector */
	mpf_activity_detector_t *detector;
	/** File to write utterance to */
	FILE                    *audio_out;
};

typedef enum {
	IFLYTEK_RECOG_MSG_OPEN_CHANNEL,
	IFLYTEK_RECOG_MSG_CLOSE_CHANNEL,
	IFLYTEK_RECOG_MSG_REQUEST_PROCESS
} iflytek_recog_msg_type_e;

/** Declaration of demo recognizer task message */
struct iflytek_recog_msg_t {
	iflytek_recog_msg_type_e  type;
	mrcp_engine_channel_t *channel; 
	mrcp_message_t        *request;
};

static apt_bool_t iflytek_recog_msg_signal(iflytek_recog_msg_type_e type, mrcp_engine_channel_t *channel, mrcp_message_t *request);
static apt_bool_t iflytek_recog_msg_process(apt_task_t *task, apt_task_msg_t *msg);

/** Declare this macro to set plugin version */
MRCP_PLUGIN_VERSION_DECLARE

/**
 * Declare this macro to use log routine of the server, plugin is loaded from.
 * Enable/add the corresponding entry in logger.xml to set a cutsom log source priority.
 *    <source name="RECOG-PLUGIN" priority="DEBUG" masking="NONE"/>
 */
MRCP_PLUGIN_LOG_SOURCE_IMPLEMENT(RECOG_PLUGIN,"RECOG-PLUGIN")

/** Use custom log source mark */
#define RECOG_LOG_MARK   APT_LOG_MARK_DECLARE(RECOG_PLUGIN)

/** Create demo recognizer engine */
MRCP_PLUGIN_DECLARE(mrcp_engine_t*) mrcp_plugin_create(apr_pool_t *pool)
{
	iflytek_recog_engine_t *demo_engine = apr_palloc(pool,sizeof(iflytek_recog_engine_t));
	apt_task_t *task;
	apt_task_vtable_t *vtable;
	apt_task_msg_pool_t *msg_pool;

	msg_pool = apt_task_msg_pool_create_dynamic(sizeof(iflytek_recog_msg_t),pool);
	demo_engine->task = apt_consumer_task_create(demo_engine,msg_pool,pool);
	if(!demo_engine->task) {
		return NULL;
	}
	task = apt_consumer_task_base_get(demo_engine->task);
	apt_task_name_set(task,RECOG_ENGINE_TASK_NAME);
	vtable = apt_task_vtable_get(task);
	if(vtable) {
		vtable->process_msg = iflytek_recog_msg_process;
	}

	apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"plugin [%s][%d] load", RECOG_ENGINE_TASK_NAME, gettid());
	/* create engine base */
	return mrcp_engine_create(
				MRCP_RECOGNIZER_RESOURCE,  /* MRCP resource identifier */
				demo_engine,               /* object to associate */
				&engine_vtable,            /* virtual methods table of engine */
				pool);                     /* pool to allocate memory from */
}

/** Destroy recognizer engine */
static apt_bool_t iflytek_recog_engine_destroy(mrcp_engine_t *engine)
{
	iflytek_recog_engine_t *demo_engine = engine->obj;
	if(demo_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(demo_engine->task);
		apt_task_destroy(task);
		demo_engine->task = NULL;
	}
	apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"plugin [%s][%d] unload", RECOG_ENGINE_TASK_NAME, gettid());
	return TRUE;
}

/** Open recognizer engine */
static apt_bool_t iflytek_recog_engine_open(mrcp_engine_t *engine)
{
	iflytek_recog_engine_t *demo_engine = engine->obj;
	if(demo_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(demo_engine->task);
		apt_task_start(task);
	}

	apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"plugin [%s][%d] engine open", RECOG_ENGINE_TASK_NAME, gettid());
	return mrcp_engine_open_respond(engine,TRUE);
}

/** Close recognizer engine */
static apt_bool_t iflytek_recog_engine_close(mrcp_engine_t *engine)
{
	iflytek_recog_engine_t *demo_engine = engine->obj;
	if(demo_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(demo_engine->task);
		apt_task_terminate(task,TRUE);
	}
	apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"plugin [%s][%d] engine close", RECOG_ENGINE_TASK_NAME, gettid());
	return mrcp_engine_close_respond(engine);
}

static mrcp_engine_channel_t* iflytek_recog_engine_channel_create(mrcp_engine_t *engine, apr_pool_t *pool)
{
	mpf_stream_capabilities_t *capabilities;
	mpf_termination_t *termination; 

	/* create demo recog channel */
	iflytek_recog_channel_t *recog_channel = apr_palloc(pool,sizeof(iflytek_recog_channel_t));
	recog_channel->demo_engine = engine->obj;
	recog_channel->recog_request = NULL;
	recog_channel->stop_response = NULL;
	recog_channel->detector = mpf_activity_detector_create(pool);
	recog_channel->audio_out = NULL;

	capabilities = mpf_sink_stream_capabilities_create(pool);
	mpf_codec_capabilities_add(
			&capabilities->codecs,
			MPF_SAMPLE_RATE_8000 | MPF_SAMPLE_RATE_16000,
			"LPCM");

	/* create media termination */
	termination = mrcp_engine_audio_termination_create(
			recog_channel,        /* object to associate */
			&audio_stream_vtable, /* virtual methods table of audio stream */
			capabilities,         /* stream capabilities */
			pool);                /* pool to allocate memory from */

	/* create engine channel base */
	recog_channel->channel = mrcp_engine_channel_create(
			engine,               /* engine */
			&channel_vtable,      /* virtual methods table of engine channel */
			recog_channel,        /* object to associate */
			termination,          /* associated media termination */
			pool);                /* pool to allocate memory from */

	apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"plugin [%s][%d] create channel", RECOG_ENGINE_TASK_NAME, gettid());
	return recog_channel->channel;
}

/** Destroy engine channel */
static apt_bool_t iflytek_recog_channel_destroy(mrcp_engine_channel_t *channel)
{
	/* nothing to destrtoy */
	apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"plugin [%s][%d] destroy channel", RECOG_ENGINE_TASK_NAME, gettid());
	return TRUE;
}

/** Open engine channel (asynchronous response MUST be sent)*/
static apt_bool_t iflytek_recog_channel_open(mrcp_engine_channel_t *channel)
{
#if PLUGIN_VERSION_AT_LEAST(1,7,0)
	if(channel->attribs) {
		/* process attributes */
		const apr_array_header_t *header = apr_table_elts(channel->attribs);
		apr_table_entry_t *entry = (apr_table_entry_t *)header->elts;
		int i;
		for(i=0; i<header->nelts; i++) {
			apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Attrib name [%s] value [%s]",entry[i].key,entry[i].val);
		}
	}
#endif
	apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"plugin [%s][%d] channel open", RECOG_ENGINE_TASK_NAME, gettid());
	return iflytek_recog_msg_signal(IFLYTEK_RECOG_MSG_OPEN_CHANNEL,channel,NULL);
}

/** Close engine channel (asynchronous response MUST be sent)*/
static apt_bool_t iflytek_recog_channel_close(mrcp_engine_channel_t *channel)
{
	apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"plugin [%s][%d] channel close", RECOG_ENGINE_TASK_NAME, gettid());
	return iflytek_recog_msg_signal(IFLYTEK_RECOG_MSG_CLOSE_CHANNEL,channel,NULL);
}

/** Process MRCP channel request (asynchronous response MUST be sent)*/
static apt_bool_t iflytek_recog_channel_request_process(mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
	apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"plugin [%s][%d] channel request process", RECOG_ENGINE_TASK_NAME, gettid());
	return iflytek_recog_msg_signal(IFLYTEK_RECOG_MSG_REQUEST_PROCESS,channel,request);
}

/** Process RECOGNIZE request */
static apt_bool_t iflytek_recog_channel_recognize(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	/* process RECOGNIZE request */
	mrcp_recog_header_t *recog_header;
	iflytek_recog_channel_t *recog_channel = channel->method_obj;
	const mpf_codec_descriptor_t *descriptor = mrcp_engine_sink_stream_codec_get(channel);

	if(!descriptor) {
		apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"Failed to Get Codec Descriptor " APT_SIDRES_FMT, MRCP_MESSAGE_SIDRES(request));
		response->start_line.status_code = MRCP_STATUS_CODE_METHOD_FAILED;
		return FALSE;
	}

	recog_channel->timers_started = TRUE;

	/* get recognizer header */
	recog_header = mrcp_resource_header_get(request);
	if(recog_header) {
		if(mrcp_resource_header_property_check(request,RECOGNIZER_HEADER_START_INPUT_TIMERS) == TRUE) {
			recog_channel->timers_started = recog_header->start_input_timers;
		}
		if(mrcp_resource_header_property_check(request,RECOGNIZER_HEADER_NO_INPUT_TIMEOUT) == TRUE) {
			mpf_activity_detector_noinput_timeout_set(recog_channel->detector,recog_header->no_input_timeout);
		}
		if(mrcp_resource_header_property_check(request,RECOGNIZER_HEADER_SPEECH_COMPLETE_TIMEOUT) == TRUE) {
			mpf_activity_detector_silence_timeout_set(recog_channel->detector,recog_header->speech_complete_timeout);
		}
		if (mrcp_resource_header_property_check(request,RECOGNIZER_HEADER_SENSITIVITY_LEVEL) == TRUE) {
			mpf_activity_detector_level_set(recog_channel->detector,  (apr_size_t)(recog_header->sensitivity_level+0.5));
		}
		apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"plugin [%s][%d] recognize params: "
				"no_input_timeout - %d, silence_timeout - %d, sensitivity_level - %f", RECOG_ENGINE_TASK_NAME, gettid(),
				recog_header->no_input_timeout, recog_header->speech_complete_timeout, recog_header->sensitivity_level);
	}

	/* add by pengc */
	mrcp_generic_header_t *generic_header;
	generic_header = mrcp_generic_header_get(request);
	if (generic_header) {
		if (mrcp_generic_header_property_check(request, GENERIC_HEADER_VENDOR_SPECIFIC_PARAMS) == TRUE) {
			const apt_pair_t *pair;
			int size = 0, id = 0;
			size = apt_pair_array_size_get(generic_header->vendor_specific_params);
			for (; id < size; id ++) {
				pair = apt_pair_array_get(generic_header->vendor_specific_params, id);
				if (pair) {
					apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"plugin [%s][%d] recognize params: generic_header: %s - %s",
						RECOG_ENGINE_TASK_NAME, gettid(), 
						apt_string_buffer_get(&pair->name), 
						apt_string_buffer_get(&pair->value));
				}
			}
		}
		apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"plugin [%s][%d] recognize params: generic_header",
				RECOG_ENGINE_TASK_NAME, gettid());
	}

	if(!recog_channel->audio_out) {
		const apt_dir_layout_t *dir_layout = channel->engine->dir_layout;
		char *file_name = apr_psprintf(channel->pool,"utter-%dkHz-%s.pcm",
							descriptor->sampling_rate/1000,
							request->channel_id.session_id.buf);
		char *file_path = apt_vardir_filepath_get(dir_layout,file_name,channel->pool);
		if(file_path) {
			apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Open Utterance Output File [%s] for Writing",file_path);
			recog_channel->audio_out = fopen(file_path,"wb");
			if(!recog_channel->audio_out) {
				apt_log(RECOG_LOG_MARK,APT_PRIO_WARNING,"Failed to Open Utterance Output File [%s] for Writing",file_path);
			}
		}
	}

	apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"plugin [%s][%d] channel recv recognize", RECOG_ENGINE_TASK_NAME, gettid());
	response->start_line.request_state = MRCP_REQUEST_STATE_INPROGRESS;
	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);
	recog_channel->recog_request = request;
	return TRUE;
}

/** Process STOP request */
static apt_bool_t iflytek_recog_channel_stop(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"plugin [%s][%d] channel stop", RECOG_ENGINE_TASK_NAME, gettid());
	/* process STOP request */
	iflytek_recog_channel_t *recog_channel = channel->method_obj;
	/* store STOP request, make sure there is no more activity and only then send the response */
	recog_channel->stop_response = response;
	return TRUE;
}

/** Process START-INPUT-TIMERS request */
static apt_bool_t iflytek_recog_channel_timers_start(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"plugin [%s][%d] channel recv start-input-request", RECOG_ENGINE_TASK_NAME, gettid());
	iflytek_recog_channel_t *recog_channel = channel->method_obj;
	recog_channel->timers_started = TRUE;
	return mrcp_engine_channel_message_send(channel,response);
}

/** Dispatch MRCP request */
static apt_bool_t iflytek_recog_channel_request_dispatch(mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
	apt_bool_t processed = FALSE;
	mrcp_message_t *response = mrcp_response_create(request,request->pool);
	switch(request->start_line.method_id) {
		case RECOGNIZER_SET_PARAMS:
			break;
		case RECOGNIZER_GET_PARAMS:
			break;
		case RECOGNIZER_DEFINE_GRAMMAR:
			break;
		case RECOGNIZER_RECOGNIZE:
			processed = iflytek_recog_channel_recognize(channel,request,response);
			break;
		case RECOGNIZER_GET_RESULT:
			break;
		case RECOGNIZER_START_INPUT_TIMERS:
			processed = iflytek_recog_channel_timers_start(channel,request,response);
			break;
		case RECOGNIZER_STOP:
			processed = iflytek_recog_channel_stop(channel,request,response);
			break;
		default:
			break;
	}
	if(processed == FALSE) {
		/* send asynchronous response for not handled request */
		mrcp_engine_channel_message_send(channel,response);
		apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"plugin [%s][%d] channel request dispatch others", RECOG_ENGINE_TASK_NAME, gettid());
	}
	return TRUE;
}

/** Callback is called from MPF engine context to destroy any additional data associated with audio stream */
static apt_bool_t iflytek_recog_stream_destroy(mpf_audio_stream_t *stream)
{
	apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"plugin [%s][%d] audio stream destroy", RECOG_ENGINE_TASK_NAME, gettid());
	return TRUE;
}

/** Callback is called from MPF engine context to perform any action before open */
static apt_bool_t iflytek_recog_stream_open(mpf_audio_stream_t *stream, mpf_codec_t *codec)
{
	/*
	 * Add iflytek session
	 * if 0 and log in
	*/
	apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"plugin [%s][%d] audio stream open", RECOG_ENGINE_TASK_NAME, gettid());
	return TRUE;
}

/** Callback is called from MPF engine context to perform any action after close */
static apt_bool_t iflytek_recog_stream_close(mpf_audio_stream_t *stream)
{
	/*
	* Sub iflytek session
	* if 0 and log out
	*/

	apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"plugin [%s][%d] audio stream close", RECOG_ENGINE_TASK_NAME, gettid());
	return TRUE;
}

/* Raise demo START-OF-INPUT event */
static apt_bool_t iflytek_recog_start_of_input(iflytek_recog_channel_t *recog_channel)
{
	/* create START-OF-INPUT event */
	mrcp_message_t *message = mrcp_event_create(
						recog_channel->recog_request,
						RECOGNIZER_START_OF_INPUT,
						recog_channel->recog_request->pool);
	if(!message) {
		return FALSE;
	}
	apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"plugin [%s][%d] channel send start-of-input", RECOG_ENGINE_TASK_NAME, gettid());
	/* set request state */
	message->start_line.request_state = MRCP_REQUEST_STATE_INPROGRESS;
	/* send asynch event */
	return mrcp_engine_channel_message_send(recog_channel->channel,message);
}

/* Load demo recognition result */
static apt_bool_t iflytek_recog_result_load(iflytek_recog_channel_t *recog_channel, mrcp_message_t *message)
{
	FILE *file;
	mrcp_engine_channel_t *channel = recog_channel->channel;
	const apt_dir_layout_t *dir_layout = channel->engine->dir_layout;
	char *file_path = apt_datadir_filepath_get(dir_layout,"result.xml",message->pool);
	if(!file_path) {
		return FALSE;
	}
	
	/* read the demo result from file */
	file = fopen(file_path,"r");
	if(file) {
		mrcp_generic_header_t *generic_header;
		char text[1024];
		apr_size_t size;
		size = fread(text,1,sizeof(text),file);
		apt_string_assign_n(&message->body,text,size,message->pool);
		fclose(file);

		/* get/allocate generic header */
		generic_header = mrcp_generic_header_prepare(message);
		if(generic_header) {
			/* set content types */
			apt_string_assign(&generic_header->content_type,"application/x-nlsml",message->pool);
			mrcp_generic_header_property_add(message,GENERIC_HEADER_CONTENT_TYPE);
		}
	}
	apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"plugin [%s][%d] read and send result.xml", RECOG_ENGINE_TASK_NAME, gettid());
	return TRUE;
}

/* Raise demo RECOGNITION-COMPLETE event */
static apt_bool_t iflytek_recog_recognition_complete(iflytek_recog_channel_t *recog_channel, mrcp_recog_completion_cause_e cause)
{
	mrcp_recog_header_t *recog_header;
	/* create RECOGNITION-COMPLETE event */
	mrcp_message_t *message = mrcp_event_create(
						recog_channel->recog_request,
						RECOGNIZER_RECOGNITION_COMPLETE,
						recog_channel->recog_request->pool);
	if(!message) {
		return FALSE;
	}

	/* get/allocate recognizer header */
	recog_header = mrcp_resource_header_prepare(message);
	if(recog_header) {
		/* set completion cause */
		recog_header->completion_cause = cause;
		mrcp_resource_header_property_add(message,RECOGNIZER_HEADER_COMPLETION_CAUSE);
	}
	/* set request state */
	message->start_line.request_state = MRCP_REQUEST_STATE_COMPLETE;

	if(cause == RECOGNIZER_COMPLETION_CAUSE_SUCCESS) {
		iflytek_recog_result_load(recog_channel,message);
	}

	recog_channel->recog_request = NULL;
	/* send asynch event */
	apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"plugin [%s][%d] channel send recognition complete", RECOG_ENGINE_TASK_NAME, gettid());
	return mrcp_engine_channel_message_send(recog_channel->channel,message);
}

/* Add by pengc */
static apr_size_t level_calculate(const mpf_frame_t *frame)
{
	apr_size_t sum = 0;
	apr_size_t count = frame->codec_frame.size/2;
	const apr_int16_t *cur = frame->codec_frame.buffer;
	const apr_int16_t *end = cur + count;

	for(; cur < end; cur++) {
		if(*cur < 0) {
			sum -= *cur;
		}
		else {
			sum += *cur;
		}
	}

	return sum / count;
}

/** Callback is called from MPF engine context to write/send new frame */
static apt_bool_t iflytek_recog_stream_write(mpf_audio_stream_t *stream, const mpf_frame_t *frame)
{
	iflytek_recog_channel_t *recog_channel = stream->obj;
	if(recog_channel->stop_response) {
		/* send asynchronous response to STOP request */
		mrcp_engine_channel_message_send(recog_channel->channel,recog_channel->stop_response);
		recog_channel->stop_response = NULL;
		recog_channel->recog_request = NULL;
		return TRUE;
	}

	if(recog_channel->recog_request) {
		mpf_detector_event_e det_event = mpf_activity_detector_process(recog_channel->detector,frame);
		switch(det_event) {
			case MPF_DETECTOR_EVENT_ACTIVITY:
				apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Detected Voice Activity " APT_SIDRES_FMT,
					MRCP_MESSAGE_SIDRES(recog_channel->recog_request));
				iflytek_recog_start_of_input(recog_channel);
				break;
			case MPF_DETECTOR_EVENT_INACTIVITY:
				apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Detected Voice Inactivity " APT_SIDRES_FMT,
					MRCP_MESSAGE_SIDRES(recog_channel->recog_request));
				iflytek_recog_recognition_complete(recog_channel,RECOGNIZER_COMPLETION_CAUSE_SUCCESS);
				break;
			case MPF_DETECTOR_EVENT_NOINPUT:
				apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Detected Noinput " APT_SIDRES_FMT,
					MRCP_MESSAGE_SIDRES(recog_channel->recog_request));
				if(recog_channel->timers_started == TRUE) {
					iflytek_recog_recognition_complete(recog_channel,RECOGNIZER_COMPLETION_CAUSE_NO_INPUT_TIMEOUT);
				}
				break;
			default:
				break;
		}

		if(recog_channel->recog_request) {
			if((frame->type & MEDIA_FRAME_TYPE_EVENT) == MEDIA_FRAME_TYPE_EVENT) {
				if(frame->marker == MPF_MARKER_START_OF_EVENT) {
					apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Detected Start of Event " APT_SIDRES_FMT " id:%d",
						MRCP_MESSAGE_SIDRES(recog_channel->recog_request),
						frame->event_frame.event_id);
				}
				else if(frame->marker == MPF_MARKER_END_OF_EVENT) {
					apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"Detected End of Event " APT_SIDRES_FMT " id:%d duration:%d ts",
						MRCP_MESSAGE_SIDRES(recog_channel->recog_request),
						frame->event_frame.event_id,
						frame->event_frame.duration);
				}
			}
		}

		if(recog_channel->audio_out) {
			fwrite(frame->codec_frame.buffer,1,frame->codec_frame.size,recog_channel->audio_out);
		}
		
		struct mpf_activity_detector_t {
			/* voice activity (silence) level threshold */
			apr_size_t			 level_threshold;
		
			/* period of activity required to complete transition to active state */
			apr_size_t			 speech_timeout;
			/* period of inactivity required to complete transition to inactive state */
			apr_size_t			 silence_timeout;
			/* noinput timeout */
			apr_size_t			 noinput_timeout;
		
			/* current state */
			int state;
			/* duration spent in current state	*/
			apr_size_t			 duration;
		}detector;

		memcpy(&detector, recog_channel->detector, sizeof(detector));
		apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"plugin [%s][%d] audio stream callback with request, detector state %d, event state %d, "\
			"level_threshold %d, duration %d, frame level %d", 
			RECOG_ENGINE_TASK_NAME, gettid(), detector.state, det_event,
			detector.level_threshold, detector.duration, level_calculate(frame));
		if (frame && frame->codec_frame.size > 0) {
			apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"plugin [%s][%d] audio stream read frame", RECOG_ENGINE_TASK_NAME, gettid());
		}
	}

	return TRUE;
}

static apt_bool_t iflytek_recog_msg_signal(iflytek_recog_msg_type_e type, mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
	apt_bool_t status = FALSE;
	iflytek_recog_channel_t *demo_channel = channel->method_obj;
	iflytek_recog_engine_t *demo_engine = demo_channel->demo_engine;
	apt_task_t *task = apt_consumer_task_base_get(demo_engine->task);
	apt_task_msg_t *msg = apt_task_msg_get(task);
	if(msg) {
		iflytek_recog_msg_t *demo_msg;
		msg->type = TASK_MSG_USER;
		demo_msg = (iflytek_recog_msg_t*) msg->data;

		demo_msg->type = type;
		demo_msg->channel = channel;
		demo_msg->request = request;
		status = apt_task_msg_signal(task,msg);
	}
	apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"plugin [%s][%d] recog msg signal", RECOG_ENGINE_TASK_NAME, gettid());
	return status;
}

static apt_bool_t iflytek_recog_msg_process(apt_task_t *task, apt_task_msg_t *msg)
{
	iflytek_recog_msg_t *demo_msg = (iflytek_recog_msg_t*)msg->data;
	switch(demo_msg->type) {
		case IFLYTEK_RECOG_MSG_OPEN_CHANNEL:
			/* open channel and send asynch response */
			mrcp_engine_channel_open_respond(demo_msg->channel,TRUE);
			break;
		case IFLYTEK_RECOG_MSG_CLOSE_CHANNEL:
		{
			/* close channel, make sure there is no activity and send asynch response */
			iflytek_recog_channel_t *recog_channel = demo_msg->channel->method_obj;
			if(recog_channel->audio_out) {
				fclose(recog_channel->audio_out);
				recog_channel->audio_out = NULL;
			}

			mrcp_engine_channel_close_respond(demo_msg->channel);
			break;
		}
		case IFLYTEK_RECOG_MSG_REQUEST_PROCESS:
			iflytek_recog_channel_request_dispatch(demo_msg->channel,demo_msg->request);
			break;
		default:
			break;
	}
	apt_log(RECOG_LOG_MARK,APT_PRIO_INFO,"plugin [%s][%d] recog msg process", RECOG_ENGINE_TASK_NAME, gettid());
	return TRUE;
}
