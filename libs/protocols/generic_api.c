/*
	Copyright (C) 2013 CurlyMo

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
#include <dirent.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <math.h>

#include "../../pilight.h"
#include "common.h"
#include "dso.h"
#include "log.h"
#include "threads.h"
#include "protocol.h"
#include "hardware.h"
#include "binary.h"
#include "json.h"
#include "gc.h"
#include "generic_api.h"

static unsigned short generic_api_loop = 1;
static unsigned short generic_api_threads = 0;
static char generic_api[] = "/sys/class/thermal/thermal_zone0/temp";

static pthread_mutex_t generic_apilock;
static pthread_mutexattr_t generic_apiattr;

static void *genericApiParse(void *param) {
	struct protocol_threads_t *node = (struct protocol_threads_t *)param;
	struct JsonNode *json = (struct JsonNode *)node->param;
	struct JsonNode *jid = NULL;
	struct JsonNode *jchild = NULL;
	struct stat st;
	
	

	FILE *fp = NULL;
	double itmp = 0;
	int *id = malloc(sizeof(int));
	char *content = NULL;
	int interval = 10, temp_offset = 0;
	int nrloops = 0, nrid = 0, y = 0;
	size_t bytes = 0;

	generic_api_threads++;

	if(!id) {
		logprintf(LOG_ERR, "out of memory");
		exit(EXIT_FAILURE);
	}

	if((jid = json_find_member(json, "id"))) {
		jchild = json_first_child(jid);
		while(jchild) {
			if(json_find_number(jchild, "id", &itmp) == 0) {
				id = realloc(id, (sizeof(int)*(size_t)(nrid+1)));
				id[nrid] = (int)round(itmp);
				nrid++;
			}
			jchild = jchild->next;
		}
	}

	if(json_find_number(json, "poll-interval", &itmp) == 0)
		interval = (int)round(itmp);
	if(json_find_number(json, "device-temperature-offset", &itmp) == 0)
		temp_offset = (int)round(itmp);

	int temp = 1111;
	int dimlevel = 111;	
	while(generic_api_loop) {
		if(protocol_thread_wait(node, interval, &nrloops) == ETIMEDOUT) {
			pthread_mutex_lock(&generic_apilock);
			/*
			for(y=0;y<nrid;y++) {
				if((fp = fopen(generic_api, "rb"))) {
					fstat(fileno(fp), &st);
					bytes = (size_t)st.st_size;

					if(!(content = realloc(content, bytes+1))) {
						logprintf(LOG_ERR, "out of memory");
						exit(EXIT_FAILURE);
					}
					memset(content, '\0', bytes+1);

					if(fread(content, sizeof(char), bytes, fp) == -1) {
						logprintf(LOG_ERR, "cannot read file: %s", generic_api);
						fclose(fp);
						break;
					} else {
						fclose(fp);
						int temp = atoi(content)+temp_offset;
						sfree((void *)&content);

						genericApi->message = json_mkobject();
						JsonNode *code = json_mkobject();
						json_append_member(code, "id", json_mknumber(id[y]));
						json_append_member(code, "temperature", json_mknumber(temp));

						json_append_member(genericApi->message, "message", code);
						json_append_member(genericApi->message, "origin", json_mkstring("receiver"));
						json_append_member(genericApi->message, "protocol", json_mkstring(genericApi->id));

						pilight.broadcast(genericApi->id, genericApi->message);
						json_delete(genericApi->message);
						genericApi->message = NULL;
					}
				} else {
					logprintf(LOG_ERR, "CPU RPI device %s does not exists", generic_api);
				}
			}
			*/

					
					
					
			JsonNode *code = json_mkobject();
			genericApi->message = json_mkobject();
			json_append_member(code, "id", json_mknumber(id[y]));
			json_append_member(code, "temperature", json_mknumber(temp));
			json_append_member(genericApi->message, "message", code);
			json_append_member(genericApi->message, "origin", json_mkstring("receiver"));
			json_append_member(genericApi->message, "protocol", json_mkstring("alecto_wsd17"));
		    pilight.broadcast("alecto_wsd17", genericApi->message);
			json_delete(genericApi->message);
			genericApi->message = NULL;
			//code = NULL;	
			//json_delete(code);
            
            		
			genericApi->message = json_mkobject();
			JsonNode *code2 = json_mkobject();
			json_append_member(code2, "id", json_mknumber(id[y]));
			json_append_member(code2, "temperature", json_mknumber(temp));
			json_append_member(genericApi->message, "message", code2);
			json_append_member(genericApi->message, "origin", json_mkstring("receiver"));
			json_append_member(genericApi->message, "protocol", json_mkstring(genericApi->id));
			json_append_member(genericApi->message, "dimlevel", json_mknumber(dimlevel));

			pilight.broadcast(genericApi->id, genericApi->message);
			json_delete(genericApi->message);
			//json_delete(code2);
			genericApi->message = NULL;
			//*code = NULL;

            
			//temp = 1900;			
			genericApi->message = json_mkobject();
			JsonNode *code3 = json_mkobject();
			json_append_member(code3, "id", json_mknumber(5));
			json_append_member(code3, "temperature", json_mknumber(temp));

			json_append_member(genericApi->message, "message", code3);
			json_append_member(genericApi->message, "origin", json_mkstring("receiver"));
			json_append_member(genericApi->message, "protocol", json_mkstring("generic_api"));
			//json_append_member(genericApi->message, "dimlevel", json_mknumber(dimlevel));

			pilight.broadcast("generic_api", genericApi->message);
			json_delete(genericApi->message);
			genericApi->message = NULL;
			//json_delete(code3);
			//*code = NULL;
			
			
			JsonNode *code4 = json_mkobject();
			genericApi->message = json_mkobject();
			json_append_member(code4, "id", json_mknumber(4));
			json_append_member(code4, "temperature", json_mknumber(temp));
			json_append_member(genericApi->message, "message", code4);
			json_append_member(genericApi->message, "origin", json_mkstring("receiver"));
			json_append_member(genericApi->message, "protocol", json_mkstring("alecto_wsd17"));
		    pilight.broadcast("alecto_wsd17", genericApi->message);
			json_delete(genericApi->message);
			genericApi->message = NULL;
			//code = NULL;	
			//json_delete(code4);
            
			temp += 1;
			pthread_mutex_unlock(&generic_apilock);
		}
	}

	sfree((void *)&id);
	generic_api_threads--;
	return (void *)NULL;
}

