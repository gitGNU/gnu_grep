/* kwset.c - search for any of a set of keywords.
   Copyright (C) 1989, 1998, 2000, 2005, 2007, 2009-2017 Free Software
   Foundation, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street - Fifth Floor, Boston, MA
   02110-1301, USA.  */

/* Written August 1989 by Mike Haertel.  */

/* The algorithm called "Commentz_Walter" below bears a startling resemblance
   to that of Beate Commentz-Walter, although it is not identical.
   See: Commentz-Walter B. A string matching algorithm fast on the average.
   Lecture Notes in Computer Science 71 (1979), 118-32
   <http://dx.doi.org/10.1007/3-540-09510-1_10>.

   For the Aho-Corasick algorithm, see:
   Aho AV, Corasick MJ. Efficient string matching: an aid to
   bibliographic search. CACM 18, 6 (1975), 333-40
   <http://dx.doi.org/10.1145/360825.360855>, which describes the
   failure function used below.

   For the Boyer-Moore algorithm, see: Boyer RS, Moore JS.
   A fast string searching algorithm. CACM 20, 10 (1977), 762-72
   <http://dx.doi.org/10.1145/359842.359859>.

   For a survey of more-recent string matching algorithms that might
   help improve performance, see: Faro S, Lecroq T. The exact online
   string matching problem: a review of the most recent results.
   ACM Computing Surveys 45, 2 (2013), 13
   <http://dx.doi.org/10.1145/2431211.2431212>.  */

#include <config.h>

#include "kwset.h"

#include <stdint.h>
#include <sys/types.h>
#include "system.h"
#include "intprops.h"
#include "memchr2.h"
#include "obstack.h"
#include "xalloc.h"

#define obstack_chunk_alloc xmalloc
#define obstack_chunk_free free

static unsigned char
U (char ch)
{
  return to_uchar (ch);
}

/* Balanced tree of edges and labels leaving a given trie node.  */
struct tree
{
  struct tree *llink;		/* Left link; MUST be first field.  */
  struct tree *rlink;		/* Right link (to larger labels).  */
  struct trie *trie;		/* Trie node pointed to by this edge.  */
  unsigned char label;		/* Label on this edge.  */
  char balance;			/* Difference in depths of subtrees.  */
};

/* Node of a trie representing a set of keywords.  */
struct trie
{
  /* If an accepting node, this is either 2*W + 1 where W is the word
     index, or is SIZE_MAX if Commentz-Walter is in use and FAIL
     specifies where to look for more info.  If not an accepting node,
     this is zero.  */
  size_t accepting;

  struct tree *links;		/* Tree of edges leaving this node.  */
  struct trie *parent;		/* Parent of this node.  */
  struct trie *next;		/* List of all trie nodes in level order.  */
  struct trie *fail;		/* Aho-Corasick failure function.  */
  ptrdiff_t depth;		/* Depth of this node from the root.  */
  ptrdiff_t shift;		/* Shift function for search failures.  */
  ptrdiff_t maxshift;		/* Max shift of self and descendants.  */
};

/* Structure returned opaquely to the caller, containing everything.  */
struct kwset
{
  struct obstack obstack;	/* Obstack for node allocation.  */
  ptrdiff_t words;		/* Number of words in the trie.  */
  struct trie *trie;		/* The trie itself.  */
  ptrdiff_t mind;		/* Minimum depth of an accepting node.  */
  ptrdiff_t maxd;		/* Maximum depth of any node.  */
  unsigned char delta[NCHAR];	/* Delta table for rapid search.  */
  struct trie *next[NCHAR];	/* Table of children of the root.  */
  char *target;			/* Target string if there's only one.  */
  ptrdiff_t *shift;		/* Used in Boyer-Moore search for one string.  */
  char const *trans;		/* Character translation table.  */

  /* If there's only one string, this is the string's last byte,
     translated via TRANS if TRANS is nonnull.  */
  char gc1;

  /* Likewise for the string's penultimate byte, if it has two or more
     bytes.  */
  char gc2;

