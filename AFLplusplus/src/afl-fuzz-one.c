/*
   american fuzzy lop++ - fuzze_one routines in different flavours
   ---------------------------------------------------------------

   Originally written by Michal Zalewski

   Now maintained by Marc Heuse <mh@mh-sec.de>,
                        Heiko Eißfeldt <heiko.eissfeldt@hexco.de> and
                        Andrea Fioraldi <andreafioraldi@gmail.com>

   Copyright 2016, 2017 Google Inc. All rights reserved.
   Copyright 2019-2020 AFLplusplus Project. All rights reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at:

     http://www.apache.org/licenses/LICENSE-2.0

   This is the real deal: the program takes an instrumented binary and
   attempts a variety of basic fuzzing tricks, paying close attention to
   how they affect the execution path.

 */

#include "afl-fuzz.h"
#include <string.h>
#include <limits.h>
#include "cmplog.h"

/* MOpt */

static int select_algorithm(afl_state_t *afl, u32 max_algorithm) {

  int i_puppet, j_puppet = 0, operator_number = max_algorithm;

  double range_sele =
      (double)afl->probability_now[afl->swarm_now][operator_number - 1];
  double sele = ((double)(rand_below(afl, 10000) * 0.0001 * range_sele));

  for (i_puppet = 0; i_puppet < operator_num; ++i_puppet) {

    if (unlikely(i_puppet == 0)) {

      if (sele < afl->probability_now[afl->swarm_now][i_puppet]) { break; }

    } else {

      if (sele < afl->probability_now[afl->swarm_now][i_puppet]) {

        j_puppet = 1;
        break;

      }

    }

  }

  if ((j_puppet == 1 &&
       sele < afl->probability_now[afl->swarm_now][i_puppet - 1]) ||
      (i_puppet + 1 < operator_num &&
       sele > afl->probability_now[afl->swarm_now][i_puppet + 1])) {

    FATAL("error select_algorithm");

  }

  return i_puppet;

}

/* Helper to choose random block len for block operations in fuzz_one().
   Doesn't return zero, provided that max_len is > 0. */

static inline u32 choose_block_len(afl_state_t *afl, u32 limit) {

  u32 min_value, max_value;
  u32 rlim = MIN(afl->queue_cycle, (u32)3);

  if (unlikely(!afl->run_over10m)) { rlim = 1; }

  switch (rand_below(afl, rlim)) {

    case 0:
      min_value = 1;
      max_value = HAVOC_BLK_SMALL;
      break;

    case 1:
      min_value = HAVOC_BLK_SMALL;
      max_value = HAVOC_BLK_MEDIUM;
      break;

    default:

      if (likely(rand_below(afl, 10))) {

        min_value = HAVOC_BLK_MEDIUM;
        max_value = HAVOC_BLK_LARGE;

      } else {

        min_value = HAVOC_BLK_LARGE;
        max_value = HAVOC_BLK_XL;

      }

  }

  if (min_value >= limit) { min_value = 1; }

  return min_value + rand_below(afl, MIN(max_value, limit) - min_value + 1);

}

/* Helper function to see if a particular change (xor_val = old ^ new) could
   be a product of deterministic bit flips with the lengths and stepovers
   attempted by afl-fuzz. This is used to avoid dupes in some of the
   deterministic fuzzing operations that follow bit flips. We also
   return 1 if xor_val is zero, which implies that the old and attempted new
   values are identical and the exec would be a waste of time. */

static u8 could_be_bitflip(u32 xor_val) {

  u32 sh = 0;

  if (!xor_val) { return 1; }

  /* Shift left until first bit set. */

  while (!(xor_val & 1)) {

    ++sh;
    xor_val >>= 1;

  }

  /* 1-, 2-, and 4-bit patterns are OK anywhere. */

  if (xor_val == 1 || xor_val == 3 || xor_val == 15) { return 1; }

  /* 8-, 16-, and 32-bit patterns are OK only if shift factor is
     divisible by 8, since that's the stepover for these ops. */

  if (sh & 7) { return 0; }

  if (xor_val == 0xff || xor_val == 0xffff || xor_val == 0xffffffff) {

    return 1;

  }

  return 0;

}

/* Helper function to see if a particular value is reachable through
   arithmetic operations. Used for similar purposes. */

static u8 could_be_arith(u32 old_val, u32 new_val, u8 blen) {

  u32 i, ov = 0, nv = 0, diffs = 0;

  if (old_val == new_val) { return 1; }

  /* See if one-byte adjustments to any byte could produce this result. */

  for (i = 0; (u8)i < blen; ++i) {

    u8 a = old_val >> (8 * i), b = new_val >> (8 * i);

    if (a != b) {

      ++diffs;
      ov = a;
      nv = b;

    }

  }

  /* If only one byte differs and the values are within range, return 1. */

  if (diffs == 1) {

    if ((u8)(ov - nv) <= ARITH_MAX || (u8)(nv - ov) <= ARITH_MAX) { return 1; }

  }

  if (blen == 1) { return 0; }

  /* See if two-byte adjustments to any byte would produce this result. */

  diffs = 0;

  for (i = 0; (u8)i < blen / 2; ++i) {

    u16 a = old_val >> (16 * i), b = new_val >> (16 * i);

    if (a != b) {

      ++diffs;
      ov = a;
      nv = b;

    }

  }

  /* If only one word differs and the values are within range, return 1. */

  if (diffs == 1) {

    if ((u16)(ov - nv) <= ARITH_MAX || (u16)(nv - ov) <= ARITH_MAX) {

      return 1;

    }

    ov = SWAP16(ov);
    nv = SWAP16(nv);

    if ((u16)(ov - nv) <= ARITH_MAX || (u16)(nv - ov) <= ARITH_MAX) {

      return 1;

    }

  }

  /* Finally, let's do the same thing for dwords. */

  if (blen == 4) {

    if ((u32)(old_val - new_val) <= ARITH_MAX ||
        (u32)(new_val - old_val) <= ARITH_MAX) {

      return 1;

    }

    new_val = SWAP32(new_val);
    old_val = SWAP32(old_val);

    if ((u32)(old_val - new_val) <= ARITH_MAX ||
        (u32)(new_val - old_val) <= ARITH_MAX) {

      return 1;

    }

  }

  return 0;

}

/* Last but not least, a similar helper to see if insertion of an
   interesting integer is redundant given the insertions done for
   shorter blen. The last param (check_le) is set if the caller
   already executed LE insertion for current blen and wants to see
   if BE variant passed in new_val is unique. */

static u8 could_be_interest(u32 old_val, u32 new_val, u8 blen, u8 check_le) {

  u32 i, j;

  if (old_val == new_val) { return 1; }

  /* See if one-byte insertions from interesting_8 over old_val could
     produce new_val. */

  for (i = 0; i < blen; ++i) {

    for (j = 0; j < sizeof(interesting_8); ++j) {

      u32 tval =
          (old_val & ~(0xff << (i * 8))) | (((u8)interesting_8[j]) << (i * 8));

      if (new_val == tval) { return 1; }

    }

  }

  /* Bail out unless we're also asked to examine two-byte LE insertions
     as a preparation for BE attempts. */

  if (blen == 2 && !check_le) { return 0; }

  /* See if two-byte insertions over old_val could give us new_val. */

  for (i = 0; (u8)i < blen - 1; ++i) {

    for (j = 0; j < sizeof(interesting_16) / 2; ++j) {

      u32 tval = (old_val & ~(0xffff << (i * 8))) |
                 (((u16)interesting_16[j]) << (i * 8));

      if (new_val == tval) { return 1; }

      /* Continue here only if blen > 2. */

      if (blen > 2) {

        tval = (old_val & ~(0xffff << (i * 8))) |
               (SWAP16(interesting_16[j]) << (i * 8));

        if (new_val == tval) { return 1; }

      }

    }

  }

  if (blen == 4 && check_le) {

    /* See if four-byte insertions could produce the same result
       (LE only). */

    for (j = 0; j < sizeof(interesting_32) / 4; ++j) {

      if (new_val == (u32)interesting_32[j]) { return 1; }

    }

  }

  return 0;

}

#ifndef IGNORE_FINDS

/* Helper function to compare buffers; returns first and last differing offset.
   We use this to find reasonable locations for splicing two files. */

static void locate_diffs(u8 *ptr1, u8 *ptr2, u32 len, s32 *first, s32 *last) {

  s32 f_loc = -1;
  s32 l_loc = -1;
  u32 pos;

  for (pos = 0; pos < len; ++pos) {

    if (*(ptr1++) != *(ptr2++)) {

      if (f_loc == -1) { f_loc = pos; }
      l_loc = pos;

    }

  }

  *first = f_loc;
  *last = l_loc;

  return;

}

#endif                                                     /* !IGNORE_FINDS */

/* Take the current entry from the queue, fuzz it for a while. This
   function is a tad too long... returns 0 if fuzzed successfully, 1 if
   skipped or bailed out. */

u8 fuzz_one_original(afl_state_t *afl) {

  u32 len, temp_len;
  u32 j;
  u32 i;
  u8 *in_buf, *out_buf, *orig_in, *ex_tmp, *eff_map = 0;
  u64 havoc_queued = 0, orig_hit_cnt, new_hit_cnt = 0, prev_cksum;
  u32 splice_cycle = 0, perf_score = 100, orig_perf, eff_cnt = 1;

  u8 ret_val = 1, doing_det = 0;

  u8  a_collect[MAX_AUTO_EXTRA];
  u32 a_len = 0;

#ifdef IGNORE_FINDS

  /* In IGNORE_FINDS mode, skip any entries that weren't in the
     initial data set. */

  if (afl->queue_cur->depth > 1) return 1;

#else

  if (unlikely(afl->custom_mutators_count)) {

    /* The custom mutator will decide to skip this test case or not. */

    LIST_FOREACH(&afl->custom_mutator_list, struct custom_mutator, {

      if (el->afl_custom_queue_get &&
          !el->afl_custom_queue_get(el->data, afl->queue_cur->fname)) {

        return 1;

      }

    });

  }

  if (likely(afl->pending_favored)) {

    /* If we have any favored, non-fuzzed new arrivals in the queue,
       possibly skip to them at the expense of already-fuzzed or non-favored
       cases. */

    if (((afl->queue_cur->was_fuzzed > 0 || afl->queue_cur->fuzz_level > 0) ||
         !afl->queue_cur->favored) &&
        likely(rand_below(afl, 100) < SKIP_TO_NEW_PROB)) {

      return 1;

    }

  } else if (!afl->non_instrumented_mode && !afl->queue_cur->favored &&

             afl->queued_paths > 10) {

    /* Otherwise, still possibly skip non-favored cases, albeit less often.
       The odds of skipping stuff are higher for already-fuzzed inputs and
       lower for never-fuzzed entries. */

    if (afl->queue_cycle > 1 &&
        (afl->queue_cur->fuzz_level == 0 || afl->queue_cur->was_fuzzed)) {

      if (likely(rand_below(afl, 100) < SKIP_NFAV_NEW_PROB)) { return 1; }

    } else {

      if (likely(rand_below(afl, 100) < SKIP_NFAV_OLD_PROB)) { return 1; }

    }

  }

#endif                                                     /* ^IGNORE_FINDS */

  if (unlikely(afl->not_on_tty)) {

    ACTF(
        "Fuzzing test case #%u (%u total, %llu uniq crashes found, "
        "perf_score=%0.0f, exec_us=%llu, hits=%u, map=%u)...",
        afl->current_entry, afl->queued_paths, afl->unique_crashes,
        afl->queue_cur->perf_score, afl->queue_cur->exec_us,
        likely(afl->n_fuzz) ? afl->n_fuzz[afl->queue_cur->n_fuzz_entry] : 0,
        afl->queue_cur->bitmap_size);
    fflush(stdout);

  }

  orig_in = in_buf = queue_testcase_get(afl, afl->queue_cur);
  len = afl->queue_cur->len;

  out_buf = afl_realloc(AFL_BUF_PARAM(out), len);
  if (unlikely(!out_buf)) { PFATAL("alloc"); }

  afl->subseq_tmouts = 0;

  afl->cur_depth = afl->queue_cur->depth;

  /*******************************************
   * CALIBRATION (only if failed earlier on) *
   *******************************************/

  if (unlikely(afl->queue_cur->cal_failed)) {

    u8 res = FSRV_RUN_TMOUT;

    if (afl->queue_cur->cal_failed < CAL_CHANCES) {

      afl->queue_cur->exec_cksum = 0;

      res =
          calibrate_case(afl, afl->queue_cur, in_buf, afl->queue_cycle - 1, 0);

      if (unlikely(res == FSRV_RUN_ERROR)) {

        FATAL("Unable to execute target application");

      }

    }

    if (unlikely(afl->stop_soon) || res != afl->crash_mode) {

      ++afl->cur_skipped_paths;
      goto abandon_entry;

    }

  }

  /************
   * TRIMMING *
   ************/

  if (unlikely(!afl->non_instrumented_mode && !afl->queue_cur->trim_done &&
               !afl->disable_trim)) {

    u32 old_len = afl->queue_cur->len;

    u8 res = trim_case(afl, afl->queue_cur, in_buf);
    orig_in = in_buf = queue_testcase_get(afl, afl->queue_cur);

    if (unlikely(res == FSRV_RUN_ERROR)) {

      FATAL("Unable to execute target application");

    }

    if (unlikely(afl->stop_soon)) {

      ++afl->cur_skipped_paths;
      goto abandon_entry;

    }

    /* Don't retry trimming, even if it failed. */

    afl->queue_cur->trim_done = 1;

    len = afl->queue_cur->len;

    /* maybe current entry is not ready for splicing anymore */
    if (unlikely(len <= 4 && old_len > 4)) --afl->ready_for_splicing_count;

  }

  memcpy(out_buf, in_buf, len);

  /*********************
   * PERFORMANCE SCORE *
   *********************/

  if (likely(!afl->old_seed_selection))
    orig_perf = perf_score = afl->queue_cur->perf_score;
  else
    afl->queue_cur->perf_score = orig_perf = perf_score =
        calculate_score(afl, afl->queue_cur);

  if (unlikely(perf_score <= 0)) { goto abandon_entry; }

  if (unlikely(afl->shm.cmplog_mode &&
               afl->queue_cur->colorized < afl->cmplog_lvl &&
               (u32)len <= afl->cmplog_max_filesize)) {

    if (unlikely(len < 4)) {

      afl->queue_cur->colorized = CMPLOG_LVL_MAX;

    } else {

      if (afl->cmplog_lvl == 3 ||
          (afl->cmplog_lvl == 2 && afl->queue_cur->tc_ref) ||
          afl->queue_cur->favored ||
          !(afl->fsrv.total_execs % afl->queued_paths) ||
          get_cur_time() - afl->last_path_time > 300000) {  // 300 seconds

        if (input_to_state_stage(afl, in_buf, out_buf, len)) {

          goto abandon_entry;

        }

      }

    }

  }

  /* Skip right away if -d is given, if it has not been chosen sufficiently
     often to warrant the expensive deterministic stage (fuzz_level), or
     if it has gone through deterministic testing in earlier, resumed runs
     (passed_det). */

  if (likely(afl->queue_cur->passed_det) || likely(afl->skip_deterministic) ||
      likely(perf_score <
             (afl->queue_cur->depth * 30 <= afl->havoc_max_mult * 100
                  ? afl->queue_cur->depth * 30
                  : afl->havoc_max_mult * 100))) {

    goto custom_mutator_stage;

  }

  /* Skip deterministic fuzzing if exec path checksum puts this out of scope
     for this main instance. */

  if (unlikely(afl->main_node_max &&
               (afl->queue_cur->exec_cksum % afl->main_node_max) !=
                   afl->main_node_id - 1)) {

    goto custom_mutator_stage;

  }

  doing_det = 1;

  /*********************************************
   * SIMPLE BITFLIP (+dictionary construction) *
   *********************************************/

#define FLIP_BIT(_ar, _b)                   \
  do {                                      \
                                            \
    u8 *_arf = (u8 *)(_ar);                 \
    u32 _bf = (_b);                         \
    _arf[(_bf) >> 3] ^= (128 >> ((_bf)&7)); \
                                            \
  } while (0)

  /* Single walking bit. */

  afl->stage_short = "flip1";
  afl->stage_max = len << 3;
  afl->stage_name = "bitflip 1/1";

  afl->stage_val_type = STAGE_VAL_NONE;

  orig_hit_cnt = afl->queued_paths + afl->unique_crashes;

  prev_cksum = afl->queue_cur->exec_cksum;

  for (afl->stage_cur = 0; afl->stage_cur < afl->stage_max; ++afl->stage_cur) {

    afl->stage_cur_byte = afl->stage_cur >> 3;

    FLIP_BIT(out_buf, afl->stage_cur);

#ifdef INTROSPECTION
    snprintf(afl->mutation, sizeof(afl->mutation), "%s FLIP_BIT1-%u",
             afl->queue_cur->fname, afl->stage_cur);
#endif

    if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }

    FLIP_BIT(out_buf, afl->stage_cur);

    /* While flipping the least significant bit in every byte, pull of an extra
       trick to detect possible syntax tokens. In essence, the idea is that if
       you have a binary blob like this:

       xxxxxxxxIHDRxxxxxxxx

       ...and changing the leading and trailing bytes causes variable or no
       changes in program flow, but touching any character in the "IHDR" string
       always produces the same, distinctive path, it's highly likely that
       "IHDR" is an atomically-checked magic value of special significance to
       the fuzzed format.

       We do this here, rather than as a separate stage, because it's a nice
       way to keep the operation approximately "free" (i.e., no extra execs).

       Empirically, performing the check when flipping the least significant bit
       is advantageous, compared to doing it at the time of more disruptive
       changes, where the program flow may be affected in more violent ways.

       The caveat is that we won't generate dictionaries in the -d mode or -S
       mode - but that's probably a fair trade-off.

       This won't work particularly well with paths that exhibit variable
       behavior, but fails gracefully, so we'll carry out the checks anyway.

      */

    if (!afl->non_instrumented_mode && (afl->stage_cur & 7) == 7) {

      u64 cksum = hash64(afl->fsrv.trace_bits, afl->fsrv.map_size, HASH_CONST);

      if (afl->stage_cur == afl->stage_max - 1 && cksum == prev_cksum) {

        /* If at end of file and we are still collecting a string, grab the
           final character and force output. */

        if (a_len < MAX_AUTO_EXTRA) {

          a_collect[a_len] = out_buf[afl->stage_cur >> 3];

        }

        ++a_len;

        if (a_len >= MIN_AUTO_EXTRA && a_len <= MAX_AUTO_EXTRA) {

          maybe_add_auto(afl, a_collect, a_len);

        }

      } else if (cksum != prev_cksum) {

        /* Otherwise, if the checksum has changed, see if we have something
           worthwhile queued up, and collect that if the answer is yes. */

        if (a_len >= MIN_AUTO_EXTRA && a_len <= MAX_AUTO_EXTRA) {

          maybe_add_auto(afl, a_collect, a_len);

        }

        a_len = 0;
        prev_cksum = cksum;

      }

      /* Continue collecting string, but only if the bit flip actually made
         any difference - we don't want no-op tokens. */

      if (cksum != afl->queue_cur->exec_cksum) {

        if (a_len < MAX_AUTO_EXTRA) {

          a_collect[a_len] = out_buf[afl->stage_cur >> 3];

        }

        ++a_len;

      }

    }

  }

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_FLIP1] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_FLIP1] += afl->stage_max;

  /* Two walking bits. */

  afl->stage_name = "bitflip 2/1";
  afl->stage_short = "flip2";
  afl->stage_max = (len << 3) - 1;

  orig_hit_cnt = new_hit_cnt;

  for (afl->stage_cur = 0; afl->stage_cur < afl->stage_max; ++afl->stage_cur) {

    afl->stage_cur_byte = afl->stage_cur >> 3;

    FLIP_BIT(out_buf, afl->stage_cur);
    FLIP_BIT(out_buf, afl->stage_cur + 1);

#ifdef INTROSPECTION
    snprintf(afl->mutation, sizeof(afl->mutation), "%s FLIP_BIT2-%u",
             afl->queue_cur->fname, afl->stage_cur);
#endif

    if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }

    FLIP_BIT(out_buf, afl->stage_cur);
    FLIP_BIT(out_buf, afl->stage_cur + 1);

  }

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_FLIP2] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_FLIP2] += afl->stage_max;

  /* Four walking bits. */

  afl->stage_name = "bitflip 4/1";
  afl->stage_short = "flip4";
  afl->stage_max = (len << 3) - 3;

  orig_hit_cnt = new_hit_cnt;

  for (afl->stage_cur = 0; afl->stage_cur < afl->stage_max; ++afl->stage_cur) {

    afl->stage_cur_byte = afl->stage_cur >> 3;

    FLIP_BIT(out_buf, afl->stage_cur);
    FLIP_BIT(out_buf, afl->stage_cur + 1);
    FLIP_BIT(out_buf, afl->stage_cur + 2);
    FLIP_BIT(out_buf, afl->stage_cur + 3);

#ifdef INTROSPECTION
    snprintf(afl->mutation, sizeof(afl->mutation), "%s FLIP_BIT4-%u",
             afl->queue_cur->fname, afl->stage_cur);
#endif

    if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }

    FLIP_BIT(out_buf, afl->stage_cur);
    FLIP_BIT(out_buf, afl->stage_cur + 1);
    FLIP_BIT(out_buf, afl->stage_cur + 2);
    FLIP_BIT(out_buf, afl->stage_cur + 3);

  }

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_FLIP4] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_FLIP4] += afl->stage_max;

  /* Effector map setup. These macros calculate:

     EFF_APOS      - position of a particular file offset in the map.
     EFF_ALEN      - length of a map with a particular number of bytes.
     EFF_SPAN_ALEN - map span for a sequence of bytes.

   */

