// This file is a part of Julia. License is MIT: https://julialang.org/license

#include <stdlib.h>
#include <setjmp.h>
#ifdef _OS_WINDOWS_
#include <malloc.h>
#endif
#include "julia.h"
#include "julia_internal.h"
#include "builtin_proto.h"
#include "julia_assert.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    jl_code_info_t *src; // contains the names and number of slots
    jl_method_instance_t *mi; // MethodInstance we're executing, or NULL if toplevel
    jl_module_t *module; // context for globals
    jl_value_t **locals; // slots for holding local slots and ssavalues
    jl_svec_t *sparam_vals; // method static parameters, if eval-ing a method body
    size_t ip; // Leak the currently-evaluating statement index to backtrace capture
    int preevaluation; // use special rules for pre-evaluating expressions (deprecated--only for ccall handling)
    int continue_at; // statement index to jump to after leaving exception handler (0 if none)
} interpreter_state;

#include "interpreter-stacktrace.c"

static jl_value_t *eval_value(jl_value_t *e, interpreter_state *s);
static jl_value_t *eval_body(jl_array_t *stmts, interpreter_state *s, size_t ip, int toplevel);

int jl_is_toplevel_only_expr(jl_value_t *e);

// type definition forms

extern int inside_typedef;

// this is a heuristic for allowing "redefining" a type to something identical
SECT_INTERP static int equiv_type(jl_datatype_t *dta, jl_datatype_t *dtb)
{
    if (!(jl_typeof(dta) == jl_typeof(dtb) &&
          dta->name->name == dtb->name->name &&
          dta->abstract == dtb->abstract &&
          dta->mutabl == dtb->mutabl &&
          dta->size == dtb->size &&
          dta->ninitialized == dtb->ninitialized &&
          jl_egal((jl_value_t*)jl_field_names(dta), (jl_value_t*)jl_field_names(dtb)) &&
          jl_nparams(dta) == jl_nparams(dtb) &&
          jl_svec_len(dta->types) == jl_svec_len(dtb->types)))
        return 0;
    jl_value_t *a=NULL, *b=NULL;
    int ok = 1;
    size_t i, nf = jl_svec_len(dta->types);
    JL_GC_PUSH2(&a, &b);
    a = jl_rewrap_unionall((jl_value_t*)dta->super, dta->name->wrapper);
    b = jl_rewrap_unionall((jl_value_t*)dtb->super, dtb->name->wrapper);
    if (!jl_types_equal(a, b))
        goto no;
    JL_TRY {
        a = jl_apply_type(dtb->name->wrapper, jl_svec_data(dta->parameters), jl_nparams(dta));
    }
    JL_CATCH {
        ok = 0;
    }
    if (!ok)
        goto no;
    assert(jl_is_datatype(a));
    a = dta->name->wrapper;
    b = dtb->name->wrapper;
    while (jl_is_unionall(a)) {
        jl_unionall_t *ua = (jl_unionall_t*)a;
        jl_unionall_t *ub = (jl_unionall_t*)b;
        if (!jl_egal(ua->var->lb, ub->var->lb) || !jl_egal(ua->var->ub, ub->var->ub) ||
            ua->var->name != ub->var->name)
            goto no;
        a = jl_instantiate_unionall(ua, (jl_value_t*)ub->var);
        b = ub->body;
    }
    assert(jl_is_datatype(a) && jl_is_datatype(b));
    a = (jl_value_t*)jl_get_fieldtypes((jl_datatype_t*)a);
    b = (jl_value_t*)jl_get_fieldtypes((jl_datatype_t*)b);
    for (i = 0; i < nf; i++) {
        jl_value_t *ta = jl_svecref(a, i);
        jl_value_t *tb = jl_svecref(b, i);
        if (jl_has_free_typevars(ta)) {
            if (!jl_has_free_typevars(tb) || !jl_egal(ta, tb))
                goto no;
        }
        else if (jl_has_free_typevars(tb) || jl_typeof(ta) != jl_typeof(tb) ||
                 !jl_types_equal(ta, tb)) {
            goto no;
        }
    }
    JL_GC_POP();
    return 1;
 no:
    JL_GC_POP();
    return 0;
}

SECT_INTERP static void check_can_assign_type(jl_binding_t *b, jl_value_t *rhs)
{
    if (b->constp && b->value != NULL && jl_typeof(b->value) != jl_typeof(rhs))
        jl_errorf("invalid redefinition of constant %s",
                  jl_symbol_name(b->name));
}

void jl_reinstantiate_inner_types(jl_datatype_t *t);
void jl_reset_instantiate_inner_types(jl_datatype_t *t);

SECT_INTERP void jl_set_datatype_super(jl_datatype_t *tt, jl_value_t *super)
{
    if (!jl_is_datatype(super) || !jl_is_abstracttype(super) ||
        tt->name == ((jl_datatype_t*)super)->name ||
        jl_subtype(super, (jl_value_t*)jl_vararg_type) ||
        jl_is_tuple_type(super) ||
        jl_is_namedtuple_type(super) ||
        jl_subtype(super, (jl_value_t*)jl_type_type) ||
        jl_subtype(super, (jl_value_t*)jl_builtin_type)) {
        jl_errorf("invalid subtyping in definition of %s",
                  jl_symbol_name(tt->name->name));
    }
    tt->super = (jl_datatype_t*)super;
    jl_gc_wb(tt, tt->super);
}

