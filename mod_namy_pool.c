/* vim: set expandtab tabstop=2 shiftwidth=2 softtabstop=2 filetype=c: */
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include <sys/shm.h>  
#include <sys/stat.h>  
#include <sys/types.h>  
#include <sys/wait.h>
#include <sys/ipc.h>  
#include <sys/sem.h>  
#include <string.h>

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

// 面倒なやつはdefine
#define TRACE(...) ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_NOTICE, 0, r->server, __VA_ARGS__)
#define TRACES(...) ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_NOTICE, 0, s, __VA_ARGS__)
#define DEBUG(...) ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, r->server, __VA_ARGS__)
#define ISINT(val) do {\
  const char *p;\
  \
  for (p = val; *p; ++p) {\
    if (!apr_isdigit(*p)) {\
      return "Argument must be numeric!";\
    }\
  }\
} while (0)

extern module AP_MODULE_DECLARE_DATA namy_pool_module;

// util function
static int namy_sem_lock(int semid, int semnum)
{
  struct sembuf sembuffer;
  sembuffer.sem_num = semnum; 
  sembuffer.sem_op = -1;  
  sembuffer.sem_flg = SEM_UNDO;  
  return semop(semid, &sembuffer, 1);
}

static int namy_sem_unlock(int semid, int semnum)
{
  struct sembuf sembuffer;
  sembuffer.sem_num = semnum; 
  sembuffer.sem_op = 1;  
  sembuffer.sem_flg = SEM_UNDO;  
  return semop(semid, &sembuffer, 1);
}

// -------- start 外部API --------------------
/**
 * コネクション取得
 * @param r request_rec
 * @param connection_pool_name confで指定したコネクションプール名
 * @return MYSQL connection 
 */
MYSQL *namy_attach_pool_connection(request_rec *r, const char* connection_pool_name)
{
  // 引数チェック
  if (r == NULL)
    return NULL;

  namy_svr_hash *entry = ap_get_module_config(r->server->module_config, &namy_pool_module);
  if (entry == NULL)
    return NULL;

  namy_svr_cfg *svr = (namy_svr_cfg *)apr_hash_get(entry->table, connection_pool_name, APR_HASH_KEY_STRING);
  // 存在しないコネクションキー
  if (svr == NULL)
  {
    return NULL;
  }

  namy_connection *tmp = NULL;
  int select = getpid()%svr->connections;
  for (tmp = svr->next; tmp != NULL; tmp = tmp->next)
  {
      if (tmp->id  == select)
      {
        break;
      }
  }

  // 使用中なので記録 （空きを探すのもいいかも）
  if (tmp->info->in_use == 1)
  {
    /*
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
    */
    // 統計情報作成
    if(svr->lock(svr->sem, svr->connections) != 0)
    {
      TRACE("[mod_namy_pool]: lock failed for stat, sem:%d, id:%d", svr->sem, svr->connections);
      return NULL;
    }

    svr->stat->conflicted++; // コネクション待ち発生
    
    if(svr->unlock(svr->sem, svr->connections) != 0)
    {
      TRACE("[mod_namy_pool]: lock failed for stat, sem:%d, id:%d", svr->sem, svr->connections);
      return NULL;
    }
    DEBUG("[mod_namy_pool] %s: connection is too busy, wait = id:%d", connection_pool_name, tmp->id);
  }

  //　コネクションロック
  if(svr->lock(svr->sem, tmp->id) != 0)
  {
    TRACE("[mod_namy_pool]: conection lock failed, sem:%d, id:%d", svr->sem, tmp->id);
    return NULL;
  }
  
  // 使用中にする
  tmp->info->in_use = 1;
  tmp->info->count++;
  tmp->info->pid = getpid();
  // 統計情報
  struct timeval t;
  gettimeofday(&t, NULL);
  tmp->info->start = (double)t.tv_sec + (double)t.tv_usec * 1e-6;;


  // pingのコストが大きいならタイマーとか最終使用日時とかで回数を減らす
  //if (mysql_ping(tmp->mysql) != 0)
  //{
  //  TRACE("[mod_namy_pool] %s: connection ping failed, id:%d", connection_pool_name, tmp->id);
  //}

  return tmp->mysql;	

}

/**
 * コネクション解放 closeしないけど他のプロセスが利用できるようになる
 * @param r request_rec
 * @param connection_pool_name confで指定したコネクションプール名
 * @param MYSQL connection 
 * @return 1 成功 0 失敗
 */
