AUTOMAKE_OPTIONS=foreign
ACLOCAL_AMFLAGS=-I scripts

lib_LTLIBRARIES=libtest.la

libtest_la_SOURCES=test.c
libtest_la_LDFLAGS=-version-info 3:2:1

distclean-local:
	-rm -f mlibtool
