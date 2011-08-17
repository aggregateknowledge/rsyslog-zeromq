/* omzeromq.c
 *
 * This output plugin enables rsyslog to write messages to a
 * ZeroMQ queue.
 *
 * Copyright 2011 Aggregate Knowledge, Inc.
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 * 
 * Author: Ken Sedgwick
 * <ken@aggregateknowledge.com>
 */

#include <zmq.h>

#include "config.h"
#include "rsyslog.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <wait.h>
#include "conf.h"
#include "syslogd-types.h"
#include "srUtils.h"
#include "template.h"
#include "unicode-helper.h"
#include "module-template.h"
#include "errmsg.h"
#include "cfsysline.h"

MODULE_TYPE_OUTPUT
MODULE_TYPE_NOKEEP

/* internal structures
 */
DEF_OMOD_STATIC_DATA
DEFobjCurrIf(errmsg)

typedef struct _instanceData {
    uchar *		connstr;
    uchar *		bindstr;
    int64		hwmsz;
    int64		swapsz;
    uchar *		identstr;
    int64		threads;
    int			pattern;

    void *		context;
    void *		socket;
} instanceData;

BEGINcreateInstance
CODESTARTcreateInstance
ENDcreateInstance


BEGINisCompatibleWithFeature
CODESTARTisCompatibleWithFeature
	if(eFeat == sFEATURERepeatedMsgReduction)
		iRet = RS_RET_OK;
ENDisCompatibleWithFeature


BEGINfreeInstance
CODESTARTfreeInstance
	if (pData->socket)
        zmq_close(pData->socket);
	if (pData->context)
        zmq_term(pData->context);
ENDfreeInstance


BEGINdbgPrintInstInfo
CODESTARTdbgPrintInstInfo
ENDdbgPrintInstInfo

static rsRetVal init_zeromq(instanceData *pData, int bSilent)
{
	DEFiRet;

    (void) bSilent;

	ASSERT(pData != NULL);
	ASSERT(pData->context == NULL);
	ASSERT(pData->socket == NULL);

    pData->context = zmq_init(pData->threads);
    pData->socket = zmq_socket(pData->context, pData->pattern);

    // Set options and identities *first* before the connect
    // and/or bind ...

    if (pData->hwmsz != -1)
        zmq_setsockopt(pData->socket, ZMQ_HWM,
                       &pData->hwmsz, sizeof(pData->hwmsz));

    if (pData->swapsz != -1)
        zmq_setsockopt(pData->socket, ZMQ_SWAP,
                       &pData->swapsz, sizeof(pData->swapsz));

    if (pData->identstr)
        zmq_setsockopt(pData->socket, ZMQ_IDENTITY,
                       pData->identstr, ustrlen(pData->identstr));

    if (pData->connstr)
        zmq_connect(pData->socket, (char *) pData->connstr);
    else
        zmq_bind(pData->socket, (char *) pData->bindstr);

	RETiRet;
}

BEGINtryResume
CODESTARTtryResume
    if (pData->socket == NULL) {
        iRet = init_zeromq(pData, 1);
    }
ENDtryResume


BEGINdoAction
CODESTARTdoAction
    if (pData->socket == NULL) {
        CHKiRet(init_zeromq(pData, 0));
    }

	size_t len = strlen((char *) ppString[0]);

    // If we're a REP socket, we check for an incoming request.
    if (pData->pattern == ZMQ_REP)
    {
        // ZMQ_REP sockets only send when we can read a request.
        zmq_msg_t reqmsg;
        zmq_msg_init(&reqmsg);
        int rc = zmq_recv(pData->socket, &reqmsg, 0);
        zmq_msg_close(&reqmsg);
        if (rc != 0)
        {
            // Something is wrong.
            iRet = RS_RET_SUSPENDED;
        }
        else
        {
            // OK, we read a request, return a response ...
            zmq_msg_t msg;
            zmq_msg_init_size(&msg, len);
            memcpy(zmq_msg_data(&msg), ppString[0], len);
            rc = zmq_send(pData->socket, &msg, ZMQ_NOBLOCK);
            zmq_msg_close(&msg);

            iRet = rc != 0 ? RS_RET_SUSPENDED : RS_RET_OK;
        }
    }
    else
    {
        // All other "normal" socket types come here and send.
        zmq_msg_t msg;
        zmq_msg_init_size(&msg, len);
        memcpy(zmq_msg_data(&msg), ppString[0], len);
        int rc = zmq_send(pData->socket, &msg, ZMQ_NOBLOCK);
        zmq_msg_close(&msg);

        iRet = rc != 0 ? RS_RET_SUSPENDED : RS_RET_OK;
    }

finalize_it:
ENDdoAction


