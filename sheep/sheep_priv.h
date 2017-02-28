/*
 * Copyright (C) 2009-2011 Nippon Telegraph and Telephone Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef __SHEEP_PRIV_H__
#define __SHEEP_PRIV_H__

#include <inttypes.h>
#include <stdbool.h>
#include <urcu/uatomic.h>
#include <time.h>
#include <pthread.h>
#include <math.h>
#include <errno.h>
#include <poll.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <dirent.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/epoll.h>
#include <signal.h>

#include "sheepdog_proto.h"
#include "event.h"
#include "util.h"
#include "work.h"
#include "net.h"
#include "sheep.h"
#include "cluster.h"
#include "rbtree.h"
#include "strbuf.h"
#include "sha1.h"
#include "config.h"
#include "sockfd_cache.h"
#include "fec.h"
#include "common.h"

 /*
  * Functions that update global info must be called in the main
  * thread.  Add main_fn markers to such functions.
  *
  * Functions that can sleep (e.g. disk I/Os or network I/Os) must be
  * called in the worker threads.  Add worker_fn markers to such
  * functions.
  */
#ifdef HAVE_TRACE
#define MAIN_FN_SECTION ".sd_main"
#define WORKER_FN_SECTION ".sd_worker"

#define main_fn __attribute__((section(MAIN_FN_SECTION)))
#define worker_fn __attribute__((section(WORKER_FN_SECTION)))
#else
#define main_fn
#define worker_fn
#endif

enum client_info_type {
	CLIENT_INFO_TYPE_DEFAULT = 1,
#ifdef HAVE_ACCELIO
	CLIENT_INFO_TYPE_XIO,
#endif
};

typedef struct req_iter_S {
	uint8_t *buf;
	uint32_t wlen;
	uint32_t dlen;
	uint64_t off;
}req_iter;

typedef struct client_info_S {
	enum client_info_type type;

	struct connection conn;

	struct request *rx_req;
	struct work rx_work;

	struct request *tx_req;
	struct work tx_work;

	struct list_head done_reqs;

	refcnt_t refcnt;

#ifdef HAVE_ACCELIO
	struct xio_msg *xio_req;
#endif
}client_info;

enum REQUST_STATUS {
	REQUEST_INIT,
	REQUEST_QUEUED,
	REQUEST_DONE,
	REQUEST_DROPPED
};

enum store_id {
	PLAIN_STORE,
	TREE_STORE
};

typedef struct request_iocb_S {
	uint32_t count;
	int efd;
	int result;
}request_iocb;

struct request {
	struct sd_req rq;
	struct sd_rsp rp;

	const struct sd_op_template *op;

	void *data;
	unsigned int data_length;

	struct client_info *ci;
	struct list_node request_list;
	struct list_node pending_list;

	refcnt_t refcnt;
	bool local;
	int local_req_efd;

	uint64_t local_oid;

	struct vnode_info *vinfo;

	struct work work;
	enum REQUST_STATUS status;
	bool stat; /* true if this request is during stat */
};

struct system_info {
	struct cluster_driver *cdrv;
	const char *cdrv_option;

	struct sd_node this_node;

	struct node_info ninfo;
	struct cluster_info cinfo;
	enum sd_node_status node_status;

	uint64_t disk_space;

	DECLARE_BITMAP(vdi_inuse, SD_NR_VDIS);
	DECLARE_BITMAP(vdi_deleted, SD_NR_VDIS);

	int local_req_efd;

	struct sd_mutex local_req_lock;
	struct list_head local_req_queue;
	struct list_head req_wait_queue;
	int nr_outstanding_reqs;

	bool gateway_only;
	bool nosync;

	struct recovery_throttling rthrottling;

	struct work_queue *net_wqueue;
	struct work_queue *gateway_wqueue;
	struct work_queue *io_wqueue;
	struct work_queue *peer_wqueue;
	struct work_queue *reclaim_wqueue;
	struct work_queue *gateway_fwd_wqueue;
	struct work_queue *remove_wqueue;
	struct work_queue *remove_peer_wqueue;
	struct work_queue *deletion_wqueue;
	struct work_queue *recovery_wqueue;
	struct work_queue *recovery_notify_wqueue;
	struct work_queue *block_wqueue;
	struct work_queue *md_wqueue;
	struct work_queue *areq_wqueue;
#ifdef HAVE_HTTP
	struct work_queue *http_wqueue;
#endif

