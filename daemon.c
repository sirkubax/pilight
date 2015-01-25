/*
	Copyright (C) 2013 - 2014 CurlyMo

	This file is part of pilight.

    pilight is free software: you can redistribute it and/or modify it under the
	terms of the GNU General Public License as published by the Free Software
	Foundation, either version 3 of the License, or (at your option) any later
	version.

    pilight is distributed in the hope that it will be useful, but WITHOUT ANY
	WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
	A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with pilight. If not, see	<http://www.gnu.org/licenses/>
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <pthread.h>
#include <ctype.h>
#include <dirent.h>

#include "pilight.h"
#include "datetime.h"
#include "common.h"
#include "settings.h"
#include "config.h"
#include "gc.h"
#include "log.h"
#include "options.h"
#include "threads.h"
#include "socket.h"
#include "json.h"
#include "wiringPi.h"
#include "irq.h"
#include "hardware.h"
#include "ssdp.h"
#include "dso.h"
#include "firmware.h"
#include "proc.h"

#ifdef UPDATE
	#include "update.h"
#endif

#ifdef WEBSERVER
	#include "webserver.h"
#endif

typedef enum {
	RECEIVER,
	SENDER,
	CONTROLLER,
	NODE,
	GUI,
	WEB
} client_type_t;

static char clients[6][11] = {
	"receiver",
	"sender",
	"controller",
	"node",
	"gui",
	"web"
};

typedef struct nodes_t {
	char uuid[21];
	int client_id;
	struct nodes_t *next;
} nodes_t;

static struct nodes_t *nodes = NULL;

typedef enum {
	WELCOME,
	IDENTIFY,
	REJECT,
	REQUEST,
	CONFIG,
	FORWARD
} steps_t;

typedef struct sendqueue_t {
	unsigned int id;
	char *message;
	char *protoname;
	char *settings;
	struct protocol_t *protopt;
	int code[255];
	char uuid[UUID_LENGTH];
	struct sendqueue_t *next;
} sendqueue_t;

static struct sendqueue_t *sendqueue;
static struct sendqueue_t *sendqueue_head;

typedef struct recvqueue_t {
	int raw[255];
	int rawlen;
	int hwtype;
	int plslen;
	struct recvqueue_t *next;
} recvqueue_t;

static struct recvqueue_t *recvqueue;
static struct recvqueue_t *recvqueue_head;

static pthread_mutex_t sendqueue_lock;
static pthread_cond_t sendqueue_signal;
static pthread_mutexattr_t sendqueue_attr;

static int sendqueue_number = 0;

static pthread_mutex_t receive_lock;
static pthread_cond_t receive_signal;
static pthread_mutexattr_t receive_attr;

static int recvqueue_number = 0;

static pthread_mutex_t recvqueue_lock;
static pthread_cond_t recvqueue_signal;
static pthread_mutexattr_t recvqueue_attr;

typedef struct bcqueue_t {
	JsonNode *jmessage;
	char *protoname;
	struct bcqueue_t *next;
} bcqueue_t;

static struct bcqueue_t *bcqueue = NULL;
static struct bcqueue_t *bcqueue_head = NULL;

static pthread_mutex_t bcqueue_lock;
static pthread_cond_t bcqueue_signal;
static pthread_mutexattr_t bcqueue_attr;

static int bcqueue_number = 0;

static struct protocol_t *procProtocol;

/* The pid_file and pid of this daemon */
static char *pid_file;
static unsigned short pid_file_free = 0;
static pid_t pid;
/* The number of receivers connected */
static int receivers = 0;
/* Daemonize or not */
static int nodaemon = 0;
/* Are we already running */
static int running = 1;
/* How many times does the code need to be resend */
static int send_repeat = 0;
/* How many times does a code need to received*/
static int receive_repeat = RECEIVE_REPEATS;
/* Are we currently sending code */
static int sending = 0;
/* If we have accepted a client, handshakes will store the type of client */
static short handshakes[MAX_CLIENTS];
/* Which mode are we running in: 1 = server, 2 = client */
static unsigned short runmode = 1;
/* Socket identifier to the server if we are running as client */
static int sockfd = 0;
/* In the client running in incognito mode */
static unsigned short incognito_mode = 0;
/* Thread pointers */
static pthread_t pth;
/* While loop conditions */
static unsigned short main_loop = 1;
/* Reset repeats after a certian amount of time */
static struct timeval tv;
/* How many nodes are connected */
static int nrnodes = 0;
/* Are we running standalone */
static int standalone = 0;
/* What is the minimum rawlenth to consider a pulse stream valid */
static int minrawlen = 1000;
/* What is the maximum rawlenth to consider a pulse stream valid */
static int maxrawlen = 0;
/* Do we need to connect to a master server:port? */
static char *master_server = NULL;
static unsigned short master_port = 0;

#ifdef WEBSERVER
/* Do we enable the webserver */
static int webserver_enable = WEBSERVER_ENABLE;
/* On what port does the webserver run */
static int webserver_port = WEBSERVER_PORT;
/* The webroot of pilight */
static char *webserver_root;
static char *webgui_tpl = NULL;
static int webserver_root_free = 0;
static int webgui_tpl_free = 0;
#endif

#ifdef UPDATE
/* Do we need to check for updates */
static int update_check = UPDATE_CHECK;
#endif

static void node_add(int id, char uuid[21]) {
	struct nodes_t *node = malloc(sizeof(struct nodes_t));
	if(!node) {
		logprintf(LOG_ERR, "out of memory");
		exit(EXIT_FAILURE);
	}
	strcpy(node->uuid, uuid);
	node->client_id = id;
	node->next = nodes;
	nodes = node;
	nrnodes++;
}

static void node_remove(int id) {
	struct nodes_t *currP, *prevP;

	prevP = NULL;

	for(currP = nodes; currP != NULL; prevP = currP, currP = currP->next) {

		if(currP->client_id == id) {
			if(prevP == NULL) {
				nodes = currP->next;
			} else {
				prevP->next = currP->next;
			}

			sfree((void *)&currP);
			nrnodes--;
			break;
		}
	}
}

static void broadcast_queue(char *protoname, JsonNode *json) {
	pthread_mutex_lock(&bcqueue_lock);
	if(bcqueue_number <= 1024) {
		struct bcqueue_t *bnode = malloc(sizeof(struct bcqueue_t));
		if(!bnode) {
			logprintf(LOG_ERR, "out of memory");
			exit(EXIT_FAILURE);
		}

		char *jstr = json_stringify(json, NULL);
        //char *conf = json_stringify(bcqueue->jmessage, NULL);
        logprintf(LOG_DEBUG, "bqueue %s", jstr);
		bnode->jmessage = json_decode(jstr);
		if(json_find_member(bnode->jmessage, "uuid") == NULL && strlen(pilight_uuid) > 0) {
			json_append_member(bnode->jmessage, "uuid", json_mkstring(pilight_uuid));
		}
		sfree((void *)&jstr);

		bnode->protoname = malloc(strlen(protoname)+1);
		if(!bnode->protoname) {
			logprintf(LOG_ERR, "out of memory");
			exit(EXIT_FAILURE);
		}
		strcpy(bnode->protoname, protoname);
        char *str = json_stringify(bnode->jmessage, NULL);
        logprintf(LOG_DEBUG, "bqueue_new %s", str);

		if(bcqueue_number == 0) {
			bcqueue = bnode;
			bcqueue_head = bnode;
		} else {
			bcqueue_head->next = bnode;
			bcqueue_head = bnode;
		}

		bcqueue_number++;
	} else {
		logprintf(LOG_ERR, "broadcast queue full");
	}
	pthread_mutex_unlock(&bcqueue_lock);
	pthread_cond_signal(&bcqueue_signal);
}

