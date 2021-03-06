/*
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 * 
 * producer
 * 
 * This sample demonstrates how to send messages to a topic
 * using an amqp address prefix.
 */

#include <proton/connection.h>
#include <proton/condition.h>
#include <proton/object.h>
#include <proton/delivery.h>
#include <proton/link.h>
#include <proton/message.h>
#include <proton/proactor.h>
#include <proton/session.h>
#include <proton/transport.h>
#include <proton/sasl.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "util.h"

typedef struct app_data_t {
  const char *host, *port;
  const char *username, *password;
  const char *amqp_address;
  char *amqp_topic_prefix;
  const char *container_id;
  int message_count;

  pn_proactor_t *proactor;
  pn_rwbytes_t message_buffer;
  int sent;
  int acknowledged;
} app_data_t;

static int exit_code = 0;

extern int optind;
extern char* optarg;
extern int optopt;
extern int opterr;

#define str_free(strptr) free((void *)strptr)

static void check_condition(pn_event_t *e, pn_condition_t *cond) {
  if (pn_condition_is_set(cond)) {
    fprintf(stderr, "%s: %s: %s\n", pn_event_type_name(pn_event_type(e)),
            pn_condition_get_name(cond), pn_condition_get_description(cond));
    pn_data_t* info = pn_condition_info(cond);
    if (info && !pn_data_is_null(info)) {
    	size_t len = 128;
        char *buf = (char *)malloc(len);
        int rc = 0;
        do {
            rc = pn_data_format(info, buf, &len);
            if (rc == PN_OVERFLOW) {
                free(buf);
                len *= 2;
                buf = (char *)malloc(len);
            }
        } while (rc == PN_OVERFLOW);
        
        fprintf(stderr, "Err info: %s\n", buf);
        free(buf);

    }
    pn_connection_close(pn_event_connection(e));
    exit_code = 1;
  }
}

#define TOPIC_PREFIX_KEY "topic-prefix"
#define TOPIC_PREFIX_KEY_SIZE sizeof(TOPIC_PREFIX_KEY)
#define TOPIC_PREFIX_KEY_LEN TOPIC_PREFIX_KEY_SIZE -1

/* 
 * Certain brokers have AMQP connections may advertise their topic prefix in the 
 * connection open response. It is possible to read the topic prefix value from 
 * the connection open response. Solace Pubsub+ advertises the topic prefix
 * with the 'topic-prefix' property key.
 * 
 * set_topic_prefix_from_connection checks the remote AMQP connection properties 
 * for the 'topic-prefix' property key and reads and assign the amqp topic prefix
 * to app_data_t if the property is present.
 *
 * */
static int set_topic_prefix_from_connection(app_data_t* app, pn_connection_t* pnc){
    pn_data_t* properties = pn_connection_remote_properties(pnc);
    static const size_t amqp_topic_prefix_len = 255;
    char amqp_topic_prefix[amqp_topic_prefix_len];
    /* get remote connection property value */
    int rc = get_data_map_string_property(properties, 
                                          TOPIC_PREFIX_KEY, 
                                          amqp_topic_prefix, 
                                          amqp_topic_prefix_len);
    /* 
     * < 0 indicates not found or invalid arguments
     * 
     * = 0 indicates property is present but is not representable as a string 
     * or does not fit in buffer
     * 
     * > 0 indicates property is found and was written into amqp_topic_prefix
     * */
    if (rc > 0) {
        /* found store in app data to format amqp address */
        str_free(app->amqp_topic_prefix);
        app->amqp_topic_prefix = strdup(amqp_topic_prefix);
    }
    return rc;
}

