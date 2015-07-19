// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2015 Haomai Wang <haomaiwang@gmail.com>
 *
 * Author: Haomai Wang <haomaiwang@gmail.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "RBTree.h"

#define RB_RED 0
#define RB_BLACK 1

/*
 * red-black trees properties:  http://en.wikipedia.org/wiki/Rbtree
 *
 *  1) A node is either red or black
 *  2) The root is black
 *  3) All leaves (NULL) are black
 *  4) Both children of every red node are black
 *  5) Every simple path from root to leaves contains the same number
 *     of black nodes.
 *
 *  4 and 5 give the O(log n) guarantee, since 4 implies you cannot have two
 *  consecutive red nodes in a path and every red node is therefore followed by
 *  a black. So if B is the number of black nodes on every simple path (as per
 *  5), then the longest possible path due to 4 is 2B.
 *
 *  We shall indicate color with case, where black nodes are uppercase and red
 *  nodes will be lowercase. Unknown color nodes shall be drawn as red within
 *  parentheses and have some accompanying text comment.
 */

#define __rb_parent(pc)    ((RBNode*)(pc & ~3))
#define __rb_color(pc)     ((pc) & 1)
#define __rb_is_black(pc)  __rb_color(pc)
#define __rb_is_red(pc)    (!__rb_color(pc))
#define rb_color(rb)       __rb_color((rb)->__rb_parent_color)
#define rb_is_red(rb)      __rb_is_red((rb)->__rb_parent_color)
#define rb_is_black(rb)    __rb_is_black((rb)->__rb_parent_color)

static inline void rb_set_parent(RBNode *rb, RBNode *p)
{
  rb->__rb_parent_color = rb_color(rb) | (unsigned long)p;
}

static inline void rb_set_parent_color(RBNode *rb, RBNode *p, int color)
{
  rb->__rb_parent_color = (unsigned long)p | color;
}

static inline void __rb_change_child(RBNode *old, RBNode *n, RBNode *parent, RBTree *root)
{
  if (parent) {
    if (parent->rb_left == old)
      parent->rb_left = n;
    else
      parent->rb_right = n;
  } else
    root->rb_node = n;
}

static inline void rb_set_black(RBNode *rb)
{
  rb->__rb_parent_color |= RB_BLACK;
}

static inline RBNode *rb_red_parent(RBNode *red)
{
  return (RBNode *)red->__rb_parent_color;
}

/*
 * Helper function for rotations:
 * - old's parent and color get assigned to new
 * - old gets assigned new as a parent and 'color' as a color.
 */
static inline void __rb_rotate_set_parents(RBNode *old, RBNode *n, RBTree *root, int color)
{
  RBNode *parent = old->parent();
  n->__rb_parent_color = old->__rb_parent_color;
  rb_set_parent_color(old, n, color);
  __rb_change_child(old, n, parent, root);
}

static inline RBNode *__rb_erase(RBNode *node, RBTree *root)
{
  RBNode *child = node->rb_right, *tmp = node->rb_left;
  RBNode *parent, *rebalance;
  unsigned long pc;

  if (!tmp) {
    /*
     * Case 1: node to erase has no more than 1 child (easy!)
     *
     * Note that if there is one child it must be red due to 5)
     * and node must be black due to 4). We adjust colors locally
     * so as to bypass __rb_erase_color() later on.
     */
    pc = node->__rb_parent_color;
    parent = __rb_parent(pc);
    __rb_change_child(node, child, parent, root);
    if (child) {
      child->__rb_parent_color = pc;
      rebalance = NULL;
    } else
      rebalance = __rb_is_black(pc) ? parent : NULL;
    tmp = parent;
  } else if (!child) {
    /* Still case 1, but this time the child is node->rb_left */
    tmp->__rb_parent_color = pc = node->__rb_parent_color;
    parent = __rb_parent(pc);
    __rb_change_child(node, tmp, parent, root);
    rebalance = NULL;
    tmp = parent;
  } else {
    RBNode *successor = child, *child2;
    tmp = child->rb_left;
    if (!tmp) {
      /*
       * Case 2: node's successor is its right child
       *
       *    (n)          (s)
       *    / \          / \
       *  (x) (s)  ->  (x) (c)
       *        \
       *        (c)
       */
      parent = successor;
      child2 = successor->rb_right;
    } else {
      /*
       * Case 3: node's successor is leftmost under
       * node's right child subtree
       *
       *    (n)          (s)
       *    / \          / \
       *  (x) (y)  ->  (x) (y)
       *      /            /
       *    (p)          (p)
       *    /            /
       *  (s)          (c)
       *    \
       *    (c)
       */
      do {
        parent = successor;
        successor = tmp;
        tmp = tmp->rb_left;
      } while (tmp);
      parent->rb_left = child2 = successor->rb_right;
      successor->rb_right = child;
      rb_set_parent(child, successor);
    }

    successor->rb_left = tmp = node->rb_left;
    rb_set_parent(tmp, successor);

    pc = node->__rb_parent_color;
    tmp = __rb_parent(pc);
    __rb_change_child(node, successor, tmp, root);
    if (child2) {
      successor->__rb_parent_color = pc;
      rb_set_parent_color(child2, parent, RB_BLACK);
      rebalance = NULL;
    } else {
      unsigned long pc2 = successor->__rb_parent_color;
      successor->__rb_parent_color = pc;
      rebalance = __rb_is_black(pc2) ? parent : NULL;
    }
    tmp = successor;
  }
  return rebalance;
}

