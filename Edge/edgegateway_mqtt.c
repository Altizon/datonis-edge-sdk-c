/*
 * edgegateway_mqtt.c
 *
 * Provides an MQTT specific implementation for the edgegateway interface
 */
#if _COMMUNICATION_MODE_MQTT_
#include<curl/curl.h>
#include "edgegateway.h"
#include "edgeutil.h"
#include "MQTTClient.h"
#include "jsonutils.h"
#include "timeutil.h"
#include <string.h>
#include <stdio.h>
#include "timeutil.h"
#include "LZFcompress.h"
#include<cjson/cJSON.h>
/** Change this string on every new client you run **/
#define CLIENT_ID_PREFIX  "_EX_CLIENT_"
#define DATONIS_API_URL "https://api.datonis.io/api/v3/things/" 
/** Retries to wait for message ACK
 * Note that this number is sufficiently large.
 * i.e. it would at max wait for 5000 * 10 = 50 seconds for the ack
 * before it gives up.
 * Ideally you should get an Ack back within 10 milliseconds
 * This number is kept large to accomodate Datonis's temporary blips when the platform is upgraded
 */
#define RETRIES_FOR_ACK  5000

/** Globals required for MQTT based communication **/
static Client client;
static Network n;
static int http_return_code = -1;
static char client_writebuf[4096], client_readbuf[4096];
static struct thing *registered_things[10];
static int registered_thing_counter = 0;
static char http_ack_context[65];
static char client_id[50];
static char password[65];

struct MemoryStruct {
  char *memory;
  size_t size;
};

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
  size_t realsize = size * nmemb;
  struct MemoryStruct *mem = (struct MemoryStruct *)userp;

  char *ptr = (char*)realloc(mem->memory, mem->size + realsize + 1);
  if (ptr == NULL) {
    printf("Not enough memory (realloc returned NULL)\n");
    return 0;
  }

  mem->memory = ptr;
  memcpy(&(mem->memory[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->memory[mem->size] = 0;

  return realsize;
}

// Function to fetch thing data from Datonis API
char* fetch_thing_data(const char *thing_key,const char *ACCESS_KEY) {
    CURL *curl;
    CURLcode res;
    char url[512];  
    struct MemoryStruct chunk;

    chunk.memory = (char*)malloc(1);  
    chunk.size = 0;           

    // Construct the full URL
    snprintf(url, sizeof(url), "%s%s", DATONIS_API_URL, thing_key);


    curl_global_init(CURL_GLOBAL_DEFAULT);

    curl = curl_easy_init();
    if(curl) {
      curl_easy_setopt(curl, CURLOPT_URL, url);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

      // Add custom headers
      struct curl_slist *headers = NULL;
      char access_key_header[256];
      snprintf(access_key_header, sizeof(access_key_header), "X-Access-Key: %s", ACCESS_KEY);  

      headers = curl_slist_append(headers, access_key_header);
      headers = curl_slist_append(headers, "Content-Type: application/json");

      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

      /* Perform the request, res will get the return code */
      res = curl_easy_perform(curl);

      /* Check for errors */
      if(res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        free(chunk.memory);  // Important to free allocated memory
        chunk.memory = NULL; // Set to NULL after freeing to prevent dangling pointer.
      }
      else {
        /*
         * Now, our chunk.memory points to a memory block that is chunk.size
         * bytes big and contains the remote file.
         */
         //printf("%s\n", chunk.memory); // Optional: print the fetched JSON
      }

      /* always cleanup */
      curl_easy_cleanup(curl);
      curl_slist_free_all(headers);
    }

    curl_global_cleanup();

    return chunk.memory; // Return the allocated memory to the caller
}

int is_thing_registered(const char *json_string) {
    if (json_string == NULL) {
        printf("JSON string is NULL.\n");
        return 0;
    }

    cJSON *json = cJSON_Parse(json_string);
    if (json == NULL) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL) {
            fprintf(stderr, "Error before: %s\n", error_ptr);
        }
        return 0;
    }

    cJSON *thing = cJSON_GetObjectItemCaseSensitive(json, "thing");
    if (thing == NULL) {
        printf("thing object not found.\n");
        cJSON_Delete(json);
        return 0;
    }

    cJSON *state = cJSON_GetObjectItemCaseSensitive(thing, "state");
    if (state == NULL) {
        printf("state object not found.\n");
        cJSON_Delete(json);
        return 0;
    }

    if (!cJSON_IsString(state)) {
        printf("state is not a string.\n");
        cJSON_Delete(json);
        return 0;
    }

    if (strcmp(state->valuestring, "registered") == 0) {
        cJSON_Delete(json);
        return 1; // State is "registered"
    } else {
        cJSON_Delete(json);
        return 0; // State is not "registered"
    }
}


