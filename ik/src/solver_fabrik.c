#include "ik/chain_tree.h"
#include "ik/effector.h"
#include "ik/log.h"
#include "ik/bone.h"
#include "ik/solver.h"
#include "ik/subtree.h"
#include "ik/transform.h"
#include "ik/vec3.inl"
#include "ik/quat.inl"

#include "cstructures/memory.h"

#include <stddef.h>
#include <math.h>

#include <stdio.h>

struct ik_solver_fabrik
{
    IK_SOLVER_HEAD

    struct ik_chain chain_tree;
    struct ik_chain** effector_chains;
    union ik_vec3* target_positions;

    int num_effectors;
};

/* ------------------------------------------------------------------------- */
static int
validate_poles_recursive(const struct ik_solver_fabrik* solver, const struct ik_chain* chain)
{
    int poles_found = 0;
    CHAIN_FOR_EACH_CHILD(chain, child)
        poles_found += validate_poles_recursive(solver, child);
    CHAIN_END_EACH

    /* Pole target constraints should only be attached to the tip bone of each
     * chain. */
    CHAIN_FOR_EACH_BONE(chain, bone)
        if (bone == chain_get_tip_bone(chain))
            continue;
        if (bone->pole != NULL)
        {
            ik_log_printf(IK_WARN, "FABRIK: Pole attached to bone (address: 0x%p) has no effect and will be ignored.", bone);
            poles_found++;
        }
    CHAIN_END_EACH

    return poles_found;
}
static void
validate_poles(const struct ik_solver_fabrik* solver)
{
    if (validate_poles_recursive(solver, &solver->chain_tree))
    {
        ik_log_printf(IK_WARN, "FABRIK: Poles only make sense when attached to the end of chains, such as effector bones, or bones with multiple children.");
    }
}

/* ------------------------------------------------------------------------- */
static void
store_effector_bones_recursive(struct ik_chain*** effector_chains_store, struct ik_chain* chain)
{
    CHAIN_FOR_EACH_CHILD(chain, child)
        store_effector_bones_recursive(effector_chains_store, child);
    CHAIN_END_EACH

    if (chain_child_count(chain) == 0)
    {
        **effector_chains_store = chain;
        (*effector_chains_store)++;
    }
}
static void
store_effector_chains(struct ik_solver_fabrik* solver)
{
    struct ik_chain** effector_chains_store = solver->effector_chains;
    store_effector_bones_recursive(&effector_chains_store, &solver->chain_tree);

    /* sanity check */
    assert(effector_chains_store - solver->effector_chains == solver->num_effectors);
}

