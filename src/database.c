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

#include <assert.h>
#include <stdio.h>
#include <utlist.h>

#include "dimq_broker_internal.h"
#include "memory_dimq.h"
#include "send_dimq.h"
#include "sys_tree.h"
#include "time_dimq.h"
#include "util_dimq.h"

/**
 * Is this context ready to take more in flight messages right now?
 * @param context the client context of interest
 * @param qos qos for the packet of interest
 * @return true if more in flight are allowed.
 */
bool db__ready_for_flight(struct dimq *context, enum dimq_msg_direction dir, int qos)
{
	struct dimq_msg_data *msgs;
	bool valid_bytes;
	bool valid_count;

	if(dir == dimq_md_out){
		msgs = &context->msgs_out;
	}else{
		msgs = &context->msgs_in;
	}

	if(msgs->inflight_maximum == 0 && db.config->max_inflight_bytes == 0){
		return true;
	}

	if(qos == 0){
		/* Deliver QoS 0 messages unless the queue is already full.
		 * For QoS 0 messages the choice is either "inflight" or dropped.
		 * There is no queueing option, unless the client is offline and
		 * queue_qos0_messages is enabled.
		 */
		if(db.config->max_queued_messages == 0 && db.config->max_inflight_bytes == 0){
			return true;
		}
		valid_bytes = ((msgs->msg_bytes - (ssize_t)db.config->max_inflight_bytes) < (ssize_t)db.config->max_queued_bytes);
		if(dir == dimq_md_out){
			valid_count = context->out_packet_count < db.config->max_queued_messages;
		}else{
			valid_count = msgs->msg_count - msgs->inflight_maximum < db.config->max_queued_messages;
		}

		if(db.config->max_queued_messages == 0){
			return valid_bytes;
		}
		if(db.config->max_queued_bytes == 0){
			return valid_count;
		}
	}else{
		valid_bytes = (ssize_t)msgs->msg_bytes12 < (ssize_t)db.config->max_inflight_bytes;
		valid_count = msgs->inflight_quota > 0;

		if(msgs->inflight_maximum == 0){
			return valid_bytes;
		}
		if(db.config->max_inflight_bytes == 0){
			return valid_count;
		}
	}

	return valid_bytes && valid_count;
}


/**
 * For a given client context, are more messages allowed to be queued?
 * It is assumed that inflight checks and queue_qos0 checks have already
 * been made.
 * @param context client of interest
 * @param qos destination qos for the packet of interest
 * @return true if queuing is allowed, false if should be dropped
 */
bool db__ready_for_queue(struct dimq *context, int qos, struct dimq_msg_data *msg_data)
{
	int source_count;
	int adjust_count;
	long source_bytes;
	ssize_t adjust_bytes = (ssize_t)db.config->max_inflight_bytes;
	bool valid_bytes;
	bool valid_count;

	if(db.config->max_queued_messages == 0 && db.config->max_queued_bytes == 0){
		return true;
	}

	if(qos == 0 && db.config->queue_qos0_messages == false){
		return false; /* This case is handled in db__ready_for_flight() */
	}else{
		source_bytes = (ssize_t)msg_data->msg_bytes12;
		source_count = msg_data->msg_count12;
	}
	adjust_count = msg_data->inflight_maximum;

	/* nothing in flight for offline clients */
	if(context->sock == INVALID_SOCKET){
		adjust_bytes = 0;
		adjust_count = 0;
	}

	valid_bytes = (source_bytes - (ssize_t)adjust_bytes) < (ssize_t)db.config->max_queued_bytes;
	valid_count = source_count - adjust_count < db.config->max_queued_messages;

	if(db.config->max_queued_bytes == 0){
		return valid_count;
	}
	if(db.config->max_queued_messages == 0){
		return valid_bytes;
	}

	return valid_bytes && valid_count;
}


int db__open(struct dimq__config *config)
{
	struct dimq__subhier *subhier;

	if(!config) return dimq_ERR_INVAL;

	db.last_db_id = 0;

	db.contexts_by_id = NULL;
	db.contexts_by_sock = NULL;
	db.contexts_for_free = NULL;
#ifdef WITH_BRIDGE
	db.bridges = NULL;
	db.bridge_count = 0;
#endif

	/* Initialize the hashtable */
	db.clientid_index_hash = NULL;

	db.subs = NULL;

	subhier = sub__add_hier_entry(NULL, &db.subs, "", 0);
	if(!subhier) return dimq_ERR_NOMEM;

	subhier = sub__add_hier_entry(NULL, &db.subs, "$SYS", (uint16_t)strlen("$SYS"));
	if(!subhier) return dimq_ERR_NOMEM;

	retain__init();

	db.config->security_options.unpwd = NULL;

#ifdef WITH_PERSISTENCE
	if(persist__restore()) return 1;
#endif

	return dimq_ERR_SUCCESS;
}

