INCLUDES =  -I$(srcdir) -I$(srcdir)/.. -I$(srcdir)/../JudyCommon/ 
AM_CFLAGS = -DJUDYL @WARN_CFLAGS@ 

noinst_LTLIBRARIES = libJudyL.la libnext.la libprev.la libcount.la libinline.la

libJudyL_la_SOURCES = JudyLCascade.c JudyLTables.c JudyLCount.c JudyLCreateBranch.c JudyLDecascade.c JudyLDel.c JudyLFirst.c JudyLFreeArray.c JudyLGet.c JudyLInsArray.c JudyLIns.c JudyLInsertBranch.c JudyLMallocIF.c JudyLMemActive.c JudyLMemUsed.c 

libnext_la_SOURCES = JudyLNext.c JudyLNextEmpty.c
libnext_la_CFLAGS = $(AM_CFLAGS) -DJUDYNEXT

libprev_la_SOURCES = JudyLPrev.c JudyLPrevEmpty.c
libprev_la_CFLAGS = $(AM_CFLAGS) -DJUDYPREV

libcount_la_SOURCES = JudyLByCount.c
libcount_la_CFLAGS = $(AM_CFLAGS) -DNOSMARTJBB -DNOSMARTJBU -DNOSMARTJLB

libinline_la_SOURCES = j__udyLGet.c
libinline_la_CFLAGS = $(AM_CFLAGS) -DJUDYGETINLINE

JudyLTables.c: JudyLTablesGen.c
	$(CC) $(INCLUDES) $(AM_CFLAGS) @CFLAGS@ -o JudyLTablesGen JudyLTablesGen.c; ./JudyLTablesGen 

JudyLByCount.c:   ../JudyCommon/JudyByCount.c
	cp -f $< $@

JudyLCascade.c:../JudyCommon/JudyCascade.c
	cp -f $< $@

JudyLCount.c:../JudyCommon/JudyCount.c
	cp -f $< $@

JudyLCreateBranch.c:../JudyCommon/JudyCreateBranch.c 
	cp -f $< $@

JudyLDecascade.c:../JudyCommon/JudyDecascade.c    
	cp -f $< $@

JudyLDel.c:../JudyCommon/JudyDel.c       
	cp -f $< $@

JudyLFirst.c:../JudyCommon/JudyFirst.c
	cp -f $< $@

JudyLFreeArray.c:../JudyCommon/JudyFreeArray.c    
	cp -f $< $@

JudyLGet.c:../JudyCommon/JudyGet.c       
	cp -f $< $@

j__udyLGet.c:../JudyCommon/JudyGet.c 
	cp -f $< $@

JudyLInsArray.c:../JudyCommon/JudyInsArray.c     
	cp -f $< $@

JudyLIns.c:../JudyCommon/JudyIns.c      
	cp -f $< $@

JudyLInsertBranch.c:../JudyCommon/JudyInsertBranch.c
	cp -f $< $@

JudyLMallocIF.c:../JudyCommon/JudyMallocIF.c     
	cp -f $< $@

JudyLMemActive.c:../JudyCommon/JudyMemActive.c
	cp -f $< $@

JudyLMemUsed.c:../JudyCommon/JudyMemUsed.c   
	cp -f $< $@

JudyLNext.c:../JudyCommon/JudyPrevNext.c
	cp -f $< $@

JudyLPrev.c:../JudyCommon/JudyPrevNext.c
	cp -f $< $@

JudyLNextEmpty.c:../JudyCommon/JudyPrevNextEmpty.c
	cp -f $< $@

JudyLPrevEmpty.c:../JudyCommon/JudyPrevNextEmpty.c
	cp -f $< $@
	
JudyLTablesGen.c:../JudyCommon/JudyTables.c
	cp -f $< $@
