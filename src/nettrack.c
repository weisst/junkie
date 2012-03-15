// -*- c-basic-offset: 4; c-backslash-column: 79; indent-tabs-mode: nil -*-
// vim:sw=4 ts=4 sts=4 expandtab
/* Copyright 2010, SecurActive.
 *
 * This file is part of Junkie.
 *
 * Junkie is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Junkie is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with Junkie.  If not, see <http://www.gnu.org/licenses/>.
 */
/* Here we handle the evolution of states in a graph which vertices
 * are predicates over previous state + new proto info (ie. match functions).
 */
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "junkie/proto/proto.h"
#include "junkie/cpp.h"
#include "junkie/tools/ext.h"
#include "junkie/tools/mallocer.h"
#include "junkie/tools/tempstr.h"
#include "nettrack.h"

LOG_CATEGORY_DEF(nettrack);
#undef LOG_CAT
#define LOG_CAT nettrack_log_category

static MALLOCER_DEF(nettrack);

/*
 * We need to register a callback for every parsers, then try all nodes whose last proto match the called one.
 */


static struct nt_parser_hook {
    struct proto_subscriber subscriber;
    // list of edges which test ends with this proto
    struct nt_edges edges;
    bool registered;    // we only subscribe to this hook when used
} parser_hooks[PROTO_CODE_MAX];


/*
 * Register Files
 */

static void npc_regfile_ctor(struct npc_register *regfile, unsigned nb_registers)
{
    for (unsigned r = 0; r < nb_registers; r++) {
        regfile[r].value = 0;
        regfile[r].size = -1;
    }
}

static struct npc_register *npc_regfile_new(unsigned nb_registers)
{
    size_t size = sizeof(struct npc_register) * nb_registers;
    struct npc_register *regfile = MALLOC(nettrack, size);
    if (! regfile) return NULL;

    npc_regfile_ctor(regfile, nb_registers);
    return regfile;
}

static void npc_regfile_dtor(struct npc_register *regfile, unsigned nb_registers)
{
    for (unsigned r = 0; r < nb_registers; r++) {
        if (regfile[r].size > 0 && regfile[r].value) {
            free((void *)regfile[r].value); // beware that individual registers are mallocated with malloc not MALLOC
        }
    }
}

static void npc_regfile_del(struct npc_register *regfile, unsigned nb_registers)
{
    npc_regfile_dtor(regfile, nb_registers);
    FREE(regfile);
}

static void register_copy(struct npc_register *dst, struct npc_register const *src)
{
    dst->size = src->size;
    if (src->size > 0 && src->value) {
        dst->value = (uintptr_t)malloc(src->size);
        assert(dst->value);  // FIXME
        memcpy((void *)dst->value, (void *)src->value, src->size);
    } else {
        dst->value = src->value;
    }
}

/* Given the regular regfile prev_regfile and the new bindings of new_regfile, return a fresh register with new bindings applied.
 * If steal_from_prev, then the previous values may be moved from prev_regfile to the new one. In any cases the new values are. */
static struct npc_register *npc_regfile_merge(struct npc_register *prev_regfile, struct npc_register *new_regfile, unsigned nb_registers, bool steal_from_prev)
{
    struct npc_register *merged = npc_regfile_new(nb_registers);
    if (! merged) return NULL;

    for (unsigned r = 0; r < nb_registers; r++) {
        if (new_regfile[r].size < 0) {  // still unbound
            if (steal_from_prev) {
                merged[r] = prev_regfile[r];
                prev_regfile[r].size = -1;
            } else {
                register_copy(merged+r, prev_regfile+r);
            }
        } else {
            merged[r] = new_regfile[r];
            new_regfile[r].size = -1;
        }
    }

    return merged;
}

/*
 * States
 */