void *broadcast(void *param) {
	int i = 0, broadcasted = 0;

	pthread_mutex_lock(&bcqueue_lock);
	while(main_loop) {
		if(bcqueue_number > 0) {
			pthread_mutex_lock(&bcqueue_lock);

			broadcasted = 0;
			JsonNode *jret = NULL;
			char *origin = NULL;

			if(json_find_string(bcqueue->jmessage, "origin", &origin) == 0) {
				logprintf(LOG_DEBUG, "broadcasted_broadcast_else: %s", bcqueue->jmessage);
				if(strcmp(origin, "config") == 0) {
					char *conf = json_stringify(bcqueue->jmessage, NULL);
					for(i=0;i<MAX_CLIENTS;i++) {
						if(handshakes[i] == GUI) {
							socket_write(socket_get_clients(i), conf);
							broadcasted = 1;
						}
					}
					if(broadcasted == 1) {
						logprintf(LOG_DEBUG, "broadcasted_broadcast: %s", conf);
					}
					sfree((void *)&conf);
				} else {
					/* Update the config */
					if(config_update(bcqueue->protoname, bcqueue->jmessage, &jret) == 0) {
						char *conf = json_stringify(jret, NULL);
						logprintf(LOG_DEBUG, "broadcasted_update: %s", conf);
						for(i=0;i<MAX_CLIENTS;i++) {
							if(handshakes[i] == GUI) {
								socket_write(socket_get_clients(i), conf);
								broadcasted = 1;
							}
						}

						if(broadcasted == 1) {
							logprintf(LOG_DEBUG, "broadcasted_broadcast2: %s", conf);
						}
						sfree((void *)&conf);
					}
					if(jret) {
						json_delete(jret);
					}

					/* The message and settings objects inside the broadcast queue is only
					   of interest for the internal pilight functions. For the outside world
					   we only communicate the message part of the queue so we rename it
					   to code for clarity and we remove the settings */
					JsonNode *jcode = NULL;
					if((jcode = json_find_member(bcqueue->jmessage, "message")) != NULL) {
						jcode->key = realloc(jcode->key, 5);
						strcpy(jcode->key, "code");
					}

					char *jinternal = json_stringify(bcqueue->jmessage, NULL);

					JsonNode *jsettings = NULL;
					if((jsettings = json_find_member(bcqueue->jmessage, "settings"))) {
						json_remove_from_parent(jsettings);
					}

					char *jbroadcast = json_stringify(bcqueue->jmessage, NULL);

					if(strcmp(bcqueue->protoname, "pilight_firmware") == 0) {
						JsonNode *code = NULL;
						if((code = json_find_member(bcqueue->jmessage, "code")) != NULL) {
							json_find_number(code, "version", &firmware.version);
							json_find_number(code, "lpf", &firmware.lpf);
							json_find_number(code, "hpf", &firmware.hpf);
						}
					}
					broadcasted = 0;

					struct JsonNode *childs = json_first_child(bcqueue->jmessage);
					int nrchilds = 0;
					while(childs) {
						nrchilds++;
						childs = childs->next;
					}

					if(receivers > 0) {
						/* Write the message to all receivers */
						for(i=0;i<MAX_CLIENTS;i++) {
							if(handshakes[i] == RECEIVER) {
								if(strcmp(jbroadcast, "{}") != 0 && nrchilds > 1) {
									socket_write(socket_get_clients(i), jbroadcast);
									broadcasted = 1;
								}
							}
						}
					}

					if(runmode == 2 && sockfd > 0) {
						logprintf(LOG_DEBUG, "broadcasted_broadcast23: %s", jbroadcast);
						struct JsonNode *jupdate = json_decode(jinternal);
						json_append_member(jupdate, "message", json_mkstring("update"));
						char *ret = json_stringify(jupdate, NULL);
						socket_write(sockfd, ret);
						broadcasted = 1;
						json_delete(jupdate);
						sfree((void *)&ret);
					}
					if((broadcasted == 1 || nodaemon == 1) && (strcmp(jbroadcast, "{}") != 0 && nrchilds > 1)) {
						logprintf(LOG_DEBUG, "broadcasted_broadcast3: %s", jbroadcast);
					}
					sfree((void *)&jinternal);
					sfree((void *)&jbroadcast);
				}
			}
			struct bcqueue_t *tmp = bcqueue;
			sfree((void *)&tmp->protoname);
			json_delete(tmp->jmessage);
			bcqueue = bcqueue->next;
			sfree((void *)&tmp);
			bcqueue_number--;
			pthread_mutex_unlock(&bcqueue_lock);
		} else {
			pthread_cond_wait(&bcqueue_signal, &bcqueue_lock);
		}
	}
	return (void *)NULL;
}

static void receive_queue(int *raw, int rawlen, int plslen, int hwtype) {
	int i = 0;

	pthread_mutex_lock(&recvqueue_lock);
	if(recvqueue_number <= 1024) {
		struct recvqueue_t *rnode = malloc(sizeof(struct recvqueue_t));
		if(!rnode) {
			logprintf(LOG_ERR, "out of memory");
			exit(EXIT_FAILURE);
		}
		for(i=0;i<rawlen;i++) {
			rnode->raw[i] = raw[i];
		}
		rnode->rawlen = rawlen;
		rnode->plslen = plslen;
		rnode->hwtype = hwtype;

		if(recvqueue_number == 0) {
			recvqueue = rnode;
			recvqueue_head = rnode;
		} else {
			recvqueue_head->next = rnode;
			recvqueue_head = rnode;
		}

		recvqueue_number++;
	} else {
		logprintf(LOG_ERR, "receiver queue full");
	}
	pthread_mutex_unlock(&recvqueue_lock);
	pthread_cond_signal(&recvqueue_signal);
}

static void receiver_create_message(protocol_t *protocol) {
	if(protocol->message) {
		char *valid = json_stringify(protocol->message, NULL);
		json_delete(protocol->message);
		if(valid && json_validate(valid) == true) {
			JsonNode *jmessage = json_mkobject();

			json_append_member(jmessage, "message", json_decode(valid));
			json_append_member(jmessage, "origin", json_mkstring("receiver"));
			json_append_member(jmessage, "protocol", json_mkstring(protocol->id));
			if(strlen(pilight_uuid) > 0) {
				json_append_member(jmessage, "uuid", json_mkstring(pilight_uuid));
			}
			if(protocol->repeats > -1) {
				json_append_member(jmessage, "repeats", json_mknumber(protocol->repeats));
			}
			char *output = json_stringify(jmessage, NULL);
			JsonNode *json = json_decode(output);
			broadcast_queue(protocol->id, json);
			sfree((void *)&output);
			json_delete(json);
			json = NULL;
			json_delete(jmessage);
		}
		protocol->message = NULL;
		sfree((void *)&valid);
	}
}

void *receive_parse_code(void *param) {

	pthread_mutex_lock(&recvqueue_lock);
	while(main_loop) {
		if(recvqueue_number > 0) {
			pthread_mutex_lock(&recvqueue_lock);

			struct protocol_t *protocol = NULL;
			struct protocols_t *pnode = protocols;
			struct protocol_plslen_t *plslengths = NULL;
			int x = 0, match = 0;

			while(pnode && main_loop) {
				protocol = pnode->listener;
				match = 0;

				if((protocol->hwtype == recvqueue->hwtype || protocol->hwtype == -1 || recvqueue->hwtype == -1) &&
				   ((((protocol->parseRaw || protocol->parseCode) &&
					  (protocol->rawlen > 0 || (protocol->minrawlen > 0 && protocol->maxrawlen > 0)))
					   || protocol->parseBinary) && protocol->pulse > 0 && protocol->plslen)) {
					plslengths = protocol->plslen;
					while(plslengths && main_loop) {
						if((recvqueue->plslen >= ((double)plslengths->length-5) &&
						    recvqueue->plslen <= ((double)plslengths->length+5))) {
							match = 1;
							break;
						}
						plslengths = plslengths->next;
					}
					if((recvqueue->rawlen == protocol->rawlen || (
					   (protocol->minrawlen > 0 && protocol->maxrawlen > 0 &&
					    recvqueue->rawlen >= protocol->minrawlen && recvqueue->rawlen <= protocol->maxrawlen)))
					    && match == 1) {
						for(x=0;x<(int)recvqueue->rawlen;x++) {
							if(x < 254) {
								memcpy(&protocol->raw[x], &recvqueue->raw[x], sizeof(int));
							}
						}
						if(protocol->parseRaw) {
							logprintf(LOG_DEBUG, "recevied pulse length of %d", recvqueue->plslen);
							logprintf(LOG_DEBUG, "called %s parseRaw()", protocol->id);
							protocol->parseRaw();
							protocol->repeats = -1;
							receiver_create_message(protocol);
						}

						/* Convert the raw codes to one's and zero's */
						for(x=0;x<recvqueue->rawlen;x++) {
							protocol->pCode[x] = protocol->code[x];

							if(protocol->raw[x] >= (plslengths->length * (1+protocol->pulse)/2)) {
								protocol->code[x] = 1;
							} else {
								protocol->code[x] = 0;
							}
							/* Check if the current code matches the previous one */
							// if(protocol->pCode[x] != protocol->code[x]) {
								// protocol->repeats = 0;
								// protocol->first = 0;
								// protocol->second = 0;
							// }
						}

						gettimeofday(&tv, NULL);
						if(protocol->first > 0) {
							protocol->first = protocol->second;
						}
						protocol->second = 1000000 * (unsigned int)tv.tv_sec + (unsigned int)tv.tv_usec;
						if(protocol->first == 0) {
							protocol->first = protocol->second;
						}

						/* Reset # of repeats after a certain delay */
						if(((int)protocol->second-(int)protocol->first) > 500000) {
							protocol->repeats = 0;
						}

						protocol->repeats++;
						/* Continue if we have recognized enough repeated codes */
						if(protocol->repeats >= (receive_repeat*protocol->rxrpt) ||
						   strcmp(protocol->id, "pilight_firmware") == 0) {
							if(protocol->parseCode) {
								logprintf(LOG_DEBUG, "caught minimum # of repeats %d of %s", protocol->repeats, protocol->id);
								logprintf(LOG_DEBUG, "called %s parseCode()", protocol->id);
								protocol->parseCode();
								receiver_create_message(protocol);
							}

							if(protocol->parseBinary) {
								/* Convert the one's and zero's into binary */
								for(x=0; x<(int)recvqueue->rawlen; x+=4) {
									if(protocol->code[x+protocol->lsb] == 1) {
										protocol->binary[x/4] = 1;
									} else {
										protocol->binary[x/4] = 0;
									}
								}

								if((double)protocol->raw[1]/((plslengths->length * (1+protocol->pulse)/2)) < 2.1) {
									x -= 4;
								}

								/* Check if the binary matches the binary length */
								if((protocol->binlen > 0 && ((x/4) == protocol->binlen))
								   || (protocol->binlen == 0 && ((x/4) == protocol->rawlen/4))) {
									logprintf(LOG_DEBUG, "called %s parseBinary()", protocol->id);

									protocol->parseBinary();
									receiver_create_message(protocol);
								}
							}
						}
					}
				}
				pnode = pnode->next;
			}

			struct recvqueue_t *tmp = recvqueue;
			recvqueue = recvqueue->next;
			sfree((void *)&tmp);
			recvqueue_number--;
			pthread_mutex_unlock(&recvqueue_lock);
		} else {
			pthread_cond_wait(&recvqueue_signal, &recvqueue_lock);
		}
	}
	return (void *)NULL;
}