static void eval_abstracttype(jl_expr_t *ex, interpreter_state *s)
{
    jl_value_t **args = jl_array_ptr_data(ex->args);
    if (inside_typedef)
        jl_error("cannot eval a new abstract type definition while defining another type");
    jl_value_t *name = args[0];
    jl_value_t *para = eval_value(args[1], s);
    jl_value_t *super = NULL;
    jl_value_t *temp = NULL;
    jl_datatype_t *dt = NULL;
    jl_value_t *w = NULL;
    jl_module_t *modu = s->module;
    JL_GC_PUSH5(&para, &super, &temp, &w, &dt);
    assert(jl_is_svec(para));
    if (jl_is_globalref(name)) {
        modu = jl_globalref_mod(name);
        name = (jl_value_t*)jl_globalref_name(name);
    }
    assert(jl_is_symbol(name));
    dt = jl_new_abstracttype(name, modu, NULL, (jl_svec_t*)para);
    w = dt->name->wrapper;
    jl_binding_t *b = jl_get_binding_wr(modu, (jl_sym_t*)name, 1);
    temp = b->value;
    check_can_assign_type(b, w);
    b->value = w;
    jl_gc_wb_binding(b, w);
    JL_TRY {
        inside_typedef = 1;
        super = eval_value(args[2], s);
        jl_set_datatype_super(dt, super);
        jl_reinstantiate_inner_types(dt);
    }
    JL_CATCH {
        jl_reset_instantiate_inner_types(dt);
        b->value = temp;
        jl_rethrow();
    }
    b->value = temp;
    if (temp == NULL || !equiv_type(dt, (jl_datatype_t*)jl_unwrap_unionall(temp))) {
        jl_checked_assignment(b, w);
    }
    JL_GC_POP();
}

static void eval_primitivetype(jl_expr_t *ex, interpreter_state *s)
{
    jl_value_t **args = (jl_value_t**)jl_array_ptr_data(ex->args);
    if (inside_typedef)
        jl_error("cannot eval a new primitive type definition while defining another type");
    jl_value_t *name = args[0];
    jl_value_t *super = NULL, *para = NULL, *vnb = NULL, *temp = NULL;
    jl_datatype_t *dt = NULL;
    jl_value_t *w = NULL;
    jl_module_t *modu = s->module;
    JL_GC_PUSH5(&para, &super, &temp, &w, &dt);
    if (jl_is_globalref(name)) {
        modu = jl_globalref_mod(name);
        name = (jl_value_t*)jl_globalref_name(name);
    }
    assert(jl_is_symbol(name));
    para = eval_value(args[1], s);
    assert(jl_is_svec(para));
    vnb  = eval_value(args[2], s);
    if (!jl_is_long(vnb))
        jl_errorf("invalid declaration of primitive type %s",
                  jl_symbol_name((jl_sym_t*)name));
    ssize_t nb = jl_unbox_long(vnb);
    if (nb < 1 || nb >= (1 << 23) || (nb & 7) != 0)
        jl_errorf("invalid number of bits in primitive type %s",
                  jl_symbol_name((jl_sym_t*)name));
    dt = jl_new_primitivetype(name, modu, NULL, (jl_svec_t*)para, nb);
    w = dt->name->wrapper;
    jl_binding_t *b = jl_get_binding_wr(modu, (jl_sym_t*)name, 1);
    temp = b->value;
    check_can_assign_type(b, w);
    b->value = w;
    jl_gc_wb_binding(b, w);
    JL_TRY {
        inside_typedef = 1;
        super = eval_value(args[3], s);
        jl_set_datatype_super(dt, super);
        jl_reinstantiate_inner_types(dt);
    }
    JL_CATCH {
        jl_reset_instantiate_inner_types(dt);
        b->value = temp;
        jl_rethrow();
    }
    b->value = temp;
    if (temp == NULL || !equiv_type(dt, (jl_datatype_t*)jl_unwrap_unionall(temp))) {
        jl_checked_assignment(b, w);
    }
    JL_GC_POP();
}

/*
 * We maintain a DAG of dependencies between incomplete datatypes. Each SCC in
 * this graph is assigned a unique representative that represents the SCC to
 * other SCCs. Obtain this representative for a given dt.
 */
static jl_datatype_t *get_scc_representative(jl_datatype_t *dt) {
    if (dt->scc == dt)
        return dt;
    // For efficiency we only update the SCC representative when merging SCCs.
    // This update here folds updating the members of the SCC into any
    // subsequent access.
    dt->scc = get_scc_representative(dt->scc);
    return dt->scc;
}

 /*
 * Check if adding a new dependency edge from scc_a to scc_b would add a cycle.
 * If so, return the cycle.
 *
 * Note: We assume the SCC dag to be fairly small such that the search here is
 * not too expensive. Should that not be the case in real world code, investigate
 * using and incremental SCC maintenance algorithm (e.g.
 * https://arxiv.org/pdf/1105.2397.pdf) instead.
 */
