/* Quagga pselect support -- header
 * Copyright (C) 2009 Chris Hall (GMCH), Highwayman
 *
 * This file is part of GNU Zebra.
 *
 * GNU Zebra is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2, or (at your
 * option) any later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Zebra; see the file COPYING.  If not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <signal.h>
#include <string.h>

#include "zassert.h"
#include "qpselect.h"
#include "qpthreads.h"
#include "qtime.h"
#include "memory.h"
#include "vector.h"

/*==============================================================================
 * Quagga pselect -- qps_xxxx
 *
 * Here and in qpselect.h is a data structure for managing multiple file
 * descriptors and running pselect to wait for I/O activity and to multiplex
 * between the file descriptors.
 *
 * The qps_selection structure manages a collection of file descriptors which
 * are to be waited on together in a pselect statement.
 *
 * NB: it is ASSUMED that a qps_selection will be private to the thread in
 *     which it is created and used.
 *
 *     There is NO mutex handling here.
 *
 * This supports pselect, so supports:
 *
 *   * waiting for file descriptors, which may each be expecting any combination
 *     of error/read/write events.
 *
 *     Files may be added or removed from the selection.  Files in the selection
 *     may then be enabled/disabled for any combination of error/read/write
 *     "mode" events.
 *
 *   * a timeout *time*
 *
 *     This is a qtime monotonic time at which to time out.  (This is unlike
 *     pselect() itself, which takes a timeout interval.)
 *
 *     Infinite timeouts are not supported.
 *
 *   * an optional signal number and sigmask
 *
 *     So that a signal may be used to interrupt a waiting pselect.
 *
 *     For this to work there must be a signal which is generally masked, and
 *     is unmasked for the duration of the pselect.
 *
 * When a pselect returns there may be a number of files with events pending.
 * The qps_dispatch_next() calls the action routine for the next event to be
 * dealt with.  Events are dispatched in the order: error, read and write, and
 * then in file descriptor order.  (So all error events in fd order, then all
 * read events, and so on.)
 *
 * Note that at no time are any modes automatically disabled.  So the system is
 * level triggered.  So, for example, a read event that is not dealt with will
 * be triggered again on the next pselect -- unless the read mode is explicitly
 * disabled for the file.
 *
 * Action Functions
 * ----------------
 *
 * There is a separate action function for each mode.  Each file has its own
 * set of action functions -- so these may be used to implement a form of
 * state machine for the file.
 *
 * When the action function is called it is passed the qps_file structure and
 * the file_info pointer from that structure.
 *
 * During an action function modes may be enabled/disabled, actions changed,
 * the file removed from the selection... there are no restrictions.
 */

static int qps_super_set_map_made = 0 ;

static void qps_make_super_set_map(void) ;

/*==============================================================================
 * qps_selection handling
 */

/* Forward references   */
static qps_file qps_file_lookup_fd(qps_selection qps, int fd, qps_file insert) ;
static void qps_file_remove(qps_selection qps, qps_file qf) ;
static void qps_super_set_zero(fd_super_set* p_set, int n) ;
static int qps_next_fd_pending(fd_super_set* pending, int fd, int fd_last) ;
static void qps_selection_validate(qps_selection qps) ;

/* See qps_make_super_set_map() and qps_pselect() below.        */
static short fd_byte_count[FD_SETSIZE] ;    /* number of bytes for fds 0..fd */

/* Initialise a selection -- allocating it if required.
 *
 * Returns the qps_selection.
 */
qps_selection
qps_selection_init_new(qps_selection qps)
{
  if (!qps_super_set_map_made)
    qps_make_super_set_map() ;  /* map the fd_super_set         */

  if (qps == NULL)
    qps = XCALLOC(MTYPE_QPS_SELECTION, sizeof(struct qps_selection)) ;
  else
    memset(qps, 0, sizeof(struct qps_selection)) ;

  /* Zeroising initialises:
   *
   *   fd_count      -- no fd's yet
   *   fd_direct     -- not direct lookup
   *
   *   files         -- empty vector
   *
   *   fd_last       -- unset
   *   enabled_count -- no fd's enabled in any mode
   *   enabled       -- empty bit vectors
   *
   *   tried_fd_last -- nothing tried yet
   *   tried_count   -- nothing tried yet
   *   results       -- empty bit vectors
   *
   *   pend_count    -- no results to dispatch
   *   pend_mnum     -- unset
   *   pend_fd       -- unset
   *
   *   signum        -- no signal to be enabled
   *   sigmask       -- unset
   *
   * So nothing else to do -- see also qps_selection_re_init(), below.
   */

  return qps ;
} ;

/* Re-initialise a selection.
 */
static void
qps_selection_re_init(qps_selection qps)
{
  memset(qps, 0, sizeof(struct qps_selection)) ;
} ;

/* Add given file to the selection, setting its fd and pointer to further
 * file information.  All modes are disabled.
 *
 * This initialises most of the qps_file structure, but not the actions.
 *
 * Adding a file using the same fd as an existing file is a FATAL error.
 *
 * Adding a file which is already a member a selection is a FATAL error.
 */
void
qps_add_file(qps_selection qps, qps_file qf, int fd, void* file_info)
{
  passert(qf->selection == NULL) ;

  qf->selection    = qps ;

  qf->file_info    = file_info ;
  qf->fd           = fd ;

  qf->enabled_bits = 0 ;

  qps_file_lookup_fd(qps, fd, qf) ;     /* Add. */
} ;

/* Remove given file from its selection, if any.
 *
 * It is the callers responsibility to ensure that the file is in a suitable
 * state to be removed from the selection.
 *
 * When the file is removed it is disabled in all modes.
 */