static int nt_state_ctor(struct nt_state *state, struct nt_state *parent, struct nt_vertex *vertex, struct npc_register *regfile)
{
    SLOG(LOG_DEBUG, "Construct state@%p from state@%p, in vertex %s", state, parent, vertex->name);

    state->regfile = regfile;
    state->parent = parent;
    if (parent) LIST_INSERT_HEAD(&parent->children, state, same_parent);
    LIST_INSERT_HEAD(&vertex->states, state, same_vertex);
    LIST_INIT(&state->children);

    return 0;
}

static struct nt_state *nt_state_new(struct nt_state *parent, struct nt_vertex *vertex, struct npc_register *regfile)
{
    struct nt_state *state = MALLOC(nettrack, sizeof(*state));
    if (! state) return NULL;
    if (0 != nt_state_ctor(state, parent, vertex, regfile)) {
        FREE(state);
        return NULL;
    }
    return state;
}

static void nt_state_del(struct nt_state *, struct nt_graph *);
static void nt_state_dtor(struct nt_state *state, struct nt_graph *graph)
{
    SLOG(LOG_DEBUG, "Destruct state@%p", state);

    // start by killing our children so that they can make use of us
    struct nt_state *child;
    while (NULL != (child = LIST_FIRST(&state->children))) {
        nt_state_del(child, graph);
    }

    if (state->parent) {
        LIST_REMOVE(state, same_parent);
        state->parent = NULL;
    }
    LIST_REMOVE(state, same_vertex);

    if (state->regfile) {
        npc_regfile_del(state->regfile, graph->nb_registers);
        state->regfile = NULL;
    }
}

static void nt_state_del(struct nt_state *state, struct nt_graph *graph)
{
    nt_state_dtor(state, graph);
    FREE(state);
}

static void nt_state_move(struct nt_state *state, struct nt_vertex *from, struct nt_vertex *to)
{
    if (from == to) return;

    SLOG(LOG_DEBUG, "Moving state@%p to vertex %s", state, to->name);
    LIST_REMOVE(state, same_vertex);
    LIST_INSERT_HEAD(&to->states, state, same_vertex);
}

/*
 * Vertices
 */

static int nt_vertex_ctor(struct nt_vertex *vertex, char const *name, struct nt_graph *graph)
{
    SLOG(LOG_DEBUG, "Construct new vertex %s", name);

    vertex->name = STRDUP(nettrack, name);
    LIST_INIT(&vertex->outgoing_edges);
    LIST_INIT(&vertex->incoming_edges);
    LIST_INIT(&vertex->states);
    LIST_INSERT_HEAD(&graph->vertices, vertex, same_graph);

    // An vertex named "root" starts with a nul state
    if (0 == strcmp("root", name)) {
        struct npc_register *regfile = npc_regfile_new(graph->nb_registers);
        if (! regfile) return -1;
        if (! nt_state_new(NULL, vertex, regfile)) {
            npc_regfile_del(regfile, graph->nb_registers);
            return -1;
        }
    }

    /* Additionnaly, there may be an action function to be called whenever this vertex is entered.
     * If so, it's named "entry_"+name. */
    // FIXME: as name is not necessarily a valid symbol identifier better use an explicit parameter.

    char *action_name = tempstr_printf("entry_%s", name);
    vertex->action_fn = lt_dlsym(graph->lib, action_name);
    if (vertex->action_fn) SLOG(LOG_DEBUG, "Found entry action %s", action_name);

    return 0;
}

static struct nt_vertex *nt_vertex_new(char const *name, struct nt_graph *graph)
{
    struct nt_vertex *vertex = MALLOC(nettrack, sizeof(*vertex));
    if (! vertex) return NULL;
    if (0 != nt_vertex_ctor(vertex, name, graph)) {
        FREE(vertex);
        return NULL;
    }
    return vertex;
}