  /* If there's only one string, this helps to match the string's last byte.
     If GC1HELP is negative, only GC1 matches the string's last byte;
     otherwise at least two bytes match, and B matches if TRANS[B] == GC1.
     If GC1HELP is in the range 0..(NCHAR - 1), there are exactly two
     such matches, and GC1HELP is the other match after conversion to
     unsigned char.  If GC1HELP is at least NCHAR, there are three or
     more such matches; e.g., Greek has three sigma characters that
     all match when case-folding.  */
  int gc1help;

  /* If true, prefer Aho-Corasick to Commentz-Walter when searching
     for multiple words.  */
  bool reverse;

  /* kwsexec implementation.  */
  size_t (*kwsexec) (kwset_t, char const *, size_t, struct kwsmatch *, bool);
};

/* Use TRANS to transliterate C.  A null TRANS does no transliteration.  */
static inline char
tr (char const *trans, char c)
{
  return trans ? trans[U(c)] : c;
}

static size_t acexec (kwset_t, char const *, size_t, struct kwsmatch *, bool);
static size_t cwexec (kwset_t, char const *, size_t, struct kwsmatch *, bool);
static size_t bmexec (kwset_t, char const *, size_t, struct kwsmatch *, bool);

/* Return a newly allocated keyword set.  A nonnull TRANS specifies a
   table of character translations to be applied to all pattern and
   search text.  If REVERSE, prefer the Aho-Corasick algorithm to the
   Commentz-Walter algorithm.  */
kwset_t
kwsalloc (char const *trans, bool reverse)
{
  struct kwset *kwset = xmalloc (sizeof *kwset);

  obstack_init (&kwset->obstack);
  kwset->words = 0;
  kwset->trie = obstack_alloc (&kwset->obstack, sizeof *kwset->trie);
  kwset->trie->accepting = 0;
  kwset->trie->links = NULL;
  kwset->trie->parent = NULL;
  kwset->trie->next = NULL;
  kwset->trie->fail = NULL;
  kwset->trie->depth = 0;
  kwset->trie->shift = 0;
  kwset->mind = PTRDIFF_MAX;
  kwset->maxd = -1;
  kwset->target = NULL;
  kwset->trans = trans;
  kwset->reverse = reverse;
  kwset->kwsexec = reverse ? cwexec : acexec;

  return kwset;
}

/* This upper bound is valid for CHAR_BIT >= 4 and
   exact for CHAR_BIT in { 4..11, 13, 15, 17, 19 }.  */
enum { DEPTH_SIZE = CHAR_BIT + CHAR_BIT / 2 };

