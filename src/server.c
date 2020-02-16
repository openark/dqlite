#include "server.h"

#include <stdlib.h>
#include <sys/un.h>

#include "../include/dqlite.h"
#include "conn.h"
#include "fsm.h"
#include "lib/assert.h"
#include "logger.h"
#include "replication.h"
#include "transport.h"
#include "vfs.h"

int dqlite__init(struct dqlite_node *d,
		 unsigned id,
		 const char *address,
		 const char *dir)
{
	int rv;
	rv = config__init(&d->config, id, address);
	if (rv != 0) {
		goto err;
	}
	rv = vfsInit(&d->vfs, &d->config);
	if (rv != 0) {
		goto err_after_config_init;
	}
	registry__init(&d->registry, &d->config);
	rv = uv_loop_init(&d->loop);
	if (rv != 0) {
		/* TODO: better error reporting */
		rv = DQLITE_ERROR;
		goto err_after_vfs_init;
	}
	rv = raftProxyInit(&d->raft_transport, &d->loop);
	if (rv != 0) {
		goto err_after_loop_init;
	}
	rv = raft_uv_init(&d->raft_io, &d->loop, dir, &d->raft_transport);
	if (rv != 0) {
		/* TODO: better error reporting */
		rv = DQLITE_ERROR;
		goto err_after_raft_transport_init;
	}
	rv = fsm__init(&d->raft_fsm, &d->config, &d->registry);
	if (rv != 0) {
		goto err_after_raft_io_init;
	}

	/* TODO: properly handle closing the dqlite server without running it */
	rv = raft_init(&d->raft, &d->raft_io, &d->raft_fsm, d->config.id,
		       d->config.address);
	if (rv != 0) {
		return rv;
	}
	/* TODO: expose these values through some API */
	raft_set_election_timeout(&d->raft, 3000);
	raft_set_heartbeat_timeout(&d->raft, 500);
	raft_set_snapshot_threshold(&d->raft, 1024);
	raft_set_snapshot_trailing(&d->raft, 8192);
	rv = replication__init(&d->replication, &d->config, &d->raft);
	if (rv != 0) {
		goto err_after_raft_fsm_init;
	}
	rv = sem_init(&d->ready, 0, 0);
	if (rv != 0) {
		/* TODO: better error reporting */
		rv = DQLITE_ERROR;
		goto err_after_raft_replication_init;
	}
	rv = sem_init(&d->stopped, 0, 0);
	if (rv != 0) {
		/* TODO: better error reporting */
		rv = DQLITE_ERROR;
		goto err_after_ready_init;
	}

	rv = pthread_mutex_init(&d->mutex, NULL);
	assert(rv == 0); /* Docs say that pthread_mutex_init can't fail */
	QUEUE__INIT(&d->queue);
	QUEUE__INIT(&d->conns);
	d->running = false;
	d->listener = NULL;
	d->bind_address = NULL;
	return 0;

err_after_ready_init:
	sem_destroy(&d->ready);
err_after_raft_replication_init:
	replication__close(&d->replication);
err_after_raft_fsm_init:
	fsm__close(&d->raft_fsm);
err_after_raft_io_init:
	raft_uv_close(&d->raft_io, NULL);
err_after_raft_transport_init:
	raftProxyClose(&d->raft_transport);
err_after_loop_init:
	uv_loop_close(&d->loop);
err_after_vfs_init:
	vfsClose(&d->vfs);
err_after_config_init:
	config__close(&d->config);
err:
	return rv;
}

void dqlite__close(struct dqlite_node *d)
{
	int rv;
	raft_free(d->listener);
	rv = pthread_mutex_destroy(&d->mutex); /* This is a no-op on Linux . */
	assert(rv == 0);
	rv = sem_destroy(&d->stopped);
	assert(rv == 0); /* Fails only if sem object is not valid */
	rv = sem_destroy(&d->ready);
	assert(rv == 0); /* Fails only if sem object is not valid */
	replication__close(&d->replication);
	fsm__close(&d->raft_fsm);
	uv_loop_close(&d->loop);
	raftProxyClose(&d->raft_transport);
	registry__close(&d->registry);
	vfsClose(&d->vfs);
	config__close(&d->config);
	if (d->bind_address != NULL) {
		sqlite3_free(d->bind_address);
	}
}

