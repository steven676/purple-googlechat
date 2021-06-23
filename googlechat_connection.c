/*
 * GoogleChat Plugin for libpurple/Pidgin
 * Copyright (c) 2015-2016 Eion Robb, Mike Ruprecht
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "googlechat_connection.h"


#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include <purple.h>

#include "googlechat_pblite.h"
#include "googlechat_json.h"
#include "googlechat.pb-c.h"
#include "googlechat_conversation.h"

void
googlechat_process_data_chunks(GoogleChatAccount *ha, const gchar *data, gsize len)
{
	JsonArray *chunks;
	guint i, num_chunks;
	
	chunks = json_decode_array(data, len);
	
	for (i = 0, num_chunks = json_array_get_length(chunks); i < num_chunks; i++) {
		JsonArray *chunk;
		JsonArray *array;
		JsonNode *array0;
		
		chunk = json_array_get_array_element(chunks, i);
		
		array = json_array_get_array_element(chunk, 1);
		array0 = json_array_get_element(array, 0);
		if (JSON_NODE_HOLDS_VALUE(array0)) {
			//probably a nooooop
			if (g_strcmp0(json_node_get_string(array0), "noop") == 0) {
				//A nope ninja delivers a wicked dragon kick
#ifdef DEBUG
				printf("noop\n");
#endif
			}
		} else {
			JsonObject *obj = json_node_get_object(array0);
			if (json_object_has_member(obj, "data")) {
				//Contains protobuf data, base64 encoded
				ProtobufCMessage *unpacked_message;
				StreamEventsResponse *events_response;
				guchar *decoded_response;
				gsize response_len;
				const gchar *data = json_object_get_string_member(obj, "data");
				
				decoded_response = g_base64_decode(data, &response_len);
				unpacked_message = protobuf_c_message_unpack(&stream_events_response__descriptor, NULL, response_len, decoded_response);
				
				events_response = (StreamEventsResponse *) unpacked_message;
				purple_signal_emit(purple_connection_get_protocol(ha->pc), "googlechat-received-event", ha->pc, events_response->event);
				
				protobuf_c_message_free_unpacked(unpacked_message, NULL);
				g_free(decoded_response);
				
				continue;
			}
		}
	}
	
	json_array_unref(chunks);
}

static int
read_all(int fd, void *buf, size_t len)
{
	unsigned int rs = 0;
	while(rs < len)
	{
		int rval = read(fd, buf + rs, len - rs);
		if (rval == 0)
			break;
		if (rval < 0)
			return rval;

		rs += rval;
	}
	return rs;
}

void
googlechat_process_channel(int fd)
{
	gsize len, lenpos = 0;
	gchar len_str[256];
	gchar *chunk;
	
	while(read(fd, len_str + lenpos, 1) > 0) {
		//read up until \n
		if (len_str[lenpos] == '\n') {
			//convert to int, use as length of string to read
			len_str[lenpos] = '\0';
#ifdef DEBUG
			printf("len_str is %s\n", len_str);
#endif
			len = atoi(len_str);
			
			chunk = g_new(gchar, len * 2);
			//XX - could be a utf-16 length*2 though, so read up until \n????
			
			if (read_all(fd, chunk, len) > 0) {
				//throw chunk to googlechat_process_data_chunks
				googlechat_process_data_chunks(NULL, chunk, len);
			}
			
			g_free(chunk);
			
			lenpos = 0;
		} else {
			lenpos = lenpos + 1;
		}
	}
}

void
googlechat_process_channel_buffer(GoogleChatAccount *ha)
{
	const gchar *bufdata;
	gsize bufsize;
	gchar *len_end;
	gchar *len_str;
	guint len_len; //len len len len len len len len len
	gsize len;
	
	g_return_if_fail(ha);
	g_return_if_fail(ha->channel_buffer);
	
	while (ha->channel_buffer->len) {
		bufdata = (gchar *) ha->channel_buffer->data;
		bufsize = ha->channel_buffer->len;
		
		len_end = g_strstr_len(bufdata, bufsize, "\n");
		if (len_end == NULL) {
			// Not enough data to read
			if (purple_debug_is_verbose()) {
				purple_debug_info("googlechat", "Couldn't find length of chunk\n");
			}
			return;
		}
		len_len = len_end - bufdata;
		len_str = g_strndup(bufdata, len_len);
		len = (gsize) atoi(len_str);
		g_free(len_str);
		
		// Len was 0 ?  Must have been a bad read :(
		g_return_if_fail(len);
		
		bufsize = bufsize - len_len - 1;
		
		if (len > bufsize) {
			// Not enough data to read
			if (purple_debug_is_verbose()) {
				purple_debug_info("googlechat", "Couldn't read %" G_GSIZE_FORMAT " bytes when we only have %" G_GSIZE_FORMAT "\n", len, bufsize);
			}
			return;
		}
		
		googlechat_process_data_chunks(ha, bufdata + len_len + 1, len);
		
		g_byte_array_remove_range(ha->channel_buffer, 0, len + len_len + 1);
		
	}
}

static void
googlechat_set_auth_headers(GoogleChatAccount *ha, PurpleHttpRequest *request)
{
	purple_http_request_header_set_printf(request, "Authorization", "Bearer %s", ha->access_token);
	
	const gchar *request_url = purple_http_request_get_url(request);
	if (g_str_has_prefix(request_url, "https://chat.google.com/webchannel/") && ha->csessionid_param) {
		purple_http_request_header_set_printf(request, "Cookie", "COMPASS=dynamite=%s", ha->csessionid_param);
	}
}


static gboolean
googlechat_longpoll_request_content(PurpleHttpConnection *http_conn, PurpleHttpResponse *response, const gchar *buffer, size_t offset, size_t length, gpointer user_data)
{
	GoogleChatAccount *ha = user_data;
	
	ha->last_data_received = time(NULL);
	
	if (!purple_http_response_is_successful(response)) {
		purple_debug_error("googlechat", "longpoll_request_content had error: '%s'\n", purple_http_response_get_error(response));
		return FALSE;
	}
	
	g_byte_array_append(ha->channel_buffer, (guint8 *) buffer, length);
	
	googlechat_process_channel_buffer(ha);
	
	return TRUE;
}

static void
googlechat_longpoll_request_closed(PurpleHttpConnection *http_conn, PurpleHttpResponse *response, gpointer user_data)
{
	GoogleChatAccount *ha = user_data;
	
	if (!PURPLE_IS_CONNECTION(purple_http_conn_get_purple_connection(http_conn))) {
		return;
	}
	
	if (ha->channel_watchdog) {
		g_source_remove(ha->channel_watchdog);
		ha->channel_watchdog = 0;
	}
	
	// remaining data 'should' have been dealt with in googlechat_longpoll_request_content
	g_byte_array_free(ha->channel_buffer, TRUE);
	ha->channel_buffer = g_byte_array_sized_new(GOOGLECHAT_BUFFER_DEFAULT_SIZE);
	
	if (purple_http_response_get_error(response) != NULL) {
		//TODO error checking
		purple_debug_error("googlechat", "longpoll_request_closed %d %s\n", purple_http_response_get_code(response), purple_http_response_get_error(response));
		googlechat_fetch_channel_sid(ha);
	} else {
		googlechat_longpoll_request(ha);
	}
}

static gboolean
channel_watchdog_check(gpointer data)
{
	PurpleConnection *pc = data;
	GoogleChatAccount *ha;
	PurpleHttpConnection *conn;
	
	if (PURPLE_IS_CONNECTION(pc)) {
		ha = purple_connection_get_protocol_data(pc);
		conn = ha->channel_connection;
		
		if (ha->last_data_received && ha->last_data_received < (time(NULL) - 60)) {
			// should have been something within the last 60 seconds
			purple_http_conn_cancel(conn);
			ha->last_data_received = 0;
		}
		
		if (!purple_http_conn_is_running(conn)) {
			googlechat_longpoll_request(ha);
		}
		
		return TRUE;
	}
	
	return FALSE;
}

void
googlechat_longpoll_request(GoogleChatAccount *ha)
{
	PurpleHttpRequest *request;
	GString *url;
	
	url = g_string_new("https://chat.google.com/webchannel/events_encoded" "?");
	if (ha->csessionid_param) {
		g_string_append_printf(url, "csessionid=%s&", purple_url_encode(ha->csessionid_param)); //TODO optional?
	}
	g_string_append(url, "VER=8&");           // channel protocol version
	g_string_append(url, "RID=rpc&");         // request identifier
	g_string_append_printf(url, "SID=%s&", purple_url_encode(ha->sid_param));  // session ID
	//g_string_append_printf(url, "AID=%s&", purple_url_encode(ha->last_aid));  // TODO acknoledge message ID
	g_string_append(url, "CI=0&");            // 0 if streaming/chunked requests should be used
	g_string_append(url, "t=1&");             // trial
	g_string_append(url, "TYPE=xmlhttp&");    // type of request
	
	request = purple_http_request_new(NULL);
	purple_http_request_set_cookie_jar(request, ha->cookie_jar);
	purple_http_request_set_url(request, url->str);
	purple_http_request_set_timeout(request, -1);  // to infinity and beyond!
	purple_http_request_set_response_writer(request, googlechat_longpoll_request_content, ha);
	purple_http_request_set_keepalive_pool(request, ha->channel_keepalive_pool);
	
	googlechat_set_auth_headers(ha, request);
	
	ha->channel_connection = purple_http_request(ha->pc, request, googlechat_longpoll_request_closed, ha);
	
	g_string_free(url, TRUE);
	
	if (ha->channel_watchdog) {
		g_source_remove(ha->channel_watchdog);
	}
	ha->channel_watchdog = g_timeout_add_seconds(1, channel_watchdog_check, ha->pc);
}

static void
googlechat_fetch_channel_sid_cb(PurpleHttpConnection *http_conn, PurpleHttpResponse *response, gpointer user_data)
{
	GoogleChatAccount *ha = user_data;
	const gchar *initial_response;
	JsonNode *node;
	gchar *sid;
	
	//https://github.com/google/closure-library/blob/master/closure/goog/labs/net/webchannel/webchannelbase.js#L2193
	// [[0,["c","<sid>","",8,12,30000]]]);
	// c, sid, hostprefix, channelversion, serverversion, serverkeepalivems
	initial_response = purple_http_response_get_header(response, "X-HTTP-Initial-Response");
	
	node = json_decode(initial_response, -1);
	sid = googlechat_json_path_query_string(node, "$[0][1][1]", NULL);

	if (sid != NULL) {
		g_free(ha->sid_param);
		ha->sid_param = sid;
	}

	json_node_free(node);
	
	googlechat_longpoll_request(ha);
}

void
googlechat_fetch_channel_sid(GoogleChatAccount *ha)
{
	PurpleHttpRequest *request;
	GString *url;
	
	g_free(ha->sid_param);
	ha->sid_param = NULL;
	
	
	url = g_string_new("https://chat.google.com/webchannel/events_encoded" "?");
	g_string_append(url, "VER=8&");           // channel protocol version
	g_string_append(url, "RID=0&");           // request identifier
	g_string_append(url, "CVER=22&");         // client type
	g_string_append(url, "TYPE=init&");       // type of request
	g_string_append(url, "$req=count%3D0&");  // noop request
	g_string_append(url, "SID=null&");        
	g_string_append(url, "t=1&");             // trial
	
	request = purple_http_request_new(NULL);
	purple_http_request_set_cookie_jar(request, ha->cookie_jar);
	purple_http_request_set_url(request, url->str);
	purple_http_request_set_timeout(request, -1);  // to infinity and beyond!
	purple_http_request_set_keepalive_pool(request, ha->channel_keepalive_pool);
	
	googlechat_set_auth_headers(ha, request);
	
	purple_http_request(ha->pc, request, googlechat_fetch_channel_sid_cb, ha);
	purple_http_request_unref(request);
	
	g_string_free(url, TRUE);
}

static void
googlechat_register_webchannel_callback(PurpleHttpConnection *http_conn, PurpleHttpResponse *response, gpointer user_data)
{
	GoogleChatAccount *ha = user_data;
	gchar *compass_cookie = purple_http_cookie_jar_get(ha->cookie_jar, "COMPASS");
	
	if (g_str_has_prefix(compass_cookie, "dynamite=")) {
		const gchar *csessionid = &compass_cookie[9];
		if (csessionid && *csessionid) {
			g_free(ha->csessionid_param);
			ha->csessionid_param = g_strdup(csessionid);
		}
	}
	
	googlechat_fetch_channel_sid(ha);
}

void
googlechat_register_webchannel(GoogleChatAccount *ha)
{
	PurpleHttpRequest *request;
	
	request = purple_http_request_new(NULL);
	purple_http_request_set_cookie_jar(request, ha->cookie_jar);
	purple_http_request_set_url(request, "https://chat.google.com/webchannel/register");
	purple_http_request_set_method(request, "POST");
	purple_http_request_header_set(request, "Content-Type", "application/x-protobuf");
	
	googlechat_set_auth_headers(ha, request);
	
	purple_http_request(ha->pc, request, googlechat_register_webchannel_callback, ha);
	purple_http_request_unref(request);
}



typedef struct {
	GoogleChatAccount *ha;
	GoogleChatApiResponseFunc callback;
	ProtobufCMessage *response_message;
	gpointer user_data;
} LazyPblistRequestStore;

static void
googlechat_pblite_request_cb(PurpleHttpConnection *http_conn, PurpleHttpResponse *response, gpointer user_data)
{
	LazyPblistRequestStore *request_info = user_data;
	GoogleChatAccount *ha = request_info->ha;
	GoogleChatApiResponseFunc callback = request_info->callback;
	gpointer real_user_data = request_info->user_data;
	ProtobufCMessage *response_message = request_info->response_message;
	ProtobufCMessage *unpacked_message;
	const gchar *raw_response;
	guchar *decoded_response;
	gsize response_len;
	const gchar *content_type;
	
	if (purple_http_response_get_error(response) != NULL) {
		g_free(request_info);
		g_free(response_message);
		purple_debug_error("googlechat", "Error from server: (%s) %s\n", purple_http_response_get_error(response), purple_http_response_get_data(response, NULL));
		return; //TODO should we send NULL to the callee?
	}
	
	if (callback != NULL) {
		raw_response = purple_http_response_get_data(response, NULL);
		
		content_type = purple_http_response_get_header(response, "X-Goog-Safety-Content-Type");
		if (g_strcmp0(content_type, "application/x-protobuf") == 0) {
			decoded_response = g_base64_decode(raw_response, &response_len);
			unpacked_message = protobuf_c_message_unpack(response_message->descriptor, NULL, response_len, decoded_response);
			
			if (unpacked_message != NULL) {
				if (purple_debug_is_verbose()) {
					gchar *pretty_json = pblite_dump_json(unpacked_message);
					purple_debug_misc("googlechat", "Response: %s", pretty_json);
					g_free(pretty_json);
				}
				
				callback(ha, unpacked_message, real_user_data);
				protobuf_c_message_free_unpacked(unpacked_message, NULL);
			} else {
				purple_debug_error("googlechat", "Error decoding protobuf!\n");
			}
		} else {
			gchar *tidied_json = googlechat_json_tidy_blank_arrays(raw_response);
			JsonArray *response_array = json_decode_array(tidied_json, -1);
			const gchar *first_element = json_array_get_string_element(response_array, 0);
			gboolean ignore_first_element = (first_element != NULL);
			
			pblite_decode(response_message, response_array, ignore_first_element);
			if (ignore_first_element) {
				purple_debug_info("googlechat", "A '%s' says '%s'\n", response_message->descriptor->name, first_element);
			}
			
			if (purple_debug_is_verbose()) {
				gchar *pretty_json = pblite_dump_json(response_message);
				purple_debug_misc("googlechat", "Response: %s", pretty_json);
				g_free(pretty_json);
			}
			
			callback(ha, response_message, real_user_data);
			
			json_array_unref(response_array);
			g_free(tidied_json);
		}
	}
	
	g_free(request_info);
	g_free(response_message);
}

PurpleHttpConnection *
googlechat_raw_request(GoogleChatAccount *ha, const gchar *path, GoogleChatContentType request_type, const gchar *request_data, gssize request_len, GoogleChatContentType response_type, PurpleHttpCallback callback, gpointer user_data)
{
	PurpleHttpRequest *request;
	PurpleHttpConnection *connection;
	const gchar *response_type_str;
	
	switch (response_type) {
		default:
		case GOOGLECHAT_CONTENT_TYPE_NONE:
		case GOOGLECHAT_CONTENT_TYPE_JSON:
			response_type_str = "json";
			break;
		case GOOGLECHAT_CONTENT_TYPE_PBLITE:
			response_type_str = "protojson";
			break;
		case GOOGLECHAT_CONTENT_TYPE_PROTOBUF:
			response_type_str = "proto";
			break;
	}
	
	request = purple_http_request_new(NULL);
	purple_http_request_set_url_printf(request, GOOGLECHAT_PBLITE_API_URL "%s%calt=%s", path, (strchr(path, '?') ? '&' : '?'), response_type_str);
	purple_http_request_set_cookie_jar(request, ha->cookie_jar);
	purple_http_request_set_keepalive_pool(request, ha->api_keepalive_pool);
	purple_http_request_set_max_len(request, G_MAXINT32 - 1);
	
	purple_http_request_header_set(request, "X-Goog-Encode-Response-If-Executable", "base64");
	if (request_type != GOOGLECHAT_CONTENT_TYPE_NONE) {
		purple_http_request_set_method(request, "POST");
		purple_http_request_set_contents(request, request_data, request_len);
		if (request_type == GOOGLECHAT_CONTENT_TYPE_PROTOBUF) {
			purple_http_request_header_set(request, "Content-Type", "application/x-protobuf");
		} else if (request_type == GOOGLECHAT_CONTENT_TYPE_PBLITE) {
			purple_http_request_header_set(request, "Content-Type", "application/json+protobuf");
		} else if (request_type == GOOGLECHAT_CONTENT_TYPE_JSON) {
			purple_http_request_header_set(request, "Content-Type", "application/json");
		}
	}
	
	googlechat_set_auth_headers(ha, request);
	connection = purple_http_request(ha->pc, request, callback, user_data);
	purple_http_request_unref(request);
	
	return connection;
}

void
googlechat_api_request(GoogleChatAccount *ha, const gchar *endpoint, ProtobufCMessage *request_message, GoogleChatApiResponseFunc callback, ProtobufCMessage *response_message, gpointer user_data)
{
	gsize request_len;
	gchar *request_data;
	LazyPblistRequestStore *request_info = g_new0(LazyPblistRequestStore, 1);
	
	JsonArray *request_encoded = pblite_encode(request_message);
	JsonNode *node = json_node_new(JSON_NODE_ARRAY);
	json_node_take_array(node, request_encoded);
	request_data = json_encode(node, &request_len);
	json_node_free(node);
	
	request_info->ha = ha;
	request_info->callback = callback;
	request_info->response_message = response_message;
	request_info->user_data = user_data;
	
	if (purple_debug_is_verbose()) {
		gchar *pretty_json = pblite_dump_json(request_message);
		purple_debug_misc("googlechat", "Request:  %s", pretty_json);
		g_free(pretty_json);
	}
	
	googlechat_raw_request(ha, endpoint, GOOGLECHAT_CONTENT_TYPE_PROTOBUF, request_data, request_len, GOOGLECHAT_CONTENT_TYPE_PROTOBUF, googlechat_pblite_request_cb, request_info);
	
	g_free(request_data);
}


void
googlechat_default_response_dump(GoogleChatAccount *ha, ProtobufCMessage *response, gpointer user_data)
{
	gchar *dump = pblite_dump_json(response);
	purple_debug_info("googlechat", "%s\n", dump);
	g_free(dump);
}

/*
gboolean
googlechat_set_active_client(PurpleConnection *pc)
{
	GoogleChatAccount *ha;
	SetActiveClientRequest request;
	
	switch(purple_connection_get_state(pc)) {
		case PURPLE_CONNECTION_DISCONNECTED:
			// I couldn't eat another bite
			return FALSE;
		case PURPLE_CONNECTION_CONNECTING:
			// Come back for more later
			return TRUE;
		default:
			break;
	}
	
	ha = purple_connection_get_protocol_data(pc);
	if (ha == NULL) {
		g_warn_if_reached();
		return TRUE;
	}
	
	if (ha->active_client_state == ACTIVE_CLIENT_STATE__ACTIVE_CLIENT_STATE_IS_ACTIVE) {
		//We're already the active client
		return TRUE;
	}
	if (ha->idle_time > GOOGLECHAT_ACTIVE_CLIENT_TIMEOUT) {
		//We've gone idle
		return TRUE;
	}
	if (!purple_presence_is_status_primitive_active(purple_account_get_presence(ha->account), PURPLE_STATUS_AVAILABLE)) {
		//We're marked as not available somehow
		return TRUE;
	}
	ha->active_client_state = ACTIVE_CLIENT_STATE__ACTIVE_CLIENT_STATE_IS_ACTIVE;
	
	set_active_client_request__init(&request);
	
	request.request_header = googlechat_get_request_header(ha);
	request.has_is_active = TRUE;
	request.is_active = TRUE;
	request.full_jid = g_strdup_printf("%s/%s", purple_account_get_username(ha->account), ha->client_id);
	request.has_timeout_secs = TRUE;
	request.timeout_secs = GOOGLECHAT_ACTIVE_CLIENT_TIMEOUT;
	
	googlechat_pblite_set_active_client(ha, &request, (GoogleChatPbliteSetActiveClientResponseFunc)googlechat_default_response_dump, NULL);
	
	googlechat_request_header_free(request.request_header);
	g_free(request.full_jid);
	
	return TRUE;
}
*/