/* Create a message with a string "sequence_<number>" encode it and return the encoded buffer. */
static pn_bytes_t encode_message(app_data_t* app) {
  /* Construct a message with the string "sequence_<app.sent>" */
  pn_message_t* message = pn_message();
  pn_data_t* body = pn_message_body(message);
  /* Create string for amqp message body */
  size_t slen = sizeof("sequence_") + 12;
  char* sbuf = malloc(slen);
  int swritten = sprintf(sbuf, "sequence_%d", app->sent);
  if (swritten < 0) {
    fprintf(stderr, "error writing message body string for sequence %d", app->sent);
    exit(1);
  }
  pn_data_put_string(body, pn_bytes(swritten, sbuf));

  /* set message durable flag */
  pn_message_set_durable(message, true);

  /* encode the message, expanding the encode buffer as needed */
  if (app->message_buffer.start == NULL) {
    static const size_t initial_size = 128;
    app->message_buffer = pn_rwbytes(initial_size, (char*)malloc(initial_size));
  }
  /* app->message_buffer is the total buffer space available. */
  /* mbuf wil point at just the portion used by the encoded message */
  {
  pn_rwbytes_t mbuf = pn_rwbytes(app->message_buffer.size, app->message_buffer.start);
  int status = 0;
  while ((status = pn_message_encode(message, mbuf.start, &mbuf.size)) == PN_OVERFLOW) {
    app->message_buffer.size *= 2;
    app->message_buffer.start = (char*)realloc(app->message_buffer.start, app->message_buffer.size);
    mbuf.size = app->message_buffer.size;
    mbuf.start = app->message_buffer.start;
  }
  if (status != 0) {
    fprintf(stderr, "error encoding message: %s\n", pn_error_text(pn_message_error(message)));
    exit(1);
  }
  pn_message_free(message);
  return pn_bytes(mbuf.size, mbuf.start);
  }
}

/* Returns true to continue, false if finished */
static bool handle(app_data_t* app, pn_event_t* event) {
  switch (pn_event_type(event)) {

   case PN_CONNECTION_INIT: {
     pn_connection_t* c = pn_event_connection(event);
     /* Set authenticate creadentials if present */
     if (app->username) {
        pn_connection_set_user(c, app->username);
        pn_connection_set_password(c, app->password);
     }
     pn_connection_set_container(c, app->container_id);
     pn_connection_open(c);
     break;
   }
    
   case PN_CONNECTION_REMOTE_OPEN: {
     char amqp_topic[PN_MAX_ADDR];
     pn_connection_t* c = pn_event_connection(event);
     set_topic_prefix_from_connection(app, c);
     pn_session_t* s = pn_session(c);
     pn_session_open(s);
     {
     pn_link_t* l = pn_sender(s, "my_sender");
     /* add topic prefix to amqp address */
     if(amqp_destination_address(
        amqp_topic, PN_MAX_ADDR,
        app->amqp_address, strlen(app->amqp_address),
        app->amqp_topic_prefix, strlen(app->amqp_topic_prefix) 
        ) < 0) {
        exit_code=1;
        return false;
     }
     printf("setting amqp topic:'%s'\n", amqp_topic);
     pn_terminus_set_address(pn_link_target(l), amqp_topic);
     pn_link_open(l);
     break;
     }

   }

   case PN_LINK_FLOW: {
     /* The peer has given us some credit, now we can send messages */
     pn_link_t *sender = pn_event_link(event);
     while (pn_link_credit(sender) > 0 && app->sent < app->message_count) {
       ++app->sent;
       /* Use sent counter as unique delivery tag. */
       pn_delivery(sender, pn_dtag((const char *)&app->sent, sizeof(app->sent)));
       {
       pn_bytes_t msgbuf = encode_message(app);
       pn_link_send(sender, msgbuf.start, msgbuf.size);
       }
       pn_link_advance(sender);
     }
     break;
   }

   case PN_DELIVERY: {
     /* We received acknowledgement from the peer that a message was delivered. */
     pn_delivery_t* d = pn_event_delivery(event);
     if (pn_delivery_remote_state(d) == PN_ACCEPTED) {
       if (++app->acknowledged == app->message_count) {
         printf("%d messages sent and acknowledged\n", app->acknowledged);
         pn_connection_close(pn_event_connection(event));
         /* Continue handling events till we receive TRANSPORT_CLOSED */
       }
     } else {
       pn_disposition_t* disposition = pn_delivery_remote(d);
       fprintf(stderr, "unexpected delivery state %d\n", (int)pn_delivery_remote_state(d));
       check_condition(event, pn_disposition_condition(disposition));
       pn_connection_close(pn_event_connection(event));
       exit_code=1;
     }
     break;
   }

   case PN_TRANSPORT_CLOSED:
    check_condition(event, pn_transport_condition(pn_event_transport(event)));
    break;

   case PN_CONNECTION_REMOTE_CLOSE:
    check_condition(event, pn_connection_remote_condition(pn_event_connection(event)));
    pn_connection_close(pn_event_connection(event));
    break;

   case PN_SESSION_REMOTE_CLOSE:
    check_condition(event, pn_session_remote_condition(pn_event_session(event)));
    pn_connection_close(pn_event_connection(event));
    break;

   case PN_LINK_REMOTE_CLOSE:
   case PN_LINK_REMOTE_DETACH:
    check_condition(event, pn_link_remote_condition(pn_event_link(event)));
    pn_connection_close(pn_event_connection(event));
    break;

   case PN_PROACTOR_INACTIVE:
    return false;

   default: break;
  }
  return true;
}

