/* Copyright (C) 2002 Timo Sirainen */

#include "common.h"
#include "hash.h"
#include "ioloop.h"
#include "network.h"
#include "fdpass.h"
#include "istream.h"
#include "env-util.h"
#include "write-full.h"
#include "master.h"
#include "client-common.h"

#include <unistd.h>

static int master_fd;
static struct io *io_master;
static struct hash_table *master_requests;
static unsigned int master_tag_counter;

static unsigned int master_pos;
static char master_buf[sizeof(struct master_login_reply)];

static void request_handle(struct master_login_reply *reply)
{
	struct client *client;
	master_callback_t *master_callback;

	client = hash_lookup(master_requests, POINTER_CAST(reply->tag));
	if (client == NULL)
		i_fatal("Master sent reply with unknown tag %u", reply->tag);

	master_callback = client->master_callback;
	client->master_tag = 0;
	client->master_callback = NULL;

	master_callback(client, reply->success);
	hash_remove(master_requests, POINTER_CAST(reply->tag));
	/* NOTE: client may be destroyed now */
}

void master_request_login(struct client *client, master_callback_t *callback,
			  unsigned int auth_pid, unsigned int auth_id)
{
	struct master_login_request req;

	i_assert(auth_pid != 0);

	memset(&req, 0, sizeof(req));
	req.version = MASTER_LOGIN_PROTOCOL_VERSION;
	req.tag = ++master_tag_counter;
	if (req.tag == 0)
		req.tag = ++master_tag_counter;
	req.auth_pid = auth_pid;
	req.auth_id = auth_id;
	req.local_ip = client->local_ip;
	req.remote_ip = client->ip;

	if (fd_send(master_fd, client->fd, &req, sizeof(req)) != sizeof(req))
		i_fatal("fd_send(%d) failed: %m", client->fd);

	client->master_tag = req.tag;
	client->master_callback = callback;

	hash_insert(master_requests, POINTER_CAST(req.tag), client);
}

void master_request_abort(struct client *client)
{
	hash_remove(master_requests, POINTER_CAST(client->master_tag));

	client->master_tag = 0;
	client->master_callback = NULL;
}

void master_notify_finished(void)
{
	struct master_login_request req;

	if (io_master == NULL)
		return;

	memset(&req, 0, sizeof(req));
	req.version = MASTER_LOGIN_PROTOCOL_VERSION;

	/* sending -1 as fd does the notification */
	if (fd_send(master_fd, -1, &req, sizeof(req)) != sizeof(req))
		i_fatal("fd_send(-1) failed: %m");
}

void master_close(void)
{
	if (io_master == NULL)
		return;

	if (close(master_fd) < 0)
		i_fatal("close(master) failed: %m");
	master_fd = -1;

	io_remove(io_master);
	io_master = NULL;

        main_close_listen();
	main_unref();

        /* may call this function again through main_unref() */
	clients_destroy_all();
}

static void master_exec(int fd)
{
	char *argv[] = { "dovecot", NULL };

	switch (fork()) {
	case -1:
		i_fatal("fork() failed: %m");
	case 0:
		if (dup2(fd, 0) < 0)
			i_fatal("master_exec: dup2(%d, 0) failed: %m", fd);
		(void)close(fd);

		if (setsid() < 0)
			i_fatal("setsid() failed: %m");

		env_put("DOVECOT_INETD=1");
		execv(SBINDIR"/dovecot", argv);
		i_fatal_status(FATAL_EXEC, "execv(%s) failed: %m",
			       SBINDIR"/dovecot");
	default:
		(void)close(fd);
	}
}

static void master_read_env(int fd)
{
	struct istream *input;
	const char *line;

	env_clean();

	/* read environment variable lines until empty line comes */
	input = i_stream_create_file(fd, default_pool, 8192, FALSE);
	do {
		switch (i_stream_read(input)) {
		case -1:
			i_fatal("EOF while reading environment from master");
		case -2:
			i_fatal("Too large environment line from master");
		}

		while ((line = i_stream_next_line(input)) != NULL &&
		       *line != '\0')
			env_put(line);
	} while (line == NULL);

	i_stream_unref(input);
}

int master_connect(const char *group_name)
{
	const char *path = PKG_RUNDIR"/master";
	int i, fd = -1;

	for (i = 0; i < 5 && fd == -1; i++) {
		fd = net_connect_unix(path);
		if (fd != -1)
			break;

		if (errno == ECONNREFUSED) {
			if (unlink(path) < 0)
				i_error("unlink(%s) failed: %m", path);
		} else if (errno != ENOENT) {
			i_fatal("Can't connect to master UNIX socket %s: %m",
				path);
		}

		/* need to create it */
		fd = net_listen_unix(path);
		if (fd != -1) {
			master_exec(fd);
			fd = -1;
		} else if (errno != EADDRINUSE) {
			i_fatal("Can't create master UNIX socket %s: %m", path);
		}
	}

	if (fd == -1)
		i_fatal("Couldn't use/create UNIX socket %s", path);

	if (group_name[0] == '\0')
		i_fatal("No login group name set");

	if (strlen(group_name) >= 256)
		i_fatal("Login group name too large: %s", group_name);

	/* group_name length is now guaranteed to be in range of 1..255 so we
	   can send <length byte><name> */
	group_name = t_strdup_printf("%c%s", (unsigned char)strlen(group_name),
				     group_name);
	if (write_full(fd, group_name, strlen(group_name)) < 0)
		i_fatal("write_full(master_fd) failed: %m");

	master_read_env(fd);
	return fd;
}

static void master_input(void *context __attr_unused__)
{
	int ret;

	ret = net_receive(master_fd, master_buf + master_pos,
			  sizeof(master_buf) - master_pos);
	if (ret < 0) {
		/* master died, kill all clients logging in */
		master_close();
		return;
	}

	master_pos += ret;
	if (master_pos < sizeof(master_buf))
		return;

	/* reply is now read */
	request_handle((struct master_login_reply *) master_buf);
	master_pos = 0;
}

void master_init(int fd, int notify)
{
	main_ref();

	master_fd = fd;
	master_requests = hash_create(default_pool, default_pool,
				      0, NULL, NULL);

        master_pos = 0;
	io_master = io_add(master_fd, IO_READ, master_input, NULL);

	if (notify) {
		/* just a note to master that we're ok. if we die before,
		   master should shutdown itself. */
		master_notify_finished();
	}
}

void master_deinit(void)
{
	hash_destroy(master_requests);

	if (io_master != NULL)
		io_remove(io_master);
}
