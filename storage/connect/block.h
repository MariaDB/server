/**************** Block H Declares Source Code File (.H) ***************/
/*  Name: BLOCK.H     Version 2.0                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          1998         */
/*                                                                     */
/*  This file contains the BLOCK pure virtual class definition.        */
/*---------------------------------------------------------------------*/
/*  Note: one of the main purpose of this base class is to take care   */
/*  of the very specific way Plug handles memory allocation.           */
/*  Instead of allocating small chunks of storage via new or malloc    */
/*  Plug works in its private memory pool in which it does the sub-    */
/*  allocation using the function PlugSubAlloc. These are never freed  */
/*  separately but when a transaction is terminated, the entire pool   */
/*  is set to empty, resulting in a very fast and efficient allocate   */
/*  process, no garbage collection problem, and an automatic recovery  */
/*  procedure (via LongJump) when the memory is exhausted.             */
/*  For this to work new must be given two parameters, first the       */
/*  global pointer of the Plug application, and an optional pointer to */
/*  the memory pool to use, defaulting to NULL meaning using the Plug  */
/*  standard default memory pool, example:                             */
/*    tabp = new(g) XTAB("EMPLOYEE");                                 */
/*  allocates a XTAB class object in the standard Plug memory pool.   */
/***********************************************************************/
#if !defined(BLOCK_DEFINED)
#define      BLOCK_DEFINED

#if defined(WIN32) && !defined(NOEX)
#define DllExport  __declspec( dllexport )
#else   // !WIN32
#define DllExport
#endif  // !WIN32

/***********************************************************************/
/*  Definition of class BLOCK with its method function new.            */
/***********************************************************************/
typedef class BLOCK *PBLOCK;

class DllExport BLOCK {
 public:
  void * operator new(size_t size, PGLOBAL g, void *p = NULL) {
//  if (trace > 2)
//    htrc("New BLOCK: size=%d g=%p p=%p\n", size, g, p);

    return (PlugSubAlloc(g, p, size));
    } // end of new

  virtual void Print(PGLOBAL, FILE *, uint) {}   // Produce file desc
  virtual void Print(PGLOBAL, char *, uint) {}   // Produce string desc
  
#if !defined(__BORLANDC__)
  // Avoid warning C4291 by defining a matching dummy delete operator
  void operator delete(void *, PGLOBAL, void *) {}
  void operator delete(void *ptr,size_t size) {}
#endif
  virtual ~BLOCK() {}

  }; // end of class BLOCK

#endif   // !BLOCK_DEFINED