int dqlite_node_create(unsigned server_id,
		       const char *server_address,
		       const char *data_dir,
		       dqlite_node **t)
{
	int rv;

	*t = sqlite3_malloc(sizeof **t);

	rv = dqlite__init(*t, server_id, server_address, data_dir);
	if (rv != 0) {
		return rv;
	}

	return 0;
}

static int ipParse(const char *address, struct sockaddr_in *addr)
{
	char buf[256];
	char *host;
	char *port;
	char *colon = ":";
	int rv;

	/* TODO: turn this poor man parsing into proper one */
	strcpy(buf, address);
	host = strtok(buf, colon);
	port = strtok(NULL, ":");
	if (port == NULL) {
		port = "8080";
	}

	rv = uv_ip4_addr(host, atoi(port), addr);
	if (rv != 0) {
		return RAFT_NOCONNECTION;
	}

	return 0;
}

int dqlite_node_set_bind_address(dqlite_node *t, const char *address)
{
	struct sockaddr_un addr_un;
	struct sockaddr_in addr_in;
	struct sockaddr *addr;
	size_t len;
	int fd;
	int rv;
	int domain = address[0] == '@' ? AF_UNIX : AF_INET;
	if (t->running) {
		return DQLITE_MISUSE;
	}
	if (domain == AF_INET) {
		memset(&addr_in, 0, sizeof addr_in);
		rv = ipParse(address, &addr_in);
		if (rv != 0) {
			return DQLITE_MISUSE;
		}
		len = sizeof addr_in;
		addr = (struct sockaddr *)&addr_in;
	} else {
		memset(&addr_un, 0, sizeof addr_un);
		addr_un.sun_family = AF_UNIX;
		len = strlen(address);
		if (len == 1) {
			/* Auto bind */
			len = 0;
		} else {
			strcpy(addr_un.sun_path + 1, address + 1);
		}
		len += sizeof(sa_family_t);
		addr = (struct sockaddr *)&addr_un;
	}
	fd = socket(domain, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd == -1) {
		return DQLITE_ERROR;
	}

	rv = bind(fd, addr, len);
	if (rv != 0) {
		close(fd);
		return DQLITE_ERROR;
	}

	rv = transport__stream(&t->loop, fd, &t->listener);
	if (rv != 0) {
		close(fd);
		return DQLITE_ERROR;
	}

	if (domain == AF_INET) {
		t->bind_address = sqlite3_malloc(strlen(address));
		if (t->bind_address == NULL) {
			/* TODO: cleanup */
			return DQLITE_NOMEM;
		}
		strcpy(t->bind_address, address);
	} else {
		len = sizeof addr_un.sun_path;
		t->bind_address = sqlite3_malloc(len);
		if (t->bind_address == NULL) {
			/* TODO: cleanup */
			return DQLITE_NOMEM;
		}
		memset(t->bind_address, 0, len);
		rv = uv_pipe_getsockname((struct uv_pipe_s *)t->listener,
					 t->bind_address, &len);
		if (rv != 0) {
			/* TODO: cleanup */
			return DQLITE_ERROR;
		}
		t->bind_address[0] = '@';
	}

	return 0;
}

const char *dqlite_node_get_bind_address(dqlite_node *t)
{
	return t->bind_address;
}

int dqlite_node_set_connect_func(dqlite_node *t,
				 int (*f)(void *arg,
					  const char *address,
					  int *fd),
				 void *arg)
{
	if (t->running) {
		return DQLITE_MISUSE;
	}
	raftProxySetConnectFunc(&t->raft_transport, f, arg);
	return 0;
}

int dqlite_node_set_network_latency(dqlite_node *t,
				    unsigned long long nanoseconds)
{
	unsigned milliseconds;
	if (t->running) {
		return DQLITE_MISUSE;
	}
	/* Currently we accept at most 500 microseconds latency. */
	if (nanoseconds < 500 * 1000) {
		return DQLITE_MISUSE;
	}
	milliseconds = nanoseconds / (1000 * 1000);
	raft_set_heartbeat_timeout(&t->raft, (milliseconds * 15) / 10);
	raft_set_election_timeout(&t->raft, milliseconds * 15);
	return 0;
}