static jl_array_t *find_new_cycle(jl_datatype_t *scc_a, jl_datatype_t *scc_b) JL_NOTSAFEPOINT {
    assert(get_scc_representative(scc_a) == scc_a);
    assert(get_scc_representative(scc_b) == scc_b);
    if (scc_a == scc_b) {
        jl_array_t *cycle = jl_alloc_vec_any(0);
        jl_array_ptr_1d_push(cycle, (jl_value_t*)scc_a);
        return cycle;
    }
    for (int i = 0; i < jl_array_len(scc_a->depends); ++i) {
        jl_array_t *cycle = find_new_cycle(
            (jl_datatype_t*)jl_array_ptr_ref(scc_a->depends, i), scc_b);
        if (cycle) {
            jl_array_ptr_1d_push(cycle, (jl_value_t*)scc_a);
            return cycle;
        }
    }
    return NULL;
}

 static void filter_scc(jl_array_t *array, jl_datatype_t *scc_rep)
{
    size_t insert_point = 0;
    for (int i = 0; i < jl_array_len(array); ++i) {
        jl_datatype_t *connected_scc = (jl_datatype_t*)jl_array_ptr_ref(array, i);
        if (get_scc_representative(connected_scc) == scc_rep) {
            // If this SCC is now part of our new SCC, skip it in this list
            continue;
        }
        if (insert_point != i) {
            jl_array_ptr_set(array, insert_point,
                jl_array_ptr_ref(array, i));
        }
        insert_point++;
    }
    jl_array_del_end(array, jl_array_len(array) - insert_point);
}

 static void filter_cycle_and_append(jl_array_t *newa, jl_array_t *olda, jl_datatype_t *scc_rep)
{
    for (int i = 0; i < jl_array_len(olda); ++i) {
        jl_datatype_t *connected_scc = (jl_datatype_t*)jl_array_ptr_ref(olda, i);
        if (get_scc_representative(connected_scc) != scc_rep)
            jl_array_ptr_1d_push(newa, (jl_value_t*)connected_scc);
    }
}

 static void incomplete_add_dep(jl_datatype_t *from, jl_datatype_t *to) {
    assert(from->incomplete);
    assert(to->incomplete);
    // If these datatypes are part of the same SCC, there's nothing
    // to do - they are contracted in the condensation.
    if (get_scc_representative(from) == get_scc_representative(to)) {
        // Nothing to do
        return;
    }
    // See if adding this edge would result in a cycle.
    jl_array_t *cycle = find_new_cycle(get_scc_representative(from),
                                        get_scc_representative(to));
    if (cycle) {
        // If so, we need to contract the cycle by merging all SCCs that
        // are part of the cycle. Arbitrarily pick the first SCC as the new
        // SCC representative.
        jl_datatype_t *new_scc_rep = (jl_datatype_t*)jl_array_ptr_ref(cycle, 0);
        // First go through and set the elements to be members of the
        // new cycle.
        for (int i = 1; i < jl_array_len(cycle); ++i) {
            jl_datatype_t *old_scc = (jl_datatype_t*)jl_array_ptr_ref(cycle, i);
            old_scc->scc = new_scc_rep;
        }
        // Now go through and merge the various array lists
        filter_scc(new_scc_rep->depends, new_scc_rep);
        filter_scc(new_scc_rep->dependents, new_scc_rep);
        for (int i = 1; i < jl_array_len(cycle); ++i) {
            jl_datatype_t *old_scc = (jl_datatype_t*)jl_array_ptr_ref(cycle, 1);
            filter_cycle_and_append(new_scc_rep->depends, old_scc->depends, new_scc_rep);
            filter_cycle_and_append(new_scc_rep->dependents, old_scc->dependents, new_scc_rep);
        }
        return;
    }
    // Check if we need to merge cycle
    jl_array_ptr_1d_push(get_scc_representative(to)->depends,
        (jl_value_t*)get_scc_representative(from));
    jl_array_ptr_1d_push(get_scc_representative(from)->dependents,
        (jl_value_t*)get_scc_representative(to));
}

 static void finish_scc(jl_module_t *mod, jl_datatype_t *scc) {
    assert(jl_array_len(scc->depends) == 0);
    arraylist_t worklist;
    arraylist_new(&worklist, 0);
    arraylist_push(&worklist, scc);
    while (worklist.len != 0) {
        jl_datatype_t *dt = (jl_datatype_t*)arraylist_pop(&worklist);
        if (!dt->incomplete)
            continue;
        dt->incomplete = 0;
        // We don't store the members of the SCC. Instead, do a search through
        // the various things that generate dependencies.
        for (int i = 0; i < jl_svec_len(scc->types); ++i) {
            jl_datatype_t *fdt = (jl_datatype_t*)jl_svecref(scc->types, i);
            if (fdt->incomplete && get_scc_representative(fdt) == scc)
                arraylist_push(&worklist, fdt);
        }
        jl_compute_field_offsets(dt);
    }
    scc->depends = NULL;
    scc->dependents = NULL;
    // See if there were any delayed method instantiations that we now need to
    // add to the method table.
    for (size_t i = 0; i < mod->deferred.len; ++i) {
        jl_svec_t *item = (jl_svec_t*)mod->deferred.items[i];
        jl_method_t *m = (jl_method_t*)jl_svecref(item, 1);
        if (((jl_datatype_t*)jl_unwrap_unionall(m->sig))->scc == scc) {
            jl_method_table_insert(
                (jl_methtable_t*)jl_svecref(item, 0), m, NULL);
        }
    }
}

static void dt_mark_incomplete(jl_datatype_t *dt)
{
    if (dt->incomplete == 0) {
        dt->scc = dt;
        dt->depends = jl_alloc_vec_any(0);
        dt->dependents = jl_alloc_vec_any(0);
    }
    dt->incomplete = 1;
}