void
qps_remove_file(qps_file qf)
{
  if (qf->selection != NULL)
    qps_file_remove(qf->selection, qf) ;
} ;

/* Ream (another) file out of the selection.
 *
 * If selection is empty, release the qps_selection structure, if required.
 *
 * See: #define qps_selection_ream_free(qps)
 *      #define qps_selection_ream_keep(qps)
 *
 * Useful for emptying out and discarding a selection:
 *
 *     while ((qf = qps_selection_ream_free(qps)))
 *       ... do what's required to release the qps_file
 *
 * The file is removed from the selection before being returned.
 *
 * Returns NULL when selection is empty (and has been released, if required).
 *
 * If the selection is not released, it may be reused without reinitialisation.
 *
 * NB: once reaming has started, the selection MUST NOT be used for anything,
 *     and the process MUST be run to completion.
 */
qps_file
qps_selection_ream(qps_selection qps, int free_structure)
{
  qps_file qf ;

  qf = vector_ream_keep(&qps->files) ;
  if (qf != NULL)
    qps_file_remove(qps, qf) ;
  else
    {
      passert(qps->fd_count == 0) ;

      if (free_structure)
        XFREE(MTYPE_QPS_SELECTION, qps) ;
      else
        qps_selection_re_init(qps) ;
    } ;

  return qf ;
} ;

/* Set the signal mask for the selection.
 *
 * This supports the unmasking of a single signal for the duration of the
 * pselect operation.
 *
 * It is assumed that the set of signals generally masked by a thread is
 * essentially static.  So this function is passed that set.  (So the sigmask
 * argument must have the signum signal masked.)
 *
 * If the set of signals masked by the thread changes, then this function
 * should be called again.
 *
 * Setting a signum == 0 turns OFF the use of the sigmask.
 */
void
qps_set_signal(qps_selection qps, int signum, sigset_t sigmask)
{
  qps->signum  = signum ;

  if (signum != 0)
    {
      assert(sigismember(&sigmask, signum)) ;
      sigdelset(&sigmask, signum) ;
      qps->sigmask = sigmask ;
    } ;
} ;

/* Execute a pselect for the given selection -- subject to the given timeout
 * *time*.
 *
 * The time-out time is an "absolute" time, as measured by qt_get_monotonic().
 *
 * A timeout time <= the current qt_get_monotonic() is treated as a zero
 * timeout period, and will return immediately from the pselect.
 *
 * There is no support for an infinite timeout.
 *
 * Returns: -1 => EINTR occurred -- ie a signal has gone off
 *           0 => hit timeout -- no files are ready
 *         > 0 => there are this many files ready in one or more modes
 *
 * All other errors are FATAL.
 *
 * The qps_dispatch_next() processes the returns from pselect().
 */
int
qps_pselect(qps_selection qps, qtime_mono_t timeout)
{
  struct timespec ts ;
  qps_mnum_t  mnum ;
  fd_set*     p_fds[qps_mnum_count] ;
  int  n ;

  /* TODO: put this under a debug skip                                  */
  qps_selection_validate(qps) ;

  /* If there is stuff still pending, tidy up by zeroising the result   */
  /* vectors.  This is to make sure that when bits are copied from      */
  /* the enabled vectors, there are none from a previous run of pselect */
  /* left hanging about.  (pselect SHOULD ignore everything above the   */
  /* given count of fds -- but it does no harm to be tidy, and should   */
  /* not have to do this often.)                                        */
  if (qps->pend_count != 0)
      qps_super_set_zero(qps->results, qps_mnum_count) ;

  /* Prepare the argument/result bitmaps                */
  /* Capture pend_mnum and tried_count[]                */

  n   = fd_byte_count[qps->fd_last] ; /* copy up to last sig. byte  */

  qps->pend_mnum = qps_mnum_count ;
  for (mnum = 0 ; mnum < qps_mnum_count ; ++mnum)
    if ((qps->tried_count[mnum] = qps->enabled_count[mnum]) != 0)
      {
        memcpy(&(qps->results[mnum].bytes), &(qps->enabled[mnum].bytes), n) ;
        p_fds[mnum] = &(qps->results[mnum].fdset) ;
        if (mnum < qps->pend_mnum)
          qps->pend_mnum = mnum ;
      }
    else
      p_fds[mnum] = NULL ;

  /* Capture tried_fd_last and set initial pend_fd.     */
  qps->tried_fd_last = qps->fd_last ;
  qps->pend_fd       = 0 ;

  /* Convert timeout time to interval for pselect()     */
  timeout -= qt_get_monotonic() ;
  if (timeout < 0)
    timeout = 0 ;

  /* Finally ready for the main event                   */
  n = pselect(qps->fd_last + 1, p_fds[qps_read_mnum],
                                p_fds[qps_write_mnum],
                                p_fds[qps_error_mnum],
                                qtime2timespec(&ts, timeout),
                                (qps->signum != 0) ? &qps->sigmask : NULL) ;

  /* If have something, set and return the pending count.               */
  if (n > 0)
    {
      assert(qps->pend_mnum < qps_mnum_count) ; /* expected something   */

      return qps->pend_count = n ;      /* set and return pending count */
    } ;

  /* Flush the results vectors -- not apparently done if n <= 0)        */
  qps_super_set_zero(qps->results, qps_mnum_count) ;

  qps->pend_count = 0 ;                 /* nothing pending              */

  /* Return appropriately, if we can                                    */
  if ((n == 0) || (errno == EINTR))
    return n ;

  zabort_errno("Failed in pselect") ;
} ;