void handle_http_ack(MessageData *md) {
    MQTTMessage *message = md->message;
    char buf[1000];

    sprintf(buf, "%.*s", (int)message->payloadlen, (char *)message->payload);
    parse_http_ack(buf, http_ack_context, &http_return_code);
}

int connect_datonis_instance(char *server) {
    int rc = -1;

    sprintf(client_id, "%s%0.0f", CLIENT_ID_PREFIX, get_time_ms());

    get_hmac(configuration.access_key, password);
    printf("Connecting to Datonis with Username %s " , configuration.access_key);

    NewNetwork(&n);
    ConnectNetwork(&n, server, 1883);
    //ConnectNetwork(&n, "localhost", 1883);
    MQTTClient(&client, &n, 1000, (unsigned char*)client_writebuf, sizeof(client_writebuf), (unsigned char*)client_readbuf, sizeof(client_readbuf));

    MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
    /* TODO: Change this based on access key -- strncpy 22 chars */
    data.willFlag = 0;
    data.MQTTVersion = 3;
    data.clientID.cstring = client_id;
    data.username.cstring = configuration.access_key;
    data.password.cstring = password;
    data.keepAliveInterval = 20;
    data.cleansession = 1;

    rc = MQTTConnect(&client, &data);
    if (rc != ERR_OK) {
        return rc;
    }
    printf("\nSuccessfully connected to Datonis\n");

    strcpy(http_ack_context, "");

    /* First subscribe for topics we are interested in */
    sprintf(configuration.http_ack_topic, "Altizon/Datonis/%s/httpAck", client_id);
    return MQTTSubscribe(&client, configuration.http_ack_topic, QOS1, handle_http_ack);
}

int connect_datonis() {
    return connect_datonis_instance("telemetry.datonis.io");
}

int disconnect_datonis() {
    MQTTDisconnect(&client);
    n.disconnect(&n);
}

int send_data(enum QoS qos, char *topic, char *data, int flag) {
    unsigned char compressedBuffer[MAX_BLOCKSIZE + MAX_HDR_SIZE + 16];
    int ret = -1;
    MQTTMessage message;
    double begin, end;
    message.qos = qos;
    if(flag == 1 && strlen(data)>1024)
    {
        ssize_t len = 0;
        printf("\nOriginal Data Size: %zd", strlen(data));
	begin = get_time_ms();
	len = LZFcompress_data(data,compressedBuffer);
	end = get_time_ms();
	printf("\nCompression Time: %lf", (end - begin));
	printf("\nCompressed Data Size: %zd", len);
	sprintf(topic, "Altizon/Datonis/lzf/%s/event", client_id);
	message.payload = (void *)compressedBuffer;
	message.payloadlen = len;
    }
    else
    {
	message.payload = (void *)data;
        message.payloadlen = strlen(data);
    }
    begin = get_time_ms();
    ret = MQTTPublish(&client, topic, &message);
    end = get_time_ms();
    printf("\nData Transmission Time: %lf", (end-begin));

    return ret;
}