BEGINparseSelectorAct
CODESTARTparseSelectorAct
CODE_STD_STRING_REQUESTparseSelectorAct(1)
	/* first check if this config line is actually for us */
	if(strncmp((char*) p, ":omzeromq:", sizeof(":omzeromq:") - 1)) {
		ABORT_FINALIZE(RS_RET_CONFLINE_UNPROCESSED);
	}

	/* ok, if we reach this point, we have something for us */
    /* eat indicator sequence  (-1 because of '\0'!) */
	p += sizeof(":omzeromq:") - 1;
	CHKiRet(createInstance(&pData));

    // Make a writeable copy of the string so we can use strtok.
    char * copy;
    CHKmalloc(copy = strdup((char *)p));

    // If there is a ';' replace it with a NULL
    char * semi = strchr(copy, ';');
    if (semi != NULL)
        *semi = '\0';

    pData->connstr = NULL;
    pData->bindstr = NULL;
    pData->pattern = ZMQ_PUSH;
    pData->hwmsz = -1;
	pData->swapsz = -1;
    pData->identstr = NULL;
    pData->context = NULL;
    pData->socket = NULL;
    pData->threads = 1;

    char * ptr1;
    char * binding;
    for (binding = strtok_r(copy, ",", &ptr1);
         binding != NULL;
         binding = strtok_r(NULL, ",", &ptr1))
    {
        // Each binding looks like foo=bar
        char * sep = strchr(binding, '=');
        if (sep == NULL)
        {
            errmsg.LogError(0, NO_ERRCODE,
                            "Invalid argument format %s, ignoring ...",
                            binding);
            continue;
        }

        // Replace '=' with '\0'.
        *sep = '\0';

        char * val = sep + 1;
        char * endp = NULL;

        if (strcmp(binding, "connect") == 0)
        {
            CHKmalloc(pData->connstr = (uchar*) strdup(val));
        }
        else if (strcmp(binding, "bind") == 0)
        {
            CHKmalloc(pData->bindstr = (uchar*) strdup(val));
        }
        else if (strcmp(binding, "hwm") == 0)
        {
            pData->hwmsz = strtoull(val, &endp, 10);
            if (*endp != '\0')
            {
                errmsg.LogError(0, NO_ERRCODE, "Invalid hwm value %s", val);
                ABORT_FINALIZE(RS_RET_INVALID_PARAMS);
            }
        }
        else if (strcmp(binding, "swap") == 0)
        {
            pData->swapsz = strtoull(val, &endp, 10);
            if (*endp != '\0')
            {
                errmsg.LogError(0, NO_ERRCODE, "Invalid swap value %s", val);
                ABORT_FINALIZE(RS_RET_INVALID_PARAMS);
            }
        }
        else if (strcmp(binding, "identity") == 0)
        {
            CHKmalloc(pData->identstr = (uchar*) strdup(val));
        }
        else if (strcmp(binding, "threads") == 0)
        {
            pData->threads = strtoull(val, &endp, 10);
            if (*endp != '\0')
            {
                errmsg.LogError(0, NO_ERRCODE, "Invalid threads value %s", val);
                ABORT_FINALIZE(RS_RET_INVALID_PARAMS);
            }
        }
        else if (strcmp(binding, "pattern") == 0)
        {
            if (strcmp(val, "push") == 0)
            {
                pData->pattern = ZMQ_PUSH;
            }
            else if (strcmp(val, "pub") == 0)
            {
                pData->pattern = ZMQ_PUB;
            }
            else if (strcmp(val, "rep") == 0)
            {
                pData->pattern = ZMQ_REP;
            }
            else
            {
                errmsg.LogError(0,
                                RS_RET_INVALID_PARAMS,
                                "error: invalid messaging pattern "
                                "- use 'push' or 'pub'");
                ABORT_FINALIZE(RS_RET_INVALID_PARAMS);
            }
        }
        else
        {
            errmsg.LogError(0, NO_ERRCODE, "Unknown argument %s", binding);
            ABORT_FINALIZE(RS_RET_INVALID_PARAMS);
        }
    }

    // You must have a connect or bind parameter.
    if (!pData->connstr && !pData->bindstr)
    {
		errmsg.LogError(0,
                        RS_RET_INVALID_PARAMS,
                        "error: connect or bind must be specified");
		ABORT_FINALIZE(RS_RET_INVALID_PARAMS);
    }

    // You can't have both connect and bind parameters.
    if (pData->connstr && pData->bindstr)
    {
		errmsg.LogError(0,
                        RS_RET_INVALID_PARAMS,
                        "error: both connect and bind specified");
		ABORT_FINALIZE(RS_RET_INVALID_PARAMS);
    }

    // Scan to the ';', or end of string.
    char * end = strchr((char *) p, ';');
    if (end != NULL)
        p = (uchar *) end + 1;
    else
        p += strlen((char *) p);

	/* check if a non-standard template is to be applied */
	if(*(p-1) == ';')
		--p;
	CHKiRet(cflineParseTemplateName(&p, *ppOMSR, 0, 0, (uchar*) "RSYSLOG_ForwardFormat"));
CODE_STD_FINALIZERparseSelectorAct
ENDparseSelectorAct


BEGINmodExit
CODESTARTmodExit
	CHKiRet(objRelease(errmsg, CORE_COMPONENT));
finalize_it:
ENDmodExit


BEGINqueryEtryPt
CODESTARTqueryEtryPt
CODEqueryEtryPt_STD_OMOD_QUERIES
ENDqueryEtryPt



/* Reset config variables for this module to default values.
 */
static rsRetVal resetConfigVariables(uchar __attribute__((unused)) *pp, void __attribute__((unused)) *pVal)
{
	DEFiRet;
	RETiRet;
}


BEGINmodInit()
CODESTARTmodInit
	*ipIFVersProvided = CURR_MOD_IF_VERSION; /* we only support the current interface specification */
CODEmodInit_QueryRegCFSLineHdlr
	CHKiRet(objUse(errmsg, CORE_COMPONENT));
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"resetconfigvariables", 1, eCmdHdlrCustomHandler, resetConfigVariables, NULL, STD_LOADABLE_MODULE_ID));
CODEmodInit_QueryRegCFSLineHdlr
ENDmodInit

/* vi:set ai:
 */
