rsyslog-zeromq
================================================================

ZeroMQ input and output modules for rsyslog.

This module allows rsyslog to read and write ZeroMQ messages.

# Installing rsyslog-zeromq

Tested with rsyslog-5.8.0.

0. Install zeromq, will need headers and devel libraries ...

1. Apply the patch 'rsyslog-zeromq.patch' to the source of rsyslog:

        cd path/to/rsyslog-5.8.0
        patch -p1 -i rsyslog-zeromq.patch

2. Copy imzeromq and omzeromq directories to plugins directory of
   rsyslog source.

        rsync -av {i,o}mzeromq path/to/rsyslog-5.8.0/plugins

3. Regenerate autotools related files:

        cd path/to/rsyslog-5.8.0
        autoreconf

4. Add '--enable-imzeromq' and '--enable-omzeromq' to ./configure
   switches.

        ./configure \
        --enable-imzeromq \
        --enable-omzeromq \
        <your-other-flags-here>

5. Build and install.

        cd path/to/rsyslog-5.8.0
        make
        make install

# Configuring the output module (omzeromq)

The :omzeromq: selector takes the following parameter components:

* connect=<endpoint>    Connect to the specified endpoint.
* bind=<endpoint>       Bind to the specified endpoint.
* identity=<identstr>   Sets the identity of the socket.
* hwm=<NNN>             Sets the high water mark of the socket.
* swap=<NNN>            Sets the swap value for the socket.
* threads=<N>           Sets the number of zeromq context threads.

The format for the selector may be specified in the standard way with
a trailing ";<FORMAT>" specifier.

Examples:

        $ModLoad omzeromq.so
        
        # Minimal configuration:
        *.* :omzeromq:bind=tcp://*:5557
        
        # Full config, all parameters specified:
        *.* :omzeromq:bind=tcp://*:5557,hwm=1000,swap=9999,identity=foobar,threads=1;RSYSLOG_ForwardFormat

# Configuring the input module (imzeromq)

The $InputZeroMQServerRun directive takes the following parameter components:

* connect=<endpoint>    Connect to the specified endpoint.
* bind=<endpoint>       Bind to the specified endpoint.
* identity=<identstr>   Sets the identity of the socket.

Examples:

        $ModLoad imzeromq.so
        
        $RuleSet Events
        
        ... ruleset definition here ...
        
        # Provides ZeroMQ reception
        $InputZeroMQServerBindRuleset Events
        $InputZeroMQServerRun       connect=tcp://*:5557,identity=yoyo