/* Dispatch the next errored/readable/writeable file, as returned by the
 * most recent qps_pselect().
 *
 * Processes the errored files, then the readable and lastly the writeable.
 *
 * Processes one file per call of this function, by invoking the file's
 * "action" routine.
 *
 * If a given file is ready in more than one mode, all modes will be processed,
 * unless the action routine for one mode disables the file for other modes, or
 * removes it from the selection.
 *
 * Returns the number of files left to process (after the one just processed).
 */
int
qps_dispatch_next(qps_selection qps)
{
  int        fd ;
  qps_file   qf ;
  qps_mnum_t mnum ;

  /* TODO: put this under a debug skip                                  */
  qps_selection_validate(qps) ;

  if (qps->pend_count == 0)
    return 0 ;                  /* quit immediately of nothing to do.   */

  fd   = qps->pend_fd ;
  mnum = qps->pend_mnum ;

  dassert( (mnum >= 0) && (mnum < qps_mnum_count)
                       && (qps->tried_count[mnum] != 0)
                       && (qps->pend_count > 0) ) ;

  while (1)
    {
      fd = qps_next_fd_pending(&(qps->results[mnum]), fd, qps->tried_fd_last) ;
      if (fd >= 0)
        break ;                 /* easy if have another fd in current mode.  */

      do                        /* step to next mode that was not empty      */
        {
          qps->tried_count[mnum] = 0 ;  /* tidy up as we go     */
          ++mnum ;
          if (mnum >= qps_mnum_count)
            zabort("Unexpectedly ran out of pending stuff") ;
        } while (qps->tried_count[mnum] == 0) ;

      qps->pend_mnum = mnum ;   /* update mode                  */
      fd = 0 ;                  /* back to the beginning        */
    } ;

  qps->pend_count -= 1 ;        /* one less pending             */
  qps->pend_fd     = fd ;       /* update scan                  */

  qf = qps_file_lookup_fd(qps, fd, NULL) ;

  dassert( ((qf->enabled_bits && qps_mbit(mnum)) != 0) &&
           (qf->actions[mnum] != NULL) ) ;

  qf->actions[mnum](qf, qf->file_info) ;  /* dispatch the required action */

  return qps->pend_count ;
} ;

/*==============================================================================
 * qps_file structure handling
 */

/* Initialise qps_file structure -- allocating one if required.
 *
 * If a template is given, then the action functions are copied from there to
 * the new structure.  See above for discussion of action functions.
 *
 * Once initialised, the file may be added to a selection.
 *
 * Returns the qps_file.
 */
qps_file
qps_file_init_new(qps_file qf, qps_file template)
{
  if (qf == NULL)
    qf = XCALLOC(MTYPE_QPS_FILE, sizeof(struct qps_file)) ;
  else
    memset(qf, 0, sizeof(struct qps_file)) ;

  /* Zeroising has initialised:
   *
   *   selection     -- NULL  -- ditto
   *
   *   file_info     -- NULL  -- is set by qps_add_file()
   *   fd            -- unset -- ditto
   *
   *   enabled_bits  -- nothing enabled
   *
   *   actions[]     -- all set to NULL
   */

  if (template != NULL)
    memcpy(qf->actions, template->actions, sizeof(qf->actions)) ;

  return qf ;
} ;

/* Free dynamically allocated qps_file structure.
 *
 * It is the caller's responsibility to have removed it from any selection it
 * may have been in.
 */
void
qps_file_free(qps_file qf)
{
  assert(qf->selection == NULL) ;       /* Mustn't be a selection member ! */

  XFREE(MTYPE_QPS_FILE, qf) ;
} ;

/* Enable (or re-enable) file for the given mode.
 *
 * If the action argument is not NULL, set the action for the mode.
 *
 * NB: It is a FATAL error to enable a mode with a NULL action.
 *
 * NB: It is a FATAL error to enable modes for a file which is not in a
 *     selection.
 */
void
qps_enable_mode(qps_file qf, qps_mnum_t mnum, qps_action* action)
{
  qps_mbit_t    mbit = qps_mbit(mnum) ;
  qps_selection qps  = qf->selection ;

  dassert(qps != NULL) ;
  dassert((mnum >= 0) && (mnum <= qps_mnum_count)) ;

  if (action != NULL)
    qf->actions[mnum] = action ;
  else
    dassert(qf->actions[mnum] != NULL) ;

  if (qf->enabled_bits & mbit)
    dassert(FD_ISSET(qf->fd, &(qps->enabled[mnum].fdset))) ;
  else
    {
      dassert( ! FD_ISSET(qf->fd, &(qps->enabled[mnum].fdset))) ;
      FD_SET(qf->fd, &(qps->enabled[mnum].fdset)) ;
      ++qps->enabled_count[mnum] ;
      qf->enabled_bits |= mbit ;
    } ;
} ;

/* Set action for given mode -- does not enable/disable.
 *
 * May unset an action by setting it NULL !
 *
 * See above for discussion of action functions.
 *
 * NB: it is a fatal error to unset an action for a mode which is enabled.
 */
void
qps_set_action(qps_file qf, qps_mnum_t mnum, qps_action* action)
{
  dassert((mnum >= 0) && (mnum <= qps_mnum_count)) ;

  if (action == NULL)
    passert((qf->enabled_bits & qps_mbit(mnum)) == 0) ;

  qf->actions[mnum] = action ;
} ;

/* Disable file for one or more modes.
 *
 * If there are any pending pending results for the modes, those are discarded.
 *
 * Note that this is modestly "optimised" to deal with disabling a single mode.
 * (Much of the time only the write mode will be being disabled !)
 *
 * NB: it is safe to disable modes which are not enabled -- even if the file
 *     is not currently a member of a selection.  (If it is not a member of a
 *     collection no modes should be enabled !)
 */

