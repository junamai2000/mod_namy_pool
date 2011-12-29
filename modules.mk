mod_namy_pool.la: mod_namy_pool.slo
	$(SH_LINK) -rpath $(libexecdir) -module -avoid-version  mod_namy_pool.lo
mod_namy_pool_test.la: mod_namy_pool_test.slo
	$(SH_LINK) -rpath $(libexecdir) -module -avoid-version  mod_namy_pool_test.lo
DISTCLEAN_TARGETS = modules.mk
shared =  mod_namy_pool.la mod_namy_pool_test.la