static void subhier_clean(struct dimq__subhier **subhier)
{
	struct dimq__subhier *peer, *subhier_tmp;
	struct dimq__subleaf *leaf, *nextleaf;

	HASH_ITER(hh, *subhier, peer, subhier_tmp){
		leaf = peer->subs;
		while(leaf){
			nextleaf = leaf->next;
			dimq__free(leaf);
			leaf = nextleaf;
		}
		subhier_clean(&peer->children);
		dimq__free(peer->topic);

		HASH_DELETE(hh, *subhier, peer);
		dimq__free(peer);
	}
}

int db__close(void)
{
	subhier_clean(&db.subs);
	retain__clean(&db.retains);
	db__msg_store_clean();

	return dimq_ERR_SUCCESS;
}


void db__msg_store_add(struct dimq_msg_store *store)
{
	store->next = db.msg_store;
	store->prev = NULL;
	if(db.msg_store){
		db.msg_store->prev = store;
	}
	db.msg_store = store;
}


void db__msg_store_free(struct dimq_msg_store *store)
{
	int i;

	dimq__free(store->source_id);
	dimq__free(store->source_username);
	if(store->dest_ids){
		for(i=0; i<store->dest_id_count; i++){
			dimq__free(store->dest_ids[i]);
		}
		dimq__free(store->dest_ids);
	}
	dimq__free(store->topic);
	dimq_property_free_all(&store->properties);
	dimq__free(store->payload);
	dimq__free(store);
}

void db__msg_store_remove(struct dimq_msg_store *store)
{
	if(store->prev){
		store->prev->next = store->next;
		if(store->next){
			store->next->prev = store->prev;
		}
	}else{
		db.msg_store = store->next;
		if(store->next){
			store->next->prev = NULL;
		}
	}
	db.msg_store_count--;
	db.msg_store_bytes -= store->payloadlen;

	db__msg_store_free(store);
}


void db__msg_store_clean(void)
{
	struct dimq_msg_store *store, *next;;

	store = db.msg_store;
	while(store){
		next = store->next;
		db__msg_store_remove(store);
		store = next;
	}
}

void db__msg_store_ref_inc(struct dimq_msg_store *store)
{
	store->ref_count++;
}

void db__msg_store_ref_dec(struct dimq_msg_store **store)
{
	(*store)->ref_count--;
	if((*store)->ref_count == 0){
		db__msg_store_remove(*store);
		*store = NULL;
	}
}


void db__msg_store_compact(void)
{
	struct dimq_msg_store *store, *next;

	store = db.msg_store;
	while(store){
		next = store->next;
		if(store->ref_count < 1){
			db__msg_store_remove(store);
		}
		store = next;
	}
}


static void db__message_remove(struct dimq_msg_data *msg_data, struct dimq_client_msg *item)
{
	if(!msg_data || !item){
		return;
	}

	DL_DELETE(msg_data->inflight, item);
	if(item->store){
		msg_data->msg_count--;
		msg_data->msg_bytes -= item->store->payloadlen;
		if(item->qos > 0){
			msg_data->msg_count12--;
			msg_data->msg_bytes12 -= item->store->payloadlen;
		}
		db__msg_store_ref_dec(&item->store);
	}

	dimq_property_free_all(&item->properties);
	dimq__free(item);
}


void db__message_dequeue_first(struct dimq *context, struct dimq_msg_data *msg_data)
{
	struct dimq_client_msg *msg;

	UNUSED(context);

	msg = msg_data->queued;
	DL_DELETE(msg_data->queued, msg);
	DL_APPEND(msg_data->inflight, msg);
	if(msg_data->inflight_quota > 0){
		msg_data->inflight_quota--;
	}
}


