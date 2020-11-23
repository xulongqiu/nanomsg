############################################################################
# external/nanomsg/Makefile
#
#   Copyright (C) 2019 Xiaomi Inc. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in
#    the documentation and/or other materials provided with the
#    distribution.
# 3. Neither the name NuttX nor the names of its contributors may be
#    used to endorse or promote products derived from this software
#    without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
# FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
# COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
# INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
# BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
# OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
# AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
# LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
# ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.
#
############################################################################

include $(APPDIR)/Make.defs

ifeq ($(CONFIG_NANOMSG_DEMO),y)
PROGNAME  = $(CONFIG_NANOMSG_DEMO_PROGNAME)
PRIORITY  = $(CONFIG_NANOMSG_DEMO_PRIORITY)
STACKSIZE = $(CONFIG_NANOMSG_DEMO_STACKSIZE)
MODULE    = $(CONFIG_NANOMSG_DEMO)
endif

SOURCE_MAIN_PATH = src

#ALL_SUBDIR = $(shell find $(SOURCE_MAIN_PATH) -type d)
ALL_SUBDIR = src \
			 src/utils \
             src/utils \
             src/transports/utils \
             src/protocols/utils \
             src/core \
             src/aio \
             src/devices

CFLAGS +=  $(ALL_INCLUDE) -DNN_USE_PIPE -DNN_USE_EPOLL -DNN_HAVE_POLL -DNN_HAVE_MSG_CONTROL -DSCM_RIGHTS=1

ifeq ($(CONFIG_NANOMSG_PROTOCOL_PAIR),y)
ALL_SUBDIR += src/protocols/pair
CFLAGS += -DNN_PROTOCOL_PAIR
endif

ifeq ($(CONFIG_NANOMSG_PROTOCOL_REQREP),y)
ALL_SUBDIR += src/protocols/reqrep
CFLAGS += -DNN_PROTOCOL_REQREP
endif

ifeq ($(CONFIG_NANOMSG_PROTOCOL_PUBSUB),y)
ALL_SUBDIR += src/protocols/pubsub
CFLAGS += -DNN_PROTOCOL_PUBSUB
endif

ifeq ($(CONFIG_NANOMSG_PROTOCOL_PIPELINE),y)
ALL_SUBDIR += src/protocols/pipeline
CFLAGS += -DNN_PROTOCOL_PIPELINE
endif

ifeq ($(CONFIG_NANOMSG_PROTOCOL_BUS),y)
ALL_SUBDIR += src/protocols/bus
CFLAGS += -DNN_PROTOCOL_BUS
endif

ifeq ($(CONFIG_NANOMSG_PROTOCOL_SURVEY),y)
ALL_SUBDIR += src/protocols/survey
CFLAGS += -DNN_PROTOCOL_SURVEY
endif

ifeq ($(CONFIG_NANOMSG_TRANSPORT_INPROC),y)
ALL_SUBDIR += src/transports/inproc
CFLAGS += -DNN_TRANSPORT_INPROC
endif

ifeq ($(CONFIG_NANOMSG_TRANSPORT_IPC),y)
ALL_SUBDIR += src/transports/ipc
CFLAGS += -DNN_TRANSPORT_IPC
endif

ifeq ($(CONFIG_NANOMSG_TRANSPORT_TCP),y)
ALL_SUBDIR += src/transports/tcp
CFLAGS += -DNN_TRANSPORT_TCP
endif

ifeq ($(CONFIG_NANOMSG_TRANSPORT_WS),y)
ALL_SUBDIR += src/transports/ws
CFLAGS += -DNN_TRANSPORT_WS
endif

ALL_INCLUDE = $(foreach d, $(ALL_SUBDIR), -I$d)

CSRCS = $(shell find $(ALL_SUBDIR) -name "*.c" -maxdepth 1)


ifeq ($(CONFIG_NANOMSG_DEMO),y)
MAINSRC = demo/nuttx_media.c
endif

include $(APPDIR)/Application.mk