#define EFF_APOS(_p) ((_p) >> EFF_MAP_SCALE2)
#define EFF_REM(_x) ((_x) & ((1 << EFF_MAP_SCALE2) - 1))
#define EFF_ALEN(_l) (EFF_APOS(_l) + !!EFF_REM(_l))
#define EFF_SPAN_ALEN(_p, _l) (EFF_APOS((_p) + (_l)-1) - EFF_APOS(_p) + 1)

  /* Initialize effector map for the next step (see comments below). Always
     flag first and last byte as doing something. */

  eff_map = afl_realloc(AFL_BUF_PARAM(eff), EFF_ALEN(len));
  if (unlikely(!eff_map)) { PFATAL("alloc"); }
  eff_map[0] = 1;

  if (EFF_APOS(len - 1) != 0) {

    eff_map[EFF_APOS(len - 1)] = 1;
    ++eff_cnt;

  }

  /* Walking byte. */

  afl->stage_name = "bitflip 8/8";
  afl->stage_short = "flip8";
  afl->stage_max = len;

  orig_hit_cnt = new_hit_cnt;

  for (afl->stage_cur = 0; afl->stage_cur < afl->stage_max; ++afl->stage_cur) {

    afl->stage_cur_byte = afl->stage_cur;

    out_buf[afl->stage_cur] ^= 0xFF;

#ifdef INTROSPECTION
    snprintf(afl->mutation, sizeof(afl->mutation), "%s FLIP_BIT8-%u",
             afl->queue_cur->fname, afl->stage_cur);
#endif

    if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }

    /* We also use this stage to pull off a simple trick: we identify
       bytes that seem to have no effect on the current execution path
       even when fully flipped - and we skip them during more expensive
       deterministic stages, such as arithmetics or known ints. */

    if (!eff_map[EFF_APOS(afl->stage_cur)]) {

      u64 cksum;

      /* If in non-instrumented mode or if the file is very short, just flag
         everything without wasting time on checksums. */

      if (!afl->non_instrumented_mode && len >= EFF_MIN_LEN) {

        cksum = hash64(afl->fsrv.trace_bits, afl->fsrv.map_size, HASH_CONST);

      } else {

        cksum = ~afl->queue_cur->exec_cksum;

      }

      if (cksum != afl->queue_cur->exec_cksum) {

        eff_map[EFF_APOS(afl->stage_cur)] = 1;
        ++eff_cnt;

      }

    }

    out_buf[afl->stage_cur] ^= 0xFF;

  }

  /* If the effector map is more than EFF_MAX_PERC dense, just flag the
     whole thing as worth fuzzing, since we wouldn't be saving much time
     anyway. */

  if (eff_cnt != (u32)EFF_ALEN(len) &&
      eff_cnt * 100 / EFF_ALEN(len) > EFF_MAX_PERC) {

    memset(eff_map, 1, EFF_ALEN(len));

    afl->blocks_eff_select += EFF_ALEN(len);

  } else {

    afl->blocks_eff_select += eff_cnt;

  }

  afl->blocks_eff_total += EFF_ALEN(len);

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_FLIP8] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_FLIP8] += afl->stage_max;

  /* Two walking bytes. */

  if (len < 2) { goto skip_bitflip; }

  afl->stage_name = "bitflip 16/8";
  afl->stage_short = "flip16";
  afl->stage_cur = 0;
  afl->stage_max = len - 1;

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < len - 1; ++i) {

    /* Let's consult the effector map... */

    if (!eff_map[EFF_APOS(i)] && !eff_map[EFF_APOS(i + 1)]) {

      --afl->stage_max;
      continue;

    }

    afl->stage_cur_byte = i;

    *(u16 *)(out_buf + i) ^= 0xFFFF;

#ifdef INTROSPECTION
    snprintf(afl->mutation, sizeof(afl->mutation), "%s FLIP_BIT16-%u",
             afl->queue_cur->fname, afl->stage_cur);
#endif

    if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
    ++afl->stage_cur;

    *(u16 *)(out_buf + i) ^= 0xFFFF;

  }

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_FLIP16] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_FLIP16] += afl->stage_max;

  if (len < 4) { goto skip_bitflip; }

  /* Four walking bytes. */

  afl->stage_name = "bitflip 32/8";
  afl->stage_short = "flip32";
  afl->stage_cur = 0;
  afl->stage_max = len - 3;

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < len - 3; ++i) {

    /* Let's consult the effector map... */
    if (!eff_map[EFF_APOS(i)] && !eff_map[EFF_APOS(i + 1)] &&
        !eff_map[EFF_APOS(i + 2)] && !eff_map[EFF_APOS(i + 3)]) {

      --afl->stage_max;
      continue;

    }

    afl->stage_cur_byte = i;

    *(u32 *)(out_buf + i) ^= 0xFFFFFFFF;

#ifdef INTROSPECTION
    snprintf(afl->mutation, sizeof(afl->mutation), "%s FLIP_BIT32-%u",
             afl->queue_cur->fname, afl->stage_cur);
#endif

    if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
    ++afl->stage_cur;

    *(u32 *)(out_buf + i) ^= 0xFFFFFFFF;

  }

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_FLIP32] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_FLIP32] += afl->stage_max;

skip_bitflip:

  if (afl->no_arith) { goto skip_arith; }

  /**********************
   * ARITHMETIC INC/DEC *
   **********************/

  /* 8-bit arithmetics. */

  afl->stage_name = "arith 8/8";
  afl->stage_short = "arith8";
  afl->stage_cur = 0;
  afl->stage_max = 2 * len * ARITH_MAX;

  afl->stage_val_type = STAGE_VAL_LE;

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < (u32)len; ++i) {

    u8 orig = out_buf[i];

    /* Let's consult the effector map... */

    if (!eff_map[EFF_APOS(i)]) {

      afl->stage_max -= 2 * ARITH_MAX;
      continue;

    }

    afl->stage_cur_byte = i;

    for (j = 1; j <= ARITH_MAX; ++j) {

      u8 r = orig ^ (orig + j);

      /* Do arithmetic operations only if the result couldn't be a product
         of a bitflip. */

      if (!could_be_bitflip(r)) {

        afl->stage_cur_val = j;
        out_buf[i] = orig + j;

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s ARITH8+-%u-%u",
                 afl->queue_cur->fname, i, j);
#endif

        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      r = orig ^ (orig - j);

      if (!could_be_bitflip(r)) {

        afl->stage_cur_val = -j;
        out_buf[i] = orig - j;

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s ARITH8--%u-%u",
                 afl->queue_cur->fname, i, j);
#endif

        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      out_buf[i] = orig;

    }

  }

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_ARITH8] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_ARITH8] += afl->stage_max;

  /* 16-bit arithmetics, both endians. */

  if (len < 2) { goto skip_arith; }

  afl->stage_name = "arith 16/8";
  afl->stage_short = "arith16";
  afl->stage_cur = 0;
  afl->stage_max = 4 * (len - 1) * ARITH_MAX;

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < (u32)len - 1; ++i) {

    u16 orig = *(u16 *)(out_buf + i);

    /* Let's consult the effector map... */

    if (!eff_map[EFF_APOS(i)] && !eff_map[EFF_APOS(i + 1)]) {

      afl->stage_max -= 4 * ARITH_MAX;
      continue;

    }

    afl->stage_cur_byte = i;

    for (j = 1; j <= ARITH_MAX; ++j) {

      u16 r1 = orig ^ (orig + j), r2 = orig ^ (orig - j),
          r3 = orig ^ SWAP16(SWAP16(orig) + j),
          r4 = orig ^ SWAP16(SWAP16(orig) - j);

      /* Try little endian addition and subtraction first. Do it only
         if the operation would affect more than one byte (hence the
         & 0xff overflow checks) and if it couldn't be a product of
         a bitflip. */

      afl->stage_val_type = STAGE_VAL_LE;

      if ((orig & 0xff) + j > 0xff && !could_be_bitflip(r1)) {

        afl->stage_cur_val = j;
        *(u16 *)(out_buf + i) = orig + j;

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s ARITH16+-%u-%u",
                 afl->queue_cur->fname, i, j);
#endif

        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      if ((orig & 0xff) < j && !could_be_bitflip(r2)) {

        afl->stage_cur_val = -j;
        *(u16 *)(out_buf + i) = orig - j;

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s ARITH16--%u-%u",
                 afl->queue_cur->fname, i, j);
#endif

        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      /* Big endian comes next. Same deal. */

      afl->stage_val_type = STAGE_VAL_BE;

      if ((orig >> 8) + j > 0xff && !could_be_bitflip(r3)) {

        afl->stage_cur_val = j;
        *(u16 *)(out_buf + i) = SWAP16(SWAP16(orig) + j);

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s ARITH16+BE-%u-%u",
                 afl->queue_cur->fname, i, j);
#endif

        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      if ((orig >> 8) < j && !could_be_bitflip(r4)) {

        afl->stage_cur_val = -j;
        *(u16 *)(out_buf + i) = SWAP16(SWAP16(orig) - j);

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s ARITH16_BE-%u-%u",
                 afl->queue_cur->fname, i, j);
#endif

        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      *(u16 *)(out_buf + i) = orig;

    }

  }

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_ARITH16] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_ARITH16] += afl->stage_max;

  /* 32-bit arithmetics, both endians. */

  if (len < 4) { goto skip_arith; }

  afl->stage_name = "arith 32/8";
  afl->stage_short = "arith32";
  afl->stage_cur = 0;
  afl->stage_max = 4 * (len - 3) * ARITH_MAX;

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < (u32)len - 3; ++i) {

    u32 orig = *(u32 *)(out_buf + i);

    /* Let's consult the effector map... */

    if (!eff_map[EFF_APOS(i)] && !eff_map[EFF_APOS(i + 1)] &&
        !eff_map[EFF_APOS(i + 2)] && !eff_map[EFF_APOS(i + 3)]) {

      afl->stage_max -= 4 * ARITH_MAX;
      continue;

    }

    afl->stage_cur_byte = i;

    for (j = 1; j <= ARITH_MAX; ++j) {

      u32 r1 = orig ^ (orig + j), r2 = orig ^ (orig - j),
          r3 = orig ^ SWAP32(SWAP32(orig) + j),
          r4 = orig ^ SWAP32(SWAP32(orig) - j);

      /* Little endian first. Same deal as with 16-bit: we only want to
         try if the operation would have effect on more than two bytes. */

      afl->stage_val_type = STAGE_VAL_LE;

      if ((orig & 0xffff) + j > 0xffff && !could_be_bitflip(r1)) {

        afl->stage_cur_val = j;
        *(u32 *)(out_buf + i) = orig + j;

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s ARITH32+-%u-%u",
                 afl->queue_cur->fname, i, j);
#endif

        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      if ((orig & 0xffff) < (u32)j && !could_be_bitflip(r2)) {

        afl->stage_cur_val = -j;
        *(u32 *)(out_buf + i) = orig - j;

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s ARITH32_-%u-%u",
                 afl->queue_cur->fname, i, j);
#endif

        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      /* Big endian next. */

      afl->stage_val_type = STAGE_VAL_BE;

      if ((SWAP32(orig) & 0xffff) + j > 0xffff && !could_be_bitflip(r3)) {

        afl->stage_cur_val = j;
        *(u32 *)(out_buf + i) = SWAP32(SWAP32(orig) + j);

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s ARITH32+BE-%u-%u",
                 afl->queue_cur->fname, i, j);
#endif

        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      if ((SWAP32(orig) & 0xffff) < (u32)j && !could_be_bitflip(r4)) {

        afl->stage_cur_val = -j;
        *(u32 *)(out_buf + i) = SWAP32(SWAP32(orig) - j);

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s ARITH32_BE-%u-%u",
                 afl->queue_cur->fname, i, j);
#endif

        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      *(u32 *)(out_buf + i) = orig;

    }

  }

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_ARITH32] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_ARITH32] += afl->stage_max;

skip_arith:

  /**********************
   * INTERESTING VALUES *
   **********************/

  afl->stage_name = "interest 8/8";
  afl->stage_short = "int8";
  afl->stage_cur = 0;
  afl->stage_max = len * sizeof(interesting_8);

  afl->stage_val_type = STAGE_VAL_LE;

  orig_hit_cnt = new_hit_cnt;

  /* Setting 8-bit integers. */

  for (i = 0; i < (u32)len; ++i) {

    u8 orig = out_buf[i];

    /* Let's consult the effector map... */

    if (!eff_map[EFF_APOS(i)]) {

      afl->stage_max -= sizeof(interesting_8);
      continue;

    }

    afl->stage_cur_byte = i;

    for (j = 0; j < (u32)sizeof(interesting_8); ++j) {

      /* Skip if the value could be a product of bitflips or arithmetics. */

      if (could_be_bitflip(orig ^ (u8)interesting_8[j]) ||
          could_be_arith(orig, (u8)interesting_8[j], 1)) {

        --afl->stage_max;
        continue;

      }

      afl->stage_cur_val = interesting_8[j];
      out_buf[i] = interesting_8[j];

#ifdef INTROSPECTION
      snprintf(afl->mutation, sizeof(afl->mutation), "%s INTERESTING8_%u_%u",
               afl->queue_cur->fname, i, j);
#endif

      if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }

      out_buf[i] = orig;
      ++afl->stage_cur;

    }

  }

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_INTEREST8] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_INTEREST8] += afl->stage_max;

  /* Setting 16-bit integers, both endians. */

  if (afl->no_arith || len < 2) { goto skip_interest; }

  afl->stage_name = "interest 16/8";
  afl->stage_short = "int16";
  afl->stage_cur = 0;
  afl->stage_max = 2 * (len - 1) * (sizeof(interesting_16) >> 1);

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < len - 1; ++i) {

    u16 orig = *(u16 *)(out_buf + i);

    /* Let's consult the effector map... */

    if (!eff_map[EFF_APOS(i)] && !eff_map[EFF_APOS(i + 1)]) {

      afl->stage_max -= sizeof(interesting_16);
      continue;

    }

    afl->stage_cur_byte = i;

    for (j = 0; j < sizeof(interesting_16) / 2; ++j) {

      afl->stage_cur_val = interesting_16[j];

      /* Skip if this could be a product of a bitflip, arithmetics,
         or single-byte interesting value insertion. */

      if (!could_be_bitflip(orig ^ (u16)interesting_16[j]) &&
          !could_be_arith(orig, (u16)interesting_16[j], 2) &&
          !could_be_interest(orig, (u16)interesting_16[j], 2, 0)) {

        afl->stage_val_type = STAGE_VAL_LE;

        *(u16 *)(out_buf + i) = interesting_16[j];

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s INTERESTING16_%u_%u",
                 afl->queue_cur->fname, i, j);
#endif

        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      if ((u16)interesting_16[j] != SWAP16(interesting_16[j]) &&
          !could_be_bitflip(orig ^ SWAP16(interesting_16[j])) &&
          !could_be_arith(orig, SWAP16(interesting_16[j]), 2) &&
          !could_be_interest(orig, SWAP16(interesting_16[j]), 2, 1)) {

        afl->stage_val_type = STAGE_VAL_BE;

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation),
                 "%s INTERESTING16BE_%u_%u", afl->queue_cur->fname, i, j);
#endif

        *(u16 *)(out_buf + i) = SWAP16(interesting_16[j]);
        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

    }

    *(u16 *)(out_buf + i) = orig;

  }

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_INTEREST16] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_INTEREST16] += afl->stage_max;

  if (len < 4) { goto skip_interest; }

  /* Setting 32-bit integers, both endians. */

  afl->stage_name = "interest 32/8";
  afl->stage_short = "int32";
  afl->stage_cur = 0;
  afl->stage_max = 2 * (len - 3) * (sizeof(interesting_32) >> 2);

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < len - 3; i++) {

    u32 orig = *(u32 *)(out_buf + i);

    /* Let's consult the effector map... */

    if (!eff_map[EFF_APOS(i)] && !eff_map[EFF_APOS(i + 1)] &&
        !eff_map[EFF_APOS(i + 2)] && !eff_map[EFF_APOS(i + 3)]) {

      afl->stage_max -= sizeof(interesting_32) >> 1;
      continue;

    }

    afl->stage_cur_byte = i;

    for (j = 0; j < sizeof(interesting_32) / 4; ++j) {

      afl->stage_cur_val = interesting_32[j];

      /* Skip if this could be a product of a bitflip, arithmetics,
         or word interesting value insertion. */

      if (!could_be_bitflip(orig ^ (u32)interesting_32[j]) &&
          !could_be_arith(orig, interesting_32[j], 4) &&
          !could_be_interest(orig, interesting_32[j], 4, 0)) {

        afl->stage_val_type = STAGE_VAL_LE;

        *(u32 *)(out_buf + i) = interesting_32[j];

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s INTERESTING32_%u_%u",
                 afl->queue_cur->fname, i, j);
#endif

        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      if ((u32)interesting_32[j] != SWAP32(interesting_32[j]) &&
          !could_be_bitflip(orig ^ SWAP32(interesting_32[j])) &&
          !could_be_arith(orig, SWAP32(interesting_32[j]), 4) &&
          !could_be_interest(orig, SWAP32(interesting_32[j]), 4, 1)) {

        afl->stage_val_type = STAGE_VAL_BE;

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation),
                 "%s INTERESTING32BE_%u_%u", afl->queue_cur->fname, i, j);
#endif

        *(u32 *)(out_buf + i) = SWAP32(interesting_32[j]);
        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

    }

    *(u32 *)(out_buf + i) = orig;

  }

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_INTEREST32] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_INTEREST32] += afl->stage_max;

skip_interest:

  /********************
   * DICTIONARY STUFF *
   ********************/

  if (!afl->extras_cnt) { goto skip_user_extras; }

  /* Overwrite with user-supplied extras. */

  afl->stage_name = "user extras (over)";
  afl->stage_short = "ext_UO";
  afl->stage_cur = 0;
  afl->stage_max = afl->extras_cnt * len;

  afl->stage_val_type = STAGE_VAL_NONE;

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < (u32)len; ++i) {

    u32 last_len = 0;

    afl->stage_cur_byte = i;

    /* Extras are sorted by size, from smallest to largest. This means
       that we don't have to worry about restoring the buffer in
       between writes at a particular offset determined by the outer
       loop. */

    for (j = 0; j < afl->extras_cnt; ++j) {

      /* Skip extras probabilistically if afl->extras_cnt > AFL_MAX_DET_EXTRAS.
         Also skip them if there's no room to insert the payload, if the token
         is redundant, or if its entire span has no bytes set in the effector
         map. */

      if ((afl->extras_cnt > afl->max_det_extras &&
           rand_below(afl, afl->extras_cnt) >= afl->max_det_extras) ||
          afl->extras[j].len > len - i ||
          !memcmp(afl->extras[j].data, out_buf + i, afl->extras[j].len) ||
          !memchr(eff_map + EFF_APOS(i), 1,
                  EFF_SPAN_ALEN(i, afl->extras[j].len))) {

        --afl->stage_max;
        continue;

      }

      last_len = afl->extras[j].len;
      memcpy(out_buf + i, afl->extras[j].data, last_len);

#ifdef INTROSPECTION
      snprintf(afl->mutation, sizeof(afl->mutation),
               "%s EXTRAS_overwrite-%u-%u", afl->queue_cur->fname, i, j);
#endif

      if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }

      ++afl->stage_cur;

    }

    /* Restore all the clobbered memory. */
    memcpy(out_buf + i, in_buf + i, last_len);

  }

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_EXTRAS_UO] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_EXTRAS_UO] += afl->stage_max;

  /* Insertion of user-supplied extras. */

  afl->stage_name = "user extras (insert)";
  afl->stage_short = "ext_UI";
  afl->stage_cur = 0;
  afl->stage_max = afl->extras_cnt * (len + 1);

  orig_hit_cnt = new_hit_cnt;

  ex_tmp = afl_realloc(AFL_BUF_PARAM(ex), len + MAX_DICT_FILE);
  if (unlikely(!ex_tmp)) { PFATAL("alloc"); }

  for (i = 0; i <= (u32)len; ++i) {

    afl->stage_cur_byte = i;

    for (j = 0; j < afl->extras_cnt; ++j) {

      if (len + afl->extras[j].len > MAX_FILE) {

        --afl->stage_max;
        continue;

      }

      /* Insert token */
      memcpy(ex_tmp + i, afl->extras[j].data, afl->extras[j].len);

      /* Copy tail */
      memcpy(ex_tmp + i + afl->extras[j].len, out_buf + i, len - i);

#ifdef INTROSPECTION
      snprintf(afl->mutation, sizeof(afl->mutation), "%s EXTRAS_insert-%u-%u",
               afl->queue_cur->fname, i, j);
#endif

      if (common_fuzz_stuff(afl, ex_tmp, len + afl->extras[j].len)) {

        goto abandon_entry;

      }

      ++afl->stage_cur;

    }

    /* Copy head */
    ex_tmp[i] = out_buf[i];

  }

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_EXTRAS_UI] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_EXTRAS_UI] += afl->stage_max;

skip_user_extras:

  if (!afl->a_extras_cnt) { goto skip_extras; }

  afl->stage_name = "auto extras (over)";
  afl->stage_short = "ext_AO";
  afl->stage_cur = 0;
  afl->stage_max = MIN(afl->a_extras_cnt, (u32)USE_AUTO_EXTRAS) * len;

  afl->stage_val_type = STAGE_VAL_NONE;

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < (u32)len; ++i) {

    u32 last_len = 0;

    afl->stage_cur_byte = i;

    u32 min_extra_len = MIN(afl->a_extras_cnt, (u32)USE_AUTO_EXTRAS);
    for (j = 0; j < min_extra_len; ++j) {

      /* See the comment in the earlier code; extras are sorted by size. */

      if (afl->a_extras[j].len > len - i ||
          !memcmp(afl->a_extras[j].data, out_buf + i, afl->a_extras[j].len) ||
          !memchr(eff_map + EFF_APOS(i), 1,
                  EFF_SPAN_ALEN(i, afl->a_extras[j].len))) {

        --afl->stage_max;
        continue;

      }

      last_len = afl->a_extras[j].len;
      memcpy(out_buf + i, afl->a_extras[j].data, last_len);

#ifdef INTROSPECTION
      snprintf(afl->mutation, sizeof(afl->mutation),
               "%s AUTO_EXTRAS_overwrite-%u-%u", afl->queue_cur->fname, i, j);
#endif

      if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }

      ++afl->stage_cur;

    }

    /* Restore all the clobbered memory. */
    memcpy(out_buf + i, in_buf + i, last_len);

  }

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_EXTRAS_AO] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_EXTRAS_AO] += afl->stage_max;

skip_extras:

  /* If we made this to here without jumping to havoc_stage or abandon_entry,
     we're properly done with deterministic steps and can mark it as such
     in the .state/ directory. */

  if (!afl->queue_cur->passed_det) { mark_as_det_done(afl, afl->queue_cur); }

custom_mutator_stage:
  /*******************
   * CUSTOM MUTATORS *
   *******************/

  if (likely(!afl->custom_mutators_count)) { goto havoc_stage; }

  afl->stage_name = "custom mutator";
  afl->stage_short = "custom";
  afl->stage_max = HAVOC_CYCLES * perf_score / afl->havoc_div / 100;
  afl->stage_val_type = STAGE_VAL_NONE;
  bool has_custom_fuzz = false;

  if (afl->stage_max < HAVOC_MIN) { afl->stage_max = HAVOC_MIN; }

  const u32 max_seed_size = MAX_FILE, saved_max = afl->stage_max;

  orig_hit_cnt = afl->queued_paths + afl->unique_crashes;

#ifdef INTROSPECTION
  afl->mutation[0] = 0;
#endif

  LIST_FOREACH(&afl->custom_mutator_list, struct custom_mutator, {

    if (el->afl_custom_fuzz) {

      afl->current_custom_fuzz = el;

      if (el->afl_custom_fuzz_count) {

        afl->stage_max = el->afl_custom_fuzz_count(el->data, out_buf, len);

      } else {

        afl->stage_max = saved_max;

      }

      has_custom_fuzz = true;

      afl->stage_short = el->name_short;

      if (afl->stage_max) {

        for (afl->stage_cur = 0; afl->stage_cur < afl->stage_max;
             ++afl->stage_cur) {

          struct queue_entry *target = NULL;
          u32                 tid;
          u8 *                new_buf = NULL;
          u32                 target_len = 0;

          /* check if splicing makes sense yet (enough entries) */
          if (likely(afl->ready_for_splicing_count > 1)) {

            /* Pick a random other queue entry for passing to external API
               that has the necessary length */

            do {

              tid = rand_below(afl, afl->queued_paths);

            } while (unlikely(tid == afl->current_entry ||

                              afl->queue_buf[tid]->len < 4));

            target = afl->queue_buf[tid];
            afl->splicing_with = tid;

            /* Read the additional testcase into a new buffer. */
            new_buf = queue_testcase_get(afl, target);
            target_len = target->len;

          }

          u8 *mutated_buf = NULL;

          size_t mutated_size =
              el->afl_custom_fuzz(el->data, out_buf, len, &mutated_buf, new_buf,
                                  target_len, max_seed_size);

          if (unlikely(!mutated_buf)) {

            FATAL("Error in custom_fuzz. Size returned: %zu", mutated_size);

          }

          if (mutated_size > 0) {

            if (common_fuzz_stuff(afl, mutated_buf, (u32)mutated_size)) {

              goto abandon_entry;

            }

            if (!el->afl_custom_fuzz_count) {

              /* If we're finding new stuff, let's run for a bit longer, limits
                permitting. */

              if (afl->queued_paths != havoc_queued) {

                if (perf_score <= afl->havoc_max_mult * 100) {

                  afl->stage_max *= 2;
                  perf_score *= 2;

                }

                havoc_queued = afl->queued_paths;

              }

            }

          }

          /* `(afl->)out_buf` may have been changed by the call to custom_fuzz
           */
          /* TODO: Only do this when `mutated_buf` == `out_buf`? Branch vs
           * Memcpy.
           */
          memcpy(out_buf, in_buf, len);

        }

      }

    }

  });

  afl->current_custom_fuzz = NULL;

  if (!has_custom_fuzz) goto havoc_stage;

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_CUSTOM_MUTATOR] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_CUSTOM_MUTATOR] += afl->stage_max;

  if (likely(afl->custom_only)) {

    /* Skip other stages */
    ret_val = 0;
    goto abandon_entry;

  }

  /****************
   * RANDOM HAVOC *
   ****************/

