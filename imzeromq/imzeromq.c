/* imzeromq.c
 *
 * This input plugin enables rsyslog to read messages from a ZeroMQ
 * queue.
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

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rsyslog.h"

#include "cfsysline.h"
#include "config.h"
#include "datetime.h"
#include "dirty.h"
#include "errmsg.h"
#include "glbl.h"
#include "module-template.h"
#include "msg.h"
#include "net.h"
#include "parser.h"
#include "prop.h"
#include "ruleset.h"
#include "srUtils.h"
#include "unicode-helper.h"

#include <zmq.h>

// As described at http://www.zeromq.org/docs:3-0-upgrade
#ifndef ZMQ_DONTWAIT
#   define ZMQ_DONTWAIT     ZMQ_NOBLOCK
#endif
#if ZMQ_VERSION_MAJOR == 2
#   define zmq_sendmsg      zmq_send
#   define zmq_recvmsg      zmq_recv
#   define ZMQ_POLL_MSEC    1000        //  zmq_poll is usec
#elif ZMQ_VERSION_MAJOR == 3
#   define ZMQ_POLL_MSEC    1           //  zmq_poll is msec
#endif

MODULE_TYPE_INPUT
MODULE_TYPE_NOKEEP
MODULE_CNFNAME("imzeromq")

/* defines */

/* Module static data */
DEF_IMOD_STATIC_DATA
DEFobjCurrIf(errmsg)
DEFobjCurrIf(glbl)
DEFobjCurrIf(datetime)
DEFobjCurrIf(prop)
DEFobjCurrIf(ruleset)

static void *				s_context = NULL;
static ruleset_t *			s_rulesetp = NULL;
static size_t				s_nitems = 0;
static zmq_pollitem_t *		s_pollitems = NULL;
static ruleset_t **			s_rulesetps = NULL;
static size_t				s_maxline = 0;
static uchar *				s_rcvbuf = NULL;
static prop_t *				s_namep = NULL;

/* config settings */

/* accept a new ruleset to bind. Checks if it exists and complains, if not */
static rsRetVal set_ruleset(void __attribute__((unused)) *pVal, uchar *pszName)
{
	ruleset_t *pRuleset;
	rsRetVal localRet;
	DEFiRet;

    localRet = ruleset.GetRuleset(ourConf, &pRuleset, pszName);
	if(localRet == RS_RET_NOT_FOUND) {
		errmsg.LogError(0, NO_ERRCODE, "error: "
                        "ruleset '%s' not found - ignored", pszName);
	}
	CHKiRet(localRet);
	s_rulesetp = pRuleset;
	DBGPRINTF("imzeromq current bind ruleset %p: '%s'\n", pRuleset, pszName);

finalize_it:
	free(pszName); /* no longer needed */
	RETiRet;
}


