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

//FILE *fp;
//#define DEBUGF(...) fp=fopen("/tmp/log", "a+"); fprintf(fp,__VA_ARGS__); fclose(fp);

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

/*
 * アラートメール送信関数
 * @param path sendmail path
 * @param from mail from
 * @param to mail to
 * @param subject mail subject
 * @param msg body
 */
static void sendmail(const char* path, const char* from, const char* to, const char* subject, const char* body)
{
  FILE *fp;
  char buf[255]={0};
  snprintf(buf, sizeof(buf), "%s -t %s", path, from);
  fp = popen(buf, "w");
  if (fp == NULL) {
    perror("Error getting hostname");
    return;
  }   
  snprintf(buf, sizeof(buf), "To: %s \r\n", to);
  fputs(buf, fp);

  snprintf(buf, sizeof(buf), "From: %s \r\n", from);
  fputs(buf, fp);

  sprintf(buf, "Subject: %s\r\n\r\n", subject);
  fputs(buf, fp);

  fputs(body, fp);
  pclose(fp);
}

/**
 * セマフォロック 
 * @param semid semaphore id 
 * @param semnum semaphore number
 * @return 0 for success, otherwise not 0 
 */
static int namy_sem_lock(int semid, int semnum)
{
  struct sembuf sembuffer;
  sembuffer.sem_num = semnum; 
  sembuffer.sem_op = -1;  
  sembuffer.sem_flg = SEM_UNDO;  
  return semop(semid, &sembuffer, 1);
}

/**
 * セマフォアンロック 
 * @param semid semaphore id 
 * @param semnum semaphore number
 * @return 0 for success, otherwise not 0 
 */
static int namy_sem_unlock(int semid, int semnum)
{
  struct sembuf sembuffer;
  sembuffer.sem_num = semnum; 
  sembuffer.sem_op = 1;  
  sembuffer.sem_flg = SEM_UNDO;  
  return semop(semid, &sembuffer, 1);
}

/**
 * セマフォロック確認 
 * @param semid semaphore id 
 * @param semnum semaphore number
 * @return 0 is locked 
 */