static void eval_structtype(jl_expr_t *ex, interpreter_state *s)
{
    jl_value_t **args = jl_array_ptr_data(ex->args);
    if (inside_typedef)
        jl_error("cannot eval a new struct type definition while defining another type");
    jl_value_t *name = args[0];
    jl_value_t *para = eval_value(args[1], s);
    jl_value_t *temp = NULL;
    jl_value_t *super = NULL;
    jl_datatype_t *dt = NULL;
    jl_value_t *w = NULL;
    jl_module_t *modu = s->module;
    JL_GC_PUSH5(&para, &super, &temp, &w, &dt);
    if (jl_is_globalref(name)) {
        modu = jl_globalref_mod(name);
        name = (jl_value_t*)jl_globalref_name(name);
    }
    assert(jl_is_symbol(name));
    assert(jl_is_svec(para));
    temp = eval_value(args[2], s);  // field names
    dt = jl_new_datatype((jl_sym_t*)name, modu, NULL, (jl_svec_t*)para,
                         (jl_svec_t*)temp, NULL,
                         0, args[5]==jl_true ? 1 : 0, jl_unbox_long(args[6]));
    w = dt->name->wrapper;

    jl_binding_t *b = jl_get_binding_wr(modu, (jl_sym_t*)name, 1);
    temp = b->value;  // save old value
    // temporarily assign so binding is available for field types
    check_can_assign_type(b, w);
    b->value = w;
    jl_gc_wb_binding(b, w);

    JL_TRY {
        inside_typedef = 1;
        // operations that can fail
        super = eval_value(args[3], s);
        jl_set_datatype_super(dt, super);
        dt->types = (jl_svec_t*)eval_value(args[4], s);
        jl_gc_wb(dt, dt->types);
        for (size_t i = 0; i < jl_svec_len(dt->types); i++) {
            jl_value_t *elt = jl_svecref(dt->types, i);
            if (jl_typeis(elt, jl_placeholder_type)) {
                jl_array_ptr_1d_push(
                        ((jl_placeholder_t*)elt)->dependents,
                        (jl_value_t*)dt);
                dt_mark_incomplete(dt);
                jl_array_ptr_1d_push(dt->depends, elt);
            } else if ((!jl_is_type(elt) && !jl_is_typevar(elt)) || jl_is_vararg_type(elt)) {
                jl_type_error_rt(jl_symbol_name(dt->name->name),
                                 "type definition",
                                 (jl_value_t*)jl_type_type, elt);
            }
        }
        jl_reinstantiate_inner_types(dt);
    }
    JL_CATCH {
        jl_reset_instantiate_inner_types(dt);
        b->value = temp;
        jl_rethrow();
    }
    jl_compute_field_offsets(dt);

    b->value = temp;
    if (temp == NULL || !equiv_type(dt, (jl_datatype_t*)jl_unwrap_unionall(temp))) {
        jl_checked_assignment(b, w);
    }

    JL_GC_POP();
}

// method definition form

static jl_value_t *eval_methoddef(jl_expr_t *ex, interpreter_state *s)
{
    jl_value_t **args = jl_array_ptr_data(ex->args);
    jl_sym_t *fname = (jl_sym_t*)args[0];
    jl_module_t *modu = s->module;
    if (jl_is_globalref(fname)) {
        modu = jl_globalref_mod(fname);
        fname = jl_globalref_name(fname);
    }
    assert(jl_expr_nargs(ex) != 1 || jl_is_symbol(fname));

    if (jl_is_symbol(fname)) {
        jl_value_t *bp_owner = (jl_value_t*)modu;
        jl_binding_t *b = jl_get_binding_for_method_def(modu, fname);
        jl_value_t **bp = &b->value;
        jl_value_t *gf = jl_generic_function_def(b->name, b->owner, bp, bp_owner, b);
        if (jl_expr_nargs(ex) == 1)
            return gf;
    }

    jl_value_t *atypes = NULL, *meth = NULL;
    JL_GC_PUSH2(&atypes, &meth);
    atypes = eval_value(args[1], s);
    meth = eval_value(args[2], s);
    jl_method_def((jl_svec_t*)atypes, (jl_code_info_t*)meth, s->module);
    JL_GC_POP();
    return jl_nothing;
}

// expression evaluator

SECT_INTERP static jl_value_t *do_call(jl_value_t **args, size_t nargs, interpreter_state *s)
{
    jl_value_t **argv;
    assert(nargs >= 1);
    JL_GC_PUSHARGS(argv, nargs);
    size_t i;
    for (i = 0; i < nargs; i++)
        argv[i] = eval_value(args[i], s);
    jl_value_t *result = jl_apply(argv, nargs);
    JL_GC_POP();
    return result;
}

SECT_INTERP static jl_value_t *do_invoke(jl_value_t **args, size_t nargs, interpreter_state *s)
{
    jl_value_t **argv;
    assert(nargs >= 2);
    JL_GC_PUSHARGS(argv, nargs - 1);
    size_t i;
    for (i = 1; i < nargs; i++)
        argv[i - 1] = eval_value(args[i], s);
    jl_method_instance_t *meth = (jl_method_instance_t*)args[0];
    assert(jl_is_method_instance(meth));
    jl_value_t *result = jl_invoke(argv[1], &argv[2], nargs - 2, meth);
    JL_GC_POP();
    return result;
}

SECT_INTERP jl_value_t *jl_eval_global_var(jl_module_t *m, jl_sym_t *e)
{
    jl_value_t *v = jl_get_global(m, e);
    if (v == NULL)
        jl_undefined_var_error(e);
    return v;
}

SECT_INTERP static int jl_source_nslots(jl_code_info_t *src) JL_NOTSAFEPOINT
{
    return jl_array_len(src->slotflags);
}

SECT_INTERP static int jl_source_nssavalues(jl_code_info_t *src) JL_NOTSAFEPOINT
{
    return jl_is_long(src->ssavaluetypes) ? jl_unbox_long(src->ssavaluetypes) : jl_array_len(src->ssavaluetypes);
}

SECT_INTERP static void eval_stmt_value(jl_value_t *stmt, interpreter_state *s)
{
    jl_value_t *res = eval_value(stmt, s);
    s->locals[jl_source_nslots(s->src) + s->ip] = res;
}