static rsRetVal add_endpoint(void __attribute__((unused)) * oldp, uchar * valp)
{
    int rv;
	DEFiRet;

    // Make a writeable copy of the string so we can use strtok.
    char * copy = NULL;
    CHKmalloc(copy = strdup((char *) valp));

    char * connstr = NULL;
    char * bindstr = NULL;
    char * identstr = NULL;
    char * patternstr = NULL;
    int nsubs = 0;
    char ** subs = NULL;
    int pattern = ZMQ_PULL;

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
        // char * endp = NULL;

        if (strcmp(binding, "connect") == 0)
        {
            CHKmalloc(connstr = strdup(val));
        }
        else if (strcmp(binding, "bind") == 0)
        {
            CHKmalloc(bindstr = strdup(val));
        }
        else if (strcmp(binding, "identity") == 0)
        {
            CHKmalloc(identstr = strdup(val));
        }
        else if (strcmp(binding, "pattern") == 0)
        {
            CHKmalloc(patternstr = strdup(val));
        }
        else if (strcmp(binding, "subscribe") == 0)
        {
            // Add the subscription value to the list.
            char * substr = NULL;
            CHKmalloc(substr = strdup(val));
            CHKmalloc(subs = realloc(subs, sizeof(char *) * nsubs + 1));
            subs[nsubs] = substr;
            ++nsubs;
        }
        else
        {
            errmsg.LogError(0, NO_ERRCODE, "Unknown argument %s", binding);
            ABORT_FINALIZE(RS_RET_INVALID_PARAMS);
        }
    }

    // You must have either a connect or a bind.
    if (!connstr && !bindstr)
    {
		errmsg.LogError(0,
                        RS_RET_INVALID_PARAMS, "error: "
                        "you must have either a connect or a bind parameter");
		ABORT_FINALIZE(RS_RET_INVALID_PARAMS);
    }

    // You can't have both.
    if (connstr && bindstr)
    {
		errmsg.LogError(0,
                        RS_RET_INVALID_PARAMS, "error: "
                        "both connect and bind specified");
		ABORT_FINALIZE(RS_RET_INVALID_PARAMS);
    }

    // check for valid patterns (pull is default)
    if (patternstr)
    {
        if (strcmp(patternstr, "pull") == 0)
        {
            pattern = ZMQ_PULL;
        }
        else if (strcmp(patternstr, "sub") == 0)
        {
            pattern = ZMQ_SUB;
        }
        else
        {
            errmsg.LogError(0,
                            RS_RET_INVALID_PARAMS, "error: "
                            "invalid messaging pattern - use 'pull' or 'sub'");
            ABORT_FINALIZE(RS_RET_INVALID_PARAMS);
        }
    }

    // Only SUB pattern supports subscriptions.
    if (nsubs && pattern != ZMQ_SUB)
    {
        errmsg.LogError(0,
                        RS_RET_INVALID_PARAMS, "error: "
                        "subscribe specified for non SUB socket");
        ABORT_FINALIZE(RS_RET_INVALID_PARAMS);
    }

    // You need at least one subscription.
    if (pattern == ZMQ_SUB && nsubs == 0)
    {
        errmsg.LogError(0,
                        RS_RET_INVALID_PARAMS, "error: "
                        "at least one subscribe needed for SUB socket. "
                        "Hint: add subscribe=,");
        ABORT_FINALIZE(RS_RET_INVALID_PARAMS);
    }

    if (!s_context)
        s_context = zmq_init(1);
    if (!s_context)
    {
		errmsg.LogError(0,
                        RS_RET_INVALID_PARAMS,
                        "zmq_init failed: %s",
                        strerror(errno));
		ABORT_FINALIZE(RS_RET_INVALID_PARAMS);
    }

    void * sock = zmq_socket(s_context, pattern);
    if (!sock)
    {
		errmsg.LogError(0,
                        RS_RET_INVALID_PARAMS,
                        "zmq_socket failed: %s",
                        strerror(errno));
		ABORT_FINALIZE(RS_RET_INVALID_PARAMS);
    }

    // Set identities and options *before* the connect/bind.

    if (identstr)
    {
        rv = zmq_setsockopt(sock, ZMQ_IDENTITY,
                            identstr, strlen(identstr));
        if (rv)
        {
            errmsg.LogError(0,
                            RS_RET_INVALID_PARAMS,
                            "zmq_setsockopt ZMQ_IDENTITY failed: %s",
                            strerror(errno));
            ABORT_FINALIZE(RS_RET_INVALID_PARAMS);
        }
    }

    // Set subscriptions.
    int ii;
    for (ii = 0; ii < nsubs; ++ii)
    {
        rv = zmq_setsockopt(sock, ZMQ_SUBSCRIBE, subs[ii], strlen(subs[ii]));
        if (rv)
        {
            errmsg.LogError(0,
                            RS_RET_INVALID_PARAMS,
                            "zmq_setsockopt ZMQ_SUBSCRIBE failed: %s",
                            strerror(errno));
            ABORT_FINALIZE(RS_RET_INVALID_PARAMS);
        }
    }

    if (connstr)
    {
        rv = zmq_connect(sock, connstr);
        if (rv)
        {
            errmsg.LogError(0,
                            RS_RET_INVALID_PARAMS,
                            "zmq_connect failed: %s",
                            strerror(errno));
            ABORT_FINALIZE(RS_RET_INVALID_PARAMS);
        }
    }
    else
    {
        rv = zmq_bind(sock, bindstr);
        if (rv)
        {
            errmsg.LogError(0,
                            RS_RET_INVALID_PARAMS,
                            "zmq_bind failed: %s",
                            strerror(errno));
            ABORT_FINALIZE(RS_RET_INVALID_PARAMS);
        }
    }

    // We're adding a new element.
    size_t ndx = s_nitems++;

    // Allocate new vectors with an additional element.
    zmq_pollitem_t * tmppollitems;
    ruleset_t ** tmprulesetps;
    CHKmalloc(tmppollitems =
              (zmq_pollitem_t *) MALLOC(sizeof(zmq_pollitem_t) * s_nitems));
    CHKmalloc(tmprulesetps =
              (ruleset_t **) MALLOC(sizeof(ruleset_t *) * s_nitems));

    // Copy any existing values into the new vectors.
    if (ndx)
    {
        memcpy(tmppollitems, s_pollitems, sizeof(zmq_pollitem_t) * ndx);
        memcpy(tmprulesetps, s_rulesetps, sizeof(ruleset_t *) * ndx);
    }

    // Clear the new element.
    memset(&tmppollitems[ndx], 0, sizeof(zmq_pollitem_t));
    memset(&tmprulesetps[ndx], 0, sizeof(ruleset_t *));

    // Free any old storage.
    if (ndx)
    {
        free(s_pollitems);
        free(s_rulesetps);
    }

    // Store the new vectors in the globals.
    s_pollitems = tmppollitems;
    s_rulesetps = tmprulesetps;

    // Setup the pollitem.
    s_pollitems[ndx].socket = sock;
    s_pollitems[ndx].events = ZMQ_POLLIN;

    // Save the ruleset.
    s_rulesetps[ndx] = s_rulesetp;

