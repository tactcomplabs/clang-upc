/*===-- upc_access.c - UPC Runtime Support Library -----------------------===
|*
|*                     The LLVM Compiler Infrastructure
|*
|* Copyright 2012-2014, Intrepid Technology, Inc.  All rights reserved.
|* This file is distributed under a BSD-style Open Source License.
|* See LICENSE-INTREPID.TXT for details.
|*
|*===---------------------------------------------------------------------===*/

#include "upc_config.h"
#include "upc_sysdep.h"
#include "upc_defs.h"
#include "upc_access.h"
#include "upc_sync.h"
#include "upc_sup.h"
#include "upc_mem.h"

//begin lib_inline_access

/* IR Remote Pointer access routines.  */

/* To speed things up, the last two unique (page, thread)
   lookups are cached.  Caller must validate the pointer
   'p' (check for NULL, etc.) before calling this routine. */
__attribute__((__always_inline__))
inline
void *
__upc_rptr_to_addr (int thread, size_t vaddr)
{
  extern GUPCR_THREAD_LOCAL unsigned long __upc_page1_ref, __upc_page2_ref;
  extern GUPCR_THREAD_LOCAL void *__upc_page1_base, *__upc_page2_base;
  void *addr;
  size_t p_offset;
  upc_page_num_t pn;
  unsigned long this_page;
  p_offset = vaddr & GUPCR_VM_OFFSET_MASK;
  pn = (vaddr >> GUPCR_VM_OFFSET_BITS) & GUPCR_VM_PAGE_MASK;
  this_page = (pn << GUPCR_THREAD_SIZE) | thread;
  if (this_page == __upc_page1_ref)
    addr = (char *) __upc_page1_base + p_offset;
  else if (this_page == __upc_page2_ref)
    addr = (char *) __upc_page2_base + p_offset;
  else
    addr = __upc_vm_map_remote_offset (thread, vaddr);
  return addr;
}

__attribute__((__always_inline__))
inline
void
upcr_llvm_remote_get (long sthread, long saddr, void *dest, size_t n)
{
  char *srcp = (char *)__upc_rptr_to_addr (sthread, saddr);
  switch (n)
    {
      case 1:
        *(u_intQI_t *) dest = *(u_intQI_t *) srcp;
	break;
      case 2:
        *(u_intHI_t *) dest = *(u_intHI_t *) srcp;
	break;
      case 4:
        *(u_intSI_t *) dest = *(u_intSI_t *) srcp;
	break;
      case 8:
        *(u_intDI_t *) dest = *(u_intDI_t *) srcp;
	break;
      case 16:
#if GUPCR_TARGET64
        *(u_intTI_t *) dest = *(u_intTI_t *) srcp;
	break;
#endif
      default:
	for (;;)
	  {
	    size_t p_offset = (saddr & GUPCR_VM_OFFSET_MASK);
	    size_t n_copy = GUPCR_MIN (GUPCR_VM_PAGE_SIZE - p_offset, n);
	    memcpy (dest, srcp, n_copy);
	    n -= n_copy;
	    if (!n)
	      break;
	    saddr += n_copy;
	    dest = (char *) dest + n_copy;
	  }
	break;
    }
}

__attribute__((__always_inline__))
inline
void
upcr_llvm_remote_put (const void *src, long dthread, long daddr, size_t n)
{
  char *destp = (char *)__upc_rptr_to_addr (dthread, daddr);
  switch (n)
    {
      case 1:
        *(u_intQI_t *) destp = *(u_intQI_t *) src;
	break;
      case 2:
        *(u_intHI_t *) destp = *(u_intHI_t *) src;
	break;
      case 4:
        *(u_intSI_t *) destp = *(u_intSI_t *) src;
	break;
      case 8:
        *(u_intDI_t *) destp = *(u_intDI_t *) src;
	break;
      case 16:
#if GUPCR_TARGET64
        *(u_intTI_t *) destp = *(u_intTI_t *) src;
	break;
#endif
      default:
	for (;;)
	  {
	    size_t p_offset = (daddr & GUPCR_VM_OFFSET_MASK);
	    size_t n_copy = GUPCR_MIN (GUPCR_VM_PAGE_SIZE - p_offset, n);
	    memcpy (destp, src, n_copy);
	    n -= n_copy;
	    if (!n)
	      break;
	    daddr += n_copy;
	    src = (char *) src + n_copy;
	  }
	break;
    }
}