/* Add the given string to the contents of the keyword set.  */
void
kwsincr (kwset_t kwset, char const *text, size_t len)
{
  struct trie *trie = kwset->trie;
  char const *trans = kwset->trans;

  if (kwset->reverse)
    text += len;

  /* Descend the trie (built of keywords) character-by-character,
     installing new nodes when necessary.  */
  while (len--)
    {
      unsigned char uc = kwset->reverse ? *--text : *text++;
      unsigned char label = trans ? trans[uc] : uc;

      /* Descend the tree of outgoing links for this trie node,
         looking for the current character and keeping track
         of the path followed.  */
      struct tree *cur = trie->links;
      struct tree *links[DEPTH_SIZE];
      enum { L, R } dirs[DEPTH_SIZE];
      links[0] = (struct tree *) &trie->links;
      dirs[0] = L;
      ptrdiff_t depth = 1;

      while (cur && label != cur->label)
        {
          links[depth] = cur;
          if (label < cur->label)
            dirs[depth++] = L, cur = cur->llink;
          else
            dirs[depth++] = R, cur = cur->rlink;
        }

      /* The current character doesn't have an outgoing link at
         this trie node, so build a new trie node and install
         a link in the current trie node's tree.  */
      if (!cur)
        {
          cur = obstack_alloc (&kwset->obstack, sizeof *cur);
          cur->llink = NULL;
          cur->rlink = NULL;
          cur->trie = obstack_alloc (&kwset->obstack, sizeof *cur->trie);
          cur->trie->accepting = 0;
          cur->trie->links = NULL;
          cur->trie->parent = trie;
          cur->trie->next = NULL;
          cur->trie->fail = NULL;
          cur->trie->depth = trie->depth + 1;
          cur->trie->shift = 0;
          cur->label = label;
          cur->balance = 0;

          /* Install the new tree node in its parent.  */
          if (dirs[--depth] == L)
            links[depth]->llink = cur;
          else
            links[depth]->rlink = cur;

          /* Back up the tree fixing the balance flags.  */
          while (depth && !links[depth]->balance)
            {
              if (dirs[depth] == L)
                --links[depth]->balance;
              else
                ++links[depth]->balance;
              --depth;
            }

          /* Rebalance the tree by pointer rotations if necessary.  */
          if (depth && ((dirs[depth] == L && --links[depth]->balance)
                        || (dirs[depth] == R && ++links[depth]->balance)))
            {
              struct tree *t, *r, *l, *rl, *lr;

              switch (links[depth]->balance)
                {
                case (char) -2:
                  switch (dirs[depth + 1])
                    {
                    case L:
                      r = links[depth], t = r->llink, rl = t->rlink;
                      t->rlink = r, r->llink = rl;
                      t->balance = r->balance = 0;
                      break;
                    case R:
                      r = links[depth], l = r->llink, t = l->rlink;
                      rl = t->rlink, lr = t->llink;
                      t->llink = l, l->rlink = lr, t->rlink = r, r->llink = rl;
                      l->balance = t->balance != 1 ? 0 : -1;
                      r->balance = t->balance != (char) -1 ? 0 : 1;
                      t->balance = 0;
                      break;
                    default:
                      abort ();
                    }
                  break;
                case 2:
                  switch (dirs[depth + 1])
                    {
                    case R:
                      l = links[depth], t = l->rlink, lr = t->llink;
                      t->llink = l, l->rlink = lr;
                      t->balance = l->balance = 0;
                      break;
                    case L:
                      l = links[depth], r = l->rlink, t = r->llink;
                      lr = t->llink, rl = t->rlink;
                      t->llink = l, l->rlink = lr, t->rlink = r, r->llink = rl;
                      l->balance = t->balance != 1 ? 0 : -1;
                      r->balance = t->balance != (char) -1 ? 0 : 1;
                      t->balance = 0;
                      break;
                    default:
                      abort ();
                    }
                  break;
                default:
                  abort ();
                }

              if (dirs[depth - 1] == L)
                links[depth - 1]->llink = t;
              else
                links[depth - 1]->rlink = t;
            }
        }

      trie = cur->trie;
    }

  /* Mark the node finally reached as accepting, encoding the
     index number of this word in the keyword set so far.  */
  if (!trie->accepting)
    trie->accepting = 1 + 2 * kwset->words;
  ++kwset->words;

  /* Keep track of the longest and shortest string of the keyword set.  */
  if (trie->depth < kwset->mind)
    kwset->mind = trie->depth;
  if (trie->depth > kwset->maxd)
    kwset->maxd = trie->depth;
}

/* Enqueue the trie nodes referenced from the given tree in the
   given queue.  */
static void
enqueue (struct tree *tree, struct trie **last)
{
  if (!tree)
    return;
  enqueue (tree->llink, last);
  enqueue (tree->rlink, last);
  (*last) = (*last)->next = tree->trie;
}

/* Compute the Aho-Corasick failure function for the trie nodes referenced
   from the given tree, given the failure function for their parent as
   well as a last resort failure node.  */
static void
treefails (struct tree const *tree, struct trie const *fail,
           struct trie *recourse, bool reverse)
{
  struct tree *cur;

  if (!tree)
    return;

  treefails (tree->llink, fail, recourse, reverse);
  treefails (tree->rlink, fail, recourse, reverse);

  /* Find, in the chain of fails going back to the root, the first
     node that has a descendant on the current label.  */
  while (fail)
    {
      cur = fail->links;
      while (cur && tree->label != cur->label)
        if (tree->label < cur->label)
          cur = cur->llink;
        else
          cur = cur->rlink;
      if (cur)
        {
          tree->trie->fail = cur->trie;
          if (!reverse && cur->trie->accepting && !tree->trie->accepting)
            tree->trie->accepting = SIZE_MAX;
          return;
        }
      fail = fail->fail;
    }

  tree->trie->fail = recourse;
}

/* Set delta entries for the links of the given tree such that
   the preexisting delta value is larger than the current depth.  */