int db__message_delete_outgoing(struct dimq *context, uint16_t mid, enum dimq_msg_state expect_state, int qos)
{
	struct dimq_client_msg *tail, *tmp;
	int msg_index = 0;

	if(!context) return dimq_ERR_INVAL;

	DL_FOREACH_SAFE(context->msgs_out.inflight, tail, tmp){
		msg_index++;
		if(tail->mid == mid){
			if(tail->qos != qos){
				return dimq_ERR_PROTOCOL;
			}else if(qos == 2 && tail->state != expect_state){
				return dimq_ERR_PROTOCOL;
			}
			msg_index--;
			db__message_remove(&context->msgs_out, tail);
			break;
		}
	}

	DL_FOREACH_SAFE(context->msgs_out.queued, tail, tmp){
		if(context->msgs_out.inflight_maximum != 0 && msg_index >= context->msgs_out.inflight_maximum){
			break;
		}

		msg_index++;
		tail->timestamp = db.now_s;
		switch(tail->qos){
			case 0:
				tail->state = dimq_ms_publish_qos0;
				break;
			case 1:
				tail->state = dimq_ms_publish_qos1;
				break;
			case 2:
				tail->state = dimq_ms_publish_qos2;
				break;
		}
		db__message_dequeue_first(context, &context->msgs_out);
	}
#ifdef WITH_PERSISTENCE
	db.persistence_changes++;
#endif

	return db__message_write_inflight_out_latest(context);
}

int db__message_insert(struct dimq *context, uint16_t mid, enum dimq_msg_direction dir, uint8_t qos, bool retain, struct dimq_msg_store *stored, dimq_property *properties, bool update)
{
	struct dimq_client_msg *msg;
	struct dimq_msg_data *msg_data;
	enum dimq_msg_state state = dimq_ms_invalid;
	int rc = 0;
	int i;
	char **dest_ids;

	assert(stored);
	if(!context) return dimq_ERR_INVAL;
	if(!context->id) return dimq_ERR_SUCCESS; /* Protect against unlikely "client is disconnected but not entirely freed" scenario */

	if(dir == dimq_md_out){
		msg_data = &context->msgs_out;
	}else{
		msg_data = &context->msgs_in;
	}

	/* Check whether we've already sent this message to this client
	 * for outgoing messages only.
	 * If retain==true then this is a stale retained message and so should be
	 * sent regardless. FIXME - this does mean retained messages will received
	 * multiple times for overlapping subscriptions, although this is only the
	 * case for SUBSCRIPTION with multiple subs in so is a minor concern.
	 */
	if(context->protocol != dimq_p_mqtt5
			&& db.config->allow_duplicate_messages == false
			&& dir == dimq_md_out && retain == false && stored->dest_ids){

		for(i=0; i<stored->dest_id_count; i++){
			if(!strcmp(stored->dest_ids[i], context->id)){
				/* We have already sent this message to this client. */
				dimq_property_free_all(&properties);
				return dimq_ERR_SUCCESS;
			}
		}
	}
	if(context->sock == INVALID_SOCKET){
		/* Client is not connected only queue messages with QoS>0. */
		if(qos == 0 && !db.config->queue_qos0_messages){
			if(!context->bridge){
				dimq_property_free_all(&properties);
				return 2;
			}else{
				if(context->bridge->start_type != bst_lazy){
					dimq_property_free_all(&properties);
					return 2;
				}
			}
		}
		if(context->bridge && context->bridge->clean_start_local == true){
			dimq_property_free_all(&properties);
			return 2;
		}
	}

	if(context->sock != INVALID_SOCKET){
		if(db__ready_for_flight(context, dir, qos)){
			if(dir == dimq_md_out){
				switch(qos){
					case 0:
						state = dimq_ms_publish_qos0;
						break;
					case 1:
						state = dimq_ms_publish_qos1;
						break;
					case 2:
						state = dimq_ms_publish_qos2;
						break;
				}
			}else{
				if(qos == 2){
					state = dimq_ms_wait_for_pubrel;
				}else{
					dimq_property_free_all(&properties);
					return 1;
				}
			}
		}else if(qos != 0 && db__ready_for_queue(context, qos, msg_data)){
			state = dimq_ms_queued;
			rc = 2;
		}else{
			/* Dropping message due to full queue. */
			if(context->is_dropping == false){
				context->is_dropping = true;
				log__printf(NULL, dimq_LOG_NOTICE,
						"Outgoing messages are being dropped for client %s.",
						context->id);
			}
			G_MSGS_DROPPED_INC();
			dimq_property_free_all(&properties);
			return 2;
		}
	}else{
		if (db__ready_for_queue(context, qos, msg_data)){
			state = dimq_ms_queued;
		}else{
			G_MSGS_DROPPED_INC();
			if(context->is_dropping == false){
				context->is_dropping = true;
				log__printf(NULL, dimq_LOG_NOTICE,
						"Outgoing messages are being dropped for client %s.",
						context->id);
			}
			dimq_property_free_all(&properties);
			return 2;
		}
	}
	assert(state != dimq_ms_invalid);

#ifdef WITH_PERSISTENCE
	if(state == dimq_ms_queued){
		db.persistence_changes++;
	}
#endif

	msg = dimq__malloc(sizeof(struct dimq_client_msg));
	if(!msg) return dimq_ERR_NOMEM;
	msg->prev = NULL;
	msg->next = NULL;
	msg->store = stored;
	db__msg_store_ref_inc(msg->store);
	msg->mid = mid;
	msg->timestamp = db.now_s;
	msg->direction = dir;
	msg->state = state;
	msg->dup = false;
	if(qos > context->max_qos){
		msg->qos = context->max_qos;
	}else{
		msg->qos = qos;
	}
	msg->retain = retain;
	msg->properties = properties;

	if(state == dimq_ms_queued){
		DL_APPEND(msg_data->queued, msg);
	}else{
		DL_APPEND(msg_data->inflight, msg);
	}
	msg_data->msg_count++;
	msg_data->msg_bytes+= msg->store->payloadlen;
	if(qos > 0){
		msg_data->msg_count12++;
		msg_data->msg_bytes12 += msg->store->payloadlen;
	}

	if(db.config->allow_duplicate_messages == false && dir == dimq_md_out && retain == false){
		/* Record which client ids this message has been sent to so we can avoid duplicates.
		 * Outgoing messages only.
		 * If retain==true then this is a stale retained message and so should be
		 * sent regardless. FIXME - this does mean retained messages will received
		 * multiple times for overlapping subscriptions, although this is only the
		 * case for SUBSCRIPTION with multiple subs in so is a minor concern.
		 */
		dest_ids = dimq__realloc(stored->dest_ids, sizeof(char *)*(size_t)(stored->dest_id_count+1));
		if(dest_ids){
			stored->dest_ids = dest_ids;
			stored->dest_id_count++;
			stored->dest_ids[stored->dest_id_count-1] = dimq__strdup(context->id);
			if(!stored->dest_ids[stored->dest_id_count-1]){
				return dimq_ERR_NOMEM;
			}
		}else{
			return dimq_ERR_NOMEM;
		}
	}
#ifdef WITH_BRIDGE
	if(context->bridge && context->bridge->start_type == bst_lazy
			&& context->sock == INVALID_SOCKET
			&& context->msgs_out.msg_count >= context->bridge->threshold){

		context->bridge->lazy_reconnect = true;
	}
#endif

	if(dir == dimq_md_out && msg->qos > 0){
		util__decrement_send_quota(context);
	}

	if(dir == dimq_md_out && update){
		rc = db__message_write_inflight_out_latest(context);
		if(rc) return rc;
		rc = db__message_write_queued_out(context);
		if(rc) return rc;
	}

	return rc;
}