havoc_stage:

  afl->stage_cur_byte = -1;

  /* The havoc stage mutation code is also invoked when splicing files; if the
     splice_cycle variable is set, generate different descriptions and such. */

  if (!splice_cycle) {

    afl->stage_name = "havoc";
    afl->stage_short = "havoc";
    afl->stage_max = (doing_det ? HAVOC_CYCLES_INIT : HAVOC_CYCLES) *
                     perf_score / afl->havoc_div / 100;

  } else {

    perf_score = orig_perf;

    snprintf(afl->stage_name_buf, STAGE_BUF_SIZE, "splice %u", splice_cycle);
    afl->stage_name = afl->stage_name_buf;
    afl->stage_short = "splice";
    afl->stage_max = SPLICE_HAVOC * perf_score / afl->havoc_div / 100;

  }

  if (afl->stage_max < HAVOC_MIN) { afl->stage_max = HAVOC_MIN; }

  temp_len = len;

  orig_hit_cnt = afl->queued_paths + afl->unique_crashes;

  havoc_queued = afl->queued_paths;

  if (afl->custom_mutators_count) {

    LIST_FOREACH(&afl->custom_mutator_list, struct custom_mutator, {

      if (el->stacked_custom && el->afl_custom_havoc_mutation_probability) {

        el->stacked_custom_prob =
            el->afl_custom_havoc_mutation_probability(el->data);
        if (el->stacked_custom_prob > 100) {

          FATAL(
              "The probability returned by "
              "afl_custom_havoc_mutation_propability "
              "has to be in the range 0-100.");

        }

      }

    });

  }

  /* We essentially just do several thousand runs (depending on perf_score)
     where we take the input file and make random stacked tweaks. */

#define MAX_HAVOC_ENTRY 59                                      /* 55 to 60 */

  u32 r_max, r;

  r_max = (MAX_HAVOC_ENTRY + 1) + (afl->extras_cnt ? 4 : 0) +
          (afl->a_extras_cnt ? 4 : 0);

  if (unlikely(afl->expand_havoc && afl->ready_for_splicing_count > 1)) {

    /* add expensive havoc cases here, they are activated after a full
       cycle without finds happened */

    r_max += 4;

  }

  if (unlikely(get_cur_time() - afl->last_path_time > 5000 /* 5 seconds */ &&
               afl->ready_for_splicing_count > 1)) {

    /* add expensive havoc cases here if there is no findings in the last 5s */

    r_max += 4;

  }

  for (afl->stage_cur = 0; afl->stage_cur < afl->stage_max; ++afl->stage_cur) {

    u32 use_stacking = 1 << (1 + rand_below(afl, afl->havoc_stack_pow2));

    afl->stage_cur_val = use_stacking;

#ifdef INTROSPECTION
    snprintf(afl->mutation, sizeof(afl->mutation), "%s HAVOC-%u",
             afl->queue_cur->fname, use_stacking);
#endif

    for (i = 0; i < use_stacking; ++i) {

      if (afl->custom_mutators_count) {

        LIST_FOREACH(&afl->custom_mutator_list, struct custom_mutator, {

          if (el->stacked_custom &&
              rand_below(afl, 100) < el->stacked_custom_prob) {

            u8 *   custom_havoc_buf = NULL;
            size_t new_len = el->afl_custom_havoc_mutation(
                el->data, out_buf, temp_len, &custom_havoc_buf, MAX_FILE);
            if (unlikely(!custom_havoc_buf)) {

              FATAL("Error in custom_havoc (return %zu)", new_len);

            }

            if (likely(new_len > 0 && custom_havoc_buf)) {

              temp_len = new_len;
              if (out_buf != custom_havoc_buf) {

                afl_realloc(AFL_BUF_PARAM(out), temp_len);
                if (unlikely(!afl->out_buf)) { PFATAL("alloc"); }
                memcpy(out_buf, custom_havoc_buf, temp_len);

              }

            }

          }

        });

      }

      switch ((r = rand_below(afl, r_max))) {

        case 0 ... 3: {

          /* Flip a single bit somewhere. Spooky! */

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " FLIP_BIT1");
          strcat(afl->mutation, afl->m_tmp);
#endif
          FLIP_BIT(out_buf, rand_below(afl, temp_len << 3));
          break;

        }

        case 4 ... 7: {

          /* Set byte to interesting value. */

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " INTERESTING8");
          strcat(afl->mutation, afl->m_tmp);
#endif
          out_buf[rand_below(afl, temp_len)] =
              interesting_8[rand_below(afl, sizeof(interesting_8))];
          break;

        }

        case 8 ... 9: {

          /* Set word to interesting value, randomly choosing endian. */

          if (temp_len < 2) { break; }

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " INTERESTING16");
          strcat(afl->mutation, afl->m_tmp);
#endif
          *(u16 *)(out_buf + rand_below(afl, temp_len - 1)) =
              interesting_16[rand_below(afl, sizeof(interesting_16) >> 1)];

          break;

        }

        case 10 ... 11: {

          /* Set word to interesting value, randomly choosing endian. */

          if (temp_len < 2) { break; }

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " INTERESTING16BE");
          strcat(afl->mutation, afl->m_tmp);
#endif
          *(u16 *)(out_buf + rand_below(afl, temp_len - 1)) = SWAP16(
              interesting_16[rand_below(afl, sizeof(interesting_16) >> 1)]);

          break;

        }

        case 12 ... 13: {

          /* Set dword to interesting value, randomly choosing endian. */

          if (temp_len < 4) { break; }

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " INTERESTING32");
          strcat(afl->mutation, afl->m_tmp);
#endif
          *(u32 *)(out_buf + rand_below(afl, temp_len - 3)) =
              interesting_32[rand_below(afl, sizeof(interesting_32) >> 2)];

          break;

        }

        case 14 ... 15: {

          /* Set dword to interesting value, randomly choosing endian. */

          if (temp_len < 4) { break; }

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " INTERESTING32BE");
          strcat(afl->mutation, afl->m_tmp);
#endif
          *(u32 *)(out_buf + rand_below(afl, temp_len - 3)) = SWAP32(
              interesting_32[rand_below(afl, sizeof(interesting_32) >> 2)]);

          break;

        }

        case 16 ... 19: {

          /* Randomly subtract from byte. */

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH8_");
          strcat(afl->mutation, afl->m_tmp);
#endif
          out_buf[rand_below(afl, temp_len)] -= 1 + rand_below(afl, ARITH_MAX);
          break;

        }

        case 20 ... 23: {

          /* Randomly add to byte. */

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH8+");
          strcat(afl->mutation, afl->m_tmp);
#endif
          out_buf[rand_below(afl, temp_len)] += 1 + rand_below(afl, ARITH_MAX);
          break;

        }

        case 24 ... 25: {

          /* Randomly subtract from word, little endian. */

          if (temp_len < 2) { break; }

          u32 pos = rand_below(afl, temp_len - 1);

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH16_-%u", pos);
          strcat(afl->mutation, afl->m_tmp);
#endif
          *(u16 *)(out_buf + pos) -= 1 + rand_below(afl, ARITH_MAX);

          break;

        }

        case 26 ... 27: {

          /* Randomly subtract from word, big endian. */

          if (temp_len < 2) { break; }

          u32 pos = rand_below(afl, temp_len - 1);
          u16 num = 1 + rand_below(afl, ARITH_MAX);

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH16_BE-%u_%u", pos,
                   num);
          strcat(afl->mutation, afl->m_tmp);
#endif
          *(u16 *)(out_buf + pos) =
              SWAP16(SWAP16(*(u16 *)(out_buf + pos)) - num);

          break;

        }

        case 28 ... 29: {

          /* Randomly add to word, little endian. */

          if (temp_len < 2) { break; }

          u32 pos = rand_below(afl, temp_len - 1);

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH16+-%u", pos);
          strcat(afl->mutation, afl->m_tmp);
#endif
          *(u16 *)(out_buf + pos) += 1 + rand_below(afl, ARITH_MAX);

          break;

        }

        case 30 ... 31: {

          /* Randomly add to word, big endian. */

          if (temp_len < 2) { break; }

          u32 pos = rand_below(afl, temp_len - 1);
          u16 num = 1 + rand_below(afl, ARITH_MAX);

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH16+BE-%u_%u", pos,
                   num);
          strcat(afl->mutation, afl->m_tmp);
#endif
          *(u16 *)(out_buf + pos) =
              SWAP16(SWAP16(*(u16 *)(out_buf + pos)) + num);

          break;

        }

        case 32 ... 33: {

          /* Randomly subtract from dword, little endian. */

          if (temp_len < 4) { break; }

          u32 pos = rand_below(afl, temp_len - 3);

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH32_-%u", pos);
          strcat(afl->mutation, afl->m_tmp);
#endif
          *(u32 *)(out_buf + pos) -= 1 + rand_below(afl, ARITH_MAX);

          break;

        }

        case 34 ... 35: {

          /* Randomly subtract from dword, big endian. */

          if (temp_len < 4) { break; }

          u32 pos = rand_below(afl, temp_len - 3);
          u32 num = 1 + rand_below(afl, ARITH_MAX);

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH32_BE-%u-%u", pos,
                   num);
          strcat(afl->mutation, afl->m_tmp);
#endif
          *(u32 *)(out_buf + pos) =
              SWAP32(SWAP32(*(u32 *)(out_buf + pos)) - num);

          break;

        }

        case 36 ... 37: {

          /* Randomly add to dword, little endian. */

          if (temp_len < 4) { break; }

          u32 pos = rand_below(afl, temp_len - 3);

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH32+-%u", pos);
          strcat(afl->mutation, afl->m_tmp);
#endif
          *(u32 *)(out_buf + pos) += 1 + rand_below(afl, ARITH_MAX);

          break;

        }

        case 38 ... 39: {

          /* Randomly add to dword, big endian. */

          if (temp_len < 4) { break; }

          u32 pos = rand_below(afl, temp_len - 3);
          u32 num = 1 + rand_below(afl, ARITH_MAX);

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH32+BE-%u-%u", pos,
                   num);
          strcat(afl->mutation, afl->m_tmp);
#endif
          *(u32 *)(out_buf + pos) =
              SWAP32(SWAP32(*(u32 *)(out_buf + pos)) + num);

          break;

        }

        case 40 ... 43: {

          /* Just set a random byte to a random value. Because,
             why not. We use XOR with 1-255 to eliminate the
             possibility of a no-op. */

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " RAND8");
          strcat(afl->mutation, afl->m_tmp);
#endif
          out_buf[rand_below(afl, temp_len)] ^= 1 + rand_below(afl, 255);
          break;

        }

        case 44 ... 46: {

          if (temp_len + HAVOC_BLK_XL < MAX_FILE) {

            /* Clone bytes. */

            u32 clone_len = choose_block_len(afl, temp_len);
            u32 clone_from = rand_below(afl, temp_len - clone_len + 1);
            u32 clone_to = rand_below(afl, temp_len);

#ifdef INTROSPECTION
            snprintf(afl->m_tmp, sizeof(afl->m_tmp), " CLONE-%s-%u-%u-%u",
                     "clone", clone_from, clone_to, clone_len);
            strcat(afl->mutation, afl->m_tmp);
#endif
            u8 *new_buf =
                afl_realloc(AFL_BUF_PARAM(out_scratch), temp_len + clone_len);
            if (unlikely(!new_buf)) { PFATAL("alloc"); }

            /* Head */

            memcpy(new_buf, out_buf, clone_to);

            /* Inserted part */

            memcpy(new_buf + clone_to, out_buf + clone_from, clone_len);

            /* Tail */
            memcpy(new_buf + clone_to + clone_len, out_buf + clone_to,
                   temp_len - clone_to);

            out_buf = new_buf;
            afl_swap_bufs(AFL_BUF_PARAM(out), AFL_BUF_PARAM(out_scratch));
            temp_len += clone_len;

          }

          break;

        }

        case 47: {

          if (temp_len + HAVOC_BLK_XL < MAX_FILE) {

            /* Insert a block of constant bytes (25%). */

            u32 clone_len = choose_block_len(afl, HAVOC_BLK_XL);
            u32 clone_to = rand_below(afl, temp_len);

#ifdef INTROSPECTION
            snprintf(afl->m_tmp, sizeof(afl->m_tmp), " CLONE-%s-%u-%u",
                     "insert", clone_to, clone_len);
            strcat(afl->mutation, afl->m_tmp);
#endif
            u8 *new_buf =
                afl_realloc(AFL_BUF_PARAM(out_scratch), temp_len + clone_len);
            if (unlikely(!new_buf)) { PFATAL("alloc"); }

            /* Head */

            memcpy(new_buf, out_buf, clone_to);

            /* Inserted part */

            memset(new_buf + clone_to,
                   rand_below(afl, 2) ? rand_below(afl, 256)
                                      : out_buf[rand_below(afl, temp_len)],
                   clone_len);

            /* Tail */
            memcpy(new_buf + clone_to + clone_len, out_buf + clone_to,
                   temp_len - clone_to);

            out_buf = new_buf;
            afl_swap_bufs(AFL_BUF_PARAM(out), AFL_BUF_PARAM(out_scratch));
            temp_len += clone_len;

          }

          break;

        }

        case 48 ... 50: {

          /* Overwrite bytes with a randomly selected chunk bytes. */

          if (temp_len < 2) { break; }

          u32 copy_len = choose_block_len(afl, temp_len - 1);
          u32 copy_from = rand_below(afl, temp_len - copy_len + 1);
          u32 copy_to = rand_below(afl, temp_len - copy_len + 1);

          if (likely(copy_from != copy_to)) {

#ifdef INTROSPECTION
            snprintf(afl->m_tmp, sizeof(afl->m_tmp), " OVERWRITE_COPY-%u-%u-%u",
                     copy_from, copy_to, copy_len);
            strcat(afl->mutation, afl->m_tmp);
#endif
            memmove(out_buf + copy_to, out_buf + copy_from, copy_len);

          }

          break;

        }

        case 51: {

          /* Overwrite bytes with fixed bytes. */

          if (temp_len < 2) { break; }

          u32 copy_len = choose_block_len(afl, temp_len - 1);
          u32 copy_to = rand_below(afl, temp_len - copy_len + 1);

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " OVERWRITE_FIXED-%u-%u",
                   copy_to, copy_len);
          strcat(afl->mutation, afl->m_tmp);
#endif
          memset(out_buf + copy_to,
                 rand_below(afl, 2) ? rand_below(afl, 256)
                                    : out_buf[rand_below(afl, temp_len)],
                 copy_len);

          break;

        }

        // increase from 4 up to 8?
        case 52 ... MAX_HAVOC_ENTRY: {

          /* Delete bytes. We're making this a bit more likely
             than insertion (the next option) in hopes of keeping
             files reasonably small. */

          if (temp_len < 2) { break; }

          /* Don't delete too much. */

          u32 del_len = choose_block_len(afl, temp_len - 1);
          u32 del_from = rand_below(afl, temp_len - del_len + 1);

#ifdef INTROSPECTION
          snprintf(afl->m_tmp, sizeof(afl->m_tmp), " DEL-%u-%u", del_from,
                   del_len);
          strcat(afl->mutation, afl->m_tmp);
#endif
          memmove(out_buf + del_from, out_buf + del_from + del_len,
                  temp_len - del_from - del_len);

          temp_len -= del_len;

          break;

        }

        default:

          r -= (MAX_HAVOC_ENTRY + 1);

          if (afl->extras_cnt) {

            if (r < 2) {

              /* Use the dictionary. */

              u32 use_extra = rand_below(afl, afl->extras_cnt);
              u32 extra_len = afl->extras[use_extra].len;

              if (extra_len > temp_len) { break; }

              u32 insert_at = rand_below(afl, temp_len - extra_len + 1);
#ifdef INTROSPECTION
              snprintf(afl->m_tmp, sizeof(afl->m_tmp), " EXTRA_OVERWRITE-%u-%u",
                       insert_at, extra_len);
              strcat(afl->mutation, afl->m_tmp);
#endif
              memcpy(out_buf + insert_at, afl->extras[use_extra].data,
                     extra_len);

              break;

            } else if (r < 4) {

              u32 use_extra = rand_below(afl, afl->extras_cnt);
              u32 extra_len = afl->extras[use_extra].len;
              if (temp_len + extra_len >= MAX_FILE) { break; }

              u8 *ptr = afl->extras[use_extra].data;
              u32 insert_at = rand_below(afl, temp_len + 1);
#ifdef INTROSPECTION
              snprintf(afl->m_tmp, sizeof(afl->m_tmp), " EXTRA_INSERT-%u-%u",
                       insert_at, extra_len);
              strcat(afl->mutation, afl->m_tmp);
#endif

              out_buf = afl_realloc(AFL_BUF_PARAM(out), temp_len + extra_len);
              if (unlikely(!out_buf)) { PFATAL("alloc"); }

              /* Tail */
              memmove(out_buf + insert_at + extra_len, out_buf + insert_at,
                      temp_len - insert_at);

              /* Inserted part */
              memcpy(out_buf + insert_at, ptr, extra_len);
              temp_len += extra_len;

              break;

            } else {

              r -= 4;

            }

          }

          if (afl->a_extras_cnt) {

            if (r < 2) {

              /* Use the dictionary. */

              u32 use_extra = rand_below(afl, afl->a_extras_cnt);
              u32 extra_len = afl->a_extras[use_extra].len;

              if (extra_len > temp_len) { break; }

              u32 insert_at = rand_below(afl, temp_len - extra_len + 1);
#ifdef INTROSPECTION
              snprintf(afl->m_tmp, sizeof(afl->m_tmp),
                       " AUTO_EXTRA_OVERWRITE-%u-%u", insert_at, extra_len);
              strcat(afl->mutation, afl->m_tmp);
#endif
              memcpy(out_buf + insert_at, afl->a_extras[use_extra].data,
                     extra_len);

              break;

            } else if (r < 4) {

              u32 use_extra = rand_below(afl, afl->a_extras_cnt);
              u32 extra_len = afl->a_extras[use_extra].len;
              if (temp_len + extra_len >= MAX_FILE) { break; }

              u8 *ptr = afl->a_extras[use_extra].data;
              u32 insert_at = rand_below(afl, temp_len + 1);
#ifdef INTROSPECTION
              snprintf(afl->m_tmp, sizeof(afl->m_tmp),
                       " AUTO_EXTRA_INSERT-%u-%u", insert_at, extra_len);
              strcat(afl->mutation, afl->m_tmp);
#endif

              out_buf = afl_realloc(AFL_BUF_PARAM(out), temp_len + extra_len);
              if (unlikely(!out_buf)) { PFATAL("alloc"); }

              /* Tail */
              memmove(out_buf + insert_at + extra_len, out_buf + insert_at,
                      temp_len - insert_at);

              /* Inserted part */
              memcpy(out_buf + insert_at, ptr, extra_len);
              temp_len += extra_len;

              break;

            } else {

              r -= 4;

            }

          }

          /* Splicing otherwise if we are still here.
             Overwrite bytes with a randomly selected chunk from another
             testcase or insert that chunk. */

          /* Pick a random queue entry and seek to it. */

          u32 tid;
          do {

            tid = rand_below(afl, afl->queued_paths);

          } while (tid == afl->current_entry || afl->queue_buf[tid]->len < 4);

          /* Get the testcase for splicing. */
          struct queue_entry *target = afl->queue_buf[tid];
          u32                 new_len = target->len;
          u8 *                new_buf = queue_testcase_get(afl, target);

          if ((temp_len >= 2 && r % 2) || temp_len + HAVOC_BLK_XL >= MAX_FILE) {

            /* overwrite mode */

            u32 copy_from, copy_to, copy_len;

            copy_len = choose_block_len(afl, new_len - 1);
            if (copy_len > temp_len) copy_len = temp_len;

            copy_from = rand_below(afl, new_len - copy_len + 1);
            copy_to = rand_below(afl, temp_len - copy_len + 1);

#ifdef INTROSPECTION
            snprintf(afl->m_tmp, sizeof(afl->m_tmp),
                     " SPLICE_OVERWRITE-%u-%u-%u-%s", copy_from, copy_to,
                     copy_len, target->fname);
            strcat(afl->mutation, afl->m_tmp);
#endif
            memmove(out_buf + copy_to, new_buf + copy_from, copy_len);

          } else {

            /* insert mode */

            u32 clone_from, clone_to, clone_len;

            clone_len = choose_block_len(afl, new_len);
            clone_from = rand_below(afl, new_len - clone_len + 1);
            clone_to = rand_below(afl, temp_len + 1);

            u8 *temp_buf = afl_realloc(AFL_BUF_PARAM(out_scratch),
                                       temp_len + clone_len + 1);
            if (unlikely(!temp_buf)) { PFATAL("alloc"); }

#ifdef INTROSPECTION
            snprintf(afl->m_tmp, sizeof(afl->m_tmp),
                     " SPLICE_INSERT-%u-%u-%u-%s", clone_from, clone_to,
                     clone_len, target->fname);
            strcat(afl->mutation, afl->m_tmp);
#endif
            /* Head */

            memcpy(temp_buf, out_buf, clone_to);

            /* Inserted part */

            memcpy(temp_buf + clone_to, new_buf + clone_from, clone_len);

            /* Tail */
            memcpy(temp_buf + clone_to + clone_len, out_buf + clone_to,
                   temp_len - clone_to);

            out_buf = temp_buf;
            afl_swap_bufs(AFL_BUF_PARAM(out), AFL_BUF_PARAM(out_scratch));
            temp_len += clone_len;

          }

          break;

          // end of default

      }

    }

    if (common_fuzz_stuff(afl, out_buf, temp_len)) { goto abandon_entry; }

    /* out_buf might have been mangled a bit, so let's restore it to its
       original size and shape. */

    out_buf = afl_realloc(AFL_BUF_PARAM(out), len);
    if (unlikely(!out_buf)) { PFATAL("alloc"); }
    temp_len = len;
    memcpy(out_buf, in_buf, len);

    /* If we're finding new stuff, let's run for a bit longer, limits
       permitting. */

    if (afl->queued_paths != havoc_queued) {

      if (perf_score <= afl->havoc_max_mult * 100) {

        afl->stage_max *= 2;
        perf_score *= 2;

      }

      havoc_queued = afl->queued_paths;

    }

  }

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  if (!splice_cycle) {

    afl->stage_finds[STAGE_HAVOC] += new_hit_cnt - orig_hit_cnt;
    afl->stage_cycles[STAGE_HAVOC] += afl->stage_max;

  } else {

    afl->stage_finds[STAGE_SPLICE] += new_hit_cnt - orig_hit_cnt;
    afl->stage_cycles[STAGE_SPLICE] += afl->stage_max;

  }

#ifndef IGNORE_FINDS

  /************
   * SPLICING *
   ************/

  /* This is a last-resort strategy triggered by a full round with no findings.
     It takes the current input file, randomly selects another input, and
     splices them together at some offset, then relies on the havoc
     code to mutate that blob. */

retry_splicing:

  if (afl->use_splicing && splice_cycle++ < SPLICE_CYCLES &&
      afl->ready_for_splicing_count > 1 && afl->queue_cur->len >= 4) {

    struct queue_entry *target;
    u32                 tid, split_at;
    u8 *                new_buf;
    s32                 f_diff, l_diff;

    /* First of all, if we've modified in_buf for havoc, let's clean that
       up... */

    if (in_buf != orig_in) {

      in_buf = orig_in;
      len = afl->queue_cur->len;

    }

    /* Pick a random queue entry and seek to it. Don't splice with yourself. */

    do {

      tid = rand_below(afl, afl->queued_paths);

    } while (tid == afl->current_entry || afl->queue_buf[tid]->len < 4);

    /* Get the testcase */
    afl->splicing_with = tid;
    target = afl->queue_buf[tid];
    new_buf = queue_testcase_get(afl, target);

    /* Find a suitable splicing location, somewhere between the first and
       the last differing byte. Bail out if the difference is just a single
       byte or so. */

    locate_diffs(in_buf, new_buf, MIN(len, (s64)target->len), &f_diff, &l_diff);

    if (f_diff < 0 || l_diff < 2 || f_diff == l_diff) { goto retry_splicing; }

    /* Split somewhere between the first and last differing byte. */

    split_at = f_diff + rand_below(afl, l_diff - f_diff);

    /* Do the thing. */

    len = target->len;
    afl->in_scratch_buf = afl_realloc(AFL_BUF_PARAM(in_scratch), len);
    memcpy(afl->in_scratch_buf, in_buf, split_at);
    memcpy(afl->in_scratch_buf + split_at, new_buf, len - split_at);
    in_buf = afl->in_scratch_buf;
    afl_swap_bufs(AFL_BUF_PARAM(in), AFL_BUF_PARAM(in_scratch));

    out_buf = afl_realloc(AFL_BUF_PARAM(out), len);
    if (unlikely(!out_buf)) { PFATAL("alloc"); }
    memcpy(out_buf, in_buf, len);

    goto custom_mutator_stage;

  }