int encode_and_send_data(enum QoS qos, char *topic, char *json, int flag) {
    /* First calculate hash */
    char buf[65];
    char *hash = get_hmac(json, buf);
    int rc;
    int cnt = 0;
    double t1 = get_time_ms();
    double t2;

    /* Remove the last curly brace */
    json[strlen(json) - 1] = '\0';
    /* Append access key and hashcode */
    strcat(json, ",");
    putJSONStringAndComma(json, "hash", hash);
    putJSONStringAndComma(json, "access_key", configuration.access_key);
    putJSONDoubleAndComma(json, "aliot_protocol_version", 2.0);
    json[strlen(json) - 1] = '\0';
    /* Now end the JSON */
    endJSON(json);

    //printf("Json being sent: %s", json);
    rc = send_data(qos, topic, json, flag);
    if (rc != 0) {
        return rc;
    }

    http_return_code = 408;
    while (cnt != RETRIES_FOR_ACK) {
        // Wait for an ACK
        MQTTYield(&client, 10);
        // Check if the ACK is for the message we sent. If not continue to wait for some more time.
        if (((http_return_code != 408) && (!strcmp(http_ack_context, hash)))
            || ((http_return_code == 500) && (!strcmp(http_ack_context, "PARSE_ERROR")))) {
            // We got the expected acknowledge for THE MESSAGE WE SENT
            break;
        }
        cnt++;
        http_return_code = 408;
    }

    t2 = get_time_ms();
    printf("Sent data at topic: %s in: %0.0f millis, retries: %d, HTTP Ack from datonis: %d\n", topic, (t2 - t1), cnt, http_return_code);
    rc = translate_http_code(http_return_code);
    return rc;
}


void handle_instruction(MessageData *md) {
    MQTTMessage *message = md->message;
    MQTTString  *topic = md->topicName;

    char buf[2048];
    char to_validate[2048];
    char server_hash[65];
    char calculated_hash[65];
    char instruction[2048];
    char thing_key[50];
    char alert_key[25];
    char topic_str[1024];
    int i = 0;
    int rc = -1;

    sprintf(topic_str, "%.*s", topic->lenstring.len, topic->lenstring.data);
    sprintf(buf, "%.*s", (int)message->payloadlen, (char *)message->payload);
    rc = parse_instruction_json(buf, to_validate, server_hash, thing_key, alert_key, instruction);
    //printf("To validate: %s\n", to_validate);
    //printf("thing key: %s\nalert key: %s\ninstruction: %s\n", thing_key, alert_key, instruction);
    if (rc >= 0) {
        char *hash = get_hmac(to_validate, calculated_hash);
        //printf("Server hash: %s\n", server_hash);
        //printf("\nRe-calculated hash: %s\n", hash);
        if (!strcmp(server_hash, hash)) {
            //printf("\nHash from the server and re-calculated from data match! Returning instruction: %s", instruction);
            for (i = 0; i < registered_thing_counter; i++) {
                if (registered_things[i]->instruction_topic != NULL && !strcmp(registered_things[i]->instruction_topic, topic_str)) {
                    registered_things[i]->handler(thing_key, alert_key, instruction);
                }
            }
        } else {
            fprintf(stderr, "\nHash code for the instruction from the server does not match with the re-calculated one. Ignoring instruction!\n");
        }
    } else {
        fprintf(stderr, "\nCould not parse the JSON content for the instruction: %s\n", buf);
    }
}


