/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Remap.
 *
 * The Initial Developer of the Original Code is
 * Richard L. Walsh
 * Portions created by the Initial Developer are Copyright (C) 2010
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 *
 * ***** END LICENSE BLOCK ***** */
/*****************************************************************************/
/*  remap.c - v1.02
 *
 *  Remap reformats & demangles IBM-style map files to make them more usable.
 *  It consolidates all of the sections of a map file (segments, modules,
 *  exports, and publics) into a single listing by address.  It also produces
 *  a listing of publics sorted by name that ignores leading underscores.
 *
 *  Optionally, it can also demangle only, producing a listing formatted
 *  like the original but resorted by the demangled names.
 *
 *  Remap also fixes a bug in ilink's handling of very large files.  When
 *  there are more than roughly 54,000 symbols, some may appear in Publics
 *  by Name but not in Publics by Value, and vice-versa.  Remap reads both
 *  sections to create a listing of all available symbols.  In extreme
 *  situations, Remap's demangle-only listing may be larger than the original
 *  map file.
 *
 *  Note:  the demangler for gcc is statically linked to the exe while the
 *  vacpp demangler is contained in demangl.dll.  Unfortunately, the dll's
 *  functions have Optlink linkage which gcc 4.xx can't handle.  As a
 *  workaround, Remap uses wrapper functions contained in a separate file,
 *  remap_vac.c.  The author compiles it using VACPP 3.65 but it could
 *  probably be compiled with gcc 3.3.5.  Alternately, someone who
 *  understands gcc's assembler semantics could replace it with some
 *  inline assembly.
 *
 */
/*****************************************************************************/

#define USE_OS2_TOOLKIT_HEADERS

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <sys\types.h>
#include <sys\stat.h>

#define INCL_DOS
#include <os2.h>

#include "remap_demangle.h"

#define INCL_LOADEXCEPTQ
#include "exceptq.h"

/*****************************************************************************/

#define HFILE_NONE      ((HFILE)-1)
#define NULLCHAR        ((char)0)

#define OPT_NO_DEMANGLE     0x01
#define OPT_DEMANGLE_ONLY   0x02
#define OPT_SHOW_ARGS       0x04
#define OPT_WS              0x08
#define OPT_WARNINGS        0x10
#define OPT_GCC             0x20
#define OPT_VAC             0x40
#define OPT_XXC             0x80

#define REMAP_END       0
#define REMAP_GRP       0x0001
#define REMAP_IMP       0x0002
#define REMAP_SEG       0x0004
#define REMAP_MOD       0x0008
#define REMAP_EPT       0x0010
#define REMAP_EXP       0x0020
#define REMAP_OBJ       0x0040
#define REMAP_ERR       0x0080
#define REMAP_TYPE      0x00FF

#define REMAP_ABS       0x0100

#define REMAP_VTABLE    0x01000
#define REMAP_THUNK     0x02000
#define REMAP_TYPEINFO  0x04000
#define REMAP_TYPENAME  0x08000
#define REMAP_GUARD     0x10000
#define REMAP_ATTRMASK  0x1F000

#define REMAP_DUP       0x40000000
#define REMAP_DUP2      0x80000000

#define REMAP_MASK      0x1F1FF

typedef struct _remap {
    struct _remap*  next;
    ULONG   type;
    ULONG   seg;
    ULONG   offs;
    char    text[1];
} REMAP;

/*****************************************************************************/

int     ParseArgs(int argc, char* argv[]);
int     Init(void);
int     LoadVacDemangler(void);
int     StartDemangler(void);
int     PrintUntil(char ** pArray);
char ** SeekToHdr(char ** pSeek, char ** pStop);
int     MatchArray(char ** pArray, char * pText);
int     StoreSegments(char ** pStop);
int     ParseSegment(char * pData, ULONG * pSeg, ULONG * pOffs);
int     ParseModule(char * pData, ULONG ulSeg, ULONG ulOffs);
int     StoreGroups(void);
int     StoreExports(void);
int     StorePublics(void);
int     StoreEntryPoint(void);
int     StoreError(char * pBuf, REMAP * r);
char *  Trim(char * pTrim, char** ppNext);
char *  TrimLine(char * pTrim);
char *  Demangle(char * pIn, char * pOut, ULONG cbOut, ULONG * pFlags);

int     MarkDuplicates(void);
int     PrintEntriesByAddress(void);
int     PrintPublicsByName(void);
int     DuplicateSorter(const void *key, const void *element);
int     AddressSorter(const void *key, const void *element);
int     NameSorter(const void *key, const void *element);
int     ImportSorter(char* pk, char* pe);
void    PrintByAddress(REMAP** pr);
void    PrintByName(REMAP** pr);
char *  DecodeFlags(ULONG flags, char* pszFlags);

int     Copy(void);
int     CopyExports(void);
REMAP** SetupPublicsSort(void);
int     PrintPublics(REMAP** pr);
char *  DecodeFlagName(ULONG flags);

/*****************************************************************************/

/** resources that have to be deallocated **/
FILE *  fi = 0;
FILE *  fo = 0;
FILE *  pi = 0;
FILE *  po = 0;
char *  buffer = 0;
ULONG   ulFiltPID = 0;

/** other globals **/
int     opts = 0;
char *  pszDemangler = 0;

char *  pCur = 0;
int     recCnt = 0;

char    fIn[CCHMAXPATH] = "";
char    fOut[CCHMAXPATH] = "";

char    bufIn[1024];
char    buf1[1024];

/* these pointers are declared in remap_vac.c */
extern PFNDEMANGLE  pfnDemangle;
extern PFNKIND      pfnKind;
extern PFNTEXT     	pfnText;
extern PFNTEXT      pfnQualifier;
extern PFNTEXT      pfnFunctionName;
extern PFNERASE     pfnErase;

/*****************************************************************************/

/** Constants **/

char *  pszWS = " \t\r\n";
char *  pszTrouble = "$w$";
char *  pszWarningL = ": warning L";
char *  pszEntryPoint = "Entry Point";
char *  pszPubByVal = "Address         Publics by Value";

char    szModule[] = "at offset ";
int     cbModule = sizeof(szModule) - 1;
char    szBytesFrom[] = "H bytes from ";
int     cbBytesFrom = sizeof(szBytesFrom) - 1;
char    szImp[] = "Imp ";
int     cbImp = sizeof(szImp) - 1;
char    szAbs[] = "Abs ";
int     cbAbs = sizeof(szAbs) - 1;
char    szPgmEP[] = "Program entry point at ";
int     cbPgmEP = sizeof(szPgmEP) - 1;

char    szVtable[] = "vtable for ";
int     cbVtable = sizeof(szVtable) - 1;
char    szThunk[] = "non-virtual thunk to ";
int     cbThunk = sizeof(szThunk) - 1;
char    szTypeInfo[] = "typeinfo for ";
int     cbTypeInfo = sizeof(szTypeInfo) - 1;
char    szTypeName[] = "typeinfo name for ";
int     cbTypeName = sizeof(szTypeName) - 1;
char    szGuard[] = "guard variable for ";
int     cbGuard = sizeof(szGuard) - 1;

char    szVtableVAC[] = "::virtual-fn-table-ptr";
int     cbVtableVAC = sizeof(szVtableVAC) - 1;

char *  apszModules[] = {"Start", "Length", "Name", "Class", ""};
char *  apszGroups[] = {"Origin", "Group", ""};
char *  apszExports[] = {"Address", "Export", "Alias", ""};
char *  apszPubByName[] = {"Address", "Publics by Name", ""};
char *  apszPubByValue[] = {"Address", "Publics by Value", ""};

char *  pszSrcExt = ".map";
char *  pszRemapExt = ".remap";
char *  pszDemapExt = ".demap";

/*****************************************************************************/

