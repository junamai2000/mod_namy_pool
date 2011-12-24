/* vim: set expandtab tabstop=2 shiftwidth=2 softtabstop=2 filetype=c: */
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <mysql.h>

#include "http_protocol.h"
#include "http_config.h"
#include "http_log.h"
#include "http_request.h"

#define APR_WANT_MEMFUNC
#define APR_WANT_STRFUNC
#include "apr_reslist.h"
#include "apr_strings.h"
#include "apr_hash.h"
#include "apr_tables.h"
#include "apr_lib.h"
#include "apr_want.h"

#include "mod_namy_pool.h"

#define TRACE(...) ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_NOTICE, 0, s, __VA_ARGS__)

extern module AP_MODULE_DECLARE_DATA namy_pool_module;

// API
MYSQL *namy_attach_pool_connection(server_rec *s)
{
  namy_svr_cfg *svr = ap_get_module_config(s->module_config, &namy_pool_module);
  namy_connection *tmp = NULL;
  for (tmp = svr->next; tmp != NULL; tmp = tmp->next)
  {
    if (tmp->info->in_use == 0)
    {
      //TRACE("[mod_namy_pool] connection is attached, id:%d", tmp->id,);
      break;
    }
  }

  // 全部使用中なので最初のコネクションを待機
  if (tmp == NULL)
  {
    // ランダムで待機コネクションを選択
    int wait = rand()%svr->connections;
    for (tmp = svr->next; tmp != NULL; tmp = tmp->next)
    {
      // ランダムで指定されたコネクションを取得
      if (tmp->id  == wait)
      {
        break;
      }
    }
    TRACE("[mod_namy_pool] connection is too busy, wait = id:%d ramdom:%d", tmp->id, wait);
  }

  //　コネクションロック
  struct sembuf sembuffer;  
  sembuffer.sem_num = tmp->id;  
  sembuffer.sem_op = -1;  
  sembuffer.sem_flg = SEM_UNDO;  
  semop(tmp->shm, &sembuffer, 1);  
  // 使用中にする
  tmp->info->in_use = 1;
  tmp->info->num_of_used++;

  return tmp->mysql;	

}

int namy_detach_pool_connection(server_rec *s, MYSQL *mysql)
{
  namy_svr_cfg *svr = ap_get_module_config(s->module_config, &namy_pool_module);
  // 空きコネクション取得
  namy_connection *tmp = NULL;
  for (tmp = svr->next; tmp != NULL; tmp = tmp->next)
  {
    if(strncmp(tmp->mysql->scramble, mysql->scramble, SCRAMBLE_LENGTH) == 0)
    {
      break;
    }
  }

  // unknown connection
  if (tmp == NULL)
  {
    return NAMY_UNKNOWN_CONNECTION;
  }

  // 解放
  struct sembuf sembuffer;  
  tmp->info->in_use = 0;
  //TRACE("[mod_namy_pool] connection is detached, id:%d", tmp->id);

  sembuffer.sem_num = tmp->id;  
  sembuffer.sem_op = 1;  
  sembuffer.sem_flg = SEM_UNDO;  
  semop(tmp->shm, &sembuffer, 1);
  return NAMY_OK;
}

void namy_close_pool_connection(server_rec *s)
{
  namy_svr_cfg *svr = ap_get_module_config(s->module_config, &namy_pool_module);
  namy_connection *tmp;
  shmctl(svr->shm, IPC_RMID, NULL);
  for (tmp = svr->next; tmp!=NULL; tmp = tmp->next)
  {
    semctl(tmp->shm, 0, IPC_RMID);
    TRACE("[mod_namy_pool] connection is closed, id:%d scramble:%s", tmp->id, tmp->mysql->scramble);
    mysql_close(tmp->mysql);
  }
}