	uatomic_bool use_journal;
	bool backend_dio;
	/* upgrade data layout before starting service if necessary*/
	bool upgrade;
	struct sd_stat stat;
};

struct disk {
	struct rb_node rb;
	char path[PATH_MAX];
	uint64_t space;
};

struct vdisk {
	struct rb_node rb;
	const struct disk *disk;
	uint64_t hash;
};

struct md {
	struct rb_root vroot;
	struct rb_root root;
	struct sd_rw_lock lock;
	uint64_t space;
	uint32_t nr_disks;
};

extern struct md md;

void update_node_disks(void);

struct siocb {
	uint32_t epoch;
	void *buf;
	uint32_t length;
	uint32_t offset;
	uint8_t ec_index;
	uint8_t copy_policy;
	uint8_t wildcard;
};

/* This structure is used to pass parameters to vdi_* functions. */
struct vdi_iocb {
	const char *name;
	const char *tag;
	uint32_t data_len;
	uint64_t size;
	uint32_t base_vid;
	uint32_t snapid;
	bool create_snapshot;
	uint8_t copy_policy;
	uint8_t store_policy;
	uint8_t nr_copies;
	uint8_t block_size_shift;
	uint64_t time;
};

/* This structure is used to get information from sheepdog. */
struct vdi_info {
	uint32_t vid;
	uint32_t snapid;
	uint32_t free_bit;
	uint64_t create_time;
};

struct store_driver {
	struct list_node list;
	enum store_id id;
	const char *name;
	int (*init)(void);
	bool (*exist)(uint64_t oid, uint8_t ec_index);
	/* create_and_write must be an atomic operation*/
	int (*create_and_write)(uint64_t oid, const struct siocb *);
	int (*write)(uint64_t oid, const struct siocb *);
	int (*read)(uint64_t oid, const struct siocb *);
	int (*format)(void);
	int (*remove_object)(uint64_t oid, uint8_t ec_index);
	int (*get_hash)(uint64_t oid, uint32_t epoch, uint8_t *sha1);
	/* Operations in recovery */
	int (*link)(uint64_t oid, uint32_t tgt_epoch);
	int (*update_epoch)(uint32_t epoch);
	int (*purge_obj)(void);
	/* Operations for snapshot */
	int (*cleanup)(void);
};

/* backend store */
int peer_read_obj(struct request *req);
int peer_decref_object(struct request *req);

int default_init(void);
bool default_exist(uint64_t oid, uint8_t ec_index);
int default_create_and_write(uint64_t oid, const struct siocb *iocb);
int default_write(uint64_t oid, const struct siocb *iocb);
int default_read(uint64_t oid, const struct siocb *iocb);
int default_link(uint64_t oid, uint32_t tgt_epoch);
int default_update_epoch(uint32_t epoch);
int default_cleanup(void);
int default_format(void);
int default_remove_object(uint64_t oid, uint8_t ec_index);
int default_get_hash(uint64_t oid, uint32_t epoch, uint8_t *sha1);
int default_purge_obj(void);

int tree_init(void);
bool tree_exist(uint64_t oid, uint8_t ec_index);
int tree_create_and_write(uint64_t oid, const struct siocb *iocb);
int tree_write(uint64_t oid, const struct siocb *iocb);
int tree_read(uint64_t oid, const struct siocb *iocb);
int tree_link(uint64_t oid, uint32_t tgt_epoch);
int tree_update_epoch(uint32_t epoch);
int tree_cleanup(void);
int tree_format(void);
int tree_remove_object(uint64_t oid, uint8_t ec_index);
int tree_get_hash(uint64_t oid, uint32_t epoch, uint8_t *sha1);
int tree_purge_obj(void);

int for_each_object_in_wd(int (*func)(uint64_t, const char *, uint32_t,
				      uint8_t, struct vnode_info *, void *),
			  bool, void *);
int for_each_object_in_stale(int (*func)(uint64_t oid, const char *path,
					 uint32_t epoch, uint8_t,
					 struct vnode_info *, void *arg),
			     void *arg);
int for_each_obj_path(int (*func)(const char *path));
size_t get_store_objsize(uint64_t oid);

