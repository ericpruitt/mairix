/***************************************
  Routines for compressing the DFA by commoning-up equivalent states
  ***************************************/

/*
 **********************************************************************
 * Copyright (C) Richard P. Curnow  2001-2003,2005,2006
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 **********************************************************************
 */

/*
  The input to this stage is the 'raw' DFA build from the NFA by the subset
  construction.  Depending on the style of the NFA, there may be large chunks
  of the DFA that have equivalent functionality, in terms of resulting in the
  same attributes for the same sequence of input tokens, but which are reached
  by different prefixes.  The idea of this stage is to common up such regions,
  to reduce the size of the DFA and hence the table sizes that are generated.

  Conceptually, the basis of the algorithm is to assign the DFA states to
  equivalence classes.  If there are N different tags-combinations, there are
  initially N+1 classes.  All states that can exit with a particular value are
  placed in a class together, and all non-accepting states are placed together.
  Now, a pass is made over all pairs of states.  Two states remain equivalent
  if for each token, their outbound transitions go to states in the same class.
  If the states do not stay equivalent, the class they were in is split
  accordingly.  This is repeated again and again until no more bisections
  occur.

  The algorithm actually used is to assign an ordering to the states based on
  their current class and outbound transitions.  The states are then sorted.
  This allows all checking to be done on near-neighbours in the sequence
  generated by the sort, which brings the execution time down to something
  finite.

  */

#include "dfasyn.h"

static int last_eq_class; /* Next class to assign */
static int Nt; /* Number of tokens; has to be made static to be visible to comparison fn. */

/* To give 'general_compre' visibility of the current equiv. classes of the
   destination states */
static DFANode **local_dfas;