int namy_is_pooled_connection(server_rec *s, MYSQL *mysql)
{
  namy_svr_cfg *svr = ap_get_module_config(s->module_config, &namy_pool_module);
  namy_connection *tmp = NULL;
  for (tmp = svr->next; tmp != NULL; tmp = tmp->next)
  {
    if(strncmp(tmp->mysql->scramble, mysql->scramble, SCRAMBLE_LENGTH) == 0)
    {
      return NAMY_OK;
    }
  }
  return NAMY_UNKNOWN_CONNECTION;
}

/************ svr cfg: manage db namy pool ****************/
typedef enum { cmd_server, cmd_user, cmd_passwd, cmd_port,
  cmd_db, cmd_opt, cmd_cons, cmd_socket
} cmd_parts;

#define ISINT(val) do {\
  const char *p;\
  \
  for (p = val; *p; ++p) {\
    if (!apr_isdigit(*p)) {\
      return "Argument must be numeric!";\
    }\
  }\
} while (0)

static const char *namy_param(cmd_parms *cmd, void *dconf, const char *val)
{
  namy_svr_cfg *svr = ap_get_module_config(cmd->server->module_config, &namy_pool_module);
  switch ((long) cmd->info) {
    case cmd_server:
      svr->server = val;
      break;
    case cmd_user:
      svr->user = val;
      break;
    case cmd_passwd:
      svr->pw = val;
      break;
    case cmd_db:
      svr->db = val;
      break;
    case cmd_port:
      ISINT(val);
      svr->port= atoi(val);
      break;
    case cmd_socket:
      svr->socket= val;
      break;
    case cmd_opt:
      ISINT(val);
      svr->option = atoi(val);
      break;
    case cmd_cons:
      ISINT(val);
      svr->connections = atoi(val);
      break;
  }
  return NULL;
}

static apr_status_t namy_pool_destroy(void *data)
{
  server_rec *s = data;
  namy_close_pool_connection(s);
  return APR_SUCCESS;
}

static void *create_namy_pool_config(apr_pool_t *pool, server_rec *s)
{
  namy_svr_cfg *svr = apr_pcalloc(pool, sizeof(namy_svr_cfg));
  svr->server = NULL;
  svr->user = NULL;
  svr->db = NULL;
  svr->pw = NULL;
  svr->socket = NULL;
  svr->option = 0;
  svr->connections = 1;
  svr->port = 0;
  svr->next = NULL;
  return svr;
}

static int namy_pool_post_config(apr_pool_t *pconf, apr_pool_t *plog,
    apr_pool_t *ptemp, server_rec *s)
{
  int segment;
  namy_svr_cfg *svr = ap_get_module_config(s->module_config, &namy_pool_module);

  // info構造体 共有スペース確保
  // 使われた回数と利用中フラグを格納
  segment = shmget(IPC_PRIVATE, sizeof(namy_cinfo)*svr->connections, S_IRUSR|S_IWUSR);  
  if (segment == -1)
  {
    TRACE("[mod_namy_pool] namy_cinfo shmget error");
    return !OK;
  }
  // 全プロセスで使う共有スペースのポインタ
  namy_cinfo* info;
  info = (namy_cinfo*)shmat(segment, NULL, 0);
  svr->shm = segment;

  // セマフォ コネクション用排他処理
  segment = semget(IPC_PRIVATE, svr->connections, S_IRUSR|S_IWUSR);
  if (segment == -1)  
  {
    TRACE("[mod_namy_pool] semaphore semget error");
    return !OK;
  }

  int i;
  for (i=0; i<svr->connections; i++)
  {
    namy_connection *con;
    int n=0;
    if ((n=semctl(segment, i, SETVAL, 1)) != 0)
    {
      TRACE("[mod_namy_pool] semaphore segment error");
      return !OK;
    }

    // 構造体作成
    con = (namy_connection*)apr_palloc(pconf, sizeof(namy_connection));
    con->id = i;
    con->shm = segment;
    con->info = info;
    con->info->in_use=0;
    con->info->num_of_used=0;
    con->next = NULL;
    // mysql connect
    MYSQL* mysql;
    mysql = mysql_init(NULL);
    mysql_real_connect(mysql,
        svr->server, svr->user,
        svr->pw, svr->db, svr->port,
        svr->socket, svr->option);
    con->mysql = mysql;

    // 共有スペースのアドレスを先に進める
    info++;
    TRACE("[mod_namy_pool] connected = id:%d scramble:%s", con->id, con->mysql->scramble);

    // コネクションのリンクリスト
    namy_connection* tmp = svr->next;
    svr->next = con;
    con->next = tmp;
  }
  // コネクション待ちする時のため	
  srand((unsigned) time(NULL));
  // メモリ解放と、コネクション解放
  apr_pool_cleanup_register(pconf, s, namy_pool_destroy, apr_pool_cleanup_null);
  return OK;
}