void run(app_data_t *app) {
  /* Loop and handle events */
  do {
    pn_event_batch_t *events = pn_proactor_wait(app->proactor);
    pn_event_t *e;
    for (e = pn_event_batch_next(events); e; e = pn_event_batch_next(events)) {
      if (!handle(app, e)) {
        return;
      }
    }
    pn_proactor_done(app->proactor, events);
  } while(true);
}

#define DEFAULT_AMQP_TOPIC_PREFIX "topic://"

#define AMQP_TOPIC_PREFIX DEFAULT_AMQP_TOPIC_PREFIX

#define AMQP_TOPIC_PREFIX_SIZE sizeof(AMQP_TOPIC_PREFIX)

#define AMQP_TOPIC_PREFIX_LEN AMQP_TOPIC_PREFIX_SIZE -1

void usage(void) {
    printf("Usage: producer [options] \n");
    printf("\t-a      The host address [localhost]\n");
    printf("\t-p      The host port [5672]\n");
    printf("\t-c      # of messages to send [10]\n");
    printf("\t-t      Target address topic [my_topic]\n");
    printf("\t-i      AMQP Container id [producer:<pid>]\n");
    printf("\t-u      Client authentication username []\n");
    printf("\t-P      Client authentication password []\n");
    printf("\t-h      Displays this message\n");
    exit(0);

}

void parse_args(int argc, char **argv, app_data_t *app){
    char c;
    char con_id[PN_MAX_ADDR];
    if (container_id(con_id, PN_MAX_ADDR, argv[0], sizeof(argv[0])) < 0){
        fprintf(stderr, "Unable to format container id from source: %s", argv[0]);
        exit(1);
    }
    /* initialize default values*/
    app->container_id = strdup(con_id); /* default to using argv[0] */
    app->host = "localhost";
    app->port = "amqp";
    app->message_count = 10;
    app->username = NULL;
    app->password = NULL;
    app->amqp_address = "my_topic";
    /* 
     * Set a default amqp topic prefix since broker do not always
     * advertise a topic prefix. 
     * The 'topic://' is the address prefix for topics for the 
     * Solace PubSub+ Message Broker.
     * */
    app->amqp_topic_prefix = (char*) malloc(sizeof(char) * AMQP_TOPIC_PREFIX_SIZE);
    strncpy(app->amqp_topic_prefix, AMQP_TOPIC_PREFIX, AMQP_TOPIC_PREFIX_SIZE);

    /* command line options */
    opterr = 0;
    while((c = getopt(argc, argv, "i:a:c:t:p:P:u:h")) != -1) {
        switch(c) {
        case 'h': usage(); break;
        case 'c': 
            app->message_count = atoi(optarg); 
            if (app->message_count < 0) usage();
            break;
        case 'a': app->host = optarg; break;
        case 'i': 
            if (container_id(con_id, PN_MAX_ADDR, optarg, sizeof(optarg)) < 0) {
                fprintf(stderr, "Unable to format container id from source: %s", optarg);
                exit(1);
            }
            str_free(app->container_id);
            app->container_id = strdup(con_id);
            break;
        case 't': app->amqp_address = optarg; break;
        case 'p': app->port = optarg; break;
        case 'P': app->password = optarg; break;
        case 'u': app->username = optarg; break;
        default: usage(); break;
        }
    }

}

int main(int argc, char **argv) {
    struct app_data_t app = {0};
    char addr[PN_MAX_ADDR];
  
    parse_args(argc, argv, &app);
    
    app.proactor = pn_proactor();
    pn_proactor_addr(addr, sizeof(addr), app.host, app.port);
    /* Initial Sasl transport for authentication */
    pn_transport_t *pnt = pn_transport();
    pn_sasl_t *sasl = pn_sasl(pnt);
    pn_sasl_set_allow_insecure_mechs(sasl, true);
    
    pn_proactor_connect2(app.proactor, NULL, pnt, addr);
    run(&app);
    pn_proactor_free(app.proactor);
    /* free app data */
    free(app.message_buffer.start);
    str_free(app.container_id);
    str_free(app.amqp_topic_prefix);
    return exit_code;
}