__attribute__((__always_inline__))
inline
void
upcr_llvm_getn (long sthread, long saddr, void *dest, size_t n)
{
  if (!dest)
    __upc_fatal ("Invalid access via null local pointer");
  if (!saddr)
    __upc_fatal ("Invalid access via null remote pointer");
  upcr_llvm_remote_get (sthread, saddr, dest, n);
}

__attribute__((__always_inline__))
inline
void
upcr_llvm_getns (long sthread, long saddr, void *dest, size_t n)
{
  if (!dest)
    __upc_fatal ("Invalid access via null local pointer");
  if (!saddr)
    __upc_fatal ("Invalid access via null remote pointer");
  GUPCR_FENCE ();
  upcr_llvm_remote_get (sthread, saddr, dest, n);
  GUPCR_READ_FENCE ();
}

__attribute__((__always_inline__))
inline
void
upcr_llvm_putn (const void *src, long dthread, long daddr, size_t n)
{
  if (!src)
    __upc_fatal ("Invalid access via null local pointer");
  if (!daddr)
    __upc_fatal ("Invalid access via null remote pointer");
  upcr_llvm_remote_put (src, dthread, daddr, n);
}

__attribute__((__always_inline__))
inline
void
upcr_llvm_putns (const void *src, long dthread, long daddr, size_t n)
{
  if (!src)
    __upc_fatal ("Invalid access via null local pointer");
  if (!daddr)
    __upc_fatal ("Invalid access via null remote pointer");
  GUPCR_WRITE_FENCE ();
  upcr_llvm_remote_put (src, dthread, daddr, n);
  GUPCR_FENCE ();
}

/* Clang UPC access routines.  */

__attribute__((__always_inline__))
static inline
void *
__upc_access_sptr_to_addr (upc_shared_ptr_t p)
{
  if (GUPCR_PTS_IS_NULL (p))
    __upc_fatal ("Invalid access via null shared pointer");
  if ((int)GUPCR_PTS_THREAD(p) >= THREADS)
    __upc_fatal ("Thread number in shared address is out of range");
  return __upc_sptr_to_addr (p);
}

//inline
u_intQI_t
__getqi2 (upc_shared_ptr_t p)
{
  const u_intQI_t *addr = (u_intQI_t *) __upc_access_sptr_to_addr (p);
  return *addr;
}

//inline
u_intHI_t
__gethi2 (upc_shared_ptr_t p)
{
  const u_intHI_t *addr = (u_intHI_t *) __upc_access_sptr_to_addr (p);
  return *addr;
}

//inline
u_intSI_t
__getsi2 (upc_shared_ptr_t p)
{
  const u_intSI_t *addr = (u_intSI_t *) __upc_access_sptr_to_addr (p);
  return *addr;
}

//inline
u_intDI_t
__getdi2 (upc_shared_ptr_t p)
{
  const u_intDI_t *addr = (u_intDI_t *) __upc_access_sptr_to_addr (p);
  return *addr;
}

#if GUPCR_TARGET64
//inline
u_intTI_t
__getti2 (upc_shared_ptr_t p)
{
  const u_intTI_t *addr = (u_intTI_t *) __upc_access_sptr_to_addr (p);
  return *addr;
}
#endif /* GUPCR_TARGET64 */

//inline
float
__getsf2 (upc_shared_ptr_t p)
{
  const float *addr = (float *) __upc_access_sptr_to_addr (p);
  return *addr;
}

//inline
double
__getdf2 (upc_shared_ptr_t p)
{
  const double *addr = (double *) __upc_access_sptr_to_addr (p);
  return *addr;
}

//inline
long double
__gettf2 (upc_shared_ptr_t p)
{
  const long double *addr = (long double *) __upc_access_sptr_to_addr (p);
  return *addr;
}

//inline
long double
__getxf2 (upc_shared_ptr_t p)
{
  const long double *addr = (long double *) __upc_access_sptr_to_addr (p);
  return *addr;
}

//inline
void
__getblk3 (void *dest, upc_shared_ptr_t src, size_t n)
{
  __upc_memget (dest, src, n);
}