static qps_mnum_t qps_first_mnum[qps_mbit(qps_mnum_count)] =
  {
     -1,  /* 0 -> -1 -- no bit set      */
      0,  /* 1 ->  0 -- B0 is first bit */
      1,  /* 2 ->  1 -- B1 is first bit */
      1,  /* 3 ->  1 -- B1 is first bit */
      2,  /* 4 ->  2 -- B2 is first bit */
      2,  /* 5 ->  2 -- B2 is first bit */
      2,  /* 6 ->  2 -- B2 is first bit */
      2   /* 7 ->  2 -- B2 is first bit */
  } ;

CONFIRM(qps_mbit(qps_mnum_count) == 8) ;

void
qps_disable_modes(qps_file qf, qps_mbit_t mbits)
{
  qps_mnum_t  mnum ;

  qps_selection qps = qf->selection ;

  dassert((mbits >= 0) && (mbits <= qps_all_mbits)) ;

  mbits &= qf->enabled_bits ;   /* don't bother with any not enabled  */
  qf->enabled_bits ^= mbits ;   /* unset what we're about to disable  */

  while (mbits != 0)
    {
      mnum = qps_first_mnum[mbits] ;

      dassert(qps->enabled_count[mnum] > 0) ;
      dassert(FD_ISSET(qf->fd, &(qps->enabled[mnum].fdset))) ;

      FD_CLR(qf->fd, &(qps->enabled[mnum].fdset)) ;
      --qps->enabled_count[mnum] ;

      if ((qps->pend_count != 0) && (qps->tried_count[mnum] != 0)
                       && (FD_ISSET(qf->fd, &(qps->results[mnum].fdset))))
        {
          FD_CLR(qf->fd, &(qps->results[mnum].fdset)) ;
          --qps->pend_count ;
        } ;

      mbits ^= qps_mbit(mnum) ;
    } ;
} ;

/*==============================================================================
 * Handling the files vector.
 *
 * For small numbers of fd's, the files vector is kept as a list, in fd order.
 * Files are found by binary chop, and added/removed by insert/delete in the
 * list.
 *
 * For large numbers of fd's, the files vector is kept as an array, indexed by
 * fd.
 */

/* Comparison function for binary chop  */
static int
qps_fd_cmp(const int** pp_fd, const qps_file* p_qf)
{
  if (**pp_fd  < (*p_qf)->fd)
    return -1 ;
  if (**pp_fd  > (*p_qf)->fd)
    return +1 ;
  return 0 ;
}

/* Lookup/Insert file by file-descriptor.
 *
 * Inserts if insert argument is not NULL.
 *
 * Returns the file we found (if any) or the file we just inserted.
 *
 * NB: FATAL error to insert file with same fd as an existing one.
 */
static qps_file
qps_file_lookup_fd(qps_selection qps, int fd, qps_file insert)
{
  qps_file qf ;
  vector_index i ;
  int   ret ;

  dassert((fd >= 0) && (fd < FD_SETSIZE)) ;

  /* Look-up                                                            */
  /*                                                                    */
  /* Set i = index for entry in files vector                            */
  /* Set ret = 0 <=> i is exact index.                                  */
  /*         < 0 <=> i is just after where entry may be inserted        */
  /*         > 0 <=> i is just before where entry may be inserted       */
  if (qps->fd_direct)
    {
      i   = fd ;        /* index of entry       */
      ret = 0 ;         /* how to insert, if do */
    }
  else
    i = vector_bsearch(&qps->files, (vector_bsearch_cmp*)qps_fd_cmp,
                                                                    &fd, &ret) ;
  if (ret == 0)
    qf = vector_get_item(&qps->files, i) ;      /* NULL if not there    */
  else
    qf = NULL ;                                 /* not there            */

  /* Insert now, if required and can: keep fd_count and fd_last up to date.  */
  if (insert != NULL)
    {
      if (qf != NULL)
        zabort("File with given fd already exists in qps_selection") ;

      /* If required, change up to a directly addressed files vector.   */
      if (!qps->fd_direct && (qps->fd_count > 9))
        {
          vector   tmp ;

          tmp = vector_move_here(NULL, &qps->files) ;

          while ((qf = vector_pop_item(tmp)) != NULL)
            vector_set_item(&qps->files, qf->fd, qf) ;

          vector_free(tmp) ;

          qps->fd_direct = 1 ;

          i   = fd ;    /* index is now the fd  */
          ret = 0 ;     /* and insert there     */
        } ;

      /* Now can insert accordint to i & ret                    */
      vector_insert_item_here(&qps->files, i, ret, insert) ;

      ++qps->fd_count ;
      if (fd > qps->fd_last)
        qps->fd_last = fd ;

      qf = insert ;     /* will return what we just inserted.   */
    } ;

  /* Sanity checking.                                                   */
  dassert( (qf == NULL) || ((qps == qf->selection) && (fd == qf->fd)) ) ;

  /* Return the file we found or inserted.                              */
  return qf ;
} ;

/* Remove file from selection.
 *
 * NB: FATAL error if file is not in the selection, or the file-descriptor
 *     is invalid (or refers to some other file !).
 */
