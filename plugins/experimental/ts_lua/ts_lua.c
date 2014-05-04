/*
  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0
 
  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/


#include <string.h>
#include <errno.h>
#include <pthread.h>

#include "ts_lua_util.h"

#define TS_LUA_MAX_STATE_COUNT                  2048

static volatile int32_t ts_lua_http_next_id = 0;
static volatile int32_t ts_lua_g_http_next_id = 0;

ts_lua_main_ctx         *ts_lua_main_ctx_array;
ts_lua_main_ctx         *ts_lua_g_main_ctx_array;

TSReturnCode
TSRemapInit(TSRemapInterface *api_info, char * errbuf ATS_UNUSED , int errbuf_size ATS_UNUSED )
{
    int     ret;

    if (!api_info || api_info->size < sizeof(TSRemapInterface))
        return TS_ERROR;

    ts_lua_main_ctx_array = TSmalloc(sizeof(ts_lua_main_ctx) * TS_LUA_MAX_STATE_COUNT);
    memset(ts_lua_main_ctx_array, 0, sizeof(ts_lua_main_ctx) * TS_LUA_MAX_STATE_COUNT);

    ret = ts_lua_create_vm(ts_lua_main_ctx_array, TS_LUA_MAX_STATE_COUNT);

    if (ret) {
        ts_lua_destroy_vm(ts_lua_main_ctx_array, TS_LUA_MAX_STATE_COUNT);
        TSfree(ts_lua_main_ctx_array);
        return TS_ERROR;
    }

    return TS_SUCCESS;
}

TSReturnCode
TSRemapNewInstance(int argc, char* argv[], void** ih, char* errbuf ATS_UNUSED , int errbuf_size ATS_UNUSED )
{
    int     ret = 0;

    if (argc < 3) {
        fprintf(stderr, "[%s] lua script file required !!", __FUNCTION__);
        return TS_ERROR;
    }

    if (strlen(argv[2]) >= TS_LUA_MAX_SCRIPT_FNAME_LENGTH - 16)
        return TS_ERROR;

    ts_lua_instance_conf *conf = TSmalloc(sizeof(ts_lua_instance_conf));
    if (!conf) {
        fprintf(stderr, "[%s] TSmalloc failed !!", __FUNCTION__);
        return TS_ERROR;
    }

    sprintf(conf->script, "%s", argv[2]);

    ret = ts_lua_add_module(conf, ts_lua_main_ctx_array, TS_LUA_MAX_STATE_COUNT, argc-2, &argv[2]);

    if (ret != 0) {
        fprintf(stderr, "[%s] ts_lua_add_module failed", __FUNCTION__);
        return TS_ERROR;
    }

    *ih = conf;

    return TS_SUCCESS;
}

void
TSRemapDeleteInstance(void* ih)
{
    TSfree(ih);
    return;
}

TSRemapStatus
TSRemapDoRemap(void* ih, TSHttpTxn rh, TSRemapRequestInfo *rri)
{
    int                 ret;
    int64_t             req_id;

    TSCont              contp;
    lua_State           *l;

    ts_lua_main_ctx     *main_ctx;
    ts_lua_http_ctx     *http_ctx;

    ts_lua_instance_conf     *instance_conf;

    instance_conf = (ts_lua_instance_conf*)ih;
    req_id = (int64_t) ts_lua_atomic_increment((&ts_lua_http_next_id), 1);

    main_ctx = &ts_lua_main_ctx_array[req_id%TS_LUA_MAX_STATE_COUNT];

    TSMutexLock(main_ctx->mutexp);

    http_ctx = ts_lua_create_http_ctx(main_ctx, instance_conf);

    http_ctx->txnp = rh;
    http_ctx->client_request_bufp = rri->requestBufp;
    http_ctx->client_request_hdrp = rri->requestHdrp;
    http_ctx->client_request_url = rri->requestUrl;
    http_ctx->remap = 1;
    l = http_ctx->lua;

    lua_getglobal(l, TS_LUA_FUNCTION_REMAP);
    if (lua_type(l, -1) != LUA_TFUNCTION) {
        TSMutexUnlock(main_ctx->mutexp);
        return TSREMAP_NO_REMAP;
    }

    contp = TSContCreate(ts_lua_http_cont_handler, NULL);
    TSContDataSet(contp, http_ctx);
    http_ctx->main_contp = contp;

    if (lua_pcall(l, 0, 1, 0) != 0) {
        fprintf(stderr, "lua_pcall failed: %s\n", lua_tostring(l, -1));
    }

    ret = lua_tointeger(l, -1);
    lua_pop(l, 1);

    TSHttpTxnHookAdd(rh, TS_HTTP_TXN_CLOSE_HOOK, contp);

    TSMutexUnlock(main_ctx->mutexp);

    return ret;
}

static int
globalHookHandler(TSCont contp, TSEvent event, void *edata) {
  TSHttpTxn txnp = (TSHttpTxn) edata;

  int ret = 0;
  int64_t req_id;

  lua_State *l;
  TSCont txn_contp;

  ts_lua_main_ctx     *main_ctx;
  ts_lua_http_ctx     *http_ctx;

  ts_lua_instance_conf *conf = (ts_lua_instance_conf *)TSContDataGet(contp);

  req_id = (int64_t) ts_lua_atomic_increment((&ts_lua_g_http_next_id), 1);
  main_ctx = &ts_lua_g_main_ctx_array[req_id%TS_LUA_MAX_STATE_COUNT];

  TSMutexLock(main_ctx->mutexp);

  http_ctx = ts_lua_create_http_ctx(main_ctx, conf);
  http_ctx->txnp = txnp;
  http_ctx->remap = 0;

  TSMBuffer bufp;
  TSMLoc hdr_loc;
  TSMLoc url_loc;

  if(TSHttpTxnClientReqGet(txnp, &bufp, &hdr_loc) == TS_SUCCESS) {
    http_ctx->client_request_bufp = bufp;
    http_ctx->client_request_hdrp = hdr_loc;
    if (TSHttpHdrUrlGet(bufp, hdr_loc, &url_loc) == TS_SUCCESS) {
      http_ctx->client_request_url = url_loc;
    }
  }

  if(!http_ctx->client_request_hdrp) {
    TSMutexUnlock(main_ctx->mutexp);
    TSHttpTxnReenable(txnp,TS_EVENT_HTTP_CONTINUE);
    return 0;
  }
 
  l = http_ctx->lua;

  switch (event) {
  case TS_EVENT_HTTP_READ_REQUEST_HDR:
    lua_getglobal(l, TS_LUA_FUNCTION_G_READ_REQUEST);
    break;

  case TS_EVENT_HTTP_SEND_REQUEST_HDR:
    lua_getglobal(l, TS_LUA_FUNCTION_G_SEND_REQUEST);
    break;

  case TS_EVENT_HTTP_READ_RESPONSE_HDR:
    lua_getglobal(l, TS_LUA_FUNCTION_G_READ_RESPONSE);
    break;

  case TS_EVENT_HTTP_SEND_RESPONSE_HDR:
    lua_getglobal(l, TS_LUA_FUNCTION_G_SEND_RESPONSE);
    break;

  case TS_EVENT_HTTP_CACHE_LOOKUP_COMPLETE:
    lua_getglobal(l, TS_LUA_FUNCTION_G_CACHE_LOOKUP_COMPLETE);
    break;

  default:
    TSMutexUnlock(main_ctx->mutexp);
    TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
    return 0;
    break;
  }

  if (lua_type(l, -1) != LUA_TFUNCTION) {
      TSMutexUnlock(main_ctx->mutexp);
      TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
      return 0;
  }

  txn_contp = TSContCreate(ts_lua_http_cont_handler, NULL);
  TSContDataSet(txn_contp, http_ctx);
  http_ctx->main_contp = txn_contp;

  if (lua_pcall(l, 0, 1, 0) != 0) {
      fprintf(stderr, "lua_pcall failed: %s\n", lua_tostring(l, -1));
  }

  ret = lua_tointeger(l, -1);
  lua_pop(l, 1);

  TSHttpTxnHookAdd(txnp, TS_HTTP_TXN_CLOSE_HOOK, txn_contp);

  TSMutexUnlock(main_ctx->mutexp);

  TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
  return ret;
}

void
TSPluginInit(int argc, const char *argv[]) {
    int ret = 0;
    ts_lua_g_main_ctx_array = TSmalloc(sizeof(ts_lua_main_ctx) * TS_LUA_MAX_STATE_COUNT);
    memset(ts_lua_g_main_ctx_array, 0, sizeof(ts_lua_main_ctx) * TS_LUA_MAX_STATE_COUNT);
    
    ret = ts_lua_create_vm(ts_lua_g_main_ctx_array, TS_LUA_MAX_STATE_COUNT);
    
    if (ret) {
      ts_lua_destroy_vm(ts_lua_g_main_ctx_array, TS_LUA_MAX_STATE_COUNT);
      TSfree(ts_lua_g_main_ctx_array);
      return;
    }
    
    if (argc < 2) {
      TSError("[%s] lua script file required !!", __FUNCTION__);
      return;
    }
    
    if (strlen(argv[1]) >= TS_LUA_MAX_SCRIPT_FNAME_LENGTH - 16) {
      TSError("[%s] lua script file name too long !!", __FUNCTION__);
      return;
    }
    
    ts_lua_instance_conf *conf = TSmalloc(sizeof(ts_lua_instance_conf));
    if (!conf) {
      TSError("[%s] TSmalloc failed !!", __FUNCTION__);
      return;
    }
    
    sprintf(conf->script, "%s", argv[1]);
    
    ret = ts_lua_add_module(conf, ts_lua_g_main_ctx_array, TS_LUA_MAX_STATE_COUNT, argc-1, (char**)&argv[1]);
    
    if (ret != 0) {
      TSError("[%s] ts_lua_add_module failed", __FUNCTION__);
      return;
    }

    TSCont global_contp = TSContCreate(globalHookHandler, NULL);
    if (!global_contp) {
      TSError("[%s] Could not create global continuation", __FUNCTION__);
      return;
    }
    TSContDataSet(global_contp, conf);

    TSHttpHookAdd(TS_HTTP_READ_REQUEST_HDR_HOOK, global_contp);
    TSHttpHookAdd(TS_HTTP_SEND_REQUEST_HDR_HOOK, global_contp);
    TSHttpHookAdd(TS_HTTP_READ_RESPONSE_HDR_HOOK, global_contp);
    TSHttpHookAdd(TS_HTTP_SEND_RESPONSE_HDR_HOOK, global_contp);
    TSHttpHookAdd(TS_HTTP_CACHE_LOOKUP_COMPLETE_HOOK, global_contp);
 
}
