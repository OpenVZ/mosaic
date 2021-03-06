#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/sendfile.h>

#include "util.h"

int scan_mounts(char *mpath, char *device)
{
	FILE *f;
	char mi[1024];

	f = fopen("/proc/self/mountstats", "r");
	if (!f)
		return -1;

	while (fgets(mi, sizeof(mi), f)) {
		char *dev, *path, *e;

		dev = mi + sizeof("device");
		e = strchr(dev, ' ');
		*e = '\0';

		path = e + 1 + sizeof("mounted on");
		e = strchr(path, ' ');
		*e = '\0';

		if (strcmp(path, mpath))
			continue;

		strcpy(device, dev);
		fclose(f);
		return 0;
	}

	fclose(f);
	return -1;
}

int remove_rec(int dir_fd)
{
	DIR *d;
	struct dirent *de;
	int ret = -1;

	d = fdopendir(dir_fd);
	while ((de = readdir(d)) != NULL) {
		int flg = 0;

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;

		if (de->d_type == DT_DIR) {
			int sub_fd;

			/* FIXME -- limit of descriptors may not be enough */
			sub_fd = openat(dir_fd, de->d_name, O_DIRECTORY);
			if (sub_fd < 0) {
				loge("%s: can't open %s: %m\n",
						__func__, de->d_name);
				goto err;
			}

			if (remove_rec(sub_fd) < 0)
				goto err;

			flg = AT_REMOVEDIR;
		}

		if (unlinkat(dir_fd, de->d_name, flg)) {
			loge("%s: can't rm %s: %m\n",
					__func__, de->d_name);
			goto err;
		}
	};

	ret = 0;

err:
	closedir(d);
	return ret;
}

/** rmdirat_r -- recursively remove empty directories. Starts at
 * base/dirs and ens at base, or at the first non-empty directory.
 * All errors are reported, except for non-existent or non-empty directores.
 *
 * Example: rmdir(fd, "/tmp", "a/b/c");
 * should remove /tmp/a/b/c, /tmp/a/b, and /tmp/a if they are empty.
 */
int rmdirat_r(int basefd, const char *base, const char *dirs)
{
	char *slash;
	int ret;
	char *dir = strdup(dirs);

	while (1) {
		ret = unlinkat(basefd, dir, AT_REMOVEDIR);
		if (ret) {
			if (errno == ENOTEMPTY)
				// non-empty dir: return
				return 0;
			if (errno != ENOENT) {
				loge("%s: can't rmdir %s/%s: %m\n",
						__func__, base, dir);
				ret = -1;
				break;
			}
			// ENOENT is skipped
		}
		// get upper dir
		slash = strrchr(dir, '/');
		if (!slash) {
			ret = 0;
			break;
		}
		// skip repeating slashes
		while (slash[-1] == '/')
			slash--;
		*slash = '\0';
	}

	free(dir);
	return ret;
}

int get_subdir_size(int fd, unsigned long *sizep)
{
	/* FIXME: implement */
	return -1;
}

int copy_file(int src_dirfd, const char *src_dir,
		int dst_dirfd, const char *dst_dir,
		const char *name)
{
	int ifd, ofd;
	ssize_t ret, s;
	struct stat st;

	if (fstatat(src_dirfd, name, &st, 0) < 0) {
		loge("%s: can't stat %s/%s: %m\n",
				__func__, src_dir, name);
		return -1;
	}
	s = st.st_size;

	ifd = openat(src_dirfd, name, O_RDONLY);
	if (ifd < 0) {
		loge("%s: can't open %s/%s: %m\n",
				__func__, src_dir, name);
		return -1;
	}

	ofd = openat(dst_dirfd, name, O_RDWR | O_CREAT | O_EXCL, st.st_mode);
	if (ofd < 0) {
		loge("%s: can't create %s/%s: %m\n",
				__func__, dst_dir, name);
		close(ifd);
		return -1;
	}
	fchown(ofd, st.st_uid, st.st_gid);

	ret = sendfile(ofd, ifd, NULL, s);
	close(ifd);
	close(ofd);

	if (ret < 0) {
		loge("%s: sendfile %s/%s -> %s failed: %m\n",
				__func__, src_dir, name, dst_dir);
		return -1;
	} else if (ret != s) {
		loge("%s: sendfile %s/%s -> %s short write (%zd/%zd)\n",
				__func__, src_dir, name, dst_dir, ret, s);
		return -1;
	}

	return 0;
}