static void nt_edge_del(struct nt_edge *);
static void nt_vertex_dtor(struct nt_vertex *vertex, struct nt_graph *graph)
{
    SLOG(LOG_DEBUG, "Destruct vertex %s", vertex->name);

    // Delete all our states
    struct nt_state *state;
    while (NULL != (state = LIST_FIRST(&vertex->states))) {
        nt_state_del(state, graph);
    }

    // Then all the edges using us
    struct nt_edge *edge;
    while (NULL != (edge = LIST_FIRST(&vertex->outgoing_edges))) {
        nt_edge_del(edge);
    }
    while (NULL != (edge = LIST_FIRST(&vertex->incoming_edges))) {
        nt_edge_del(edge);
    }

    LIST_REMOVE(vertex, same_graph);

    FREE(vertex->name);
    vertex->name = NULL;
}

static void nt_vertex_del(struct nt_vertex *vertex, struct nt_graph *graph)
{
    nt_vertex_dtor(vertex, graph);
    FREE(vertex);
}


/*
 * Edges
 */

static proto_cb_t parser_hook;
static int nt_edge_ctor(struct nt_edge *edge, struct nt_graph *graph, struct nt_vertex *from, struct nt_vertex *to, char const *match_fn_name, bool spawn, bool grab, unsigned death_range, struct proto *inner_proto)
{
    SLOG(LOG_DEBUG, "Construct new edge@%p from %s to %s", edge, from->name, to->name);

    edge->match_fn = lt_dlsym(graph->lib, match_fn_name);
    if (! edge->match_fn) {
        SLOG(LOG_ERR, "Cannot find match function %s", match_fn_name);
        return -1;
    }
    edge->from = from;
    edge->to = to;
    edge->spawn = spawn;
    edge->grab = grab;
    edge->death_range = death_range;
    edge->nb_matches = edge->nb_tries = 0;
    edge->graph = graph;
    LIST_INSERT_HEAD(&from->outgoing_edges, edge, same_from);
    LIST_INSERT_HEAD(&to->incoming_edges, edge, same_to);
    LIST_INSERT_HEAD(&graph->edges, edge, same_graph);
    LIST_INSERT_HEAD(&parser_hooks[inner_proto->code].edges, edge, same_hook);
    if (! parser_hooks[inner_proto->code].registered) {
        if (0 == proto_subscriber_ctor(&parser_hooks[inner_proto->code].subscriber, inner_proto, parser_hook)) {
            parser_hooks[inner_proto->code].registered = true;
        }
    }

    return 0;
}

static struct nt_edge *nt_edge_new(struct nt_graph *graph, struct nt_vertex *from, struct nt_vertex *to, char const *match_fn_name, bool spawn, bool grab, unsigned death_range, struct proto *inner_proto)
{
    struct nt_edge *edge = MALLOC(nettrack, sizeof(*edge));
    if (! edge) return NULL;
    if (0 != nt_edge_ctor(edge, graph, from, to, match_fn_name, spawn, grab, death_range, inner_proto)) {
        FREE(edge);
        return NULL;
    }
    return edge;
}

static void nt_edge_dtor(struct nt_edge *edge)
{
    SLOG(LOG_DEBUG, "Destruct edge@%p", edge);

    LIST_REMOVE(edge, same_from);
    LIST_REMOVE(edge, same_to);
    LIST_REMOVE(edge, same_graph);
    LIST_REMOVE(edge, same_hook);

    edge->graph = NULL;
    edge->match_fn = NULL;
}

static void nt_edge_del(struct nt_edge *edge)
{
    nt_edge_dtor(edge);
    FREE(edge);
}

/*
 * Graph
 */

static LIST_HEAD(nt_graphs, nt_graph) started_graphs;

static int nt_graph_ctor(struct nt_graph *graph, char const *name, char const *libname, unsigned nb_registers)
{
    SLOG(LOG_DEBUG, "Construct new graph %s", name);

    graph->nb_registers = nb_registers;
    graph->lib = lt_dlopen(libname);
    if (! graph->lib) {
        SLOG(LOG_ERR, "Cannot load netmatch shared object %s: %s", libname, lt_dlerror());
        return -1;
    }
    graph->name = STRDUP(nettrack, name);
    graph->started = false;
    graph->nb_frames = 0;

    LIST_INIT(&graph->vertices);
    LIST_INIT(&graph->edges);

    return 0;
}