void *send_code(void *param) {
	int i = 0, x = 0;
	struct sched_param sched;

	/* Make sure the pilight sender gets
	   the highest priority available */
	memset(&sched, 0, sizeof(sched));
	sched.sched_priority = 80;
	pthread_setschedparam(pthread_self(), SCHED_FIFO, &sched);

	pthread_mutex_lock(&sendqueue_lock);

	while(main_loop) {
		if(sendqueue_number > 0) {
			sending = 1;
			pthread_mutex_lock(&sendqueue_lock);
			pthread_mutex_lock(&receive_lock);

			struct protocol_t *protocol = sendqueue->protopt;
			struct hardware_t *hw = NULL;

			JsonNode *message = NULL;

			if(sendqueue->message && strcmp(sendqueue->message, "{}") != 0) {
				if(json_validate(sendqueue->message) == true) {
					if(!message) {
						message = json_mkobject();
					}
					json_append_member(message, "origin", json_mkstring("sender"));
					json_append_member(message, "protocol", json_mkstring(protocol->id));
					json_append_member(message, "message", json_decode(sendqueue->message));
					if(strlen(sendqueue->uuid) > 0) {
						json_append_member(message, "uuid", json_mkstring(sendqueue->uuid));
					}
					json_append_member(message, "repeat", json_mknumber(1));
				}
			}
			if(sendqueue->settings && strcmp(sendqueue->settings, "{}") != 0) {
				if(json_validate(sendqueue->settings) == true) {
					if(!message) {
						message = json_mkobject();
					}
					json_append_member(message, "settings", json_decode(sendqueue->settings));
				}
			}

			/* Create a single code with all repeats included */
			int code_len = (protocol->rawlen*send_repeat*protocol->txrpt)+1;
			size_t send_len = (size_t)(code_len * (int)sizeof(int));
			int longCode[code_len];
			memset(longCode, 0, send_len);

			for(i=0;i<(send_repeat*protocol->txrpt);i++) {
				for(x=0;x<protocol->rawlen;x++) {
					longCode[x+(protocol->rawlen*i)]=sendqueue->code[x];
				}
			}

			longCode[code_len] = 0;
			struct conf_hardware_t *tmp_confhw = conf_hardware;
			while(tmp_confhw) {
				if(protocol->hwtype == tmp_confhw->hardware->type) {
					hw = tmp_confhw->hardware;
					break;
				}
				tmp_confhw = tmp_confhw->next;
			}

			if(hw && hw->send) {
				logprintf(LOG_DEBUG, "**** RAW CODE ****");
				if(log_level_get() >= LOG_DEBUG) {
					for(i=0;i<protocol->rawlen;i++) {
						printf("%d ", protocol->raw[i]);
					}
					printf("\n");
				}
				logprintf(LOG_DEBUG, "**** RAW CODE ****");
				if(hw->send(longCode) == 0) {
					logprintf(LOG_DEBUG, "successfully send %s code", protocol->id);
					if(strcmp(protocol->id, "raw") == 0) {
						int plslen = protocol->raw[protocol->rawlen-1]/PULSE_DIV;
						receive_queue(protocol->raw, protocol->rawlen, plslen, -1);
					}
				} else {
					logprintf(LOG_ERR, "failed to send code");
				}
			} else {
				if(strcmp(protocol->id, "raw") == 0) {
					int plslen = protocol->raw[protocol->rawlen-1]/PULSE_DIV;
					receive_queue(protocol->raw, protocol->rawlen, plslen, -1);
				}
			}

			if(message) {
				broadcast_queue(sendqueue->protoname, message);
				json_delete(message);
				message = NULL;
			}

			struct sendqueue_t *tmp = sendqueue;
			if(tmp->message) {
				sfree((void *)&tmp->message);
			}
			if(tmp->settings) {
				sfree((void *)&tmp->settings);
			}
			sfree((void *)&tmp->protoname);
			sendqueue = sendqueue->next;
			sfree((void *)&tmp);
			sendqueue_number--;
			sending = 0;
			pthread_mutex_unlock(&sendqueue_lock);
			pthread_mutex_unlock(&receive_lock);
			pthread_cond_signal(&receive_signal);
		} else {
			pthread_cond_wait(&sendqueue_signal, &sendqueue_lock);
		}
	}
	return (void *)NULL;
}

/* Send a specific code */
static void send_queue(JsonNode *json) {
	int match = 0, x = 0;
	struct timeval tcurrent;
	char *uuid = NULL;
	/* Hold the final protocol struct */
	struct protocol_t *protocol = NULL;
	struct sched_param sched;

	/* Make sure the pilight sender gets
	   the highest priority available */
	memset(&sched, 0, sizeof(sched));
	sched.sched_priority = 80;
	pthread_setschedparam(pthread_self(), SCHED_FIFO, &sched);

	JsonNode *jcode = NULL;
	JsonNode *jprotocols = NULL;
	JsonNode *jprotocol = NULL;

	if(!(jcode = json_find_member(json, "code"))) {
		logprintf(LOG_ERR, "sender did not send any codes");
		json_delete(jcode);
	} else if(!(jprotocols = json_find_member(jcode, "protocol"))) {
		logprintf(LOG_ERR, "sender did not provide a protocol name");
		json_delete(jcode);
	} else {
		json_find_string(jcode, "uuid", &uuid);
		/* If we matched a protocol and are not already sending, continue */
		if((!uuid || (uuid && strcmp(uuid, pilight_uuid) == 0)) && send_repeat > 0) {
			jprotocol = json_first_child(jprotocols);
			while(jprotocol && match == 0) {
				match = 0;
				if(jprotocol->tag == JSON_STRING) {
					struct protocols_t *pnode = protocols;
					/* Retrieve the used protocol */
					while(pnode) {
						protocol = pnode->listener;
						/* Check if the protocol exists */
						if(protocol_device_exists(protocol, jprotocol->string_) == 0 && match == 0) {
							match = 1;
							break;
						}
						pnode = pnode->next;
					}
				}
				jprotocol = jprotocol->next;
			}
			if(match == 1 && protocol->createCode) {
				/* Let the protocol create his code */
				if(protocol->createCode(jcode) == 0) {
					pthread_mutex_lock(&sendqueue_lock);
					if(sendqueue_number <= 1024) {
						struct sendqueue_t *mnode = malloc(sizeof(struct sendqueue_t));
						if(!mnode) {
							logprintf(LOG_ERR, "out of memory");
							exit(EXIT_FAILURE);
						}
						gettimeofday(&tcurrent, NULL);
						mnode->id = 1000000 * (unsigned int)tcurrent.tv_sec + (unsigned int)tcurrent.tv_usec;
						mnode->message = NULL;
						if(protocol->message) {
							char *jsonstr = json_stringify(protocol->message, NULL);
							json_delete(protocol->message);
							if(json_validate(jsonstr) == true) {
								mnode->message = malloc(strlen(jsonstr)+1);
								if(!mnode->message) {
									logprintf(LOG_ERR, "out of memory");
									exit(EXIT_FAILURE);
								}
								strcpy(mnode->message, jsonstr);
							}
							sfree((void *)&jsonstr);
							protocol->message = NULL;
						}
						for(x=0;x<protocol->rawlen;x++) {
							mnode->code[x]=protocol->raw[x];
						}
						mnode->protoname = malloc(strlen(protocol->id)+1);
						if(!mnode->protoname) {
							logprintf(LOG_ERR, "out of memory");
							exit(EXIT_FAILURE);
						}
						strcpy(mnode->protoname, protocol->id);
						mnode->protopt = protocol;

						struct options_t *tmp_options = protocol->options;
						double itmp = 0;
						char *stmp = NULL;
						struct JsonNode *jsettings = json_mkobject();
						while(tmp_options) {
							if(tmp_options->conftype == CONFIG_SETTING) {
								if(tmp_options->vartype == JSON_NUMBER && json_find_number(jcode, tmp_options->name, &itmp) == 0) {
									json_append_member(jsettings, tmp_options->name, json_mknumber(itmp));
								} else if(tmp_options->vartype == JSON_STRING && json_find_string(jcode, tmp_options->name, &stmp) == 0) {
									json_append_member(jsettings, tmp_options->name, json_mkstring(stmp));
								}
							}
							tmp_options = tmp_options->next;
						}
						char *strsett = json_stringify(jsettings, NULL);
						mnode->settings = malloc(strlen(strsett)+1);
						strcpy(mnode->settings, strsett);
						sfree((void *)&strsett);
						json_delete(jsettings);

						if(uuid) {
							strcpy(mnode->uuid, uuid);
						} else {
							memset(mnode->uuid, '\0', UUID_LENGTH);
						}
						if(sendqueue_number == 0) {
							sendqueue = mnode;
							sendqueue_head = mnode;
						} else {
							sendqueue_head->next = mnode;
							sendqueue_head = mnode;
						}
						sendqueue_number++;
					} else {
						logprintf(LOG_ERR, "send queue full");
					}
					pthread_mutex_unlock(&sendqueue_lock);
					pthread_cond_signal(&sendqueue_signal);
				}
			}
		}

		if(jcode) {
			json_delete(jcode);
		}
	}
}

