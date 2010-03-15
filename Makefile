##############################################################################
# Lua-RPC library, Copyright (C) 2001 Russell L. Smith. All rights reserved. #
#   Email: russ@q12.org   Web: www.q12.org                                   #
# For documentation, see http://www.q12.org/lua. For the license agreement,  #
# see the file LICENSE that comes with this distribution.                    #
##############################################################################
 
# the path to lua
LUAINC=/usr/include/lua5.1/
LUALIB=/usr/local/lib
 
LIBTOOL=libtool --tag=CC --quiet
UNAME := $(shell uname)

LIBRARY = rpc

# compiler, arguments and libs for GCC under unix
CFLAGS += -ansi -fpic -std=c99 -pedantic -g -DLUARPC_STANDALONE -DBUILD_RPC -Wall

OBJECTS = luarpc.o transport.o client.o server.o luagoodies.o luarpc_serial.o luarpc_socket.o serial_posix.o
# luarpc-client.o
# compiler, arguments and libs for GCC under windows
#CC=gcc -Wall
#CFLAGS=-DWIN32
#LIB=-llua -llualib -lwsock32 -lm

all: socket udp.so

##############################################################################
# don't change anything below this line
 
ifeq ($(UNAME), Linux)
LFLAGS = -O -shared -fpic
CFLAGS += -D_POSIX_C_SOURCE=199309L
endif
ifeq ($(UNAME), Darwin)
LFLAGS = -O -fpic -dynamiclib -undefined dynamic_lookup
endif

.SUFFIXES: .o .c

protocol.so: protocol.o
	gcc $(LFLAGS) -o $@ $<


udp.so: udp.o
	gcc $(LFLAGS) -o $@ $<

socket:
	CFLAGS=-DLUARPC_ENABLE_SOCKET $(MAKE) $(LIBRARY).so

serial:
	CFLAGS=-DLUARPC_ENABLE_SERIAL $(MAKE) $(LIBRARY).so
%.o : %.c $(DEPS)
	gcc $(CFLAGS) -I$(LUAINC) -o $@ -c $<

$(LIBRARY).so: $(OBJECTS)
	gcc $(LFLAGS) -o $(LIBRARY).so $(OBJECTS)

.PHONY : clean
clean:
	-rm -rf *~ *.o *.lo *.la *.obj a.out .libs core