extern struct list_head store_drivers;
#define add_store_driver(driver)				\
static void __attribute__((constructor)) add_ ## driver(void)	\
{								\
	list_add(&driver.list, &store_drivers);			\
}

static inline struct store_driver *find_store_driver(const char *name)
{
	struct store_driver *driver;

	list_for_each_entry(driver, &store_drivers, list) {
		if (strcmp(driver->name, name) == 0)
			return driver;
	}
	return NULL;
}

extern struct system_info *sys;
extern struct store_driver *sd_store;
extern char *obj_path;
extern char *epoch_path;

/* One should call this function to get sys->epoch outside main thread */
static inline uint32_t sys_epoch(void)
{
	return uatomic_read(&sys->cinfo.epoch);
}

static inline bool is_aligned_to_pagesize(void *p)
{
	return ((uintptr_t)p & (getpagesize() - 1)) == 0;
}

int create_listen_port(const char *bindaddr, int port);
#ifdef HAVE_ACCELIO
int xio_create_listen_port(const char *bindaddr, int port, bool rdma);
#endif
int init_unix_domain_socket(const char *dir);
void unregister_listening_fds(void);

int init_store_driver(bool is_gateway);
int init_global_pathnames(const char *d, char *);
int init_base_path(const char *dir);
int init_disk_space(const char *d);
int lock_base_dir(const char *d);

int fill_vdi_state_list(const struct sd_req *hdr,
		struct sd_rsp *rsp, void *data);
bool oid_is_readonly(uint64_t oid);
int get_vdi_copy_number(uint32_t vid);
int get_vdi_copy_policy(uint32_t vid);
uint32_t get_vdi_object_size(uint32_t vid);
uint8_t get_vdi_block_size_shift(uint32_t vid);
int get_obj_copy_number(uint64_t oid, int nr_zones);
int get_req_copy_number(struct request *req);
int add_vdi_state(uint32_t vid, int nr_copies, bool snapshot,
		  uint8_t, uint8_t block_size_shift, uint32_t parent_vid);
int add_vdi_state_unordered(uint32_t vid, int nr_copies, bool snapshot,
		  uint8_t, uint8_t block_size_shift, uint32_t parent_vid);
int vdi_exist(uint32_t vid);
int vdi_create(const struct vdi_iocb *iocb, uint32_t *new_vid);
int vdi_snapshot(const struct vdi_iocb *iocb, uint32_t *new_vid);
int vdi_delete(const struct vdi_iocb *iocb, struct request *req);
void vdi_mark_deleted(uint32_t vid);
int vdi_lookup(const struct vdi_iocb *iocb, struct vdi_info *info);
void clean_vdi_state(void);
int sd_delete_vdi(const char *name);
int sd_lookup_vdi(const char *name, uint32_t *vid);
int sd_create_hyper_volume(const char *name, uint32_t *vdi_id);

bool vdi_lock(uint32_t vid, const struct node_id *owner, int type);
bool vdi_unlock(uint32_t vid, const struct node_id *owner, int type);
void apply_vdi_lock_state(struct vdi_state *vs);
void create_vdi_state_checkpoint(int epoch);
int get_vdi_state_checkpoint(int epoch, uint32_t vid, void *data);
void free_vdi_state_checkpoint(int epoch);
void log_vdi_op_lock(uint32_t vid, const struct node_id *owner, int type);
void log_vdi_op_unlock(uint32_t vid, const struct node_id *owner, int type);
void play_logged_vdi_ops(void);
bool is_refresh_required(uint32_t vid);
void validate_myself(uint32_t vid);
void invalidate_other_nodes(uint32_t vid);
int inode_coherence_update(uint32_t vid, bool validate,
			   const struct node_id *sender);
void remove_node_from_participants(const struct node_id *left);
void run_vid_gc(uint32_t vid);

extern int ec_max_data_strip;

int read_vdis(char *data, int len, unsigned int *rsp_len);
int read_del_vdis(char *data, int len, unsigned int *rsp_len);

int get_vdi_attr(struct sheepdog_vdi_attr *vattr, int data_len, uint32_t vid,
		uint32_t *attrid, uint64_t ctime, bool write,
		bool excl, bool delete);

