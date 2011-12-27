/* vim: set expandtab tabstop=2 shiftwidth=2 softtabstop=2 filetype=c: */
#ifndef _mod_namy_pool_h
#define _mod_namy_pool_h 

#include <sys/time.h>
#include <time.h>

#include "httpd.h"
#include "mysql.h"
#include "apr_hash.h"

// コネクションの状態
// 書き込みが発生するので、shmに入れる
typedef struct {
  int in_use; // 使用中なら1, それ以外は0
  unsigned long count; // 使われた回数
  pid_t pid; //利用してるプロセス
  double start; // lockした時間
  double avg; // 平均lock時間
  double max; // 最大lock時間
} namy_cinfo;

// 統計情報
typedef struct {
  unsigned long  conflicted; // ロック待ち
} namy_stat;

// コネクション保存構造体
typedef struct _namy_connection {
  int id;
  MYSQL *mysql; // コネクション
  namy_cinfo *info; // コネクション状態
  struct _namy_connection *next; // リンクリスト
} namy_connection;

// unlock,lock関数
typedef int (*util_func)(int semid, int semnum);

// サーバーセッティング
typedef struct {
  const char *server;
  const char *user;
  const char *pw;
  const char *db;
  const char *socket;
  int port;
  int option;
  int connections; // 接続するコネクション数
  int shm; // 共有メモリ用 (namy_statとか)
  int sem; // コネクションロック用セマフォ
  util_func lock; // ロック関数用ポインタ
  util_func unlock; // アンロック用関数ポインタ
  namy_stat *stat; // 統計情報
  namy_connection* next; // 全コネクションにアクセス
} namy_svr_cfg;

typedef struct {
  apr_hash_t *table; // key->connection でnamy_svr_cfgを保存
} namy_svr_hash;

// ユーティリティー
#define NAMY_UNKNOWN_CONNECTION 0
#define NAMY_OK 1
MYSQL* namy_attach_pool_connection(request_rec *r, const char* connection_pool_name);
int    namy_detach_pool_connection(request_rec *r, MYSQL *mysql);
void   namy_close_pool_connection(server_rec *s);
int    namy_is_pooled_connection(request_rec *r, MYSQL *mysql);
#endif