static int namy_pool_info_handler(request_rec *r)
{
  if (strcmp(r->handler, "namy_pool")) {
    return DECLINED;
  }   
  r->content_type = "text/html";    

  ap_rputs("<html><body>\n", r); 

  ap_rputs(namy_pool_module.name, r); 
  ap_rputs("<br />\n", r); 

  namy_svr_cfg *svr = ap_get_module_config(r->server->module_config, &namy_pool_module);
  namy_connection *tmp = NULL;
  ap_rputs("<table border=\"1\"><tr><td>connection id</td><td>mysql scrable string</td><td>shm number</td><td>number of connection used</td><td>is connection used?</td></tr>\n", r); 
  for (tmp = svr->next; tmp != NULL; tmp = tmp->next)
  {
    ap_rprintf(r, "<tr><td>%d</td><td>%s</td><td>%d</td><td>%d</td><td>%d</td></tr>\n",
        tmp->id,
        ap_escape_html(r->pool, tmp->mysql->scramble),
        tmp->shm,
        tmp->info->num_of_used,
        tmp->info->in_use
        );
  }
  ap_rputs("</table>", r);

  ap_rputs("</body></html>\n", r); 
  return OK; 
}

static void namy_pool_hooks(apr_pool_t *pool)
{
  ap_hook_post_config(namy_pool_post_config, NULL, NULL, APR_HOOK_MIDDLE);
  ap_hook_handler(namy_pool_info_handler, NULL, NULL, APR_HOOK_MIDDLE);
  //  ap_hook_pre_config(pool_pre_config, NULL, NULL, APR_HOOK_MIDDLE);
  //  ap_hook_child_init(pool_child_init, NULL, NULL, APR_HOOK_MIDDLE);
  //  apr_dbd_init(pool);
}

static const command_rec namy_pool_cmds[] = {
  AP_INIT_TAKE1("NAMY_POOL_SERVER", namy_param, (void*)cmd_server, RSRC_CONF,
      "mysql server address"),
  AP_INIT_TAKE1("NAMY_POOL_USERNAME", namy_param, (void*)cmd_user, RSRC_CONF,
      "mysql user name"),
  AP_INIT_TAKE1("NAMY_POOL_PASSWORD", namy_param, (void*)cmd_passwd, RSRC_CONF,
      "mysql password"),
  AP_INIT_TAKE1("NAMY_POOL_PORT", namy_param, (void*)cmd_port, RSRC_CONF,
      "mysql port"),
  AP_INIT_TAKE1("NAMY_POOL_SOCKET", namy_param, (void*)cmd_socket, RSRC_CONF,
      "mysql socket"),
  AP_INIT_TAKE1("NAMY_POOL_DB", namy_param, (void*)cmd_db, RSRC_CONF,
      "mysql db name"),
  AP_INIT_TAKE1("NAMY_POOL_OPT", namy_param, (void*)cmd_opt, RSRC_CONF,
      "mysql option"),
  AP_INIT_TAKE1("NAMY_POOL_NUM_OF_CONNECTION", namy_param, (void*)cmd_cons, RSRC_CONF,
      "number of mysql conection"),
  {NULL}
};

module AP_MODULE_DECLARE_DATA namy_pool_module = {
  STANDARD20_MODULE_STUFF,
  NULL,
  NULL,
  create_namy_pool_config,
  NULL,
  namy_pool_cmds,
  namy_pool_hooks
};