static int namy_sem_is_locked(int semid, int semnum)
{
  return semctl(semid, semnum, GETVAL);
}

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

  namy_svr_cfg* entry = ap_get_module_config(r->server->module_config, &namy_pool_module);
  // 念のためチェック
  if (entry == NULL)
    return NULL;

  namy_dir_cfg* dir = (namy_dir_cfg*)apr_hash_get(entry->table, connection_pool_name, APR_HASH_KEY_STRING);
  // 存在しないコネクションキー
  if (dir == NULL)
  {
    return NULL;
  }

  //http://httpd.apache.org/docs/2.1/ja/mod/mod_proxy_balancer.html
  // for each worker in workers
  //     worker lbstatus += worker lbfactor
  //     total factor    += worker lbfactor
  //     if worker lbstatus > candidate lbstatus
  //     candidate = worker
  //
  // candidate lbstatus -= total factor
  namy_connection_cfg* con;
  int index, candidate=0, total=0;
  if (dir->servers > 1)
  {
    for (index=0; index<dir->servers; index++)
    {
      dir->bl->weight_status[index] += dir->bl->weight[index];
      total += dir->bl->weight[index];
      if (index==0 || dir->bl->weight_status[index] > dir->bl->weight_status[candidate])
        candidate = index;
    }
    dir->bl->weight_status[candidate] -= total;

    //DEBUGF("candidate: %d\n", candidate);
    index = 0;
    for (con=dir->next; con!=NULL; con=con->next)
    {
      if (index == candidate)
        break;
      index++;
      //DEBUGF("loop index: %d\n", index);
    }
    //DEBUGF("index: %d\n", index);
  }
  // １サーバーの場合はロードバランシングを走らせない
  else
  {
    con=dir->next;
  }
  
  int i = getpid()%con->connections;
  // 使用中なので記録
  if (con->is_locked(con->sem, con->table[i].id) == 0)
  {
    // ランダムで待機コネクションを選択
    // コネクションが増えてきたら、なにかした方がいいかも
    //i = rand()%svr->connections;
    
    // 統計情報作成
    if (con->lock(con->sem, con->connections) != 0)
    {
      TRACE("[mod_namy_pool]: lock failed for stat, sem:%d, id:%d", con->sem, con->connections);
      return NULL;
    }
    con->stat->conflicted++; // コネクション待ち発生
    if (con->unlock(con->sem, con->connections) != 0)
    {
      TRACE("[mod_namy_pool]: lock failed for stat, sem:%d, id:%d", con->sem, con->connections);
      return NULL;
    }
    DEBUG("[mod_namy_pool] %s: connection is too busy, wait = id:%d", connection_pool_name, con->table[i].id);
  }

  //　コネクションロック
  if(con->lock(con->sem, con->table[i].id) != 0)
  {
    TRACE("[mod_namy_pool]: conection lock failed, sem:%d, id:%d", con->sem, con->table[i].id);
    return NULL;
  }
  
  con->table[i].info->count++;
  con->table[i].info->pid = getpid();
  // 統計情報
  struct timeval t;
  gettimeofday(&t, NULL);
  con->table[i].info->start = (double)t.tv_sec + (double)t.tv_usec * 1e-6;;

  if (dir->servers == 1)
    return con->table[i].mysql;

  // 複数コネクションがある場合は、コネクションの生存確認
  // 死んでる場合は、weightテーブルを書き換える
  long now;
  now = time(NULL);
  if (con->stat->last_check_time + entry->interval < now)
  {
    if (mysql_ping(con->table[i].mysql) != 0)
    {
      dir->bl->failure_count++;
      if (dir->bl->failure_count > entry->allow_max_failure)
      {
        // fallback to next priority
        // weightを0にしてLBから外す
        dir->bl->weight_status[candidate] = 0;
        dir->bl->weight[candidate] = 0;
        dir->bl->priority[candidate] = -1;

        // priority をチェックしてweightを作る
        int j, max_priority = INT_MAX;
        namy_connection_cfg *tmp;
        // 一番高い優先度を探す
        for (j=0, tmp=dir->next; tmp!=NULL; tmp=tmp->next, j++)
        {
          // the smaller wins
          if (dir->bl->priority[j] != -1 && dir->bl->priority[j] < max_priority)
          {
            max_priority = dir->bl->priority[j];
          }
        }
        for (j=0, tmp=dir->next; tmp!=NULL; tmp=tmp->next, j++)
        {
          if (max_priority == dir->bl->priority[j])
            dir->bl->weight[j] = con->weight;
          else
            dir->bl->weight[j] = 0;
        }
        
        TRACE("[mod_namy_pool]: %s:%s conection failed fallback to next priority", con->server, con->db);

        if (entry->mail_to)
          sendmail(entry->sendmail, entry->mail_from, entry->mail_to, "connection failed", "connection failed");

      }
      //　コネクションアンロック
      if(con->unlock(con->sem, con->table[i].id) != 0)
      {
        TRACE("[mod_namy_pool]: conection unlock failed, sem:%d, id:%d", con->sem, con->table[i].id);
      }
      return NULL;
    }
    dir->bl->failure_count = 0;
  }
  con->stat->last_check_time = time(NULL);
  return con->table[i].mysql;
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

  namy_svr_cfg* entry = ap_get_module_config(r->server->module_config, &namy_pool_module);
  // 念のためチェック
  if (entry == NULL)
    return !NAMY_OK;

  apr_hash_index_t *hi;
  void *key, *val;
  // 各コネクションを取り出す
  for (hi = apr_hash_first(r->pool, entry->table); hi; hi = apr_hash_next(hi))
  {
    apr_hash_this(hi,(void*)&key, NULL, (void*)&val);
    // confで設定したコネクション情報取得
    char *con_name = (char*)key;
    namy_dir_cfg* dir = (namy_dir_cfg*)val;
    namy_connection_cfg* con;
    for (con=dir->next; con!=NULL; con=con->next)
    {
      int i, not_found = 1; 
      for (i = 0; i < con->connections; i++)
      {
        // 同一のコネクションかどうか
        if (con->table[i].mysql == mysql)
        {
          not_found = 0;
          break;
        }
      }

      // unknown connection 次のテーブルへ
      if (not_found)
      {
        continue;
      }

      // 解放
      con->table[i].info->pid = 0;
      // 統計情報
      struct timeval t;
      gettimeofday(&t, NULL);
      double end = (double)t.tv_sec + (double)t.tv_usec * 1e-6;;
      double diff = end - con->table[i].info->start;
      con->table[i].info->avg = (con->table[i].info->avg + diff)/2;
      if (con->table[i].info->max < diff)
        con->table[i].info->max = diff;

      DEBUG("[mod_namy_pool] %s: connection is detached, id:%d", con_name, con->table[i].id);

      //　コネクションアンロック
      if(con->unlock(con->sem, con->table[i].id) != 0)
      {
        TRACE("[mod_namy_pool]: conection unlock failed, sem:%d, id:%d", con->sem, con->table[i].id);
      }
      break;
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

  namy_svr_cfg* entry = ap_get_module_config(s->module_config, &namy_pool_module);
  if (entry == NULL)
    return;

  apr_hash_index_t *hi;
  // 念のためチェック
  void *key, *val;
  // 各コネクションを取り出す
  for (hi = apr_hash_first(NULL, entry->table); hi; hi = apr_hash_next(hi))
  {
    apr_hash_this(hi, (void*)&key, NULL, (void *)&val);
    char *con_name = (char*)key;
    
    // confで設定したコネクション情報取得
    namy_dir_cfg* dir = (namy_dir_cfg*)val;

    if (shmctl(dir->shm, 0, IPC_RMID) != 0)
    {
      TRACES("[mod_namy_pool] %s: dir->shm clean up error", con_name);
    }
    if (shmdt(dir->next->table[0].info) != 0)
    {
      TRACES("[mod_namy_pool] %s: dir->shm detach error", con_name);
    }

    namy_connection_cfg* con;
    for (con=dir->next; con!=NULL; con=con->next)
    {
      // コネクションクローズ 
      //if (shmdt(svr->sem) != 0)
      //{
      //  TRACES("[mod_namy_pool] %s: svr->sem detach error", con_name);
      //}
      if (semctl(con->sem, 0, IPC_RMID) != 0)
      {
        TRACES("[mod_namy_pool] %s: con->sem clean up error", con_name);
      }

      int i;
      for (i = 0; i < con->connections; i++)
      {
        //TRACES("[mod_namy_pool] %s: connection is closed, id:%d scramble:%s",
        //    con_name, con->table[i].id, con->table[i].mysql->scramble);
        mysql_close(con->table[i].mysql);
      }
      TRACES("[mod_namy_pool] %s: connection is closed, server:%s",
          con_name, con->server);
    }
  }
  apr_hash_clear(entry->table);
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

  namy_svr_cfg* entry = ap_get_module_config(r->server->module_config, &namy_pool_module);
  // 念のためチェック
  if (entry==NULL)
    return NAMY_UNKNOWN_CONNECTION;

  apr_hash_index_t *hi;
  void *key, *val;
  // 各コネクションを取り出す
  for (hi = apr_hash_first(NULL, entry->table); hi; hi = apr_hash_next(hi))
  {
    apr_hash_this(hi, (void*)&key, NULL, (void*)&val);
    // confで設定したコネクション情報取得
    namy_dir_cfg* dir = (namy_dir_cfg*)val;
    namy_connection_cfg* con;
    for (con=dir->next; con!=NULL; con=con->next)
    {
      // 全コネクションチェック  
      int i;
      for (i = 0; i < con->connections; i++)
      {
        if (con->table[i].mysql == mysql)
        {
          return NAMY_OK;
        }
      }
    }
  }
  return NAMY_UNKNOWN_CONNECTION;
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
  namy_svr_cfg* svr = apr_pcalloc(pool, sizeof(namy_svr_cfg));
  svr->table = apr_hash_make(pool);
  svr->interval = 300; // デフォルトは5分
  svr->allow_max_failure = 100;
  svr->sendmail = "/usr/sbin/sendmail";
  svr->mail_from = "root";
  svr->mail_to = NULL;
  return svr;
}

//
// 設定ファイルで作られた情報から、コネクションを作る
//
static int namy_pool_post_config(apr_pool_t *pconf, apr_pool_t *plog,
    apr_pool_t *ptemp, server_rec *s)
{
  namy_svr_cfg* entry = ap_get_module_config(s->module_config, &namy_pool_module);
  apr_hash_index_t *hi;
  void *key, *val;
  // 各コネクションを取り出す
  for (hi = apr_hash_first(pconf, entry->table); hi; hi = apr_hash_next(hi))
  {
    apr_hash_this(hi, (void*)&key, NULL, (void*)&val);
    char *con_name = (char*)key;

    namy_dir_cfg* dir = (namy_dir_cfg*)val;
    namy_connection_cfg *con;

    // info構造体 共有スペース確保
    // 使われた回数と利用中フラグを格納
    size_t total = 
      (sizeof(int)*dir->servers * 4) + // 4つのテーブル
      (sizeof(namy_cinfo)*dir->connections) +
      (sizeof(namy_stat)*dir->connections) + 
      (sizeof(balancer)*dir->servers);

    int segment = shmget(IPC_PRIVATE, total, S_IRUSR|S_IWUSR);  
    if (segment == -1)
    {
      TRACES("[mod_namy_pool] %s: namy_cinfo shmget error", con_name);
      return !OK;
    }
    dir->shm = segment;

    void *shm_addr = shmat(segment, NULL, 0);
    int offset = 0;

    // confで設定したコネクション情報取得
    for (con=dir->next; con!=NULL; con=con->next)
    {
      // セマフォ コネクション用排他処理
      // 統計情報用も含めて+1
      segment = semget(IPC_PRIVATE, con->connections + 1, S_IRUSR|S_IWUSR);
      if (segment == -1)  
      {
        TRACES("[mod_namy_pool] %s: semaphore semget error", con_name);
        return !OK;
      }
      con->sem = segment;

      // svr->table
      con->table = (namy_connection*)apr_palloc(pconf, sizeof(namy_connection)*con->connections);

      int i;
      for (i = 0; i < con->connections; i++)
      {
        // コネクション用セマフォ初期化
        if (semctl(con->sem, i, SETVAL, 1) != 0)
        {
          TRACES("[mod_namy_pool] %s: semaphore segment error", con_name);
          return !OK;
        }

        // 構造体作成
        con->table[i].id = i;
        con->table[i].info = (namy_cinfo*)(shm_addr + offset);
        con->table[i].info->count = 0;
        con->table[i].info->pid = 0;
        offset += sizeof(namy_cinfo);

        // mysql connect
        MYSQL* mysql = mysql_init(NULL);
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

        if (!mysql_real_connect(mysql,
            con->server, con->user,
            con->pw, con->db, con->port,
            con->socket, con->option))
        {
          TRACES("[mod_namy_pool] %s: connection to %s failed", con_name, con->server);
          return !OK;
        }
        // copy to pool
        con->table[i].mysql = mysql;

        //ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_NOTICE, 0, s,
        //    "[mod_namy_pool] %s: connected = id:%d scramble:%s", con_name, svr->table[i].id, svr->table[i].mysql->scramble);
      }
      // 統計情報のアドレス
      con->stat = (namy_stat*)(shm_addr + offset);
      offset += sizeof(namy_stat);
      con->stat->conflicted = 0;
      con->stat->last_check_time = time(NULL);
      // 関数登録
      con->lock = (void *)&namy_sem_lock;
      con->unlock = (void *)&namy_sem_unlock;
      con->is_locked = (void *)&namy_sem_is_locked;

      // shm　設定
      dir->bl = (balancer*)(shm_addr + offset);
      offset += sizeof(balancer);
      dir->bl->total_weight += con->weight;
      dir->bl->total_priority += con->priority;
      // 統計情報のセマフォ初期化 セマフォ番号は０から始まるから+1しない
      if (semctl(con->sem, con->connections, SETVAL, 1) != 0)
      {
        TRACES("[mod_namy_pool] %s: stat semaphore segment error", con_name);
        return !OK;
      }
      TRACES("[mod_namy_pool] %s: connected to %s with %d conections", con_name, con->server, con->connections);
    }

    // バランシングテーブル作成
    dir->bl->weight = (int *)(shm_addr + offset);
    offset += sizeof(int)*dir->servers;
    //DEBUGF("p1: %p\n", shm_addr + offset);
    dir->bl->weight_status = (int *)(shm_addr + offset);
    offset += sizeof(int)*dir->servers;
    //DEBUGF("p2: %p\n", shm_addr + offset);
    dir->bl->priority = (int *)(shm_addr + offset);
    offset += sizeof(int)*dir->servers;
    //DEBUGF("p3: %p\n", shm_addr + offset);
    dir->bl->active_status = (int *)(shm_addr + offset);
    offset += sizeof(int)*dir->servers;

    // priority をチェックしてweightを作る
    int i, max_priority = INT_MAX;
    // 一番高い優先度を探す
    for (i=0, con=dir->next; con!=NULL; con=con->next, i++)
    {
      // the smaller wins
      if (con->priority < max_priority)
      {
        max_priority = con->priority;
      }
      dir->bl->priority[i] = con->priority;
    }
    for (i=0, con=dir->next; con!=NULL; con=con->next, i++)
    {
      if (max_priority == dir->bl->priority[i])
        dir->bl->weight[i] = con->weight;
      else
        dir->bl->weight[i] = 0;
    }
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

  namy_svr_cfg* entry = ap_get_module_config(r->server->module_config, &namy_pool_module);
  apr_hash_index_t *hi;
  void *key, *val;
  // 各コネクションを取り出す
  for (hi = apr_hash_first(r->pool, entry->table); hi; hi = apr_hash_next(hi))
  {
    apr_hash_this(hi, (void*)&key, NULL, (void*)&val);
    // confで設定したコネクション情報取得
    char *con_name = (char*)key;
    namy_dir_cfg* dir = (namy_dir_cfg*)val;
    namy_connection_cfg* con= NULL;

    ap_rputs("<table border=\"1\"><tr><td>", r);
    // プール毎の情報
    ap_rprintf(r, "<tr><td><b>Connection Pool Name: <b>%s</td></tr>", con_name);
    ap_rprintf(r, "<tr><td><b>Servers: </b>%d</td></tr>", dir->servers);
  
    // バランサーテーブル
    ap_rputs("<tr><td><table border=\"1\">", r);
    int index;
    ap_rputs("<tr>", r);
    for (index=0; index<dir->servers; index++)
    {
      ap_rprintf(r, "<td>id:%d, weight:%d</td>", index, dir->bl->weight[index]);
    }
    ap_rputs("</tr>", r);
    ap_rputs("<tr>", r);
    for (index=0; index<dir->servers; index++)
    {
      ap_rprintf(r, "<td>%d</td>", dir->bl->weight_status[index]);
    }
    ap_rputs("</tr>", r);
    ap_rputs("</table></td></tr>", r);

    for (con=dir->next; con!=NULL; con=con->next)
    {
      ap_rputs("<tr><td><table border=\"1\">", r);
      ap_rprintf(r, "<tr><td colspan=\"4\"><b>Server Name: </b>%s</td><td colspan=\"4\"><b>port: </b>%d</td></tr>", con->server, con->port);
      ap_rprintf(r, "<tr><td colspan=\"4\"><b>User: </b>%s</td><td colspan=\"4\"><b>Database: </b>%s</td></tr>", con->user, con->db);
      ap_rprintf(r, "<tr><td colspan=\"4\"><b>SHM: </b>%d</td><td colspan=\"4\"><b>SEM: </b>%d</td></tr>", dir->shm, con->sem);
      ap_rprintf(r, "<tr><td colspan=\"4\"><b>Weight: </b>%d</td><td colspan=\"4\"><b>Priority: </b>%d</td></tr>", con->weight, con->priority);
      ap_rprintf(r, "<tr><td colspan=\"4\"><b>Conflict: </b>%ld</td><td colspan=\"4\"><b>Connections:</b> %d</td></tr>", con->stat->conflicted, con->connections);

      // コネクション毎の情報
      ap_rputs(
          "<td>connection id</td>"
          "<td>thread id in mysqld</td>"
          "<td>mysql scrable string</td>"
          "<td>count</td>"
          "<td>current user</td>"
          "<td>avg</td>"
          "<td>max</td>"
          "<td>sem locked</td>"
          "</tr>\n", r); 

      int i;
      for (i = 0; i < con->connections; i++)
      {
        ap_rprintf(r, 
            "<tr><td>%d</td>"
            "<td>%ld</td>"
            "<td>%s</td>"
            "<td>%ld</td>"
            "<td>%d</td>"
            "<td>%10.20f</td>"
            "<td>%10.20f</td>"
            "<td>%d</td></tr>\n",
            con->table[i].id,
            con->table[i].mysql->thread_id,
            ap_escape_html(r->pool, con->table[i].mysql->scramble),
            con->table[i].info->count,
            con->table[i].info->pid,
            con->table[i].info->avg,
            con->table[i].info->max,
            (con->is_locked(con->sem, con->table[i].id) == 0)? 1: 0
            );
      }

      ap_rputs("</table></td></tr>", r);
    }
    ap_rputs("</td></tr><table><br />", r);
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
//
// 
static const char *namy_section(cmd_parms *cmd, void *mconfig, const char *arg)
{
  const char *errmsg;
  const char *endp = ap_strrchr_c(arg, '>');
  const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
  char *old_path = cmd->path;
  int old_overrides = cmd->override;
  ap_conf_vector_t *new_dir_conf = ap_create_per_dir_config(cmd->pool);
  
  // シンタックスチェック
  if (err != NULL) {
    return err;
  }

  if (endp == NULL) {
    return apr_pstrcat(cmd->pool, cmd->cmd->name,
        "> directive missing closing '>'", NULL);
  }
 
 // プール名取得 
  arg=apr_pstrndup(cmd->pool, arg, endp-arg);
  if (!arg) {
    return "<NamyPool> block must specify a pool name";
  }

  // pool name should be unique
  namy_svr_cfg* entry = ap_get_module_config(cmd->server->module_config, &namy_pool_module);
  namy_dir_cfg *dir = (namy_dir_cfg*)apr_hash_get(entry->table, arg, APR_HASH_KEY_STRING);
  if (dir != NULL)
    return "found the same connection pool name, you can just use one time";

  dir = (namy_dir_cfg*)ap_set_config_vectors(cmd->server, new_dir_conf, cmd->path, &namy_pool_module, cmd->pool);
  apr_hash_set(entry->table, arg, APR_HASH_KEY_STRING, (namy_dir_cfg*)dir);
  
  // ディレクトリの中に
  cmd->path = (char*)arg;
  errmsg = ap_walk_config(cmd->directive->first_child, cmd, new_dir_conf);
  if (errmsg != NULL)
    return errmsg;

  if (dir->next == NULL)
    return "no PoolServer is provided, specify at least one server";

  // 戻ってくる
  cmd->path = old_path;
  cmd->override = old_overrides;

  return NULL;
}

// command list
typedef enum { cmd_ping_interval, cmd_max_failure, cmd_send_mail, cmd_mail_to, cmd_mail_from } cmd_parts;
static const char *set_option(cmd_parms *cmd, void *dbconf, const char* val)
{
  namy_svr_cfg* entry = ap_get_module_config(cmd->server->module_config, &namy_pool_module);
  switch ((long) cmd->info) {
    case cmd_ping_interval:
      ISINT(val);
      entry->interval = atoi(val);
      break;
    case cmd_max_failure:
      ISINT(val);
      entry->allow_max_failure = atoi(val);
      break;
    case cmd_send_mail:
      entry->sendmail = val;
      break;
    case cmd_mail_from:
      entry->mail_from = val;
      break;
    case cmd_mail_to:
      entry->mail_to = val;
      break;
  }
  return NULL;
}

//
//
//
static const char *add_server(cmd_parms *cmd, void *dummy, const char *db_string)
{
  namy_connection_cfg* con = apr_pcalloc(cmd->pool, sizeof(namy_connection_cfg));
  con->connections = 1; 
  con->weight = 1;

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

    if (!strcasecmp(name,"server"))
    {
      con->server = value;
    }    
    else if (!strcasecmp(name,"db"))
    {
      con->db = value;
    }
    else if (!strcasecmp(name,"user"))
    {
      con->user = value;
    }
    else if (!strcasecmp(name,"port"))
    {
      ISINT(value);
      con->port = atoi(value);
    }
    else if (!strcasecmp(name,"password"))
    {
      con->pw = value;
    }
    else if (!strcasecmp(name,"socket"))
    {
      con->socket = value;
    }
    else if (!strcasecmp(name,"option"))
    {
      ISINT(value);
      con->option = atoi(value);
    }
    else if (!strcasecmp(name,"connestion"))
    {
      ISINT(value);
      int connection = atoi(value);
      if (connection<1)
        return "connection should be greater than 0";
      con->connections = atoi(value);
    }
    else if (!strcasecmp(name,"weight"))
    {
      ISINT(value);
      int weight = atoi(value);
      if (weight<0)
        return "weight should be positive";
      con->weight = weight;
    }
    else if (!strcasecmp(name,"priority"))
    {
      ISINT(value);
      int priority = atoi(value);
      if (priority<0)
        return "priority should be positive";
      con->priority = atoi(value);
    }
    else
    {
      return db_string;
    }
    cpy_str = NULL;
  }
  con->sem = 0;

  // TRACE
  /*
  ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_NOTICE, 0, cmd->server,
      "[mod_namy_pool] server=%s, db=%s, user=%s, port=%d, passwd=%s, socket=%s, option=%d, connestion=%d, weight=%d",
      con->server,
      con->db,
      con->user,
      con->port,
      con->pw,
      con->socket,
      con->option,
      con->connections,
      con->weight
      );
   */

  namy_svr_cfg* entry = ap_get_module_config(cmd->server->module_config, &namy_pool_module);
  namy_dir_cfg *dir = (namy_dir_cfg*)apr_hash_get(entry->table, cmd->path, APR_HASH_KEY_STRING);
  namy_connection_cfg* tmp = dir->next;
  dir->next = con;
  con->next = tmp;
  dir->servers++;
  dir->connections += con->connections;

  return NULL;
}

//
//
//
static void *create_namy_pool_dir_config(apr_pool_t *p, char *dummy)
{
  namy_dir_cfg* new = (namy_dir_cfg*)apr_pcalloc(p, sizeof(namy_dir_cfg));
  new->name = NULL;
  new->servers = 0;  // 1プールに何台サーバーがあるか
  new->next = NULL;
  return (void *) new; 
}

//
// 設定ファイルのエントリー
//
static const command_rec namy_pool_cmds[] = {
  // プールセクション
  AP_INIT_RAW_ARGS("<NamyPool", namy_section, NULL, RSRC_CONF,
      "Container for directives affecting resources located in the proxied location"),
  // プールセクション内の設定
  AP_INIT_ITERATE("PoolServer", add_server, NULL, RSRC_CONF,
      "A server connection setting"),
  // mysql_pintインターバール
  AP_INIT_TAKE1("NamyPoolPingInterval", set_option, (void*)cmd_ping_interval, RSRC_CONF,
      "default mysql_ping interval in second: default 300 sec"),
  // mysql_ping失敗の許可回数 
  AP_INIT_TAKE1("NamyPoolMaxFailure", set_option, (void*)cmd_max_failure, RSRC_CONF,
      "the max number of ping failure before fallback: default 100 times"),
  // エラーメールのfrom
  AP_INIT_TAKE1("NamyPoolMailFrom", set_option, (void*)cmd_mail_from, RSRC_CONF,
      "error mail from"),
  // エラーメールのto
  AP_INIT_TAKE1("NamyPoolMailTo", set_option, (void*)cmd_mail_to, RSRC_CONF,
      "error mail to"),
  // メールプログラムのパス
  AP_INIT_TAKE1("NamyPoolSendMail", set_option, (void*)cmd_send_mail, RSRC_CONF,
      "sendmail path: default /usr/sbin/sendmail"),
  {NULL}
};

//
// モジュール構造隊
//
module AP_MODULE_DECLARE_DATA namy_pool_module = {
  STANDARD20_MODULE_STUFF,
  create_namy_pool_dir_config,
  NULL,
  create_namy_pool_config,
  NULL,
  namy_pool_cmds,
  namy_pool_hooks
};