/* ------------------------------------------------------------------------- */
static void
calculate_target_data(struct ik_solver_fabrik* solver)
{
    int i;

    for (i = 0; i != solver->num_effectors; ++i)
    {
        union ik_vec3 tip_pos;
        const struct ik_bone* base_bone = chain_get_base_bone(&solver->chain_tree);
        const struct ik_bone* root_bone = solver->root_bone;
        const struct ik_chain* eff_chain = solver->effector_chains[i];
        const struct ik_bone* tip_bone = chain_get_tip_bone(eff_chain);
        struct ik_effector* eff = tip_bone->effector;
        ikreal* target = solver->target_positions[i].f;

        /*
         * The "actual" target position is calculated once and must be stored in
         * a space outside of the bones being solved. It is retrieved by the
         * FABRIK algorithm each iteration.
         *
         * The actual target position depends on the effector target position
         * and the effector weight. Most of the time it is just equal to the
         * effector's target position.
         *
         * Note that sometimes the parent of root_bone is not equal to the
         * parent of base_bone. We transform the target position up to the
         * parent of base_bone because all parent bones are static from the
         * solver's point of view.
         */
        ik_vec3_copy(target, eff->target_position.f);
        ik_transform_bone_pos_g2l(target, ik_bone_get_parent(root_bone), ik_bone_get_parent(base_bone));

        /* In order to lerp between tip bone position and target, transform tip
         * bone position into same space */
        tip_pos = tip_bone->position;
        ik_transform_bone_pos_l2g(tip_pos.f, tip_bone, ik_bone_get_parent(base_bone));

        /* lerp by effector weight to get actual target position */
        ik_vec3_sub_vec3(target, tip_pos.f);
        ik_vec3_mul_scalar(target, eff->weight);
        ik_vec3_add_vec3(target, tip_pos.f);

        /* nlerp actual target position around the next sub-base bone. Makes
         * transitions look more natural */
        if (eff->features & IK_EFFECTOR_WEIGHT_NLERP)
        {
            ikreal distance_to_target;
            const struct ik_bone* subbase_bone = chain_get_base_bone(eff_chain);
            union ik_vec3 to_tip = tip_bone->position;
            union ik_vec3 to_eff = eff->target_position;

            /* Need two vectors from subbase to tip and from subbase to effector target */
            ik_transform_bone_pos_l2g(to_tip.f, tip_bone, subbase_bone);
            ik_transform_bone_pos_g2l(to_eff.f, ik_bone_get_parent(root_bone), subbase_bone);

            /* The effective distance is a lerp between the distances of these two vectors*/
            distance_to_target = ik_vec3_length(to_eff.f) * eff->weight;
            distance_to_target += ik_vec3_length(to_tip.f) * (1.0 - eff->weight);

            /* nlerp the target position by pinning it to the base bone */
            ik_transform_bone_pos_g2l(target, ik_bone_get_parent(base_bone), subbase_bone);
            ik_vec3_normalize(target);
            ik_vec3_mul_scalar(target, distance_to_target);
            ik_transform_bone_pos_l2g(target, subbase_bone, ik_bone_get_parent(base_bone));
        }
    }
}

/* ------------------------------------------------------------------------- */
static union ik_vec3
solve_chain_forwards_recurse(struct ik_chain* chain,
                             union ik_vec3** target_store,
                             const struct ik_bone* base_bone)
{
    union ik_vec3 target;
    int avg_count;

    /* Target position for the tip of each chain is the average position of all
     * solved base bone positions */
    avg_count = 0;
    ik_vec3_set_zero(target.f);
    CHAIN_FOR_EACH_CHILD(chain, child)
        union ik_vec3 base_pos = solve_chain_forwards_recurse(child, target_store, base_bone);
        ik_vec3_add_vec3(target.f, base_pos.f);
        ++avg_count;
    CHAIN_END_EACH

    /* Reached a leaf chain. Retrieve target position calculated earlier in
     * target_store and transform it into the correct space */
    if (avg_count == 0)
    {
        ik_vec3_copy(target.f, (*target_store)++->f);
        ik_transform_bone_pos_g2l(target.f, ik_bone_get_parent(base_bone), chain_get_tip_bone(chain));
    }
    else
        ik_vec3_div_scalar(target.f, avg_count);

    /* tip bone is handled slightly differently, as it has no constraints when
     * doing forward iteration */
    {
        union ik_quat delta;
        struct ik_bone* bone = chain_get_tip_bone(chain);

        /* Rotate bone towards target position */
        ik_quat_angle_of(delta.f, target.f);
        ik_quat_mul_quat(bone->rotation.f, delta.f);

        /*
         * Because the bone has rotated, the target position will have moved
         * in global space. In local space this equates to the position rotating
         * around the bone by the same amount in the opposite direction.
         *
         * This is slightly faster than a quat mul
         */
        ik_vec3_set(target.f, 0, 0, ik_vec3_length(target.f));

        /*
         * New target position in local space is at the tail end of this bone,
         * minus the bone's offset position. The offset position is in parent
         * bone space, so we must move the target to the tail of this bone,
         * transform it into parent space, then subtract this bone's offset
         * from it.
         */
        target.v.z -= bone->length;
        ik_vec3_rotate_quat(target.f, bone->rotation.f);
        ik_vec3_add_vec3(target.f, bone->position.f);
        /* target is not quite in parent bone space yet, see beginning of
         * for-loop below */
    }

    CHAIN_FOR_EACH_BONE_PAIR(chain, bone, child)
        union ik_quat delta;
        union ik_quat offset_rot;
        union ik_vec3 child_tail_pos;

        /* complete transformation into parent space */
        target.v.z += bone->length;

        /*
         * The child bone can have an offset position relative to the current
         * bone's head position, which introduces an offset rotation that has
         * to be compensated. Calculate this rotation now.
         *
         *                   o <- tip_pos_head
         *       tip bone -> |
         *                   o <- tip_pos_tail
         *                  .
         *                 .
         *                .
         *               o <- base_pos_head
         *  base bone -> |
         *               o
         *
         */
        child_tail_pos = child->position;
        child_tail_pos.v.z += bone->length;
        ik_quat_angle_of(offset_rot.f, child_tail_pos.f);

        /* Rotate bone towards target position. Make sure to apply the before-mentioned
         * offset rotation. */
        ik_quat_angle_of(delta.f, target.f);
        ik_quat_mul_quat_conj(delta.f, offset_rot.f);
        ik_quat_mul_quat(bone->rotation.f, delta.f);

        /* The child bone must retain its orientation. Rotate it in the opposite
         * direction */
        ik_quat_mul_quat_conj(child->rotation.f, delta.f);

        /*
         * Because the bone has rotated, the target position will have moved
         * in global space. In local space this equates to the position rotating
         * around the bone by the same amount in the opposite direction.
         */
        ik_vec3_rotate_quat_conj(target.f, delta.f);

        /*
         * New target position in local space is at the tail end of this bone,
         * minus the bone's offset position. The offset position is in parent
         * bone space, so we must move the target to the tail of this bone,
         * transform it into parent space, then subtract this bone's offset
         * from it.
         */
        ik_vec3_sub_vec3(target.f, child_tail_pos.f);
        ik_vec3_rotate_quat(target.f, bone->rotation.f);
        ik_vec3_add_vec3(target.f, bone->position.f);
        /* target is not quite in parent bone space yet, the transformation is
         * completed at the start of this for-loop */
    CHAIN_END_EACH

    return target;
}
static union ik_vec3
solve_chain_forwards(struct ik_solver_fabrik* solver)
{
    const struct ik_bone* base_bone = chain_get_base_bone(&solver->chain_tree);
    union ik_vec3* target_store = solver->target_positions;
    union ik_vec3 target = solve_chain_forwards_recurse(&solver->chain_tree, &target_store, base_bone);

    /* This sets up the target position correctly for backwards iteration */
    ik_vec3_sub_vec3(target.f, base_bone->position.f);
    ik_vec3_negate(target.f);
    ik_vec3_add_vec3(target.f, base_bone->position.f);

    return target;
}