const ssize_t max_val_len = 1024;

int write_var(int dirfd, const char *dir,
		const char *name, const char *val)
{
	int fd, ret = 0;
	ssize_t len = 0;

	if (val) {
		len = strlen(val) + 1; // +1 for \0
		if (len > max_val_len) {
			loge("%s: variable %s too long "
					"(%zd > %zd)!\n",
					__func__, name, len, max_val_len);
			return -1;
		}
	}

	fd = openat(dirfd, name, O_WRONLY|O_CREAT);
	if (fd < 0) {
		loge("%s: can't open %s/%s: %m\n",
				__func__, dir, name);
		return -1;
	}

	if (len == 0) {
		if (ftruncate(fd, 0) < 0) {
			loge("%s: can't truncate %s/%s: %m\n",
					__func__, dir, name);
			ret = -1;
		}
	} else {
		if (write(fd, val, len) != len) {
			loge("%s: can't write %s/%s: %m\n",
					__func__, dir, name);
			ret = -1;
		}
	}

	if (close(fd) < 0) {
		loge("%s: can't close %s/%s: %m\n",
				__func__, dir, name);
		ret = -1;
	}

	return ret;
}

char *read_var(int dirfd, const char *dir, const char *name)
{
	int fd;
	struct stat st;
	char *buf = NULL;
	ssize_t s;

	if (fstatat(dirfd, name, &st, 0) < 0) {
		if (errno != ENOENT)
			loge("%s: can't stat %s/%s: %m\n",
					__func__, dir, name);
		return NULL;
	}
	s = st.st_size;
	if (s == 0)
		return NULL;

	fd = openat(dirfd, name, O_RDONLY);
	if (fd < 0) {
		loge("%s: can't open %s/%s: %m\n",
				__func__, dir, name);
		return NULL;
	}

	if (s > max_val_len) {
		loge("%s: file %s/%s size too large (%zd > %zd)\n",
				__func__, dir, name, s, max_val_len);
		goto out;
	}

	buf = malloc(s);
	if (!buf) {
		loge("%s: can't allocate %zdb memory: %m\n",
				__func__, s);
		goto out;
	}
	if (read(fd, &buf, s) != s) {
		loge("%s: can't read %s/%s: %m\n",
				__func__, dir, name);
		free(buf);
		buf = NULL;
	}

out:
	close(fd);
	return buf;
}

static void arg2str(char *const argv[], char *buf, int len)
{
	int i, r;
	char *sp = buf;
	char *ep = buf + len;

	for (i = 0; argv[i] != NULL; i++) {
		r = snprintf(sp, ep - sp, "%s ", argv[i]);
		if (r >= ep - sp)
			break;
		sp += r;
	}
}

/*
 * This is the same as libc-provided execvp (as far as I am able
 * to check with strace), the only difference is we have our own set
 * of directories to look a binary in, instead of of relying on PATH.
 *
 * We are a library and should not rely on PATH containing /sbin and
 * be useful in general.
 *
 * Alternative approach would be to modify PATH, but this is forbidden for
 * threaded processes (which we could have), or if search in PATH is
 * failed, do our own (which is very long and cumbersome).
 *
 * The code is written by me, taken from the ploop library:
 * https://github.com/kolyshkin/ploop/blob/master/lib/util.c
 */