int local_get_node_list(const struct sd_req *req, struct sd_rsp *rsp,
			void *data, const struct sd_node *sender);

struct vnode_info *grab_vnode_info(struct vnode_info *vnode_info);
struct vnode_info *get_vnode_info(void);
void put_vnode_info(struct vnode_info *vinfo);
struct vnode_info *alloc_vnode_info(const struct rb_root *);
struct vnode_info *get_vnode_info_epoch(uint32_t epoch,
					struct vnode_info *cur_vinfo);
int get_nodes_epoch(uint32_t epoch, struct vnode_info *cur_vinfo,
		    struct sd_node *nodes, int len);

void wait_get_vdis_done(void);

int get_nr_copies(struct vnode_info *vnode_info);

void wakeup_requests_on_epoch(void);
void wakeup_requests_on_oid(uint64_t oid);
void wakeup_all_requests(void);
void resume_suspended_recovery(void);

int create_cluster(int port, int64_t zone, int nr_vnodes,
		   bool explicit_addr);
int leave_cluster(void);

void queue_cluster_request(struct request *req);

int prepare_iocb(uint64_t oid, const struct siocb *iocb, bool create);
int err_to_sderr(const char *path, uint64_t oid, int err);
int discard(int fd, uint64_t start, uint32_t end);
bool store_id_match(enum store_id id);

int update_epoch_log(uint32_t epoch, struct sd_node *nodes, size_t nr_nodes);
int inc_and_log_epoch(void);

extern char *config_path;
int set_cluster_config(const struct cluster_info *cinfo);
int set_node_space(uint64_t space);
int get_node_space(uint64_t *space);
bool is_cluster_formatted(void);
bool was_cluster_shutdowned(void);
int set_cluster_shutdown(bool);

int store_file_write(void *buffer, size_t len);
void *store_file_read(void);

int epoch_log_read(uint32_t epoch, struct sd_node *nodes,
				int len, int *nr_nodes);
int epoch_log_read_with_timestamp(uint32_t epoch, struct sd_node *nodes,
				int len, int *nr_nodes, time_t *timestamp);
int epoch_log_read_remote(uint32_t epoch, struct sd_node *nodes,
				int len, int *nr_nodes, time_t *timestamp,
				struct vnode_info *vinfo);
uint32_t get_latest_epoch(void);
void init_config_path(const char *base_path);
int init_node_config_file(void);
int init_config_file(void);
int get_obj_list(const struct sd_req *, struct sd_rsp *, void *);
int objlist_cache_cleanup(uint32_t vid);
void objlist_cache_format(void);

int start_recovery(struct vnode_info *cur_vinfo, struct vnode_info *, bool,
		   bool);
bool oid_in_recovery(uint64_t oid);
bool node_in_recovery(void);
void get_recovery_state(struct recovery_state *state);
void set_recovery(struct recovery_throttling *rthrottling);
struct recovery_throttling get_recovery(void);

int sd_write_object(uint64_t oid, char *data, unsigned int datalen,
		    uint64_t offset, bool create);
int sd_write_object_fwd(uint64_t oid, char *data, unsigned int datalen,
			uint64_t offset, bool create);
int sd_read_object(uint64_t oid, char *data, unsigned int datalen,
		   uint64_t offset);
int sd_read_object_fwd(uint64_t oid, char *data, unsigned int datalen,
		   uint64_t offset);
int sd_remove_object(uint64_t oid);
int sd_dec_object_refcnt(uint64_t data_oid, uint32_t generation,
			 uint32_t refcnt);

struct request_iocb *local_req_init(void);
int exec_local_req(struct sd_req *rq, void *data);
int exec_local_req_async(struct sd_req *rq, void *, struct request_iocb *);
int local_req_wait(struct request_iocb *iocb);

void local_request_init(void);

int objlist_cache_insert(uint64_t oid);
void objlist_cache_remove(uint64_t oid);

void put_request(struct request *req);
void get_request(struct request *req);
void requeue_request(struct request *req);

int sheep_bnode_writer(uint64_t oid, void *mem, unsigned int len,
		       uint64_t offset, uint32_t flags, int copies,
		       int copy_policy, bool create, bool direct);
int sheep_bnode_reader(uint64_t oid, void **mem, unsigned int len,
		       uint64_t offset);