static struct nt_graph *nt_graph_new(char const *name, char const *libname, unsigned nb_registers)
{
    struct nt_graph *graph = MALLOC(nettrack, sizeof(*graph));
    if (! graph) return NULL;
    if (0 != nt_graph_ctor(graph, name, libname, nb_registers)) {
        FREE(graph);
        return NULL;
    }
    return graph;
}

static void nt_graph_start(struct nt_graph *graph)
{
    if (graph->started) return;
    SLOG(LOG_DEBUG, "Starting nettracking with graph %s", graph->name);
    graph->started = true;
    LIST_INSERT_HEAD(&started_graphs, graph, entry);
}

static void nt_graph_stop(struct nt_graph *graph)
{
    if (! graph->started) return;
    SLOG(LOG_DEBUG, "Stopping nettracking with graph %s", graph->name);
    graph->started = false;
    LIST_REMOVE(graph, entry);
}

static void nt_graph_dtor(struct nt_graph *graph)
{
    SLOG(LOG_DEBUG, "Destruct graph %s", graph->name);

    nt_graph_stop(graph);

    // Delete all our vertices
    struct nt_vertex *vertex;
    while (NULL != (vertex = LIST_FIRST(&graph->vertices))) {
        nt_vertex_del(vertex, graph);
    }
    // Then we are not supposed to have any edge left
    assert(LIST_EMPTY(&graph->edges));

    (void)lt_dlclose(graph->lib);
    graph->lib = NULL;

    FREE(graph->name);
    graph->name = NULL;
}

static void nt_graph_del(struct nt_graph *graph)
{
    nt_graph_dtor(graph);
    FREE(graph);
}

/*
 * Update graph with proto_infos
 */