int db__message_update_outgoing(struct dimq *context, uint16_t mid, enum dimq_msg_state state, int qos)
{
	struct dimq_client_msg *tail;

	DL_FOREACH(context->msgs_out.inflight, tail){
		if(tail->mid == mid){
			if(tail->qos != qos){
				return dimq_ERR_PROTOCOL;
			}
			tail->state = state;
			tail->timestamp = db.now_s;
			return dimq_ERR_SUCCESS;
		}
	}
	return dimq_ERR_NOT_FOUND;
}


static void db__messages_delete_list(struct dimq_client_msg **head)
{
	struct dimq_client_msg *tail, *tmp;

	DL_FOREACH_SAFE(*head, tail, tmp){
		DL_DELETE(*head, tail);
		db__msg_store_ref_dec(&tail->store);
		dimq_property_free_all(&tail->properties);
		dimq__free(tail);
	}
	*head = NULL;
}


int db__messages_delete(struct dimq *context, bool force_free)
{
	if(!context) return dimq_ERR_INVAL;

	if(force_free || context->clean_start || (context->bridge && context->bridge->clean_start)){
		db__messages_delete_list(&context->msgs_in.inflight);
		db__messages_delete_list(&context->msgs_in.queued);
		context->msgs_in.msg_bytes = 0;
		context->msgs_in.msg_bytes12 = 0;
		context->msgs_in.msg_count = 0;
		context->msgs_in.msg_count12 = 0;
	}

	if(force_free || (context->bridge && context->bridge->clean_start_local)
			|| (context->bridge == NULL && context->clean_start)){

		db__messages_delete_list(&context->msgs_out.inflight);
		db__messages_delete_list(&context->msgs_out.queued);
		context->msgs_out.msg_bytes = 0;
		context->msgs_out.msg_bytes12 = 0;
		context->msgs_out.msg_count = 0;
		context->msgs_out.msg_count12 = 0;
	}

	return dimq_ERR_SUCCESS;
}