int namy_detach_pool_connection(request_rec *r, MYSQL *mysql)
{
  // 引数チェック
  if (r == NULL || mysql == NULL)
    return !NAMY_OK;

  namy_svr_hash *entry = ap_get_module_config(r->server->module_config, &namy_pool_module);
  if (entry == NULL)
    return !NAMY_OK;

  apr_hash_index_t *hi;
  void *key, *val;
  // 各コネクションを取り出す
  for (hi = apr_hash_first(r->pool, entry->table); hi; hi = apr_hash_next(hi))
  {
    apr_hash_this(hi,(void*)&key, NULL, (void*)&val);
    // confで設定したコネクション情報取得
    namy_svr_cfg *svr = (namy_svr_cfg*)val;
    char *con_name = (char*)key;

    namy_connection *tmp = NULL;
    for (tmp = svr->next; tmp != NULL; tmp = tmp->next)
    {
      // 同一のコネクションかどうか
      // scrambleよりもポインタ比較の方が速いので採用
      //if(strncmp(tmp->mysql->scramble, mysql->scramble, SCRAMBLE_LENGTH) == 0)
      if (tmp->mysql == mysql)
      {
        break;
      }
    }

    // unknown connection 次のテーブルへ
    if (tmp == NULL)
    {
      continue;
    }

    // 解放
    tmp->info->in_use = 0;
    tmp->info->pid = 0;
    // 統計情報
    struct timeval t;
    gettimeofday(&t, NULL);
    double end = (double)t.tv_sec + (double)t.tv_usec * 1e-6;;
    double diff = end - tmp->info->start;
    tmp->info->avg = (tmp->info->avg + diff)/2;
    if (tmp->info->max < diff)
      tmp->info->max = diff;

    DEBUG("[mod_namy_pool] %s: connection is detached, id:%d", con_name, tmp->id);

    //　コネクションアンロック
    if(svr->unlock(svr->sem, tmp->id) != 0)
    {
      TRACE("[mod_namy_pool]: conection unlock failed, sem:%d, id:%d", svr->sem, tmp->id);
    }
    return NAMY_OK;
  }
  return NAMY_UNKNOWN_CONNECTION;
}

/**
 * コネクション解放 closeしないけど他のプロセスが利用できるようになる
 * @param r request_rec
 * @param connection_pool_name confで指定したコネクションプール名
 * @param MYSQL connection 
 * @return 1 成功 0 失敗
 */
void namy_close_pool_connection(server_rec *s)
{
  // 引数チェック
  if (s == NULL)
    return;

  namy_svr_hash *entry = ap_get_module_config(s->module_config, &namy_pool_module);
  if (entry == NULL)
    return;

  apr_hash_index_t *hi;
  void *key, *val;
  // 各コネクションを取り出す
  for (hi = apr_hash_first(NULL, entry->table); hi; hi = apr_hash_next(hi))
  {
    apr_hash_this(hi, (void*)&key, NULL, (void *)&val);
    // confで設定したコネクション情報取得
    namy_svr_cfg *svr = (namy_svr_cfg*)val;
    char *con_name = (char*)key;
   
    // コネクションクローズ 
    namy_connection *tmp;
    if (shmctl(svr->shm, 0, IPC_RMID) != 0)
    {
      TRACES("[mod_namy_pool] %s: svr->shm clean up error", con_name);
    }
      
    if(semctl(svr->sem, 0, IPC_RMID) != 0)
    {
      TRACES("[mod_namy_pool] %s: tmp->shm clean up error", con_name);
    }

    for (tmp = svr->next; tmp!=NULL; tmp = tmp->next)
    {
      //DEBUG("[mod_namy_pool] %s: connection is closed, id:%d scramble:%s",
      //  con_name, tmp->id, tmp->mysql->scramble);
      mysql_close(tmp->mysql);
    }
    TRACES("[mod_namy_pool] %s: connection is closed, server:%s",
      con_name, svr->server);
  }

}

/**
 * プールしてるコネクションかどうかチェック 
 * @param r request_rec
 * @param connection_pool_name confで指定したコネクションプール名
 * @param MYSQL connection 
 * @return 1 poolコネクション 0 NG 
 */