char *  pszAddressHdr =
      "\n ------------------------\n"
        " *  Symbols by Address  *\n"
        " ------------------------\n\n";

char *  pszNameHdr =
      "\n ---------------------\n"
        " *  Symbols by Name  *\n"
        " ---------------------\n\n";

char *  pszColumnHdr =
        "    Seg:Offset    Flags  Name                      External Name\n"
        "   -------------  -----  ------------------------  ------------------------\n";

char *  pszLegend =
      "\n Flags:  G = group    S = segment   M = module   E = entry point\n"
        "         i = import   x = export    a = absolute address\n";

char *  pszLegendGCC =
        "         v = vtable   t = typeinfo  n = typeinfo name\n"
        "         k = non-virtual thunk      g = guard variable\n\n";

char *  pszLegendVAC =
        "         v = vtable\n\n";

char *  pszLegendXXC = "\n\n";

/*****************************************************************************/

char *  pszHelp =
      "\n remap v1.02 - (C)2010  R L Walsh\n"
        " Reformats and demangles IBM-style .map files.\n\n"
        " Usage:  remap [-options] [optional_files] mapfile[.map]\n"
        " General options:\n"
        "   -a  show demangled method arguments\n"
        "   -d  demangle only, don't reformat\n"
        "   -n  don't demangle symbols\n"
        "   -m  include linker warning messages (errors are always displayed)\n"
        "   -o  specify output file             (default: *.remap or *.demap)\n"
        "   -w  preserve whitespace in symbols  (default: replace with undersores)\n"
        " Demangler options:\n"
        "   -g  use builtin GCC demangler       (default)\n"
        "   -v  use VAC demangler               (requires demangl.dll)\n"
        "   -x  use specified demangler         (example: \"myfilt.exe -z -n yyy\")\n"
        "\n";

/*****************************************************************************/

int main(int argc, char* argv[])
{
  EXCEPTIONREGISTRATIONRECORD ExRegRec;
  int     xq;
  int     rtn = 1;
  char ** pArray;

  xq = LoadExceptq(&ExRegRec, 0);

  if (!ParseArgs(argc, argv)) {
    if (xq)
      UninstallExceptq(&ExRegRec);
    return 1;
  }

do {
  if (!Init()) {
    fprintf(stderr, "Init failed\n");
    break;
  }

  if (opts & OPT_DEMANGLE_ONLY) {
    rtn = (Copy() ? 0 : 1);
    break;
  }

  if (!PrintUntil(apszModules)) {
    fprintf(stderr, "modules header not found\n");
    break;
  }

  if (!StoreSegments(apszGroups)) {
    fprintf(stderr, "StoreSegments failed\n");
    break;
  }

  if (!StoreGroups()) {
    fprintf(stderr, "StoreGroups failed\n");
    break;
  }

  pArray = SeekToHdr(apszExports, apszPubByName);
  if (!pArray) {
    fprintf(stderr, "publics by name header not found\n");
    break;
  }

  if (pArray == apszExports) {
    if (!StoreExports()) {
        fprintf(stderr, "StoreExports failed\n");
        break;
    }

    if (!SeekToHdr(apszPubByName, 0)) {
        fprintf(stderr, "publics by name header not found\n");
        break;
    }
  }

  if (!StorePublics()) {
    fprintf(stderr, "StorePublics failed\n");
    break;
  }

  StoreEntryPoint();

  if (!MarkDuplicates())
    break;

  if (!PrintEntriesByAddress())
    break;

  if (!PrintPublicsByName())
    break;

  if (opts & OPT_WARNINGS)
    PrintUntil(0);

  rtn = 0;

} while (0);

  /* general cleanup */
  if (fo)
    fclose(fo);
  if (fi)
    fclose(fi);
  if (buffer)
    free(buffer);

  /* cleanup if we used an external demangler */
  if (ulFiltPID)
    DosSendSignalException(ulFiltPID, XCPT_SIGNAL_BREAK);
  if (po)
    fclose(po);
  if (pi)
    fclose(pi);

  if (xq)
    UninstallExceptq(&ExRegRec);

  return rtn;
}

/*****************************************************************************/
/* This lets options & files be specified on the commandline in almost any
   order.  The only restriction is that optional files be specified in the
   same order as the options that required them.
*/

int     ParseArgs(int argc, char* argv[])
{
  int     ctr;
  int     order = 99;
  int     needInfile = 1;
  int     needOutfile = 0;
  int     needDemangler = 0;
  char *  ptr;

  if (argc < 2) {
    fprintf(stderr, pszHelp);
    return 0;
  }

  for (ctr = 1; ctr < argc; ctr++) {

    if (*argv[ctr] == '-' || *argv[ctr] == '/') {
      ptr = argv[ctr];

      while (*(++ptr)) {
        switch(*ptr) {
          case 'a':
          case 'A':
            opts |= OPT_SHOW_ARGS;
            break;

          case 'd':
          case 'D':
            opts |= OPT_DEMANGLE_ONLY;
            break;

          case 'm':
          case 'M':
            opts |= OPT_WARNINGS;
            break;

          case 'w':
          case 'W':
            opts |= OPT_WS;
            break;

          case 'n':
          case 'N':
            opts |= OPT_NO_DEMANGLE;
            break;

          case 'g':
          case 'G':
            opts &= ~OPT_VAC;
            opts |= OPT_GCC;
            break;

          case 'v':
          case 'V':
            opts &= ~OPT_GCC;
            opts |= OPT_VAC;
            break;

          case 'o':
          case 'O':
            needOutfile = order--;
            break;

          case 'x':
          case 'X':
            opts |= OPT_XXC;
            needDemangler = order--;
            break;
        } /* switch */
      } /* while */

      continue;
    } /* if */

    if (needDemangler && needDemangler > needOutfile) {
      pszDemangler = strdup(argv[ctr]);
      needDemangler = 0;
    } else
    if (needOutfile && needOutfile > needDemangler) {
      strcpy(fOut, argv[ctr]);
      needOutfile = 0;
    } else
    if (needInfile) {
      strcpy(fIn, argv[ctr]);
      needInfile = 0;
    } else {
      fprintf(stderr, "extra argument '%s'\n", argv[ctr]);
      return 0;
    }
  } /* for */

  if (needInfile || needOutfile || needDemangler) {
    fprintf(stderr, "missing argument for %s\n",
            (needInfile ? "map file" :
             (needOutfile ? "output file" : "demangler program")));
    return 0;
  }

  if (!(opts & (OPT_GCC | OPT_VAC | OPT_XXC)))
    opts |= OPT_GCC;

  return 1;
}

/*****************************************************************************/