static inline void __rb_insert(RBNode *node, RBTree *root)
{
  RBNode *parent = rb_red_parent(node), *gparent, *tmp;

  while (true) {
    /*
     * Loop invariant: node is red
     *
     * If there is a black parent, we are done.
     * Otherwise, take some corrective action as we don't
     * want a red root or two consecutive red nodes.
     */
    if (!parent) {
      rb_set_parent_color(node, NULL, RB_BLACK);
      break;
    } else if (rb_is_black(parent))
      break;

    gparent = rb_red_parent(parent);

    tmp = gparent->rb_right;
    if (parent != tmp) {	/* parent == gparent->rb_left */
      if (tmp && rb_is_red(tmp)) {
        /*
         * Case 1 - color flips
         *
         *       G            g
         *      / \          / \
         *     p   u  -->   P   U
         *    /            /
         *   n            n
         *
         * However, since g's parent might be red, and
         * 4) does not allow this, we need to recurse
         * at g.
         */
        rb_set_parent_color(tmp, gparent, RB_BLACK);
        rb_set_parent_color(parent, gparent, RB_BLACK);
        node = gparent;
        parent = node->parent();
        rb_set_parent_color(node, parent, RB_RED);
        continue;
      }

      tmp = parent->rb_right;
      if (node == tmp) {
        /*
         * Case 2 - left rotate at parent
         *
         *      G             G
         *     / \           / \
         *    p   U  -->    n   U
         *     \           /
         *      n         p
         *
         * This still leaves us in violation of 4), the
         * continuation into Case 3 will fix that.
         */
        parent->rb_right = tmp = node->rb_left;
        node->rb_left = parent;
        if (tmp)
          rb_set_parent_color(tmp, parent,
                              RB_BLACK);
        rb_set_parent_color(parent, node, RB_RED);
        parent = node;
        tmp = node->rb_right;
      }

      /*
       * Case 3 - right rotate at gparent
       *
       *        G           P
       *       / \         / \
       *      p   U  -->  n   g
       *     /                 \
       *    n                   U
       */
      gparent->rb_left = tmp;  /* == parent->rb_right */
      parent->rb_right = gparent;
      if (tmp)
        rb_set_parent_color(tmp, gparent, RB_BLACK);
      __rb_rotate_set_parents(gparent, parent, root, RB_RED);
      break;
    } else {
      tmp = gparent->rb_left;
      if (tmp && rb_is_red(tmp)) {
        /* Case 1 - color flips */
        rb_set_parent_color(tmp, gparent, RB_BLACK);
        rb_set_parent_color(parent, gparent, RB_BLACK);
        node = gparent;
        parent = node->parent();
        rb_set_parent_color(node, parent, RB_RED);
        continue;
      }

      tmp = parent->rb_left;
      if (node == tmp) {
        /* Case 2 - right rotate at parent */
        parent->rb_left = tmp = node->rb_right;
        node->rb_right = parent;
        if (tmp)
          rb_set_parent_color(tmp, parent,
                              RB_BLACK);
        rb_set_parent_color(parent, node, RB_RED);
        parent = node;
        tmp = node->rb_left;
      }

      /* Case 3 - left rotate at gparent */
      gparent->rb_right = tmp;  /* == parent->rb_left */
      parent->rb_left = gparent;
      if (tmp)
        rb_set_parent_color(tmp, gparent, RB_BLACK);
      __rb_rotate_set_parents(gparent, parent, root, RB_RED);
      break;
    }
  }
}

/*
 * Inline version for rb_erase() use - we want to be able to inline
 * and eliminate the dummy_rotate callback there
 */