#endif                                                     /* !IGNORE_FINDS */

  ret_val = 0;

/* we are through with this queue entry - for this iteration */
abandon_entry:

  afl->splicing_with = -1;

  /* Update afl->pending_not_fuzzed count if we made it through the calibration
     cycle and have not seen this entry before. */

  if (!afl->stop_soon && !afl->queue_cur->cal_failed &&
      (afl->queue_cur->was_fuzzed == 0 || afl->queue_cur->fuzz_level == 0) &&
      !afl->queue_cur->disabled) {

    if (!afl->queue_cur->was_fuzzed) {

      --afl->pending_not_fuzzed;
      afl->queue_cur->was_fuzzed = 1;
      afl->reinit_table = 1;
      if (afl->queue_cur->favored) { --afl->pending_favored; }

    }

  }

  ++afl->queue_cur->fuzz_level;
  orig_in = NULL;
  return ret_val;

#undef FLIP_BIT

}

/* MOpt mode */
static u8 mopt_common_fuzzing(afl_state_t *afl, MOpt_globals_t MOpt_globals) {

  if (!MOpt_globals.is_pilot_mode) {

    if (swarm_num == 1) {

      afl->key_module = 2;
      return 0;

    }

  }

  u32 len, temp_len;
  u32 i;
  u32 j;
  u8 *in_buf, *out_buf, *orig_in, *ex_tmp, *eff_map = 0;
  u64 havoc_queued = 0, orig_hit_cnt, new_hit_cnt = 0, cur_ms_lv, prev_cksum;
  u32 splice_cycle = 0, perf_score = 100, orig_perf, eff_cnt = 1;

  u8 ret_val = 1, doing_det = 0;

  u8  a_collect[MAX_AUTO_EXTRA];
  u32 a_len = 0;

#ifdef IGNORE_FINDS

  /* In IGNORE_FINDS mode, skip any entries that weren't in the
     initial data set. */

  if (afl->queue_cur->depth > 1) return 1;

#else

  if (likely(afl->pending_favored)) {

    /* If we have any favored, non-fuzzed new arrivals in the queue,
       possibly skip to them at the expense of already-fuzzed or non-favored
       cases. */

    if (((afl->queue_cur->was_fuzzed > 0 || afl->queue_cur->fuzz_level > 0) ||
         !afl->queue_cur->favored) &&
        rand_below(afl, 100) < SKIP_TO_NEW_PROB) {

      return 1;

    }

  } else if (!afl->non_instrumented_mode && !afl->queue_cur->favored &&

             afl->queued_paths > 10) {

    /* Otherwise, still possibly skip non-favored cases, albeit less often.
       The odds of skipping stuff are higher for already-fuzzed inputs and
       lower for never-fuzzed entries. */

    if (afl->queue_cycle > 1 &&
        (afl->queue_cur->fuzz_level == 0 || afl->queue_cur->was_fuzzed)) {

      if (likely(rand_below(afl, 100) < SKIP_NFAV_NEW_PROB)) { return 1; }

    } else {

      if (likely(rand_below(afl, 100) < SKIP_NFAV_OLD_PROB)) { return 1; }

    }

  }

#endif                                                     /* ^IGNORE_FINDS */

  if (afl->not_on_tty) {

    ACTF("Fuzzing test case #%u (%u total, %llu uniq crashes found)...",
         afl->current_entry, afl->queued_paths, afl->unique_crashes);
    fflush(stdout);

  }

  /* Map the test case into memory. */
  orig_in = in_buf = queue_testcase_get(afl, afl->queue_cur);
  len = afl->queue_cur->len;

  out_buf = afl_realloc(AFL_BUF_PARAM(out), len);
  if (unlikely(!out_buf)) { PFATAL("alloc"); }

  afl->subseq_tmouts = 0;

  afl->cur_depth = afl->queue_cur->depth;

  /*******************************************
   * CALIBRATION (only if failed earlier on) *
   *******************************************/

  if (unlikely(afl->queue_cur->cal_failed)) {

    u8 res = FSRV_RUN_TMOUT;

    if (afl->queue_cur->cal_failed < CAL_CHANCES) {

      afl->queue_cur->exec_cksum = 0;

      res =
          calibrate_case(afl, afl->queue_cur, in_buf, afl->queue_cycle - 1, 0);

      if (res == FSRV_RUN_ERROR) {

        FATAL("Unable to execute target application");

      }

    }

    if (afl->stop_soon || res != afl->crash_mode) {

      ++afl->cur_skipped_paths;
      goto abandon_entry;

    }

  }

  /************
   * TRIMMING *
   ************/

  if (unlikely(!afl->non_instrumented_mode && !afl->queue_cur->trim_done &&
               !afl->disable_trim)) {

    u32 old_len = afl->queue_cur->len;

    u8 res = trim_case(afl, afl->queue_cur, in_buf);
    orig_in = in_buf = queue_testcase_get(afl, afl->queue_cur);

    if (unlikely(res == FSRV_RUN_ERROR)) {

      FATAL("Unable to execute target application");

    }

    if (unlikely(afl->stop_soon)) {

      ++afl->cur_skipped_paths;
      goto abandon_entry;

    }

    /* Don't retry trimming, even if it failed. */

    afl->queue_cur->trim_done = 1;

    len = afl->queue_cur->len;

    /* maybe current entry is not ready for splicing anymore */
    if (unlikely(len <= 4 && old_len > 4)) --afl->ready_for_splicing_count;

  }

  memcpy(out_buf, in_buf, len);

  /*********************
   * PERFORMANCE SCORE *
   *********************/

  if (likely(!afl->old_seed_selection))
    orig_perf = perf_score = afl->queue_cur->perf_score;
  else
    orig_perf = perf_score = calculate_score(afl, afl->queue_cur);

  if (unlikely(perf_score <= 0)) { goto abandon_entry; }

  if (unlikely(afl->shm.cmplog_mode &&
               afl->queue_cur->colorized < afl->cmplog_lvl &&
               (u32)len <= afl->cmplog_max_filesize)) {

    if (unlikely(len < 4)) {

      afl->queue_cur->colorized = CMPLOG_LVL_MAX;

    } else {

      if (afl->cmplog_lvl == 3 ||
          (afl->cmplog_lvl == 2 && afl->queue_cur->tc_ref) ||
          !(afl->fsrv.total_execs % afl->queued_paths) ||
          get_cur_time() - afl->last_path_time > 300000) {  // 300 seconds

        if (input_to_state_stage(afl, in_buf, out_buf, len)) {

          goto abandon_entry;

        }

      }

    }

  }

  /* Go to pacemker fuzzing if MOpt is doing well */

  cur_ms_lv = get_cur_time();
  if (!(afl->key_puppet == 0 &&
        ((cur_ms_lv - afl->last_path_time < (u32)afl->limit_time_puppet) ||
         (afl->last_crash_time != 0 &&
          cur_ms_lv - afl->last_crash_time < (u32)afl->limit_time_puppet) ||
         afl->last_path_time == 0))) {

    afl->key_puppet = 1;
    goto pacemaker_fuzzing;

  }

  /* Skip right away if -d is given, if we have done deterministic fuzzing on
     this entry ourselves (was_fuzzed), or if it has gone through deterministic
     testing in earlier, resumed runs (passed_det). */

  if (likely(afl->skip_deterministic || afl->queue_cur->was_fuzzed ||
             afl->queue_cur->passed_det)) {

    goto havoc_stage;

  }

  /* Skip deterministic fuzzing if exec path checksum puts this out of scope
     for this main instance. */

  if (unlikely(afl->main_node_max &&
               (afl->queue_cur->exec_cksum % afl->main_node_max) !=
                   afl->main_node_id - 1)) {

    goto havoc_stage;

  }

  doing_det = 1;

  /*********************************************
   * SIMPLE BITFLIP (+dictionary construction) *
   *********************************************/

#define FLIP_BIT(_ar, _b)                   \
  do {                                      \
                                            \
    u8 *_arf = (u8 *)(_ar);                 \
    u32 _bf = (_b);                         \
    _arf[(_bf) >> 3] ^= (128 >> ((_bf)&7)); \
                                            \
  } while (0)

  /* Single walking bit. */

  afl->stage_short = "flip1";
  afl->stage_max = len << 3;
  afl->stage_name = "bitflip 1/1";

  afl->stage_val_type = STAGE_VAL_NONE;

  orig_hit_cnt = afl->queued_paths + afl->unique_crashes;

  prev_cksum = afl->queue_cur->exec_cksum;

  for (afl->stage_cur = 0; afl->stage_cur < afl->stage_max; ++afl->stage_cur) {

    afl->stage_cur_byte = afl->stage_cur >> 3;

    FLIP_BIT(out_buf, afl->stage_cur);

#ifdef INTROSPECTION
    snprintf(afl->mutation, sizeof(afl->mutation), "%s MOPT_FLIP_BIT1-%u",
             afl->queue_cur->fname, afl->stage_cur);
#endif
    if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }

    FLIP_BIT(out_buf, afl->stage_cur);

    /* While flipping the least significant bit in every byte, pull of an extra
       trick to detect possible syntax tokens. In essence, the idea is that if
       you have a binary blob like this:

       xxxxxxxxIHDRxxxxxxxx

       ...and changing the leading and trailing bytes causes variable or no
       changes in program flow, but touching any character in the "IHDR" string
       always produces the same, distinctive path, it's highly likely that
       "IHDR" is an atomically-checked magic value of special significance to
       the fuzzed format.

       We do this here, rather than as a separate stage, because it's a nice
       way to keep the operation approximately "free" (i.e., no extra execs).

       Empirically, performing the check when flipping the least significant bit
       is advantageous, compared to doing it at the time of more disruptive
       changes, where the program flow may be affected in more violent ways.

       The caveat is that we won't generate dictionaries in the -d mode or -S
       mode - but that's probably a fair trade-off.

       This won't work particularly well with paths that exhibit variable
       behavior, but fails gracefully, so we'll carry out the checks anyway.

      */

    if (!afl->non_instrumented_mode && (afl->stage_cur & 7) == 7) {

      u64 cksum = hash64(afl->fsrv.trace_bits, afl->fsrv.map_size, HASH_CONST);

      if (afl->stage_cur == afl->stage_max - 1 && cksum == prev_cksum) {

        /* If at end of file and we are still collecting a string, grab the
           final character and force output. */

        if (a_len < MAX_AUTO_EXTRA) {

          a_collect[a_len] = out_buf[afl->stage_cur >> 3];

        }

        ++a_len;

        if (a_len >= MIN_AUTO_EXTRA && a_len <= MAX_AUTO_EXTRA) {

          maybe_add_auto(afl, a_collect, a_len);

        }

      } else if (cksum != prev_cksum) {

        /* Otherwise, if the checksum has changed, see if we have something
           worthwhile queued up, and collect that if the answer is yes. */

        if (a_len >= MIN_AUTO_EXTRA && a_len <= MAX_AUTO_EXTRA) {

          maybe_add_auto(afl, a_collect, a_len);

        }

        a_len = 0;
        prev_cksum = cksum;

      }

      /* Continue collecting string, but only if the bit flip actually made
         any difference - we don't want no-op tokens. */

      if (cksum != afl->queue_cur->exec_cksum) {

        if (a_len < MAX_AUTO_EXTRA) {

          a_collect[a_len] = out_buf[afl->stage_cur >> 3];

        }

        ++a_len;

      }

    }                                       /* if (afl->stage_cur & 7) == 7 */

  }                                                   /* for afl->stage_cur */

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_FLIP1] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_FLIP1] += afl->stage_max;

  /* Two walking bits. */

  afl->stage_name = "bitflip 2/1";
  afl->stage_short = "flip2";
  afl->stage_max = (len << 3) - 1;

  orig_hit_cnt = new_hit_cnt;

  for (afl->stage_cur = 0; afl->stage_cur < afl->stage_max; ++afl->stage_cur) {

    afl->stage_cur_byte = afl->stage_cur >> 3;

    FLIP_BIT(out_buf, afl->stage_cur);
    FLIP_BIT(out_buf, afl->stage_cur + 1);

#ifdef INTROSPECTION
    snprintf(afl->mutation, sizeof(afl->mutation), "%s MOPT_FLIP_BIT2-%u",
             afl->queue_cur->fname, afl->stage_cur);
#endif
    if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }

    FLIP_BIT(out_buf, afl->stage_cur);
    FLIP_BIT(out_buf, afl->stage_cur + 1);

  }                                                   /* for afl->stage_cur */

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_FLIP2] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_FLIP2] += afl->stage_max;

  /* Four walking bits. */

  afl->stage_name = "bitflip 4/1";
  afl->stage_short = "flip4";
  afl->stage_max = (len << 3) - 3;

  orig_hit_cnt = new_hit_cnt;

  for (afl->stage_cur = 0; afl->stage_cur < afl->stage_max; ++afl->stage_cur) {

    afl->stage_cur_byte = afl->stage_cur >> 3;

    FLIP_BIT(out_buf, afl->stage_cur);
    FLIP_BIT(out_buf, afl->stage_cur + 1);
    FLIP_BIT(out_buf, afl->stage_cur + 2);
    FLIP_BIT(out_buf, afl->stage_cur + 3);

#ifdef INTROSPECTION
    snprintf(afl->mutation, sizeof(afl->mutation), "%s MOPT_FLIP_BIT4-%u",
             afl->queue_cur->fname, afl->stage_cur);
#endif
    if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }

    FLIP_BIT(out_buf, afl->stage_cur);
    FLIP_BIT(out_buf, afl->stage_cur + 1);
    FLIP_BIT(out_buf, afl->stage_cur + 2);
    FLIP_BIT(out_buf, afl->stage_cur + 3);

  }                                                   /* for afl->stage_cur */

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_FLIP4] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_FLIP4] += afl->stage_max;

  /* Effector map setup. These macros calculate:

     EFF_APOS      - position of a particular file offset in the map.
     EFF_ALEN      - length of a map with a particular number of bytes.
     EFF_SPAN_ALEN - map span for a sequence of bytes.

   */

#define EFF_APOS(_p) ((_p) >> EFF_MAP_SCALE2)
#define EFF_REM(_x) ((_x) & ((1 << EFF_MAP_SCALE2) - 1))
#define EFF_ALEN(_l) (EFF_APOS(_l) + !!EFF_REM(_l))
#define EFF_SPAN_ALEN(_p, _l) (EFF_APOS((_p) + (_l)-1) - EFF_APOS(_p) + 1)

  /* Initialize effector map for the next step (see comments below). Always
         flag first and last byte as doing something. */

  eff_map = afl_realloc(AFL_BUF_PARAM(eff), EFF_ALEN(len));
  if (unlikely(!eff_map)) { PFATAL("alloc"); }
  eff_map[0] = 1;

  if (EFF_APOS(len - 1) != 0) {

    eff_map[EFF_APOS(len - 1)] = 1;
    ++eff_cnt;

  }

  /* Walking byte. */

  afl->stage_name = "bitflip 8/8";
  afl->stage_short = "flip8";
  afl->stage_max = len;

  orig_hit_cnt = new_hit_cnt;

  for (afl->stage_cur = 0; afl->stage_cur < afl->stage_max; ++afl->stage_cur) {

    afl->stage_cur_byte = afl->stage_cur;

    out_buf[afl->stage_cur] ^= 0xFF;

#ifdef INTROSPECTION
    snprintf(afl->mutation, sizeof(afl->mutation), "%s MOPT_FLIP_BIT8-%u",
             afl->queue_cur->fname, afl->stage_cur);
#endif
    if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }

    /* We also use this stage to pull off a simple trick: we identify
       bytes that seem to have no effect on the current execution path
       even when fully flipped - and we skip them during more expensive
       deterministic stages, such as arithmetics or known ints. */

    if (!eff_map[EFF_APOS(afl->stage_cur)]) {

      u64 cksum;

      /* If in non-instrumented mode or if the file is very short, just flag
         everything without wasting time on checksums. */

      if (!afl->non_instrumented_mode && len >= EFF_MIN_LEN) {

        cksum = hash64(afl->fsrv.trace_bits, afl->fsrv.map_size, HASH_CONST);

      } else {

        cksum = ~afl->queue_cur->exec_cksum;

      }

      if (cksum != afl->queue_cur->exec_cksum) {

        eff_map[EFF_APOS(afl->stage_cur)] = 1;
        ++eff_cnt;

      }

    }

    out_buf[afl->stage_cur] ^= 0xFF;

  }                                                   /* for afl->stage_cur */

  /* If the effector map is more than EFF_MAX_PERC dense, just flag the
     whole thing as worth fuzzing, since we wouldn't be saving much time
     anyway. */

  if (eff_cnt != (u32)EFF_ALEN(len) &&
      eff_cnt * 100 / EFF_ALEN(len) > EFF_MAX_PERC) {

    memset(eff_map, 1, EFF_ALEN(len));

    afl->blocks_eff_select += EFF_ALEN(len);

  } else {

    afl->blocks_eff_select += eff_cnt;

  }

  afl->blocks_eff_total += EFF_ALEN(len);

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_FLIP8] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_FLIP8] += afl->stage_max;

  /* Two walking bytes. */

  if (len < 2) { goto skip_bitflip; }

  afl->stage_name = "bitflip 16/8";
  afl->stage_short = "flip16";
  afl->stage_cur = 0;
  afl->stage_max = len - 1;

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < len - 1; ++i) {

    /* Let's consult the effector map... */

    if (!eff_map[EFF_APOS(i)] && !eff_map[EFF_APOS(i + 1)]) {

      --afl->stage_max;
      continue;

    }

    afl->stage_cur_byte = i;

    *(u16 *)(out_buf + i) ^= 0xFFFF;

#ifdef INTROSPECTION
    snprintf(afl->mutation, sizeof(afl->mutation), "%s MOPT_FLIP_BIT16-%u",
             afl->queue_cur->fname, afl->stage_cur);
#endif
    if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
    ++afl->stage_cur;

    *(u16 *)(out_buf + i) ^= 0xFFFF;

  }                                                   /* for i = 0; i < len */

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_FLIP16] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_FLIP16] += afl->stage_max;

  if (len < 4) { goto skip_bitflip; }

  /* Four walking bytes. */

  afl->stage_name = "bitflip 32/8";
  afl->stage_short = "flip32";
  afl->stage_cur = 0;
  afl->stage_max = len - 3;

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < len - 3; ++i) {

    /* Let's consult the effector map... */
    if (!eff_map[EFF_APOS(i)] && !eff_map[EFF_APOS(i + 1)] &&
        !eff_map[EFF_APOS(i + 2)] && !eff_map[EFF_APOS(i + 3)]) {

      --afl->stage_max;
      continue;

    }

    afl->stage_cur_byte = i;

    *(u32 *)(out_buf + i) ^= 0xFFFFFFFF;

#ifdef INTROSPECTION
    snprintf(afl->mutation, sizeof(afl->mutation), "%s MOPT_FLIP_BIT32-%u",
             afl->queue_cur->fname, afl->stage_cur);
#endif
    if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
    ++afl->stage_cur;

    *(u32 *)(out_buf + i) ^= 0xFFFFFFFF;

  }                                               /* for i = 0; i < len - 3 */

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_FLIP32] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_FLIP32] += afl->stage_max;

skip_bitflip:

  if (afl->no_arith) { goto skip_arith; }

  /**********************
   * ARITHMETIC INC/DEC *
   **********************/

  /* 8-bit arithmetics. */

  afl->stage_name = "arith 8/8";
  afl->stage_short = "arith8";
  afl->stage_cur = 0;
  afl->stage_max = 2 * len * ARITH_MAX;

  afl->stage_val_type = STAGE_VAL_LE;

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < (u32)len; ++i) {

    u8 orig = out_buf[i];

    /* Let's consult the effector map... */

    if (!eff_map[EFF_APOS(i)]) {

      afl->stage_max -= 2 * ARITH_MAX;
      continue;

    }

    afl->stage_cur_byte = i;

    for (j = 1; j <= ARITH_MAX; ++j) {

      u8 r = orig ^ (orig + j);

      /* Do arithmetic operations only if the result couldn't be a product
         of a bitflip. */

      if (!could_be_bitflip(r)) {

        afl->stage_cur_val = j;
        out_buf[i] = orig + j;

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s MOPT_ARITH8+-%u-%u",
                 afl->queue_cur->fname, i, j);
#endif
        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      r = orig ^ (orig - j);

      if (!could_be_bitflip(r)) {

        afl->stage_cur_val = -j;
        out_buf[i] = orig - j;

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s MOPT_ARITH8_-%u-%u",
                 afl->queue_cur->fname, i, j);
#endif
        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      out_buf[i] = orig;

    }

  }                                                   /* for i = 0; i < len */

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_ARITH8] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_ARITH8] += afl->stage_max;

  /* 16-bit arithmetics, both endians. */

  if (len < 2) { goto skip_arith; }

  afl->stage_name = "arith 16/8";
  afl->stage_short = "arith16";
  afl->stage_cur = 0;
  afl->stage_max = 4 * (len - 1) * ARITH_MAX;

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < len - 1; ++i) {

    u16 orig = *(u16 *)(out_buf + i);

    /* Let's consult the effector map... */

    if (!eff_map[EFF_APOS(i)] && !eff_map[EFF_APOS(i + 1)]) {

      afl->stage_max -= 4 * ARITH_MAX;
      continue;

    }

    afl->stage_cur_byte = i;

    for (j = 1; j <= ARITH_MAX; ++j) {

      u16 r1 = orig ^ (orig + j), r2 = orig ^ (orig - j),
          r3 = orig ^ SWAP16(SWAP16(orig) + j),
          r4 = orig ^ SWAP16(SWAP16(orig) - j);

      /* Try little endian addition and subtraction first. Do it only
         if the operation would affect more than one byte (hence the
         & 0xff overflow checks) and if it couldn't be a product of
         a bitflip. */

      afl->stage_val_type = STAGE_VAL_LE;

      if ((orig & 0xff) + j > 0xff && !could_be_bitflip(r1)) {

        afl->stage_cur_val = j;
        *(u16 *)(out_buf + i) = orig + j;

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s MOPT_ARITH16+-%u-%u",
                 afl->queue_cur->fname, i, j);
#endif
        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      if ((orig & 0xff) < j && !could_be_bitflip(r2)) {

        afl->stage_cur_val = -j;
        *(u16 *)(out_buf + i) = orig - j;

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s MOPT_ARITH16_-%u-%u",
                 afl->queue_cur->fname, i, j);
#endif
        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      /* Big endian comes next. Same deal. */

      afl->stage_val_type = STAGE_VAL_BE;

      if ((orig >> 8) + j > 0xff && !could_be_bitflip(r3)) {

        afl->stage_cur_val = j;
        *(u16 *)(out_buf + i) = SWAP16(SWAP16(orig) + j);

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation),
                 "%s MOPT_ARITH16+BE-%u-%u", afl->queue_cur->fname, i, j);
#endif
        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      if ((orig >> 8) < j && !could_be_bitflip(r4)) {

        afl->stage_cur_val = -j;
        *(u16 *)(out_buf + i) = SWAP16(SWAP16(orig) - j);

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation),
                 "%s MOPT_ARITH16_BE+%u+%u", afl->queue_cur->fname, i, j);