SECT_INTERP static jl_value_t *eval_value(jl_value_t *e, interpreter_state *s)
{
    jl_code_info_t *src = s->src;
    if (jl_is_ssavalue(e)) {
        ssize_t id = ((jl_ssavalue_t*)e)->id - 1;
        if (src == NULL || id >= jl_source_nssavalues(src) || id < 0 || s->locals == NULL)
            jl_error("access to invalid SSAValue");
        else
            return s->locals[jl_source_nslots(src) + id];
    }
    if (jl_is_slot(e)) {
        ssize_t n = jl_slot_number(e);
        if (src == NULL || n > jl_source_nslots(src) || n < 1 || s->locals == NULL)
            jl_error("access to invalid slot number");
        jl_value_t *v = s->locals[n - 1];
        if (v == NULL)
            jl_undefined_var_error((jl_sym_t*)jl_array_ptr_ref(src->slotnames, n - 1));
        return v;
    }
    if (jl_is_quotenode(e)) {
        return jl_quotenode_value(e);
    }
    if (jl_is_globalref(e)) {
        return jl_eval_global_var(jl_globalref_mod(e), jl_globalref_name(e));
    }
    if (jl_is_symbol(e)) {  // bare symbols appear in toplevel exprs not wrapped in `thunk`
        return jl_eval_global_var(s->module, (jl_sym_t*)e);
    }
    if (jl_is_pinode(e)) {
        jl_value_t *val = eval_value(jl_fieldref_noalloc(e, 0), s);
#ifndef JL_NDEBUG
        JL_GC_PUSH1(&val);
        jl_typeassert(val, jl_fieldref_noalloc(e, 1));
        JL_GC_POP();
#endif
        return val;
    }
    assert(!jl_is_phinode(e) && !jl_is_phicnode(e) && !jl_is_upsilonnode(e) && "malformed AST");
    if (!jl_is_expr(e))
        return e;
    jl_expr_t *ex = (jl_expr_t*)e;
    jl_value_t **args = jl_array_ptr_data(ex->args);
    size_t nargs = jl_array_len(ex->args);
    jl_sym_t *head = ex->head;
    if (head == call_sym) {
        return do_call(args, nargs, s);
    }
    else if (head == invoke_sym) {
        return do_invoke(args, nargs, s);
    }
    else if (head == isdefined_sym) {
        jl_value_t *sym = args[0];
        int defined = 0;
        if (jl_is_slot(sym)) {
            ssize_t n = jl_slot_number(sym);
            if (src == NULL || n > jl_source_nslots(src) || n < 1 || s->locals == NULL)
                jl_error("access to invalid slot number");
            defined = s->locals[n - 1] != NULL;
        }
        else if (jl_is_globalref(sym)) {
            defined = jl_boundp(jl_globalref_mod(sym), jl_globalref_name(sym));
        }
        else if (jl_is_symbol(sym)) {
            defined = jl_boundp(s->module, (jl_sym_t*)sym);
        }
        else if (jl_is_expr(sym) && ((jl_expr_t*)sym)->head == static_parameter_sym) {
            ssize_t n = jl_unbox_long(jl_exprarg(sym, 0));
            assert(n > 0);
            if (s->sparam_vals && n <= jl_svec_len(s->sparam_vals)) {
                jl_value_t *sp = jl_svecref(s->sparam_vals, n - 1);
                defined = !jl_is_typevar(sp);
            }
            else {
                // static parameter val unknown needs to be an error for ccall
                jl_error("could not determine static parameter value");
            }
        }
        else {
            assert(0 && "malformed isdefined expression");
        }
        return defined ? jl_true : jl_false;
    }
    else if (head == throw_undef_if_not_sym) {
        jl_value_t *cond = eval_value(args[1], s);
        assert(jl_is_bool(cond));
        if (cond == jl_false) {
            jl_sym_t *var = (jl_sym_t*)args[0];
            if (var == getfield_undefref_sym)
                jl_throw(jl_undefref_exception);
            else
                jl_undefined_var_error(var);
        }
        return jl_nothing;
    }
    else if (head == new_sym) {
        jl_value_t **argv;
        JL_GC_PUSHARGS(argv, nargs);
        for (size_t i = 0; i < nargs; i++)
            argv[i] = eval_value(args[i], s);
        assert(jl_is_structtype(argv[0]));
        jl_value_t *v = jl_new_structv((jl_datatype_t*)argv[0], &argv[1], nargs - 1);
        JL_GC_POP();
        return v;
    }
    else if (head == splatnew_sym) {
        jl_value_t **argv;
        JL_GC_PUSHARGS(argv, 2);
        argv[0] = eval_value(args[0], s);
        argv[1] = eval_value(args[1], s);
        assert(jl_is_structtype(argv[0]));
        jl_value_t *v = jl_new_structt((jl_datatype_t*)argv[0], argv[1]);
        JL_GC_POP();
        return v;
    }
    else if (head == static_parameter_sym) {
        ssize_t n = jl_unbox_long(args[0]);
        assert(n > 0);
        if (s->sparam_vals && n <= jl_svec_len(s->sparam_vals)) {
            jl_value_t *sp = jl_svecref(s->sparam_vals, n - 1);
            if (jl_is_typevar(sp) && !s->preevaluation)
                jl_undefined_var_error(((jl_tvar_t*)sp)->name);
            return sp;
        }
        // static parameter val unknown needs to be an error for ccall
        jl_error("could not determine static parameter value");
    }
    else if (head == copyast_sym) {
        return jl_copy_ast(eval_value(args[0], s));
    }
    else if (head == exc_sym) {
        return jl_current_exception();
    }
    else if (head == boundscheck_sym) {
        return jl_true;
    }
    else if (head == meta_sym || head == inbounds_sym || head == loopinfo_sym) {
        return jl_nothing;
    }
    else if (head == gc_preserve_begin_sym || head == gc_preserve_end_sym) {
        // The interpreter generally keeps values that were assigned in this scope
        // rooted. If the interpreter learns to be more aggressive here, we may
        // want to explicitly root these values.
        return jl_nothing;
    }
    else if (head == method_sym && nargs == 1) {
        return eval_methoddef(ex, s);
    }
    jl_errorf("unsupported or misplaced expression %s", jl_symbol_name(head));
    abort();
}