static void
qps_file_remove(qps_selection qps, qps_file qf)
{
  qps_file qfd ;
  int   fd_last ;

  passert((qf->fd >= 0) && (qf->fd <= qps->fd_last) && (qps == qf->selection)) ;

  /* Look-up and remove.                                                */
  if (qps->fd_direct)
    {
      qfd     = vector_unset_item(&qps->files, qf->fd) ; /* NULL if not there */
      fd_last = vector_end(&qps->files) - 1 ;
    }
  else
    {
      qps_file qf_last ;
      int ret ;
      vector_index i = vector_bsearch(&qps->files,
                                      (vector_bsearch_cmp*)qps_fd_cmp,
                                      &qf->fd, &ret) ;
      if (ret == 0)
        qfd = vector_delete_item(&qps->files, i) ;
      else
        qfd = NULL ;

      qf_last = vector_get_last_item(&qps->files) ;
      if (qf_last != NULL)
        fd_last = qf_last->fd ;
      else
        fd_last = -1 ;
    } ;

  passert(qfd == qf) ;  /* must have been there and be the expected file  */

  /* Keep fd_count and fd_last up to date.                              */
  dassert(qps->fd_count > 0) ;
  --qps->fd_count ;

  dassert( ((qps->fd_count != 0) && (fd_last >= 0)) ||
           ((qps->fd_count == 0) && (fd_last <  0)) ) ;

  qps->fd_last = (fd_last >= 0) ? fd_last : 0 ;

  /* Also, remove the from all vectors.                                 */
  qps_disable_modes(qf, qps_all_mbits) ;

  /* Is no longer in the selection.                                     */
  qf->selection = NULL ;
} ;

 /*==============================================================================
 * fd_super_set support.
 *
 * For large sets of file descriptors something faster than testing for all
 * possible bits is required.  The fd_super_set assumes that the fd_set is a
 * straightforward bit-vector, and overlays a 32-bit word array and a byte
 * array over that.
 *
 * Cannot tell if the underlying bit vector is arranged in bytes, or some
 * longer words.  Cannot tell if words are held big or little endian.  Cannot
 * tell if lowest numbered fd will be highest or lowest in whatever unit it's
 * held in.
 *
 * So...  we have maps for fd -> our word index, and fd -> byte index.
 *
 *        we have a map for fd -> mask for bit used in its byte.
 *
 * We require that fds will be numbered consistently in bytes.  That is,
 * every (fd mod 8) == n will appear in the same bit in a byte, for all fd (
 * for n = 0..7).  This allows the final map, which takes a byte value and
 * returns the lowest numbered fd in the byte, mod 8.
 *
 * To copy all the bytes for all descriptors 0..fd, also construct
 * fd_byte_count[] -- which copes with the fact that on a big-endian machine
 * it is possible that descriptor fd - 8 may be in a higher numbered byte than
 * fd !  Using this count assumes that the underlying system really does not
 * look at bits beyond the given maximum fd.
 */

static short    fd_word_map[FD_SETSIZE] ; /* maps fd to word index         */
static short    fd_byte_map[FD_SETSIZE] ; /* maps fd to byte index         */
static uint8_t  fd_bit_map [FD_SETSIZE] ; /* maps fd to bit in byte        */

static int8_t   fd_first_map[256] ; /* maps byte value to 0..7, where that */
                                    /* is the lowest fd bit set in byte.   */

#define QPS_TESTING    0        /* true => testing                      */

#if !QPS_TESTING

/* Not testing, so map to the standard FD_SET etc. functions.   */
# define qFD_SET   FD_SET
# define qFD_CLR   FD_CLR
# define qFD_ISSET FD_ISSET
# define qFD_ZERO  FD_ZERO

#else

/* Set up the testing                                           */

# define QPS_TEST_WORD  4       /* Wordsize                             */
# define QPS_TEST_BE    1       /* true => big-endian                   */
# define QPS_TEST_B_ORD 07      /* 07 => bits 0..7, 70 => bits 7..0     */

# define QPS_TEST_WORD_BITS  (QPS_TEST_WORD * 8)
# if QPS_TEST_BE
# define QPS_BYTE(fd) ( ((fd / QPS_TEST_WORD_BITS) * QPS_TEST_WORD) \
                       + (QPS_TEST_WORD - 1) - ((fd % QPS_TEST_WORD_BITS) / 8) )
#else
# define QPS_BYTE(fd) ( fd / 8 )
#endif

# if QPS_TEST_B_ORD == 07
#  define QPS_BIT(fd) (0x01 << (fd & 0x7))
# else
#  define QPS_BIT(fd) (0x80 >> (fd & 0x7))
# endif

  static void
  qFD_SET(int fd, fd_set* set)
  {
    *((uint8_t*)set + QPS_BYTE(fd)) |= QPS_BIT(fd) ;
  } ;

  static void
  qFD_CLR(int fd, fd_set* set)
  {
    *((uint8_t*)set + QPS_BYTE(fd)) &= ~QPS_BIT(fd) ;
  } ;

  static int
  qFD_ISSET(int fd, fd_set* set)
  {
    return (*((uint8_t*)set + QPS_BYTE(fd)) & QPS_BIT(fd)) != 0 ;
  } ;

  static void
  qFD_ZERO(fd_set* set)
  {
    memset(set, 0, sizeof(fd_set)) ;
  } ;

#endif

/* Scan for next fd in given fd set, and clear it.
 *
 * Starts at the given fd, will not consider anything above fd_last.
 *
 * Returns next fd, or -1 if none.
 */