void
googlechat_search_results_send_im(PurpleConnection *pc, GList *row, void *user_data)
{
	PurpleAccount *account = purple_connection_get_account(pc);
	const gchar *who = g_list_nth_data(row, 0);
	PurpleIMConversation *imconv;
	
	imconv = purple_conversations_find_im_with_account(who, account);
	if (imconv == NULL) {
		imconv = purple_im_conversation_new(account, who);
	}
	purple_conversation_present(PURPLE_CONVERSATION(imconv));
}

void
googlechat_search_results_get_info(PurpleConnection *pc, GList *row, void *user_data)
{
	googlechat_get_info(pc, g_list_nth_data(row, 0));
}

void
googlechat_search_results_add_buddy(PurpleConnection *pc, GList *row, void *user_data)
{
	PurpleAccount *account = purple_connection_get_account(pc);

	if (!purple_blist_find_buddy(account, g_list_nth_data(row, 0)))
		purple_blist_request_add_buddy(account, g_list_nth_data(row, 0), "GoogleChat", g_list_nth_data(row, 1));
}

void
googlechat_search_users_text_cb(PurpleHttpConnection *connection, PurpleHttpResponse *response, gpointer user_data)
{
	GoogleChatAccount *ha = user_data;
	const gchar *response_data;
	size_t response_size;
	JsonArray *resultsarray;
	JsonObject *node;
	gint index, length;
	gchar *search_term;
	JsonObject *status;
	
	PurpleNotifySearchResults *results;
	PurpleNotifySearchColumn *column;
	
	if (purple_http_response_get_error(response) != NULL) {
		purple_notify_error(ha->pc, _("Search Error"), _("There was an error searching for the user"), purple_http_response_get_error(response), purple_request_cpar_from_connection(ha->pc));
		g_dataset_destroy(connection);
		return;
	}
	
	response_data = purple_http_response_get_data(response, &response_size);
	node = json_decode_object(response_data, response_size);
	
	search_term = g_dataset_get_data(connection, "search_term");
	resultsarray = json_object_get_array_member(node, "results");
	length = json_array_get_length(resultsarray);
	
	if (length == 0) {
		status = json_object_get_object_member(node, "status");
		
		if (!json_object_has_member(status, "personalResultsNotReady") || json_object_get_boolean_member(status, "personalResultsNotReady") == TRUE) {
			//Not ready yet, retry
			googlechat_search_users_text(ha, search_term);
			
		} else {		
			gchar *primary_text = g_strdup_printf(_("Your search for the user \"%s\" returned no results"), search_term);
			purple_notify_warning(ha->pc, _("No users found"), primary_text, "", purple_request_cpar_from_connection(ha->pc));
			g_free(primary_text);
		}
		
		g_dataset_destroy(connection);
		json_object_unref(node);
		return;
	}
	
	results = purple_notify_searchresults_new();
	if (results == NULL)
	{
		g_dataset_destroy(connection);
		json_object_unref(node);
		return;
	}
		
	/* columns: Friend ID, Name, Network */
	column = purple_notify_searchresults_column_new(_("ID"));
	purple_notify_searchresults_column_add(results, column);
	column = purple_notify_searchresults_column_new(_("Display Name"));
	purple_notify_searchresults_column_add(results, column);
	
	purple_notify_searchresults_button_add(results, PURPLE_NOTIFY_BUTTON_ADD, googlechat_search_results_add_buddy);
	purple_notify_searchresults_button_add(results, PURPLE_NOTIFY_BUTTON_INFO, googlechat_search_results_get_info);
	purple_notify_searchresults_button_add(results, PURPLE_NOTIFY_BUTTON_IM, googlechat_search_results_send_im);
	
	for(index = 0; index < length; index++)
	{
		JsonNode *result = json_array_get_element(resultsarray, index);
		
		gchar *id = googlechat_json_path_query_string(result, "$.person.personId", NULL);
		gchar *displayname = googlechat_json_path_query_string(result, "$.person.name[*].displayName", NULL);
		GList *row = NULL;
		
		row = g_list_append(row, id);
		row = g_list_append(row, displayname);
		
		purple_notify_searchresults_row_add(results, row);
	}
	
	purple_notify_searchresults(ha->pc, NULL, search_term, NULL, results, NULL, NULL);
	
	g_dataset_destroy(connection);
	json_object_unref(node);
}

