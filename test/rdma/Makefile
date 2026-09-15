# INTERNAL VALIDATION ONLY -- drop before upstream PR.

CFLAGS  ?= -Wall -Wextra -O2 -g

all: uverbs_ctx_holder vfmig_barrier_peer dmabuf_test_plugin.so

uverbs_ctx_holder: LDLIBS := -libverbs
uverbs_ctx_holder: uverbs_ctx_holder.c

# Standalone barrier peer stand-in (wire-only; no libibverbs).
vfmig_barrier_peer: vfmig_barrier_peer.c

# Stand-in for libcuda on the udmabuf C/R gate: recreates the buffers and
# rewrites dmabuf_fds so the late bind pass has something to bind to.
# Needs criu's plugin headers, hence the reach back into the tree.
CRIU_TOP := $(CURDIR)/../..
dmabuf_test_plugin.so: dmabuf_test_plugin.c
	$(CC) $(CFLAGS) -shared -fPIC -iquote $(CRIU_TOP) -iquote $(CRIU_TOP)/criu/include \
		-iquote $(CRIU_TOP)/include -iquote $(CRIU_TOP)/images \
		-o $@ $<

clean:
	$(RM) uverbs_ctx_holder vfmig_barrier_peer dmabuf_test_plugin.so

.PHONY: all clean
