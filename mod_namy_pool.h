/* vim: set expandtab tabstop=2 shiftwidth=2 softtabstop=2 filetype=c: */
#ifndef _mod_namy_pool_h
#define _mod_namy_pool_h 

#include "httpd.h"
#include "mysql.h"
#include "apr_hash.h"

// コネクションの状態
// 書き込みが発生するので、shmに入れる
typedef struct {
  int in_use; // 使用中なら1, それ以外は0
  int num_of_used; // 使われた回数
} namy_cinfo;

// コネクション保存構造体
typedef struct _namy_connection {
  int id;
  MYSQL *mysql; // コネクション
  namy_cinfo *info; // コネクション状態
  struct _namy_connection *next; // リンクリスト
} namy_connection;

// サーバーセッティング
typedef struct {
  const char *server;
  const char *user;
  const char *pw;
  const char *db;
  const char *socket;
  int port;
  int option;
  int connections;
  int shm;
  int sem;
  namy_connection* next; // 全コネクションにアクセス
} namy_svr_cfg;

typedef struct {
  apr_hash_t *table;
} namy_svr_hash;

// ユーティリティー
#define NAMY_UNKNOWN_CONNECTION 0
#define NAMY_OK 1
MYSQL* namy_attach_pool_connection(request_rec *r, const char* connection_pool_name);
int    namy_detach_pool_connection(request_rec *r, MYSQL *mysql);
void   namy_close_pool_connection(server_rec *s);
int    Namy_is_pooled_connection(request_rec *r, MYSQL *mysql);
#endif