static int
qps_next_fd_pending(fd_super_set* pending, int fd, int fd_last)
{
  uint8_t b ;

  while (pending->words[fd_word_map[fd]] == 0)  /* step past zero words */
    {
      fd = (fd & ~ (FD_WORD_BITS - 1)) + FD_WORD_BITS ;
                                        /* step to start of next word   */
      if (fd > fd_last)
        return -1 ;                     /* quit if past last            */
    } ;

  fd &= ~0x0007 ;                       /* step back to first in byte   */
  while ((b = pending->bytes[fd_byte_map[fd]]) == 0)
    {
      fd += 8 ;
      if (fd > fd_last)
        return -1 ;
    } ;

  fd += fd_first_map[b] ;

  dassert(fd <= fd_last) ;
  dassert((b & fd_bit_map[fd]) == fd_bit_map[fd]) ;

  FD_CLR(fd, &pending->fdset) ;

  dassert((b ^ fd_bit_map[fd]) == pending->bytes[fd_byte_map[fd]]) ;

  return fd ;
} ;

/* Make a map of the fd_super_set.
 *
 * The form of an fd_set is not defined.  This code verifies that it is, in
 * fact a bit vector, and hence that the fd_super_set works here !
 *
 * It is a FATAL error if things don't work out.
 */
static void
qps_make_super_set_map(void)
{
  fd_super_set test ;
  int fd, i, iw, ib ;

  /* (1) check that a zeroised fd_super_set is an empty one.    */
  qps_super_set_zero(&test, 1) ;

  for (fd = 0 ; fd < FD_SETSIZE ; ++fd)
    if (qFD_ISSET(fd, &test.fdset))
      zabort("Zeroised fd_super_set is not empty") ;

  /* (2) check that zeroising the fd_set doesn't change things  */
  qFD_ZERO(&test.fdset) ;
  for (iw = 0 ; iw < FD_SUPER_SET_WORD_SIZE ; ++iw)
    if (test.words[iw] != 0)
      zabort("Zeroised fd_super_set is not all zero words") ;

  /* (3) check that setting one fd sets one bit, and construct the      */
  /*     fd_word_map[], fd_byte_map[] and fd_bit_map[].                 */
  for (fd = 0 ; fd < FD_SETSIZE ; ++fd)
    {
      fd_word_t w ;

      qFD_SET(fd, &test.fdset) ;

      w = 0 ;
      for (iw = 0 ; iw < FD_SUPER_SET_WORD_SIZE ; ++iw)
        {
          if (test.words[iw] != 0)
            {
              if (w != 0)
                zabort("FD_SET set a bit in more than one word") ;

              w = test.words[iw] ;
              if ((w == 0) || ((w & (w - 1)) != 0))
                zabort("FD_SET set more than one bit in a word") ;

              fd_word_map[fd] = iw ;

              ib = iw * FD_WORD_BYTES ;
              while (test.bytes[ib] == 0)
                {
                  ++ib ;
                  if (ib >= ((iw + 1) * FD_WORD_BYTES))
                    zabort("FD_SET set something beyond the expected bytes") ;
                } ;
              fd_byte_map[fd] = ib ;
              fd_bit_map[fd]  = test.bytes[ib] ;
            } ;
        } ;

        if (w == 0)
          zabort("FD_SET did not set any bit in any word") ;

      qFD_CLR(fd, &test.fdset) ;

      for (iw = 0 ; iw < FD_SUPER_SET_WORD_SIZE ; ++iw)
        if (test.words[iw] != 0)
          zabort("FD_CLR did not leave the fd_super_set empty") ;
    } ;

  /* (4) check the fd_byte_map.                                         */
  /*     make sure that have  8 contiguous fd to a byte.                */
  /*     make sure that have 32 contiguous fd to a word.                */

  for (fd = 0 ; fd < FD_SETSIZE ; fd += 8)
    {
      int fds ;
      ib  = fd_byte_map[fd] ;
      iw  = fd_word_map[fd] ;

      /* Must share the same byte as the next 7 fds                     */
      for (fds = fd + 1 ; fds < (fd + 8) ; ++fds)
        if (fd_byte_map[fds] != ib)
          zabort("Broken fd_byte_map -- not 8 contiguous fd's in a byte") ;

      /* Must not share the same byte as any other set of 8 fd's        */
      for (fds = 0 ; fds < FD_SETSIZE ; fds += 8)
        if ((fd_byte_map[fds] == ib) && (fds != fd))
          zabort("Broken fd_byte_map -- fd's not in expected bytes") ;

      /* Must be one of the bytes in the current word's fd's            */
      if ( (ib < (iw * FD_WORD_BYTES)) || (ib >= ((iw + 1) * FD_WORD_BYTES)) )
        zabort("Broken fd_byte_map -- fd's not in expected words") ;
    } ;

  /* (5) check the fd_bit_map                                           */
  /*     make sure that all fd mod 8 map to the same byte value         */

  for (i = 0 ; i < 8 ; ++i)
    {
      uint8_t b = fd_bit_map[i] ;
      for (fd = 8 + i ; fd < FD_SETSIZE ; fd += 8)
        if (fd_bit_map[fd] != b)
          zabort("Broken fd_bit_map -- inconsistent bit mapping") ;
    } ;

  /* (6) construct fd_first_map, to get lowest numbered fd (mod 8) from */
  /*     a given byte value.                                            */

  for (i = 0 ; i < 256 ; ++i)
    fd_first_map[i] = -1 ;

  for (fd = 0 ; fd < 8 ; ++fd)
    {
      uint8_t fdb = fd_bit_map[fd] ;
      for (i = 1 ; i < 256 ; ++i)
        if ((fd_first_map[i] == -1) && ((i & fdb) != 0))
          fd_first_map[i] = fd ;
    } ;

  for (i = 1 ; i < 256 ; ++i)
    if (fd_first_map[i] == -1)
      zabort("Broken fd_first_map -- missing bits") ;

  /* (7) construct fd_byte_count[] -- number of bytes required to       */
  /*     include fds 0..fd.                                             */

  i = 0 ;
  for (fd = 0 ; fd < FD_SETSIZE ; ++fd)
    {
      int c = fd_byte_map[fd] + 1 ;

      if (c < i)
        c = i ;         /* use largest so far.  => big-endian   */
      else
        i = c ;         /* keep largest so far up to date       */

      fd_byte_count[fd] = c ;
    } ;

#if QPS_TESTING

  /* Checking that the maps have been correctly deduced          */

  for (fd = 0 ; fd < FD_SETSIZE ; ++fd)
    {
      uint8_t b ;
      short   c ;

      iw        = fd / QPS_TEST_WORD_BITS ;
      if (QPS_TEST_BE)
        ib      = ( ((fd / QPS_TEST_WORD_BITS) * QPS_TEST_WORD) +
                      (QPS_TEST_WORD - 1) - ((fd % QPS_TEST_WORD_BITS) / 8) ) ;
      else
        ib      = ( fd / 8 ) ;

      if (QPS_TEST_B_ORD == 07)
        b       = 0x01 << (fd % 8) ;
      else
        b       = 0x80 >> (fd % 8) ;

      if (QPS_TEST_BE)
        c = (iw + 1) * QPS_TEST_WORD ;
      else
        c = (ib + 1) ;

      if (fd_word_map[fd] != iw)
        zabort("Broken fd_word_map") ;
      if (fd_byte_map[fd] != ib)
        zabort("Broken fd_byte_map") ;
      if (fd_bit_map[fd]  != b)
        zabort("Broken fd_bit_map") ;
      if (fd_byte_count[fd] != c)
        zabort("Broken fd_byte_count") ;
    } ;

  for (i = 1 ; i < 256 ; ++i)
    {
      uint8_t b = i ;
      fd = 0 ;
      if (QPS_TEST_B_ORD == 07)
        {
          while ((b & 1) == 0)
            {
              b >>= 1 ;
              ++fd ;
            } ;
        }
      else
        {
          while ((b & 0x80) == 0)
            {
              b <<= 1 ;
              ++fd ;
            } ;
        } ;

      if (fd_first_map[i] != fd)
        zabort("Broken fd_first_map") ;
    } ;

  zabort("OK fd mapping") ;
#endif

  /* Phew -- we're all set now                                          */
  qps_super_set_map_made = 1 ;
} ;