static void
treedelta (struct tree const *tree, ptrdiff_t depth, unsigned char delta[])
{
  if (!tree)
    return;
  treedelta (tree->llink, depth, delta);
  treedelta (tree->rlink, depth, delta);
  if (depth < delta[tree->label])
    delta[tree->label] = depth;
}

/* Return true if A has every label in B.  */
static bool _GL_ATTRIBUTE_PURE
hasevery (struct tree const *a, struct tree const *b)
{
  if (!b)
    return true;
  if (!hasevery (a, b->llink))
    return false;
  if (!hasevery (a, b->rlink))
    return false;
  while (a && b->label != a->label)
    if (b->label < a->label)
      a = a->llink;
    else
      a = a->rlink;
  return !!a;
}

/* Compute a vector, indexed by character code, of the trie nodes
   referenced from the given tree.  */
static void
treenext (struct tree const *tree, struct trie *next[])
{
  if (!tree)
    return;
  treenext (tree->llink, next);
  treenext (tree->rlink, next);
  next[tree->label] = tree->trie;
}

/* Prepare a built keyword set for use.  */
void
kwsprep (kwset_t kwset)
{
  char const *trans = kwset->trans;
  ptrdiff_t i;
  unsigned char deltabuf[NCHAR];
  unsigned char *delta = trans ? deltabuf : kwset->delta;
  struct trie *curr, *last;

  if (kwset->words == 1)
    {
      if (!kwset->reverse)
        {
          kwset_t new_kwset;

          /* Enqueue the immediate descendants in the level order queue.  */
          for (curr = last = kwset->trie; curr; curr = curr->next)
             enqueue (curr->links, &last);

          /* Looking for just one string.  Extract it from the trie.  */
          kwset->target = obstack_alloc (&kwset->obstack, kwset->mind);
          for (i = 0, curr = kwset->trie; i < kwset->mind; ++i)
            {
              kwset->target[i] = curr->links->label;
              curr = curr->next;
            }

          new_kwset = kwsalloc (kwset->trans, true);
          kwsincr (new_kwset, kwset->target, kwset->mind);
          obstack_free (&kwset->obstack, NULL);
          *kwset = *new_kwset;
          free (new_kwset);
        }
      kwset->kwsexec = bmexec;
    }

  /* Initial values for the delta table; will be changed later.  The
     delta entry for a given character is the smallest depth of any
     node at which an outgoing edge is labeled by that character.  */
  memset (delta, MIN (kwset->mind, UCHAR_MAX), sizeof deltabuf);

  /* Traverse the nodes of the trie in level order, simultaneously
     computing the delta table, failure function, and shift function.  */
  for (curr = last = kwset->trie; curr; curr = curr->next)
    {
      /* Enqueue the immediate descendants in the level order queue.  */
      enqueue (curr->links, &last);

      /* Update the delta table for the descendants of this node.  */
      treedelta (curr->links, curr->depth, delta);

      /* Compute the failure function for the descendants of this node.  */
      treefails (curr->links, curr->fail, kwset->trie, kwset->reverse);

      if (kwset->reverse)
        {
          curr->shift = kwset->mind;
          curr->maxshift = kwset->mind;

          /* Update the shifts at each node in the current node's chain
             of fails back to the root.  */
          struct trie *fail;
          for (fail = curr->fail; fail; fail = fail->fail)
            {
              /* If the current node has some outgoing edge that the fail
                 doesn't, then the shift at the fail should be no larger
                 than the difference of their depths.  */
              if (!hasevery (fail->links, curr->links))
                if (curr->depth - fail->depth < fail->shift)
                  fail->shift = curr->depth - fail->depth;

              /* If the current node is accepting then the shift at the
                 fail and its descendants should be no larger than the
                 difference of their depths.  */
              if (curr->accepting && fail->maxshift > curr->depth - fail->depth)
                fail->maxshift = curr->depth - fail->depth;
            }
        }
    }

  if (kwset->reverse)
    {
      /* Traverse the trie in level order again, fixing up all nodes whose
         shift exceeds their inherited maxshift.  */
      for (curr = kwset->trie->next; curr; curr = curr->next)
        {
          if (curr->maxshift > curr->parent->maxshift)
            curr->maxshift = curr->parent->maxshift;
          if (curr->shift > curr->maxshift)
            curr->shift = curr->maxshift;
        }
    }

  /* Create a vector, indexed by character code, of the outgoing links
     from the root node.  */
  struct trie *nextbuf[NCHAR];
  struct trie **next = trans ? nextbuf : kwset->next;
  memset (next, 0, sizeof nextbuf);
  treenext (kwset->trie->links, next);
  if (trans)
    for (i = 0; i < NCHAR; ++i)
      kwset->next[i] = next[U(trans[i])];

  /* Decide whether to use the simple Boyer-Moore algorithm, instead
     of the hairy Commentz-Walter algorithm.  */
  if (kwset->words == 1)
    {
      /* Looking for just one string.  Extract it from the trie.  */
      kwset->target = obstack_alloc (&kwset->obstack, kwset->mind);
      for (i = kwset->mind - 1, curr = kwset->trie; i >= 0; --i)
        {
          kwset->target[i] = curr->links->label;
          curr = curr->next;
        }

      /* Looking for the delta2 shift that might be made after a
         backwards match has failed.  Extract it from the trie.  */
      if (kwset->mind > 1)
        {
          kwset->shift
            = obstack_alloc (&kwset->obstack,
                             sizeof *kwset->shift * (kwset->mind - 1));
          for (i = 0, curr = kwset->trie->next; i < kwset->mind - 1; ++i)
            {
              kwset->shift[i] = curr->shift;
              curr = curr->next;
            }
        }

      char gc1 = tr (trans, kwset->target[kwset->mind - 1]);

      /* Set GC1HELP according to whether exactly one, exactly two, or
         three-or-more characters match GC1.  */
      int gc1help = -1;
      if (trans)
        {
          char const *equiv1 = memchr (trans, gc1, NCHAR);
          char const *equiv2 = memchr (equiv1 + 1, gc1,
                                       trans + NCHAR - (equiv1 + 1));
          if (equiv2)
            gc1help = (memchr (equiv2 + 1, gc1, trans + NCHAR - (equiv2 + 1))
                       ? NCHAR
                       : U(gc1) ^ (equiv1 - trans) ^ (equiv2 - trans));
        }

      kwset->gc1 = gc1;
      kwset->gc1help = gc1help;
      if (kwset->mind > 1)
        kwset->gc2 = tr (trans, kwset->target[kwset->mind - 2]);
    }

  /* Fix things up for any translation table.  */
  if (trans)
    for (i = 0; i < NCHAR; ++i)
      kwset->delta[i] = delta[U(trans[i])];
}

