/******************************************************************************
 *  *
 * References:
 *  1. Stratum Protocol - [link](https://reference.cash/mining/stratum-protocol)
 *****************************************************************************/

#include "stratum_api.h"
#include "cJSON.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "utils.h"
#include <stdio.h>
#include <string.h>

#define BUFFER_SIZE 1024
static const char * TAG = "stratum_api";

static char * json_rpc_buffer = NULL;
static size_t json_rpc_buffer_size = 0;

// A message ID that must be unique per request that expects a response.
// For requests not expecting a response (called notifications), this is null.
static int send_uid = 1;

static void debug_stratum_tx(const char *);

void STRATUM_V1_initialize_buffer()
{
    json_rpc_buffer = malloc(BUFFER_SIZE);
    json_rpc_buffer_size = BUFFER_SIZE;
    memset(json_rpc_buffer, 0, BUFFER_SIZE);
    if (json_rpc_buffer == NULL) {
        printf("Error: Failed to allocate memory for buffer\n");
        exit(1);
    }
}

void cleanup_stratum_buffer()
{
    free(json_rpc_buffer);
}

static void realloc_json_buffer(size_t len)
{
    size_t old, new;

    old = strlen(json_rpc_buffer);
    new = old + len + 1;

    if (new < json_rpc_buffer_size) {
        return;
    }

    new = new + (BUFFER_SIZE - (new % BUFFER_SIZE));
    void * new_sockbuf = realloc(json_rpc_buffer, new);

    if (new_sockbuf == NULL) {
        fprintf(stderr, "Error: realloc failed in recalloc_sock()\n");
        esp_restart();
    }

    json_rpc_buffer = new_sockbuf;
    memset(json_rpc_buffer + old, 0, new - old);
    json_rpc_buffer_size = new;
}

char * STRATUM_V1_receive_jsonrpc_line(int sockfd)
{
    if (json_rpc_buffer == NULL) {
        STRATUM_V1_initialize_buffer();
    }
    char *line, *tok = NULL;
    char recv_buffer[BUFFER_SIZE];
    int nbytes;
    size_t buflen = 0;

    if (!strstr(json_rpc_buffer, "\n")) {
        do {
            memset(recv_buffer, 0, BUFFER_SIZE);
            nbytes = recv(sockfd, recv_buffer, BUFFER_SIZE - 1, 0);
            if (nbytes == -1) {
                perror("recv");
                esp_restart();
            }

            realloc_json_buffer(nbytes);
            strncat(json_rpc_buffer, recv_buffer, nbytes);
        } while (!strstr(json_rpc_buffer, "\n"));
    }
    buflen = strlen(json_rpc_buffer);
    tok = strtok(json_rpc_buffer, "\n");
    line = strdup(tok);
    int len = strlen(line);
    if (buflen > len + 1)
        memmove(json_rpc_buffer, json_rpc_buffer + len + 1, buflen - len + 1);
    else
        strcpy(json_rpc_buffer, "");
    return line;
}