int db__messages_easy_queue(struct dimq *context, const char *topic, uint8_t qos, uint32_t payloadlen, const void *payload, int retain, uint32_t message_expiry_interval, dimq_property **properties)
{
	struct dimq_msg_store *stored;
	const char *source_id;
	enum dimq_msg_origin origin;

	if(!topic) return dimq_ERR_INVAL;

	stored = dimq__calloc(1, sizeof(struct dimq_msg_store));
	if(stored == NULL) return dimq_ERR_NOMEM;

	stored->topic = dimq__strdup(topic);
	if(stored->topic == NULL){
		db__msg_store_free(stored);
		return dimq_ERR_INVAL;
	}

	stored->qos = qos;
	if(db.config->retain_available == false){
		stored->retain = 0;
	}else{
		stored->retain = retain;
	}

	stored->payloadlen = payloadlen;
	stored->payload = dimq__malloc(stored->payloadlen+1);
	if(stored->payload == NULL){
		db__msg_store_free(stored);
		return dimq_ERR_NOMEM;
	}
	/* Ensure payload is always zero terminated, this is the reason for the extra byte above */
	((uint8_t *)stored->payload)[stored->payloadlen] = 0;
	memcpy(stored->payload, payload, stored->payloadlen);

	if(context && context->id){
		source_id = context->id;
	}else{
		source_id = "";
	}
	if(properties){
		stored->properties = *properties;
		*properties = NULL;
	}

	if(context){
		origin = dimq_mo_client;
	}else{
		origin = dimq_mo_broker;
	}
	if(db__message_store(context, stored, message_expiry_interval, 0, origin)) return 1;

	return sub__messages_queue(source_id, stored->topic, stored->qos, stored->retain, &stored);
}

/* This function requires topic to be allocated on the heap. Once called, it owns topic and will free it on error. Likewise payload and properties. */
int db__message_store(const struct dimq *source, struct dimq_msg_store *stored, uint32_t message_expiry_interval, dbid_t store_id, enum dimq_msg_origin origin)
{
	assert(stored);

	if(source && source->id){
		stored->source_id = dimq__strdup(source->id);
	}else{
		stored->source_id = dimq__strdup("");
	}
	if(!stored->source_id){
		log__printf(NULL, dimq_LOG_ERR, "Error: Out of memory.");
		db__msg_store_free(stored);
		return dimq_ERR_NOMEM;
	}

	if(source && source->username){
		stored->source_username = dimq__strdup(source->username);
		if(!stored->source_username){
			db__msg_store_free(stored);
			return dimq_ERR_NOMEM;
		}
	}
	if(source){
		stored->source_listener = source->listener;
	}
	stored->mid = 0;
	stored->origin = origin;
	if(message_expiry_interval > 0){
		stored->message_expiry_time = db.now_real_s + message_expiry_interval;
	}else{
		stored->message_expiry_time = 0;
	}

	stored->dest_ids = NULL;
	stored->dest_id_count = 0;
	db.msg_store_count++;
	db.msg_store_bytes += stored->payloadlen;

	if(!store_id){
		stored->db_id = ++db.last_db_id;
	}else{
		stored->db_id = store_id;
	}

	db__msg_store_add(stored);

	return dimq_ERR_SUCCESS;
}

int db__message_store_find(struct dimq *context, uint16_t mid, struct dimq_msg_store **stored)
{
	struct dimq_client_msg *tail;

	if(!context) return dimq_ERR_INVAL;

	*stored = NULL;
	DL_FOREACH(context->msgs_in.inflight, tail){
		if(tail->store->source_mid == mid){
			*stored = tail->store;
			return dimq_ERR_SUCCESS;
		}
	}

	DL_FOREACH(context->msgs_in.queued, tail){
		if(tail->store->source_mid == mid){
			*stored = tail->store;
			return dimq_ERR_SUCCESS;
		}
	}

	return 1;
}

/* Called on reconnect to set outgoing messages to a sensible state and force a
 * retry, and to set incoming messages to expect an appropriate retry. */
