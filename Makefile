builddir=.
top_srcdir=/etc/httpd
top_builddir=/usr/lib/httpd
include /usr/lib/httpd/build/special.mk

APXS=apxs
APACHECTL=apachectl

#DEFS=-Dmy_define=my_value
INCLUDES=-I/usr/local/include/mysql 
SH_LIBS=-L/usr/local/lib/mysql -lmysqlclient

all: local-shared-build

install: install-modules-yes

clean:
	-rm -f mod_my_pool.o mod_my_pool.lo mod_my_pool.slo mod_my_pool.la 
	-rm -f mod_my_pool_test.o mod_my_pool_test.lo mod_my_pool_test.slo mod_my_pool_test.la 

test: reload
	lynx -mime_header http://localhost/connections

reload: install restart

start:
	$(APACHECTL) start
restart:
	$(APACHECTL) restart
stop:
	$(APACHECTL) stop