#endif
        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      *(u16 *)(out_buf + i) = orig;

    }

  }                                               /* for i = 0; i < len - 1 */

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_ARITH16] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_ARITH16] += afl->stage_max;

  /* 32-bit arithmetics, both endians. */

  if (len < 4) { goto skip_arith; }

  afl->stage_name = "arith 32/8";
  afl->stage_short = "arith32";
  afl->stage_cur = 0;
  afl->stage_max = 4 * (len - 3) * ARITH_MAX;

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < len - 3; ++i) {

    u32 orig = *(u32 *)(out_buf + i);

    /* Let's consult the effector map... */

    if (!eff_map[EFF_APOS(i)] && !eff_map[EFF_APOS(i + 1)] &&
        !eff_map[EFF_APOS(i + 2)] && !eff_map[EFF_APOS(i + 3)]) {

      afl->stage_max -= 4 * ARITH_MAX;
      continue;

    }

    afl->stage_cur_byte = i;

    for (j = 1; j <= ARITH_MAX; ++j) {

      u32 r1 = orig ^ (orig + j), r2 = orig ^ (orig - j),
          r3 = orig ^ SWAP32(SWAP32(orig) + j),
          r4 = orig ^ SWAP32(SWAP32(orig) - j);

      /* Little endian first. Same deal as with 16-bit: we only want to
         try if the operation would have effect on more than two bytes. */

      afl->stage_val_type = STAGE_VAL_LE;

      if ((orig & 0xffff) + j > 0xffff && !could_be_bitflip(r1)) {

        afl->stage_cur_val = j;
        *(u32 *)(out_buf + i) = orig + j;

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s MOPT_ARITH32+-%u-%u",
                 afl->queue_cur->fname, i, j);
#endif
        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      if ((orig & 0xffff) < j && !could_be_bitflip(r2)) {

        afl->stage_cur_val = -j;
        *(u32 *)(out_buf + i) = orig - j;

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s MOPT_ARITH32_-%u-%u",
                 afl->queue_cur->fname, i, j);
#endif
        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      /* Big endian next. */

      afl->stage_val_type = STAGE_VAL_BE;

      if ((SWAP32(orig) & 0xffff) + j > 0xffff && !could_be_bitflip(r3)) {

        afl->stage_cur_val = j;
        *(u32 *)(out_buf + i) = SWAP32(SWAP32(orig) + j);

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation),
                 "%s MOPT_ARITH32+BE-%u-%u", afl->queue_cur->fname, i, j);
#endif
        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      if ((SWAP32(orig) & 0xffff) < j && !could_be_bitflip(r4)) {

        afl->stage_cur_val = -j;
        *(u32 *)(out_buf + i) = SWAP32(SWAP32(orig) - j);

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation),
                 "%s MOPT_ARITH32_BE-%u-%u", afl->queue_cur->fname, i, j);
#endif
        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      *(u32 *)(out_buf + i) = orig;

    }

  }                                               /* for i = 0; i < len - 3 */

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_ARITH32] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_ARITH32] += afl->stage_max;

skip_arith:

  /**********************
   * INTERESTING VALUES *
   **********************/

  afl->stage_name = "interest 8/8";
  afl->stage_short = "int8";
  afl->stage_cur = 0;
  afl->stage_max = len * sizeof(interesting_8);

  afl->stage_val_type = STAGE_VAL_LE;

  orig_hit_cnt = new_hit_cnt;

  /* Setting 8-bit integers. */

  for (i = 0; i < (u32)len; ++i) {

    u8 orig = out_buf[i];

    /* Let's consult the effector map... */

    if (!eff_map[EFF_APOS(i)]) {

      afl->stage_max -= sizeof(interesting_8);
      continue;

    }

    afl->stage_cur_byte = i;

    for (j = 0; j < sizeof(interesting_8); ++j) {

      /* Skip if the value could be a product of bitflips or arithmetics. */

      if (could_be_bitflip(orig ^ (u8)interesting_8[j]) ||
          could_be_arith(orig, (u8)interesting_8[j], 1)) {

        --afl->stage_max;
        continue;

      }

      afl->stage_cur_val = interesting_8[j];
      out_buf[i] = interesting_8[j];

#ifdef INTROSPECTION
      snprintf(afl->mutation, sizeof(afl->mutation),
               "%s MOPT_INTERESTING8-%u-%u", afl->queue_cur->fname, i, j);
#endif
      if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }

      out_buf[i] = orig;
      ++afl->stage_cur;

    }

  }                                                   /* for i = 0; i < len */

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_INTEREST8] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_INTEREST8] += afl->stage_max;

  /* Setting 16-bit integers, both endians. */

  if (afl->no_arith || len < 2) { goto skip_interest; }

  afl->stage_name = "interest 16/8";
  afl->stage_short = "int16";
  afl->stage_cur = 0;
  afl->stage_max = 2 * (len - 1) * (sizeof(interesting_16) >> 1);

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < len - 1; ++i) {

    u16 orig = *(u16 *)(out_buf + i);

    /* Let's consult the effector map... */

    if (!eff_map[EFF_APOS(i)] && !eff_map[EFF_APOS(i + 1)]) {

      afl->stage_max -= sizeof(interesting_16);
      continue;

    }

    afl->stage_cur_byte = i;

    for (j = 0; j < sizeof(interesting_16) / 2; ++j) {

      afl->stage_cur_val = interesting_16[j];

      /* Skip if this could be a product of a bitflip, arithmetics,
         or single-byte interesting value insertion. */

      if (!could_be_bitflip(orig ^ (u16)interesting_16[j]) &&
          !could_be_arith(orig, (u16)interesting_16[j], 2) &&
          !could_be_interest(orig, (u16)interesting_16[j], 2, 0)) {

        afl->stage_val_type = STAGE_VAL_LE;

        *(u16 *)(out_buf + i) = interesting_16[j];

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation),
                 "%s MOPT_INTERESTING16-%u-%u", afl->queue_cur->fname, i, j);
#endif
        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      if ((u16)interesting_16[j] != SWAP16(interesting_16[j]) &&
          !could_be_bitflip(orig ^ SWAP16(interesting_16[j])) &&
          !could_be_arith(orig, SWAP16(interesting_16[j]), 2) &&
          !could_be_interest(orig, SWAP16(interesting_16[j]), 2, 1)) {

        afl->stage_val_type = STAGE_VAL_BE;

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation),
                 "%s MOPT_INTERESTING16BE-%u-%u", afl->queue_cur->fname, i, j);
#endif
        *(u16 *)(out_buf + i) = SWAP16(interesting_16[j]);
        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

    }

    *(u16 *)(out_buf + i) = orig;

  }                                               /* for i = 0; i < len - 1 */

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_INTEREST16] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_INTEREST16] += afl->stage_max;

  if (len < 4) { goto skip_interest; }

  /* Setting 32-bit integers, both endians. */

  afl->stage_name = "interest 32/8";
  afl->stage_short = "int32";
  afl->stage_cur = 0;
  afl->stage_max = 2 * (len - 3) * (sizeof(interesting_32) >> 2);

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < len - 3; ++i) {

    u32 orig = *(u32 *)(out_buf + i);

    /* Let's consult the effector map... */

    if (!eff_map[EFF_APOS(i)] && !eff_map[EFF_APOS(i + 1)] &&
        !eff_map[EFF_APOS(i + 2)] && !eff_map[EFF_APOS(i + 3)]) {

      afl->stage_max -= sizeof(interesting_32) >> 1;
      continue;

    }

    afl->stage_cur_byte = i;

    for (j = 0; j < sizeof(interesting_32) / 4; ++j) {

      afl->stage_cur_val = interesting_32[j];

      /* Skip if this could be a product of a bitflip, arithmetics,
         or word interesting value insertion. */

      if (!could_be_bitflip(orig ^ (u32)interesting_32[j]) &&
          !could_be_arith(orig, interesting_32[j], 4) &&
          !could_be_interest(orig, interesting_32[j], 4, 0)) {

        afl->stage_val_type = STAGE_VAL_LE;

        *(u32 *)(out_buf + i) = interesting_32[j];

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation),
                 "%s MOPT_INTERESTING32-%u-%u", afl->queue_cur->fname, i, j);
#endif
        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

      if ((u32)interesting_32[j] != SWAP32(interesting_32[j]) &&
          !could_be_bitflip(orig ^ SWAP32(interesting_32[j])) &&
          !could_be_arith(orig, SWAP32(interesting_32[j]), 4) &&
          !could_be_interest(orig, SWAP32(interesting_32[j]), 4, 1)) {

        afl->stage_val_type = STAGE_VAL_BE;

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation),
                 "%s MOPT_INTERESTING32BE-%u-%u", afl->queue_cur->fname, i, j);
#endif
        *(u32 *)(out_buf + i) = SWAP32(interesting_32[j]);
        if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
        ++afl->stage_cur;

      } else {

        --afl->stage_max;

      }

    }

    *(u32 *)(out_buf + i) = orig;

  }                                               /* for i = 0; i < len - 3 */

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_INTEREST32] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_INTEREST32] += afl->stage_max;

skip_interest:

  /********************
   * DICTIONARY STUFF *
   ********************/

  if (!afl->extras_cnt) { goto skip_user_extras; }

  /* Overwrite with user-supplied extras. */

  afl->stage_name = "user extras (over)";
  afl->stage_short = "ext_UO";
  afl->stage_cur = 0;
  afl->stage_max = afl->extras_cnt * len;

  afl->stage_val_type = STAGE_VAL_NONE;

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < (u32)len; ++i) {

    u32 last_len = 0;

    afl->stage_cur_byte = i;

    /* Extras are sorted by size, from smallest to largest. This means
       that we don't have to worry about restoring the buffer in
       between writes at a particular offset determined by the outer
       loop. */

    for (j = 0; j < afl->extras_cnt; ++j) {

      /* Skip extras probabilistically if afl->extras_cnt > AFL_MAX_DET_EXTRAS.
         Also skip them if there's no room to insert the payload, if the token
         is redundant, or if its entire span has no bytes set in the effector
         map. */

      if ((afl->extras_cnt > afl->max_det_extras &&
           rand_below(afl, afl->extras_cnt) >= afl->max_det_extras) ||
          afl->extras[j].len > len - i ||
          !memcmp(afl->extras[j].data, out_buf + i, afl->extras[j].len) ||
          !memchr(eff_map + EFF_APOS(i), 1,
                  EFF_SPAN_ALEN(i, afl->extras[j].len))) {

        --afl->stage_max;
        continue;

      }

      last_len = afl->extras[j].len;
      memcpy(out_buf + i, afl->extras[j].data, last_len);

#ifdef INTROSPECTION
      snprintf(afl->mutation, sizeof(afl->mutation),
               "%s MOPT_EXTRAS_overwrite-%u-%u", afl->queue_cur->fname, i, j);
#endif

      if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }

      ++afl->stage_cur;

    }

    /* Restore all the clobbered memory. */
    memcpy(out_buf + i, in_buf + i, last_len);

  }                                                   /* for i = 0; i < len */

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_EXTRAS_UO] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_EXTRAS_UO] += afl->stage_max;

  /* Insertion of user-supplied extras. */

  afl->stage_name = "user extras (insert)";
  afl->stage_short = "ext_UI";
  afl->stage_cur = 0;
  afl->stage_max = afl->extras_cnt * (len + 1);

  orig_hit_cnt = new_hit_cnt;

  ex_tmp = afl_realloc(AFL_BUF_PARAM(ex), len + MAX_DICT_FILE);
  if (unlikely(!ex_tmp)) { PFATAL("alloc"); }

  for (i = 0; i <= (u32)len; ++i) {

    afl->stage_cur_byte = i;

    for (j = 0; j < afl->extras_cnt; ++j) {

      if (len + afl->extras[j].len > MAX_FILE) {

        --afl->stage_max;
        continue;

      }

      /* Insert token */
      memcpy(ex_tmp + i, afl->extras[j].data, afl->extras[j].len);

      /* Copy tail */
      memcpy(ex_tmp + i + afl->extras[j].len, out_buf + i, len - i);

#ifdef INTROSPECTION
      snprintf(afl->mutation, sizeof(afl->mutation),
               "%s MOPT_EXTRAS_insert-%u-%u", afl->queue_cur->fname, i, j);
#endif

      if (common_fuzz_stuff(afl, ex_tmp, len + afl->extras[j].len)) {

        goto abandon_entry;

      }

      ++afl->stage_cur;

    }

    /* Copy head */
    ex_tmp[i] = out_buf[i];

  }                                                  /* for i = 0; i <= len */

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_EXTRAS_UI] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_EXTRAS_UI] += afl->stage_max;

skip_user_extras:

  if (!afl->a_extras_cnt) { goto skip_extras; }

  afl->stage_name = "auto extras (over)";
  afl->stage_short = "ext_AO";
  afl->stage_cur = 0;
  afl->stage_max = MIN(afl->a_extras_cnt, (u32)USE_AUTO_EXTRAS) * len;

  afl->stage_val_type = STAGE_VAL_NONE;

  orig_hit_cnt = new_hit_cnt;

  for (i = 0; i < (u32)len; ++i) {

    u32 last_len = 0;

    afl->stage_cur_byte = i;

    u32 min_extra_len = MIN(afl->a_extras_cnt, (u32)USE_AUTO_EXTRAS);
    for (j = 0; j < min_extra_len; ++j) {

      /* See the comment in the earlier code; extras are sorted by size. */

      if ((afl->a_extras[j].len) > (len - i) ||
          !memcmp(afl->a_extras[j].data, out_buf + i, afl->a_extras[j].len) ||
          !memchr(eff_map + EFF_APOS(i), 1,
                  EFF_SPAN_ALEN(i, afl->a_extras[j].len))) {

        --afl->stage_max;
        continue;

      }

      last_len = afl->a_extras[j].len;
      memcpy(out_buf + i, afl->a_extras[j].data, last_len);

#ifdef INTROSPECTION
      snprintf(afl->mutation, sizeof(afl->mutation),
               "%s MOPT_AUTO_EXTRAS_overwrite-%u-%u", afl->queue_cur->fname, i,
               j);
#endif

      if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }

      ++afl->stage_cur;

    }

    /* Restore all the clobbered memory. */
    memcpy(out_buf + i, in_buf + i, last_len);

  }                                                   /* for i = 0; i < len */

  new_hit_cnt = afl->queued_paths + afl->unique_crashes;

  afl->stage_finds[STAGE_EXTRAS_AO] += new_hit_cnt - orig_hit_cnt;
  afl->stage_cycles[STAGE_EXTRAS_AO] += afl->stage_max;

skip_extras:

  /* If we made this to here without jumping to havoc_stage or abandon_entry,
     we're properly done with deterministic steps and can mark it as such
     in the .state/ directory. */

  if (!afl->queue_cur->passed_det) { mark_as_det_done(afl, afl->queue_cur); }

  /****************
   * RANDOM HAVOC *
   ****************/

havoc_stage:
pacemaker_fuzzing:

  afl->stage_cur_byte = -1;

  /* The havoc stage mutation code is also invoked when splicing files; if the
     splice_cycle variable is set, generate different descriptions and such. */

  if (!splice_cycle) {

    afl->stage_name = MOpt_globals.havoc_stagename;
    afl->stage_short = MOpt_globals.havoc_stagenameshort;
    afl->stage_max = (doing_det ? HAVOC_CYCLES_INIT : HAVOC_CYCLES) *
                     perf_score / afl->havoc_div / 100;

  } else {

    perf_score = orig_perf;

    snprintf(afl->stage_name_buf, STAGE_BUF_SIZE,
             MOpt_globals.splice_stageformat, splice_cycle);
    afl->stage_name = afl->stage_name_buf;
    afl->stage_short = MOpt_globals.splice_stagenameshort;
    afl->stage_max = SPLICE_HAVOC * perf_score / afl->havoc_div / 100;

  }

  s32 temp_len_puppet;

  // for (; afl->swarm_now < swarm_num; ++afl->swarm_now)
  {

    if (afl->key_puppet == 1) {

      if (unlikely(afl->orig_hit_cnt_puppet == 0)) {

        afl->orig_hit_cnt_puppet = afl->queued_paths + afl->unique_crashes;
        afl->last_limit_time_start = get_cur_time();
        afl->SPLICE_CYCLES_puppet =
            (rand_below(
                 afl, SPLICE_CYCLES_puppet_up - SPLICE_CYCLES_puppet_low + 1) +
             SPLICE_CYCLES_puppet_low);

      }

    }                                            /* if afl->key_puppet == 1 */

    {

#ifndef IGNORE_FINDS
    havoc_stage_puppet:
#endif

      afl->stage_cur_byte = -1;

      /* The havoc stage mutation code is also invoked when splicing files; if
         the splice_cycle variable is set, generate different descriptions and
         such. */

      if (!splice_cycle) {

        afl->stage_name = MOpt_globals.havoc_stagename;
        afl->stage_short = MOpt_globals.havoc_stagenameshort;
        afl->stage_max = (doing_det ? HAVOC_CYCLES_INIT : HAVOC_CYCLES) *
                         perf_score / afl->havoc_div / 100;

      } else {

        perf_score = orig_perf;
        snprintf(afl->stage_name_buf, STAGE_BUF_SIZE,
                 MOpt_globals.splice_stageformat, splice_cycle);
        afl->stage_name = afl->stage_name_buf;
        afl->stage_short = MOpt_globals.splice_stagenameshort;
        afl->stage_max = SPLICE_HAVOC * perf_score / afl->havoc_div / 100;

      }

      if (afl->stage_max < HAVOC_MIN) { afl->stage_max = HAVOC_MIN; }

      temp_len = len;

      orig_hit_cnt = afl->queued_paths + afl->unique_crashes;

      havoc_queued = afl->queued_paths;

      u32 r_max;

      r_max = 15 + ((afl->extras_cnt + afl->a_extras_cnt) ? 2 : 0);

      if (unlikely(afl->expand_havoc && afl->ready_for_splicing_count > 1)) {

        /* add expensive havoc cases here, they are activated after a full
           cycle without finds happened */

        ++r_max;

      }

      for (afl->stage_cur = 0; afl->stage_cur < afl->stage_max;
           ++afl->stage_cur) {

        u32 use_stacking = 1 << (1 + rand_below(afl, afl->havoc_stack_pow2));

        afl->stage_cur_val = use_stacking;

        for (i = 0; i < operator_num; ++i) {

          MOpt_globals.cycles_v3[i] = MOpt_globals.cycles_v2[i];

        }

#ifdef INTROSPECTION
        snprintf(afl->mutation, sizeof(afl->mutation), "%s MOPT_HAVOC-%u",
                 afl->queue_cur->fname, use_stacking);
#endif

        for (i = 0; i < use_stacking; ++i) {

          switch (select_algorithm(afl, r_max)) {

            case 0:
              /* Flip a single bit somewhere. Spooky! */
              FLIP_BIT(out_buf, rand_below(afl, temp_len << 3));
              MOpt_globals.cycles_v2[STAGE_FLIP1]++;
#ifdef INTROSPECTION
              snprintf(afl->m_tmp, sizeof(afl->m_tmp), " FLIP_BIT1");
              strcat(afl->mutation, afl->m_tmp);
#endif
              break;

            case 1:
              if (temp_len < 2) { break; }
              temp_len_puppet = rand_below(afl, (temp_len << 3) - 1);
              FLIP_BIT(out_buf, temp_len_puppet);
              FLIP_BIT(out_buf, temp_len_puppet + 1);
              MOpt_globals.cycles_v2[STAGE_FLIP2]++;
#ifdef INTROSPECTION
              snprintf(afl->m_tmp, sizeof(afl->m_tmp), " FLIP_BIT2");
              strcat(afl->mutation, afl->m_tmp);
#endif
              break;

            case 2:
              if (temp_len < 2) { break; }
              temp_len_puppet = rand_below(afl, (temp_len << 3) - 3);
              FLIP_BIT(out_buf, temp_len_puppet);
              FLIP_BIT(out_buf, temp_len_puppet + 1);
              FLIP_BIT(out_buf, temp_len_puppet + 2);
              FLIP_BIT(out_buf, temp_len_puppet + 3);
              MOpt_globals.cycles_v2[STAGE_FLIP4]++;
#ifdef INTROSPECTION
              snprintf(afl->m_tmp, sizeof(afl->m_tmp), " FLIP_BIT4");
              strcat(afl->mutation, afl->m_tmp);
#endif
              break;

            case 3:
              if (temp_len < 4) { break; }
              out_buf[rand_below(afl, temp_len)] ^= 0xFF;
              MOpt_globals.cycles_v2[STAGE_FLIP8]++;
#ifdef INTROSPECTION
              snprintf(afl->m_tmp, sizeof(afl->m_tmp), " FLIP_BIT8");
              strcat(afl->mutation, afl->m_tmp);
#endif
              break;

            case 4:
              if (temp_len < 8) { break; }
              *(u16 *)(out_buf + rand_below(afl, temp_len - 1)) ^= 0xFFFF;
              MOpt_globals.cycles_v2[STAGE_FLIP16]++;
#ifdef INTROSPECTION
              snprintf(afl->m_tmp, sizeof(afl->m_tmp), " FLIP_BIT16");
              strcat(afl->mutation, afl->m_tmp);
#endif
              break;

            case 5:
              if (temp_len < 8) { break; }
              *(u32 *)(out_buf + rand_below(afl, temp_len - 3)) ^= 0xFFFFFFFF;
              MOpt_globals.cycles_v2[STAGE_FLIP32]++;
#ifdef INTROSPECTION
              snprintf(afl->m_tmp, sizeof(afl->m_tmp), " FLIP_BIT32");
              strcat(afl->mutation, afl->m_tmp);
#endif
              break;

            case 6:
              out_buf[rand_below(afl, temp_len)] -=
                  1 + rand_below(afl, ARITH_MAX);
              out_buf[rand_below(afl, temp_len)] +=
                  1 + rand_below(afl, ARITH_MAX);
              MOpt_globals.cycles_v2[STAGE_ARITH8]++;
#ifdef INTROSPECTION
              snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH8");
              strcat(afl->mutation, afl->m_tmp);
#endif
              break;

            case 7:
              /* Randomly subtract from word, random endian. */
              if (temp_len < 8) { break; }
              if (rand_below(afl, 2)) {

                u32 pos = rand_below(afl, temp_len - 1);
                *(u16 *)(out_buf + pos) -= 1 + rand_below(afl, ARITH_MAX);
#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH16-%u", pos);
                strcat(afl->mutation, afl->m_tmp);
#endif

              } else {

                u32 pos = rand_below(afl, temp_len - 1);
                u16 num = 1 + rand_below(afl, ARITH_MAX);
#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH16BE-%u-%u",
                         pos, num);
                strcat(afl->mutation, afl->m_tmp);
#endif
                *(u16 *)(out_buf + pos) =
                    SWAP16(SWAP16(*(u16 *)(out_buf + pos)) - num);

              }

              /* Randomly add to word, random endian. */
              if (rand_below(afl, 2)) {

                u32 pos = rand_below(afl, temp_len - 1);
#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH16+-%u", pos);
                strcat(afl->mutation, afl->m_tmp);
#endif
                *(u16 *)(out_buf + pos) += 1 + rand_below(afl, ARITH_MAX);

              } else {

                u32 pos = rand_below(afl, temp_len - 1);
                u16 num = 1 + rand_below(afl, ARITH_MAX);
#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH16BE+-%u-%u",
                         pos, num);
                strcat(afl->mutation, afl->m_tmp);
#endif
                *(u16 *)(out_buf + pos) =
                    SWAP16(SWAP16(*(u16 *)(out_buf + pos)) + num);

              }

              MOpt_globals.cycles_v2[STAGE_ARITH16]++;
              break;

            case 8:
              /* Randomly subtract from dword, random endian. */
              if (temp_len < 8) { break; }
              if (rand_below(afl, 2)) {

                u32 pos = rand_below(afl, temp_len - 3);
#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH32_-%u", pos);
                strcat(afl->mutation, afl->m_tmp);
#endif
                *(u32 *)(out_buf + pos) -= 1 + rand_below(afl, ARITH_MAX);

              } else {

                u32 pos = rand_below(afl, temp_len - 3);
                u32 num = 1 + rand_below(afl, ARITH_MAX);
#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH32BE_-%u-%u",
                         pos, num);
                strcat(afl->mutation, afl->m_tmp);
#endif
                *(u32 *)(out_buf + pos) =
                    SWAP32(SWAP32(*(u32 *)(out_buf + pos)) - num);

              }

              /* Randomly add to dword, random endian. */
              // if (temp_len < 4) break;
              if (rand_below(afl, 2)) {

                u32 pos = rand_below(afl, temp_len - 3);
#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH32+-%u", pos);
                strcat(afl->mutation, afl->m_tmp);
#endif
                *(u32 *)(out_buf + pos) += 1 + rand_below(afl, ARITH_MAX);

              } else {

                u32 pos = rand_below(afl, temp_len - 3);
                u32 num = 1 + rand_below(afl, ARITH_MAX);
#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp), " ARITH32BE+-%u-%u",
                         pos, num);
                strcat(afl->mutation, afl->m_tmp);
#endif
                *(u32 *)(out_buf + pos) =
                    SWAP32(SWAP32(*(u32 *)(out_buf + pos)) + num);

              }

              MOpt_globals.cycles_v2[STAGE_ARITH32]++;
              break;

            case 9:
              /* Set byte to interesting value. */
              if (temp_len < 4) { break; }
              out_buf[rand_below(afl, temp_len)] =
                  interesting_8[rand_below(afl, sizeof(interesting_8))];
              MOpt_globals.cycles_v2[STAGE_INTEREST8]++;
#ifdef INTROSPECTION
              snprintf(afl->m_tmp, sizeof(afl->m_tmp), " INTERESTING8");
              strcat(afl->mutation, afl->m_tmp);
#endif
              break;

            case 10:
              /* Set word to interesting value, randomly choosing endian. */
              if (temp_len < 8) { break; }
              if (rand_below(afl, 2)) {

#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp), " INTERESTING16");
                strcat(afl->mutation, afl->m_tmp);
#endif
                *(u16 *)(out_buf + rand_below(afl, temp_len - 1)) =
                    interesting_16[rand_below(afl,
                                              sizeof(interesting_16) >> 1)];

              } else {

#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp), " INTERESTING16BE");
                strcat(afl->mutation, afl->m_tmp);
#endif
                *(u16 *)(out_buf + rand_below(afl, temp_len - 1)) =
                    SWAP16(interesting_16[rand_below(
                        afl, sizeof(interesting_16) >> 1)]);

              }

              MOpt_globals.cycles_v2[STAGE_INTEREST16]++;
              break;

            case 11:
              /* Set dword to interesting value, randomly choosing endian. */

              if (temp_len < 8) { break; }

              if (rand_below(afl, 2)) {

#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp), " INTERESTING32");
                strcat(afl->mutation, afl->m_tmp);
#endif
                *(u32 *)(out_buf + rand_below(afl, temp_len - 3)) =
                    interesting_32[rand_below(afl,
                                              sizeof(interesting_32) >> 2)];

              } else {

#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp), " INTERESTING32BE");
                strcat(afl->mutation, afl->m_tmp);
#endif
                *(u32 *)(out_buf + rand_below(afl, temp_len - 3)) =
                    SWAP32(interesting_32[rand_below(
                        afl, sizeof(interesting_32) >> 2)]);

              }

              MOpt_globals.cycles_v2[STAGE_INTEREST32]++;
              break;

            case 12:

              /* Just set a random byte to a random value. Because,
                 why not. We use XOR with 1-255 to eliminate the
                 possibility of a no-op. */

              out_buf[rand_below(afl, temp_len)] ^= 1 + rand_below(afl, 255);
              MOpt_globals.cycles_v2[STAGE_RANDOMBYTE]++;
#ifdef INTROSPECTION
              snprintf(afl->m_tmp, sizeof(afl->m_tmp), " RAND8");
              strcat(afl->mutation, afl->m_tmp);
#endif
              break;

            case 13: {

              /* Delete bytes. We're making this a bit more likely
                 than insertion (the next option) in hopes of keeping
                 files reasonably small. */

              u32 del_from, del_len;

              if (temp_len < 2) { break; }

              /* Don't delete too much. */

              del_len = choose_block_len(afl, temp_len - 1);

              del_from = rand_below(afl, temp_len - del_len + 1);

#ifdef INTROSPECTION
              snprintf(afl->m_tmp, sizeof(afl->m_tmp), " DEL-%u%u", del_from,
                       del_len);
              strcat(afl->mutation, afl->m_tmp);
#endif
              memmove(out_buf + del_from, out_buf + del_from + del_len,
                      temp_len - del_from - del_len);

              temp_len -= del_len;
              MOpt_globals.cycles_v2[STAGE_DELETEBYTE]++;
              break;

            }

            case 14:

              if (temp_len + HAVOC_BLK_XL < MAX_FILE) {

                /* Clone bytes (75%) or insert a block of constant bytes (25%).
                 */

                u8  actually_clone = rand_below(afl, 4);
                u32 clone_from, clone_to, clone_len;
                u8 *new_buf;

                if (likely(actually_clone)) {

                  clone_len = choose_block_len(afl, temp_len);
                  clone_from = rand_below(afl, temp_len - clone_len + 1);

                } else {

                  clone_len = choose_block_len(afl, HAVOC_BLK_XL);
                  clone_from = 0;

                }

                clone_to = rand_below(afl, temp_len);

#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp), " CLONE_%s-%u-%u-%u",
                         actually_clone ? "clone" : "insert", clone_from,
                         clone_to, clone_len);
                strcat(afl->mutation, afl->m_tmp);
#endif
                new_buf = afl_realloc(AFL_BUF_PARAM(out_scratch),
                                      temp_len + clone_len);
                if (unlikely(!new_buf)) { PFATAL("alloc"); }

                /* Head */

                memcpy(new_buf, out_buf, clone_to);

                /* Inserted part */

                if (actually_clone) {

                  memcpy(new_buf + clone_to, out_buf + clone_from, clone_len);

                } else {

                  memset(new_buf + clone_to,
                         rand_below(afl, 2)
                             ? rand_below(afl, 256)
                             : out_buf[rand_below(afl, temp_len)],
                         clone_len);

                }

                /* Tail */
                memcpy(new_buf + clone_to + clone_len, out_buf + clone_to,
                       temp_len - clone_to);

                out_buf = new_buf;
                afl_swap_bufs(AFL_BUF_PARAM(out), AFL_BUF_PARAM(out_scratch));
                temp_len += clone_len;
                MOpt_globals.cycles_v2[STAGE_Clone75]++;

              }

              break;

            case 15: {

              /* Overwrite bytes with a randomly selected chunk (75%) or fixed
                 bytes (25%). */

              u32 copy_from, copy_to, copy_len;

              if (temp_len < 2) { break; }

              copy_len = choose_block_len(afl, temp_len - 1);

              copy_from = rand_below(afl, temp_len - copy_len + 1);
              copy_to = rand_below(afl, temp_len - copy_len + 1);

              if (likely(rand_below(afl, 4))) {

                if (likely(copy_from != copy_to)) {

#ifdef INTROSPECTION
                  snprintf(afl->m_tmp, sizeof(afl->m_tmp),
                           " OVERWRITE_COPY-%u-%u-%u", copy_from, copy_to,
                           copy_len);
                  strcat(afl->mutation, afl->m_tmp);
#endif
                  memmove(out_buf + copy_to, out_buf + copy_from, copy_len);

                }

              } else {

#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp),
                         " OVERWRITE_FIXED-%u-%u-%u", copy_from, copy_to,
                         copy_len);
                strcat(afl->mutation, afl->m_tmp);
#endif
                memset(out_buf + copy_to,
                       rand_below(afl, 2) ? rand_below(afl, 256)
                                          : out_buf[rand_below(afl, temp_len)],
                       copy_len);

              }

              MOpt_globals.cycles_v2[STAGE_OverWrite75]++;
              break;

            }                                                    /* case 15 */

              /* Values 16 and 17 can be selected only if there are any extras
                 present in the dictionaries. */

            case 16: {

              /* Overwrite bytes with an extra. */

              if (!afl->extras_cnt ||
                  (afl->a_extras_cnt && rand_below(afl, 2))) {

                /* No user-specified extras or odds in our favor. Let's use an
                  auto-detected one. */

                u32 use_extra = rand_below(afl, afl->a_extras_cnt);
                u32 extra_len = afl->a_extras[use_extra].len;

                if (extra_len > (u32)temp_len) break;

                u32 insert_at = rand_below(afl, temp_len - extra_len + 1);
#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp),
                         " AUTO_EXTRA_OVERWRITE-%u-%u", insert_at, extra_len);
                strcat(afl->mutation, afl->m_tmp);
#endif
                memcpy(out_buf + insert_at, afl->a_extras[use_extra].data,
                       extra_len);

              } else {

                /* No auto extras or odds in our favor. Use the dictionary. */

                u32 use_extra = rand_below(afl, afl->extras_cnt);
                u32 extra_len = afl->extras[use_extra].len;

                if (extra_len > (u32)temp_len) break;

                u32 insert_at = rand_below(afl, temp_len - extra_len + 1);
#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp),
                         " EXTRA_OVERWRITE-%u-%u", insert_at, extra_len);
                strcat(afl->mutation, afl->m_tmp);
