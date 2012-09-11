// -*- c-basic-offset: 4; c-backslash-column: 79; indent-tabs-mode: nil -*-
// vim:sw=4 ts=4 sts=4 expandtab
#ifndef NETTRACK_H_120126
#define NETTRACK_H_120126
#include <stdbool.h>
#include <ltdl.h>
#include "junkie/tools/queue.h"
#include "junkie/tools/log.h"
#include "junkie/tools/timeval.h"
#include "junkie/netmatch.h"

LOG_CATEGORY_DEC(nettrack);

// FIXME: some locks for all these lists

struct nt_state {
    LIST_ENTRY(nt_state) same_parent;
    TAILQ_ENTRY(nt_state) same_vertex;
    /* When a new state is spawned we keep a relationship with parent/children,
     * so that it's possible to terminate a whole family. */
    struct nt_state *parent;
    struct nt_vertex *vertex;
    unsigned index_h; // where I'm located on vertex->states[]
    LIST_HEAD(nt_states, nt_state) children;
    struct npc_register *regfile;
    struct timeval last_used;
    struct timeval last_enter;  // used to find out the age of a state. states are ordered on same_vertex list according to this field (more recently entered at head)
};

struct nt_vertex {
    char *name;
    LIST_ENTRY(nt_vertex) same_graph;
    LIST_HEAD(nt_edges, nt_edge) outgoing_edges;
    struct nt_edges incoming_edges;
    // User defined actions on entry
    npc_match_fn *entry_fn;
    int64_t timeout;   // if >0, number of seconds to keep an inactive state in here
    unsigned index_size;   // the index size (>=1)
    unsigned nb_states;
    TAILQ_HEAD(nt_states_tq, nt_state) states[]; // the states currently waiting in this node (BEWARE: variable size!)
};

struct nt_edge {
    LIST_ENTRY(nt_edge) same_graph;
    struct nt_graph *graph;
    struct nt_vertex *from , *to;
    LIST_ENTRY(nt_edge) same_from, same_to;
    LIST_ENTRY(nt_edge) same_hook;
    npc_match_fn *match_fn;
    npc_match_fn *from_index_fn, *to_index_fn;
    int64_t min_age;    // cross the edge only if its age is greater than this
    // what to do when taken
    bool spawn;  // ie create a new child (otherwise bring the matching state right here)
    bool grab;   // stop looking for other possible transitions
    unsigned death_range;  // terminate all descendants of my Nth parent (if 0, kill all my descendants). if ~0, no kill at all.
    // for statistics
    uint64_t nb_matches, nb_tries;
};

struct nt_graph {
    char *name;
    LIST_HEAD(nt_vertices, nt_vertex) vertices;
    LIST_ENTRY(nt_graph) entry; // in the list of all started graphs
    bool started;
    struct nt_edges edges;
    unsigned nb_registers;
    lt_dlhandle lib;
    unsigned default_index_size;    // index size if not specified in the vertex
    // for statistics
    uint64_t nb_frames;
    // The hooks
    // We need to register a callback for every parsers, then try all nodes whose last proto matches the called one.
    struct nt_parser_hook {
        struct proto_subscriber subscriber;
        struct proto *on_proto; // we keep this for unregistering to subscription
        // list of edges which test ends with this proto
        struct nt_edges edges;
        bool registered;    // we only subscribe to this hook when used (and avoid registering twice)
        struct nt_graph *graph; // backlink to the graph
    } parser_hooks[PROTO_CODE_MAX+1];
#   define FULL_PARSE_EVENT PROTO_CODE_MAX
};

void nettrack_init(void);
void nettrack_fini(void);

#endif