static void parser_hook(struct proto_subscriber *subscriber, struct proto_info const *last, size_t cap_len, uint8_t const *packet)
{
    (void)cap_len;
    (void)packet;

    SLOG(LOG_DEBUG, "Updating graph with inner info from %s", last->parser->proto->name);

    // Find the parser_hook
    struct nt_parser_hook *hook = DOWNCAST(subscriber, subscriber, nt_parser_hook);
    assert(hook >= parser_hooks+0);
    assert(hook < parser_hooks+(NB_ELEMS(parser_hooks)));
    assert(hook->registered);

    struct nt_edge *edge;
    LIST_FOREACH(edge, &hook->edges, same_hook) {
        // Test this edge for transition
        struct nt_state *state, *tmp;
        LIST_FOREACH_SAFE(state, &edge->from->states, same_vertex, tmp) {   // Beware that this state may move
            SLOG(LOG_DEBUG, "Testing state from vertex %s for %s into %s",
                    edge->from->name,
                    edge->spawn ? "spawn":"move",
                    edge->to->name);
            edge->nb_tries ++;
            /* Delayed bindings:
             *   Matching functions does not change the bindings of the regfile while performing the tests because
             *   we want the binding to take effect only if the tests succeed. Also, since the test order is not
             *   specified then a given test can not both bind and references the same register. Thus, we pass it
             *   two regfiles: one with the actual bindings (read only) and one with the new bindings. On exit, if
             *   the test passed, then the new bindings overwrite the previous ones; otherwise they are discarded.
             *   We try to do this as efficiently as possible by reusing the previously boxed values whenever
             *   possible rather than reallocing/copying them.
             * TODO:
             *   - a flag per node telling of the match function write into the regfile or not would comes handy;
             *   - prevent the test expressions to read and write the same register;
             */
            struct npc_register tmp_regfile[edge->graph->nb_registers];
            npc_regfile_ctor(tmp_regfile, edge->graph->nb_registers);
            if (edge->match_fn(last, state->regfile, tmp_regfile)) {
                SLOG(LOG_DEBUG, "Match!");
                edge->nb_matches ++;
                // We need the merged state in all cases but when we have no action and we don't keep the result
                struct npc_register *merged_regfile = NULL;
                if (edge->to->action_fn || !LIST_EMPTY(&edge->to->outgoing_edges)) {
                    merged_regfile = npc_regfile_merge(state->regfile, tmp_regfile, edge->graph->nb_registers, !edge->spawn);
                    if (! merged_regfile) {
                        SLOG(LOG_WARNING, "Cannot create the new register file");
                        // so be it
                    }
                }
                // Call the entry function
                if (edge->to->action_fn && merged_regfile) {
                    SLOG(LOG_DEBUG, "Calling entry function for vertex '%s'", edge->to->name);
                    edge->to->action_fn(merged_regfile);
                }
                // Now move/spawn/dispose of state
                if (edge->spawn) {
                    if (!LIST_EMPTY(&edge->to->outgoing_edges) && merged_regfile) { // or we do not need to spawn anything
                        if (NULL == (state = nt_state_new(state, edge->to, merged_regfile))) {
                            npc_regfile_del(merged_regfile, edge->graph->nb_registers);
                        }
                    }
                } else {    // move the whole state
                    if (LIST_EMPTY(&edge->to->outgoing_edges)) {  // rather dispose of former state
                        nt_state_del(state, edge->graph);
                    } else if (merged_regfile) {    // replace former regfile with new one
                        npc_regfile_del(state->regfile, edge->graph->nb_registers);
                        state->regfile = merged_regfile;
                        nt_state_move(state, edge->from, edge->to);
                    }
                }
                npc_regfile_dtor(tmp_regfile, edge->graph->nb_registers);
                if (edge->grab) return; // FIXME: oups! there can be more than one graph in this hook, and we don't want to skip all
            } else {
                SLOG(LOG_DEBUG, "No match");
                npc_regfile_dtor(tmp_regfile, edge->graph->nb_registers);
            }
        }
    }
}

/*
 * Extensions
 *
 * It's enough to have a single make-graph function, taking as parameters whatever is required to create the whole graph
 * (except that match expressions are replaced by name of the C function).
 * For instance, here is how a graph might be defined:
 *
 * (make-nettrack "sample graph" "/tmp/libfile.so" nb-registers
 *   ; list of vertices
 *   '((root)
 *     (tcp-syn) ; merely the names. Later: timeout, etc...
 *     (etc...))
 *   ; list of edges
 *   '((match-fun1 root tcp-syn spawn (kill 2))
 *     (match-fun2 root blabla ...)
 *     (etc...)))
 *
 * This returns a smob object that can later be started, deleted, queried for stats, ...
 * but not edited (cannot add/remove vertices nor edges).
 */

static void add_vertex(struct nt_graph *graph, SCM vertex_)
{
    // for now, a vertex is merely a list with a single symbol, the name
    SCM name_ = scm_car(vertex_);
    struct nt_vertex *vertex = nt_vertex_new(scm_to_tempstr(name_), graph);
    if (! vertex) scm_throw(scm_from_latin1_symbol("cannot-create-nt-vertex"), scm_list_1(name_));
}

// Will create the vertex with default attributes if not found
static struct nt_vertex *nt_vertex_lookup(struct nt_graph *graph, char const *name)
{
    struct nt_vertex *vertex;
    LIST_FOREACH(vertex, &graph->vertices, same_graph) {
        if (0 == strcmp(name, vertex->name)) return vertex;
    }

    // Create a new one
    return nt_vertex_new(name, graph);
}

static SCM spawn_sym;
static SCM grab_sym;
static SCM kill_sym;