static void client_sender_parse_code(int i, JsonNode *json) {
	int sd = socket_get_clients(i);

	if(incognito_mode == 0 && i > -1 && handshakes[i] != NODE) {
		/* Don't let the sender wait until we have send the code */
		socket_close(sd);
		handshakes[i] = -1;
	}

	send_queue(json);
}

static void control_device(struct conf_devices_t *dev, char *state, JsonNode *values) {
	struct conf_settings_t *sett = NULL;
	struct conf_values_t *val = NULL;
	struct options_t *opt = NULL;
	struct protocols_t *tmp_protocols = NULL;

	JsonNode *code = json_mkobject();
	JsonNode *json = json_mkobject();
	JsonNode *jprotocols = json_mkarray();

	/* Check all protocol options */
	tmp_protocols = dev->protocols;
	while(tmp_protocols) {
		json_append_element(jprotocols, json_mkstring(tmp_protocols->name));
		if((opt = tmp_protocols->listener->options)) {
			while(opt) {
				sett = dev->settings;
				while(sett) {
					/* Retrieve the device id's */
					if(strcmp(sett->name, "id") == 0) {
						val = sett->values;
						while(val) {
							if((opt->conftype == CONFIG_ID)
							   && strcmp(val->name, opt->name) == 0
							   && json_find_member(code, opt->name) == NULL) {
								if(val->type == CONFIG_TYPE_STRING) {
									json_append_member(code, val->name, json_mkstring(val->string_));
								} else if(val->type == CONFIG_TYPE_NUMBER) {
									json_append_member(code, val->name, json_mknumber(val->number_));
								}
							}
							val = val->next;
						}
					}
					if(strcmp(sett->name, opt->name) == 0
					   && opt->conftype == CONFIG_SETTING) {
						val = sett->values;
						if(json_find_member(code, opt->name) == NULL) {
							if(val->type == CONFIG_TYPE_STRING) {
								json_append_member(code, opt->name, json_mkstring(val->string_));
							} else if(val->type == CONFIG_TYPE_NUMBER) {
								json_append_member(code, opt->name, json_mknumber(val->number_));
							}
						}
					}
					sett = sett->next;
				}
				opt = opt->next;
			}
			while(values) {
				opt = tmp_protocols->listener->options;
				while(opt) {
					if((opt->conftype == CONFIG_VALUE || opt->conftype == CONFIG_OPTIONAL)
					   && strcmp(values->key, opt->name) == 0
					   && json_find_member(code, opt->name) == NULL) {
						if(values->tag == JSON_STRING) {
							json_append_member(code, values->key, json_mkstring(values->string_));
						} else if(values->tag == JSON_NUMBER) {
							json_append_member(code, values->key, json_mknumber(values->number_));
						}
					}
					opt = opt->next;
				}
				values = values->next;
			}
		}
		/* Send the new device state */
		if((opt = tmp_protocols->listener->options)) {
			while(opt) {
				if(json_find_member(code, opt->name) == NULL) {
					if(opt->conftype == CONFIG_STATE && opt->argtype == OPTION_NO_VALUE && strcmp(opt->name, state) == 0) {
						json_append_member(code, opt->name, json_mknumber(1));
						break;
					} else if(opt->conftype == CONFIG_STATE && opt->argtype == OPTION_HAS_VALUE) {
						json_append_member(code, opt->name, json_mkstring(state));
						break;
					}
				}
				opt = opt->next;
			}
		}
		tmp_protocols = tmp_protocols->next;
	}

	/* Construct the right json object */
	json_append_member(code, "protocol", jprotocols);
	if(dev->dev_uuid && (dev->protocols->listener->hwtype == SENSOR
	   || dev->protocols->listener->hwtype == HWRELAY)) {
		json_append_member(code, "uuid", json_mkstring(dev->dev_uuid));
	}
	json_append_member(json, "code", code);
	json_append_member(json, "message", json_mkstring("send"));

	send_queue(json);

	json_delete(json);
}

static void client_node_parse_code(int i, JsonNode *json) {
	int sd = socket_get_clients(i);
	char *message = NULL;

	if(json_find_string(json, "message", &message) == 0) {
		/* Send the config file to the controller */
		if(strcmp(message, "request config") == 0) {
			struct JsonNode *jsend = config_broadcast_create();
			char *output = json_stringify(jsend, NULL);
			socket_write(sd, output);
			sfree((void *)&output);
			json_delete(jsend);
		} else if(strcmp(message, "update") == 0) {
			char *pname = NULL;
			if(json_find_string(json, "protocol", &pname) == 0) {
				JsonNode *jcode = NULL;
				JsonNode *jmessage = NULL;
				if((jmessage = json_find_member(json, "message")) != NULL) {
					json_remove_from_parent(jmessage);
				}
				if((jcode = json_find_member(json, "code")) != NULL) {
					jcode->key = realloc(jcode->key, 9);
					strcpy(jcode->key, "message");
				}

				broadcast_queue(pname, json);
			}
		}
	}
}

static void client_controller_parse_code(int i, JsonNode *json) {
	int sd = socket_get_clients(i);
	char *message = NULL;
	char *location = NULL;
	char *device = NULL;
	char *tmp = NULL;
	struct conf_locations_t *slocation;
	struct conf_devices_t *sdevice;
	JsonNode *code = NULL;
	JsonNode *values = NULL;

	logprintf(LOG_DEBUG, "in controller");
	if(json_find_string(json, "message", &message) == 0) {
		/* Send the config file to the controller */
		if(strcmp(message, "request config") == 0) {
			struct JsonNode *jsend = config_broadcast_create();
			char *output = json_stringify(jsend, NULL);
			socket_write(sd, output);
			sfree((void *)&output);
			json_delete(jsend);
		/* Control a specific device */
		} else if(strcmp(message, "send") == 0) {
			/* Check if got a code */
			if(!(code = json_find_member(json, "code"))) {
				logprintf(LOG_ERR, "controller did not send any codes");
			} else {
				/* Check if a location and device are given */
				if(json_find_string(code, "location", &location) != 0) {
					logprintf(LOG_ERR, "controller did not send a location");
				} else if(json_find_string(code, "device", &device) != 0) {
					logprintf(LOG_ERR, "controller did not send a device");
				/* Check if the device and location exists in the config file */
				} else if(config_get_location(location, &slocation) == 0) {
					if(config_get_device(location, device, &sdevice) == 0) {
						char *state = malloc(4);
						if(!state) {
							logprintf(LOG_ERR, "out of memory");
							exit(EXIT_FAILURE);
						}
						if(json_find_string(code, "state", &tmp) == 0) {
							state = realloc(state, strlen(tmp)+1);
							if(!state) {
								logprintf(LOG_ERR, "out of memory");
								exit(EXIT_FAILURE);
							}
							strcpy(state, tmp);
						} else {
							state = realloc(state, 4);
							if(!state) {
								logprintf(LOG_ERR, "out of memory");
								exit(EXIT_FAILURE);
							}
							memset(state, '\0', 4);
						}
						/* Send the device code */
						values = json_find_member(code, "values");
						if(values) {
							values = json_first_child(values);
						}

						control_device(sdevice, state, values);
						sfree((void *)&state);
					} else {
						logprintf(LOG_ERR, "the device \"%s\" does not exist", device);
					}
				} else {
					logprintf(LOG_ERR, "the location \"%s\" does not exist", location);
				}
				if(incognito_mode == 0 && handshakes[i] != GUI && handshakes[i] != NODE) {
					socket_close(sd);
					handshakes[i] = -1;
				}
			}
		}
        else if(strcmp(message, "receiver") == 0) {
			char *output = json_stringify(json, NULL);
			logprintf(LOG_DEBUG, "controll_reciver %s", output);
            JsonNode *jsettings = NULL;
            if((jsettings = json_find_member(json, "message")) != NULL ) {
                json_remove_from_parent(jsettings);
            }

            //char * pname;
            //pname = json_find_member(json, "protocol")
		    //pilight.broadcast(procProtocol->id, procProtocol->message);
		    pilight.broadcast("generic_api", json);
        }
    }
    else if(json_find_string(json, "protocol", &message) == 0) {
		char *output = json_stringify(json, NULL);
		logprintf(LOG_DEBUG, "controll_reciver2 %s", output);
        //JsonNode *jsettings = NULL;
        //if((jsettings = json_find_member(json, "message")) != NULL ) {
        //    json_remove_from_parent(jsettings);
        //}
	    pilight.broadcast(message, json);
    }
    else{
		logprintf(LOG_DEBUG, "controll_reciver3");
    }
}