static inline void __rb_erase_color(RBNode *parent, RBTree *root)
{
  RBNode *node = NULL, *sibling, *tmp1, *tmp2;

  while (true) {
    /*
     * Loop invariants:
     * - node is black (or NULL on first iteration)
     * - node is not the root (parent is not NULL)
     * - All leaf paths going through parent and node have a
     *   black node count that is 1 lower than other leaf paths.
     */
    sibling = parent->rb_right;
    if (node != sibling) {	/* node == parent->rb_left */
      if (rb_is_red(sibling)) {
        /*
         * Case 1 - left rotate at parent
         *
         *     P               S
         *    / \             / \
         *   N   s    -->    p   Sr
         *      / \         / \
         *     Sl  Sr      N   Sl
         */
        parent->rb_right = tmp1 = sibling->rb_left;
        sibling->rb_left = parent;
        rb_set_parent_color(tmp1, parent, RB_BLACK);
        __rb_rotate_set_parents(parent, sibling, root,
                                RB_RED);
        sibling = tmp1;
      }
      tmp1 = sibling->rb_right;
      if (!tmp1 || rb_is_black(tmp1)) {
        tmp2 = sibling->rb_left;
        if (!tmp2 || rb_is_black(tmp2)) {
          /*
           * Case 2 - sibling color flip
           * (p could be either color here)
           *
           *    (p)           (p)
           *    / \           / \
           *   N   S    -->  N   s
           *      / \           / \
           *     Sl  Sr        Sl  Sr
           *
           * This leaves us violating 5) which
           * can be fixed by flipping p to black
           * if it was red, or by recursing at p.
           * p is red when coming from Case 1.
           */
          rb_set_parent_color(sibling, parent,
                              RB_RED);
          if (rb_is_red(parent))
            rb_set_black(parent);
          else {
            node = parent;
            parent = node->parent();
            if (parent)
              continue;
          }
          break;
        }
        /*
         * Case 3 - right rotate at sibling
         * (p could be either color here)
         *
         *   (p)           (p)
         *   / \           / \
         *  N   S    -->  N   Sl
         *     / \             \
         *    sl  Sr            s
         *                       \
         *                        Sr
         */
        sibling->rb_left = tmp1 = tmp2->rb_right;
        tmp2->rb_right = sibling;
        parent->rb_right = tmp2;
        if (tmp1)
          rb_set_parent_color(tmp1, sibling,
                              RB_BLACK);
        tmp1 = sibling;
        sibling = tmp2;
      }
      /*
       * Case 4 - left rotate at parent + color flips
       * (p and sl could be either color here.
       *  After rotation, p becomes black, s acquires
       *  p's color, and sl keeps its color)
       *
       *      (p)             (s)
       *      / \             / \
       *     N   S     -->   P   Sr
       *        / \         / \
       *      (sl) sr      N  (sl)
       */
      parent->rb_right = tmp2 = sibling->rb_left;
      sibling->rb_left = parent;
      rb_set_parent_color(tmp1, sibling, RB_BLACK);
      if (tmp2)
        rb_set_parent(tmp2, parent);
      __rb_rotate_set_parents(parent, sibling, root,
                              RB_BLACK);
      break;
    } else {
      sibling = parent->rb_left;
      if (rb_is_red(sibling)) {
        /* Case 1 - right rotate at parent */
        parent->rb_left = tmp1 = sibling->rb_right;
        sibling->rb_right = parent;
        rb_set_parent_color(tmp1, parent, RB_BLACK);
        __rb_rotate_set_parents(parent, sibling, root,
                                RB_RED);
        sibling = tmp1;
      }
      tmp1 = sibling->rb_left;
      if (!tmp1 || rb_is_black(tmp1)) {
        tmp2 = sibling->rb_right;
        if (!tmp2 || rb_is_black(tmp2)) {
          /* Case 2 - sibling color flip */
          rb_set_parent_color(sibling, parent,
                              RB_RED);
          if (rb_is_red(parent))
            rb_set_black(parent);
          else {
            node = parent;
            parent = node->parent();
            if (parent)
              continue;
          }
          break;
        }
        /* Case 3 - right rotate at sibling */
        sibling->rb_right = tmp1 = tmp2->rb_left;
        tmp2->rb_left = sibling;
        parent->rb_left = tmp2;
        if (tmp1)
          rb_set_parent_color(tmp1, sibling,
                              RB_BLACK);
        tmp1 = sibling;
        sibling = tmp2;
      }
      /* Case 4 - left rotate at parent + color flips */
      parent->rb_left = tmp2 = sibling->rb_right;
      sibling->rb_right = parent;
      rb_set_parent_color(tmp1, sibling, RB_BLACK);
      if (tmp2)
        rb_set_parent(tmp2, parent);
      __rb_rotate_set_parents(parent, sibling, root,
                              RB_BLACK);
      break;
    }
  }
}

void RBTree::insert_color(RBNode *node)
{
  __rb_insert(node, this);
}

void RBTree::erase(RBNode *node)
{
  RBNode *rebalance = __rb_erase(node, this);
  if (rebalance)
    __rb_erase_color(rebalance, this);
}

void RBTree::replace(RBNode *victim, RBNode *n)
{
  RBNode *parent = victim->parent();

  /* Set the surrounding nodes to point to the replacement */
  __rb_change_child(victim, n, parent, this);
  if (victim->rb_left)
    rb_set_parent(victim->rb_left, n);
  if (victim->rb_right)
    rb_set_parent(victim->rb_right, n);

  /* Copy the pointers/colour from the victim to the replacement */
  *n = *victim;
}
