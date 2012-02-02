pkglib_LTLIBRARIES = imzeromq.la

imzeromq_la_SOURCES = imzeromq.c
imzeromq_la_CPPFLAGS = -I$(top_srcdir) $(PTHREADS_CFLAGS) $(RSRT_CFLAGS) -I/usr/local/include
imzeromq_la_LDFLAGS = -module -avoid-version
imzeromq_la_LIBADD = -L/usr/local/lib -lzmq