static int maybeBootstrap(dqlite_node *d,
			  const unsigned id,
			  const char *address)
{
	struct raft_configuration configuration;
	int rv;
	if (id != 1) {
		return 0;
	}
	raft_configuration_init(&configuration);
	rv = raft_configuration_add(&configuration, id, address, true);
	if (rv != 0) {
		assert(rv == RAFT_NOMEM);
		rv = DQLITE_NOMEM;
		goto out;
	};
	rv = raft_bootstrap(&d->raft, &configuration);
	if (rv != 0) {
		if (rv == RAFT_CANTBOOTSTRAP) {
			rv = 0;
		} else {
			rv = DQLITE_ERROR;
		}
		goto out;
	}
out:
	raft_configuration_close(&configuration);
	return rv;
}

/* Callback invoked when the stop async handle gets fired.
 *
 * This callback will walk through all active handles and close them. After the
 * last handle (which must be the 'stop' async handle) is closed, the loop gets
 * stopped.
 */
static void raftCloseCb(struct raft *raft)
{
	struct dqlite_node *s = raft->data;
	raft_uv_close(&s->raft_io, NULL);
	uv_close((struct uv_handle_s *)&s->stop, NULL);
	uv_close((struct uv_handle_s *)&s->startup, NULL);
	uv_close((struct uv_handle_s *)s->listener, NULL);
}

static void destroy_conn(struct conn *conn)
{
	QUEUE__REMOVE(&conn->queue);
	sqlite3_free(conn);
}

static void stop_cb(uv_async_t *stop)
{
	struct dqlite_node *d = stop->data;
	queue *head;
	struct conn *conn;

	/* We expect that we're being executed after dqlite__stop and so the
	 * running flag is off. */
	assert(!d->running);

	QUEUE__FOREACH(head, &d->conns)
	{
		conn = QUEUE__DATA(head, struct conn, queue);
		conn__stop(conn);
	}
	raft_close(&d->raft, raftCloseCb);
}

/* Callback invoked as soon as the loop as started.
 *
 * It unblocks the s->ready semaphore.
 */
static void startup_cb(uv_timer_t *startup)
{
	struct dqlite_node *d = startup->data;
	int rv;
	d->running = true;
	rv = sem_post(&d->ready);
	assert(rv == 0); /* No reason for which posting should fail */
}

static void listenCb(uv_stream_t *listener, int status)
{
	struct dqlite_node *t = listener->data;
	struct uv_stream_s *stream;
	struct conn *conn;
	int rv;

	if (status != 0) {
		/* TODO: log the error. */
		return;
	}

	switch (listener->type) {
		case UV_TCP:
			stream = raft_malloc(sizeof(struct uv_tcp_s));
			if (stream == NULL) {
				return;
			}
			rv = uv_tcp_init(&t->loop, (struct uv_tcp_s *)stream);
			assert(rv == 0);
			break;
		case UV_NAMED_PIPE:
			stream = raft_malloc(sizeof(struct uv_pipe_s));
			if (stream == NULL) {
				return;
			}
			rv = uv_pipe_init(&t->loop, (struct uv_pipe_s *)stream,
					  0);
			assert(rv == 0);
			break;
		default:
			assert(0);
	}

	rv = uv_accept(listener, stream);
	if (rv != 0) {
		goto err;
	}

	/* We accept unix socket connections only from the same process. */
	if (listener->type == UV_NAMED_PIPE) {
		struct ucred cred;
		socklen_t len;
		int fd;
		fd = stream->io_watcher.fd;
		len = sizeof cred;
		rv = getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len);
		if (rv != 0) {
			goto err;
		}
		if (cred.pid != getpid()) {
			goto err;
		}
	}

	conn = sqlite3_malloc(sizeof *conn);
	if (conn == NULL) {
		goto err;
	}
	rv = conn__start(conn, &t->config, &t->loop, &t->registry, &t->raft,
			 stream, &t->raft_transport, destroy_conn);
	if (rv != 0) {
		goto err_after_conn_alloc;
	}

	QUEUE__PUSH(&t->conns, &conn->queue);

	return;