/* ------------------------------------------------------------------------- */
static void
solve_chain_backwards_constraints_recurse(struct ik_chain* chain, union ik_vec3 target)
{
    CHAIN_FOR_EACH_BONE_R(chain, bone)

    CHAIN_END_EACH

    CHAIN_FOR_EACH_CHILD(chain, child)
        solve_chain_backwards_constraints_recurse(child, target);
    CHAIN_END_EACH
}
static void
solve_chain_backwards_constraints(struct ik_solver_fabrik* solver, union ik_vec3 target)
{
    solve_chain_backwards_constraints_recurse(&solver->chain_tree, target);
}

/* ------------------------------------------------------------------------- */
static void
solve_chain_backwards_recurse(struct ik_chain* chain, union ik_vec3 target)
{
    CHAIN_FOR_EACH_BONE(chain, bone)

    CHAIN_END_EACH

    CHAIN_FOR_EACH_CHILD(chain, child)
        solve_chain_backwards_recurse(child, target);
    CHAIN_END_EACH
}
static void
solve_chain_backwards(struct ik_solver_fabrik* solver, union ik_vec3 target)
{
    solve_chain_backwards_recurse(&solver->chain_tree, target);
}

/* ------------------------------------------------------------------------- */
static int
fabrik_init(struct ik_solver* solver_base, const struct ik_subtree* subtree)
{
    int num_chains;
    struct ik_solver_fabrik* solver = (struct ik_solver_fabrik*)solver_base;

    chain_tree_init(&solver->chain_tree);
    if (chain_tree_build(&solver->chain_tree, subtree) != 0)
        goto build_chain_tree_failed;

    solver->num_effectors = subtree_leaves(subtree);
    num_chains = chain_count(&solver->chain_tree);

    solver->effector_chains = MALLOC(sizeof(*solver->effector_chains) * solver->num_effectors);
    if (solver->effector_chains == NULL)
        goto alloc_effector_bones_failed;

    solver->target_positions = MALLOC(sizeof(*solver->target_positions) * solver->num_effectors);
    if (solver->target_positions == NULL)
        goto alloc_target_positions_failed;

    store_effector_chains(solver);
    validate_poles(solver);

    ik_log_printf(IK_DEBUG, "FABRIK: Initialized with %d end-effectors. %d chains were created.",
                  solver->num_effectors, num_chains);

    return 0;

    alloc_target_positions_failed   : FREE(solver->effector_chains);
    alloc_effector_bones_failed     :
    build_chain_tree_failed         : chain_tree_deinit(&solver->chain_tree);
    return -1;
}