/* Operations */

const struct sd_op_template *get_sd_op(uint8_t opcode);
const char *op_name(const struct sd_op_template *op);
bool is_cluster_op(const struct sd_op_template *op);
bool is_local_op(const struct sd_op_template *op);
bool is_peer_op(const struct sd_op_template *op);
bool is_gateway_op(const struct sd_op_template *op);
bool is_force_op(const struct sd_op_template *op);
bool is_logging_op(const struct sd_op_template *op);
bool has_process_work(const struct sd_op_template *op);
bool has_process_main(const struct sd_op_template *op);
void do_process_work(struct work *work);
int do_process_main(const struct sd_op_template *op, const struct sd_req *req,
		    struct sd_rsp *rsp, void *data,
		    const struct sd_node *sender);
int gateway_to_peer_opcode(int opcode);

extern uint32_t last_gathered_epoch;

static inline bool vnode_is_local(const struct sd_vnode *v)
{
	return node_id_cmp(&v->node->nid, &sys->this_node.nid) == 0;
}

static inline bool node_is_local(const struct sd_node *n)
{
	return node_eq(n, &sys->this_node);
}

/*
 * If the object is read-only, the fragmentation doesn't happen.  In addition,
 * if the object is unlikely to be accessed sequentially, the fragmentation is
 * not a problem.  We can make such objects sparse so that we can use spaces
 * more efficiently.
 */
static inline bool is_sparse_object(uint64_t oid)
{
	return is_ledger_object(oid) || is_vdi_obj(oid);
}

/* gateway operations */
int gateway_read_obj(struct request *req);
int gateway_write_obj(struct request *req);
int gateway_create_and_write_obj(struct request *req);
int gateway_remove_obj(struct request *req);
int gateway_decref_object(struct request *req);

bool is_erasure_oid(uint64_t oid);
uint8_t local_ec_index(struct vnode_info *vinfo, uint64_t oid);

/*
 * return true if the request updates a data_vdi_id field of a vdi object
 *
 * XXX: we assume that VMs don't update the inode header and the data_vdi_id
 * field at the same time.
 */
static inline bool is_data_vid_update(const struct sd_req *hdr)
{
	return is_vdi_obj(hdr->obj.oid) &&
		data_vid_offset(0) <= hdr->obj.offset &&
		hdr->obj.offset + hdr->data_length <=
			data_vid_offset(SD_INODE_DATA_INDEX);
}

/* store layout migration */
int sd_migrate_store(int from, int to);

struct sockfd *sheep_get_sockfd(const struct node_id *);
void sheep_put_sockfd(const struct node_id *, struct sockfd *);
void sheep_del_sockfd(const struct node_id *, struct sockfd *);
int sheep_exec_req(const struct node_id *nid, struct sd_req *hdr, void *data);
bool sheep_need_retry(uint32_t epoch);

/* journal_file.c */
int journal_file_init(const char *path, size_t size, bool skip);
void clean_journal_file(const char *p);
int
journal_write_store(uint64_t oid, const char *buf, size_t size, off_t, bool);
int journal_remove_object(uint64_t oid);

/* md.c */
bool md_add_disk(const char *path, bool);
uint64_t md_init_space(void);
const char *md_get_object_dir(uint64_t oid);
int md_handle_eio(const char *);
bool md_exist(uint64_t oid, uint8_t ec_index, char *path);
int md_get_stale_path(uint64_t oid, uint32_t epoch, uint8_t ec_index, char *);
uint32_t md_get_info(struct sd_md_info *info);
int md_plug_disks(char *disks);
int md_unplug_disks(char *disks);
uint64_t md_get_size(uint64_t *used);
uint32_t md_nr_disks(void);



/* http.c */
#ifdef HAVE_HTTP
int http_init(const char *options);
#endif /* END BUILD_HTTP */

#ifdef HAVE_NFS
int nfs_init(const char *options);
int nfs_create(const char *name);
int nfs_delete(const char *name);
#endif

extern bool wildcard_recovery;

struct request *alloc_request(struct client_info *ci, uint32_t data_length);
void queue_request(struct request *req);
void free_request(struct request *req);

#ifdef HAVE_ACCELIO
void xio_send_reply(struct client_info *ci);
#endif

#endif