static struct threadqueue_t *genericApiInitDev(JsonNode *jdevice) {
	generic_api_loop = 1;
	char *output = json_stringify(jdevice, NULL);
	JsonNode *json = json_decode(output);
	sfree((void *)&output);

	struct protocol_threads_t *node = protocol_thread_init(genericApi, json);
	return threads_register("generic_api", &genericApiParse, (void *)node, 0);
}

static void genericApiThreadGC(void) {
	generic_api_loop = 0;
	protocol_thread_stop(genericApi);
	while(generic_api_threads > 0) {
		usleep(10);
	}
	protocol_thread_free(genericApi);
}

static void genApiPrintHelp(void) {
	printf("\t -t --temperature=temperature\tset the temperature\n");
}

static int genApiCreateCode(JsonNode *code) {
	double itmp = 0;
	int id = -999;
	int temp = -999;
	int humi = -999;
	int batt = -1;

	if(json_find_number(code, "id", &itmp) == 0)
		id = (int)round(itmp);
	if(json_find_number(code, "temperature", &itmp) == 0)
		temp = (int)round(itmp);
	if(json_find_number(code, "humidity", &itmp) == 0)
		humi = (int)round(itmp);
	if(json_find_number(code, "battery", &itmp) == 0)
		batt = (int)round(itmp);

	if(id == -999 && temp == -999 && humi == -999 && batt == -1) {
		logprintf(LOG_ERR, "generic_api: insufficient number of arguments test");
		return EXIT_FAILURE;
	} 
	//else {
	//	genWeatherCreateMessage(id, temp, humi, batt);
	//}
	return EXIT_SUCCESS;
}


#ifndef MODULE
__attribute__((weak))
#endif
void genericApiInit(void) {
	pthread_mutexattr_init(&generic_apiattr);
	pthread_mutexattr_settype(&generic_apiattr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&generic_apilock, &generic_apiattr);

	protocol_register(&genericApi);
	protocol_set_id(genericApi, "generic_api");
	protocol_device_add(genericApi, "generic_api", "generic device via api");
	//protocol_device_add(genericApi, "kaku_dimmer", "genericApi KlikAanKlikUit Dimmers");
	genericApi->devtype = WEATHER;//DIMMER; //
	//genericApi->hwtype = SENSOR;

	options_add(&genericApi->options, 't', "temperature", OPTION_HAS_VALUE, CONFIG_VALUE, JSON_NUMBER, NULL, "^[0-9]{1,5}$");
	options_add(&genericApi->options, 'i', "id", OPTION_HAS_VALUE, CONFIG_ID, JSON_NUMBER, NULL, "[0-9]");
	options_add(&genericApi->options, 'd', "dimlevel", OPTION_HAS_VALUE, CONFIG_VALUE, JSON_NUMBER, NULL, "^([0-9]{1}|[1][0-5])$");

	options_add(&genericApi->options, 0, "device-decimals", OPTION_HAS_VALUE, CONFIG_SETTING, JSON_NUMBER, (void *)3, "[0-9]");
	options_add(&genericApi->options, 0, "device-temperature-offset", OPTION_HAS_VALUE, CONFIG_SETTING, JSON_NUMBER, (void *)0, "[0-9]");
	options_add(&genericApi->options, 0, "gui-decimals", OPTION_HAS_VALUE, CONFIG_SETTING, JSON_NUMBER, (void *)3, "[0-9]");
	options_add(&genericApi->options, 0, "gui-show-temperature", OPTION_HAS_VALUE, CONFIG_SETTING, JSON_NUMBER, (void *)1, "^[10]{1}$");
	options_add(&genericApi->options, 0, "poll-interval", OPTION_HAS_VALUE, CONFIG_SETTING, JSON_NUMBER, (void *)10, "[0-9]");
	
	options_add(&genericApi->options, 0, "dimlevel-minimum", OPTION_HAS_VALUE, CONFIG_SETTING, JSON_NUMBER, (void *)0, "^([0-9]{1}|[1][0-5])$");
	options_add(&genericApi->options, 0, "dimlevel-maximum", OPTION_HAS_VALUE, CONFIG_SETTING, JSON_NUMBER, (void *)15, "^([0-9]{1}|[1][0-5])$");
	options_add(&genericApi->options, 0, "gui-readonly", OPTION_HAS_VALUE, CONFIG_SETTING, JSON_NUMBER, (void *)0, "^[10]{1}$");

	genericApi->initDev=&genericApiInitDev;
	//clarus_switch->parseCode=&clarusSwParseCode; //Receiving?
	
	genericApi->threadGC=&genericApiThreadGC;
	genericApi->printHelp=&genApiPrintHelp;
	genericApi->createCode=&genApiCreateCode;  //Sending?
}

#ifdef MODULE
void compatibility(struct module_t *module) {
	module->name = "generic_api";
	module->version = "1.0";
	module->reqversion = "5.0";
	module->reqcommit = NULL;
}

void init(void) {
	genericApiInit();
}
#endif