#endif
                memcpy(out_buf + insert_at, afl->extras[use_extra].data,
                       extra_len);

              }

              MOpt_globals.cycles_v2[STAGE_OverWriteExtra]++;

              break;

            }

              /* Insert an extra. */

            case 17: {

              u32 use_extra, extra_len,
                  insert_at = rand_below(afl, temp_len + 1);
              u8 *ptr;

              /* Insert an extra. Do the same dice-rolling stuff as for the
                previous case. */

              if (!afl->extras_cnt ||
                  (afl->a_extras_cnt && rand_below(afl, 2))) {

                use_extra = rand_below(afl, afl->a_extras_cnt);
                extra_len = afl->a_extras[use_extra].len;
                ptr = afl->a_extras[use_extra].data;
#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp),
                         " AUTO_EXTRA_INSERT-%u-%u", insert_at, extra_len);
                strcat(afl->mutation, afl->m_tmp);
#endif

              } else {

                use_extra = rand_below(afl, afl->extras_cnt);
                extra_len = afl->extras[use_extra].len;
                ptr = afl->extras[use_extra].data;
#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp), " EXTRA_INSERT-%u-%u",
                         insert_at, extra_len);
                strcat(afl->mutation, afl->m_tmp);
#endif

              }

              if (temp_len + extra_len >= MAX_FILE) break;

              out_buf = afl_realloc(AFL_BUF_PARAM(out), temp_len + extra_len);
              if (unlikely(!out_buf)) { PFATAL("alloc"); }

              /* Tail */
              memmove(out_buf + insert_at + extra_len, out_buf + insert_at,
                      temp_len - insert_at);

              /* Inserted part */
              memcpy(out_buf + insert_at, ptr, extra_len);

              temp_len += extra_len;
              MOpt_globals.cycles_v2[STAGE_InsertExtra]++;
              break;

            }

            default: {

              if (unlikely(afl->ready_for_splicing_count < 2)) break;

              u32 tid;
              do {

                tid = rand_below(afl, afl->queued_paths);

              } while (tid == afl->current_entry ||

                       afl->queue_buf[tid]->len < 4);

              /* Get the testcase for splicing. */
              struct queue_entry *target = afl->queue_buf[tid];
              u32                 new_len = target->len;
              u8 *                new_buf = queue_testcase_get(afl, target);

              if ((temp_len >= 2 && rand_below(afl, 2)) ||
                  temp_len + HAVOC_BLK_XL >= MAX_FILE) {

                /* overwrite mode */

                u32 copy_from, copy_to, copy_len;

                copy_len = choose_block_len(afl, new_len - 1);
                if (copy_len > temp_len) copy_len = temp_len;

                copy_from = rand_below(afl, new_len - copy_len + 1);
                copy_to = rand_below(afl, temp_len - copy_len + 1);

#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp),
                         " SPLICE_OVERWRITE-%u-%u-%u-%s", copy_from, copy_to,
                         copy_len, target->fname);
                strcat(afl->mutation, afl->m_tmp);
#endif
                memmove(out_buf + copy_to, new_buf + copy_from, copy_len);

              } else {

                /* insert mode */

                u32 clone_from, clone_to, clone_len;

                clone_len = choose_block_len(afl, new_len);
                clone_from = rand_below(afl, new_len - clone_len + 1);
                clone_to = rand_below(afl, temp_len + 1);

                u8 *temp_buf = afl_realloc(AFL_BUF_PARAM(out_scratch),
                                           temp_len + clone_len + 1);
                if (unlikely(!temp_buf)) { PFATAL("alloc"); }

#ifdef INTROSPECTION
                snprintf(afl->m_tmp, sizeof(afl->m_tmp),
                         " SPLICE_INSERT-%u-%u-%u-%s", clone_from, clone_to,
                         clone_len, target->fname);
                strcat(afl->mutation, afl->m_tmp);
#endif
                /* Head */

                memcpy(temp_buf, out_buf, clone_to);

                /* Inserted part */

                memcpy(temp_buf + clone_to, new_buf + clone_from, clone_len);

                /* Tail */
                memcpy(temp_buf + clone_to + clone_len, out_buf + clone_to,
                       temp_len - clone_to);

                out_buf = temp_buf;
                afl_swap_bufs(AFL_BUF_PARAM(out), AFL_BUF_PARAM(out_scratch));
                temp_len += clone_len;

              }

              MOpt_globals.cycles_v2[STAGE_Splice]++;
              break;

            }  // end of default:

          }                                    /* switch select_algorithm() */

        }                                      /* for i=0; i < use_stacking */

        ++*MOpt_globals.pTime;

        u64 temp_total_found = afl->queued_paths + afl->unique_crashes;

        if (common_fuzz_stuff(afl, out_buf, temp_len)) {

          goto abandon_entry_puppet;

        }

        /* out_buf might have been mangled a bit, so let's restore it to its
           original size and shape. */

        out_buf = afl_realloc(AFL_BUF_PARAM(out), len);
        if (unlikely(!out_buf)) { PFATAL("alloc"); }
        temp_len = len;
        memcpy(out_buf, in_buf, len);

        /* If we're finding new stuff, let's run for a bit longer, limits
           permitting. */

        if (afl->queued_paths != havoc_queued) {

          if (perf_score <= afl->havoc_max_mult * 100) {

            afl->stage_max *= 2;
            perf_score *= 2;

          }

          havoc_queued = afl->queued_paths;

        }

        if (unlikely(afl->queued_paths + afl->unique_crashes >
                     temp_total_found)) {

          u64 temp_temp_puppet =
              afl->queued_paths + afl->unique_crashes - temp_total_found;
          afl->total_puppet_find = afl->total_puppet_find + temp_temp_puppet;

          if (MOpt_globals.is_pilot_mode) {

            for (i = 0; i < operator_num; ++i) {

              if (MOpt_globals.cycles_v2[i] > MOpt_globals.cycles_v3[i]) {

                MOpt_globals.finds_v2[i] += temp_temp_puppet;

              }

            }

          } else {

            for (i = 0; i < operator_num; i++) {

              if (afl->core_operator_cycles_puppet_v2[i] >
                  afl->core_operator_cycles_puppet_v3[i])

                afl->core_operator_finds_puppet_v2[i] += temp_temp_puppet;

            }

          }

        }                                                             /* if */

      } /* for (afl->stage_cur = 0; afl->stage_cur < afl->stage_max;

           ++afl->stage_cur) { */

      new_hit_cnt = afl->queued_paths + afl->unique_crashes;

      if (MOpt_globals.is_pilot_mode) {

        if (!splice_cycle) {

          afl->stage_finds[STAGE_HAVOC] += new_hit_cnt - orig_hit_cnt;
          afl->stage_cycles[STAGE_HAVOC] += afl->stage_max;

        } else {

          afl->stage_finds[STAGE_SPLICE] += new_hit_cnt - orig_hit_cnt;
          afl->stage_cycles[STAGE_SPLICE] += afl->stage_max;

        }

      }

#ifndef IGNORE_FINDS

      /************
       * SPLICING *
       ************/

    retry_splicing_puppet:

      if (afl->use_splicing &&
          splice_cycle++ < (u32)afl->SPLICE_CYCLES_puppet &&
          afl->ready_for_splicing_count > 1 && afl->queue_cur->len >= 4) {

        struct queue_entry *target;
        u32                 tid, split_at;
        u8 *                new_buf;
        s32                 f_diff, l_diff;

        /* First of all, if we've modified in_buf for havoc, let's clean that
           up... */

        if (in_buf != orig_in) {

          in_buf = orig_in;
          len = afl->queue_cur->len;

        }

        /* Pick a random queue entry and seek to it. Don't splice with yourself.
         */

        do {

          tid = rand_below(afl, afl->queued_paths);

        } while (tid == afl->current_entry || afl->queue_buf[tid]->len < 4);

        afl->splicing_with = tid;
        target = afl->queue_buf[tid];

        /* Read the testcase into a new buffer. */
        new_buf = queue_testcase_get(afl, target);

        /* Find a suitable splicin g location, somewhere between the first and
           the last differing byte. Bail out if the difference is just a single
           byte or so. */

        locate_diffs(in_buf, new_buf, MIN(len, target->len), &f_diff, &l_diff);

        if (f_diff < 0 || l_diff < 2 || f_diff == l_diff) {

          goto retry_splicing_puppet;

        }

        /* Split somewhere between the first and last differing byte. */

        split_at = f_diff + rand_below(afl, l_diff - f_diff);

        /* Do the thing. */

        len = target->len;
        afl->in_scratch_buf = afl_realloc(AFL_BUF_PARAM(in_scratch), len);
        memcpy(afl->in_scratch_buf, in_buf, split_at);
        memcpy(afl->in_scratch_buf + split_at, new_buf, len - split_at);
        in_buf = afl->in_scratch_buf;
        afl_swap_bufs(AFL_BUF_PARAM(in), AFL_BUF_PARAM(in_scratch));

        out_buf = afl_realloc(AFL_BUF_PARAM(out), len);
        if (unlikely(!out_buf)) { PFATAL("alloc"); }
        memcpy(out_buf, in_buf, len);

        goto havoc_stage_puppet;

      }                                                  /* if splice_cycle */

#endif                                                     /* !IGNORE_FINDS */

      ret_val = 0;

    abandon_entry:
    abandon_entry_puppet:

      if ((s64)splice_cycle >= afl->SPLICE_CYCLES_puppet) {

        afl->SPLICE_CYCLES_puppet =
            (rand_below(
                 afl, SPLICE_CYCLES_puppet_up - SPLICE_CYCLES_puppet_low + 1) +
             SPLICE_CYCLES_puppet_low);

      }

      afl->splicing_with = -1;

      /* Update afl->pending_not_fuzzed count if we made it through the
         calibration cycle and have not seen this entry before. */
      /*
        // TODO FIXME: I think we need this plus need an -L -1 check
        if (!afl->stop_soon && !afl->queue_cur->cal_failed &&
            (afl->queue_cur->was_fuzzed == 0 || afl->queue_cur->fuzz_level == 0)
        && !afl->queue_cur->disabled) {

          if (!afl->queue_cur->was_fuzzed) {

            --afl->pending_not_fuzzed;
            afl->queue_cur->was_fuzzed = 1;
            if (afl->queue_cur->favored) { --afl->pending_favored; }

          }

        }

      */

      orig_in = NULL;

      if (afl->key_puppet == 1) {

        if (unlikely(
                afl->queued_paths + afl->unique_crashes >
                ((afl->queued_paths + afl->unique_crashes) * limit_time_bound +
                 afl->orig_hit_cnt_puppet))) {

          afl->key_puppet = 0;
          afl->orig_hit_cnt_puppet = 0;
          afl->last_limit_time_start = 0;

        }

      }

      if (unlikely(*MOpt_globals.pTime > MOpt_globals.period)) {

        afl->total_pacemaker_time += *MOpt_globals.pTime;
        *MOpt_globals.pTime = 0;
        new_hit_cnt = afl->queued_paths + afl->unique_crashes;

        if (MOpt_globals.is_pilot_mode) {

          afl->swarm_fitness[afl->swarm_now] =
              (double)(afl->total_puppet_find - afl->temp_puppet_find) /
              ((double)(afl->tmp_pilot_time) / afl->period_pilot_tmp);

        }

        afl->temp_puppet_find = afl->total_puppet_find;
        u64 temp_stage_finds_puppet = 0;
        for (i = 0; i < operator_num; ++i) {

          if (MOpt_globals.is_pilot_mode) {

            double temp_eff = 0.0;

            if (MOpt_globals.cycles_v2[i] > MOpt_globals.cycles[i]) {

              temp_eff =
                  (double)(MOpt_globals.finds_v2[i] - MOpt_globals.finds[i]) /
                  (double)(MOpt_globals.cycles_v2[i] - MOpt_globals.cycles[i]);

            }

            if (afl->eff_best[afl->swarm_now][i] < temp_eff) {

              afl->eff_best[afl->swarm_now][i] = temp_eff;
              afl->L_best[afl->swarm_now][i] = afl->x_now[afl->swarm_now][i];

            }

          }

          MOpt_globals.finds[i] = MOpt_globals.finds_v2[i];
          MOpt_globals.cycles[i] = MOpt_globals.cycles_v2[i];
          temp_stage_finds_puppet += MOpt_globals.finds[i];

        }                                    /* for i = 0; i < operator_num */

        if (MOpt_globals.is_pilot_mode) {

          afl->swarm_now = afl->swarm_now + 1;
          if (afl->swarm_now == swarm_num) {

            afl->key_module = 1;
            for (i = 0; i < operator_num; ++i) {

              afl->core_operator_cycles_puppet_v2[i] =
                  afl->core_operator_cycles_puppet[i];
              afl->core_operator_cycles_puppet_v3[i] =
                  afl->core_operator_cycles_puppet[i];
              afl->core_operator_finds_puppet_v2[i] =
                  afl->core_operator_finds_puppet[i];

            }

            double swarm_eff = 0.0;
            afl->swarm_now = 0;
            for (i = 0; i < swarm_num; ++i) {

              if (afl->swarm_fitness[i] > swarm_eff) {

                swarm_eff = afl->swarm_fitness[i];
                afl->swarm_now = i;

              }

            }

            if (afl->swarm_now < 0 || afl->swarm_now > swarm_num - 1) {

              PFATAL("swarm_now error number  %d", afl->swarm_now);

            }

          }                               /* if afl->swarm_now == swarm_num */

          /* adjust pointers dependent on 'afl->swarm_now' */
          afl->mopt_globals_pilot.finds =
              afl->stage_finds_puppet[afl->swarm_now];
          afl->mopt_globals_pilot.finds_v2 =
              afl->stage_finds_puppet_v2[afl->swarm_now];
          afl->mopt_globals_pilot.cycles =
              afl->stage_cycles_puppet[afl->swarm_now];
          afl->mopt_globals_pilot.cycles_v2 =
              afl->stage_cycles_puppet_v2[afl->swarm_now];
          afl->mopt_globals_pilot.cycles_v3 =
              afl->stage_cycles_puppet_v3[afl->swarm_now];

        } else {

          for (i = 0; i < operator_num; i++) {

            afl->core_operator_finds_puppet[i] =
                afl->core_operator_finds_puppet_v2[i];
            afl->core_operator_cycles_puppet[i] =
                afl->core_operator_cycles_puppet_v2[i];
            temp_stage_finds_puppet += afl->core_operator_finds_puppet[i];

          }

          afl->key_module = 2;

          afl->old_hit_count = new_hit_cnt;

        }                                                  /* if pilot_mode */

      }         /* if (unlikely(*MOpt_globals.pTime > MOpt_globals.period)) */

    }                                                              /* block */

  }                                                                /* block */

  return ret_val;

}

#undef FLIP_BIT

u8 core_fuzzing(afl_state_t *afl) {

  return mopt_common_fuzzing(afl, afl->mopt_globals_core);

}

u8 pilot_fuzzing(afl_state_t *afl) {

  return mopt_common_fuzzing(afl, afl->mopt_globals_pilot);

}