static int db__message_reconnect_reset_outgoing(struct dimq *context)
{
	struct dimq_client_msg *msg, *tmp;

	context->msgs_out.msg_bytes = 0;
	context->msgs_out.msg_bytes12 = 0;
	context->msgs_out.msg_count = 0;
	context->msgs_out.msg_count12 = 0;
	context->msgs_out.inflight_quota = context->msgs_out.inflight_maximum;

	DL_FOREACH_SAFE(context->msgs_out.inflight, msg, tmp){
		context->msgs_out.msg_count++;
		context->msgs_out.msg_bytes += msg->store->payloadlen;
		if(msg->qos > 0){
			context->msgs_out.msg_count12++;
			context->msgs_out.msg_bytes12 += msg->store->payloadlen;
			util__decrement_send_quota(context);
		}

		switch(msg->qos){
			case 0:
				msg->state = dimq_ms_publish_qos0;
				break;
			case 1:
				msg->state = dimq_ms_publish_qos1;
				break;
			case 2:
				if(msg->state == dimq_ms_wait_for_pubcomp){
					msg->state = dimq_ms_resend_pubrel;
				}else{
					msg->state = dimq_ms_publish_qos2;
				}
				break;
		}
	}
	/* Messages received when the client was disconnected are put
	 * in the dimq_ms_queued state. If we don't change them to the
	 * appropriate "publish" state, then the queued messages won't
	 * get sent until the client next receives a message - and they
	 * will be sent out of order.
	 */
	DL_FOREACH_SAFE(context->msgs_out.queued, msg, tmp){
		context->msgs_out.msg_count++;
		context->msgs_out.msg_bytes += msg->store->payloadlen;
		if(msg->qos > 0){
			context->msgs_out.msg_count12++;
			context->msgs_out.msg_bytes12 += msg->store->payloadlen;
		}
		if(db__ready_for_flight(context, dimq_md_out, msg->qos)){
			switch(msg->qos){
				case 0:
					msg->state = dimq_ms_publish_qos0;
					break;
				case 1:
					msg->state = dimq_ms_publish_qos1;
					break;
				case 2:
					msg->state = dimq_ms_publish_qos2;
					break;
			}
			db__message_dequeue_first(context, &context->msgs_out);
		}
	}

	return dimq_ERR_SUCCESS;
}


/* Called on reconnect to set incoming messages to expect an appropriate retry. */
static int db__message_reconnect_reset_incoming(struct dimq *context)
{
	struct dimq_client_msg *msg, *tmp;

	context->msgs_in.msg_bytes = 0;
	context->msgs_in.msg_bytes12 = 0;
	context->msgs_in.msg_count = 0;
	context->msgs_in.msg_count12 = 0;
	context->msgs_in.inflight_quota = context->msgs_in.inflight_maximum;

	DL_FOREACH_SAFE(context->msgs_in.inflight, msg, tmp){
		context->msgs_in.msg_count++;
		context->msgs_in.msg_bytes += msg->store->payloadlen;
		if(msg->qos > 0){
			context->msgs_in.msg_count12++;
			context->msgs_in.msg_bytes12 += msg->store->payloadlen;
			util__decrement_receive_quota(context);
		}

		if(msg->qos != 2){
			/* Anything <QoS 2 can be completely retried by the client at
			 * no harm. */
			db__message_remove(&context->msgs_in, msg);
		}else{
			/* Message state can be preserved here because it should match
			 * whatever the client has got. */
		}
	}

	/* Messages received when the client was disconnected are put
	 * in the dimq_ms_queued state. If we don't change them to the
	 * appropriate "publish" state, then the queued messages won't
	 * get sent until the client next receives a message - and they
	 * will be sent out of order.
	 */
	DL_FOREACH_SAFE(context->msgs_in.queued, msg, tmp){
		context->msgs_in.msg_count++;
		context->msgs_in.msg_bytes += msg->store->payloadlen;
		if(msg->qos > 0){
			context->msgs_in.msg_count12++;
			context->msgs_in.msg_bytes12 += msg->store->payloadlen;
		}
		if(db__ready_for_flight(context, dimq_md_in, msg->qos)){
			switch(msg->qos){
				case 0:
					msg->state = dimq_ms_publish_qos0;
					break;
				case 1:
					msg->state = dimq_ms_publish_qos1;
					break;
				case 2:
					msg->state = dimq_ms_publish_qos2;
					break;
			}
			db__message_dequeue_first(context, &context->msgs_in);
		}
	}

	return dimq_ERR_SUCCESS;
}


int db__message_reconnect_reset(struct dimq *context)
{
	int rc;

	rc = db__message_reconnect_reset_outgoing(context);
	if(rc) return rc;
	return db__message_reconnect_reset_incoming(context);
}


