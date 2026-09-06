AM_CFLAGS = -pthread
AM_CXXFLAGS = -pthread
AM_CPPFLAGS = -I$(top_srcdir)/../../../src/ -I$(top_srcdir)/../../../src/libretro-common/include

#add this so that frontends can use "../types.h" for instance (they were built expecting to be in subdirectories of the main desmume dir
AM_CPPFLAGS += -I$(top_srcdir)/../../../src/frontend

#the MCP server is part of the core and available to every posix frontend
AM_CPPFLAGS += -DHAVE_MCP

AM_LDFLAGS = -pthread