/* Zeroise 'n' contiguous fd_super_sets
 *
 * NB: this MUST be used in place of FD_ZERO because the fd_set may be shorter
 *     than the overlayed words/bytes vectors.
 *
 * NB: it is CONFIRMED elsewhere that the fd_set is no longer than the overlays.
 */
static void
qps_super_set_zero(fd_super_set* p_set, int n)
{
  memset(p_set, 0, SIZE(fd_super_set, n)) ;
} ;

#if 0   /* Mask unused function */
/* Copy 'n' contiguous fd_super_sets
 */
static void
qps_super_set_copy(fd_super_set* p_dst, fd_super_set* p_src, int n)
{
  memcpy(p_dst, p_src, SIZE(fd_super_set, n)) ;
} ;
#endif

/* Compare 'n' contiguous fd_super_sets
 */
static int
qps_super_set_cmp(fd_super_set* p_a, fd_super_set* p_b, int n)
{
  return memcmp(p_a, p_b, SIZE(fd_super_set, n)) ;
} ;

/* Count the number of bits set in 'n' contiguous fd_super_sets.
 */
static int
qps_super_set_count(fd_super_set* p_set, int n)
{
  fd_word_t* p ;
  int count = 0 ;

  n *= FD_SUPER_SET_WORD_SIZE ;
  confirm(sizeof(fd_super_set) == (FD_SUPER_SET_WORD_SIZE * FD_WORD_BYTES)) ;

  p = (fd_word_t*)p_set ;
  while (n--)
    {
      fd_word_t w = *p++ ;
      while (w != 0)
        {
          ++count ;
          w &= (w - 1) ;
        } ;
    } ;

  return count ;
} ;

/*==============================================================================
 * Selection state check -- for debug purposes.
 *
 * Runs a check across a given selection and verifies that:
 *
 *   1) for !fd_direct that the files are in fd order in the vector
 *      and are unique, and there are no NULL entries.
 *   2) for  fd_direct that the file fd and the index match
 *      and the last entry is not NULL
 *   3) that all files point at the selection
 *   4) that the enabled modes in each file are valid
 *   5) the number of files in the selection matches fd_count.
 *   6) the highest numbered fd matches fd_last
 *   7) that the enabled counts in the selection are correct
 *   8) that the enabled modes in each file match the enabled modes in the
 *      selection
 *   9) that no extraneous fds are set in the enabled vectors
 *
 * If there are no pending fds:
 *
 *  10) if there are no pending fds, that the results vectors are empty.
 *
 * If there are pending fds:
 *
 *  11) that pend_mnum is valid and pend_fd <= tried_fd_last.
 *
 *  12) that the tried_count for modes 0..pend_mnum-1 is zero,
 *      and the tried_count for pend_mnum is not.
 *
 *  13) that the result vectors for modes where tried count == 0 are empty.
 *
 *  14) that the remaining result bits are a subset of the enabled bits.
 *
 *  15) that no bits beyond tried_fd_last are set in the result vectors.
 *
 *  16) that no bits before pend_fd are set in the pemd_mnum result vector.
 *
 *  17) that the number of bits remaining matches pend_count.
 */