int db__message_remove_incoming(struct dimq* context, uint16_t mid)
{
	struct dimq_client_msg *tail, *tmp;

	if(!context) return dimq_ERR_INVAL;

	DL_FOREACH_SAFE(context->msgs_in.inflight, tail, tmp){
		if(tail->mid == mid) {
			if(tail->store->qos != 2){
				return dimq_ERR_PROTOCOL;
			}
			db__message_remove(&context->msgs_in, tail);
			return dimq_ERR_SUCCESS;
		}
	}

	return dimq_ERR_NOT_FOUND;
}


int db__message_release_incoming(struct dimq *context, uint16_t mid)
{
	struct dimq_client_msg *tail, *tmp;
	int retain;
	char *topic;
	char *source_id;
	int msg_index = 0;
	bool deleted = false;
	int rc;

	if(!context) return dimq_ERR_INVAL;

	DL_FOREACH_SAFE(context->msgs_in.inflight, tail, tmp){
		msg_index++;
		if(tail->mid == mid){
			if(tail->store->qos != 2){
				return dimq_ERR_PROTOCOL;
			}
			topic = tail->store->topic;
			retain = tail->retain;
			source_id = tail->store->source_id;

			/* topic==NULL should be a QoS 2 message that was
			 * denied/dropped and is being processed so the client doesn't
			 * keep resending it. That means we don't send it to other
			 * clients. */
			if(topic == NULL){
				db__message_remove(&context->msgs_in, tail);
				deleted = true;
			}else{
				rc = sub__messages_queue(source_id, topic, 2, retain, &tail->store);
				if(rc == dimq_ERR_SUCCESS || rc == dimq_ERR_NO_SUBSCRIBERS){
					db__message_remove(&context->msgs_in, tail);
					deleted = true;
				}else{
					return 1;
				}
			}
		}
	}

	DL_FOREACH_SAFE(context->msgs_in.queued, tail, tmp){
		if(context->msgs_in.inflight_maximum != 0 && msg_index >= context->msgs_in.inflight_maximum){
			break;
		}

		msg_index++;
		tail->timestamp = db.now_s;

		if(tail->qos == 2){
			send__pubrec(context, tail->mid, 0, NULL);
			tail->state = dimq_ms_wait_for_pubrel;
			db__message_dequeue_first(context, &context->msgs_in);
		}
	}
	if(deleted){
		return dimq_ERR_SUCCESS;
	}else{
		return dimq_ERR_NOT_FOUND;
	}
}

static int db__message_write_inflight_out_single(struct dimq *context, struct dimq_client_msg *msg)
{
	dimq_property *cmsg_props = NULL, *store_props = NULL;
	int rc;
	uint16_t mid;
	int retries;
	int retain;
	const char *topic;
	uint8_t qos;
	uint32_t payloadlen;
	const void *payload;
	uint32_t expiry_interval;

	expiry_interval = 0;
	if(msg->store->message_expiry_time){
		if(db.now_real_s > msg->store->message_expiry_time){
			/* Message is expired, must not send. */
			if(msg->direction == dimq_md_out && msg->qos > 0){
				util__increment_send_quota(context);
			}
			db__message_remove(&context->msgs_out, msg);
			return dimq_ERR_SUCCESS;
		}else{
			expiry_interval = (uint32_t)(msg->store->message_expiry_time - db.now_real_s);
		}
	}
	mid = msg->mid;
	retries = msg->dup;
	retain = msg->retain;
	topic = msg->store->topic;
	qos = (uint8_t)msg->qos;
	payloadlen = msg->store->payloadlen;
	payload = msg->store->payload;
	cmsg_props = msg->properties;
	store_props = msg->store->properties;

	switch(msg->state){
		case dimq_ms_publish_qos0:
			rc = send__publish(context, mid, topic, payloadlen, payload, qos, retain, retries, cmsg_props, store_props, expiry_interval);
			if(rc == dimq_ERR_SUCCESS || rc == dimq_ERR_OVERSIZE_PACKET){
				db__message_remove(&context->msgs_out, msg);
			}else{
				return rc;
			}
			break;

		case dimq_ms_publish_qos1:
			rc = send__publish(context, mid, topic, payloadlen, payload, qos, retain, retries, cmsg_props, store_props, expiry_interval);
			if(rc == dimq_ERR_SUCCESS){
				msg->timestamp = db.now_s;
				msg->dup = 1; /* Any retry attempts are a duplicate. */
				msg->state = dimq_ms_wait_for_puback;
			}else if(rc == dimq_ERR_OVERSIZE_PACKET){
				db__message_remove(&context->msgs_out, msg);
			}else{
				return rc;
			}
			break;

		case dimq_ms_publish_qos2:
			rc = send__publish(context, mid, topic, payloadlen, payload, qos, retain, retries, cmsg_props, store_props, expiry_interval);
			if(rc == dimq_ERR_SUCCESS){
				msg->timestamp = db.now_s;
				msg->dup = 1; /* Any retry attempts are a duplicate. */
				msg->state = dimq_ms_wait_for_pubrec;
			}else if(rc == dimq_ERR_OVERSIZE_PACKET){
				db__message_remove(&context->msgs_out, msg);
			}else{
				return rc;
			}
			break;

		case dimq_ms_resend_pubrel:
			rc = send__pubrel(context, mid, NULL);
			if(!rc){
				msg->state = dimq_ms_wait_for_pubcomp;
			}else{
				return rc;
			}
			break;

		case dimq_ms_invalid:
		case dimq_ms_send_pubrec:
		case dimq_ms_resend_pubcomp:
		case dimq_ms_wait_for_puback:
		case dimq_ms_wait_for_pubrec:
		case dimq_ms_wait_for_pubrel:
		case dimq_ms_wait_for_pubcomp:
		case dimq_ms_queued:
			break;
	}
	return dimq_ERR_SUCCESS;
}