void pso_updating(afl_state_t *afl) {

  afl->g_now++;
  if (afl->g_now > afl->g_max) { afl->g_now = 0; }
  afl->w_now =
      (afl->w_init - afl->w_end) * (afl->g_max - afl->g_now) / (afl->g_max) +
      afl->w_end;
  int tmp_swarm, i, j;
  u64 temp_operator_finds_puppet = 0;
  for (i = 0; i < operator_num; ++i) {

    afl->operator_finds_puppet[i] = afl->core_operator_finds_puppet[i];

    for (j = 0; j < swarm_num; ++j) {

      afl->operator_finds_puppet[i] =
          afl->operator_finds_puppet[i] + afl->stage_finds_puppet[j][i];

    }

    temp_operator_finds_puppet =
        temp_operator_finds_puppet + afl->operator_finds_puppet[i];

  }

  for (i = 0; i < operator_num; ++i) {

    if (afl->operator_finds_puppet[i]) {

      afl->G_best[i] = (double)((double)(afl->operator_finds_puppet[i]) /
                                (double)(temp_operator_finds_puppet));

    }

  }

  for (tmp_swarm = 0; tmp_swarm < swarm_num; ++tmp_swarm) {

    double x_temp = 0.0;
    for (i = 0; i < operator_num; ++i) {

      afl->probability_now[tmp_swarm][i] = 0.0;
      afl->v_now[tmp_swarm][i] =
          afl->w_now * afl->v_now[tmp_swarm][i] +
          RAND_C * (afl->L_best[tmp_swarm][i] - afl->x_now[tmp_swarm][i]) +
          RAND_C * (afl->G_best[i] - afl->x_now[tmp_swarm][i]);
      afl->x_now[tmp_swarm][i] += afl->v_now[tmp_swarm][i];
      if (afl->x_now[tmp_swarm][i] > v_max) {

        afl->x_now[tmp_swarm][i] = v_max;

      } else if (afl->x_now[tmp_swarm][i] < v_min) {

        afl->x_now[tmp_swarm][i] = v_min;

      }

      x_temp += afl->x_now[tmp_swarm][i];

    }

    for (i = 0; i < operator_num; ++i) {

      afl->x_now[tmp_swarm][i] = afl->x_now[tmp_swarm][i] / x_temp;
      if (likely(i != 0)) {

        afl->probability_now[tmp_swarm][i] =
            afl->probability_now[tmp_swarm][i - 1] + afl->x_now[tmp_swarm][i];

      } else {

        afl->probability_now[tmp_swarm][i] = afl->x_now[tmp_swarm][i];

      }

    }

    if (afl->probability_now[tmp_swarm][operator_num - 1] < 0.99 ||
        afl->probability_now[tmp_swarm][operator_num - 1] > 1.01) {

      FATAL("ERROR probability");

    }

  }

  afl->swarm_now = 0;
  afl->key_module = 0;

}

static inline void add_pat_to_list (char_pat *char_pat_list, u32 pos, u8 *char_val, u32 char_num) {

    char_pat *cur_char_pat = char_pat_list + pos;

    cur_char_pat->char_num = char_num;
    if (char_num == 0) {
        cur_char_pat->char_val = NULL;
        return;
    }
    
    cur_char_pat->char_val = (u8*) malloc (sizeof (u8) * char_num);
    assert (cur_char_pat->char_val != NULL);
    memcpy (cur_char_pat->char_val, char_val, char_num);
    
    return;
}


static inline patreg_seed* add_patreg (afl_state_t *afl, u8 *seed_ctx, u32 seed_len) {

    patreg_seed *ps = (patreg_seed*) malloc (sizeof (patreg_seed) + 
                                             sizeof (u8) * seed_len +
                                             sizeof (char_pat) * seed_len + 1);
    assert (ps != NULL);
    
    ps->char_pat_list = (char_pat*)(ps + 1);
    ps->seed_len      = seed_len;
    ps->seed          = afl->queue_cur;
    ps->seed_ctx      = (u8*) (ps->char_pat_list + seed_len);
    memcpy (ps->seed_ctx, seed_ctx, seed_len);
    ps->seed_ctx [seed_len] = 0;
    
    ps->next = afl->patreg_seed_head;
    afl->patreg_seed_head = ps;
    afl->patreg_seed_num++;
    
    return ps;
}

static inline void del_patreg (afl_state_t *afl) {

    patreg_seed *ps = afl->patreg_seed_head;
    while (ps != NULL) {
        afl->patreg_seed_head = ps->next;

        char_pat *cp = ps->char_pat_list;
        u32 pos = 0;
        while (pos < ps->seed_len) {
            free (cp->char_val);
            cp->char_val = NULL;

            pos++;
            cp++;
        }

        free (ps);
        ps = afl->patreg_seed_head;
    }

    return;
}

static inline u8* get_test_name (u8 * out_name)
{   
    if (strstr (out_name, "orig") != NULL) {
        /* id:000000,time:0,orig:test3 */
        u8 *pos = NULL;
        while (*out_name != 0) {
            if (*out_name == ':') {
                pos = out_name;
            }
            out_name++;
        }

        assert (pos != NULL);
        return pos+1;
    }
    else {
        /* id:000005,src:000002,time:198,op:havoc,rep:4 */
        return out_name;
    }  
}

void gen_pattern (afl_state_t *afl) {

//  header  
//    u32 seed_len;
//    u32 char_size;
//    char_pat []
//         u32 char_num
//         u8[] chars
    u8 tests_name[1024];
    
    patreg_seed *ps = afl->patreg_seed_head;
    while (ps != NULL) {

        snprintf (tests_name, sizeof (tests_name), "in/%s.pat", get_test_name (ps->seed->fname));
        printf ("@@@ gen test-name: %s \r\n", tests_name);
        FILE *psf = fopen (tests_name, "wb");
        assert (psf != NULL);
        
        fwrite (&ps->seed_len, 1, sizeof (u32), psf);

        u32 char_size = 0;
        u32 pos = 0;
        while (pos < ps->seed_len) {
            char_size += ps->char_pat_list[pos].char_num;
            char_size += sizeof (u32);
            pos++;
        }
        fwrite (&char_size, 1, sizeof (u32), psf);

        pos = 0;
        char_pat *cp = ps->char_pat_list;
        while (pos < ps->seed_len) {

            fwrite (&cp->char_num, 1, sizeof (u32), psf);
            if (cp->char_num != 0) {
                fwrite (cp->char_val, 1, cp->char_num * sizeof (u8), psf);
            }

            //printf ("\t==> Pos = %u, CP->CharNum = %u \r\n", pos, cp->char_num);
            pos++;
            cp++;
        }

        fclose (psf);
        ps = ps->next;
    }

    
    del_patreg (afl);
    return;
}

/* fuzzing-based pattern recognization of input seeds */
u8 syntax_pl_fuzzing(afl_state_t *afl) {
    
    u8 *in_buf = queue_testcase_get(afl, afl->queue_cur);
    u32 len = afl->queue_cur->len;
    
    u32 map_size = afl->fsrv.real_map_size;
    u8 * trace_bits = afl->fsrv.trace_bits;

    u32 start_bb = 1;
    u8 *str_bb = getenv ("AFL_START_BB");
    if (str_bb != NULL) {
        start_bb = (u32)atoi(str_bb);
    }

    fprintf (stderr, "syntax_pl_fuzzing: %s[%u], start_bb:%u\r\n", afl->queue_cur->fname, afl->queue_cur->len, start_bb);

    patreg_seed *ps = add_patreg (afl, in_buf, len);
    char_pat *char_pat_list = ps->char_pat_list;
    
    u32 pos = 0;
    while (pos < len) {
        u8  origin   = in_buf[pos];

        u8 char_val[256];
        u32 char_num = 0;
        
        u32 byte_val = 0;        
        while (byte_val < 256) {

            if (byte_val == origin) {
                byte_val++;
                continue;
            }

            in_buf[pos] = (u8)byte_val;
            common_fuzz_stuff(afl, in_buf, len);

            #ifdef __DEBUG__
            printf ("[%u]Trace bits: ", byte_val);
            #endif
            u32 path_len = 0;
            for (u32 i = start_bb; i < map_size; i++) {
                if (trace_bits[i]) {
                    path_len++;
                    #ifdef __DEBUG__
                    printf ("%u ", i);
                    #endif
                }
            }
            #ifdef __DEBUG__
            printf ("\r\n");
            #endif

            if (path_len >= afl->threshold_pathlen) {
                char_val[char_num++] = byte_val;
                //printf ("(%u)[%u -> %u]path_length: %u\n", pos, (u32)origin, (u32)byte_val, path_len);
            }

            #ifdef __DEBUG__
            sleep (1);
            #endif
            
            byte_val++;
        }

        add_pat_to_list (char_pat_list, pos, char_val, char_num);
        
        in_buf[pos] = origin;
        pos++;
        fprintf (stderr, ">>pilot-fuzzing:[%u/%u]      \r", pos, len);
    }

    return 0;
}


/* fuzzing-based pattern recognization of input seeds */
u8 semantic_pl_fuzzing(afl_state_t *afl) {
    
    u8 *in_buf = queue_testcase_get(afl, afl->queue_cur);
    u32 len = afl->queue_cur->len;
    
    MsgIB *msg_itb = afl->msg_itb;

    //fprintf (stderr, "syntax_pl_fuzzing: %s[%u] --> var: [off-%u][length-%u] samples: %u\r\n", 
    //         afl->queue_cur->fname, afl->queue_cur->len, msg_itb->SIndex, msg_itb->Length, msg_itb->SampleNum);
    
    u32 sample_num  = msg_itb->SampleNum;
    u8 *var = in_buf + msg_itb->SIndex;
    for (u32 index = 0; index < sample_num; index++) {

        switch (msg_itb->Length)
        {
            case 1:
            {
                u8 origin = *var;
                u8 *sample_addr = (u8*)(msg_itb + 1);
                
                *var = sample_addr[index];
                //fprintf (stderr, "\t[%u][%u:%u] var = %u, len = %u\r\n", index, msg_itb->SIndex, msg_itb->Length, *((u32 *)var), len);
                common_fuzz_stuff(afl, in_buf, len);
                *var = origin;
                 
                break;
            }
            case 2:
            {
                u16 origin = *((u16 *)var);
                u16 *sample_addr = (u16*)(msg_itb + 1);
                
                *((u16 *)var) = sample_addr[index];
                //fprintf (stderr, "\t[%u][%u:%u] var = %u, len = %u\r\n", index, msg_itb->SIndex, msg_itb->Length, *((u32 *)var), len);
                common_fuzz_stuff(afl, in_buf, len);
                *((u16 *)var) = origin;
                
                break;
            }
            case 4:
            {
                u32 origin = *((u32 *)var);
                u32 *sample_addr = (u32*)(msg_itb + 1);
                
                *((u32 *)var) = sample_addr[index];
                //fprintf (stderr, "\t[%u][%u:%u] var = %u, len = %u\r\n", index, msg_itb->SIndex, msg_itb->Length, *((u32 *)var), len);
                common_fuzz_stuff(afl, in_buf, len);
                *((u32 *)var) = origin;
                
                break;
            }
            case 8:
            {
                u64 origin = *((u64 *)var);
                u64 *sample_addr = (u64*)(msg_itb + 1);
                
                *((u64 *)var) = sample_addr[index];
                //fprintf (stderr, "\t[%u][%u:%u] var = %u, len = %u\r\n", index, msg_itb->SIndex, msg_itb->Length, *((u32 *)var), len);
                common_fuzz_stuff(afl, in_buf, len);
                *((u64 *)var) = origin;
                
                break;
            }
            default:
            {
                assert (0);
            }
        }
    }

    return 0;
}


static inline void load_patterns (/*struct queue_entry *q, 
*/ seed_tmpt *stmpt) {

    /* load only once */
    if (stmpt->char_num == 0) {
        int fd = open("char.pat", O_RDONLY);
        if (fd > 0) {
            ck_read(fd, &stmpt->char_num, sizeof (u32), "char.pat");
            assert (stmpt->char_num != 0);
            DEBUG_PRINT ("stmpt->char_num = %u \r\n", stmpt->char_num);
            ck_read(fd, stmpt->char_pattern, sizeof (stmpt->char_pattern), "char.pat");

            u32 j = 0;
            for (u32 i = 0; i < 256; i++) {
                if (stmpt->char_pattern[i] == CHAR_NORMAL) {
                    stmpt->char_set[j++] = i;
                }
            }
            close (fd);
        }

        fd = open("stru.pat", O_RDONLY);
        if (fd > 0) {     
            ck_read(fd, &stmpt->spat_num, sizeof (u32), "stru.pat");
            assert (stmpt->spat_num != 0 && stmpt->spat_num <= MAX_STPAT);

            printf ("stmpt->spat_num = %u \r\n", stmpt->spat_num);
            for (u32 i = 0; i < stmpt->spat_num; i++) {
                strupat *spat = stmpt->spat + i;
                ck_read(fd, &spat->len, sizeof (u32), "stru.pat");
                ck_read(fd, spat->pat, spat->len, "stru.pat");
                printf ("\t[%u][N-%u][%s]\r\n", i, spat->len, spat->pat);
            }
            close (fd);
        }
        else {
            stmpt->spat_num = 0;
        }
    }

#if 0
    u8 tmpt_name[1024];
    u32 len = q->len;

    stmpt->seed_len = 0;
    stmpt->in_tmpt = stmpt->out_tmpt = NULL;
    
    u8 *tname = get_test_name (q->fname);
    if (tname == q->fname) {
        snprintf (tmpt_name, sizeof (tmpt_name), "%s.tmpt", tname);
    }
    else {
        snprintf (tmpt_name, sizeof (tmpt_name), "in/%s.tmpt", tname);
    }

    int fd = open(tmpt_name, O_RDONLY);
    if (unlikely(fd < 0)) { return; }

    ck_read(fd, &stmpt->seed_len, sizeof (u32), tmpt_name);
    if (stmpt->seed_len != len) {
        stmpt->seed_len = 0;
        close(fd);
        return;
    }

    stmpt->in_tmpt = (u8*) malloc (len);
    assert (stmpt->in_tmpt != NULL);
    ck_read(fd, stmpt->in_tmpt, len, tmpt_name);
 
    stmpt->out_tmpt = NULL;  
    close(fd);
#endif

    return;
}


static inline u32 gen_random_item (afl_state_t *afl, 
                                            seed_tmpt *stmpt, strupat *spat, 
                                            u8* buf, u32 buf_size)
{
    u32 len = 0;
    u32 r_num  = 1 + rand_below(afl, 4);
    for (u32 n = 0; n < r_num; n++) {
        for (u32 i = 0; i < spat->len; i++) {
            if (spat->pat [i] != 'B') {
                if (len < buf_size)
                    buf[len++] = spat->pat [i];
            }
            else {
                /* random generate */
                u32 r_index = 1 + rand_below(afl, stmpt->char_num);
                u32 r_len = 1 + rand_below(afl, spat->len*4);
                for (u32 j = 0; j < r_len; j++) {
                    if (len < buf_size)
                        buf[len++] = stmpt->char_set [r_index];
                }
            }
        }

        buf[len] = 0;
        //printf ("[%u]%s  -----  buf[%u] = %s \r\n", n, spat->pat, len, buf);
    }

    return len;
}



static inline u32 havoc_mutate (afl_state_t *afl, u8 **cur_out_buf, u32 out_len) {

    u8 *out_buf = *cur_out_buf;
    u32 r_max, r;

#define MAX_HAVOC_ENTRY 59                                      /* 55 to 60 */

    r_max = (MAX_HAVOC_ENTRY + 1) + (afl->extras_cnt ? 4 : 0) + (afl->a_extras_cnt ? 4 : 0);

/* We essentially just do several thousand runs (depending on perf_score)
   where we take the input file and make random stacked tweaks. */
#define FLIP_BIT(_ar, _b)                       \
              do {                                      \
                                                        \
                u8 *_arf = (u8 *)(_ar);                 \
                u32 _bf = (_b);                         \
                _arf[(_bf) >> 3] ^= (128 >> ((_bf)&7));\
                                                        \
              } while (0)
   
    u32 use_stacking = 1 << (1 + rand_below(afl, afl->havoc_stack_pow2));
    afl->stage_cur_val = use_stacking;
    for (u32 i = 0; i < use_stacking; ++i) {
    
        switch ((r = rand_below(afl, r_max))) {
    
            case 0 ... 3: {
    
                /* Flip a single bit somewhere. Spooky! */
                FLIP_BIT(out_buf, rand_below(afl, out_len << 3));
                break;
            }
    
            case 4 ... 7: {
    
                /* Set byte to interesting value. */
                out_buf[rand_below(afl, out_len)] = interesting_8[rand_below(afl, sizeof(interesting_8))];
                break;
            }
    
            case 8 ... 9: {
    
                /* Set word to interesting value, randomly choosing endian. */
                if (out_len < 2) { break; }
                *(u16 *)(out_buf + rand_below(afl, out_len - 1)) = interesting_16[rand_below(afl, sizeof(interesting_16) >> 1)];
                break;
            }
    
            case 10 ... 11: {
    
                /* Set word to interesting value, randomly choosing endian. */
                if (out_len < 2) { break; }
                *(u16 *)(out_buf + rand_below(afl, out_len - 1)) = SWAP16(interesting_16[rand_below(afl, sizeof(interesting_16) >> 1)]);
                break;
            }
    
            case 12 ... 13: {
    
                /* Set dword to interesting value, randomly choosing endian. */
                if (out_len < 4) { break; }
                *(u32 *)(out_buf + rand_below(afl, out_len - 3)) = interesting_32[rand_below(afl, sizeof(interesting_32) >> 2)];
                break;
            }
    
            case 14 ... 15: {
    
                /* Set dword to interesting value, randomly choosing endian. */
                if (out_len < 4) { break; }
                *(u32 *)(out_buf + rand_below(afl, out_len - 3)) = SWAP32(interesting_32[rand_below(afl, sizeof(interesting_32) >> 2)]);
                break;
            }
    
            case 16 ... 19: {
    
                /* Randomly subtract from byte. */
                out_buf[rand_below(afl, out_len)] -= 1 + rand_below(afl, ARITH_MAX);
                break;
            }
    
            case 20 ... 23: {
    
                /* Randomly add to byte. */
                out_buf[rand_below(afl, out_len)] += 1 + rand_below(afl, ARITH_MAX);
                break;
            }
    
            case 24 ... 25: {
                    
                /* Randomly subtract from word, little endian. */
                if (out_len < 2) { break; }
                u32 pos = rand_below(afl, out_len - 1);
                *(u16 *)(out_buf + pos) -= 1 + rand_below(afl, ARITH_MAX);
                break;
            }
    
            case 26 ... 27: {
    
                /* Randomly subtract from word, big endian. */
                if (out_len < 2) { break; }
    
                u32 pos = rand_below(afl, out_len - 1);
                u16 num = 1 + rand_below(afl, ARITH_MAX);
                *(u16 *)(out_buf + pos) =
                SWAP16(SWAP16(*(u16 *)(out_buf + pos)) - num);
                break;
            }
    
            case 28 ... 29: {
    
                /* Randomly add to word, little endian. */
                if (out_len < 2) { break; }
    
                u32 pos = rand_below(afl, out_len - 1);
                *(u16 *)(out_buf + pos) += 1 + rand_below(afl, ARITH_MAX);
                break;
            }
    
            case 30 ... 31: {
    
                /* Randomly add to word, big endian. */
                if (out_len < 2) { break; }
    
                u32 pos = rand_below(afl, out_len - 1);
                u16 num = 1 + rand_below(afl, ARITH_MAX);
                *(u16 *)(out_buf + pos) =
                SWAP16(SWAP16(*(u16 *)(out_buf + pos)) + num);
                break;
            }
    
            case 32 ... 33: {
    
                /* Randomly subtract from dword, little endian. */
                if (out_len < 4) { break; }
    
                u32 pos = rand_below(afl, out_len - 3);
                *(u32 *)(out_buf + pos) -= 1 + rand_below(afl, ARITH_MAX);
                break;
            }
    
            case 34 ... 35: {
    
                /* Randomly subtract from dword, big endian. */
                if (out_len < 4) { break; }
    
                u32 pos = rand_below(afl, out_len - 3);
                u32 num = 1 + rand_below(afl, ARITH_MAX);
    
                *(u32 *)(out_buf + pos) =
                SWAP32(SWAP32(*(u32 *)(out_buf + pos)) - num);
                break;
            }
    
            case 36 ... 37: {
    
                /* Randomly add to dword, little endian. */
                if (out_len < 4) { break; }
    
                u32 pos = rand_below(afl, out_len - 3);
                *(u32 *)(out_buf + pos) += 1 + rand_below(afl, ARITH_MAX);
                break;
            }
    
            case 38 ... 39: {
    
                /* Randomly add to dword, big endian. */
                if (out_len < 4) { break; }
    
                u32 pos = rand_below(afl, out_len - 3);
                u32 num = 1 + rand_below(afl, ARITH_MAX);
                *(u32 *)(out_buf + pos) =
                SWAP32(SWAP32(*(u32 *)(out_buf + pos)) + num);
                break;
            }
    
            case 40 ... 43: {
    
                /* Just set a random byte to a random value. Because,
                   why not. We use XOR with 1-255 to eliminate the possibility of a no-op. */
                out_buf[rand_below(afl, out_len)] ^= 1 + rand_below(afl, 255);
                break;
            }
    
            case 44 ... 46: {
    
                if (out_len + HAVOC_BLK_XL < MAX_FILE) {
    
                       /* Clone bytes. */
                       u32 clone_len = choose_block_len(afl, out_len);
                       u32 clone_from = rand_below(afl, out_len - clone_len + 1);
                       u32 clone_to = rand_below(afl, out_len);
    
                       u8 *new_buf = afl_realloc(AFL_BUF_PARAM(out_scratch), out_len + clone_len);
                       if (unlikely(!new_buf)) { PFATAL("alloc"); }
    
                       /* Head */
                       memcpy(new_buf, out_buf, clone_to);
    
                       /* Inserted part */
                       memcpy(new_buf + clone_to, out_buf + clone_from, clone_len);
    
                       /* Tail */
                       memcpy(new_buf + clone_to + clone_len, out_buf + clone_to, out_len - clone_to);
    
                       out_buf = new_buf;
                       afl_swap_bufs(AFL_BUF_PARAM(out), AFL_BUF_PARAM(out_scratch));
                       out_len += clone_len;
                 }
                 break;
            }
    
            case 47: {
    
                 if (out_len + HAVOC_BLK_XL < MAX_FILE) {
    
                        /* Insert a block of constant bytes (25%). */
                        u32 clone_len = choose_block_len(afl, HAVOC_BLK_XL);
                        u32 clone_to = rand_below(afl, out_len);
    
                        u8 *new_buf = afl_realloc(AFL_BUF_PARAM(out_scratch), out_len + clone_len);
                        if (unlikely(!new_buf)) { PFATAL("alloc"); }
    
                        /* Head */
                        memcpy(new_buf, out_buf, clone_to);
    
                        /* Inserted part */
                        memset(new_buf + clone_to,
                                   rand_below(afl, 2) ? rand_below(afl, 256):out_buf[rand_below(afl, out_len)], clone_len);
    
                        /* Tail */
                        memcpy(new_buf + clone_to + clone_len, out_buf + clone_to, out_len - clone_to);
    
                        out_buf = new_buf;
                        afl_swap_bufs(AFL_BUF_PARAM(out), AFL_BUF_PARAM(out_scratch));
                        out_len += clone_len;
                }
    
                break;
            }
    
            case 48 ... 50: {
    
                /* Overwrite bytes with a randomly selected chunk bytes. */
                 if (out_len < 2) { break; }
    
                 u32 copy_len = choose_block_len(afl, out_len - 1);
                 u32 copy_from = rand_below(afl, out_len - copy_len + 1);
                 u32 copy_to = rand_below(afl, out_len - copy_len + 1);
    
                 if (likely(copy_from != copy_to)) {
                    memmove(out_buf + copy_to, out_buf + copy_from, copy_len);
                 }
                break;
            }
    
            case 51: {
    
                /* Overwrite bytes with fixed bytes. */
                if (out_len < 2) { break; }
    
                u32 copy_len = choose_block_len(afl, out_len - 1);
                u32 copy_to = rand_below(afl, out_len - copy_len + 1);
                memset(out_buf + copy_to,
                       rand_below(afl, 2) ? rand_below(afl, 256):out_buf[rand_below(afl, out_len)], copy_len);
                break;
            }
    
            // increase from 4 up to 8?
            case 52 ... MAX_HAVOC_ENTRY: {
    
                /* Delete bytes. We're making this a bit more likely
                           than insertion (the next option) in hopes of keeping files reasonably small. */
                if (out_len < 2) { break; }
    
                /* Don't delete too much. */
                u32 del_len = choose_block_len(afl, out_len - 1);
                u32 del_from = rand_below(afl, out_len - del_len + 1);
                memmove(out_buf + del_from, out_buf + del_from + del_len, out_len - del_from - del_len);
                out_len -= del_len;
                break;
            }
    
            default: 
                break;
        }
    
    }

    *cur_out_buf = out_buf;
    return out_len;
}