static void calculate_signatures(DFANode **seq, DFANode **dfas, int ndfas)/*{{{*/
/**** Determine state signatures based on transitions and current classes. ****/
{
  unsigned long sig;
  int i, t;

  for (i=0; i<ndfas; i++) {
    DFANode *s = seq[i];
    sig = 0UL;
    for (t=0; t<Nt; t++) {
      int di = s->map[t];
      if (di >= 0) {
        DFANode *d = dfas[di];
        int deq_class = d->eq_class;

        sig = increment(sig, deq_class & 0xf); /* 16 bit pairs in sig */
      }
    }

    s->signature = sig;
  }
}
/*}}}*/
static int general_compare(const void *a, const void *b)/*{{{*/
/************************* Do full compare on states *************************/
{
  Castderef (a, const DFANode *, aa);
  Castderef (b, const DFANode *, bb);

  if (aa->eq_class < bb->eq_class) {
    return -1;
  } else if (aa->eq_class > bb->eq_class) {
    return +1;
  } else if (aa->signature < bb->signature) {
    return -1;
  } else if (aa->signature > bb->signature) {
    return +1;
  } else {
    /* The hard way... */
    int i;
    for (i=0; i<Nt; i++) {
      int am = aa->map[i];
      int bm = bb->map[i];

      /* Map transition destinations to the current equivalence class of the
         destination state (otherwise compressor is very pessimistic). */
      am = (am>=0) ? local_dfas[am]->eq_class: -1;
      bm = (bm>=0) ? local_dfas[bm]->eq_class: -1;

      if      (am < bm) return -1;
      else if (am > bm) return +1;
    }

  }

  /* If you get here, the states are still equivalent */
  return 0;

}
/*}}}*/
static int split_classes(DFANode **seq, DFANode **dfas, int ndfas)/*{{{*/
/*********************** Do one pass of class splitting ***********************/
{
  int i;
  int had_to_split = 0;

  calculate_signatures(seq, dfas, ndfas);
  qsort(seq, ndfas, sizeof(DFANode *), general_compare);

  seq[0]->new_eq_class = seq[0]->eq_class;

  for (i=1; i<ndfas; i++) {
    seq[i]->new_eq_class = seq[i]->eq_class;

    if (seq[i]->eq_class == seq[i-1]->eq_class) {
      /* May need to split, otherwise states were previously separated anyway
         */

      if (general_compare(seq+i, seq+i-1) != 0) {
        /* Different transition pattern, split existing equivalent class */
        had_to_split = 1;
        seq[i]->new_eq_class = ++last_eq_class;
        if (verbose) fprintf(stderr, "Found %d equivalence classes\r", last_eq_class+1);
      } else {
        /* This works even if seq[i-1] was assigned a new class due to
           splitting from seq[i-2] etc. */
        seq[i]->new_eq_class = seq[i-1]->new_eq_class;
      }
    }
  }

  /* Set classes to new class values. */
  for (i=0; i<ndfas; i++) {
    seq[i]->eq_class = seq[i]->new_eq_class;
  }

  return had_to_split;

}
/*}}}*/
static int initial_compare(const void *a, const void *b)/*{{{*/
/************************** Sort based on tags **************************/
{
  Castderef (a, const DFANode *, aa);
  Castderef (b, const DFANode *, bb);
  int status;
  int i;

  for (i=0; i<n_evaluators; i++) {

    const char *ar = aa->attrs[i], *br = bb->attrs[i];
    if (!ar) ar = get_defattr(i);
    if (!br) br = get_defattr(i);

    /* Sort so that states with identical attributes appear together. */
    if (!ar && br) {
      return -1;
    } else if (ar && !br) {
      return +1;
    } else {
      if (ar && br) {
        status = strcmp(ar, br);
        if      (status < 0) return -1;
        else if (status > 0) return +1;
      }

      /* So neither had an attribute at all, or both did and they were equal.
       * i.e. need to look at attributes further up the vectors */
    }
  }

  /* Got here => both states were identical in terms of their attribute sets */
  return 0;
}
/*}}}*/
static void assign_initial_classes(DFANode **seq, int ndfas)/*{{{*/
/******************* Determine initial equivalence classes. *******************/
{
  int i;
  qsort(seq, ndfas, sizeof(DFANode *), initial_compare);

  last_eq_class = 0;

  seq[0]->eq_class = last_eq_class;

  for (i=1; i<ndfas; i++) {
    if (initial_compare(seq+i-1, seq+i) != 0) {
      /* Not same as previous entry, assign a new class */
      seq[i]->eq_class = ++last_eq_class;
    } else {
      /* Same class as last entry */
      seq[i]->eq_class = last_eq_class;
    }
  }
}
/*}}}*/
/*{{{ compress_states() */
static void compress_states(struct DFA *dfa, int n_dfa_entries, struct DFAEntry *dfa_entries)
/***** Compress the DFA so there is precisely one state in each eq. class *****/
{
  int *reps;
  int i, j, t;
  int neqc;
  int new_index;

  if (verbose) fprintf(stderr, "%d DFA states before compression\n", dfa->n);

  if (report) {
    fprintf(report,
        "\n-----------------------------\n"
        "------ COMPRESSING DFA ------\n"
        "-----------------------------\n");
  }

  neqc = 1 + last_eq_class;

  /* Array containing which state is the representative of each eq. class.
     Keep the state which had the lowest array index. */
  reps = new_array(int, neqc);

  for (i=0; i<neqc; i++) reps[i] = -1; /* undefined */

  /* Go through DFA states to find the representative of each class. */
  for (i=0; i<dfa->n; i++) {
    int eqc = dfa->s[i]->eq_class;
    if (reps[eqc] < 0) {
      reps[eqc] = i;
      dfa->s[i]->is_rep = 1;
    } else {
      dfa->s[i]->is_rep = 0;
    }
  }

  /* Go through DFA states and assign new indices. */
  for (i=0, new_index=0; i<dfa->n; i++) {
    if (dfa->s[i]->is_dead) {
      dfa->s[i]->new_index = -1;
      if (report) fprintf(report, "Old DFA state %d becomes -1 (dead state)\n", i);
    } else if (dfa->s[i]->is_rep) {
      dfa->s[i]->new_index = new_index++;
      if (report) fprintf(report, "Old DFA state %d becomes %d\n", i, dfa->s[i]->new_index);
    } else {
      int eqc = dfa->s[i]->eq_class;
      int rep = reps[eqc];

      /* This assignment works because the representative for the class
         must have been done earlier in the loop. */
      dfa->s[i]->new_index = dfa->s[rep]->new_index;

      if (report) fprintf(report, "Old DFA state %d becomes %d (formerly %d)\n", i, dfa->s[i]->new_index, rep);
    }
  }

  /* Go through all transitions and fix them up. */
  for (i=0; i<dfa->n; i++) {
    DFANode *s = dfa->s[i];
    for (t=0; t<Nt; t++) {
      int dest = s->map[t];
      if (dest >= 0) {
        s->map[t] = dfa->s[dest]->new_index;
      }
    }
  }

  /* Go through the entries and fix their states */
  for (i=0; i<n_dfa_entries; i++) {
    int ni = dfa->s[dfa_entries[i].state_number]->new_index;
    if (report) {
      fprintf(report, "Entry <%s>, formerly state %d, now state %d\n",
          dfa_entries[i].entry_name,
          dfa_entries[i].state_number, ni);
    }
    dfa_entries[i].state_number = dfa->s[dfa_entries[i].state_number]->new_index;
  }

  /* Fix from_state */
  for (i=0; i<dfa->n; i++) {
    int old_from_state, new_from_state;
    /* If we're not going to preserve the state, move along */
    if (!dfa->s[i]->is_rep) continue;
    old_from_state = dfa->s[i]->from_state;
    /* Any entry state ..., move along */
    if (old_from_state < 0) continue;
    new_from_state = dfa->s[reps[dfa->s[old_from_state]->eq_class]]->new_index;
    dfa->s[i]->from_state = new_from_state;
  }

  /* Go through and crunch the entries in the DFA array, fixing up the indices */
  for (i=j=0; i<dfa->n; i++) {
    if (!dfa->s[i]->is_dead && dfa->s[i]->is_rep) {
      dfa->s[j] = dfa->s[i];
      dfa->s[j]->index = dfa->s[j]->new_index;
      j++;
    }
  }

  free(reps);
  dfa->n = new_index; /* ignore dead states which are completely pruned. */
  if (verbose) fprintf(stderr, "%d DFA states after compression", dfa->n);
}
/*}}}*/
static void discard_nfa_bitmaps(struct DFA *dfa)/*{{{*/
/********** Discard the (now inaccurate) NFA bitmaps from the states **********/
{
  int i;
  for (i=0; i<dfa->n; i++) {
    free(dfa->s[i]->nfas);
    dfa->s[i]->nfas = NULL;
  }
  return;
}
/*}}}*/
static void print_classes(DFANode **dfas, int ndfas)/*{{{*/
{
  int i;
#if 1
  /* Comment out to print this stuff for debug */
  return;
#endif
  if (!report) return;
  fprintf(report, "Equivalence classes are :\n");
  for (i=0; i<ndfas; i++) {
    fprintf(report, "State %d class %d\n", i, dfas[i]->eq_class);
  }
  fprintf(report, "\n");
  return;
}
/*}}}*/
static int has_any_nondefault_attribute(const DFANode *x)/*{{{*/
{
  int result = 0;
  int i;
  for (i=0; i<n_evaluators; i++) {
    if (x->attrs[i]) {
      char *defattr;
      defattr = get_defattr(i);
      if (defattr && strcmp(defattr, x->attrs[i])) {
        result = 1;
        break;
      }
    }
  }
  return result;
}
/*}}}*/
static void find_dead_states(DFANode **dfas, int ndfas, int ntokens)/*{{{*/
{
  /* Find any state that has no transitions out of it and no attribute.
   * If you get there, you're guaranteed to be stuck.
   * Then, repeatedly look for states which are such that all transitions from
   * them lead to dead states.  Mark these dead too.
   * Then, go through all the dead states and remove their transitions.
   * This will force them all into a single class later. */

  int did_any;
  int i, j;
  /* Eventually, consider looking for results that are non-default. */
  char *leads_to_result;
  int total_found = 0;

  leads_to_result = new_array(char, ndfas);
  memset(leads_to_result, 0, ndfas);

  if (report) {
    fprintf(report, "Searching for dead states...\n");
  }

  do {
    did_any = 0;
    for (i=0; i<ndfas; i++) {
      if (leads_to_result[i] == 0) {
        if (has_any_nondefault_attribute(dfas[i])) {
          leads_to_result[i] = 1;
          did_any = 1;
          continue;
        }

        for (j=0; j<ntokens; j++) {
          int next_state = dfas[i]->map[j];
          if ((next_state >= 0) && leads_to_result[next_state]) {
            leads_to_result[i] = 1;
            did_any = 1;
            goto do_next_dfa_state;
          }
        }
      }
do_next_dfa_state:
      (void) 0;
    }
  } while (did_any);


  /* Now prune any transition to states that have no path to a result. */
  for (i=0; i<ndfas; i++) {
    if (leads_to_result[i] == 0) {
      total_found++;
      if (report) {
        fprintf(report, "DFA state %d is dead\n", i);
      }
      dfas[i]->from_state = -1;
      dfas[i]->via_token = -1;
      dfas[i]->is_dead = 1;
    } else {
      dfas[i]->is_dead = 0;
    }

    for (j=0; j<ntokens; j++) {
      int next_state = dfas[i]->map[j];
      if (leads_to_result[next_state] == 0) {
        dfas[i]->map[j] = -1;
      }
    }
  }

  free(leads_to_result);

  if (!total_found && report) {
    fprintf(report, "(no dead states found)\n");
  }
}
/*}}}*/
/*{{{ compress_dfa() */
void compress_dfa(struct DFA *dfa, int ntokens,
    int n_dfa_entries, struct DFAEntry *dfa_entries)
{
  DFANode **seq; /* Storage for node sequence */
  int i;
  int had_to_split;

  /* Safety net */
  if (dfa->n <= 0) return;

  local_dfas = dfa->s;
  Nt = ntokens;

  seq = new_array(DFANode *, dfa->n);
  for (i=0; i<dfa->n; i++) {
    seq[i] = dfa->s[i];
  }

  find_dead_states(dfa->s, dfa->n, ntokens);

  assign_initial_classes(seq, dfa->n);

  do {
    print_classes(dfa->s, dfa->n);
    had_to_split = split_classes(seq, dfa->s, dfa->n);
  } while (had_to_split);

  print_classes(dfa->s, dfa->n);

  compress_states(dfa, n_dfa_entries, dfa_entries);
  discard_nfa_bitmaps(dfa);

  free(seq);
  return;

}
/*}}}*/