err_after_conn_alloc:
	sqlite3_free(conn);
err:
	uv_close((struct uv_handle_s *)stream, (uv_close_cb)raft_free);
}

static int taskRun(struct dqlite_node *d)
{
	int rv;

	/* TODO: implement proper cleanup upon error by spinning the loop a few
	 * times. */
	assert(d->listener != NULL);

	rv = uv_listen(d->listener, 128, listenCb);
	if (rv != 0) {
		return rv;
	}
	d->listener->data = d;

	/* Initialize notification handles. */
	d->stop.data = d;
	rv = uv_async_init(&d->loop, &d->stop, stop_cb);
	assert(rv == 0);

	/* Schedule startup_cb to be fired as soon as the loop starts. It will
	 * unblock clients of taskReady. */
	d->startup.data = d;
	rv = uv_timer_init(&d->loop, &d->startup);
	assert(rv == 0);
	rv = uv_timer_start(&d->startup, startup_cb, 0, 0);
	assert(rv == 0);

	d->raft.data = d;
	rv = raft_start(&d->raft);
	if (rv != 0) {
		/* Unblock any client of taskReady */
		sem_post(&d->ready);
		return rv;
	}

	rv = uv_run(&d->loop, UV_RUN_DEFAULT);
	assert(rv == 0);

	/* Unblock any client of taskReady */
	rv = sem_post(&d->ready);
	assert(rv == 0); /* no reason for which posting should fail */

	return 0;
}

static void *taskStart(void *arg)
{
	struct dqlite_node *t = arg;
	int rv;
	rv = taskRun(t);
	if (rv != 0) {
		uintptr_t result = rv;
		return (void *)result;
	}
	return NULL;
}

void dqlite_node_destroy(dqlite_node *d)
{
	dqlite__close(d);
	sqlite3_free(d);
}

/* Wait until a dqlite server is ready and can handle connections.
**
** Returns true if the server has been successfully started, false otherwise.
**
** This is a thread-safe API, but must be invoked before any call to
** dqlite_stop or dqlite_handle.
*/
static bool taskReady(struct dqlite_node *d)
{
	/* Wait for the ready semaphore */
	sem_wait(&d->ready);
	return d->running;
}

int dqlite_node_start(dqlite_node *t)
{
	int rv;

	rv = maybeBootstrap(t, t->config.id, t->config.address);
	if (rv != 0) {
		goto err;
	}

	rv = pthread_create(&t->thread, 0, &taskStart, t);
	if (rv != 0) {
		goto err;
	}

	if (!taskReady(t)) {
		rv = DQLITE_ERROR;
		goto err;
	}

	return 0;

err:
	return rv;
}

int dqlite_node_stop(dqlite_node *d)
{
	void *result;
	int rv;

	/* Grab the queue mutex, so we can be sure no new incoming request will
	 * be enqueued from this point on. */
	pthread_mutex_lock(&d->mutex);

	/* Turn off the running flag, so calls to dqlite_handle will fail
	 * with DQLITE_STOPPED. This needs to happen before we send the stop
	 * signal since the stop callback expects to see that the flag is
	 * off. */
	d->running = false;

	rv = uv_async_send(&d->stop);
	assert(rv == 0);

	pthread_mutex_unlock(&d->mutex);

	rv = pthread_join(d->thread, &result);
	assert(rv == 0);

	return (uintptr_t)result;
}

int dqlite_node_recover(dqlite_node *n,
			struct dqlite_node_info infos[],
			int n_info)
{
	struct raft_configuration configuration;
	int i;
	int rv;

	raft_configuration_init(&configuration);
	for (i = 0; i < n_info; i++) {
		struct dqlite_node_info *info = &infos[i];
		rv = raft_configuration_add(&configuration, info->id,
					    info->address, true);
		if (rv != 0) {
			assert(rv == RAFT_NOMEM);
			rv = DQLITE_NOMEM;
			goto out;
		};
	}

	rv = raft_recover(&n->raft, &configuration);
	if (rv != 0) {
		rv = DQLITE_ERROR;
		goto out;
	}

out:
	raft_configuration_close(&configuration);
	return rv;
}
