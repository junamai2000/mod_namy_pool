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
  unsigned long count; // 使われた回数
  pid_t pid; //利用してるプロセス
  double start; // lockした時間
  double avg; // 平均lock時間
  double max; // 最大lock時間
} namy_cinfo;

// 統計情報
typedef struct {
  unsigned long  conflicted; // ロック待ち発生回数
  long last_check_time;
} namy_stat;

// コネクション保存構造体
typedef struct _namy_connection {
  int id;
  MYSQL *mysql; // コネクション
  namy_cinfo *info; // コネクション状態
} namy_connection;

// unlock,lock関数
typedef int (*util_func)(int semid, int semnum);

// サーバーセッティング
typedef struct _namy_connection_cfg {
  const char *server;
  const char *user;
  const char *pw;
  const char *db;
  const char *socket;
  int port;
  int option;
  int connections; // 接続するコネクション数
  int weight;
  int priority;
  int sem; // コネクションロック用セマフォ
  util_func lock; // ロック関数用ポインタ
  util_func unlock; // アンロック用関数ポインタ
  util_func is_locked; // ロック確認
  namy_stat *stat; // 統計情報
  namy_connection* table; // 全コネクションにアクセス
  struct _namy_connection_cfg *next; // 1 pool に複数サーバー
} namy_connection_cfg;

// バランシングテーブル
typedef struct {
  int *weight; // 重みテーブル
  int *weight_status;
  int *failure_count;
  int *priority; // 優先度テーブル 冗長構成用
} balancer;

// プール毎の設定
typedef struct {
  const char *name;
  int servers;  // プールに何台サーバーがあるか
  int connections; // プール全体のコネクション数
  int shm;
  balancer *bl;
  namy_connection_cfg *next; // リンクリスト
  namy_connection_cfg **pool; // 配列
} namy_dir_cfg;

// 全体の設定
typedef struct {
  apr_hash_t *table; // key->connection でnamy_svr_cfgを保存
  int interval; // コネクションチェックのインターバル
  int allow_max_failure;
  const char *mail_from;
  const char *mail_to;
  const char *sendmail;
} namy_svr_cfg;

// ユーティリティー
#define NAMY_UNKNOWN_CONNECTION 0
#define NAMY_OK 1
MYSQL* namy_attach_pool_connection(request_rec *r, const char* connection_pool_name);
int    namy_detach_pool_connection(request_rec *r, MYSQL *mysql);
void   namy_close_pool_connection(server_rec *s);
int    namy_is_pooled_connection(request_rec *r, MYSQL *mysql);
#endif
