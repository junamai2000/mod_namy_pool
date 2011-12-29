/* 
**  Then activate it in Apache's httpd.conf file for instance
**  for the URL /mod_namy_pool_test in as follows:
**
**    #   httpd.conf
**    LoadModule namy_pool_test_module modules/mod_namy_pool_test.so
**    <Location /print>
**    SetHandler mod_namy_pool_test 
**    </Location>
*/ 

#include "httpd.h"
#include "http_config.h"
#include "http_protocol.h"
#include "ap_config.h"

#include <mysql.h>

#include <sys/types.h>
#include <unistd.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>

#include "mod_namy_pool.h"

/* The sample content handler */
static int namy_pool_test_handler(request_rec *r)
{
	int count;
	MYSQL *mysql;
	MYSQL_RES *result;
	MYSQL_ROW record;

    if (strcmp(r->handler, "namy_pool_test")) {
        return DECLINED;
    }
    r->content_type = "text/html";      

    if (!r->header_only)
        ap_rputs("The sample page from mod_namy_pool_test.c\n", r);

	mysql = namy_attach_pool_connection(r, "test");
	if (mysql==NULL)
		return OK;

	mysql_query(mysql, "select * from test limit 100");
	/*
	if (getpid()%2)
	{
		if(mysql_query(mysql, "SELECT * FROM test order by name desc limit 1000"))
		{
			fprintf(stderr, "query error: %s\n",mysql_error(mysql));
			//return OK;
		}
	}
	else 
	{
		if(mysql_query(mysql, "SELECT * FROM test order by name asc limit 1000"))
		{
			fprintf(stderr, "query error: %s\n",mysql_error(mysql));
			//return OK;
		}
	}
	*/
	result = mysql_store_result(mysql);
	for(count = mysql_num_rows(result); count > 0; count--)
	{
		record = mysql_fetch_row(result);
		ap_rprintf(r, "PARENT:  Article no. %s ", record[0]);
		ap_rprintf(r, "PARENT: Title: %s\n", record[1]);
	}

	namy_detach_pool_connection(r, mysql);
    return OK;
}

static void namy_pool_test_register_hooks(apr_pool_t *p)
{
    ap_hook_handler(namy_pool_test_handler, NULL, NULL, APR_HOOK_MIDDLE);
}

/* Dispatch list for API hooks */
module AP_MODULE_DECLARE_DATA namy_pool_test_module = {
    STANDARD20_MODULE_STUFF, 
    NULL,                  /* create per-dir    config structures */
    NULL,                  /* merge  per-dir    config structures */
    NULL,                  /* create per-server config structures */
    NULL,                  /* merge  per-server config structures */
    NULL,                  /* table of config file commands       */
    namy_pool_test_register_hooks  /* register hooks                      */
};

