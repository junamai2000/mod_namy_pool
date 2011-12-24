#include <sys/shm.h>
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

extern module AP_MODULE_DECLARE_DATA namy_pool_module;

MYSQL *namy_attach_pool_connection(namy_svr_cfg *svr)
{
	namy_connection *con = NULL;
	namy_connection *tmp = NULL;
	for (tmp = svr->connections; tmp!=NULL; )
	{
		if (tmp->info->in_use == 0)
		{
			con = tmp;
			//fprintf(fp, "namy_pool: attach con->id = %d\n", con->id);
			break;
		}
		tmp = tmp->next;
	}

	// 全部使用中なので最初のコネクションを待機
	if (con == NULL)
	{
		// ランダムで待機コネクションを洗濯
		int wait = rand()%svr->num_of_connections;
		int i=0;
		for (tmp = svr->connections; i<=wait||tmp!=NULL; i++)
		{
			con = tmp;
			tmp = tmp->next;
		}
		fprintf(stderr, "namy_pool connection busy wait = id:%d ramdom:%d\n", con->id, wait);
	}

	//　コネクションロック
	struct sembuf sembuffer;  
	sembuffer.sem_num = con->id;  
	sembuffer.sem_op = -1;  
	sembuffer.sem_flg = SEM_UNDO;  
	semop(con->shm, &sembuffer, 1);  
	// 使用中にする
	con->info->in_use = 1;
	con->info->num_of_used++;
	
	return con->mysql;	

}

int namy_detach_pool_connection(namy_svr_cfg *svr, MYSQL *mysql)
{
	// 空きコネクション取得
	namy_connection *tmp = NULL;
	namy_connection *con = NULL;
	for (tmp = svr->connections; tmp!=NULL; )
	{
		if(strncmp(tmp->mysql->scramble, mysql->scramble, SCRAMBLE_LENGTH) == 0)
		{
			con = tmp;
			break;
		}
		tmp = tmp->next;
	}

	// unknown connection
	if (con == NULL)
	{
		return NAMY_UNKNOWN_CONNECTION;
	}
	
	// 解放
	struct sembuf sembuffer;  
	con->info->in_use = 0;
	//fprintf(fp, "namy_pool: detached con->id = %d\n", con->id);

	sembuffer.sem_num = con->id;  
	sembuffer.sem_op = 1;  
	sembuffer.sem_flg = SEM_UNDO;  
	semop(con->shm, &sembuffer, 1);
	return NAMY_OK;
}

void namy_close_pool_connection(namy_svr_cfg *svr)
{
	namy_connection *tmp;
	shmctl(svr->shm, IPC_RMID, NULL);
	for (tmp = svr->connections; tmp!=NULL; )
	{
		semctl(tmp->shm, 0, IPC_RMID);
		fprintf(stderr, "namy_pool closed = id:%d scramble: %s\n", tmp->id, tmp->mysql->scramble);
		mysql_close(tmp->mysql);
		tmp = tmp->next;
	}
}

int namy_is_pooled_connection(namy_svr_cfg *svr, MYSQL *mysql)
{
	namy_connection *tmp = NULL;
	namy_connection *con = NULL;
	for (tmp = svr->connections; tmp!=NULL; )
	{
		if(strncmp(tmp->mysql->scramble, mysql->scramble, SCRAMBLE_LENGTH) == 0)
		{
			con = tmp;
			return NAMY_OK;
		}
		tmp = tmp->next;
	}
	return NAMY_UNKNOWN_CONNECTION;
}

/************ svr cfg: manage db namy pool ****************/
typedef enum { cmd_server, cmd_user, cmd_passwd, cmd_port,
               cmd_db, cmd_opt, cmd_cons, cmd_socket
} cmd_parts;

#define ISINT(val) do {                                                 \
        const char *p;                                                  \
                                                                        \
        for (p = val; *p; ++p) {                                        \
            if (!apr_isdigit(*p)) {                                     \
                return "Argument must be numeric!";                     \
            }                                                           \
        }                                                               \
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
		svr->num_of_connections = atoi(val);
        break;
    }
    return NULL;
}

static apr_status_t namy_pool_destroy(void *data)
{
	server_rec *s = data;
	namy_svr_cfg *svr = ap_get_module_config(s->module_config, &namy_pool_module);
	namy_close_pool_connection(svr);
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
	svr->num_of_connections = 1;
	svr->port = 0;
	svr->connections = NULL;
    return svr;
}

static int namy_pool_post_config(apr_pool_t *pconf, apr_pool_t *plog,
                           apr_pool_t *ptemp, server_rec *s)
{
	int segment;
	namy_svr_cfg *svr = ap_get_module_config(s->module_config, &namy_pool_module);
	segment = shmget(IPC_PRIVATE, sizeof(namy_cinfo)*svr->num_of_connections, S_IRUSR|S_IWUSR);  
	if (segment == -1)
	{
		fprintf(stderr, "shmget error\n");
		exit(1);
	}

	// 全プロセスで使う共有スペース
	namy_cinfo* alloc_info;
	// info構造体shmに作る
	alloc_info = (namy_cinfo*)shmat(segment, NULL, 0);
	svr->shm = segment;
	// コネクション用排他処理
	segment = semget(IPC_PRIVATE, svr->num_of_connections, S_IRUSR|S_IWUSR);
	if (segment == -1)  
	{
		fprintf(stderr, "semget error--\n");
		exit(1);
	}

	int i;
	for (i=0; i<svr->num_of_connections; i++)
	{
		namy_connection *con;
		int n=0;
		if ((n=semctl(segment, i, SETVAL, 1)) != 0)  
		{
			fprintf(stderr, "semget error: %d: %s\n", segment,strerror(n));
			exit(0);
		}

		con = (namy_connection*)apr_palloc(pconf, sizeof(namy_connection));
		con->id = i;
		con->shm = segment;
		con->info = alloc_info;
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
		alloc_info++;

		fprintf(stderr, "namy_pool connected = id:%d scramble:%s\n", con->id, con->mysql->scramble);

		if(svr->connections==NULL)
		{
			svr->connections = con;
		}
		else
		{
			namy_connection* tmp = svr->connections;
			svr->connections = con;
			con->next = tmp;
		}
	}
	srand((unsigned) time(NULL));
	apr_pool_cleanup_register(pconf, s, namy_pool_destroy, apr_pool_cleanup_null);
    return OK;
}

static int namy_pool_info_handler(request_rec *r)
{
	if (strcmp(r->handler, "namy_pool")) {
		return DECLINED;
	}   
	r->content_type = "text/html";    

	//if (!r->header_only)
	ap_rputs("<html><body>\n", r); 

	ap_rputs(namy_pool_module.name, r); 
	ap_rputs("<br />\n", r); 

	namy_svr_cfg *svr = ap_get_module_config(r->server->module_config, &namy_pool_module);
	namy_connection *tmp = NULL;
	ap_rputs("<table border=\"1\"><tr><td>connection id</td><td>mysql scrable string</td><td>shm number</td><td>number of connection used</td><td>is connection used?</td></tr>\n", r); 
	for (tmp = svr->connections; tmp!=NULL; )
	{
		ap_rprintf(r, "<tr><td>%d</td><td>%s</td><td>%d</td><td>%d</td><td>%d</td></tr>\n",
				tmp->id,
				ap_escape_html(r->pool, tmp->mysql->scramble),
				tmp->shm,
				tmp->info->num_of_used,
				tmp->info->in_use
				);
		tmp = tmp->next;
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

