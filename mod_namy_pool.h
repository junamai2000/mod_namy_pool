#ifndef _mod_namy_pool_h
#define _mod_namy_pool_h 

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <mysql.h>
#include <sys/shm.h>  
#include <sys/stat.h>  
#include <sys/types.h>  
#include <sys/wait.h>
#include <sys/ipc.h>  
#include <sys/sem.h>  
#include <string.h>
#include <errno.h>

// コネクションの状態
// 書き込みが発生するので、shmに入れる
typedef struct {
	int in_use; // 使用中なら1, それ以外は0
	int num_of_used; // 使われた回数
} namy_cinfo;

// コネクション保存構造体
typedef struct _namy_connection {
	int id;
	int shm; // shm番号 セマフォに利用
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
	namy_connection* next; // 全コネクションにアクセス
} namy_svr_cfg;

// ユーティリティー
#define NAMY_UNKNOWN_CONNECTION 0
#define NAMY_OK 1
MYSQL* namy_attach_pool_connection(server_rec *s);
int    namy_detach_pool_connection(server_rec *s, MYSQL *mysql);
void   namy_close_pool_connection(server_rec *s);
int    namy_is_pooled_connection(server_rec *s, MYSQL *mysql);
#endif