int     Init(void)
{
  ULONG   ulSize;
  char *  ptr;
  char    szFile[CCHMAXPATH];

  if (!*fIn) {
    fprintf(stderr, ".map file not specified\n");
    return 0;
  }

  ptr = strrchr(fIn, '.');
  if (!ptr) {
    ptr = strchr(fIn, 0);
    strcpy(ptr, pszSrcExt);
  }

  if (DosQueryPathInfo(fIn, FIL_QUERYFULLNAME, szFile, sizeof(szFile))) {
    fprintf(stderr, "invalid input filename or path - '%s'\n", fIn);
    return 0;
  }
  strcpy(fIn, szFile);

  if (!*fOut) {
    ptr = strrchr(fIn, '\\');
    if (!ptr)
      ptr = fIn - 1;
    ptr++;
    strcpy(fOut, ptr);

    ptr = strrchr(fOut, '.');
    if (!ptr)
      ptr = strchr(fOut, 0);
    strcpy(ptr, (opts & OPT_DEMANGLE_ONLY) ? pszDemapExt : pszRemapExt);
  }

  if (DosQueryPathInfo(fOut, FIL_QUERYFULLNAME, szFile, sizeof(szFile))) {
    fprintf(stderr, "invalid output filename or path - '%s'\n", fOut);
    return 0;
  }
  strcpy(fOut, szFile);

  if (!stricmp(fIn, fOut)) {
    fprintf(stderr, "input and output files must have different names or paths\n");
    return 0;
  }

  if (DosQueryPathInfo(fIn, FIL_STANDARD, szFile, sizeof(szFile))) {
    fprintf(stderr, "unable to find input file '%s'\n", fIn);
    return 0;
  }
  ulSize = ((FILESTATUS3*)szFile)->cbFile;

  fi = fopen(fIn, "r");
  if (!fi) {
    fprintf(stderr, "unable to open input file '%s'\n", fIn);
    return 0;
  }

  if (!(opts & OPT_NO_DEMANGLE)) {
    if (opts & OPT_XXC) {
      if (!StartDemangler())
        return 0;
    }
    else
    if (opts & OPT_VAC) {
      if (!LoadVacDemangler()) {
        fprintf(stderr, "unable to load VAC demangler 'demangl.dll'\n");
        return 0;
      }
    }
  }

  buffer = malloc(ulSize);
  if (!buffer) {
    fprintf(stderr, "malloc for main buffer failed - size= %ld\n", ulSize);
    return 0;
  }
  memset(buffer, 0, ulSize);
  pCur = buffer;

  fo = fopen(fOut, "w");
  if (!fo) {
    fprintf(stderr, "unable to open output file '%s'\n", fOut);
    return 0;
  }

  return 1;
}

/*****************************************************************************/
/* This loads demangl.dll.  If it can't be found on the LIBPATH, it looks
   for it in the same directory as remap.exe (which may not be the current
   directory.
*/

int     LoadVacDemangler(void)
{
  HMODULE   hmod = 0;
  PPIB      ppib;
  PTIB      ptib;
  char *    ptr;
  char      szFailName[16];

  *szFailName = 0;
  if (DosLoadModule(szFailName, sizeof(szFailName), "DEMANGL", &hmod)) {
    DosGetInfoBlocks(&ptib, &ppib);
    if (DosQueryModuleName(ppib->pib_hmte, CCHMAXPATH, buf1) ||
        (ptr = strrchr(buf1, '\\')) == 0)
      return 0;

    strcpy(&ptr[1], "DEMANGL.DLL");
    if (DosLoadModule(szFailName, sizeof(szFailName), buf1, &hmod))
      return 0;
  }

  if (DosQueryProcAddr(hmod, 0, "demangle",     (PFN*)&pfnDemangle) ||
      DosQueryProcAddr(hmod, 0, "kind",         (PFN*)&pfnKind) ||
      DosQueryProcAddr(hmod, 0, "text",         (PFN*)&pfnText) ||
      DosQueryProcAddr(hmod, 0, "qualifier",    (PFN*)&pfnQualifier) ||
      DosQueryProcAddr(hmod, 0, "functionName", (PFN*)&pfnFunctionName) ||
      DosQueryProcAddr(hmod, 0, "erase",        (PFN*)&pfnErase)) {
    DosFreeModule(hmod);
    return 0;
  }

  return 1;
}

/*****************************************************************************/
/* This starts an external demangler after redirecting its stdin & stdout
   to a set of pipes.
*/

int     StartDemangler(void)
{
  ULONG   rc;
  HFILE   hTemp;

  HFILE   piRead  = HFILE_NONE;
  HFILE   piWrite = HFILE_NONE;
  HFILE   poRead  = HFILE_NONE;
  HFILE   poWrite = HFILE_NONE;
  HFILE   siSave  = HFILE_NONE;
  HFILE   soSave  = HFILE_NONE;
  RESULTCODES res;
  char *  pExe;
  char *  pArgs;
  char    szErr[32];

  strcpy(buf1, pszDemangler);
  pExe = strchr(buf1, 0) + 1;
  *pExe++ = 0;

  pArgs = Trim(buf1, 0);
  if (!pArgs) {
    fprintf(stderr, "no demangler program name\n");
    return 0;
  }
  strcpy(pExe, pArgs);

  rc = DosCreatePipe(&piRead, &piWrite, 1024);
  if (!rc)
    rc = DosCreatePipe(&poRead, &poWrite, 1024);
  if (rc) {
    fprintf(stderr, "DosCreatePipe - rc= %ld\n", rc);
    return 0;
  }

  pi = fdopen((int)poRead, "r");
  po = fdopen((int)piWrite, "w");
  if (!pi || !po) {
    fprintf(stderr, "fdopen failed - pi= %p  po= %p\n",
            (void*)pi, (void*)po);
    return 0;
  }
  setbuf(pi, 0);
  setbuf(po, 0);

  rc = DosDupHandle(0, &siSave);
  if (!rc)
    rc = DosDupHandle(1, &soSave);
  if (rc) {
    fprintf(stderr, "DosDupHandle to save stdin/out- rc= %ld\n", rc);
    return 0;
  }

  rc = DosClose(0);
  if (!rc)
     rc = DosClose(1);
  if (rc) {
    fprintf(stderr, "DosClose for stdin/out - rc= %ld\n", rc);
    return 0;
  }

  hTemp = 0;
  rc = DosDupHandle(piRead, &hTemp);
  if (!rc) {
    hTemp = 1;
    rc = DosDupHandle(poWrite, &hTemp);
  }
  if (rc) {
    fprintf(stderr, "DosDupHandle to redirect stdin/out - rc= %ld\n", rc);
    return 0;
  }

  DosClose(piRead);
  DosClose(poWrite);

  DosSetFHState(piWrite, OPEN_FLAGS_NOINHERIT);
  DosSetFHState(poRead,  OPEN_FLAGS_NOINHERIT);
  DosSetFHState(siSave,  OPEN_FLAGS_NOINHERIT);
  DosSetFHState(soSave,  OPEN_FLAGS_NOINHERIT);

  rc = DosExecPgm(szErr, sizeof(szErr), EXEC_ASYNC,
                  pArgs, 0, &res, pExe);
  if (rc) {
    fprintf(stderr, "DosExecPgm - rc= %ld\n", rc);
    return 0;
  }
  ulFiltPID = res.codeTerminate;

  hTemp = 0;
  rc = DosDupHandle(siSave, &hTemp);
  if (!rc) {
    hTemp = 1;
    rc = DosDupHandle(soSave, &hTemp);
  }
  if (rc) {
    fprintf(stderr, "DosDupHandle to restore stdin/out - rc= %ld\n", rc);
    return 0;
  }

  DosClose(siSave);
  DosClose(soSave);

  return 1;
}

/*****************************************************************************/
/* Copy from input to output until a specified header is reached. */

int     PrintUntil(char ** pArray)
{
  int     found = 0;
  int     blank = 0;
  char *  ptr;

  while (fgets(bufIn, sizeof(bufIn), fi)) {

    ptr = bufIn + strspn(bufIn, pszWS);
    if (!*ptr) {
      if (!blank)
        fputs(bufIn, fo);
      blank = 1;
      continue;
    }
    blank = 0;

    if (pArray) {
      found = MatchArray(pArray, ptr);
      if (found)
        break;
    }

    if (!(opts & OPT_WARNINGS) && strstr(ptr, pszWarningL))
      continue;

    fputs(bufIn, fo);
  }

  return found;
}

/*****************************************************************************/
/* Read lines until either the "seek" or "stop" string is found */

char ** SeekToHdr(char ** pSeek, char ** pStop)
{
  char ** pRtn = 0;

  while (fgets(bufIn, sizeof(bufIn), fi)) {
    if (MatchArray(pSeek, bufIn)) {
      pRtn = pSeek;
      break;
    }

    if (pStop) {
      if (MatchArray(pStop, bufIn)) {
        pRtn = pStop;
        break;
      }
    }
  }

  return pRtn;
}