int namy_is_pooled_connection(request_rec *r, MYSQL *mysql)
{
  // 引数チェック
  if (r==NULL||mysql==NULL)
    return NAMY_UNKNOWN_CONNECTION;

  namy_svr_hash *entry = ap_get_module_config(r->server->module_config, &namy_pool_module);
  if (entry==NULL)
    return NAMY_UNKNOWN_CONNECTION;

  apr_hash_index_t *hi;
  void *key, *val;
  // 各コネクションを取り出す
  for (hi = apr_hash_first(NULL, entry->table); hi; hi = apr_hash_next(hi))
  {
    apr_hash_this(hi, (void*)&key, NULL, (void*)&val);
    // confで設定したコネクション情報取得
    namy_svr_cfg *svr = (namy_svr_cfg*)val;
    // 全コネクションチェック  
    namy_connection *tmp = NULL;
    for (tmp = svr->next; tmp != NULL; tmp = tmp->next)
    {
      // ポインタ(アドレスが同じ)調べるだけでいいんじゃないか？
      // 違うサーバーに接続した時にscrambleは一緒になるんじゃね？
      //if(strncmp(tmp->mysql->scramble, mysql->scramble, SCRAMBLE_LENGTH) == 0)
      if (tmp->mysql == mysql)
      {
        return NAMY_OK;
      }
    }
  }
  return NAMY_UNKNOWN_CONNECTION;
}
// -------- end 外部API --------------------

// -------- start 内i部の関数 ---------------------
// conf解析関数
// 設定のサンプル
//  NAMY_POOL "test"
//        "server=localhost;db=namai;user=root;
//         port=3340;password=sss;socket=/var/lib/mysql/mysql.sock;
//         opt=0;connestion=5"
//
// 全項目
//  server=localhost;
//  db=namai;
//  user=root;
//  port=3340;
//  password=sss;
//  socket=/var/lib/mysql/mysql.sock;
//  option=0;
//  connestion=5
static const char *namy_parse_connection(cmd_parms *cmd, void *dbconf, const char *key, const char* db_string)
{
  namy_svr_hash *entry = ap_get_module_config(cmd->server->module_config, &namy_pool_module);

  // 実際のコネクション情報を作る
  namy_svr_cfg *svr = apr_pcalloc(cmd->pool, sizeof(namy_svr_cfg));

  // db string 解析
  char *setting, *last1, *last2;
  char *cpy_str = apr_pstrdup(cmd->pool, db_string);
  // string 解析
  while ((setting = apr_strtok(cpy_str, ";", &last1)))
  {
    char *name = apr_strtok(setting, "=", &last2);
    char *value;
    apr_collapse_spaces(name, name);
    value = apr_strtok(NULL, "=", &last2);
    //apr_collapse_spaces(value, value);

    if (!strcasecmp(name,"server"))
    {
      svr->server = value;
    }    
    else if (!strcasecmp(name,"db"))
    {
      svr->db = value;
    }
    else if (!strcasecmp(name,"user"))
    {
      svr->user = value;
    }
    else if (!strcasecmp(name,"port"))
    {
      ISINT(value);
      svr->port = atoi(value);
    }
    else if (!strcasecmp(name,"password"))
    {
      svr->pw = value;
    }
    else if (!strcasecmp(name,"socket"))
    {
      svr->socket = value;
    }
    else if (!strcasecmp(name,"option"))
    {
      ISINT(value);
      svr->option = atoi(value);
    }
    else if (!strcasecmp(name,"connestion"))
    {
      ISINT(value);
      svr->connections = atoi(value);
    }
    else
    {
      return db_string;
    }
    cpy_str = NULL;
  }
  svr->shm = 0;
  svr->sem = 0;
  svr->next = NULL;

  // TRACE
  ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, cmd->server,
      "[mod_namy_pool] server=%s, db=%s, user=%s, port=%d, passwd=%s, socket=%s, option=%d, connestion=%d",
      svr->server,
      svr->db,
      svr->user,
      svr->port,
      svr->pw,
      svr->socket,
      svr->option,
      svr->connections
      );

  // コネクションを保存
  apr_hash_set(entry->table, key, APR_HASH_KEY_STRING, (namy_svr_cfg*)svr);
  return NULL;
}

//
// メモリ解放、コネクション解放関数
//
static apr_status_t namy_pool_destroy(void *data)
{
  server_rec *s = data;
  namy_close_pool_connection(s);
  return APR_SUCCESS;
}