int db__message_write_inflight_out_all(struct dimq *context)
{
	struct dimq_client_msg *tail, *tmp;
	int rc;

	if(context->state != dimq_cs_active || context->sock == INVALID_SOCKET){
		return dimq_ERR_SUCCESS;
	}

	DL_FOREACH_SAFE(context->msgs_out.inflight, tail, tmp){
		rc = db__message_write_inflight_out_single(context, tail);
		if(rc) return rc;
	}
	return dimq_ERR_SUCCESS;
}


int db__message_write_inflight_out_latest(struct dimq *context)
{
	struct dimq_client_msg *tail, *next;
	int rc;

	if(context->state != dimq_cs_active
			|| context->sock == INVALID_SOCKET
			|| context->msgs_out.inflight == NULL){

		return dimq_ERR_SUCCESS;
	}

	if(context->msgs_out.inflight->prev == context->msgs_out.inflight){
		/* Only one message */
		return db__message_write_inflight_out_single(context, context->msgs_out.inflight);
	}

	/* Start at the end of the list and work backwards looking for the first
	 * message in a non-publish state */
	tail = context->msgs_out.inflight->prev;
	while(tail != context->msgs_out.inflight &&
			(tail->state == dimq_ms_publish_qos0
			 || tail->state == dimq_ms_publish_qos1
			 || tail->state == dimq_ms_publish_qos2)){

		tail = tail->prev;
	}

	/* Tail is now either the head of the list, if that message is waiting for
	 * publish, or the oldest message not waiting for a publish. In the latter
	 * case, any pending publishes should be next after this message. */
	if(tail != context->msgs_out.inflight){
		tail = tail->next;
	}

	while(tail){
		next = tail->next;
		rc = db__message_write_inflight_out_single(context, tail);
		if(rc) return rc;
		tail = next;
	}
	return dimq_ERR_SUCCESS;
}


int db__message_write_queued_in(struct dimq *context)
{
	struct dimq_client_msg *tail, *tmp;
	int rc;

	if(context->state != dimq_cs_active){
		return dimq_ERR_SUCCESS;
	}

	DL_FOREACH_SAFE(context->msgs_in.queued, tail, tmp){
		if(context->msgs_out.inflight_maximum != 0 && context->msgs_in.inflight_quota == 0){
			break;
		}

		if(tail->qos == 2){
			tail->state = dimq_ms_send_pubrec;
			db__message_dequeue_first(context, &context->msgs_in);
			rc = send__pubrec(context, tail->mid, 0, NULL);
			if(!rc){
				tail->state = dimq_ms_wait_for_pubrel;
			}else{
				return rc;
			}
		}
	}
	return dimq_ERR_SUCCESS;
}


int db__message_write_queued_out(struct dimq *context)
{
	struct dimq_client_msg *tail, *tmp;

	if(context->state != dimq_cs_active){
		return dimq_ERR_SUCCESS;
	}

	DL_FOREACH_SAFE(context->msgs_out.queued, tail, tmp){
		if(context->msgs_out.inflight_maximum != 0 && context->msgs_out.inflight_quota == 0){
			break;
		}

		switch(tail->qos){
			case 0:
				tail->state = dimq_ms_publish_qos0;
				break;
			case 1:
				tail->state = dimq_ms_publish_qos1;
				break;
			case 2:
				tail->state = dimq_ms_publish_qos2;
				break;
		}
		db__message_dequeue_first(context, &context->msgs_out);
	}
	return dimq_ERR_SUCCESS;
}