finalize_it:
	free(valp); /* in any case, this is no longer needed */
	RETiRet;
}


rsRetVal rcv_loop(thrdInfo_t *pThrd)
{
	DEFiRet;

    long timeout = 1000 * ZMQ_POLL_MSEC;	// one second

    while (!pThrd->bShallStop)
    {
        zmq_poll(s_pollitems, s_nitems, timeout);
        size_t ii;
        for (ii = 0; ii < s_nitems; ++ii)
        {
            if (s_pollitems[ii].revents & ZMQ_POLLIN)
            {
                zmq_msg_t zmqmsg;

                zmq_msg_init(&zmqmsg);
                zmq_recvmsg(s_pollitems[ii].socket, &zmqmsg, 0);

                size_t sz = zmq_msg_size(&zmqmsg);
                if (sz > s_maxline - 1)
                    sz = s_maxline - 1;
                memcpy(s_rcvbuf, zmq_msg_data(&zmqmsg), sz);
                s_rcvbuf[sz] = '\0';	// Is this needed?

                msg_t * logmsg;
                if (msgConstruct(&logmsg) == RS_RET_OK)
                {
                    MsgSetRawMsg(logmsg, (char*) s_rcvbuf, sz);
                    MsgSetInputName(logmsg, s_namep);
                    MsgSetRuleset(logmsg, s_rulesetps[ii]);
                    MsgSetFlowControlType(logmsg, eFLOWCTL_NO_DELAY);
                    logmsg->msgFlags = NEEDS_PARSING;
                    submitMsg(logmsg);
                }

                zmq_msg_close(&zmqmsg);
            }
        }
    }

	RETiRet;
}

BEGINrunInput
CODESTARTrunInput
	iRet = rcv_loop(pThrd);
ENDrunInput