int register_thing(struct thing *thing) {
    char *thing_data_json;
    int rc = -1;

    // 1. Fetch thing data from Datonis API
    thing_data_json = fetch_thing_data(thing->key,configuration.access_key); 
    printf("Thing data fetched: %s\n", thing_data_json);

    if (thing_data_json == NULL) {
        printf("Failed to fetch thing data from Datonis API.\n");
        return -1;  
    }


    // 2. Check if the state is "registered"
    if (!is_thing_registered(thing_data_json)) {
        printf("Thing is already registered.\n");
        // Thing is registered, proceed with registration logic
        char topic[500], jsonbuf[2048];
        char *json;

        sprintf(topic, "Altizon/Datonis/%s/register", client_id); 
        json = get_thing_register_json(jsonbuf, thing); 
        rc = encode_and_send_data(QOS1, topic, json, 0);  

        if (rc != ERR_OK) { 
            free(thing_data_json); // Free the JSON data before returning.
            return rc;
        }
        printf("Thing registered successfully.\n");
 
    } else {
        printf("Thing is already registered. Handling instruction only.\n");
    }
    if (thing->handler != NULL) {
            printf("Subscribing for instructions\n");
            sprintf(thing->instruction_topic, "Altizon/Datonis/%s/thing/%s/executeInstruction", configuration.access_key, thing->key); // Assume configuration.access_key is defined elsewhere.
            rc = MQTTSubscribe(&client, thing->instruction_topic, QOS2, handle_instruction); // Assume MQTTSubscribe and handle_instruction are defined elsewhere.
            printf("Subscribe return code: %d\n", rc);
    }
    else{
        rc = 0; 
    }
    
    free(thing_data_json);  // Important: Free the allocated memory
    return rc;
}

int transmit_thing_heartbeat(struct thing *thing) {
    char topic[200], jsonbuf[2048];
    memset(jsonbuf, '\0', sizeof(jsonbuf));
    sprintf(topic, "Altizon/Datonis/%s/heartbeat", client_id);
    char *json = get_thing_heartbeat_json(jsonbuf, thing);
    return encode_and_send_data(QOS0, topic, json,0);
}

int transmit_thing_data(struct thing *thing, char* value, char *waypoint) {
    char topic[200], jsonbuf[2048];
    memset(jsonbuf, '\0', sizeof(jsonbuf));

    char *json = get_thing_data_json(jsonbuf, thing, value, waypoint);
   	sprintf(topic, "Altizon/Datonis/%s/event", client_id);
    return encode_and_send_data(QOS1, topic, json,0);
}

int transmit_compressed_thing_data(struct thing *thing, char* value, char *waypoint) {
    char topic[200], jsonbuf[2048];
    memset(jsonbuf, '\0', sizeof(jsonbuf));

    char *json = get_thing_data_json(jsonbuf, thing, value, waypoint);
   	sprintf(topic, "Altizon/Datonis/%s/event", client_id);
    return encode_and_send_data(QOS1, topic, json,1);
}

int transmit_thing_alert(struct thing *thing, int level, char *message, char *data) {
    char topic[200], jsonbuf[4096];
    memset(jsonbuf, '\0', sizeof(jsonbuf));
    sprintf(topic, "Altizon/Datonis/%s/alert", client_id);
    char *json = get_thing_alert_json(jsonbuf, thing, level, message, data);
    return encode_and_send_data(QOS1, topic, json,0);
}

int transmit_instruction_feedback(char * alert_key, int level, char *message, char *data) {
    char topic[200], jsonbuf[4096];
    memset(jsonbuf, '\0', sizeof(jsonbuf));
    sprintf(topic, "Altizon/Datonis/%s/alert", client_id);
    char *json = get_instruction_alert_json(jsonbuf, alert_key, level, message, data);
    return encode_and_send_data(QOS1, topic, json,0);
}

int transmit_thing_data_packet(char *packet) {
    char topic[200];
    sprintf(topic, "Altizon/Datonis/%s/event", client_id);
    return encode_and_send_data(QOS1, topic, packet, 0);
}

int transmit_compressed_thing_data_packet(char *packet) {
    char topic[200];
    sprintf(topic, "Altizon/Datonis/%s/event", client_id);
    return encode_and_send_data(QOS1, topic, packet,1);
}
void yield(int milliseconds) {
    MQTTYield(&client, milliseconds);
}

#endif