//inline
void
__putqi2 (upc_shared_ptr_t p, u_intQI_t v)
{
  u_intQI_t * const addr = (u_intQI_t *) __upc_access_sptr_to_addr (p);
  *addr = v;
}

//inline
void
__puthi2 (upc_shared_ptr_t p, u_intHI_t v)
{
  u_intHI_t * const addr = (u_intHI_t *) __upc_access_sptr_to_addr (p);
  *addr = v;
}

//inline
void
__putsi2 (upc_shared_ptr_t p, u_intSI_t v)
{
  u_intSI_t * const addr = (u_intSI_t *) __upc_access_sptr_to_addr (p);
  *addr = v;
}

//inline
void
__putdi2 (upc_shared_ptr_t p, u_intDI_t v)
{
  u_intDI_t * const addr = (u_intDI_t *) __upc_access_sptr_to_addr (p);
  *addr = v;
}

#if GUPCR_TARGET64
//inline
void
__putti2 (upc_shared_ptr_t p, u_intTI_t v)
{
  u_intTI_t * const addr = (u_intTI_t *) __upc_access_sptr_to_addr (p);
  *addr = v;
}
#endif /* GUPCR_TARGET64 */

//inline
void
__putsf2 (upc_shared_ptr_t p, float v)
{
  float * const addr = (float *) __upc_access_sptr_to_addr (p);
  *addr = v;
}

//inline
void
__putdf2 (upc_shared_ptr_t p, double v)
{
  double * const addr = (double *) __upc_access_sptr_to_addr (p);
  *addr = v;
}

//inline
void
__puttf2 (upc_shared_ptr_t p, long double v)
{
  long double * const addr = (long double *) __upc_access_sptr_to_addr (p);
  *addr = v;
}

//inline
void
__putxf2 (upc_shared_ptr_t p, long double v)
{
  long double * const addr = (long double *) __upc_access_sptr_to_addr (p);
  *addr = v;
}

//inline
void
__putblk3 (upc_shared_ptr_t dest, void *src, size_t n)
{
  __upc_memput (dest, src, n);
}

//inline
void
__copyblk3 (upc_shared_ptr_t dest, upc_shared_ptr_t src, size_t n)
{
  __upc_memcpy (dest, src, n);
}

/* Strict memory accesses. */

//inline
u_intQI_t
__getsqi2 (upc_shared_ptr_t p)
{
  const u_intQI_t *addr = (u_intQI_t *) __upc_access_sptr_to_addr (p);
  u_intQI_t result;
  GUPCR_FENCE ();
  result = *addr;
  GUPCR_READ_FENCE ();
  return result;
}

//inline
u_intHI_t
__getshi2 (upc_shared_ptr_t p)
{
  const u_intHI_t *addr = (u_intHI_t *) __upc_access_sptr_to_addr (p);
  u_intHI_t result;
  GUPCR_FENCE ();
  result = *addr;
  GUPCR_READ_FENCE ();
  return result;
}

//inline
u_intSI_t
__getssi2 (upc_shared_ptr_t p)
{
  const u_intSI_t *addr = (u_intSI_t *) __upc_access_sptr_to_addr (p);
  u_intSI_t result;
  GUPCR_FENCE ();
  result = *addr;
  GUPCR_READ_FENCE ();
  return result;
}

//inline
u_intDI_t
__getsdi2 (upc_shared_ptr_t p)
{
  const u_intDI_t *addr = (u_intDI_t *) __upc_access_sptr_to_addr (p);
  u_intDI_t result;
  GUPCR_FENCE ();
  result = *addr;
  GUPCR_READ_FENCE ();
  return result;
}

#if GUPCR_TARGET64
//inline
u_intTI_t
__getsti2 (upc_shared_ptr_t p)
{
  const u_intTI_t *addr = (u_intTI_t *) __upc_access_sptr_to_addr (p);
  u_intTI_t result;
  GUPCR_FENCE ();
  result = *addr;
  GUPCR_READ_FENCE ();
  return result;
}
#endif /* GUPCR_TARGET64 */

//inline
float
__getssf2 (upc_shared_ptr_t p)
{
  const float *addr = (float *) __upc_access_sptr_to_addr (p);
  float result;
  GUPCR_FENCE ();
  result = *addr;
  GUPCR_READ_FENCE ();
  return result;
}