/* Delta2 portion of a Boyer-Moore search.  *TP is the string text
   pointer; it is updated in place.  EP is the end of the string text,
   and SP the end of the pattern.  LEN is the pattern length; it must
   be at least 2.  TRANS, if nonnull, is the input translation table.
   GC1 and GC2 are the last and second-from last bytes of the pattern,
   transliterated by TRANS; the caller precomputes them for
   efficiency.  If D1 is nonnull, it is a delta1 table for shifting *TP
   when failing.  KWSET->shift says how much to shift.  */
static inline bool
bm_delta2_search (char const **tpp, char const *ep, char const *sp,
                  ptrdiff_t len,
                  char const *trans, char gc1, char gc2,
                  unsigned char const *d1, kwset_t kwset)
{
  char const *tp = *tpp;
  ptrdiff_t d = len, skip = 0;

  while (true)
    {
      ptrdiff_t i = 2;
      if (tr (trans, tp[-2]) == gc2)
        {
          while (++i <= d)
            if (tr (trans, tp[-i]) != tr (trans, sp[-i]))
              break;
          if (i > d)
            {
              for (i = d + skip + 1; i <= len; ++i)
                if (tr (trans, tp[-i]) != tr (trans, sp[-i]))
                  break;
              if (i > len)
                {
                  *tpp = tp - len;
                  return true;
                }
            }
        }

      tp += d = kwset->shift[i - 2];
      if (tp > ep)
        break;
      if (tr (trans, tp[-1]) != gc1)
        {
          if (d1)
            tp += d1[U(tp[-1])];
          break;
        }
      skip = i - 1;
    }

  *tpp = tp;
  return false;
}