/*****************************************************************************/
/* Match individual words in a string - this reduces the chance that a
   string will be missed due to formatting variations.
*/

int     MatchArray(char ** pArray, char * pText)
{
  while (**pArray) {
    pText = strstr(pText, *pArray);
    if (!pText)
      return 0;

    pText += strlen(*pArray);
    pArray++;
  }

  return 1;
}

/*****************************************************************************/
/* This parses and save module & segment info */

int     StoreSegments(char ** pStop)
{
  ULONG   seg = 0;
  ULONG   offs = 0;
  char *  ptr;
  char *  pErr = "unexpected end of file";

  while (fgets(bufIn, sizeof(bufIn), fi)) {

    ptr = bufIn + strspn(bufIn, pszWS);

    if (!*ptr)
      continue;

    if (ptr[4] == ':' && ptr[13] == ' ' && ptr[23] == 'H') {
      if (!ParseSegment(ptr, &seg, &offs)) {
        pErr = "malformed segment header";
        break;
      }
      continue;
    }

    if (!strncmp(ptr, szModule, cbModule)) {
      ptr += cbModule;
      if (!ParseModule(ptr, seg, offs)) {
        pErr = "malformed module listing";
        break;
      }
      continue;
    }

    if (!MatchArray(pStop, ptr)) {
      pErr = "modules header not found";
      break;
    }

    return 1;
  }

  fprintf(stderr, "%s\n", pErr);
  return 0;
}

/*****************************************************************************/

int     ParseSegment(char * pData, ULONG * pSeg, ULONG * pOffs)
{
  ULONG   lth;
  char *  pEnd;
  char *  pLth;
  char *  pName;
  char *  pClass;
  REMAP * r = (REMAP*)pCur;

  pData = Trim(pData, &pLth);
  pLth = Trim(pLth, &pName);
  if (!pData || !pLth)
    return 0;

  *pSeg = strtoul(pData, &pEnd, 16);
  if (!*pSeg || *pSeg > 255 || *pEnd != ':')
    return 0;

  *pOffs = strtoul(&pEnd[1], 0, 16);
  lth = strtoul(pLth, 0, 16);

  pName = Trim(pName, &pClass);
  pClass = TrimLine(pClass);
  if (!pName || !pClass)
    return 0;

  r->type |= REMAP_SEG;
  r->seg  = *pSeg;
  r->offs = *pOffs;
  pCur += sprintf(r->text, "%05lX%c%s%c%s",
                  lth, NULLCHAR, pName, NULLCHAR, pClass);
  pCur += sizeof(REMAP);
  r->next = (REMAP*)pCur;
  recCnt++;

  return 1;
}

/*****************************************************************************/

int     ParseModule(char * pData, ULONG ulSeg, ULONG ulOffs)
{
  ULONG   offs;
  ULONG   lth;
  char *  ptr;
  char *  pEnd;
  char *  pLib;
  char *  pSrc;
  REMAP * r = (REMAP*)pCur;

  offs = strtoul(pData, &ptr, 16);
  if (*ptr != ' ') {
    fprintf(stderr, "error getting offs - *ptr='%s'\n", ptr);
    return 0;
  }

  lth = strtoul(ptr, &pEnd, 16);
  if (strncmp(pEnd, szBytesFrom, cbBytesFrom)) {
    fprintf(stderr, "error in strncmp\n");
    return 0;
  }

  pLib = pEnd + cbBytesFrom;
  pSrc = strchr(pLib, '(');
  if (!pSrc) {
    fprintf(stderr, "couldn't find '('\n");
    return 0;
  }

  ptr = pSrc - 1;
  while (*ptr == ' ' || *ptr == '\t')
    *ptr-- = 0;

  pSrc++;
  ptr = strchr(pSrc, ')');
  if (!ptr) {
    fprintf(stderr, "couldn't find ')'\n");
    return 0;
  }
  *ptr = 0;
  ptr = strrchr(pSrc, '\\');
  if (!ptr)
    ptr = strrchr(pSrc, '/');
  if (ptr)
    pSrc = ptr + 1;


  r->type |= REMAP_MOD;
  r->seg  = ulSeg;
  r->offs = ulOffs + offs;
  pCur += sprintf(r->text, "%05lX%c%s%c%s",
                  lth, NULLCHAR, pSrc, NULLCHAR, pLib);
  pCur += sizeof(REMAP);
  r->next = (REMAP*)pCur;
  recCnt++;

  return 1;
}

/*****************************************************************************/
/* Parse & store the Groups section */

int     StoreGroups(void)
{
  char *  pSegOffs;
  char *  pGroup;

  while (fgets(bufIn, sizeof(bufIn), fi)) {

    REMAP * r = (REMAP*)pCur;

    pSegOffs = Trim(bufIn, &pGroup);
    if (!pSegOffs)
      break;

    r->seg = strtoul(pSegOffs, &pSegOffs, 16);
    if (r->seg > 255 || *pSegOffs != ':')
      return 0;
    r->offs = strtoul(&pSegOffs[1], 0, 16);

    pGroup = TrimLine(pGroup);
    if (!pGroup)
      return 0;

    r->type |= REMAP_GRP;
    strcpy(r->text, pGroup);
    pCur = strchr(r->text, 0) + 1;
    r->next = (REMAP*)pCur;
    recCnt++;
  }

  return 1;
}

/*****************************************************************************/
/* Parse, demangle, & store Exports.  Only the internal name is demangled;
   the external name is left as-is so it can be matched to other listings.
*/

int     StoreExports(void)
{
  int     skip = 0;
  char *  pSegOffs;
  char *  pExport;
  char *  pAlias;

  while (fgets(bufIn, sizeof(bufIn), fi)) {

    REMAP * r = (REMAP*)pCur;

    pSegOffs = Trim(bufIn, &pExport);
    if (!pSegOffs) {
      if (!skip) {
        skip = 1;
        continue;
      }
      break;
    }

    r->seg = strtoul(pSegOffs, &pSegOffs, 16);
    if (r->seg > 255 || *pSegOffs != ':') {
      fprintf(stderr, "r->seg failed - r->seg= %lx  *pSegOffs= '%c'\n",
              r->seg, *pSegOffs);
      return 0;
    }
    r->offs = strtoul(&pSegOffs[1], 0, 16);

    pExport = Trim(pExport, &pAlias);
    pAlias = Trim(pAlias, 0);
    if (!pExport || !pAlias) {
      fprintf(stderr, "Trim failed - pExport= %p  pAlias= %p\n",
              pExport, pAlias);
      return 0;
    }

    pAlias = Demangle(pAlias, buf1, sizeof(buf1), &r->type);
    if (!pAlias) {
      fprintf(stderr, "Demangle failed for alias\n");
      return 0;
    }

    r->type |= REMAP_EXP;
    pCur += sprintf(r->text, "%s%c%s",
                    pAlias, NULLCHAR, pExport);
    pCur += sizeof(REMAP);
    r->next = (REMAP*)pCur;
    recCnt++;
  }

  return 1;
}

/*****************************************************************************/
/* This is the big one.  It reads both Publics by Name & Publics by Value.
   Normally, they will be identical but in very large files each one may
   have symbols the other doesn't.  For imported symbols, only the internal
   name is demangled;  the external name is left as-is.
*/