#ifdef WEBSERVER
static void client_webserver_parse_code(int i, char buffer[BUFFER_SIZE]) {
	int sd = socket_get_clients(i);
	int x = 0;
	FILE *f;
	unsigned char *p = NULL;
	unsigned char buff[BUFFER_SIZE];
	char *cache = NULL;
	char *path = NULL;
	char *mimetype = NULL;
	struct stat sb;
	struct sockaddr_in sockin;
	socklen_t len = sizeof(sockin);

	if(getsockname(sd, (struct sockaddr *)&sockin, &len) == -1) {
		logprintf(LOG_ERR, "could not determine server ip address");
	} else {
		if(strstr(buffer, " HTTP/") == NULL) {
			return;
		}
		p = buff;
		if(strstr(buffer, "/logo.png") != NULL) {
			if(!(path = malloc(strlen(webserver_root)+strlen(webgui_tpl)+strlen("logo.png")+2))) {
				logprintf(LOG_ERR, "out of memory");
				exit(EXIT_FAILURE);
			}
			sprintf(path, "%s/%s/logo.png", webserver_root, webgui_tpl);
			if((f = fopen(path, "rb"))) {
				fstat(fileno(f), &sb);
				mimetype = webserver_mimetype("image/png");
				webserver_create_header(&p, "200 OK", mimetype, (unsigned int)sb.st_size);
				send(sd, buff, (size_t)(p-buff), MSG_NOSIGNAL);
				x = 0;
				if(!(cache = malloc(BUFFER_SIZE))) {
					logprintf(LOG_ERR, "out of memory");
					exit(EXIT_FAILURE);
				}
				memset(cache, '\0', BUFFER_SIZE);
				while(!feof(f)) {
					x = (int)fread(cache, 1, BUFFER_SIZE, f);
					send(sd, cache, (size_t)x, MSG_NOSIGNAL);
				}
				fclose(f);
				sfree((void *)&cache);
				sfree((void *)&mimetype);
			} else {
				logprintf(LOG_NOTICE, "pilight logo not found");
			}
			sfree((void *)&path);
		} else {
		    /* Catch all webserver page to inform users on which port the webserver runs */
			mimetype = webserver_mimetype("text/html");
			webserver_create_header(&p, "200 OK", mimetype, (unsigned int)BUFFER_SIZE);
			send(sd, buff, (size_t)(p-buff), MSG_NOSIGNAL);
			if(webserver_enable == 1) {
				if(!(cache = malloc(BUFFER_SIZE))) {
					logprintf(LOG_ERR, "out of memory");
					exit(EXIT_FAILURE);
				}
				memset(cache, '\0', BUFFER_SIZE);
				sprintf(cache, "<html><head><title>pilight</title></head>"
							   "<body><center><img src=\"logo.png\"><br />"
							   "<p style=\"color: #0099ff; font-weight: 800px;"
							   "font-family: Verdana; font-size: 20px;\">"
							   "The pilight webgui is located at "
							   "<a style=\"text-decoration: none; color: #0099ff;"
							   "font-weight: 800px; font-family: Verdana; font-size:"
							   "20px;\" href=\"http://%s:%d\">http://%s:%d</a></p>"
							   "</center></body></html>",
							   inet_ntoa(sockin.sin_addr),
							   webserver_port,
							   inet_ntoa(sockin.sin_addr),
							   webserver_port);
				send(sd, cache, strlen(cache), MSG_NOSIGNAL);
				sfree((void *)&cache);
			} else {
				send(sd, "<body><center><img src=\"logo.png\"></center></body></html>", 57, MSG_NOSIGNAL);
			}
			sfree((void *)&mimetype);
		}
	}
}
#endif

