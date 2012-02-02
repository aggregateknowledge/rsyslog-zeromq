pkglib_LTLIBRARIES = omzeromq.la

omzeromq_la_SOURCES = omzeromq.c
omzeromq_la_CPPFLAGS = -I$(top_srcdir) $(PTHREADS_CFLAGS) $(RSRT_CFLAGS) -I/usr/local/include
omzeromq_la_LDFLAGS = -module -avoid-version
omzeromq_la_LIBADD = -L/usr/local/lib -lzmq

EXTRA_DIST = 