/* ------------------------------------------------------------------------- */
static void
fabrik_deinit(struct ik_solver* solver_base)
{
    struct ik_solver_fabrik* solver = (struct ik_solver_fabrik*)solver_base;

    chain_tree_deinit(&solver->chain_tree);
    FREE(solver->target_positions);
    FREE(solver->effector_chains);
}

/* ------------------------------------------------------------------------- */
static int
all_targets_reached(struct ik_solver_fabrik* solver, ikreal tol_squared)
{
    /* TODO broken
    int i;
    for (i = 0; i != solver->num_effectors; ++i)
    {
        struct ik_bone* bone = solver->effector_bones[i];
        struct ik_effector* eff = bone->effector;
        union ik_vec3 diff = bone->position;

        ik_vec3_sub_vec3(diff.f, eff->target_position.f);
        if (ik_vec3_length_squared(diff.f) > tol_squared)
            return 0;
    }*/

    return 0;
}

/* ------------------------------------------------------------------------- */
static int
fabrik_solve(struct ik_solver* solver_base)
{
    struct ik_solver_fabrik* solver = (struct ik_solver_fabrik*)solver_base;
    struct ik_algorithm* alg = solver->algorithm;
    int iteration = alg->max_iterations;
    ikreal tol_squared = alg->tolerance * alg->tolerance;

    calculate_target_data(solver);

    while (iteration-- > 0)
    {
        union ik_vec3 base_pos = solve_chain_forwards(solver);
        //solve_chain_backwards(solver, base_pos);

        /*if (alg->features & IK_ALGORITHM_CONSTRAINTS)
            solve_chain_backwards_constraints(solver, base_pos);
        else*/

        if (all_targets_reached(solver, tol_squared))
            break;
    }

    return alg->max_iterations - iteration;
}

/* ------------------------------------------------------------------------- */
static void
visit_bones_recursive(const struct ik_chain* chain, ik_bone_visit_func visit, void* param)
{
    CHAIN_FOR_EACH_BONE_R(chain, bone)
        visit(bone, param);
    CHAIN_END_EACH

    CHAIN_FOR_EACH_CHILD(chain, child)
        visit_bones_recursive(child, visit, param);
    CHAIN_END_EACH
}
static void
fabrik_visit_bones(const struct ik_solver* solver_base, ik_bone_visit_func visit, void* param)
{
    struct ik_solver_fabrik* solver = (struct ik_solver_fabrik*)solver_base;
    visit_bones_recursive(&solver->chain_tree, visit, param);
}

/* ------------------------------------------------------------------------- */
static void
fabrik_visit_effectors(const struct ik_solver* solver_base, ik_bone_visit_func visit, void* param)
{
    int i;
    struct ik_solver_fabrik* solver = (struct ik_solver_fabrik*)solver_base;

    for (i = 0; i != solver->num_effectors; ++i)
        visit(chain_get_tip_bone(solver->effector_chains[i]), param);
}

/* ------------------------------------------------------------------------- */
struct ik_solver_interface ik_solver_FABRIK = {
    "fabrik",
    sizeof(struct ik_solver_fabrik),
    fabrik_init,
    fabrik_deinit,
    fabrik_solve,
    fabrik_visit_bones,
    fabrik_visit_effectors
};