/* Parse the incoming buffer from the client */
static void socket_parse_data(int i, char *buffer) {
	int sd = socket_get_clients(i);
	struct sockaddr_in address;
	int addrlen = sizeof(address);
	char *message;
	char *incognito;
	JsonNode *json = NULL;
	short x = 0;

	getpeername(sd, (struct sockaddr*)&address, (socklen_t*)&addrlen);

	if(strcmp(buffer, "HEART") == 0) {
		socket_write(sd, "BEAT");
	} else {
		logprintf(LOG_DEBUG, "socket recv_parse_data: %s", buffer);
		//logprintf(LOG_DEBUG, "socket message: %s", message);
		/* Serve static webserver page. This is the only request that's is
		   expected not to be a json object */
#ifdef WEBSERVER
		if(strstr(buffer, " HTTP/")) {
			logprintf(LOG_INFO, "client recognized as web");
			handshakes[i] = WEB;
			client_webserver_parse_code(i, buffer);
			socket_close(sd);
		} else if(json_validate(buffer) == true) {
#else
		if(json_validate(buffer) == true) {
#endif
			json = json_decode(buffer);
            char *output = json_stringify(json, NULL);
		    logprintf(LOG_DEBUG, "socket r_p_d j: %s", output);

			/* The incognito mode is used by the daemon to emulate certain clients.
			   Temporary change the client type from the node mode to the emulated
			   client mode. */
			if(json_find_string(json, "incognito", &incognito) == 0) {
				incognito_mode = 1;
		        logprintf(LOG_DEBUG, "socket incognito: %s", output);
				for(x=0;x<(sizeof(clients)/sizeof(clients[0]));x++) {
					if(strcmp(clients[x], incognito) == 0) {
						handshakes[i] = x;
						break;
					}
				}
			} else if(json_find_string(json, "message", &message) == 0) {
		        logprintf(LOG_DEBUG, "socket m r_p_d j: %s", output);
				if(handshakes[i] != NODE && handshakes[i] != RECEIVER && handshakes[i] > -1) {
					if(runmode == 2 && sockfd > 0 && strcmp(message, "request config") != 0) {
						socket_write(sockfd, "{\"incognito\":\"%s\"}", clients[handshakes[i]]);
						socket_write(sockfd, buffer);
					}
				}
				if(handshakes[i] == NODE) {
					client_node_parse_code(i, json);
				} else if(handshakes[i] == SENDER) {
					client_sender_parse_code(i, json);
					if(strcmp(message, "send") == 0) {
						for(x=0;x<(sizeof(clients)/sizeof(clients[0]));x++) {
							if(handshakes[x] == NODE) {
								socket_write(socket_get_clients(x), "{\"incognito\":\"sender\"}");
								socket_write(socket_get_clients(x), buffer);
							}
						}
					}
				} else if(handshakes[i] == CONTROLLER || handshakes[i] == GUI) {
		            logprintf(LOG_DEBUG, "socket recv_C|G: %s", buffer);
					client_controller_parse_code(i, json);
					if(strcmp(message, "send") == 0) {
						for(x=0;x<(sizeof(clients)/sizeof(clients[0]));x++) {
							if(handshakes[x] == NODE) {
								socket_write(socket_get_clients(x), "{\"incognito\":\"controller\"}");
								socket_write(socket_get_clients(x), buffer);
							}
						}
					}
                    else if(strcmp(message, "receiver") == 0) {
						for(x=0;x<(sizeof(clients)/sizeof(clients[0]));x++) {
		                    logprintf(LOG_DEBUG, "socket W_recv_C|G: %s", buffer);
							if(handshakes[x] == NODE || handshakes[x] == GUI) {
								socket_write(socket_get_clients(x), "{\"incognito\":\"controller\"}");
								socket_write(socket_get_clients(x), buffer);
							}
						}
					}
				} else {
		            logprintf(LOG_DEBUG, "socket E r_p_d j: %s", output);
		            logprintf(LOG_DEBUG, "socket message: %s", message);
					/* Check if we matched a know client type */
					for(x=0;x<(sizeof(clients)/sizeof(clients[0]));x++) {
						char *tmp = malloc(8+strlen(clients[x]));
						if(!tmp) {
							logprintf(LOG_ERR, "out of memory");
							exit(EXIT_FAILURE);
						}
						sprintf(tmp, "client %s", clients[x]);
						tmp[7+strlen(clients[x])] = '\0';
						if(strcmp(message, tmp) == 0) {
							socket_write(sd, "{\"message\":\"accept client\"}");
							logprintf(LOG_INFO, "client recognized as %s", clients[x]);

							handshakes[i] = x;

							if(handshakes[i] == NODE) {
								char *uuid = NULL;
								if(json_find_string(json, "uuid", &uuid) == 0) {
									node_add(i, uuid);
								} else {
									handshakes[i] = -1;
								}
							}
							if(handshakes[i] == RECEIVER || handshakes[i] == GUI || handshakes[i] == NODE)
								receivers++;
							sfree((void *)&tmp);
							break;
						}
						sfree((void *)&tmp);
					}
				}
				/* Directly after using the incognito mode, restore the node mode */
				if(incognito_mode == 1) {
					for(x=0;x<(sizeof(clients)/sizeof(clients[0]));x++) {
						if(strcmp(clients[x], "node") == 0) {
							handshakes[i] = x;
							break;
						}
					}
					incognito_mode = 0;
				}
			}
            
            else if(json_find_string(json, "message", &message) == 0) {
				logprintf(LOG_DEBUG, "socket ELIFif: %s", output);
				logprintf(LOG_DEBUG, "socket ELIFif message: %s", message);
            } 
            else if(json_find_string(json, "origin", &message) == 0) {
				logprintf(LOG_DEBUG, "socket ELorig: %s", output);
				logprintf(LOG_DEBUG, "socket ELorig message: %s", message);
                if(strcmp(message, "receiver") == 0) {
				    logprintf(LOG_DEBUG, "socket EL before contr: %s", message);
					client_controller_parse_code(-1, json);
                }
                else{
				    logprintf(LOG_DEBUG, "socket ELorig message: not rec %s", message);
                }
            } 
            //else if(json_find_string(output, "message", &message) == 0) {
			//	logprintf(LOG_DEBUG, "socket ELIFif: %s", output);
			//	logprintf(LOG_DEBUG, "socket ELIFif message: %s", message);
            //} 
			else {
                //if(strcmp(message, "receiver") == 0){
				logprintf(LOG_DEBUG, "socket ELSE: %s", output);
				//logprintf(LOG_DEBUG, "socket ELSE message: %s", message);
			}
            
			if(handshakes[i] == -1 && socket_get_clients(i) > 0) {
				socket_write(sd, "{\"message\":\"reject client\"}");
				socket_close(sd);
			}
		}
	}
	if(json) {
		json_delete(json);
		json = NULL;
	}
}

static void socket_client_disconnected(int i) {
	if(handshakes[i] == RECEIVER || handshakes[i] == GUI || handshakes[i] == NODE)
		receivers--;

	handshakes[i] = -1;

	if(handshakes[i] == NODE) {
		node_remove(i);
	}

}

void *receive_code(void *param) {
	struct sched_param sched;
	int plslen = 0, rawlen = 0;
	int rawcode[255] = {0};
	int duration = 0;

	/* Make sure the pilight receiving gets
	   the highest priority available */
	memset(&sched, 0, sizeof(sched));
	sched.sched_priority = 70;
	pthread_setschedparam(pthread_self(), SCHED_FIFO, &sched);

	struct hardware_t *hw = (hardware_t *)param;

	pthread_mutex_lock(&receive_lock);
	while(main_loop && hw->receive) {
		if(sending == 0) {
			pthread_mutex_lock(&receive_lock);
			duration = hw->receive();

			if(duration > 0) {
				rawcode[rawlen] = duration;
				rawlen++;
				if(rawlen > 254) {
					rawlen = 0;
				}
				if(duration > 4440) {
					if((duration/PULSE_DIV) < 3000) { // Maximum footer pulse of 100000
						plslen = duration/PULSE_DIV;
					}
					/* Let's do a little filtering here as well */
					if(rawlen >= minrawlen && rawlen <= maxrawlen) {
						receive_queue(rawcode, rawlen, plslen, hw->type);
					}
					rawlen = 0;
				}
			}
			pthread_mutex_unlock(&receive_lock);
		} else {
			pthread_cond_wait(&receive_signal, &receive_lock);
		}
	}
	return (void *)NULL;
}

void *clientize(void *param) {
	steps_t steps = WELCOME;
	struct ssdp_list_t *ssdp_list = NULL;
    char *recvBuff = NULL;
	char *message = NULL;
	char *protocol = NULL;
	int client_type = 0;
	JsonNode *json = NULL;
	JsonNode *jreturn = NULL;
	int x = 0;
	int client_loop = 1;

	while(main_loop) {
		client_loop = 1;
		steps = WELCOME;

		ssdp_list = NULL;
		if(master_server && master_port > 0) {
			if((sockfd = socket_connect(master_server, master_port)) == -1) {
				logprintf(LOG_ERR, "could not connect to pilight-daemon");
				client_loop = 0;
			}
		} else if(ssdp_seek(&ssdp_list) == -1) {
			logprintf(LOG_ERR, "no pilight ssdp connections found");
			client_loop = 0;
		} else {
			if((sockfd = socket_connect(ssdp_list->ip, ssdp_list->port)) == -1) {
				logprintf(LOG_ERR, "could not connect to pilight-daemon");
				client_loop = 0;
			}
		}
		if(ssdp_list) {
			ssdp_free(ssdp_list);
		}

		while(client_loop) {
			if(steps > WELCOME) {
				/* Clear the receive buffer again and read the welcome message */
				if((recvBuff = socket_read(sockfd)) != NULL) {
					json = json_decode(recvBuff);
					json_find_string(json, "message", &message);
					logprintf(LOG_DEBUG, "socket recv_clientize: %s", recvBuff);
				} else {
					client_loop = 0;
					break;
				}
			}
			if(main_loop == 0) {
				break;
			}
			switch(steps) {
				case WELCOME:
					socket_write(sockfd, "{\"message\":\"client node\",\"uuid\":\"%s\"}", pilight_uuid);
					steps=IDENTIFY;
				break;
				case IDENTIFY:
					if(strcmp(message, "accept client") == 0) {
						steps=FORWARD;
					}
					if(strcmp(message, "reject client") == 0) {
						steps=REJECT;
					}
					sfree((void *)&recvBuff);
				case REQUEST:
					socket_write(sockfd, "{\"message\":\"request config\"}");
					steps=CONFIG;
					if(json) {
						json_delete(json);
						json = NULL;
					}
				break;
				case CONFIG:
					if((jreturn = json_find_member(json, "config"))) {
						config_parse(jreturn);
						json_delete(jreturn);
						steps=FORWARD;
					}
					if(json) {
						json_delete(json);
						json = NULL;
					}
					sfree((void *)&recvBuff);
				break;
				case FORWARD: {
					char *pch = strtok(recvBuff, "\n");
					if(json) {
						json_delete(json);
						json = NULL;
					}
					while(pch) {
						json = json_decode(recvBuff);
						if((jreturn = json_find_member(json, "incognito")) && jreturn->tag == JSON_STRING) {
							for(x=0;x<(sizeof(clients)/sizeof(clients[0]));x++) {
								if(strcmp(clients[x], jreturn->string_) == 0) {
									client_type = x;
									break;
								}
							}
						} else {
							if(client_type == SENDER) {
								client_sender_parse_code(-1, json);
							} else if(client_type == CONTROLLER) {
					            logprintf(LOG_DEBUG, "recv_clientize CON: %s", recvBuff);
								client_controller_parse_code(-1, json);
							} else if(client_type == -1) {
								if(!json_find_member(json, "config")) {
									if(json_find_string(json, "origin", &message) == 0 &&
									   json_find_string(json, "protocol", &protocol) == 0) {
										broadcast_queue(protocol, json);
									}
								}
							}
						}
						pch = strtok(NULL, "\n");
					}
					sfree((void *)&recvBuff);
				} break;
				case REJECT:
				default:
					if(recvBuff) {
						sfree((void *)&recvBuff);
					}
					main_loop = 0;
				break;
			}
		}

		if(json) {
			json_delete(json);
			json = NULL;
		}

		if(main_loop == 1) {
			config_gc();
			logprintf(LOG_NOTICE, "connection to main pilight daemon lost");
			logprintf(LOG_NOTICE, "trying to reconnect...");
			sleep(1);
		}
	}

	socket_close(sockfd);

	return NULL;
}

static void save_pid(pid_t npid) {
	int f = 0;
	char buffer[BUFFER_SIZE];
	memset(buffer, '\0', BUFFER_SIZE);
	if((f = open(pid_file, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR)) != -1) {
		lseek(f, 0, SEEK_SET);
		sprintf(buffer, "%d", npid);
		ssize_t i = write(f, buffer, strlen(buffer));
		if(i != strlen(buffer)) {
			logprintf(LOG_ERR, "could not store pid in %s", pid_file);
		}
	}
	close(f);
}

static void daemonize(void) {
	log_file_enable();
	log_shell_disable();
	/* Get the pid of the fork */
	pid_t npid = fork();
	switch(npid) {
		case 0:
		break;
		case -1:
			logprintf(LOG_ERR, "could not daemonize program");
			exit(1);
		break;
		default:
			save_pid(npid);
			logprintf(LOG_INFO, "daemon started with pid: %d", npid);
			exit(0);
		break;
	}
}

/* Garbage collector of main program */
int main_gc(void) {

	main_loop = 0;
	sending = 0;

	/* If we are running in node mode, the clientize
	   thread is waiting for a response from the main
	   daemon. This means we can't gracefull stop that
	   thread. However, by sending a HEART message the
	   main daemon will response with a BEAT. This allows
	   us to stop the socket_read function and properly
	   stop the clientize thread. */
	if(runmode == 2) {
		socket_write(sockfd, "HEART");
	}

	struct conf_hardware_t *tmp_confhw = conf_hardware;
	while(tmp_confhw) {
		if(tmp_confhw->hardware->deinit) {
			tmp_confhw->hardware->deinit();
		}
		tmp_confhw = tmp_confhw->next;
	}

	pthread_mutex_unlock(&recvqueue_lock);
	pthread_cond_signal(&recvqueue_signal);
	usleep(1000);

	pthread_mutex_unlock(&sendqueue_lock);
	pthread_cond_signal(&sendqueue_signal);

	pthread_mutex_unlock(&receive_lock);
	pthread_cond_signal(&receive_signal);

	pthread_mutex_unlock(&bcqueue_lock);
	pthread_cond_signal(&bcqueue_signal);

	struct nodes_t *tmp_nodes;
	while(nodes) {
		tmp_nodes = nodes;
		nodes = nodes->next;
		sfree((void *)&tmp_nodes);
	}

	if(running == 0) {
		/* Remove the stale pid file */
		if(access(pid_file, F_OK) != -1) {
			if(remove(pid_file) != -1) {
				logprintf(LOG_DEBUG, "removed stale pid_file %s", pid_file);
			} else {
				logprintf(LOG_ERR, "could not remove stale pid file %s", pid_file);
			}
		}
	}

	if(pid_file_free) {
		sfree((void *)&pid_file);
	}

#ifdef WEBSERVER
	if(webserver_enable == 1) {
		webserver_gc();
	}
	if(webserver_root_free) {
		sfree((void *)&webserver_root);
	}
	if(webgui_tpl_free) {
		sfree((void *)&webgui_tpl);
	}
#endif

#ifdef UPDATE
	if(update_check) {
		update_gc();
	}
#endif

	if(master_server) {
		sfree((void *)&master_server);
	}

	datetime_gc();
	ssdp_gc();
	protocol_gc();
	hardware_gc();
	settings_gc();
	options_gc();
	socket_gc();
	dso_gc();

	whitelist_free();
	threads_gc();
	pthread_join(pth, NULL);
	log_gc();

	sfree((void *)&nodes);
	sfree((void *)&progname);

	return 0;
}

static void procProtocolInit(void) {
	protocol_register(&procProtocol);
	protocol_set_id(procProtocol, "process");
	protocol_device_add(procProtocol, "process", "pilight proc. API");
	procProtocol->devtype = PROC;
	procProtocol->hwtype = API;
	procProtocol->multipleId = 0;
	procProtocol->config = 0;

	options_add(&procProtocol->options, 'c', "cpu", OPTION_HAS_VALUE, CONFIG_VALUE, JSON_NUMBER, NULL, NULL);
	options_add(&procProtocol->options, 'r', "ram", OPTION_HAS_VALUE, CONFIG_VALUE, JSON_NUMBER, NULL, NULL);
}

#ifdef FIRMWARE
void *firmware_loop(void *param) {
	unsigned int interval = 1;
	char fwfile[4096] = {'\0'};
	int fwupdate = 0;

	settings_find_number("firmware-update", &fwupdate);

	while(main_loop) {
		/* Check if firmware needs to be updated */
		if(fwupdate == 1 && firmware.version > 0) {
			char *fwtmp = fwfile;
			if(firmware_check(&fwtmp) == 0) {
				firmware.version = 0;
				size_t fwl = strlen(FIRMWARE_PATH)+strlen(fwfile)+2;
				char fwpath[fwl];
				memset(fwpath, '\0', fwl);
				sprintf(fwpath, "%s%s", FIRMWARE_PATH, fwfile);
				logprintf(LOG_INFO, "**** START UPD. FW ****");
				if(firmware_update(fwpath) != 0) {
					logprintf(LOG_INFO, "**** FAILED UPD. FW ****");
				} else {
					logprintf(LOG_INFO, "**** DONE UPD. FW ****");
				}
				fwupdate = 0;
			}
		interval = 60;
		}

		sleep(interval);
	}
	return NULL;
}
#endif

int main(int argc, char **argv) {

	procProtocolInit();

	struct ifaddrs *ifaddr, *ifa;
	int family = 0;
	char *p = NULL;

	progname = malloc(16);
	if(!progname) {
		logprintf(LOG_ERR, "out of memory");
		exit(EXIT_FAILURE);
	}
	strcpy(progname, "pilight-daemon");

	if(geteuid() != 0) {
		printf("%s requires root priveliges in order to run\n", progname);
		sfree((void *)&progname);
		exit(EXIT_FAILURE);
	}

	/* Run main garbage collector when quiting the daemon */
	gc_attach(main_gc);

	/* Catch all exit signals for gc */
	gc_catch();

#ifdef __FreeBSD__
	if(rep_getifaddrs(&ifaddr) == -1) {
		logprintf(LOG_ERR, "could not get network adapter information");
		goto clear;
	}
#else
	if(getifaddrs(&ifaddr) == -1) {
		perror("getifaddrs");
		goto clear;
	}
#endif

	for(ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
		if(ifa->ifa_addr == NULL) {
			continue;
		}

		family = ifa->ifa_addr->sa_family;

		if((strstr(ifa->ifa_name, "lo") == NULL && strstr(ifa->ifa_name, "vbox") == NULL
		    && strstr(ifa->ifa_name, "dummy") == NULL) && (family == AF_INET || family == AF_INET6)) {
			if((p = genuuid(ifa->ifa_name)) == NULL) {
				logprintf(LOG_ERR, "could not generate the device uuid");
				freeifaddrs(ifaddr);
				goto clear;
			} else {
				strcpy(pilight_uuid, p);
				sfree((void *)&p);
				break;
			}
		}
	}
#ifdef __FreeBSD__
	rep_freeifaddrs(ifaddr);
#else
	freeifaddrs(ifaddr);
#endif

	firmware.version = 0;
	firmware.lpf = 0;
	firmware.hpf = 0;

	log_level_set(LOG_INFO);

	log_file_enable();
	log_shell_disable();

	char settingstmp[] = SETTINGS_FILE;
	settings_set_file(settingstmp);

	struct socket_callback_t socket_callback;
	struct options_t *options = NULL;
	struct ssdp_list_t *ssdp_list = NULL;

	char buffer[BUFFER_SIZE];
	int f, itmp, show_help = 0, show_version = 0, show_default = 0;
	char *hwfile = NULL;
	char *stmp = NULL;
	char *args = NULL;
	int port = 0;

	memset(buffer, '\0', BUFFER_SIZE);

	options_add(&options, 'H', "help", OPTION_NO_VALUE, 0, JSON_NULL, NULL, NULL);
	options_add(&options, 'V', "version", OPTION_NO_VALUE, 0, JSON_NULL, NULL, NULL);
	options_add(&options, 'D', "nodaemon", OPTION_NO_VALUE, 0, JSON_NULL, NULL, NULL);
	options_add(&options, 'F', "settings", OPTION_HAS_VALUE, 0, JSON_NULL, NULL, NULL);
	options_add(&options, 'S', "server", OPTION_HAS_VALUE, 0, JSON_NULL, NULL, "^(([0-9]|[1-9][0-9]|1[0-9]{2}|2[0-4][0-9]|25[0-5]).){3}([0-9]|[1-9][0-9]|1[0-9]{2}|2[0-4][0-9]|25[0-5])$");
	options_add(&options, 'P', "port", OPTION_HAS_VALUE, 0, JSON_NULL, NULL, "[0-9]{1,4}");

	while(1) {
		int c;
		c = options_parse(&options, argc, argv, 1, &args);
		if(c == -1)
			break;
		if(c == -2) {
			show_help = 1;
			break;
		}
		switch(c) {
			case 'H':
				show_help = 1;
			break;
			case 'V':
				show_version = 1;
			break;
			case 'F':
				if(settings_set_file(args) == EXIT_FAILURE) {
					return EXIT_FAILURE;
				}
			break;
			case 'S':
				if(!(master_server = malloc(strlen(args)+1))) {
					logprintf(LOG_ERR, "out of memory");
					exit(EXIT_FAILURE);
				}
				strcpy(master_server, args);
			break;
			case 'P':
				master_port = (unsigned short)atoi(args);
			break;
			case 'D':
				nodaemon=1;
			break;
			default:
				show_default = 1;
			break;
		}
	}
	options_delete(options);

	if(show_help) {
		printf("Usage: %s [options]\n", progname);
		printf("\t -H --help\t\t\tdisplay usage summary\n");
		printf("\t -V --version\t\t\tdisplay version\n");
		printf("\t -F --settings\t\t\tsettings file\n");
		printf("\t -D --nodaemon\t\t\tdo not daemonize and\n");
		printf("\t -S --server=x.x.x.x\t\tconnect to server address\n");
		printf("\t -P --port=xxxx\t\t\tconnect to server port\n");
		printf("\t\t\t\t\tshow debug information\n");
		goto clear;
	}
	if(show_version) {
		printf("%s version %s, commit %s\n", progname, VERSION, HASH);
		goto clear;
	}
	if(show_default) {
		printf("Usage: %s [options]\n", progname);
		goto clear;
	}

	char pilight_learn[] = "pilight-learn";
	char pilight_debug[] = "pilight-debug";
	char pilight_raw[] = "pilight-raw";
	if((pid = findproc(pilight_raw, NULL, 1)) > 0) {
		logprintf(LOG_ERR, "pilight-raw instance found (%d)", (int)pid);
		goto clear;
	}

	if((pid = findproc(pilight_learn, NULL, 1)) > 0) {
		logprintf(LOG_ERR, "pilight-learn instance found (%d)", (int)pid);
		goto clear;
	}

	if((pid = findproc(pilight_debug, NULL, 1)) > 0) {
		logprintf(LOG_ERR, "pilight-debug instance found (%d)", (int)pid);
		goto clear;
	}

	if(settings_read() != 0) {
		goto clear;
	}

#ifdef WEBSERVER
	settings_find_number("webserver-enable", &webserver_enable);
	settings_find_number("webserver-port", &webserver_port);
	if(settings_find_string("webserver-root", &webserver_root) != 0) {
		webserver_root = realloc(webserver_root, strlen(WEBSERVER_ROOT)+1);
		if(!webserver_root) {
			logprintf(LOG_ERR, "out of memory");
			exit(EXIT_FAILURE);
		}
		strcpy(webserver_root, WEBSERVER_ROOT);
		webserver_root_free = 1;
	}
	if(settings_find_string("webgui-template", &webgui_tpl) != 0) {
		/* If no webserver port was set, use the default webserver port */
		webgui_tpl = malloc(strlen(WEBGUI_TEMPLATE)+1);
		if(!webgui_tpl) {
			logprintf(LOG_ERR, "out of memory");
			exit(EXIT_FAILURE);
		}
		strcpy(webgui_tpl, WEBGUI_TEMPLATE);
		webgui_tpl_free = 1;
	}
#endif

#ifdef UPDATE
	settings_find_number("update-check", &update_check);
#endif

	if(settings_find_string("pid-file", &pid_file) != 0) {
		pid_file = realloc(pid_file, strlen(PID_FILE)+1);
		if(!pid_file) {
			logprintf(LOG_ERR, "out of memory");
			exit(EXIT_FAILURE);
		}
		strcpy(pid_file, PID_FILE);
		pid_file_free = 1;
	}

	if((f = open(pid_file, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR)) != -1) {
		if(read(f, buffer, BUFFER_SIZE) != -1) {
			//If the file is empty, create a new process
			strcat(buffer, "\0");
			if(!atoi(buffer)) {
				running = 0;
			} else {
				//Check if the process is running
				kill(atoi(buffer), 0);
				//If not, create a new process
				if(errno == ESRCH) {
					running = 0;
				}
			}
		}
	} else {
		logprintf(LOG_ERR, "could not open / create pid_file %s", pid_file);
		goto clear;
	}
	close(f);

	if(settings_find_number("log-level", &itmp) == 0) {
		itmp += 2;
		log_level_set(itmp);
	}

	if(settings_find_string("log-file", &stmp) == 0) {
		log_file_set(stmp);
	}

	logprintf(LOG_INFO, "version %s, commit %s", VERSION, HASH);

	if(nodaemon == 1 || running == 1) {
		log_file_disable();
		log_shell_enable();
		log_level_set(LOG_DEBUG);
	}

	if(settings_find_number("send-repeats", &send_repeat) != 0) {
		send_repeat = SEND_REPEATS;
	}

	settings_find_number("receive-repeats", &receive_repeat);

	if(running == 1) {
		nodaemon=1;
		logprintf(LOG_NOTICE, "already active (pid %d)", atoi(buffer));
		log_level_set(LOG_NOTICE);
		log_shell_disable();
		goto clear;
	}

	/* Initialize peripheral modules */
	hardware_init();
	/* Initialize protocols */
	protocol_init();

	struct protocols_t *tmp = protocols;
	while(tmp) {
		if(tmp->listener->rawlen < minrawlen && tmp->listener->rawlen > 0) {
			minrawlen = tmp->listener->rawlen;
		}
		if(tmp->listener->minrawlen < minrawlen && tmp->listener->minrawlen > 0) {
			minrawlen = tmp->listener->minrawlen;
		}
		if(tmp->listener->rawlen > maxrawlen) {
			maxrawlen = tmp->listener->rawlen;
		}
		if(tmp->listener->maxrawlen > maxrawlen) {
			maxrawlen = tmp->listener->maxrawlen;
		}
		tmp = tmp->next;
	}

	if(settings_find_string("hardware-file", &hwfile) == 0) {
		hardware_set_file(hwfile);
		if(hardware_read() == EXIT_FAILURE) {
			goto clear;
		}
	} else {
		JsonNode *root = json_decode("{\"none\":{}}");
		hardware_parse(root);
		json_delete(root);
	}

	settings_find_number("port", &port);
	settings_find_number("standalone", &standalone);

	if(standalone == 0 || (master_server && master_port > 0)) {
		if(master_server && master_port > 0) {
			if((sockfd = socket_connect(master_server, master_port)) == -1) {
				logprintf(LOG_NOTICE, "no pilight daemon found, daemonizing");
			} else {
				logprintf(LOG_NOTICE, "a pilight daemon was found, clientizing");
				runmode = 2;
			}
		} else if(ssdp_seek(&ssdp_list) == -1) {
			logprintf(LOG_NOTICE, "no pilight daemon found, daemonizing");
		} else {
			logprintf(LOG_NOTICE, "a pilight daemon was found, clientizing");
			runmode = 2;
		}
		if(ssdp_list) {
			ssdp_free(ssdp_list);
		}
	}

	if(runmode == 1) {
		if(settings_find_string("config-file", &stmp) == 0) {
			if(config_set_file(stmp) == 0) {
				if(config_read() != 0) {
					goto clear;
				} else {
					receivers++;
				}

				if(log_level_get() >= LOG_DEBUG && nodaemon == 1) {
					config_print();
				}
			}
		}

		socket_start((unsigned short)port);
		if(standalone == 0) {
			ssdp_start();
		}
	}
	if(nodaemon == 0) {
		daemonize();
	} else {
		save_pid(getpid());
	}

	pthread_mutexattr_init(&sendqueue_attr);
	pthread_mutexattr_settype(&sendqueue_attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&sendqueue_lock, &sendqueue_attr);

	pthread_mutexattr_init(&recvqueue_attr);
	pthread_mutexattr_settype(&recvqueue_attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&recvqueue_lock, &recvqueue_attr);

	pthread_mutexattr_init(&receive_attr);
	pthread_mutexattr_settype(&receive_attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&receive_lock, &receive_attr);

	pthread_mutexattr_init(&bcqueue_attr);
	pthread_mutexattr_settype(&bcqueue_attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&bcqueue_lock, &bcqueue_attr);

    //initialise all handshakes to -1 so not checked
	memset(handshakes, -1, sizeof(handshakes));

	/* Export certain daemon function to global usage */
	pilight.broadcast = &broadcast_queue;
	pilight.send = &send_queue;
	pilight.receive = &receive_queue;

	/* Run certain daemon functions from the socket library */
    socket_callback.client_disconnected_callback = &socket_client_disconnected;
    socket_callback.client_connected_callback = NULL;
    socket_callback.client_data_callback = &socket_parse_data;

	/* Start threads library that keeps track of all threads used */
	threads_create(&pth, NULL, &threads_start, (void *)NULL);

	/* The daemon running in client mode, register a seperate thread that
	   communicates with the server */
	if(runmode == 2) {
		threads_register("node", &clientize, (void *)NULL, 0);
	} else {
		/* Register a seperate thread for the socket server */
		threads_register("socket", &socket_wait, (void *)&socket_callback, 0);
		if(standalone == 0) {
			threads_register("ssdp", &ssdp_wait, (void *)NULL, 0);
		}
	}
	threads_register("sender", &send_code, (void *)NULL, 0);
	threads_register("broadcaster", &broadcast, (void *)NULL, 0);

#ifdef UPDATE
	if(update_check && runmode == 1) {
		threads_register("updater", &update_poll, (void *)NULL, 0);
	}
#endif

	struct conf_hardware_t *tmp_confhw = conf_hardware;
	while(tmp_confhw) {
		if(tmp_confhw->hardware->init) {
			if(tmp_confhw->hardware->init() == EXIT_FAILURE) {
				logprintf(LOG_ERR, "could not initialize %s hardware mode", tmp_confhw->hardware->id);
				goto clear;
			}
			threads_register(tmp_confhw->hardware->id, &receive_code, (void *)tmp_confhw->hardware, 0);
		}
		tmp_confhw = tmp_confhw->next;
	}

	threads_register("receive parser", &receive_parse_code, (void *)NULL, 0);

#ifdef WEBSERVER
	/* Register a seperate thread for the webserver */
	if(webserver_enable == 1 && runmode == 1) {
		webserver_start();
		/* Register a seperate thread in which the webserver communicates
		   the main daemon as if it where a gui */
		threads_register("webserver client", &webserver_clientize, (void *)NULL, 0);
		threads_register("webserver broadcast", &webserver_broadcast, (void *)NULL, 0);
	} else {
		webserver_enable = 0;
	}
#endif

#ifdef FIRMWARE
	threads_register("firmware upgrader", &firmware_loop, (void *)NULL, 0);
#endif

	int checkram = 0, checkcpu = 0;
	int i = -1;
	while(main_loop) {
		double cpu = 0.0, ram = 0.0;
		cpu = getCPUUsage();
		ram = getRAMUsage();

		if((i > -1) && (cpu > 60)) {
			threads_cpu_usage();
			if(checkcpu == 0) {
				if(cpu > 90) {
					logprintf(LOG_ERR, "cpu usage way too high %f%", cpu);
				} else {
					logprintf(LOG_ERR, "cpu usage too high %f%", cpu);
				}
				logprintf(LOG_ERR, "checking again in 10 seconds");
				sleep(10);
			} else {
				if(cpu > 90) {
					logprintf(LOG_ERR, "cpu usage still way too high %f%, exiting", cpu);
				} else {
					logprintf(LOG_ERR, "cpu usage still too high %f%, stopping", cpu);
				}
			}
			if(checkcpu == 1) {
				if(cpu > 90) {
					exit(EXIT_FAILURE);
				} else {
					goto clear;
				}
			}
			checkcpu = 1;
		} else if((i > -1) && (ram > 60)) {
			if(checkram == 0) {
				if(ram > 90) {
					logprintf(LOG_ERR, "ram usage way too high %f%", ram);
					exit(EXIT_FAILURE);
				} else {
					logprintf(LOG_ERR, "ram usage too high %f%", ram);
				}
				logprintf(LOG_ERR, "checking again in 10 seconds");
				sleep(10);
			} else {
				if(ram > 90) {
					logprintf(LOG_ERR, "ram usage still way too high %f%, exiting", ram);
				} else {
					logprintf(LOG_ERR, "ram usage still too high %f%, stopping", ram);
				}
			}
			if(checkram == 1) {
				if(ram > 90) {
					exit(EXIT_FAILURE);
				} else {
					goto clear;
				}
			}
			checkram = 1;
		} else {
			checkcpu = 0;
			checkram = 0;
			if((i > 0 && i%3 == 0) || (i == -1)) {
				procProtocol->message = json_mkobject();
				JsonNode *code = json_mkobject();
				json_append_member(code, "cpu", json_mknumber(cpu));
				if(ram > 0) {
					json_append_member(code, "ram", json_mknumber(ram));
				}

				json_append_member(procProtocol->message, "values", code);
				json_append_member(procProtocol->message, "origin", json_mkstring("config"));
				json_append_member(procProtocol->message, "type", json_mknumber(PROC));
				pilight.broadcast(procProtocol->id, procProtocol->message);
				procProtocol->message = NULL;
				i = 0;
			}
			i++;
		}
		sleep(1);
	}

	return EXIT_SUCCESS;

clear:
	if(nodaemon == 0) {
		log_level_set(LOG_NOTICE);
		log_shell_disable();
	}
	if(main_loop == 1) {
		main_gc();
		gc_clear();
	}
	return EXIT_FAILURE;
}