/* PL-based fuzzing  */
u8 pl_offical_fuzzing(afl_state_t *afl) {
    u8 ret_val = 1, doing_det = 0;
    u8 *in_buf, *orig_in, *out_buf;
    u32 len, temp_len;
    u64 path_queued = 0, orig_hit_cnt, new_hit_cnt = 0;
    u32 splice_cycle = 0;
    u32 cross_paths = 0;
  
    /* Map the test case into memory. */
    orig_in = in_buf = queue_testcase_get(afl, afl->queue_cur);
    len     = afl->queue_cur->len;
    out_buf = afl_realloc(AFL_BUF_PARAM(out), len);
    assert (out_buf != NULL);

    seed_tmpt *stmpt = &afl->stmpt;
    load_patterns(/*afl->queue_cur, */stmpt);
    
    afl->subseq_tmouts = 0;
    afl->cur_depth = afl->queue_cur->depth;

    /*******************************************
     * CALIBRATION (only if failed earlier on) *
     *******************************************/
    if (unlikely(afl->queue_cur->cal_failed)) {
        u8 res = FSRV_RUN_TMOUT;
        if (afl->queue_cur->cal_failed < CAL_CHANCES) {
            afl->queue_cur->exec_cksum = 0;
            res = calibrate_case(afl, afl->queue_cur, in_buf, afl->queue_cycle - 1, 0);
            if (res == FSRV_RUN_ERROR) FATAL("Unable to execute target application");
        }

        if (afl->stop_soon || res != afl->crash_mode) {
            ++afl->cur_skipped_paths;
            goto abandon_entry;
        }
    }

    /*******************************************
     * TRIMMING                                *
     *******************************************/
    if (unlikely(!afl->non_instrumented_mode && !afl->queue_cur->trim_done && !afl->disable_trim)) {    
        u32 old_len = afl->queue_cur->len;    
        u8 res = trim_case(afl, afl->queue_cur, in_buf);
        in_buf = queue_testcase_get(afl, afl->queue_cur); 
        if (unlikely(res == FSRV_RUN_ERROR)) FATAL("Unable to execute target application");
        if (unlikely(afl->stop_soon)) {  
            ++afl->cur_skipped_paths;
            goto abandon_entry;
        }
    
        /* Don't retry trimming, even if it failed. */
        afl->queue_cur->trim_done = 1;   
        len = afl->queue_cur->len;
        /* maybe current entry is not ready for splicing anymore */
        if (unlikely(len <= 4 && old_len > 4)) --afl->ready_for_splicing_count;    
    }    
    memcpy(out_buf, in_buf, len);


    /*******************************************
     * PERFORMANCE SCORE                       *
     *******************************************/
    u32 perf_score, orig_perf;
    if (likely(!afl->old_seed_selection))
        orig_perf = perf_score = afl->queue_cur->perf_score;
    else
        afl->queue_cur->perf_score = orig_perf = perf_score = calculate_score(afl, afl->queue_cur);    
    if (unlikely(perf_score <= 0)) { goto abandon_entry; }


    /*****************************************************
     * Following character-patterns: byte mutation       *
     *****************************************************/
    if (stmpt->char_num == 0) goto havoc_stage;
    if (afl->queue_cur->was_fuzzed) goto byteinc_stage;
    
    afl->stage_name  = "byte-op";
    afl->stage_short = "byte-op";
    afl->stage_max   = len;
    u32 pos=0, random_byte;
    u8 valid_byte=0, orgi_byte;
    path_queued = afl->queued_paths;
    for (afl->stage_cur = 0; afl->stage_cur < afl->stage_max; ++afl->stage_cur) {
        pos = afl->stage_cur;
        if (stmpt->char_pattern[in_buf[pos]] == CHAR_CRUCIAL) continue;

        for (u32 times = 0; times < stmpt->char_num; times++) {
            random_byte = times;//rand_below(afl, stmpt->char_num);
            valid_byte  = stmpt->char_set [random_byte];
            if (valid_byte == pos || stmpt->char_pattern[valid_byte] == CHAR_CRUCIAL) {
                continue;
            }

            orgi_byte    = out_buf[pos];
            out_buf[pos] = valid_byte;
            if (common_fuzz_stuff(afl, out_buf, len)) { goto abandon_entry; }
            out_buf[pos] = orgi_byte;            
        }
    }
    afl->queue_cur->was_fuzzed = true;

    /*****************************************************
     * Following character-patterns: increase bytes      *
     *****************************************************/ 
byteinc_stage:
    afl->stage_name  = "byte-inc";
    afl->stage_short = "byte-inc";
    afl->stage_max   = HAVOC_CYCLES + stmpt->char_num;
    path_queued = afl->queued_paths;
    cross_paths = afl->stmpt.cross_paths;
    
    u8 random_bytes[64];   
    for (afl->stage_cur = 0; afl->stage_cur < afl->stage_max; ++afl->stage_cur) {

        u32 r_offset = rand_below(afl, len);
        while (stmpt->char_pattern[in_buf[r_offset]] == CHAR_CRUCIAL) {
            r_offset = rand_below(afl, len);
        }

        u32 r_size = 1 + rand_below(afl, sizeof (random_bytes));
        for (u32 i = 0; i < r_size; i++)
        {
            random_bytes [i] = in_buf[r_offset];
        }
        random_bytes[r_size] = 0;

        u8 *new_buf = afl_realloc(AFL_BUF_PARAM(out_scratch), len + r_size + 1);
        if (unlikely(!new_buf)) { PFATAL("alloc"); }

        /* insert the random bytes */
        memmove(new_buf, out_buf, r_offset+1);
        memmove(new_buf+r_offset+1, random_bytes, r_size);
        memmove(new_buf+r_offset+1+r_size, out_buf+r_offset+1, len-r_offset-1);

        u32 total_len = len + r_size;
        new_buf[total_len] = 0;   
        out_buf = new_buf;

        if (common_fuzz_stuff(afl, out_buf, total_len)) { goto abandon_entry; }

        /* restore out_buf. */
        out_buf = afl_realloc(AFL_BUF_PARAM(out), len);
        if (unlikely(!out_buf)) { PFATAL("alloc"); }
        memcpy(out_buf, in_buf, len);

        if (afl->queued_paths != path_queued) {
            afl->stage_max = afl->stage_max + HAVOC_CYCLES;
            path_queued = afl->queued_paths;

            if (cross_paths != afl->stmpt.cross_paths)
                afl->stage_max += HAVOC_CYCLES;
            cross_paths = afl->stmpt.cross_paths;
        }   
    }

    
    /*****************************************************
     * Following stru-patterns: increase item E.G.,"B":B,*
     *****************************************************/
    if (stmpt->spat_num == 0) 
        goto havoc_stage;
    
    afl->stage_name  = "stru-inc";
    afl->stage_short = "stru-inc";
    afl->stage_max   = HAVOC_CYCLES + stmpt->char_num;
    path_queued = afl->queued_paths;
    cross_paths = afl->stmpt.cross_paths;
    
    u32 pt_offset[MAX_STPAT];
    for (u32 sn = 0; sn < stmpt->spat_num; sn++) {
        u8 Sb = stmpt->spat[sn].pat[0];
        for (u32 pos = 0; pos < len; pos++) {
            if (in_buf[pos] == Sb || 
                stmpt->char_pattern[in_buf[pos]] != CHAR_CRUCIAL) {
                pt_offset [sn] = pos;
                //printf ("[%u]%s ---> %s --- start at %u \r\n", sn, in_buf, stmpt->spat[sn].pat, pt_offset [sn]);
                break;
            }
        }
    }

    u8 item_buf [256];
    for (afl->stage_cur = 0; afl->stage_cur < afl->stage_max; ++afl->stage_cur) {

        for (u32 sn = 0; sn < stmpt->spat_num; sn++) {
            strupat *spat = stmpt->spat + sn;
            
            u32 st_off = pt_offset[sn];         
            u32 item_len = gen_random_item (afl, stmpt, spat, item_buf, sizeof (item_buf));

            u8 *new_buf = afl_realloc(AFL_BUF_PARAM(out_scratch), len + item_len + 1);
            if (unlikely(!new_buf)) { PFATAL("alloc"); }

            /* insert the random bytes */
            out_buf [len] = 0;
            memmove(new_buf, out_buf, st_off);
            memmove(new_buf+st_off, item_buf, item_len);
            memmove(new_buf+st_off+item_len, out_buf+st_off, len-st_off);

            u32 total_len = len + item_len;
            new_buf[total_len] = 0;   
            out_buf = new_buf;

            //printf ("[st_off:%u]out_buf = %s <---> new_buf = %s \r\n", st_off, out_buf, new_buf);
            //sleep (2);

            if (common_fuzz_stuff(afl, out_buf, total_len)) { goto abandon_entry; }
                
            
            /* restore out_buf. */
            out_buf = afl_realloc(AFL_BUF_PARAM(out), len);
            if (unlikely(!out_buf)) { PFATAL("alloc"); }
            memcpy(out_buf, in_buf, len);
        }

        if (afl->queued_paths != path_queued) {
            afl->stage_max = afl->stage_max + HAVOC_CYCLES;
            path_queued = afl->queued_paths;

            if (cross_paths != afl->stmpt.cross_paths)
                afl->stage_max += HAVOC_CYCLES;
            cross_paths = afl->stmpt.cross_paths;
        }  
    }

havoc_stage:

    /*******************************************
     * Random mutate: havoc                    *
     *******************************************/
    afl->stage_cur_byte = -1;

    /* The havoc stage mutation code is also invoked when splicing files; if the
       splice_cycle variable is set, generate different descriptions and such. */

    if (!splice_cycle) {

        afl->stage_name = "havoc";
        afl->stage_short = "havoc";
        afl->stage_max = (doing_det ? HAVOC_CYCLES_INIT : HAVOC_CYCLES) *
                         perf_score / afl->havoc_div / 100;
    } else {
        perf_score = orig_perf;
        snprintf(afl->stage_name_buf, STAGE_BUF_SIZE, "splice %u", splice_cycle);
        afl->stage_name = afl->stage_name_buf;
        afl->stage_short = "splice";
        afl->stage_max = SPLICE_HAVOC * perf_score / afl->havoc_div / 100;
    }
    if (afl->stage_max < HAVOC_MIN) { afl->stage_max = HAVOC_MIN; }

    temp_len = len;
    orig_hit_cnt = afl->queued_paths + afl->unique_crashes;
    path_queued = afl->queued_paths;
    cross_paths = afl->stmpt.cross_paths;

    for (afl->stage_cur = 0; afl->stage_cur < afl->stage_max; ++afl->stage_cur) {

        temp_len = havoc_mutate (afl, &out_buf, temp_len);

        if (common_fuzz_stuff(afl, out_buf, temp_len)) { goto abandon_entry; }

        /* out_buf might have been mangled a bit, so let's restore it to its
           original size and shape. */
        out_buf = afl_realloc(AFL_BUF_PARAM(out), len);
        if (unlikely(!out_buf)) { PFATAL("alloc"); }
        temp_len = len;
        memcpy(out_buf, in_buf, len);

        /* If we're finding new stuff, let's run for a bit longer, limits permitting. */
        if (afl->queued_paths != path_queued) {
            if (perf_score <= afl->havoc_max_mult * 100) {
                afl->stage_max = afl->stage_max*2;
                perf_score *= 2;

                if (cross_paths != afl->stmpt.cross_paths)
                    afl->stage_max += HAVOC_CYCLES;
            }

            path_queued = afl->queued_paths;
            cross_paths = afl->stmpt.cross_paths;
        }   

    }

    new_hit_cnt = afl->queued_paths + afl->unique_crashes;
    if (!splice_cycle) {    
        afl->stage_finds[STAGE_HAVOC] += new_hit_cnt - orig_hit_cnt;
        afl->stage_cycles[STAGE_HAVOC] += afl->stage_max;
    
    } else {    
        afl->stage_finds[STAGE_SPLICE] += new_hit_cnt - orig_hit_cnt;
        afl->stage_cycles[STAGE_SPLICE] += afl->stage_max;    
    }

    
    /**********************************
     * SPLICING                       *
     **********************************/
    
    /* This is a last-resort strategy triggered by a full round with no findings.
       It takes the current input file, randomly selects another input, and
       splices them together at some offset, then relies on the havoc
       code to mutate that blob. */
    
retry_splicing:
    
    if (afl->use_splicing && splice_cycle++ < SPLICE_CYCLES &&
        afl->ready_for_splicing_count > 1 && afl->queue_cur->len >= 4) {
    
        struct queue_entry *target;
        u32                 tid, split_at;
        u8 *                new_buf;
        s32                 f_diff, l_diff;
    
        /* First of all, if we've modified in_buf for havoc, let's clean that
           up... */   
        if (in_buf != orig_in) {   
            in_buf = orig_in;
            len = afl->queue_cur->len; 
        }
    
        /* Pick a random queue entry and seek to it. Don't splice with yourself. */  
        do {
            tid = rand_below(afl, afl->queued_paths); 
        } while (tid == afl->current_entry || afl->queue_buf[tid]->len < 4);
    
        /* Get the testcase */
        afl->splicing_with = tid;
        target = afl->queue_buf[tid];
        new_buf = queue_testcase_get(afl, target);
    
        /* Find a suitable splicing location, somewhere between the first and
           the last differing byte. Bail out if the difference is just a single byte or so. */ 
        locate_diffs(in_buf, new_buf, MIN(len, (s64)target->len), &f_diff, &l_diff);
    
        if (f_diff < 0 || l_diff < 2 || f_diff == l_diff) { goto retry_splicing; }
    
        /* Split somewhere between the first and last differing byte. */ 
        split_at = f_diff + rand_below(afl, l_diff - f_diff);
    
        /* Do the thing. */   
        len = target->len;
        afl->in_scratch_buf = afl_realloc(AFL_BUF_PARAM(in_scratch), len);
        memcpy(afl->in_scratch_buf, in_buf, split_at);
        memcpy(afl->in_scratch_buf + split_at, new_buf, len - split_at);
        in_buf = afl->in_scratch_buf;
        afl_swap_bufs(AFL_BUF_PARAM(in), AFL_BUF_PARAM(in_scratch));
    
        out_buf = afl_realloc(AFL_BUF_PARAM(out), len);
        if (unlikely(!out_buf)) { PFATAL("alloc"); }
        memcpy(out_buf, in_buf, len);
    
        goto havoc_stage;  
    }

    ret_val = 0;
/* we are through with this queue entry - for this iteration */
abandon_entry:
    //free (stmpt->in_tmpt);
    free (stmpt->out_tmpt);
    ++afl->queue_cur->fuzz_level;    
    return ret_val;
}



static inline void havoc_fuzzing(afl_state_t *afl, u8 *in_buf, u32 len) {

    u8 *out_buf;
    u32 temp_len;

    out_buf = afl_realloc(AFL_BUF_PARAM(out), len);
    assert (out_buf != NULL);
    memcpy(out_buf, in_buf, len);
    temp_len = len;

    afl->stage_name  = "onfly_havoc";
    afl->stage_short = "onfly_havoc";
    afl->stage_max   =  afl->pl_havoc_num;

    common_fuzz_stuff(afl, out_buf, temp_len);
    for (afl->stage_cur = 0; afl->stage_cur < afl->stage_max; ++afl->stage_cur) {

        temp_len = havoc_mutate (afl, &out_buf, temp_len);

        common_fuzz_stuff(afl, out_buf, temp_len);

        /* out_buf might have been mangled a bit, so let's restore it to its original size and shape. */
        out_buf = afl_realloc(AFL_BUF_PARAM(out), len);
        if (unlikely(!out_buf)) { PFATAL("alloc"); }
        
        temp_len = len;
        memcpy(out_buf, in_buf, len);
    }
 
    return;
}



void read_seed_fuzz(afl_state_t *afl, u8 *dir, u8 rem) 
{
    struct dirent **nl;
    s32             nl_cnt;
    u32             i;
    struct stat st;
    
    nl_cnt = scandir(dir, &nl, NULL, alphasort);
    if (nl_cnt < 0)
    {
        return;
    }
    ACTF("Scanning %s, get seeds: %u\r\n", dir, nl_cnt);

    /* for these seed, we only consider the PC */
    setenv("AFL_TRACE_DU_SHUTDOWN", "1", 1);

    /* select random seeds to save */
    u32 seed_nos[SAVE_SEED_PER_UNIT] = {-1};
    u32 unit_size = nl_cnt/SAVE_SEED_PER_UNIT;
    if (nl_cnt > 0) {
        for (u32 ix = 0; ix < SAVE_SEED_PER_UNIT; ix++)
        {
            (void)unit_size;
            //seed_nos [ix] = ix*unit_size + random () % unit_size;
        }
    }

    u32 random_saved;
    i = nl_cnt;
    do {
        --i;
        random_saved = -1;

        if (nl[i]->d_name[0] == '.') {
            free(nl[i]);
            continue;            
        }
        u8 *seed = alloc_printf("%s/%s", dir, nl[i]->d_name);

        if (lstat(seed, &st) || access(seed, R_OK)) {
            PFATAL("Unable to access '%s'", seed);
        }
        
        u32 len = (u32)st.st_size;
        u8 *buf = afl_realloc((void **)&afl->testcase_buf, len);
        if (unlikely(!buf)) {
            PFATAL("Unable to malloc '%s' with len %u", seed, len);
        }

        int fd = open(seed, O_RDONLY);
        if (unlikely(fd < 0)) { PFATAL("Unable to open '%s'", seed); }
        ck_read(fd, buf, len, seed);
        close(fd);

        for (u32 ix = 0; ix < SAVE_SEED_PER_UNIT; ix++)
        {
            if (seed_nos[ix] == i) {
                random_saved = i;
                break;
            }
        }

        if (i != random_saved) {
            if (rem)
                remove (seed);
        }
        else {
            u8 *target = alloc_printf("in/%s", nl[i]->d_name);
            rename (seed, target);
            add_to_queue(afl, target, st.st_size >= MAX_FILE ? MAX_FILE : st.st_size, 1);
        }
        
        havoc_fuzzing(afl, buf, len);
        free (nl[i]);
        ck_free(seed);
          
    } while (i > 0);

    free(nl);
    unsetenv ("AFL_TRACE_DU_SHUTDOWN");
    return;
}


u8 fuzz_one_standard(afl_state_t *afl)
{
    MsgHdr *msg_header;

    if (afl->queue_cur->is_send == 0) {
        msg_header = format_msg (PL_MSG_SEED);
        MsgSeed *msg_seed = (MsgSeed*)(msg_header + 1);
        msg_seed->SeedKey = afl->current_entry;       
        char* seed_path   = (char*) (msg_seed + 1);         
        char *cur_dir = get_current_dir_name ();
        sprintf (seed_path, "%s/%s", cur_dir, afl->queue_cur->fname);
        msg_seed->SeedLength = strlen (seed_path)+1;
        msg_header->MsgLen += sizeof (MsgSeed) + msg_seed->SeedLength;
        afl->queue_cur->is_send = 1;
        free (cur_dir);
        //OKF (">>[fuzz_one_standard]send PL_MSG_SEED: [%u]%s", msg_seed->SeedKey, seed_path);
    }
    else {
        msg_header = format_msg (PL_MSG_EMPTY);
        //OKF (">>[fuzz_one_standard]send PL_MSG_EMPTY: [%u]", afl->current_entry);
    }
    pl_send ((char*)msg_header, msg_header->MsgLen);

    /* wait response before start fuzzing */
    msg_header = (MsgHdr *)pl_recv();
    
    u8 ret = fuzz_one_original(afl);
    if (afl->pl_stat > (u32)afl->init_seed)
    {
        read_seed_fuzz(afl, "../tests/", 0);
        afl->init_seed = -1;
    }
    
    switch (msg_header->MsgType)
    {
        case PL_MSG_SWMODE:
        {
            switch_fz_mode(pl_mode_pilot);
            msg_header = format_msg (PL_MSG_SWMODE_READY);
            pl_send ((char*)msg_header, msg_header->MsgLen);
            ret = 0; /* skip the next seed */
            //OKF (">>[fuzz_one_standard]recv PL_MSG_SWMODE, send PL_MSG_SWMODE_READY back.");
            break;
        }
        case PL_MSG_SEED:
        case PL_MSG_EMPTY:
        {
            break;
        }
        case PL_MSG_GEN_SEED:
        case PL_MSG_GEN_SEED_DONE:
        {
            CLEAR_SCRING;
            
            char *seed_dir = (char *)(msg_header + 1);      
            //OKF ("[fuzz_one_standard] recv PL_MSG_GEN_SEED: %s, queued_paths:%u", seed_dir, afl->queued_paths);
            read_seed_fuzz(afl, seed_dir, 1);
            if (!(afl->pl_stat%4))
            {
                read_seed_fuzz(afl, "../tests/", 0);
            }
            //OKF ("[fuzz_one_standard] finish READING new seeds, queued_paths:%u", afl->queued_paths);

            msg_header = format_msg (msg_header->MsgType);
            pl_send ((char*)msg_header, msg_header->MsgLen);
            
            break;
        }
        default:
        {
            //OKF (">>[fuzz_one_standard]unexpected msg type: %u", msg_header->MsgType);
            assert (0);
        }
    }

    afl->pl_stat++;

    //OKF (">>[fuzz_one_standard]seed %u fuzzing done.", afl->current_entry);
    return ret;
}


/* larger change for MOpt implementation: the original fuzz_one was renamed
   to fuzz_one_original. All documentation references to fuzz_one therefore
   mean fuzz_one_original */

u8 fuzz_one(afl_state_t *afl) {

  int key_val_lv_1 = 0, key_val_lv_2 = 0;

#ifdef _AFL_DOCUMENT_MUTATIONS

  u8 path_buf[PATH_MAX];
  if (afl->do_document == 0) {

    snprintf(path_buf, PATH_MAX, "%s/mutations", afl->out_dir);
    afl->do_document = mkdir(path_buf, 0700);  // if it exists we do not care
    afl->do_document = 1;

  } else {

    afl->do_document = 2;
    afl->stop_soon = 2;

  }

#endif

  switch (afl->pl_fuzzing_type) {
    case PL_SYNTAX_FZ:
        return syntax_pl_fuzzing (afl);
    case PL_SEMANTIC_FZ:
        if (get_fz_mode() == pl_mode_pilot)
        {
            return semantic_pl_fuzzing (afl);
        }
        else
        {
            return fuzz_one_standard (afl);
        }
    default:
        break;
  }


  // if limit_time_sig == -1 then both are run after each other

  if (afl->limit_time_sig <= 0) { key_val_lv_1 = fuzz_one_original(afl); }

  if (afl->limit_time_sig != 0) {

    if (afl->key_module == 0) {

      key_val_lv_2 = pilot_fuzzing(afl);

    } else if (afl->key_module == 1) {

      key_val_lv_2 = core_fuzzing(afl);

    } else if (afl->key_module == 2) {

      pso_updating(afl);

    }

  }

  return (key_val_lv_1 | key_val_lv_2);

}