/* initialize and return if will run or not */
BEGINwillRun
CODESTARTwillRun
	/* we need to create the inputName property (only once during our
       lifetime) */
	CHKiRet(prop.Construct(&s_namep));
	CHKiRet(prop.SetString(s_namep,
                           UCHAR_CONSTANT("imzeromq"),
                           sizeof("imzeromq") - 1));
	CHKiRet(prop.ConstructFinalize(s_namep));

    // If there are no endpoints this is pointless ...
    if (s_nitems == 0)
		ABORT_FINALIZE(RS_RET_NO_RUN);

	s_maxline = glbl.GetMaxLine();

	CHKmalloc(s_rcvbuf = MALLOC((s_maxline + 1) * sizeof(char)));
finalize_it:
ENDwillRun


BEGINafterRun
CODESTARTafterRun
	/* do cleanup here */
    size_t ii;
    if (s_pollitems != NULL)
    {
        for (ii = 0; ii < s_nitems; ++ii)
            zmq_close(s_pollitems[ii].socket);

        free(s_pollitems);
        s_pollitems = NULL;

        free(s_rulesetps);
        s_rulesetps = NULL;
    }

	if(s_rcvbuf != NULL)
    {
		free(s_rcvbuf);
		s_rcvbuf = NULL;
	}
	if(s_namep != NULL)
		prop.Destruct(&s_namep);
ENDafterRun


BEGINmodExit
CODESTARTmodExit
	/* release what we no longer need */
	objRelease(errmsg, CORE_COMPONENT);
	objRelease(glbl, CORE_COMPONENT);
	objRelease(datetime, CORE_COMPONENT);
	objRelease(prop, CORE_COMPONENT);
    objRelease(ruleset, CORE_COMPONENT);
ENDmodExit


BEGINisCompatibleWithFeature
CODESTARTisCompatibleWithFeature
	if(eFeat == sFEATURENonCancelInputTermination)
		iRet = RS_RET_OK;
ENDisCompatibleWithFeature


BEGINqueryEtryPt
CODESTARTqueryEtryPt
CODEqueryEtryPt_STD_IMOD_QUERIES
CODEqueryEtryPt_IsCompatibleWithFeature_IF_OMOD_QUERIES
ENDqueryEtryPt

static rsRetVal resetConfigVariables(uchar __attribute__((unused)) *pp,
                                     void __attribute__((unused)) *pVal)
{
#if 0
	if(pszBindAddr != NULL) {
		free(pszBindAddr);
		pszBindAddr = NULL;
	}
    /* the default is to query only every second time */
	iTimeRequery = TIME_REQUERY_DFLT;
#endif
	return RS_RET_OK;
}


BEGINmodInit()
CODESTARTmodInit
    /* we only support the current interface specification */
	*ipIFVersProvided = CURR_MOD_IF_VERSION;
CODEmodInit_QueryRegCFSLineHdlr
	CHKiRet(objUse(errmsg, CORE_COMPONENT));
	CHKiRet(objUse(glbl, CORE_COMPONENT));
	CHKiRet(objUse(datetime, CORE_COMPONENT));
	CHKiRet(objUse(prop, CORE_COMPONENT));
    CHKiRet(objUse(ruleset, CORE_COMPONENT));

	/* register config file handlers */

    CHKiRet(omsdRegCFSLineHdlr(UCHAR_CONSTANT("inputzeromqserverbindruleset"),
                               0, eCmdHdlrGetWord, set_ruleset,
                               NULL, STD_LOADABLE_MODULE_ID));
    CHKiRet(omsdRegCFSLineHdlr(UCHAR_CONSTANT("inputzeromqserverrun"),
                               0, eCmdHdlrGetWord, add_endpoint,
                               NULL, STD_LOADABLE_MODULE_ID));
    CHKiRet(omsdRegCFSLineHdlr(UCHAR_CONSTANT("resetconfigvariables"),
                               1, eCmdHdlrCustomHandler, resetConfigVariables,
                               NULL, STD_LOADABLE_MODULE_ID));

ENDmodInit
/* vim:set ai:
 */
