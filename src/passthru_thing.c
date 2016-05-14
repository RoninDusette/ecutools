/**
 * ecutools: IoT Automotive Tuning, Diagnostics & Analytics
 * Copyright (C) 2014  Jeremy Hahn
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

#include "passthru_thing.h"

void passthru_thing_shadow_onopen(passthru_shadow *shadow) {
  syslog(LOG_DEBUG, "passthru_thing_shadow_onopen");
}

void passthru_thing_shadow_ondelta(const char *pJsonValueBuffer, uint32_t valueLength, jsonStruct_t *pJsonStruct_t) {
  syslog(LOG_DEBUG, "passthru_thing_shadow_ondelta pJsonValueBuffer=%.*s", valueLength, pJsonValueBuffer);
  if(passthru_shadow_build_report_json(DELTA_REPORT, SHADOW_MAX_SIZE_OF_RX_BUFFER, pJsonValueBuffer, valueLength)) {
    messageArrivedOnDelta = true;
  }
}

void passthru_thing_shadow_onupdate(const char *pThingName, ShadowActions_t action, Shadow_Ack_Status_t status,
    const char *pReceivedJsonDocument, void *pContextData) {

  syslog(LOG_DEBUG, "passthru_thing_shadow_onupdate: pThingName=%s, pReceivedJsonDocument=%s, pContextData=%s", pThingName, pReceivedJsonDocument, (char *) pContextData);

  if(strncmp(pThingName, AWS_IOT_MY_THING_NAME, strlen(AWS_IOT_MY_THING_NAME)) == 0) { // Receiving message that was sent by this device

    shadow_message *message = passthru_shadow_parser_parse(pReceivedJsonDocument);

    // report: connected
    if(message && message->state && message->state->reported && message->state->reported->connected) {
      syslog(LOG_DEBUG, "passthru_thing_shadow_onupdate: connected=%s", message->state->reported->connected);
      if(strncmp(message->state->reported->connected, "false", strlen("false")) == 0) {
        passthru_thing_disconnect();
      }
    }

    // report: log
    if(message && message->state && message->state->reported && message->state->reported->log) {
      syslog(LOG_DEBUG, "passthru_thing_shadow_onupdate: log=%s", message->state->reported->log);

      // report: log: LOG_FILE
      if(strncmp(message->state->reported->log, "LOG_FILE", strlen("LOG_FILE")) == 0) {
        thing->logger->type = CANBUS_LOGTYPE_FILE;
        canbus_logger_run(thing->logger);
      }

      // report: log: LOG_AWSIOT
      if(strncmp(message->state->reported->log, "LOG_AWSIOT", strlen("LOG_AWSIOT")) == 0) {
        thing->logger->type = CANBUS_LOGTYPE_AWSIOT;
        canbus_logger_run(thing->logger);
      }

      // report: log: LOG_NONE
      if(strncmp(message->state->reported->log, "LOG_NONE", strlen("LOG_CANCEL")) == 0) {
        syslog(LOG_DEBUG, "passthru_thing_shadow_onupdate:  LOG_NONE");
        if(thing->logger->isrunning) {
          syslog(LOG_DEBUG, "passthru_thing_shadow_onupdate: stopping logger thread");
          canbus_logger_stop(thing->logger);
        }
      }
    }

    passthru_shadow_parser_free_message(message);
  }

  if(action == SHADOW_GET) {
    syslog(LOG_DEBUG, "passthru_thing_shadow_onupdate: SHADOW_GET");
  } 
  else if(action == SHADOW_UPDATE) {
    syslog(LOG_DEBUG, "passthru_thing_shadow_onupdate: SHADOW_UPDATE");
  } 
  else if(action == SHADOW_DELETE) {
    syslog(LOG_DEBUG, "passthru_thing_shadow_onupdate: SHADOW_DELETE");
  }

  if (status == SHADOW_ACK_TIMEOUT) {
    syslog(LOG_DEBUG, "Update Timeout--");
  }
  else if(status == SHADOW_ACK_REJECTED) {
    syslog(LOG_DEBUG, "Update Rejected");
  }
  else if(status == SHADOW_ACK_ACCEPTED) {
    syslog(LOG_DEBUG, "Update Accepted !!");
  }
}

void passthru_thing_shadow_onget(const char *pJsonValueBuffer, uint32_t valueLength, jsonStruct_t *pJsonStruct_t) {
  syslog(LOG_DEBUG, "passthru_thing_shadow_onget: message=%s", pJsonValueBuffer);
}

void passthru_thing_shadow_ondisconnect() {
  thing->state = THING_STATE_DISCONNECTED;
  syslog(LOG_DEBUG, "passthru_thing_shadow_ondisconnect: disconnected");
}

void passthru_thing_shadow_onerror(passthru_shadow *shadow, const char *message) {
  syslog(LOG_ERR, "passthru_thing_shadow_onerror: message=%s", message);
}

void *passthru_thing_shadow_yield_thread(void *ptr) {

  syslog(LOG_DEBUG, "passthru_thing_shadow_yield_thread: started");

  syslog(LOG_DEBUG, "passthru_thing_shadow_yield_thread: sending connect report to AWS IoT");
  passthru_thing_send_connect_report(thing);

  while((thing->state & THING_STATE_CONNECTED) || (thing->state & THING_STATE_CLOSING)) {

    thing->shadow->rc = aws_iot_shadow_yield(thing->shadow->mqttClient, 200);
    if(thing->shadow->rc == NETWORK_ATTEMPTING_RECONNECT) {
      syslog(LOG_DEBUG, "Attempting to reconnect to AWS IoT shadow service");
      sleep(1);
      continue;
    }

    if(messageArrivedOnDelta) {
      syslog(LOG_DEBUG, "Sending delta message to AWS IoT. message=%s", DELTA_REPORT);
      passthru_shadow_update(thing->shadow, DELTA_REPORT);
      messageArrivedOnDelta = false;
    }

    if(thing->state & THING_STATE_CLOSING) {
      if(passthru_thing_send_disconnect_report(thing) != 0) {
        syslog(LOG_ERR, "passthru_thing_shadow_yield_thread: failed to send disconnect report!");
        sleep(2);
        continue;
      }
    }

    //syslog(LOG_DEBUG, "passthru_thing_shadow_yield_thread: waiting for delta");
    sleep(1);
  }

  syslog(LOG_DEBUG, "passthru_thing_shadow_yield_thread: stopping");
  return NULL;
}

int passthru_thing_send_connect_report() {
  char pJsonDocument[SHADOW_MAX_SIZE_OF_RX_BUFFER];
  char state[255] = "{\"connected\": \"true\"}";
  if(!passthru_shadow_build_report_json(pJsonDocument, SHADOW_MAX_SIZE_OF_RX_BUFFER, state, strlen(state))) {
    syslog(LOG_ERR, "passthru_thing_send_connect_report: failed to build JSON state message. state=%s", state);
    return 1;
  }
  return passthru_shadow_update(thing->shadow, pJsonDocument);
}

int passthru_thing_send_disconnect_report() {
  char pJsonDocument[SHADOW_MAX_SIZE_OF_RX_BUFFER];
  char state[255] = "{\"connected\": \"false\"}";
  if(!passthru_shadow_build_report_json(pJsonDocument, SHADOW_MAX_SIZE_OF_RX_BUFFER, state, strlen(state))) {
    syslog(LOG_ERR, "passthru_thing_send_disconnect_report: failed to build JSON state message. state=%s", state);
    return 1;
  }
  return passthru_shadow_update(thing->shadow, pJsonDocument);
}

void passthru_thing_init(const char *thingId) {

  syslog(LOG_DEBUG, "%s", thingId);

  thing = malloc(sizeof(passthru_thing));
  thing->name = malloc(strlen(thingId)+1);
  strncpy(thing->name, thingId, strlen(thingId)+1);
  thing->logger = malloc(sizeof(canbus_logger));
  thing->logger->canbus = malloc(sizeof(canbus_client));

  thing->shadow = malloc(sizeof(passthru_shadow));
  memset(thing->shadow, 0, sizeof(passthru_shadow));

  thing->shadow->onopen = &passthru_thing_shadow_onopen;
  thing->shadow->ondelta = &passthru_thing_shadow_ondelta;
  thing->shadow->onupdate = &passthru_thing_shadow_onupdate;
  thing->shadow->onget = &passthru_thing_shadow_onget;
  thing->shadow->ondisconnect = &passthru_thing_shadow_ondisconnect;
  thing->shadow->onerror = &passthru_thing_shadow_onerror;
}

int passthru_thing_run() {
  thing->state = THING_STATE_CONNECTING;
  if(passthru_shadow_connect(thing->shadow) != 0) {
    syslog(LOG_CRIT, "passthru_thing_run: unable to connect to AWS IoT shadow service");
    return 1;
  }
  if(thing->shadow->rc != SUCCESS) {
    syslog(LOG_CRIT, "passthru_thing_run: unable to connect to AWS IoT shadow service");
    return 2;
  }
  thing->state = THING_STATE_CONNECTED;

  pthread_create(&thing->shadow->yield_thread, NULL, passthru_thing_shadow_yield_thread, NULL);
  pthread_join(thing->shadow->yield_thread, NULL);
  syslog(LOG_DEBUG, "passthru_passthru_thing: run loop complete");
  return 0;
}

void passthru_thing_disconnect() {
  thing->state = THING_STATE_DISCONNECTING;
  if(passthru_shadow_disconnect(thing->shadow) != 0) {
    syslog(LOG_ERR, "passthru_thing_disconnect: failed to disconnect from AWS IoT. rc=%d", thing->shadow->rc);
  }
}

void passthru_thing_close() {
  if(!(thing->state & THING_STATE_CONNECTED)) return; 
  syslog(LOG_DEBUG, "passthru_thing_close: closing thing. name=%s", thing->name);
  thing->state = THING_STATE_CLOSING;
  while(!(thing->state & THING_STATE_DISCONNECTED)) {
    syslog(LOG_DEBUG, "passthru_thing_close: waiting for thing to disconnect");
    sleep(1);
  }
  syslog(LOG_DEBUG, "passthru_thing_close: closed");
}

void passthru_thing_destroy() {
  syslog(LOG_DEBUG, "passthru_thing_destroy");
  passthru_shadow_destroy(thing->shadow);
  free(thing->logger->canbus);
  free(thing->logger);
  free(thing->shadow);
  free(thing);
}