#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_compat.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/priv.h>
#include <sys/tree.h>
#include <sys/queue.h>
#include <sys/dtrace.h>

#include <machine/bus.h>

#include "vtdtr.h"

#define BITS                8
#define MAX_BITSHIFT        ((size_t)1 << (sizeof(size_t)*BITS-1))
#define VTDTR_BITMASK       ((MAX_BITSHIFT - 1) | (MAX_BITSHIFT))
#define VTDTR_DEFAULT_SIZE  ((size_t)VTDTR_BITMASK)
#define VTDTR_ALL_EVENTS    ((size_t)VTDTR_BITMASK)
#define VTDTR_MTX_NAME_SIZE 64

/*
 * Lets us implement the linked list.
 */
struct vtdtr_qentry {
	struct vtdtr_event        *event;
	STAILQ_ENTRY(vtdtr_qentry) next;
};

/*
 * The queue is kept on a per-ucred basis. We do not want to deal with race
 * conditions in the kernel and instead leave it up to the user to handle in
 * case of more complex situations.
 */
struct vtdtr_queue {
	struct mtx                  mtx;         /* Queue mutex */
	struct proc                *proc;        /* Queue for a given proc */
	RB_ENTRY(vtdtr_queue)       qnode;       /* This is a tree node */
	STAILQ_HEAD(, vtdtr_qentry) head;        /* Head of the queue */
	size_t                      max_size;    /* Max events we can hold */
	size_t                      num_entries; /* Events in queue */
	size_t                      event_flags; /* Configuration flags */
	size_t                      drops;       /* Number of event drops */
};

static struct vtdtr_qentry *vtdtr_construct_entry(struct vtdtr_event *);
static int vtdtr_subscribed(struct vtdtr_queue *, struct vtdtr_event *);
static int vtdtr_read(struct cdev *, struct uio *, int);
static int vtdtr_ioctl(struct cdev *, u_long, caddr_t, int, struct thread *);
static int vtdtr_open(struct cdev *, int, int, struct thread *);
static int vtdtr_close(struct cdev *, int, int, struct thread *);
static int vtdtr_modevent(module_t, int, void *);
static int qtreecmp(struct vtdtr_queue *, struct vtdtr_queue *);

/*
 * We keep the queues as a red-black tree.
 */
static struct mtx qtree_mtx;
static RB_HEAD(vtdtr_qtree, vtdtr_queue) vtdtr_queue_tree =
    RB_INITIALIZER(&vtdtr_queue_tree);
RB_GENERATE_STATIC(vtdtr_qtree, vtdtr_queue, qnode, qtreecmp);

static struct cdev *vtdtr_dev;
static d_ioctl_t    vtdtr_ioctl;
static d_read_t     vtdtr_read;

static struct cdevsw vtdtr_cdevsw = {
	.d_version = D_VERSION,
	.d_read    = vtdtr_read,
	.d_write   = NULL,
	.d_ioctl   = vtdtr_ioctl,
	.d_open    = vtdtr_open,
	.d_close   = vtdtr_close,
	.d_name    = "vtdtr"
};

/*
 * Helper function used to create a queue entry
 */
static struct vtdtr_qentry *
vtdtr_construct_entry(struct vtdtr_event *e)
{
	struct vtdtr_qentry *ent;
	ent = malloc(sizeof(struct vtdtr_qentry), M_TEMP, M_WAITOK | M_ZERO);
	ent->event = e;

	return (ent);
}

/*
 * XXX: This is currently limited to a number of event types. In the future,
 * there might need to be a more complicated structure, but for now, this will
 * do.
 *
 * FIXME: If we want to allow the users to configure their queues dynamically,
 * we should either do this under a mutex of perform a CAS.
 */
static int
vtdtr_subscribed(struct vtdtr_queue *q, struct vtdtr_event *e)
{

	if (e->type > MAX_BITSHIFT)
		return (0);

	return (q->event_flags & ((size_t)1 << e->type));
}

void
vtdtr_enqueue(struct vtdtr_event *e)
{
	struct vtdtr_queue *q, *tmp;
	struct vtdtr_qentry *ent;

	q = NULL;
	tmp = NULL;

	/*
	 * Iterate over all the known queues
	 */
	mtx_lock(&qtree_mtx);
	RB_FOREACH_SAFE(q, vtdtr_qtree, &vtdtr_queue_tree, tmp) {
		/*
		 * Check if the queue is subscribed to the event
		 */
		mtx_lock(&q->mtx);
		if (q->num_entries >= q->max_size) {
			q->drops++;
			mtx_unlock(&q->mtx);
			continue;
		}

		if (vtdtr_subscribed(q, e)) {
			ent = vtdtr_construct_entry(e);
			STAILQ_INSERT_TAIL(&q->head, ent, next);
			q->num_entries++;
		}
		mtx_unlock(&q->mtx);
	}
	mtx_unlock(&qtree_mtx);
}

static int
vtdtr_read(struct cdev *dev __unused, struct uio *uio, int flags __unused)
{
	return (0);
}