/* Return the address of the first byte in the buffer S (of size N)
   that matches the last byte specified by KWSET, a singleton.
   Return NULL if there is no match.  */
static char const *
memchr_kwset (char const *s, size_t n, kwset_t kwset)
{
  if (kwset->gc1help < 0)
    return memchr (s, kwset->gc1, n);
  int small_heuristic = 2;
  int small = (- (uintptr_t) s % sizeof (long)
               + small_heuristic * sizeof (long));
  size_t ntrans = kwset->gc1help < NCHAR && small < n ? small : n;
  char const *slim = s + ntrans;
  for (; s < slim; s++)
    if (kwset->trans[U(*s)] == kwset->gc1)
      return s;
  n -= ntrans;
  return n == 0 ? NULL : memchr2 (s, kwset->gc1, kwset->gc1help, n);
}

/* Return the offset of the first byte in the buffer S (of size N)
   that matches the last byte specified by KWSET, a pair.
   Return SIZE_MAX if there is no match.  */
static size_t
memoff2_kwset (char const *s, size_t n, kwset_t kwset,
               struct kwsmatch *kwsmatch)
{
  struct tree const *cur = kwset->trie->links;
  struct tree const *clink = cur->llink ? cur->llink : cur->rlink;
  char const *mch = (clink
                     ? memchr2 (s, cur->label, clink->label, n)
                     : memchr (s, cur->label, n));
  if (! mch)
    return SIZE_MAX;
  else
    {
      size_t off = mch - s;
      if (*mch == cur->label)
        kwsmatch->index = cur->trie->accepting / 2;
      else
        kwsmatch->index = clink->trie->accepting / 2;
      kwsmatch->offset[0] = off;
      kwsmatch->size[0] = 1;
      return off;
    }
}

/* Fast Boyer-Moore search (inlinable version).  */
static inline size_t
bmexec_trans (kwset_t kwset, char const *text, size_t size)
{
  unsigned char const *d1;
  char const *ep, *sp, *tp;
  int d;
  ptrdiff_t len = kwset->mind;
  char const *trans = kwset->trans;

  if (len == 0)
    return 0;
  if (len > size)
    return SIZE_MAX;
  if (len == 1)
    {
      tp = memchr_kwset (text, size, kwset);
      return tp ? tp - text : SIZE_MAX;
    }

  d1 = kwset->delta;
  sp = kwset->target + len;
  tp = text + len;
  char gc1 = kwset->gc1;
  char gc2 = kwset->gc2;

  /* Significance of 12: 1 (initial offset) + 10 (skip loop) + 1 (md2).  */
  ptrdiff_t len12;
  if (!INT_MULTIPLY_WRAPV (len, 12, &len12) && len12 < size)
    /* 11 is not a bug, the initial offset happens only once.  */
    for (ep = text + size - 11 * len; tp <= ep; )
      {
        char const *tp0 = tp;
        d = d1[U(tp[-1])], tp += d;
        d = d1[U(tp[-1])], tp += d;
        if (d != 0)
          {
            d = d1[U(tp[-1])], tp += d;
            d = d1[U(tp[-1])], tp += d;
            d = d1[U(tp[-1])], tp += d;
            if (d != 0)
              {
                d = d1[U(tp[-1])], tp += d;
                d = d1[U(tp[-1])], tp += d;
                d = d1[U(tp[-1])], tp += d;
                if (d != 0)
                  {
                    d = d1[U(tp[-1])], tp += d;
                    d = d1[U(tp[-1])], tp += d;

                    /* As a heuristic, prefer memchr to seeking by
                       delta1 when the latter doesn't advance much.  */
                    int advance_heuristic = 16 * sizeof (long);
                    if (advance_heuristic <= tp - tp0)
                      continue;
                    tp--;
                    tp = memchr_kwset (tp, text + size - tp, kwset);
                    if (! tp)
                      return SIZE_MAX;
                    tp++;
                    if (ep <= tp)
                      break;
                  }
              }
          }
        if (bm_delta2_search (&tp, ep, sp, len, trans, gc1, gc2, d1, kwset))
          return tp - text;
      }

  /* Now only a few characters are left to search.  Carefully avoid
     ever producing an out-of-bounds pointer.  */
  ep = text + size;
  d = d1[U(tp[-1])];
  while (d <= ep - tp)
    {
      d = d1[U((tp += d)[-1])];
      if (d != 0)
        continue;
      if (bm_delta2_search (&tp, ep, sp, len, trans, gc1, gc2, NULL, kwset))
        return tp - text;
    }

  return SIZE_MAX;
}

