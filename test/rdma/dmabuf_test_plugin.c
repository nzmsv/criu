/*
 * INTERNAL VALIDATION ONLY -- drop before upstream PR.
 *
 * Stand-in for libcuda on the udmabuf C/R gate.
 *
 * The DMA-BUF rebind contract splits across two parties. CRIU exports a
 * DMA-BUF-backed MR's buffer at dump and records one fd per line in
 * dmabuf_fds; at restore, whoever owns that memory recreates it and
 * rewrites the same lines with the fds it came back as, and CRIU's late
 * pass binds each MR to the fd at its recorded index. On a real GPU
 * checkpoint the second party is libcuda, reached through cuda-checkpoint.
 *
 * run_vfmig_dmabuf_mr_cr.sh has no GPU and no libcuda: its buffer is a
 * udmabuf over a sealed memfd, and nothing recreates it. This plugin is
 * that missing party, so the gate can exercise the bind path end to end.
 *
 * It runs on POST_RESUME_DEVICES, which fires in the criu master process
 * immediately before the bind pass -- so the fds it creates are already in
 * the fd table the bind ioctl resolves against, which is the namespace
 * dmabuf_fds is defined in.
 *
 * The buffer is fresh, not the one dumped: its contents are zero, not the
 * holder's pattern. That is deliberate and is not what the gate checks.
 * The holder verifies its pattern through the restored *memfd mapping*,
 * which CRIU restores as an ordinary VMA and which never went through the
 * MR. What binding to a fresh buffer proves is the half that was missing --
 * that an MR restored as an unbacked shell can be pointed at real memory
 * again, exactly as MR_BIND_DMABUF to a different buffer does.
 */
#define _GNU_SOURCE
#include "criu-log.h"
#include "plugin.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/udmabuf.h>

#define DMABUF_FDS_IMG "dmabuf_fds"
#define MAX_DMABUF_FDS 64
/* Matches uverbs_ctx_holder's g_mr_buf; override for other holders. */
#define DEFAULT_BUF_SIZE 8192

static int make_udmabuf(size_t len)
{
	struct udmabuf_create create = {};
	int memfd, udev, dmabuf_fd;

	memfd = memfd_create("vfmig-cr-rebind", MFD_ALLOW_SEALING | MFD_CLOEXEC);
	if (memfd < 0) {
		pr_perror("dmabuf test plugin: memfd_create");
		return -1;
	}
	if (ftruncate(memfd, len)) {
		pr_perror("dmabuf test plugin: ftruncate");
		close(memfd);
		return -1;
	}
	/* udmabuf refuses a memfd that can still shrink under it. */
	if (fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK)) {
		pr_perror("dmabuf test plugin: F_SEAL_SHRINK");
		close(memfd);
		return -1;
	}

	udev = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
	if (udev < 0) {
		pr_perror("dmabuf test plugin: open /dev/udmabuf (CONFIG_UDMABUF?)");
		close(memfd);
		return -1;
	}

	create.memfd = memfd;
	create.offset = 0;
	create.size = len;
	dmabuf_fd = ioctl(udev, UDMABUF_CREATE, &create);
	close(udev);
	close(memfd);
	if (dmabuf_fd < 0) {
		pr_perror("dmabuf test plugin: UDMABUF_CREATE");
		return -1;
	}
	return dmabuf_fd;
}

static int dmabuf_test_post_resume(int pid)
{
	char path[PATH_MAX];
	int dir_fd, n_lines = 0, i, ret = -1;
	int fds[MAX_DMABUF_FDS];
	size_t len;
	char line[32];
	FILE *f;

	dir_fd = criu_get_image_dir();
	if (dir_fd < 0)
		return -ENOTSUP;

	snprintf(path, sizeof(path), "/proc/%ld/fd/%d/" DMABUF_FDS_IMG, (long)getpid(), dir_fd);

	/*
	 * No file means this checkpoint had no DMA-BUF-backed objects, which
	 * is not this plugin's business -- let the chain carry on.
	 */
	f = fopen(path, "r");
	if (!f)
		return -ENOTSUP;

	while (fgets(line, sizeof(line), f)) {
		if (line[0] == '\n' || line[0] == '\0')
			continue;
		if (n_lines >= MAX_DMABUF_FDS) {
			pr_err("dmabuf test plugin: more than %d lines in %s\n", MAX_DMABUF_FDS, DMABUF_FDS_IMG);
			fclose(f);
			return -1;
		}
		n_lines++;
	}
	fclose(f);

	if (!n_lines)
		return -ENOTSUP;

	len = getenv("VFMIG_TEST_DMABUF_SIZE") ? strtoul(getenv("VFMIG_TEST_DMABUF_SIZE"), NULL, 0) :
						 DEFAULT_BUF_SIZE;

	for (i = 0; i < n_lines; i++) {
		fds[i] = make_udmabuf(len);
		if (fds[i] < 0) {
			while (--i >= 0)
				close(fds[i]);
			return -1;
		}
	}

	/*
	 * Rewrite in place: the bind pass keys each MR to its line number,
	 * so the order has to survive even though the numbers do not.
	 */
	f = fopen(path, "w");
	if (!f) {
		pr_perror("dmabuf test plugin: reopen %s for write", DMABUF_FDS_IMG);
		goto out_close;
	}
	for (i = 0; i < n_lines; i++) {
		if (fprintf(f, "%d\n", fds[i]) < 0) {
			pr_perror("dmabuf test plugin: write %s", DMABUF_FDS_IMG);
			fclose(f);
			goto out_close;
		}
	}
	if (fclose(f)) {
		pr_perror("dmabuf test plugin: close %s", DMABUF_FDS_IMG);
		goto out_close;
	}

	/*
	 * The fds stay open on purpose: the bind pass resolves them out of
	 * this process's table moments from now.
	 */
	pr_info("dmabuf test plugin: recreated %d udmabuf(s) of %zu bytes for pid %d\n", n_lines, len, pid);
	return 0;

out_close:
	for (i = 0; i < n_lines; i++)
		close(fds[i]);
	return ret;
}

CR_PLUGIN_REGISTER_DUMMY("dmabuf_test")
CR_PLUGIN_REGISTER_HOOK(CR_PLUGIN_HOOK__POST_RESUME_DEVICES, dmabuf_test_post_resume)