// phi nodes don't behave like proper instructions, so we require a special interpreter to handle them
SECT_INTERP static size_t eval_phi(jl_array_t *stmts, interpreter_state *s, size_t ns, size_t to)
{
    size_t from = s->ip;
    size_t ip = to;
    unsigned nphi = 0;
    for (ip = to; ip < ns; ip++) {
        jl_value_t *e = jl_array_ptr_ref(stmts, ip);
        if (!jl_is_phinode(e))
            break;
        nphi += 1;
    }
    if (nphi) {
        jl_value_t **dest = &s->locals[jl_source_nslots(s->src) + to];
        jl_value_t **phis; // = (jl_value_t**)alloca(sizeof(jl_value_t*) * nphi);
        JL_GC_PUSHARGS(phis, nphi);
        for (unsigned i = 0; i < nphi; i++) {
            jl_value_t *e = jl_array_ptr_ref(stmts, to + i);
            assert(jl_is_phinode(e));
            jl_array_t *edges = (jl_array_t*)jl_fieldref_noalloc(e, 0);
            ssize_t edge = -1;
            size_t closest = to; // implicit edge has `to <= edge - 1 < to + i`
            // this is because we could see the following IR (all 1-indexed):
            //   goto %3 unless %cond
            //   %2 = phi ...
            //   %3 = phi (1)[1 => %a], (2)[2 => %b]
            // from = 1, to = closest = 2, i = 1 --> edge = 2, edge_from = 2, from = 2
            for (unsigned j = 0; j < jl_array_len(edges); ++j) {
                size_t edge_from = jl_unbox_long(jl_arrayref(edges, j)); // 1-indexed
                if (edge_from == from + 1) {
                    if (edge == -1)
                        edge = j;
                }
                else if (closest < edge_from && edge_from < (to + i + 1)) {
                    // if we found a nearer implicit branch from fall-through,
                    // that occurred since the last explicit branch,
                    // we should use the value from that edge instead
                    edge = j;
                    closest = edge_from;
                }
            }
            jl_value_t *val = NULL;
            unsigned n_oldphi = closest - to;
            if (n_oldphi) {
                // promote this implicit branch to a basic block start
                // and move all phi values to their position in edges
                // note that we might have already processed some phi nodes
                // in this basic block, so we need to be extra careful here
                // to ignore those
                for (unsigned j = 0; j < n_oldphi; j++) {
                    dest[j] = phis[j];
                }
                for (unsigned j = n_oldphi; j < i; j++) {
                    // move the rest to the start of phis
                    phis[j - n_oldphi] = phis[j];
                    phis[j] = NULL;
                }
                from = closest - 1;
                i -= n_oldphi;
                dest += n_oldphi;
                to += n_oldphi;
                nphi -= n_oldphi;
            }
            if (edge != -1) {
                // if edges list doesn't contain last branch, or the value is explicitly undefined
                // then this value should be unused.
                jl_array_t *values = (jl_array_t*)jl_fieldref_noalloc(e, 1);
                val = jl_array_ptr_ref(values, edge);
                if (val)
                    val = eval_value(val, s);
            }
            phis[i] = val;
        }
        // now move all phi values to their position in edges
        for (unsigned j = 0; j < nphi; j++) {
            dest[j] = phis[j];
        }
        JL_GC_POP();
    }
    return ip;
}