/* Fast Boyer-Moore search.  */
static size_t
bmexec (kwset_t kwset, char const *text, size_t size,
        struct kwsmatch *kwsmatch, bool longest)
{
  /* Help the compiler inline in two ways, depending on whether
     kwset->trans is null.  */
  size_t ret = (kwset->trans
                ? bmexec_trans (kwset, text, size)
                : bmexec_trans (kwset, text, size));

  if (ret != SIZE_MAX)
    {
       kwsmatch->index = 0;
       kwsmatch->offset[0] = ret;
       kwsmatch->size[0] = kwset->mind;
    }

  return ret;
}

/* Hairy multiple string search with the Commentz-Walter algorithm.  */
static size_t
cwexec (kwset_t kwset, char const *text, size_t len,
        struct kwsmatch *kwsmatch, bool longest)
{
  struct trie * const *next;
  struct trie const *trie;
  struct trie const *accept;
  char const *beg, *lim, *mch, *lmch;
  unsigned char c;
  unsigned char const *delta;
  ptrdiff_t d;
  char const *end, *qlim;
  struct tree const *tree;
  char const *trans;

  /* Initialize register copies and look for easy ways out.  */
  if (len < kwset->mind)
    return SIZE_MAX;
  if (!kwset->trans && kwset->maxd == 1 && kwset->words == 2)
    return memoff2_kwset (text, len, kwset, kwsmatch);
  next = kwset->next;
  delta = kwset->delta;
  trans = kwset->trans;
  lim = text + len;
  end = text;
  if ((d = kwset->mind) != 0)
    mch = NULL;
  else
    {
      mch = text, accept = kwset->trie;
      goto match;
    }

  if (len >= 4 * kwset->mind)
    qlim = lim - 4 * kwset->mind;
  else
    qlim = NULL;

  while (lim - end >= d)
    {
      if (qlim && end <= qlim)
        {
          end += d - 1;
          while ((d = delta[c = *end]) && end < qlim)
            {
              end += d;
              end += delta[U(*end)];
              end += delta[U(*end)];
            }
          ++end;
        }
      else
        d = delta[c = (end += d)[-1]];
      if (d)
        continue;
      beg = end - 1;
      trie = next[c];
      if (trie->accepting)
        {
          mch = beg;
          accept = trie;
        }
      d = trie->shift;
      while (beg > text)
        {
          unsigned char uc = *--beg;
          c = trans ? trans[uc] : uc;
          tree = trie->links;
          while (tree && c != tree->label)
            if (c < tree->label)
              tree = tree->llink;
            else
              tree = tree->rlink;
          if (tree)
            {
              trie = tree->trie;
              if (trie->accepting)
                {
                  mch = beg;
                  accept = trie;
                }
            }
          else
            break;
          d = trie->shift;
        }
      if (mch)
        goto match;
    }
  return SIZE_MAX;

 match:
  /* Given a known match, find the longest possible match anchored
     at or before its starting point.  This is nearly a verbatim
     copy of the preceding main search loops.  */
  if (longest)
    {
      if (lim - mch > kwset->maxd)
        lim = mch + kwset->maxd;
      lmch = 0;
      d = 1;
      while (lim - end >= d)
        {
          if ((d = delta[c = (end += d)[-1]]) != 0)
            continue;
          beg = end - 1;
          if (!(trie = next[c]))
            {
              d = 1;
              continue;
            }
          if (trie->accepting && beg <= mch)
            {
              lmch = beg;
              accept = trie;
            }
          d = trie->shift;
          while (beg > text)
            {
              unsigned char uc = *--beg;
              c = trans ? trans[uc] : uc;
              tree = trie->links;
              while (tree && c != tree->label)
                if (c < tree->label)
                  tree = tree->llink;
                else
                  tree = tree->rlink;
              if (tree)
                {
                  trie = tree->trie;
                  if (trie->accepting && beg <= mch)
                    {
                      lmch = beg;
                      accept = trie;
                    }
                }
              else
                break;
              d = trie->shift;
            }
          if (lmch)
            {
              mch = lmch;
              goto match;
            }
          if (!d)
            d = 1;
        }
    }