//inline
double
__getsdf2 (upc_shared_ptr_t p)
{
  const double *addr = (double *) __upc_access_sptr_to_addr (p);
  double result;
  GUPCR_FENCE ();
  result = *addr;
  GUPCR_READ_FENCE ();
  return result;
}

//inline
long double
__getstf2 (upc_shared_ptr_t p)
{
  const long double *addr = (long double *) __upc_access_sptr_to_addr (p);
  long double result;
  GUPCR_FENCE ();
  result = *addr;
  GUPCR_READ_FENCE ();
  return result;
}

//inline
long double
__getsxf2 (upc_shared_ptr_t p)
{
  const long double *addr = (long double *) __upc_access_sptr_to_addr (p);
  long double result;
  GUPCR_FENCE ();
  result = *addr;
  GUPCR_READ_FENCE ();
  return result;
}

//inline
void
__getsblk3 (void *dest, upc_shared_ptr_t src, size_t len)
{
  GUPCR_FENCE ();
  __getblk3 (dest, src, len);
  GUPCR_READ_FENCE ();
}

//inline
void
__putsqi2 (upc_shared_ptr_t p, u_intQI_t v)
{
  u_intQI_t *addr = (u_intQI_t *) __upc_access_sptr_to_addr (p);
  GUPCR_WRITE_FENCE ();
  *addr = v;
  GUPCR_FENCE ();
}

//inline
void
__putshi2 (upc_shared_ptr_t p, u_intHI_t v)
{
  u_intHI_t *addr = (u_intHI_t *) __upc_access_sptr_to_addr (p);
  GUPCR_WRITE_FENCE ();
  *addr = v;
  GUPCR_FENCE ();
}

//inline
void
__putssi2 (upc_shared_ptr_t p, u_intSI_t v)
{
  u_intSI_t *addr = (u_intSI_t *) __upc_access_sptr_to_addr (p);
  GUPCR_WRITE_FENCE ();
  *addr = v;
  GUPCR_FENCE ();
}

//inline
void
__putsdi2 (upc_shared_ptr_t p, u_intDI_t v)
{
  u_intDI_t *addr = (u_intDI_t *) __upc_access_sptr_to_addr (p);
  GUPCR_WRITE_FENCE ();
  *addr = v;
  GUPCR_FENCE ();
}

#if GUPCR_TARGET64
//inline
void
__putsti2 (upc_shared_ptr_t p, u_intTI_t v)
{
  u_intTI_t *addr = (u_intTI_t *) __upc_access_sptr_to_addr (p);
  GUPCR_WRITE_FENCE ();
  *addr = v;
  GUPCR_FENCE ();
}
#endif /* GUPCR_TARGET64 */

//inline
void
__putssf2 (upc_shared_ptr_t p, float v)
{
  float *addr = (float *) __upc_access_sptr_to_addr (p);
  GUPCR_WRITE_FENCE ();
  *addr = v;
  GUPCR_FENCE ();
}

//inline
void
__putsdf2 (upc_shared_ptr_t p, double v)
{
  double *addr = (double *) __upc_access_sptr_to_addr (p);
  GUPCR_WRITE_FENCE ();
  *addr = v;
  GUPCR_FENCE ();
}

//inline
void
__putstf2 (upc_shared_ptr_t p, long double v)
{
  long double *addr = (long double *) __upc_access_sptr_to_addr (p);
  GUPCR_WRITE_FENCE ();
  *addr = v;
  GUPCR_FENCE ();
}

//inline
void
__putsxf2 (upc_shared_ptr_t p, long double v)
{
  long double *addr = (long double *) __upc_access_sptr_to_addr (p);
  GUPCR_WRITE_FENCE ();
  *addr = v;
  GUPCR_FENCE ();
}

//inline
void
__putsblk3 (upc_shared_ptr_t dest, void *src, size_t len)
{
  GUPCR_WRITE_FENCE ();
  __putblk3 (dest, src, len);
  GUPCR_FENCE ();
}

//inline
void
__copysblk3 (upc_shared_ptr_t dest, upc_shared_ptr_t src, size_t len)
{
  GUPCR_WRITE_FENCE ();
  __copyblk3 (dest, src, len);
  GUPCR_FENCE ();
}

//inline
void
__upc_fence (void)
{
  GUPCR_FENCE ();
}
//end lib_inline_access