int     StorePublics(void)
{
  int     skip = 0;
  int     byValue = 0;
  int     ctr;
  char *  ptr;
  char *  pEnd;
  char *  pSymbol;
  char *  pImport;


  while (fgets(bufIn, sizeof(bufIn), fi)) {

    REMAP * r = (REMAP*)pCur;

    ptr = bufIn + strspn(bufIn, pszWS);

    if (!*ptr) {
      if (!skip) {
        skip = 1;
        continue;
      }

      if (byValue)
        break;

      if (!SeekToHdr(apszPubByValue, 0)) {
        fprintf(stderr, "publics by value header not found\n");
        return 0;
      }
      byValue = 1;
      skip = 0;
      continue;
    }
    skip = 1;

    r->seg = strtoul(ptr, &pEnd, 16);
    if (r->seg > 255 || *pEnd != ':') {
      StoreError(bufIn, r);
      continue;
    }

    r->offs = strtoul(&pEnd[1], &ptr, 16);

    for (pEnd = ptr, ctr = 0; pEnd && *pEnd; pEnd = strpbrk(pEnd, pszWS)) {
      pEnd += strspn(pEnd, pszWS);
      if (!*pEnd)
        break;
      ctr++;
    }

    if (!ctr) {
      StoreError(bufIn, r);
      continue;
    }
    ptr += strspn(ptr, pszWS);

    if (ctr == 3 && !strncmp(ptr, szImp, cbImp)) {
      ctr--;
      r->type |= REMAP_IMP;
      ptr += cbImp;
    } else
    if (ctr == 2 && !strncmp(ptr, szAbs, cbAbs)) {
      ctr--;
      r->type |= REMAP_ABS;
      ptr += cbAbs;
    } else
    if (ctr != 1) {
      StoreError(bufIn, r);
      continue;
    }

    pSymbol = Trim(ptr, &ptr);
    if (!pSymbol) {
      fprintf(stderr, "symbol name not found\n");
      StoreError(bufIn, r);
      continue;
    }

    pSymbol = Demangle(pSymbol, buf1, sizeof(buf1), &r->type);
    if (!pSymbol) {
      fprintf(stderr, "Demangle failed for symbol name\n");
      StoreError(bufIn, r);
      continue;
    }

    /* Mapsym can't handle names over 255 characters long. */
    if (opts & OPT_DEMANGLE_ONLY) {
      if (strlen(pSymbol) > 255)
        strcpy(&pSymbol[252], "...");
    }

    strcpy(r->text, pSymbol);
    pCur = strchr(r->text, 0) + 1;

    if (ctr == 2) {
      pImport = Trim(ptr, 0);
      if (!pImport) {
        fprintf(stderr, "import name not found\n");
        StoreError(bufIn, r);
        continue;
      }

      if (*pImport == '(') {
        pImport++;
        ptr = strchr(pImport, 0) - 1;
        if (*ptr == ')')
          *ptr = 0;
      }

      strcpy(pCur, pImport);
      pCur = strchr(pCur, 0) + 1;
    }

    if (!(r->type & REMAP_IMP))
      r->type |= REMAP_OBJ;
    r->next = (REMAP*)pCur;
    recCnt++;
  }

  return 1;
}

/*****************************************************************************/

int     StoreEntryPoint(void)
{
  int       skip = 0;
  char *    ptr;
  REMAP *   r = (REMAP*)pCur;

  while (fgets(bufIn, sizeof(bufIn), fi)) {
    ptr = bufIn + strspn(bufIn, pszWS);

    if (!*ptr) {
      if (!skip) {
        skip = 1;
        continue;
      }
      break;
    }

    if (strncmp(ptr, szPgmEP, cbPgmEP))
      break;

    ptr += cbPgmEP;
    r->seg  = strtoul(ptr, &ptr, 16);
    r->offs = strtoul(&ptr[1], 0, 16);
    strcpy(r->text, pszEntryPoint);
    pCur = strchr(r->text, 0) + 1;        

    r->type |= REMAP_EPT;
    r->next = (REMAP*)pCur;
    recCnt++;
    return 1;
  }

  fprintf(stderr, "unable to locate program entry point\n");
  return 0;
}

/*****************************************************************************/
/* This is used by StorePublics to store unrecognized lines.  If the line
   is a warning msg, it's output immediately so that it appears before the
   Publics are listed.
*/

int     StoreError(char * pBuf, REMAP * r)
{
  char *  ptr;

  if (strstr(pBuf, pszWarningL)) {
    if (opts & OPT_WARNINGS)
      fputs(pBuf, fo);
    return 1;
  }

  r->type = REMAP_ERR;
  r->seg  = 0xffff;
  r->offs = 0xffffffff;

  ptr = strchr(pBuf, 0) - 1;
  while (ptr > pBuf && strchr(pszWS, *ptr))
    ptr--;
  *(++ptr) = 0;
  strcpy(r->text, pBuf);
  pCur = strchr(r->text, 0) + 1;
  r->next = (REMAP*)pCur;
  recCnt++;

  return 1;
}

/*****************************************************************************/
/* This trims words in-place.  It returns the first non-whitespace character
   in pTrim & puts a null after the last non-ws char.  If ppNext is supplied,
   it points to the character after the inserted null.
*/

char *  Trim(char * pTrim, char** ppNext)
{
  char *  ptr;

  if (ppNext)
    *ppNext = 0;

  if (!pTrim)
    return 0;

  pTrim += strspn(pTrim, pszWS);
  if (!*pTrim)
    return 0;

  ptr = strpbrk(pTrim, pszWS);
  if (!ptr)
    return pTrim;

  *ptr++ = 0;
  if (ppNext && *ptr)
    *ppNext = ptr;

  return pTrim;
}

/*****************************************************************************/
/* This trims a line in-place. */

char *  TrimLine(char * pTrim)
{
  char *  ptr;

  if (!pTrim)
    return 0;

  pTrim += strspn(pTrim, pszWS);
  if (!*pTrim)
    return 0;

  ptr = strchr(pTrim, 0) - 1;
  while (ptr > pTrim && strchr(pszWS, *ptr))
    ptr--;
  *(++ptr) = 0;

  return pTrim;
}

/*****************************************************************************/
/* Called by the GCC demangler one or more times to copy the pieces
   of a demangled method to an output buffer.  pv is actually a ptr
   to the pOut & cbOut arguments to Demangle() - cheesy, huh?
*/

void    DemangleCallback(const char* pSrc, size_t cbSrc, void * pv)
{
  struct ptr_lth {
    char *  pbuf;
    ULONG   cbbuf;
  } * p;
  size_t  cnt;

  p = (struct ptr_lth*)pv;
  cnt = strlen(p->pbuf);
  if (cnt + cbSrc + 1 < p->cbbuf)
    strcpy(p->pbuf + cnt, pSrc);

  return;
}

/*****************************************************************************/
/* This handles demangling by all 3 demanglers:  an external process;
   VAC via demangl.dll; and the builtin GCC demangler.
*/