SECT_INTERP static jl_value_t *eval_body(jl_array_t *stmts, interpreter_state *s, size_t ip, int toplevel)
{
    jl_handler_t __eh;
    size_t ns = jl_array_len(stmts);

    while (1) {
        s->ip = ip;
        if (ip >= ns)
            jl_error("`body` expression must terminate in `return`. Use `block` instead.");
        if (toplevel)
            jl_get_ptls_states()->world_age = jl_world_counter;
        jl_value_t *stmt = jl_array_ptr_ref(stmts, ip);
        assert(!jl_is_phinode(stmt));
        size_t next_ip = ip + 1;
        assert(!jl_is_phinode(stmt) && !jl_is_phicnode(stmt) && "malformed AST");
        if (jl_is_gotonode(stmt)) {
            next_ip = jl_gotonode_label(stmt) - 1;
        }
        else if (jl_is_upsilonnode(stmt)) {
            jl_value_t *val = jl_fieldref_noalloc(stmt, 0);
            if (val)
                val = eval_value(val, s);
            jl_value_t *phic = s->locals[jl_source_nslots(s->src) + ip];
            assert(jl_is_ssavalue(phic));
            ssize_t id = ((jl_ssavalue_t*)phic)->id - 1;
            s->locals[jl_source_nslots(s->src) + id] = val;
        }
        else if (jl_is_expr(stmt)) {
            // Most exprs are allowed to end a BB by fall through
            jl_sym_t *head = ((jl_expr_t*)stmt)->head;
            assert(head != unreachable_sym);
            if (head == return_sym) {
                return eval_value(jl_exprarg(stmt, 0), s);
            }
            else if (head == assign_sym) {
                jl_value_t *lhs = jl_exprarg(stmt, 0);
                jl_value_t *rhs = eval_value(jl_exprarg(stmt, 1), s);
                if (jl_is_slot(lhs)) {
                    ssize_t n = jl_slot_number(lhs);
                    assert(n <= jl_source_nslots(s->src) && n > 0);
                    s->locals[n - 1] = rhs;
                }
                else {
                    jl_module_t *modu;
                    jl_sym_t *sym;
                    if (jl_is_globalref(lhs)) {
                        modu = jl_globalref_mod(lhs);
                        sym = jl_globalref_name(lhs);
                    }
                    else {
                        assert(jl_is_symbol(lhs));
                        modu = s->module;
                        sym = (jl_sym_t*)lhs;
                    }
                    JL_GC_PUSH1(&rhs);
                    jl_binding_t *b = jl_get_binding_wr(modu, sym, 1);
                    jl_checked_assignment(b, rhs);
                    JL_GC_POP();
                }
            }
            else if (head == goto_ifnot_sym) {
                jl_value_t *cond = eval_value(jl_exprarg(stmt, 0), s);
                if (cond == jl_false) {
                    next_ip = jl_unbox_long(jl_exprarg(stmt, 1)) - 1;
                }
                else if (cond != jl_true) {
                    jl_type_error("if", (jl_value_t*)jl_bool_type, cond);
                }
            }
            else if (head == enter_sym) {
                jl_enter_handler(&__eh);
                // This is a bit tricky, but supports the implementation of PhiC nodes.
                // They are conceptually slots, but the slot to store to doesn't get explicitly
                // mentioned in the store (aka the "UpsilonNode") (this makes them integrate more
                // nicely with the rest of the SSA representation). In a compiler, we would figure
                // out which slot to store to at compile time when we encounter the statement. We
                // can't quite do that here, but we do something similar: We scan the catch entry
                // block (the only place where PhiC nodes may occur) to find all the Upsilons we
                // can possibly encounter. Then, we remember which slot they store to (we abuse the
                // SSA value result array for this purpose). TODO: We could do this only the first
                // time we encounter a given enter.
                size_t catch_ip = jl_unbox_long(jl_exprarg(stmt, 0)) - 1;
                while (catch_ip < ns) {
                    jl_value_t *phicnode = jl_array_ptr_ref(stmts, catch_ip);
                    if (!jl_is_phicnode(phicnode))
                        break;
                    jl_array_t *values = (jl_array_t*)jl_fieldref_noalloc(phicnode, 0);
                    for (size_t i = 0; i < jl_array_len(values); ++i) {
                        jl_value_t *val = jl_array_ptr_ref(values, i);
                        assert(jl_is_ssavalue(val));
                        size_t upsilon = ((jl_ssavalue_t*)val)->id - 1;
                        assert(jl_is_upsilonnode(jl_array_ptr_ref(stmts, upsilon)));
                        s->locals[jl_source_nslots(s->src) + upsilon] = jl_box_ssavalue(catch_ip + 1);
                    }
                    s->locals[jl_source_nslots(s->src) + catch_ip] = NULL;
                    catch_ip += 1;
                }
                // store current top of exception stack for restore in pop_exception.
                s->locals[jl_source_nslots(s->src) + ip] = jl_box_ulong(jl_excstack_state());
                if (!jl_setjmp(__eh.eh_ctx, 1)) {
                    return eval_body(stmts, s, next_ip, toplevel);
                }
                else if (s->continue_at) { // means we reached a :leave expression
                    ip = s->continue_at;
                    s->continue_at = 0;
                    continue;
                }
                else { // a real exeception
                    ip = catch_ip;
                    continue;
                }
            }
            else if (head == leave_sym) {
                int hand_n_leave = jl_unbox_long(jl_exprarg(stmt, 0));
                assert(hand_n_leave > 0);
                // equivalent to jl_pop_handler(hand_n_leave), but retaining eh for longjmp:
                jl_ptls_t ptls = jl_get_ptls_states();
                jl_handler_t *eh = ptls->current_task->eh;
                while (--hand_n_leave > 0)
                    eh = eh->prev;
                jl_eh_restore_state(eh);
                // leave happens during normal control flow, but we must
                // longjmp to pop the eval_body call for each enter.
                s->continue_at = next_ip;
                jl_longjmp(eh->eh_ctx, 1);
            }
            else if (head == pop_exception_sym) {
                size_t prev_state = jl_unbox_ulong(eval_value(jl_exprarg(stmt, 0), s));
                jl_restore_excstack(prev_state);
            }
            else if (toplevel) {
                if (head == method_sym && jl_expr_nargs(stmt) > 1) {
                    eval_methoddef((jl_expr_t*)stmt, s);
                }
                else if (head == abstracttype_sym) {
                    eval_abstracttype((jl_expr_t*)stmt, s);
                }
                else if (head == primtype_sym) {
                    eval_primitivetype((jl_expr_t*)stmt, s);
                }
                else if (head == structtype_sym) {
                    eval_structtype((jl_expr_t*)stmt, s);
                }
                else if (jl_is_toplevel_only_expr(stmt)) {
                    jl_toplevel_eval(s->module, stmt);
                }
                else if (head == meta_sym) {
                    if (jl_expr_nargs(stmt) == 1 && jl_exprarg(stmt, 0) == (jl_value_t*)nospecialize_sym) {
                        jl_set_module_nospecialize(s->module, 1);
                    }
                    if (jl_expr_nargs(stmt) == 1 && jl_exprarg(stmt, 0) == (jl_value_t*)specialize_sym) {
                        jl_set_module_nospecialize(s->module, 0);
                    }
                }
                else {
                    eval_stmt_value(stmt, s);
                }
            }
            else {
                eval_stmt_value(stmt, s);
            }
        }
        else if (jl_is_newvarnode(stmt)) {
            jl_value_t *var = jl_fieldref(stmt, 0);
            assert(jl_is_slot(var));
            ssize_t n = jl_slot_number(var);
            assert(n <= jl_source_nslots(s->src) && n > 0);
            s->locals[n - 1] = NULL;
        }
        else if (toplevel && jl_is_linenode(stmt)) {
            jl_lineno = jl_linenode_line(stmt);
        }
        else {
            eval_stmt_value(stmt, s);
        }
        ip = eval_phi(stmts, s, ns, next_ip);
    }
    abort();
}

// preparing method IR for interpreter