static void add_edge(struct nt_graph *graph, SCM edge_)
{
    // edge is a list of: match-name inner-proto, vertex-name, vertex-name, param
    // where param can be: spawn, grab, (kill n) ...
    SCM name_ = scm_car(edge_);
    edge_ = scm_cdr(edge_);

    struct proto *inner_proto = proto_of_scm_name(scm_symbol_to_string(scm_car(edge_)));
    if (! inner_proto) {
        scm_throw(scm_from_latin1_symbol("cannot-create-nt-edge"), scm_list_1(scm_car(edge_)));
    }
    edge_ = scm_cdr(edge_);

    struct nt_vertex *from = nt_vertex_lookup(graph, scm_to_tempstr(scm_car(edge_)));
    if (! from) scm_throw(scm_from_latin1_symbol("cannot-create-nt-edge"), scm_list_1(scm_car(edge_)));
    edge_ = scm_cdr(edge_);

    struct nt_vertex *to   = nt_vertex_lookup(graph, scm_to_tempstr(scm_car(edge_)));
    if (! to) scm_throw(scm_from_latin1_symbol("cannot-create-nt-edge"), scm_list_1(scm_car(edge_)));
    edge_ = scm_cdr(edge_);

    bool spawn = false;
    bool grab = false;
    unsigned death_range = 0;

    while (! scm_is_null(edge_)) {
        SCM param = scm_car(edge_);
        if (scm_eq_p(param, spawn_sym)) {
            spawn = true;
        } else if (scm_eq_p(param, grab_sym)) {
            grab = true;
        } else if (scm_is_true(scm_list_p(param)) && scm_eq_p(scm_car(param), kill_sym)) {
            death_range = scm_to_uint(scm_cadr(param));
        } else {
            scm_throw(scm_from_latin1_symbol("unknown-edge-parameter"), scm_list_1(param));
        }
        edge_ = scm_cdr(edge_);
    }

    struct nt_edge *edge = nt_edge_new(graph, from, to, scm_to_tempstr(name_), spawn, grab, death_range, inner_proto);
    if (! edge) scm_throw(scm_from_latin1_symbol("cannot-create-nt-edge"), scm_list_1(name_));
}

static scm_t_bits graph_tag;

static size_t free_graph_smob(SCM graph_smob)
{
    struct nt_graph *graph = (struct nt_graph *)SCM_SMOB_DATA(graph_smob);
    nt_graph_del(graph);
    return 0;
}

static int print_graph_smob(SCM graph_smob, SCM port, scm_print_state unused_ *pstate)
{
    struct nt_graph *graph = (struct nt_graph *)SCM_SMOB_DATA(graph_smob);

    char const *head = tempstr_printf("#<nettrack-graph %s with %u regs", graph->name, graph->nb_registers);
    scm_puts(head, port);

    struct nt_vertex *vertex;
    LIST_FOREACH(vertex, &graph->vertices, same_graph) {
        char const *l = tempstr_printf("\n  vertex %s%s%s", vertex->name,
            LIST_EMPTY(&vertex->outgoing_edges)? " noOut":"",
            LIST_EMPTY(&vertex->incoming_edges)? " noIn":"");
        scm_puts(l, port);
    }

    struct nt_edge *edge;
    LIST_FOREACH(edge, &graph->edges, same_graph) {
        char const *l = tempstr_printf("\n  edge %s -> %s", edge->from->name, edge->to->name);
        scm_puts(l, port);
    }

    scm_puts(" >", port);

    return 1;   // success
}