char *  Demangle(char * pIn, char * pOut, ULONG cbOut, ULONG * pFlags)
{
  int     ndx;
  char *  ptr;

  if (opts & OPT_NO_DEMANGLE)
    return pIn;

  *pOut = 0;

  if (opts & OPT_XXC) {
    fprintf(po, "%s\n", pIn);
    fgets(pOut, cbOut, pi);

    if (!(opts & OPT_WS)) {
      ptr = pOut;
      while ((ptr = strpbrk(ptr, pszWS)) != 0)
        *ptr++ = '_';
    }

    return pOut;
  }

  /* The functions in demangl.dll all use Optlink which gcc 4.x can't handle,
     so they're invoked via wrapper functions in remap_vac.c.  Each wrapper
     has '_vac' appended to the function's original name.
  */
  if (opts & OPT_VAC) {
    Name *    nm;
    NameKind  nk;

    nm = demangle_vac(pIn, &ptr, (RegularNames | ClassNames | SpecialNames));
    if (!nm)
      return pIn;

    nk = kind_vac(nm);

    if (!(opts & OPT_SHOW_ARGS) &&
        (nk == MemberFunction || nk == Function)) {

      if (nk == MemberFunction) {
        ptr = qualifier_vac(nm);
        if (ptr) {
          strcpy(pOut, ptr);
          strcat(pOut, "::");
        }
      }
      ptr = functionName_vac(nm);
      if (ptr)
        strcat(pOut, ptr);
    }
    else {
      ptr = text_vac(nm);
      if (!ptr)
        return pIn;
      strcpy(pOut, ptr);
    }

    if (nk == Special && (ptr = strstr(pOut, szVtableVAC)) != 0) {
      char *  pEnd;

      *pFlags |= REMAP_VTABLE;
      *ptr = 0;

      /* vtable entries for subclasses are formatted '{subclass}class';
       * this reformats it as 'class::subclass'
      */
      if (*pOut == '{' && (pEnd = strchr(pOut, '}')) != 0) {
        *pEnd++ = 0;
        *ptr++ = ':';
        *ptr++ = ':';
        strcpy(ptr, &pOut[1]);
        strcpy(pOut, pEnd);
      }
    }

    erase_vac(nm);

    if (!(opts & OPT_WS)) {
      ptr = pOut;
      while ((ptr = strpbrk(ptr, pszWS)) != 0)
        *ptr++ = '_';
    }

    return pOut;
  }

  /* OPT_GCC */
  if (!memcmp(pIn, "__Z", 3) || !memcmp(pIn, "@_Z", 3))
    ndx = 1;
  else
  if (!memcmp(pIn, "_Z", 2))
    ndx = 0;
  else
    return pIn;

  /* Mozilla (at least) appends a unique identifier to many symbols
     that the demangler can't handle.  If the leading characters of
     the identifier ("$w$") are found, remove the identifier.
  */
  ptr = strstr(pIn, pszTrouble);
  if (ptr)
    *ptr = 0;

  *pOut = 0;
  if (!cplus_demangle_v3_callback(&pIn[ndx],
                                  ((opts & OPT_SHOW_ARGS) ? DMGL_PARAMS : 0),
                                  &DemangleCallback, &pOut))
    return pIn;

  ptr = strchr(pOut, 0) - 1;
  while (ptr > pOut && strchr(pszWS, *ptr))
    ptr--;
  *(++ptr) = 0;

  /* Convert metadata generated by the demangler into flags,
     then remove the metadata.
  */
  if (strchr(pOut, ' ')) {
    if (!strncmp(pOut, szVtable, cbVtable)) {
      *pFlags |= REMAP_VTABLE;
      strcpy(pOut, pOut + cbVtable);
    }
    else
    if (!strncmp(pOut, szThunk, cbThunk)) {
      *pFlags |= REMAP_THUNK;
      strcpy(pOut, pOut + cbThunk);

      /* remove the argument list that gets included for thunks */
      if (!(opts & OPT_SHOW_ARGS) &&
          (ptr = strchr(pOut, '(')) != 0)
        *ptr = 0;
    }
    else
    if (!strncmp(pOut, szTypeInfo, cbTypeInfo)) {
      *pFlags |= REMAP_TYPEINFO;
      strcpy(pOut, pOut + cbTypeInfo);
    }
    else
    if (!strncmp(pOut, szTypeName, cbTypeName)) {
      *pFlags |= REMAP_TYPENAME;
      strcpy(pOut, pOut + cbTypeName);
    }
    else
    if (!strncmp(pOut, szGuard, cbGuard)) {
      *pFlags |= REMAP_GUARD;
      strcpy(pOut, pOut + cbGuard);
    }
  }

  if (!(opts & OPT_WS)) {
    ptr = pOut;
    while ((ptr = strpbrk(ptr, pszWS)) != 0)
        *ptr++ = '_';
  }

  return pOut;
}

/*****************************************************************************/
/* Remove the duplicate entries that result from reading both
   Publics by Name and Publics by value.
*/

int     MarkDuplicates(void)
{
  int     ctr;
  REMAP** pr;
  REMAP** pArr;
  REMAP * pRec;

  if (!recCnt) {
    fprintf(stderr, "no records to sort\n");
    return 0;
  }

  pr = (REMAP**)malloc((recCnt + 1) * sizeof(REMAP*));
  if (!pr) {
    fprintf(stderr, "malloc failed for MarkDuplicates - bytes= %d\n",
            recCnt * sizeof(REMAP*));
    return 0;
  }

  pRec = (REMAP*)buffer;
  pArr = pr;
  ctr = 0;
  while (pRec->next) {
    *pArr++ = pRec;
    pRec = pRec->next;
    ctr++;
  }
  *pArr = 0;

  if (ctr != recCnt) {
    fprintf(stderr, "invalid record count:  cnt= %d  recCnt= %d\n",
            ctr, recCnt);
    return 0;
  }

  qsort(pr, recCnt, sizeof(REMAP*), DuplicateSorter);
  free(pr);

  return 1;
}

/*****************************************************************************/
/* Sort then print all entries by address. */

int     PrintEntriesByAddress(void)
{
  int     ctr;
  REMAP** pr;
  REMAP** pArr;
  REMAP * pRec;

  pr = (REMAP**)malloc((recCnt + 1) * sizeof(REMAP*));
  if (!pr) {
    fprintf(stderr, "malloc failed for PrintEntriesByAddress - bytes= %d\n",
            recCnt * sizeof(REMAP*));
    return 0;
  }

  pRec = (REMAP*)buffer;
  pArr = pr;
  ctr  = 0;
  while (pRec->next) {
    if (!(pRec->type & REMAP_DUP2)) {
      *pArr++ = pRec;
      ctr++;
    }
    pRec = pRec->next;
  }
  *pArr = 0;

  qsort(pr, ctr, sizeof(REMAP*), AddressSorter);

  PrintByAddress(pr);
  free(pr);

  return 1;
}

/*****************************************************************************/
/* Sort then print publics by address. */


int     PrintPublicsByName(void)
{
  int     ctr;
  REMAP** pr;
  REMAP** pArr;
  REMAP * pRec;

  pr = (REMAP**)malloc((recCnt + 1) * sizeof(REMAP*));
  if (!pr) {
    fprintf(stderr, "malloc failed for PublicsByName - bytes= %d\n",
            recCnt * sizeof(REMAP*));
    return 0;
  }

  pRec = (REMAP*)buffer;
  pArr = pr;
  ctr  = 0;
  while (pRec->next) {
    if ((pRec->type & (REMAP_IMP | REMAP_EXP | REMAP_EPT)) ||
        (pRec->type & (REMAP_OBJ | REMAP_DUP)) == REMAP_OBJ) {
      if (!(pRec->type & REMAP_DUP2)) {
        *pArr++ = pRec;
        ctr++;
      }
    }
    pRec = pRec->next;
  }
  *pArr = 0;

  qsort(pr, ctr, sizeof(REMAP*), NameSorter);

  PrintByName(pr);
  free(pr);

  return 1;
}

/*****************************************************************************/
/* qsort callback for identifying duplicates */

int     DuplicateSorter(const void *key, const void *element)
{
  int     res;
  ULONG   kType;
  ULONG   eType;

  res = (*(REMAP**)key)->seg - (*(REMAP**)element)->seg;
  if (res)
    return res;

  res = (*(REMAP**)key)->offs - (*(REMAP**)element)->offs;
  if (res)
    return res;

  kType = (*(REMAP**)key)->type;
  eType = (*(REMAP**)element)->type;

  res = (kType & REMAP_MASK) - (eType & REMAP_MASK);
  if (res)
    return res;

  res = strcmp((*(REMAP**)key)->text, (*(REMAP**)element)->text);
  if (res)
    return res;

  if (kType & REMAP_IMP) {
    res = ImportSorter((*(REMAP**)key)->text, (*(REMAP**)element)->text);
    if (res)
      return res;
  }

  if (!(kType & REMAP_DUP2) && !(eType & REMAP_DUP2))
    (*(REMAP**)key)->type |= REMAP_DUP2;

  return 0;
}