jl_code_info_t *jl_code_for_interpreter(jl_method_instance_t *mi)
{
    jl_code_info_t *src = (jl_code_info_t*)mi->uninferred;
    if (jl_is_method(mi->def.value)) {
        if (!src || (jl_value_t*)src == jl_nothing) {
            if (mi->def.method->source) {
                src = (jl_code_info_t*)mi->def.method->source;
            }
            else {
                assert(mi->def.method->generator);
                src = jl_code_for_staged(mi);
            }
        }
        if (src && (jl_value_t*)src != jl_nothing) {
            JL_GC_PUSH1(&src);
            src = jl_uncompress_ast(mi->def.method, NULL, (jl_array_t*)src);
            mi->uninferred = (jl_value_t*)src;
            jl_gc_wb(mi, src);
            JL_GC_POP();
        }
    }
    if (!src || !jl_is_code_info(src)) {
        jl_error("source missing for method called in interpreter");
    }
    return src;
}

// interpreter entry points

struct jl_interpret_call_args {
    jl_method_instance_t *mi;
    jl_value_t *f;
    jl_value_t **args;
    uint32_t nargs;
};

SECT_INTERP CALLBACK_ABI void *jl_interpret_call_callback(interpreter_state *s, void *vargs)
{
    struct jl_interpret_call_args *args =
        (struct jl_interpret_call_args *)vargs;
    JL_GC_PROMISE_ROOTED(args);
    jl_code_info_t *src = jl_code_for_interpreter(args->mi);

    jl_array_t *stmts = src->code;
    assert(jl_typeis(stmts, jl_array_any_type));
    jl_value_t **locals;
    JL_GC_PUSHARGS(locals, jl_source_nslots(src) + jl_source_nssavalues(src) + 2);
    locals[0] = (jl_value_t*)src;
    locals[1] = (jl_value_t*)stmts;
    s->locals = locals + 2;
    s->src = src;
    if (jl_is_module(args->mi->def.value)) {
        s->module = args->mi->def.module;
    }
    else {
        s->module = args->mi->def.method->module;
        size_t nargs = args->mi->def.method->nargs;
        int isva = args->mi->def.method->isva ? 1 : 0;
        size_t i;
        s->locals[0] = args->f;
        for (i = 1; i < nargs - isva; i++)
            s->locals[i] = args->args[i - 1];
        if (isva) {
            assert(nargs >= 2);
            s->locals[nargs - 1] = jl_f_tuple(NULL, &args->args[nargs - 2], args->nargs + 2 - nargs);
        }
    }
    s->sparam_vals = args->mi->sparam_vals;
    s->preevaluation = 0;
    s->continue_at = 0;
    s->mi = args->mi;
    jl_value_t *r = eval_body(stmts, s, 0, 0);
    JL_GC_POP();
    return (void*)r;
}

SECT_INTERP jl_value_t *jl_fptr_interpret_call(jl_value_t *f, jl_value_t **args, uint32_t nargs, jl_code_instance_t *codeinst)
{
    struct jl_interpret_call_args callback_args = { codeinst->def, f, args, nargs };
    return (jl_value_t*)enter_interpreter_frame(jl_interpret_call_callback, (void *)&callback_args);
}

struct jl_interpret_toplevel_thunk_args {
    jl_module_t *m;
    jl_code_info_t *src;
};
SECT_INTERP CALLBACK_ABI void *jl_interpret_toplevel_thunk_callback(interpreter_state *s, void *vargs) {
    struct jl_interpret_toplevel_thunk_args *args =
        (struct jl_interpret_toplevel_thunk_args*)vargs;
    JL_GC_PROMISE_ROOTED(args);
    jl_array_t *stmts = args->src->code;
    assert(jl_typeis(stmts, jl_array_any_type));
    jl_value_t **locals;
    JL_GC_PUSHARGS(locals, jl_source_nslots(args->src) + jl_source_nssavalues(args->src));
    s->src = args->src;
    s->locals = locals;
    s->module = args->m;
    s->sparam_vals = jl_emptysvec;
    s->continue_at = 0;
    s->mi = NULL;
    size_t last_age = jl_get_ptls_states()->world_age;
    jl_value_t *r = eval_body(stmts, s, 0, 1);
    jl_get_ptls_states()->world_age = last_age;
    JL_GC_POP();
    return (void*)r;
}

SECT_INTERP jl_value_t *jl_interpret_toplevel_thunk(jl_module_t *m, jl_code_info_t *src)
{
    struct jl_interpret_toplevel_thunk_args args = { m, src };
    return (jl_value_t *)enter_interpreter_frame(jl_interpret_toplevel_thunk_callback, (void*)&args);
}

// deprecated: do not use this method in new code
// it uses special scoping / evaluation / error rules
// which should instead be handled in lowering
struct interpret_toplevel_expr_in_args {
    jl_module_t *m;
    jl_value_t *e;
    jl_code_info_t *src;
    jl_svec_t *sparam_vals;
};

SECT_INTERP CALLBACK_ABI void *jl_interpret_toplevel_expr_in_callback(interpreter_state *s, void *vargs)
{
    struct interpret_toplevel_expr_in_args *args =
        (struct interpret_toplevel_expr_in_args*)vargs;
    JL_GC_PROMISE_ROOTED(args);
    s->src = args->src;
    s->module = args->m;
    s->sparam_vals = args->sparam_vals;
    s->preevaluation = (s->sparam_vals != NULL);
    s->continue_at = 0;
    s->mi = NULL;
    jl_value_t *v = eval_value(args->e, s);
    assert(v);
    return (void*)v;
}

SECT_INTERP jl_value_t *jl_interpret_toplevel_expr_in(jl_module_t *m, jl_value_t *e, jl_code_info_t *src, jl_svec_t *sparam_vals)
{
    struct interpret_toplevel_expr_in_args args = {
        m, e, src, sparam_vals
    };
    return (jl_value_t *)enter_interpreter_frame(jl_interpret_toplevel_expr_in_callback, (void*)&args);
}

#ifdef __cplusplus
}
#endif
