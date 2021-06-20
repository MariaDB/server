#ifndef WSREP_PS_H
#define WSREP_PS_H

#include <stdint.h>
#include <stdbool.h>
#include "wsrep_api.h"

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

/* Current API version: */
#define WSREP_PS_API_VERSION 0x200

/* Minimum supported API version on the calling server side: */
#define WSREP_PS_MIN_API_VERSION 0x200

/*!
 * Structures for communicating information that will be exposed
 * through Performance Schema tables:
 */

#define WSREP_HOSTNAME_LENGTH 64
#define WSREP_STATUS_LENGTH 32

/* Information about the current state of all nodes in the cluster: */

typedef struct {
  /* Version number for this data structure: */
  int      wsrep_version;

  /* Local node index: */
  uint32_t wsrep_local_index;

  /* Unique node ID (UUID): */
  char wsrep_node_id[WSREP_UUID_STR_LEN + 1];

  /* User-assigned host name: */
  char wsrep_host_name[WSREP_HOSTNAME_LENGTH + 1];

  /* The UUID of the cluster: */
  char wsrep_cluster_state_uuid[WSREP_UUID_STR_LEN + 1];

  /* The UUID of the state stored on this node: */
  char wsrep_local_state_uuid[WSREP_UUID_STR_LEN + 1];

  /* Status PRIMARY/NON_PRIMARY: */
  char wsrep_status[WSREP_STATUS_LENGTH + 1];

  /* Segment of the node: */
  uint32_t wsrep_segment;

  /* Sequence number of the last applied transaction: */
  uint64_t wsrep_last_applied;

  /* Sequence number of the last committed transaction: */
  uint64_t wsrep_last_committed;

  /* Total number of write-sets replicated: */
  uint64_t wsrep_replicated;

  /* Total size of write-sets replicated: */
  uint64_t wsrep_replicated_bytes;

  /* Total number of write-sets received: */
  uint64_t wsrep_received;

  /* Total size of write-sets received: */
  uint64_t wsrep_received_bytes;

  /* Total number of local transactions that were aborted by slave
     transactions while in execution: */
  uint64_t wsrep_local_bf_aborts;

  /* Total number of local transactions committed: */
  uint64_t wsrep_local_commits;

  /* Total number of local transactions that failed certification test: */
  uint64_t wsrep_local_cert_failures;

  /* Average distance between the highest and lowest concurrently
     applied seqno: */
  double wsrep_apply_window;

  /* Average distance between the highest and lowest concurrently
     committed seqno: */
  double wsrep_commit_window;
} wsrep_node_info_t;

/*! Data structure with statistics of the current node: */

typedef struct {
  /* Version number for this data structure: */
  int      wsrep_version;

  /* Local node index: */
  int      wsrep_local_index;

  /* Unique node ID (UUID): */
  char     wsrep_node_id[WSREP_UUID_STR_LEN + 1];

  /* Total number of keys replicated: */
  uint64_t wsrep_repl_keys;

  /* Total size of keys replicated: */
  uint64_t wsrep_repl_keys_bytes;

  /* Total size of data replicated: */
  uint64_t wsrep_repl_data_bytes;

  /* Total size of other bits replicated: */
  uint64_t wsrep_repl_other_bytes;

  /* Total number of transaction replays due to asymmetric lock
     granularity: */
  uint64_t wsrep_local_replays;

  /* Current (instantaneous) length of the send queue: */
  uint64_t wsrep_local_send_queue;

  /* Send queue length averaged over time since the last
     FLUSH STATUS command: */
  double   wsrep_local_send_queue_avg;

  /* Current (instantaneous) length of the recv queue: */
  uint64_t wsrep_local_recv_queue;

  /* Recv queue length averaged over interval since the last
     FLUSH STATUS command: */
  double   wsrep_local_recv_queue_avg;

  /* The fraction of time (out of 1.0) since the last
     SHOW GLOBAL STATUS that flow control is effective: */
  uint64_t wsrep_flow_control_paused;

  /* The number of flow control messages sent by the local node
     to the cluster: */
  uint64_t wsrep_flow_control_sent;

  /* The number of flow control messages the node has received,
     including those the node has sent: */
  uint64_t wsrep_flow_control_recv;

  /* This variable shows whether a node has flow control
     enabled for normal traffic: */
  char     wsrep_flow_control_status[WSREP_STATUS_LENGTH + 1];

  /* Average distance between the highest and lowest seqno
     value that can be possibly applied in parallel: */
  double   wsrep_cert_deps_distance;

  /* The number of locally running transactions which have been
     registered inside the wsrep provider: */
  uint64_t wsrep_open_transactions;

  /* This status variable provides figures for the replication
     latency on group communication: */
  uint64_t wsrep_evs_repl_latency;
} wsrep_node_stat_t;

/*!
 * @brief Get general cluster information to expose through
 * Performance Schema.
 *
 * @param wsrep       wsrep provider handle.
 * @param nodes       array of data structures with the information
 *                    about the cluster members.
 * @param size        size of the nodes array.
 * @param my_index    index of the current node.
 * @param max_version the maximum version number of the data
 *                    structure that is supported by the server
 *                    calling this function.
 */
typedef wsrep_status_t
  (*wsrep_ps_fetch_cluster_info_t) (wsrep_t*            wsrep,
                                    wsrep_node_info_t** nodes,
                                    uint32_t*           size,
                                    int32_t*            my_index,
                                    uint32_t            max_version);

#define WSREP_PS_FETCH_CLUSTER_INFO_FUNC "wsrep_ps_fetch_cluster_info_v2"

/*!
 * @brief Freeing memory occupied by cluster information.
 *
 * @param wsrep       wsrep provider handle.
 * @param nodes       array of data structures with the information
 *                    about the cluster members.
 */
typedef void
  (*wsrep_ps_free_cluster_info_t) (wsrep_t*           wsrep,
                                   wsrep_node_info_t* nodes);

#define WSREP_PS_FREE_CLUSTER_INFO_FUNC "wsrep_ps_free_cluster_info"

/*!
 * @brief Get the statistical information to expose through
 * Performance Schema.
 *
 * @param wsrep       wsrep provider handle.
 * @param nodes       array of data structures with the statistical
 *                    information about the cluster nodes. Currently
 *                    only one item is returned.
 * @param size        size of the nodes array (currently = 1).
 * @param my_index    index of the current node.
 * @param max_version the maximum version number of the data
 *                    structure that is supported by the server
 *                    calling this function.
 */
typedef wsrep_status_t
  (*wsrep_ps_fetch_node_stat_t) (wsrep_t*            wsrep,
                                 wsrep_node_stat_t** nodes,
                                 uint32_t*           size,
                                 int32_t*            my_index,
                                 uint32_t            max_version);

#define WSREP_PS_FETCH_NODE_STAT_FUNC "wsrep_ps_fetch_node_stat_v2"

/*!
 * @brief Freeing memory occupied by statistical information.
 *
 * @param wsrep       wsrep provider handle.
 * @param nodes       array of data structures with the statistical
 *                    information about the cluster nodes.
 */
typedef void
  (*wsrep_ps_free_node_stat_t) (wsrep_t*           wsrep,
                                wsrep_node_stat_t* nodes);

#define WSREP_PS_FREE_NODE_STAT_FUNC "wsrep_ps_free_node_stat"


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* WSREP_PS_H */