static int
vtdtr_ioctl(struct cdev *dev __unused, u_long cmd, caddr_t addr,
    int flags __unused, struct thread *td)
{
	struct vtdtr_conf *conf;
	struct vtdtr_queue *q, tmp;
	size_t max_size;
	size_t event_flags;
	/*
	 * FIXME: No.
	 */
	static int first = 1;

	max_size = VTDTR_DEFAULT_SIZE;
	event_flags = VTDTR_ALL_EVENTS;

	switch (cmd) {
	case VTDTRIOC_CONF:
		/*
		 * FIXME: Nasty, but currently helps prevent concurrency issues.
		 */
		if (!first)
			return (EBUSY);
		first = 0;
		conf = (struct vtdtr_conf *)addr;
		tmp.proc = td->td_proc;
		mtx_lock(&qtree_mtx);
		q = RB_FIND(vtdtr_qtree, &vtdtr_queue_tree, &tmp);
		mtx_unlock(&qtree_mtx);
		if (q == NULL)
			return (ENOENT);

		/*
		 * We just set the default configuration if no configuration has
		 * been passed in. Eases programming on the consumer side.
		 */
		if (conf == NULL)
			goto finalize_conf;

		if (conf->max_size != 0)
			max_size = conf->max_size;
		if (conf->event_flags != 0)
			event_flags = conf->event_flags;

finalize_conf:
		/*
		 * FIXME: Similarly as in vtdtr_subscribed, if we want this to
		 * be done dynamically, we should either lock or perform CAS.
		 */
		q->max_size = max_size;
		q->event_flags = event_flags;
		break;
	default:
		break;
	}
	return (0);
}

static int
vtdtr_open(struct cdev *dev __unused, int oflags, int devtype, struct thread *td)
{
	struct vtdtr_queue *q, tmp;
	int error, n;
	char mtx_name[VTDTR_MTX_NAME_SIZE];

	/*
	 * Check if we can inspect what DTrace is doing to the kernel, as we are
	 * currently not limiting what the consumer can listen for.
	 */
	error = priv_check(td, PRIV_DTRACE_KERNEL);
	if (error)
		return (error);

	tmp.proc = td->td_proc;

	/*
	 * If we find an entry in the tree, that means a process is trying to
	 * open two file descriptors for the driver. For simplicity and
	 * performance reasons (and sort of to impose a certain type of
	 * architecture to avoid race conditions), we return EBUSY.
	 */
	mtx_lock(&qtree_mtx);
	q = RB_FIND(vtdtr_qtree, &vtdtr_queue_tree, &tmp);
	if (q != NULL) {
		mtx_unlock(&qtree_mtx);
		return (EBUSY);
	}
	mtx_unlock(&qtree_mtx);

	/*
	 * Set up the queue's initial state.
	 */
	q = malloc(sizeof(struct vtdtr_queue), M_DEVBUF, M_WAITOK | M_ZERO);
	q->proc = td->td_proc;

	n = snprintf(mtx_name, VTDTR_MTX_NAME_SIZE,
	    "VTDTR Queue Mutex: %d", q->proc->p_pid);
	KASSERT(n < VTDTR_MTX_NAME_SIZE,
	    ("n = %zu %s(%d)", n, __func__, __LINE__));

	mtx_init(&q->mtx, "vtdtrqmtx", mtx_name, MTX_DEF);
	STAILQ_INIT(&q->head);

	/*
	 * Drop it into the tree.
	 */
	mtx_lock(&qtree_mtx);
	RB_INSERT(vtdtr_qtree, &vtdtr_queue_tree, q);
	mtx_unlock(&qtree_mtx);

	return (0);
}

static void
vtdtr_flush(struct vtdtr_queue *q)
{
	struct vtdtr_qentry *ent, *tmp;

	ASSERT(MUTEX_HELD(q->mtx));
	STAILQ_FOREACH_SAFE(ent, &q->head, next, tmp) {
		STAILQ_REMOVE(&q->head, ent, vtdtr_qentry, next);
		free(ent->event, M_TEMP);
		free(ent, M_TEMP);
	}
}

static int
vtdtr_close(struct cdev *dev __unused, int foo, int bar, struct thread *td)
{
	struct vtdtr_queue *q, tmp;

	tmp.proc = td->td_proc;

	mtx_lock(&qtree_mtx);
	q = RB_FIND(vtdtr_qtree, &vtdtr_queue_tree, &tmp);
	if (q == NULL) {
		mtx_unlock(&qtree_mtx);
		return (ESRCH);
	}

	RB_REMOVE(vtdtr_qtree, &vtdtr_queue_tree, q);
	mtx_unlock(&qtree_mtx);

	mtx_lock(&q->mtx);
	vtdtr_flush(q);
	mtx_unlock(&q->mtx);

	free(q, M_DEVBUF);

	return (0);
}

static int
vtdtr_modevent(module_t mod __unused, int type, void *data __unused)
{
	switch(type) {
	case MOD_LOAD:
		if (bootverbose)
			printf("vtdtr: <vtdtr device>\n");
		vtdtr_dev = make_dev_credf(MAKEDEV_ETERNAL_KLD, &vtdtr_cdevsw, 0,
		    NULL, UID_ROOT, GID_WHEEL, 0440, "vtdtr");
		mtx_init(&qtree_mtx, "Queue tree mutex", NULL, MTX_DEF);
		break;
	case MOD_UNLOAD:
		/*
		 * FIXME: We ought to clean up the queues here.
		 */
		mtx_destroy(&qtree_mtx);
		destroy_dev(vtdtr_dev);
		break;
	case MOD_SHUTDOWN:
		break;
	default:
		return (EOPNOTSUPP);
	};

	return (0);
}

static int
qtreecmp(struct vtdtr_queue *q1, struct vtdtr_queue *q2)
{
	if (q1->proc->p_pid == q2->proc->p_pid)
		return (0);

	return ((q1->proc->p_pid < q2->proc->p_pid) ? -1 : 1);
}

DEV_MODULE(vtdtr, vtdtr_modevent, NULL);
MODULE_VERSION(vtdtr, 1);