/* 

POST https://people-pa.clients6.google.com/v2/people/lookup
id=actual_email_address%40gmail.com&type=EMAIL&matchType=LENIENT&requestMask.includeField.paths=person.email&requestMask.includeField.paths=person.gender&requestMask.includeField.paths=person.in_app_reachability&requestMask.includeField.paths=person.metadata&requestMask.includeField.paths=person.name&requestMask.includeField.paths=person.phone&requestMask.includeField.paths=person.photo&requestMask.includeField.paths=person.read_only_profile_info&extensionSet.extensionNames=GOOGLECHAT_ADDITIONAL_DATA&extensionSet.extensionNames=GOOGLECHAT_OFF_NETWORK_GAIA_LOOKUP&extensionSet.extensionNames=GOOGLECHAT_PHONE_DATA&coreIdParams.useRealtimeNotificationExpandedAcls=true&key=AIzaSyAfFJCeph-euFSwtmqFZi0kaKk-cZ5wufM

id=%2B123456789&type=PHONE&matchType=LENIENT&requestMask.includeField.paths=person.email&requestMask.includeField.paths=person.gender&requestMask.includeField.paths=person.in_app_reachability&requestMask.includeField.paths=person.metadata&requestMask.includeField.paths=person.name&requestMask.includeField.paths=person.phone&requestMask.includeField.paths=person.photo&requestMask.includeField.paths=person.read_only_profile_info&extensionSet.extensionNames=GOOGLECHAT_ADDITIONAL_DATA&extensionSet.extensionNames=GOOGLECHAT_OFF_NETWORK_GAIA_LOOKUP&extensionSet.extensionNames=GOOGLECHAT_PHONE_DATA&coreIdParams.useRealtimeNotificationExpandedAcls=true&quotaFilterType=PHONE&key=AIzaSyAfFJCeph-euFSwtmqFZi0kaKk-cZ5wufM

*/