void STRATUM_V1_parse(StratumApiV1Message * message, const char * stratum_json)
{
    cJSON * json = cJSON_Parse(stratum_json);

    cJSON * id_json = cJSON_GetObjectItem(json, "id");
    int16_t parsed_id = -1;
    if (id_json != NULL && cJSON_IsNumber(id_json)) {
        parsed_id = id_json->valueint;
    }
    message->message_id = parsed_id;

    cJSON * method_json = cJSON_GetObjectItem(json, "method");
    stratum_method result = STRATUM_UNKNOWN;
    if (method_json != NULL && cJSON_IsString(method_json)) {
        if (strcmp("mining.notify", method_json->valuestring) == 0) {
            result = MINING_NOTIFY;
        } else if (strcmp("mining.set_difficulty", method_json->valuestring) == 0) {
            result = MINING_SET_DIFFICULTY;
        } else if (strcmp("mining.set_version_mask", method_json->valuestring) == 0) {
            result = MINING_SET_VERSION_MASK;
        }
    } else {
        // parse results
        cJSON * result_json = cJSON_GetObjectItem(json, "result");
        if (result_json == NULL){
            message->response_success = false;
        }
        else if (cJSON_IsBool(result_json)) {
            result = STRATUM_RESULT;
            if (cJSON_IsTrue(result_json)) {
                message->response_success = true;
            }else{
                message->response_success = false;
            }
        } else {
            cJSON * mask = cJSON_GetObjectItem(result_json, "version-rolling.mask");
            if (mask != NULL) {
                result = STRATUM_RESULT_VERSION_MASK;
                message->version_mask = strtoul(mask->valuestring, NULL, 16);
            }
        }
    }

    message->method = result;

    if (message->method == MINING_NOTIFY) {

        mining_notify * new_work = malloc(sizeof(mining_notify));
        // new_work->difficulty = difficulty;
        cJSON * params = cJSON_GetObjectItem(json, "params");
        new_work->job_id = strdup(cJSON_GetArrayItem(params, 0)->valuestring);
        new_work->prev_block_hash = strdup(cJSON_GetArrayItem(params, 1)->valuestring);
        new_work->coinbase_1 = strdup(cJSON_GetArrayItem(params, 2)->valuestring);
        new_work->coinbase_2 = strdup(cJSON_GetArrayItem(params, 3)->valuestring);

        cJSON * merkle_branch = cJSON_GetArrayItem(params, 4);
        new_work->n_merkle_branches = cJSON_GetArraySize(merkle_branch);
        if (new_work->n_merkle_branches > MAX_MERKLE_BRANCHES) {
            printf("Too many Merkle branches.\n");
            abort();
        }
        new_work->merkle_branches = malloc(HASH_SIZE * new_work->n_merkle_branches);
        for (size_t i = 0; i < new_work->n_merkle_branches; i++) {
            hex2bin(cJSON_GetArrayItem(merkle_branch, i)->valuestring, new_work->merkle_branches + HASH_SIZE * i, HASH_SIZE * 2);
        }

        new_work->version = strtoul(cJSON_GetArrayItem(params, 5)->valuestring, NULL, 16);
        new_work->target = strtoul(cJSON_GetArrayItem(params, 6)->valuestring, NULL, 16);
        new_work->ntime = strtoul(cJSON_GetArrayItem(params, 7)->valuestring, NULL, 16);

        message->mining_notification = new_work;

        // params can be varible length
        int paramsLength = cJSON_GetArraySize(params);
        int value = cJSON_IsTrue(cJSON_GetArrayItem(params, paramsLength - 1));
        message->should_abandon_work = value;
    } else if (message->method == MINING_SET_DIFFICULTY) {
        cJSON * params = cJSON_GetObjectItem(json, "params");
        uint32_t difficulty = cJSON_GetArrayItem(params, 0)->valueint;

        message->new_difficulty = difficulty;
    } else if (message->method == MINING_SET_VERSION_MASK) {

        cJSON * params = cJSON_GetObjectItem(json, "params");
        uint32_t version_mask = strtoul(cJSON_GetArrayItem(params, 0)->valuestring, NULL, 16);
        message->version_mask = version_mask;
    }

    cJSON_Delete(json);
}

void STRATUM_V1_free_mining_notify(mining_notify * params)
{
    free(params->job_id);
    free(params->prev_block_hash);
    free(params->coinbase_1);
    free(params->coinbase_2);
    free(params->merkle_branches);
    free(params);
}

int _parse_stratum_subscribe_result_message(const char * result_json_str, char ** extranonce, int * extranonce2_len)
{
    cJSON * root = cJSON_Parse(result_json_str);
    if (root == NULL) {
        ESP_LOGE(TAG, "Unable to parse %s", result_json_str);
        return -1;
    }
    cJSON * result = cJSON_GetObjectItem(root, "result");
    if (result == NULL) {
        ESP_LOGE(TAG, "Unable to parse subscribe result %s", result_json_str);
        return -1;
    }

    cJSON * extranonce2_len_json = cJSON_GetArrayItem(result, 2);
    if (extranonce2_len_json == NULL) {
        ESP_LOGE(TAG, "Unable to parse extranonce2_len: %s", result->valuestring);
        return -1;
    }
    *extranonce2_len = extranonce2_len_json->valueint;

    cJSON * extranonce_json = cJSON_GetArrayItem(result, 1);
    if (extranonce_json == NULL) {
        ESP_LOGE(TAG, "Unable parse extranonce: %s", result->valuestring);
        return -1;
    }
    *extranonce = malloc(strlen(extranonce_json->valuestring) + 1);
    strcpy(*extranonce, extranonce_json->valuestring);

    cJSON_Delete(root);

    return 0;
}