/*****************************************************************************/
/* qsort callback for sorting by address */

int     AddressSorter(const void *key, const void *element)
{
  int     res;
  ULONG   kType;
  ULONG   eType;

  res = (*(REMAP**)key)->seg - (*(REMAP**)element)->seg;
  if (res)
    return res;

  res = (*(REMAP**)key)->offs - (*(REMAP**)element)->offs;
  if (res)
    return res;

  kType = (*(REMAP**)key)->type;
  eType = (*(REMAP**)element)->type;

  res = (kType & REMAP_TYPE) - (eType & REMAP_TYPE);
  if (res) {
    if ((kType & REMAP_OBJ) && (eType & REMAP_EXP))
      (*(REMAP**)key)->type |= REMAP_DUP;
    else
    if ((kType & REMAP_EXP) && (eType & REMAP_OBJ))
      (*(REMAP**)element)->type |= REMAP_DUP;

    return res;
  }

  if (kType & REMAP_IMP) {
    res = ImportSorter((*(REMAP**)key)->text, (*(REMAP**)element)->text);
    if (res)
      return res;
  }

  return stricmp((*(REMAP**)key)->text, (*(REMAP**)element)->text);
}

/*****************************************************************************/
/* qsort callback for sorting by name */

int     NameSorter(const void *key, const void *element)
{
  int     res;
  char *  pk = (*(REMAP**)key)->text;
  char *  pe = (*(REMAP**)element)->text;

  while (*pk && *pk == '_')
    pk++;
  while (*pe && *pe == '_')
    pe++;

  res = stricmp(pk, pe);
  if (res)
    return res;

  res = (*(REMAP**)key)->seg - (*(REMAP**)element)->seg;
  if (res)
    return res;

  res = (*(REMAP**)key)->offs - (*(REMAP**)element)->offs;
  if (res)
    return res;

  if (((*(REMAP**)key)->type & REMAP_IMP) &&
      ((*(REMAP**)element)->type & REMAP_IMP))
    return ImportSorter(pk, pe);

  return stricmp((*(REMAP**)key)->text, (*(REMAP**)element)->text);
}

/*****************************************************************************/
/* Convenience function for sorting imports by module & exported name. */

int     ImportSorter(char* pk, char* pe)
{
  int     res;
  char *  pkdot;
  char *  pedot;

  pk = strchr(pk, 0) + 1;
  pe = strchr(pe, 0) + 1;

  pkdot = strchr(pk, '.');
  pedot = strchr(pe, '.');

  if (pkdot && pedot &&
    isdigit(pkdot[1]) && isdigit(pedot[1])) {
    *pkdot = 0;
    *pedot = 0;
    res = stricmp(pk, pe);
    *pkdot = '.';
    *pedot = '.';
    if (res)
      return res;

    return atoi(&pkdot[1]) - atoi(&pedot[1]);
  }

  return stricmp(pk, pe);
}

/*****************************************************************************/
/* Print the unified listing by address. */

void    PrintByAddress(REMAP** pr)
{
  int     errhdr = 0;
  int     last   = 0;
  char *  p0;
  char *  p1;
  char *  p2;
  char *  pNL;
  REMAP * r;
  char    szFlags[16];

  r = *pr;

  fputs(pszAddressHdr, fo);
  fputs(pszColumnHdr, fo);

  while (r) {

    switch (r->type & REMAP_TYPE) {
      case REMAP_GRP:
        p0 = r->text;
        fprintf(fo, "%s G %04lX:%08lX         %s\n",
                (last ? "\n" : ""),
                r->seg, r->offs, p0);
        last = REMAP_GRP;
        break;

      case REMAP_SEG:
        p0 = r->text;
        p1 = strchr(p0, 0) + 1;
        p2 = strchr(p1, 0) + 1;
        fprintf(fo, "%s S %04lX:%08lX  %-5s  %-24s  %s\n",
                (last == REMAP_SEG ? "" : "\n"),
                r->seg, r->offs, p0, p1, p2);
        last = REMAP_SEG;
        break;

      case REMAP_MOD:
        p0 = r->text;
        p1 = strchr(p0, 0) + 1;
        p2 = strchr(p1, 0) + 1;

        pNL = "\n";
        if (last == REMAP_SEG || last == REMAP_MOD) {
            if (pr[1] && (pr[1]->type & (REMAP_SEG | REMAP_MOD)))
                pNL = "";
        }

        fprintf(fo, "%s M %04lX:%08lX  %-5s  %-24s  (%s)\n",
                pNL, r->seg, r->offs, p0, p1, p2);
        last = REMAP_MOD;
        break;

      case REMAP_IMP:
        p0 = r->text;
        p1 = strchr(p0, 0) + 1;

        fprintf(fo, " . %04lX:%08lX  %-5s  %-24s  [%s]\n",
            r->seg, r->offs, DecodeFlags(r->type, szFlags), p0, p1);
        last = REMAP_IMP;
        break;

      case REMAP_EXP:
        p0 = r->text;
        p1 = strchr(p0, 0) + 1;

        fprintf(fo, " . %04lX:%08lX  %-5s  %-24s  [%s]\n",
                r->seg, r->offs, DecodeFlags(r->type, szFlags), p0, p1);
        last = REMAP_EXP;
        break;

      case REMAP_OBJ:
        if (r->type & REMAP_DUP)
          break;

        p0 = r->text;
        fprintf(fo, " . %04lX:%08lX  %-5s  %s\n",
                r->seg, r->offs, DecodeFlags(r->type, szFlags), p0);
        last = REMAP_OBJ;
        break;

      case REMAP_EPT:
        p0 = r->text;
        fprintf(fo, " E %04lX:%08lX  %-5s  <%s>\n",
                r->seg, r->offs, DecodeFlags(r->type, szFlags), p0);
        last = REMAP_EPT;
        break;

      case REMAP_ERR:
        if (!errhdr) {
            errhdr = 1;
            fprintf(fo, "\n Type  Mapfile lines that couldn't be parsed\n");
        }
        p0 = r->text;
        fprintf(fo, " ? %s\n", p0);
        last = REMAP_ERR;
        break;

      default:
        fprintf(fo, " ERROR:  unknown type= %lu\n", (r->type & REMAP_TYPE));
        last = REMAP_ERR;
        break;
    }

    pr++;
    r = *pr;
  }

  fputs(pszLegend, fo);
  if (opts & OPT_GCC)
    fputs(pszLegendGCC, fo);
  else
  if (opts & OPT_VAC)
    fputs(pszLegendVAC, fo);
  else
    fputs(pszLegendXXC, fo);

  return;
}

/*****************************************************************************/
/* Print the publics listing by name. */

void    PrintByName(REMAP** pr)
{
  char *  p0;
  char *  p1;
  REMAP * r;
  char    szFlags[16];

  r = *pr;

  fputs(pszNameHdr, fo);
  fputs(pszColumnHdr, fo);

  while (r) {

    switch (r->type & REMAP_TYPE) {

      case REMAP_IMP:
        p0 = r->text;
        p1 = strchr(p0, 0) + 1;
        fprintf(fo, "   %04lX:%08lX  %-5s  %-24s  [%s]\n",
                r->seg, r->offs, DecodeFlags(r->type, szFlags), p0, p1);
        break;

      case REMAP_EXP:
        p0 = r->text;
        p1 = strchr(p0, 0) + 1;
        fprintf(fo, "   %04lX:%08lX  %-5s  %-24s  [%s]\n",
                r->seg, r->offs, DecodeFlags(r->type, szFlags), p0, p1);
        break;

      case REMAP_OBJ:
        if (r->type & REMAP_DUP) {
          fprintf(fo, " ERROR:  found REMAP_DUP\n");
          break;
        }

        p0 = r->text;
        fprintf(fo, "   %04lX:%08lX  %-5s  %s\n",
                r->seg, r->offs, DecodeFlags(r->type, szFlags), p0);
        break;

      case REMAP_EPT:
        p0 = r->text;
        fprintf(fo, " E %04lX:%08lX  %-5s  <%s>\n",
                r->seg, r->offs, DecodeFlags(r->type, szFlags), p0);
        break;

      default:
        fprintf(fo, " ERROR:  unexpected type= %lu\n",
                (r->type & REMAP_TYPE));
        break;
    }

    pr++;
    r = *pr;
  }

  fputs(pszLegend, fo);
  if (opts & OPT_GCC)
    fputs(pszLegendGCC, fo);
  else
  if (opts & OPT_VAC)
    fputs(pszLegendVAC, fo);
  else
    fputs(pszLegendXXC, fo);

  return;
}