  kwsmatch->index = accept->accepting / 2;
  kwsmatch->offset[0] = mch - text;
  kwsmatch->size[0] = accept->depth;

  return mch - text;
}

/* Hairy multiple string search with the Aho-Corasick algorithm.
   (inlinable version)  */
static inline size_t
acexec_trans (kwset_t kwset, char const *text, size_t len,
              struct kwsmatch *kwsmatch, bool longest)
{
  struct trie * const *next;
  struct trie const *trie, *accept;
  char const *tp, *left, *lim;
  struct tree const *tree;
  char const *trans;

  /* Initialize register copies and look for easy ways out.  */
  if (len < kwset->mind)
    return SIZE_MAX;
  trans = kwset->trans;
  if (!trans && kwset->maxd == 1 && kwset->words == 2)
    return memoff2_kwset (text, len, kwset, kwsmatch);

  next = kwset->next;
  trie = kwset->trie;
  lim = text + len;
  tp = text;

  if (!trie->accepting)
    {
      unsigned char c = tr (trans, *tp++);

      while (true)
        {
          while (! (trie = next[c]))
            {
              if (tp >= lim)
                return SIZE_MAX;
              c = tr (trans, *tp++);
            }

          while (true)
            {
              if (trie->accepting)
                goto match;
              if (tp >= lim)
                return SIZE_MAX;
              c = tr (trans, *tp++);

              for (tree = trie->links; c != tree->label; )
                {
                  tree = c < tree->label ? tree->llink : tree->rlink;
                  if (! tree)
                    {
                      trie = trie->fail;
                      if (!trie)
                        goto next_trie;
                      if (trie->accepting)
                        {
                          --tp;
                          goto match;
                        }
                      tree = trie->links;
                    }
                }
              trie = tree->trie;
            }
        next_trie:;
        }
    }

 match:
  accept = trie;
  while (accept->accepting == SIZE_MAX)
    accept = accept->fail;
  left = tp - accept->depth;

  /* Try left-most longest match.  */
  if (longest)
    {
      while (tp < lim)
        {
          struct trie const *accept1;
          char const *left1;
          unsigned char c = tr (trans, *tp++);
          tree = trie->links;
          while (tree && c != tree->label)
            tree = c < tree->label ? tree->llink : tree->rlink;
          if (!tree)
            break;
          trie = tree->trie;
          if (trie->accepting)
            {
              accept1 = trie;
              while (accept1->accepting == SIZE_MAX)
                accept1 = accept1->fail;
              left1 = tp - accept1->depth;
              if (left1 <= left)
                {
                  left = left1;
                  accept = accept1;
                }
            }
        }
    }

  kwsmatch->index = accept->accepting / 2;
  kwsmatch->offset[0] = left - text;
  kwsmatch->size[0] = accept->depth;

  return left - text;
}

/* Hairy multiple string search with Aho-Corasick algorithm.  */
static size_t
acexec (kwset_t kwset, char const *text, size_t size,
        struct kwsmatch *kwsmatch, bool longest)
{
  /* Help the compiler inline in two ways, depending on whether
     kwset->trans is null.  */
  return (kwset->trans
          ? acexec_trans (kwset, text, size, kwsmatch, longest)
          : acexec_trans (kwset, text, size, kwsmatch, longest));
}

/* Find the first instance of a KWSET member in TEXT, which has SIZE bytes.
   Return the offset (into TEXT) of the first byte of the matching substring,
   or SIZE_MAX if no match is found.  Upon a match, store details in
   *KWSMATCH: index of matched keyword, start offset (same as the return
   value), and length.  If LONGEST, find the longest match; otherwise
   any match will do.  */
size_t
kwsexec (kwset_t kwset, char const *text, size_t size,
         struct kwsmatch *kwsmatch, bool longest)
{
  return kwset->kwsexec (kwset, text, size, kwsmatch, longest);
}

/* Free the components of the given keyword set.  */
void
kwsfree (kwset_t kwset)
{
  obstack_free (&kwset->obstack, NULL);
  free (kwset);
}