int STRATUM_V1_subscribe(int socket, char ** extranonce, int * extranonce2_len, char * model)
{
    // Subscribe
    char subscribe_msg[BUFFER_SIZE];
    sprintf(subscribe_msg, "{\"id\": %d, \"method\": \"mining.subscribe\", \"params\": [\"bitaxe/%s\"]}\n", send_uid++, model);
    debug_stratum_tx(subscribe_msg);
    write(socket, subscribe_msg, strlen(subscribe_msg));
    char * line;
    line = STRATUM_V1_receive_jsonrpc_line(socket);
    ESP_LOGI(TAG, "Received result %s", line);

    _parse_stratum_subscribe_result_message(line, extranonce, extranonce2_len);

    free(line);

    return 1;
}

int STRATUM_V1_suggest_difficulty(int socket, uint32_t difficulty)
{
    char difficulty_msg[BUFFER_SIZE];
    sprintf(difficulty_msg, "{\"id\": %d, \"method\": \"mining.suggest_difficulty\", \"params\": [%ld]}\n", send_uid++, difficulty);
    debug_stratum_tx(difficulty_msg);
    write(socket, difficulty_msg, strlen(difficulty_msg));

    /* TODO: fix race condition with first mining.notify message
    char * line;
    line = STRATUM_V1_receive_jsonrpc_line(socket);

    ESP_LOGI(TAG, "Received result %s", line);

    free(line);
    */

    return 1;
}

int STRATUM_V1_authenticate(int socket, const char * username, const char * pass)
{
    char authorize_msg[BUFFER_SIZE];
    sprintf(authorize_msg, "{\"id\": %d, \"method\": \"mining.authorize\", \"params\": [\"%s\", \"%s\"]}\n", send_uid++, username,
            pass);
    debug_stratum_tx(authorize_msg);

    write(socket, authorize_msg, strlen(authorize_msg));

    return 1;
}

/// @param socket Socket to write to
/// @param username The client’s user name.
/// @param jobid The job ID for the work being submitted.
/// @param ntime The hex-encoded time value use in the block header.
/// @param extranonce_2 The hex-encoded value of extra nonce 2.
/// @param nonce The hex-encoded nonce value to use in the block header.
void STRATUM_V1_submit_share(int socket, const char * username, const char * jobid, const char * extranonce_2, const uint32_t ntime,
                             const uint32_t nonce, const uint32_t version)
{
    char submit_msg[BUFFER_SIZE];
    sprintf(submit_msg,
            "{\"id\": %d, \"method\": \"mining.submit\", \"params\": [\"%s\", \"%s\", \"%s\", \"%08lx\", \"%08lx\", \"%08lx\"]}\n",
            send_uid++, username, jobid, extranonce_2, ntime, nonce, version);
    debug_stratum_tx(submit_msg);
    write(socket, submit_msg, strlen(submit_msg));
}

void STRATUM_V1_configure_version_rolling(int socket, uint32_t * version_mask)
{
    char configure_msg[BUFFER_SIZE * 2];
    sprintf(configure_msg,
            "{\"id\": %d, \"method\": \"mining.configure\", \"params\": [[\"version-rolling\"], {\"version-rolling.mask\": "
            "\"ffffffff\"}]}\n",
            send_uid++);
    ESP_LOGI(TAG, "tx: %s", configure_msg);
    write(socket, configure_msg, strlen(configure_msg));

    char * line = STRATUM_V1_receive_jsonrpc_line(socket);
    cJSON * json = cJSON_Parse(line);

    ESP_LOGI(TAG, "Received result %s", line);

    cJSON * result = cJSON_GetObjectItem(json, "result");
    if (result != NULL) {
        cJSON * version_rolling_enabled = cJSON_GetObjectItem(result, "version-rolling");
        if (cJSON_IsBool(version_rolling_enabled) && cJSON_IsTrue(version_rolling_enabled)) {
            cJSON * mask = cJSON_GetObjectItem(result, "version-rolling.mask");
            uint32_t version_mask = strtoul(mask->valuestring, NULL, 16);
            ESP_LOGI(TAG, "Set version mask: %08lx", version_mask);
        }
    }else{
        printf("configure_version result null\n");
    }

    cJSON_Delete(json);
    free(line);

    return;
}

static void debug_stratum_tx(const char * msg)
{
    ESP_LOGI(TAG, "tx: %s", msg);
}