/*****************************************************************************/
/* Convert flags to character flags. */

char *  DecodeFlags(ULONG flags, char* pszFlags)
{
  int     ctr = 0;

  if (flags & REMAP_EXP)
    pszFlags[ctr++] = 'x';

  if (flags & REMAP_IMP)
    pszFlags[ctr++] = 'i';
  
  if (flags & REMAP_ABS)
    pszFlags[ctr++] = 'a';

  if (flags & REMAP_VTABLE)
    pszFlags[ctr++] = 'v';

  if (flags & REMAP_THUNK)
    pszFlags[ctr++] = 'k';

  if (flags & REMAP_TYPEINFO)
    pszFlags[ctr++] = 't';

  if (flags & REMAP_TYPENAME)
    pszFlags[ctr++] = 'n';

  if (flags & REMAP_GUARD)
    pszFlags[ctr++] = 'g';

  if (flags & REMAP_DUP)
    pszFlags[ctr++] = '1';

  if (flags & REMAP_DUP2)
    pszFlags[ctr++] = '2';

  if (flags & ~REMAP_MASK)
    pszFlags[ctr++] = '?';

  pszFlags[ctr] = 0;

  return pszFlags;
}

/*****************************************************************************/
/*  code used to when -d (demangle-only) option is selected                  */
/*****************************************************************************/

int     Copy(void)
{
  char ** pSeek = 0;
  REMAP** pArr;

  /* copy lines until either the exports or publics section is encountered */
  while (fgets(bufIn, sizeof(bufIn), fi)) {
    pSeek = apszExports;
    if (MatchArray(pSeek, bufIn)) {
      fputs(bufIn, fo);
      break;
    }
    pSeek = apszPubByName;
    if (MatchArray(pSeek, bufIn)) {
      fputs(bufIn, fo);
      break;
    }
    if (!(opts & OPT_WARNINGS) && strstr(bufIn, pszWarningL))
      continue;

    fputs(bufIn, fo);
  }

  /* if there are exports, demangle & reprint them */
  if (pSeek == apszExports) {
    if (!CopyExports())
      return 0;
  }

  /* read & store both Pubs by Name & Pubs by Value */
  if (!StorePublics()) {
    fprintf(stderr, "StorePublics failed\n");
    return 0;
  }

  /* sort the publics and eliminate dups */
  pArr = SetupPublicsSort();
  if (!pArr)
    return 0;

  /* sort by name & print */
  qsort(pArr, recCnt, sizeof(REMAP*), NameSorter);
  PrintPublics(pArr);

  /* sort by value & print */
  qsort(pArr, recCnt, sizeof(REMAP*), AddressSorter);
  fprintf(fo, "\n\n %s\n", pszPubByVal);
  PrintPublics(pArr);

  /* free the array of public entries */
  free(pArr);

  /* copy whatever remains (entrypoint & trailing linker messages) */
  fputs("\n", fo);
  while (fgets(bufIn, sizeof(bufIn), fi)) {
    if (!(opts & OPT_WARNINGS) && strstr(bufIn, pszWarningL))
      continue;
    fputs(bufIn, fo);
  }

  return 1;
}

/*****************************************************************************/
/* Demangle & reprint Exports */

int     CopyExports(void)
{
  int     blank = 0;
  ULONG   flags;
  char *  p0;
  char *  p1;
  char *  p2;

  while (fgets(bufIn, sizeof(bufIn), fi)) {

    p0 = bufIn + strspn(bufIn, pszWS);
    if (!*p0) {
      blank = 1;
      fputs(bufIn, fo);
      continue;
    }

    if (blank) {
      blank = 0;
      if (MatchArray(apszPubByName, p0)) {
        fputs(bufIn, fo);
        break;
      }
    }

    p0 = Trim(p0, &p1);
    p1 = Trim(p1, &p2);
    p2 = Trim(p2, 0);
    if (!p1 || !p2) {
      fprintf(stderr, "CopyExports failed - p1= %p  p2= %p\n", p1, p2);
      return 0;
    }

    flags = 0;
    p2 = Demangle(p2, buf1, sizeof(buf1), &flags);
    fprintf(fo, " %s %22s  %s%s\n", p0, p1, p2, DecodeFlagName(flags));
  }

  return 1;
}

/*****************************************************************************/
/* Identify duplicates, then copy non-duplicate entries to a new array. */

REMAP** SetupPublicsSort(void)
{
  int     ctr;
  REMAP** pRtn;
  REMAP** pArr;
  REMAP * pRec;

  if (!MarkDuplicates())
    return 0;

  pRtn = (REMAP**)malloc((recCnt + 1) * sizeof(REMAP*));
  if (!pRtn) {
    fprintf(stderr, "malloc failed for SetupPublicsSort - bytes= %d\n",
            recCnt * sizeof(REMAP*));
    return 0;
  }

  pRec = (REMAP*)buffer;
  pArr = pRtn;
  ctr = 0;
  while (pRec->next) {
    if (!(pRec->type & REMAP_DUP2)) {
      *pArr++ = pRec;
      ctr++;
    }
    pRec = pRec->next;
  }
  *pArr = 0;

  recCnt = ctr;

  return pRtn;
}

/*****************************************************************************/
/* Print publics by name or value in the same format as the original file. */

int     PrintPublics(REMAP** pr)
{
  char *  p0;
  char *  p1;
  REMAP * r;

  r = *pr;

  while (r) {

    switch (r->type & REMAP_TYPE) {

      case REMAP_IMP:
        if (r->type & REMAP_ATTRMASK) {
          strcpy(buf1, r->text);
          strcat(buf1, DecodeFlagName(r->type));
          p0 = buf1;
        }
        else
          p0 = r->text;
        p1 = strchr(r->text, 0) + 1;
        fprintf(fo, " %04lX:%08lX  Imp  %-20s (%s)\n",
                r->seg, r->offs, p0, p1);
        break;

      case REMAP_OBJ:
        p0 = r->text;
        fprintf(fo, " %04lX:%08lX  %s  %s%s\n",
                r->seg, r->offs,
                ((r->type & REMAP_ABS) ? "Abs" : "   "),
                p0, DecodeFlagName(r->type));
        break;

      default:
        fprintf(fo, " %s\n", r->text);
        break;
    }

    pr++;
    r = *pr;
  }

  return 1;
}

/*****************************************************************************/
/* Convert the flags generated when the function was demangled into
   strings that will be appended to the method name.
*/

char *  DecodeFlagName(ULONG flags)
{
  if (flags & REMAP_VTABLE)
    return "::{vtable}";

  if (flags & REMAP_THUNK)
    return "::{thunk}";

  if (flags & REMAP_TYPEINFO)
    return "::{typeinfo}";

  if (flags & REMAP_TYPENAME)
    return "::{typename}";

  if (flags & REMAP_GUARD)
    return "::{guard_variable}";

  return "";
}

/*****************************************************************************/