static int my_execvp(const char *file, char *const argv[])
{
	char *const paths[] = { "/sbin", "/bin", "/usr/sbin", "/usr/bin",
		"/usr/local/sbin", "/usr/bin", NULL };
	int i, ret;

	if (file[0] == '/')
		return execv(file, argv);

	for (i = 0; paths[i] != NULL; i++) {
		char cmd[PATH_MAX];

		snprintf(cmd, sizeof(cmd), "%s/%s", paths[i], file);
		ret = execv(cmd, argv);
	}

	return ret;
}

int run_prg_rc(char *const argv[], int hide_mask, int *rc)
{
	int pid, ret, status;
	char cmd[512];
//	struct cleanup_hook *h;

	arg2str(argv, cmd, sizeof(cmd));
//	debug1("Running: %s", cmd);

	pid = fork();
	if (pid == 0) {
		int fd = open("/dev/null", O_RDONLY);
		if (fd >= 0) {
			dup2(fd, STDIN_FILENO);

			if (hide_mask & HIDE_STDOUT)
				 dup2(fd, STDOUT_FILENO);

			if (hide_mask & HIDE_STDERR)
				 dup2(fd, STDERR_FILENO);

			close(fd);
		} else {
			loge("%s: can't open /dev/null: %m\n",
					__func__);
			return -1;
		}

		my_execvp(argv[0], argv);

		loge("%s: can't exec %s: %m\n", __func__, argv[0]);
		return 127;
	} else if (pid == -1) {
		loge("%s: can't fork: %m\n", __func__);
		return -1;
	}
//	h = register_cleanup_hook(cleanup_kill_process, &pid);
	while ((ret = waitpid(pid, &status, 0)) == -1)
		if (errno != EINTR)
			break;
//	unregister_cleanup_hook(h);
	if (ret == -1) {
		loge("%s: can't waitpid %s: %m\n", __func__, cmd);
		return -1;
	} else if (WIFEXITED(status)) {
		ret = WEXITSTATUS(status);
		if (rc) {
			*rc = ret;
			return 0;
		}
		if (ret == 0)
			return 0;
		loge("%s: command %s exited with code %d\n",
				__func__, cmd, ret);
	} else if (WIFSIGNALED(status)) {
		loge("%s: command %s received signal %d\n",
				__func__, cmd, WTERMSIG(status));
	} else
		loge("%s: command %s died\n", __func__, cmd);

	return -1;
}

int run_prg(char *const argv[])
{
	return run_prg_rc(argv, 0, NULL);
}

/** Check if a given file or directory exists
 * Returns:
 *  1 - exists
 *  0 - does not exist
 * -1 - some unexpected system error (it is also reported)
 */
int path_exists(const char *path)
{
	if (access(path, F_OK) == 0)
		return 1;
	if (errno == ENOENT)
		return 0;

	loge("%s: can't access %s: %m\n", __func__, path);
	return -1;
}

int mkdir_p(const char *path, int use_last_component, int mode)
{
	char buf[PATH_MAX];
	const char *ps, *p;
	int len;

	if (path == NULL)
		return 0;

	ps = path + 1;
	while ((p = strchr(ps, '/'))) {
		len = p - path + 1;
		snprintf(buf, len, "%s", path);
		ps = p + 1;
		if (path_exists(buf) != 1) {
			if (mkdir(buf, mode)) {
				loge("%s: can't create directory"
						" %s: %m\n", __func__, buf);
				return -1;
			}
		}
	}

	if (!use_last_component)
		return 0;

	if (path_exists(path) != 1) {
		if (mkdir(path, mode)) {
			loge("%s: can't create directory %s: %m\n",
					__func__, path);
			return -1;
		}
	}

	return 0;
}

#ifdef DOTEST
int main(int argc, char **argv)
{
	int fd;

	fd = open(argv[1], O_DIRECTORY);
	if (fd < 0)
		return -1;

	return remove_rec(fd);
}
#endif
