/**************** Block H Declares Source Code File (.H) ***************/
/*  Name: BLOCK.H     Version 2.1                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          1998 - 2020  */
/*                                                                     */
/*  This file contains the BLOCK pure virtual class definition.        */
/*---------------------------------------------------------------------*/
/*  Note: one of the main purpose of this base class is to take care   */
/*  of the very specific way Connect handles memory allocation.        */
/*  Instead of allocating small chunks of storage via new or malloc    */
/*  Connect works in its private memory pool in which it does the sub- */
/*  allocation using the function PlugSubAlloc. These are never freed  */
/*  separately but when a transaction is terminated, the entire pool   */
/*  is set to empty, resulting in a very fast and efficient allocate   */
/*  process, no garbage collection problem, and an automatic recovery  */
/*  procedure (via throw) when the memory is exhausted.                */
/*  For this to work new must be given two parameters, first the       */
/*  global pointer of the Plug application, and an optional pointer to */
/*  the memory pool to use, defaulting to NULL meaning using the Plug  */
/*  standard default memory pool, example:                             */
/*    tabp = new(g) XTAB("EMPLOYEE");                                  */
/*  allocates a XTAB class object in the standard Plug memory pool.    */
/***********************************************************************/
#if !defined(BLOCK_DEFINED)
#define      BLOCK_DEFINED

#if defined(_WIN32) && !defined(NOEX)
#define DllExport  __declspec( dllexport )
#else   // !_WIN32
#define DllExport
#endif  // !_WIN32

/***********************************************************************/
/*  Definition of class BLOCK with its method function new.            */
/***********************************************************************/
typedef class BLOCK *PBLOCK;

class DllExport BLOCK {
 public:
  void *operator new(size_t size, PGLOBAL g, void *mp = NULL) {
	  xtrc(256, "New BLOCK: size=%d g=%p p=%p\n", size, g, mp);
		return PlugSubAlloc(g, mp, size);
  } // end of new

	void* operator new(size_t size, long long mp) {
		xtrc(256, "Realloc at: mp=%lld\n", mp);
		return (void*)mp;
	} // end of new

	virtual void Printf(PGLOBAL, FILE *, uint) {}   // Produce file desc
  virtual void Prints(PGLOBAL, char *, uint) {}   // Produce string desc
  
  // Avoid gcc errors by defining matching dummy delete operators
  void operator delete(void*, PGLOBAL, void *) {}
	void operator delete(void*, long long) {}
	void operator delete(void*) {}

  virtual ~BLOCK() {}
}; // end of class BLOCK

#endif   // !BLOCK_DEFINED