void
googlechat_search_users_text(GoogleChatAccount *ha, const gchar *text)
{
	PurpleHttpRequest *request;
	GString *url = g_string_new("https://people-pa.clients6.google.com/v2/people/autocomplete?");
	PurpleHttpConnection *connection;
	
	g_string_append_printf(url, "query=%s&", purple_url_encode(text));
	g_string_append(url, "client=GOOGLECHAT_WITH_DATA&");
	g_string_append(url, "pageSize=20&");
	g_string_append_printf(url, "key=%s&", purple_url_encode(GOOGLE_GPLUS_KEY));
	
	request = purple_http_request_new(NULL);
	purple_http_request_set_cookie_jar(request, ha->cookie_jar);
	purple_http_request_set_url(request, url->str);
	
	googlechat_set_auth_headers(ha, request);

	connection = purple_http_request(ha->pc, request, googlechat_search_users_text_cb, ha);
	purple_http_request_unref(request);
	
	g_dataset_set_data_full(connection, "search_term", g_strdup(text), g_free);
	
	g_string_free(url, TRUE);
}

void
googlechat_search_users(PurpleProtocolAction *action)
{
	PurpleConnection *pc = purple_protocol_action_get_connection(action);
	GoogleChatAccount *ha = purple_connection_get_protocol_data(pc);
	
	purple_request_input(pc, _("Search for friends..."),
					   _("Search for friends..."),
					   NULL,
					   NULL, FALSE, FALSE, NULL,
					   _("_Search"), G_CALLBACK(googlechat_search_users_text),
					   _("_Cancel"), NULL,
					   purple_request_cpar_from_connection(pc),
					   ha);

}