static struct ext_function sg_make_nettrack;
static SCM g_make_nettrack(SCM name_, SCM libname_, SCM nb_registers_, SCM vertices_, SCM edges_)
{
    scm_dynwind_begin(0);

    // Create an empty graph
    struct nt_graph *graph = nt_graph_new(scm_to_tempstr(name_), scm_to_tempstr(libname_), scm_to_uint(nb_registers_));
    if (! graph) {
        scm_throw(scm_from_latin1_symbol("cannot-create-nt-graph"), scm_list_1(name_));
        assert(!"Never reached");
    }
    scm_dynwind_unwind_handler((void (*)(void *))nt_graph_del, graph, 0);

    // Create vertices
    while (! scm_is_null(vertices_)) {
        add_vertex(graph, scm_car(vertices_));
        vertices_ = scm_cdr(vertices_);
    }

    // Create edges
    while (! scm_is_null(edges_)) {
        add_edge(graph, scm_car(edges_));
        edges_ = scm_cdr(edges_);
    }

    // build the smob
    SCM smob;
    SCM_NEWSMOB(smob, graph_tag, graph);

    scm_dynwind_end();
    return smob;
}

static struct ext_function sg_nettrack_start;
static SCM g_nettrack_start(SCM graph_smob)
{
    scm_assert_smob_type(graph_tag, graph_smob);
    struct nt_graph *graph = (struct nt_graph *)SCM_SMOB_DATA(graph_smob);
    nt_graph_start(graph);
    return SCM_UNSPECIFIED;
}    

static struct ext_function sg_nettrack_stop;
static SCM g_nettrack_stop(SCM graph_smob)
{
    scm_assert_smob_type(graph_tag, graph_smob);
    struct nt_graph *graph = (struct nt_graph *)SCM_SMOB_DATA(graph_smob);
    nt_graph_stop(graph);
    return SCM_UNSPECIFIED;
}    
    
/*
 * Init
 */

static unsigned inited;
void nettrack_init(void)
{
    if (inited++) return;
    log_category_nettrack_init();
	ext_init();
    mallocer_init();
    MALLOCER_INIT(nettrack);
    spawn_sym = scm_permanent_object(scm_from_latin1_symbol("spawn"));
    grab_sym  = scm_permanent_object(scm_from_latin1_symbol("grab"));
    kill_sym  = scm_permanent_object(scm_from_latin1_symbol("kill"));

    LIST_INIT(&started_graphs);

    // Init parser_hooks
    for (unsigned h = 0; h < NB_ELEMS(parser_hooks); h++) {
        parser_hooks[h].registered = false;
        LIST_INIT(&parser_hooks[h].edges);
    }

    // Create a SMOB for nt_graph
    graph_tag = scm_make_smob_type("nettrack-graph", sizeof(struct nt_graph));
    scm_set_smob_free(graph_tag, free_graph_smob);
    scm_set_smob_print(graph_tag, print_graph_smob);
    ext_function_ctor(&sg_make_nettrack,
        "make-nettrack", 5, 0, 0, g_make_nettrack,
        "(make-nettrack \"sample graph\" \"/tmp/libfile.so\" nb-registers\n"
        "  ; list of vertices (optional)\n"
        "  '((root)\n"
        "    (tcp-syn) ; merely the names. Later: timeout, etc...\n"
        "    (etc...))\n"
        "  ; list of edges\n"
        "  '((\"match-fun1\" inner-proto root tcp-syn spawn (kill 2))\n"
        "    (\"match-fun2\" inner-proto root blabla ...)\n"
        "    (etc...))) : create a nettrack graph.\n"
        "Note: you are not supposed to use this directly.\n");

    ext_function_ctor(&sg_nettrack_start,
        "nettrack-start", 1, 0, 0, g_nettrack_start,
        "(nettrack-start graph): start listening events for this graph.\n"
        "See also (? 'nettrack-stop)\n");

    ext_function_ctor(&sg_nettrack_stop,
        "nettrack-stop", 1, 0, 0, g_nettrack_stop,
        "(nettrack-stop graph): stop listening events for this graph.\n"
        "See also (? 'nettrack-start)\n");
}

void nettrack_fini(void)
{
    if (--inited) return;

    mallocer_fini();
	ext_fini();
    log_category_nettrack_fini();
}