//
// 設定ファイル構造体 メモリ確保
//
static void *create_namy_pool_config(apr_pool_t *pool, server_rec *s)
{
  namy_svr_hash *svr = apr_pcalloc(pool, sizeof(namy_svr_hash));
  svr->table = apr_hash_make(pool);
  return svr;
}

//
// 設定ファイルで作られた情報から、コネクションを作る
//
static int namy_pool_post_config(apr_pool_t *pconf, apr_pool_t *plog,
    apr_pool_t *ptemp, server_rec *s)
{
  namy_svr_hash *entry = ap_get_module_config(s->module_config, &namy_pool_module);
  apr_hash_index_t *hi;
  void *key, *val;
  // 各コネクションを取り出す
  for (hi = apr_hash_first(pconf, entry->table); hi; hi = apr_hash_next(hi))
  {
    apr_hash_this(hi, (void*)&key, NULL, (void*)&val);
    // confで設定したコネクション情報取得
    namy_svr_cfg *svr = (namy_svr_cfg*)val;
    char *con_name = (char*)key;
    
    int segment;
    // info構造体 共有スペース確保
    // 使われた回数と利用中フラグを格納
    segment = shmget(IPC_PRIVATE, sizeof(namy_cinfo)*svr->connections + sizeof(namy_stat), S_IRUSR|S_IWUSR);  
    if (segment == -1)
    {
      TRACES("[mod_namy_pool] %s: namy_cinfo shmget error", con_name);
      return !OK;
    }
    // 全プロセスで使う共有スペースのポインタ
    namy_cinfo* info;
    info = (namy_cinfo*)shmat(segment, NULL, 0);
    svr->shm = segment;

    // セマフォ コネクション用排他処理
    // 統計情報用も含めて+1
    segment = semget(IPC_PRIVATE, svr->connections + 1, S_IRUSR|S_IWUSR);
    if (segment == -1)  
    {
      TRACES("[mod_namy_pool] %s: semaphore semget error", con_name);
      return !OK;
    }
    svr->sem = segment;

    int i;
    namy_connection *con;
    for (i=0; i<svr->connections; i++)
    {
      // コネクション用セマフォ初期か
      if (semctl(svr->sem, i, SETVAL, 1) != 0)
      {
        TRACES("[mod_namy_pool] %s: semaphore segment error", con_name);
        return !OK;
      }

      // 構造体作成
      con = (namy_connection*)apr_palloc(pconf, sizeof(namy_connection));
      con->id = i;
      con->info = info;
      con->info->in_use = 0;
      con->info->count = 0;
      con->info->pid = 0;
      con->next = NULL;
      // mysql connect
      MYSQL* mysql;
      mysql = mysql_init(NULL);

      //------------------------------------------------
      // Note: mysql_real_connect() incorrectly reset 
      // the MYSQL_OPT_RECONNECT option to its default value 
      // before MySQL 5.1.6. Therefore, prior to that version, 
      // if you want reconnect to be enabled for each connection, 
      // you must call mysql_options() with the MYSQL_OPT_RECONNECT 
      // option after each call to mysql_real_connect(). 
      // This is not necessary as of 5.1.6: Call mysql_options() 
      // only before mysql_real_connect() as usual. 
      //-------------------------------------------------
      my_bool my_true = TRUE;
      mysql_options(mysql, MYSQL_OPT_RECONNECT, &my_true);
      
      mysql_real_connect(mysql,
          svr->server, svr->user,
          svr->pw, svr->db, svr->port,
          svr->socket, svr->option);
      if (mysql == NULL)
      {
        TRACES("[mod_namy_pool] %s: connection to %s failed", con_name, svr->server);
        return !OK;
      }
      // copy to pool
      con->mysql = mysql;

      // 共有スペースのアドレスを先に進める
      info++;
      ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, s,
        "[mod_namy_pool] %s: connected = id:%d scramble:%s", con_name, con->id, con->mysql->scramble);

      // コネクションのリンクリスト
      namy_connection* tmp = svr->next;
      svr->next = con;
      con->next = tmp;
    }
    // 統計情報のアドレス
    svr->stat = (namy_stat*)info;
    svr->stat->conflicted = 0;
    svr->lock = (void *)&namy_sem_lock;
    svr->unlock = (void *)&namy_sem_unlock;
    // 統計情報のセマフォ初期化 セマフォ番号は０から始まるから+1しない
    if (semctl(svr->sem, svr->connections, SETVAL, 1) != 0)
    {
      TRACES("[mod_namy_pool] %s: stat semaphore segment error", con_name);
      return !OK;
    }
    TRACES("[mod_namy_pool] %s: connected to %s with %d conections", con_name, svr->server, svr->connections);
  }

  // コネクション待ちする時のため	
  srand((unsigned) time(NULL));
  // メモリ解放と、コネクション解放
  apr_pool_cleanup_register(pconf, s, namy_pool_destroy, apr_pool_cleanup_null);
  return OK;
}