static void
qps_selection_validate(qps_selection qps)
{
  int   fd_last ;
  int   enabled_count[qps_mnum_count] ;
  fd_full_set enabled ;

  qps_file     qf ;
  int          fd, n, mnum, p_mnum ;
  vector_index i ;

  /* 1..4)  Run down the selection vector and check.            */
  /*        Collect new enabled_count and enabled bit vectors.  */

  for (mnum = 0 ; mnum < qps_mnum_count ; ++mnum)
    enabled_count[mnum] = 0 ;
  qps_super_set_zero(enabled, qps_mnum_count) ;

  n = 0 ;
  fd_last = -1 ;
  for (VECTOR_ITEMS(&qps->files, qf, i))
    {
      if (qf != NULL)
        {
          ++n ;                         /* Number of files              */

          if (qps->fd_direct)
            {
              if (qf->fd != (int)i)     /* index and fd must match      */
                zabort("File vector index and fd mismatch") ;
            }
          else
            {
              if (qf->fd <= fd_last)    /* must be unique and in order  */
                zabort("File vector not in order") ;
            } ;

          fd_last = qf->fd ;            /* keep track of last fd        */

          if (qf->selection != qps)     /* file must refer to selection */
            zabort("File does not refer to its selection") ;

          if ((qf->enabled_bits < 0) || (qf->enabled_bits > qps_all_mbits))
            zabort("File enabled bits are invalid") ;

          /* Capture enabled state of all files.                        */
          for (mnum = 0 ; mnum < qps_mnum_count ; ++mnum)
            if (qf->enabled_bits & qps_mbit(mnum))
              {
                ++enabled_count[mnum] ;
                FD_SET(qf->fd, &enabled[mnum].fdset) ;
              } ;
        }
      else
        if (!qps->fd_direct)
          zabort("Found NULL entry in !fd_direct files vector") ;
    } ;

  if ((n != 0) && (vector_get_last_item(&qps->files) == NULL))
    zabort("Last entry in file vector is NULL") ;

  /* 5) check that the number of files tallies.                         */
  if (n != qps->fd_count)
    zabort("Number of files in the selection does not tally") ;

  /* 6) check the last fd                                               */
  if ( ((n == 0) && (qps->fd_last !=0)) || (fd_last != qps->fd_last) )
    zabort("The last fd does not tally") ;

  /* 7) check that the enabled counts tally.                            */
  for (mnum = 0 ; mnum < qps_mnum_count ; ++mnum)
    if (enabled_count[mnum] != qps->enabled_count[mnum])
      zabort("Enabled counts do not tally") ;

  /* 8..9) Check that the enabled vectors are the same as the ones just */
  /*       created by scanning the files.                               */
  if (qps_super_set_cmp(enabled, qps->enabled, qps_mnum_count) != 0)
    zabort("Enabled bit vectors do not tally") ;

  /* 10) if there are no pending fds, check result vectors empty.       */
  if (qps->pend_count == 0)
    {
      if (qps_super_set_count(qps->results, qps_mnum_count) != 0)
        zabort("Nothing pending, but result vectors not empty") ;

      return ;
    } ;

  /* This is to stop gcc whining about signed/unsigned comparisons.     */
  p_mnum = qps->pend_mnum ;

  /* 11) that pend_mnum is valid and pend_fd <= tried_fd_last.          */
  if ( (p_mnum < 0) || (p_mnum > qps_mnum_count)
                    || (qps->pend_fd < 0)
                    || (qps->pend_fd > qps->tried_fd_last) )
    zabort("Invalid pend_mnum or pend_fd") ;

  /* 12) check tried_count[]                                            */
  for (mnum = 0 ; mnum < qps_mnum_count ; ++mnum)
    {
      if ((mnum <  p_mnum) && (qps->tried_count[mnum] != 0))
        zabort("Non-zero tried_count for mode < pend_mnum") ;
      if ((mnum == p_mnum) && (qps->tried_count[qps->pend_mnum] <= 0))
        zabort("Zero tried_count for pend_mnum") ;
      if ((mnum >  p_mnum) && (qps->tried_count[mnum] < 0))
        zabort("Invalid tried_count for mode > pend_mnum") ;
    } ;

  /* 13) check result vectors for modes where tried count == 0          */
  n = (qps_super_set_count(qps->results, qps_mnum_count) != 0) ;
  for (mnum = 0 ; mnum < qps_mnum_count ; ++mnum)
    if ((qps->tried_count[mnum] == 0)
                          && (qps_super_set_count(&qps->results[mnum], 1) != 0))
      zabort("Non-empty bit vector where tried count == 0") ;

  /* 14) check remaining results are a subset of the enableds.          */
  /* 15) check no bit beyond tried_fd_last is set in the results.       */
  /* 16) check no bit before pend_fd is set in the pemd_mnum results.   */
  /* 17) check the number of bits remaining matches pend_count.         */

  n = 0 ;
  for (mnum = 0 ; mnum < qps_mnum_count ; ++mnum)
    if (qps->tried_count[mnum] != 0)
      {
        for (fd = 0 ; fd < FD_SETSIZE ; ++fd)
          if (FD_ISSET(fd, &qps->results[mnum].fdset))
            {
              ++n ;
              if (fd > qps->tried_fd_last)
                zabort("Found pending fd beyond tried_fd_last") ;
              if ( ! FD_ISSET(fd, &qps->results[mnum].fdset))
                zabort("Found pending fd which is not enabled") ;
              if ((mnum == p_mnum) && (fd < qps->pend_fd))
                zabort("Found pending fd < current next pending") ;
            } ;
      } ;

  if (n != qps->pend_count)
    zabort("Non-empty bit vector where tried count == 0") ;
} ;