//
// 統計情報を表示
//
static int namy_pool_info_handler(request_rec *r)
{
  if (strcmp(r->handler, "namy_pool")) {
    return DECLINED;
  }   
  r->content_type = "text/html";    

  ap_rputs("<html><body>\n", r); 

  ap_rputs(namy_pool_module.name, r); 
  ap_rputs("<br />\n", r); 

  namy_svr_hash *entry = ap_get_module_config(r->server->module_config, &namy_pool_module);
  apr_hash_index_t *hi;
  void *key, *val;
  // 各コネクションを取り出す
  for (hi = apr_hash_first(r->pool, entry->table); hi; hi = apr_hash_next(hi))
  {
    apr_hash_this(hi, (void*)&key, NULL, (void*)&val);
    // confで設定したコネクション情報取得
    namy_svr_cfg *svr = (namy_svr_cfg*)val;
    char *con_name = (char*)key;

    ap_rprintf(r, "<br /><b>Connection Pool Identity: <b>%s<br />", con_name);
    ap_rprintf(r, "<b>Server Name: </b>%s<br />", svr->server);
    ap_rprintf(r, "<b>Server Port: </b>%d<br />", svr->port);
    ap_rprintf(r, "<b>Server User: </b>%s<br />", svr->user);
    ap_rprintf(r, "<b>Server Database: </b>%s<br />", svr->db);
    ap_rprintf(r, "<b>Server SHM: </b>%d<br />", svr->shm);
    ap_rprintf(r, "<b>Connection SEM: </b>%d<br />", svr->sem);
    ap_rprintf(r, "<b>Conflict: </b>%ld<br />", svr->stat->conflicted);

    namy_connection *tmp = NULL;

    ap_rputs("<table border=\"1\"><tr>"
             "<td>connection id</td>"
             "<td>mysql scrable string</td>"
             "<td>count</td>"
             "<td>in use</td>"
             "<td>current user</td>"
             "<td>avg</td>"
             "<td>max</td>"
             "</tr>\n", r); 

    for (tmp = svr->next; tmp != NULL; tmp = tmp->next)
    {
      ap_rprintf(r, "<tr><td>%d</td><td>%s</td><td>%ld</td><td>%d</td><td>%d</td><td>%10.20f</td><td>%10.20f</td></tr>\n",
          tmp->id,
          ap_escape_html(r->pool, tmp->mysql->scramble),
          tmp->info->count,
          tmp->info->in_use,
          tmp->info->pid,
          tmp->info->avg,
          tmp->info->max
          );
    }

    ap_rputs("</table>", r);
  }

  ap_rputs("</body></html>\n", r); 
  return OK; 
}

//
// フック登録
//
static void namy_pool_hooks(apr_pool_t *pool)
{
  ap_hook_post_config(namy_pool_post_config, NULL, NULL, APR_HOOK_MIDDLE);
  ap_hook_handler(namy_pool_info_handler, NULL, NULL, APR_HOOK_MIDDLE);
}

//
// 設定ファイルのエントリー
//
static const command_rec namy_pool_cmds[] = {
  AP_INIT_ITERATE2("NAMY_POOL", namy_parse_connection, NULL, RSRC_CONF,
      "sample: \"server=localhost;db=namai;user=root;port=3340;password=sss;socket=/var/lib/mysql/mysql.sock;opt=0;connestion=5\""),
  {NULL}
};

//
// モジュール構造隊
//
module AP_MODULE_DECLARE_DATA namy_pool_module = {
  STANDARD20_MODULE_STUFF,
  NULL,
  NULL,
  create_namy_pool_config,
  NULL,
  namy_pool_cmds,
  namy_pool_hooks
};

