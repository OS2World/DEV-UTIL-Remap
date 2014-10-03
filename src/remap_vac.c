/*****************************************************************************/
/*  remap_vac.c                                                              */
/*****************************************************************************/

#include "remap_demangle.h"

/*****************************************************************************/

PFNDEMANGLE pfnDemangle;
PFNKIND     pfnKind;
PFNTEXT     pfnText;
PFNTEXT     pfnQualifier;
PFNTEXT     pfnFunctionName;
PFNERASE    pfnErase;

/*****************************************************************************/
/*  gcc 4.x doesn't handle _Optlink correctly, so these functions have
 *  to be wrapped and the wrapper has to be compiled using VAC or some
 *  other compiler that can handle it.
 */

Name* __cdecl   demangle_vac(char* name, char** rest, unsigned long options)
{
  return pfnDemangle(name, rest, options);
}

NameKind __cdecl kind_vac(Name* name)
{
  return pfnKind(name);
}

char* __cdecl   text_vac(Name* name)
{
  return pfnText(name);
}

char* __cdecl   qualifier_vac(Name* name)
{
  return pfnQualifier(name);
}

char* __cdecl   functionName_vac(Name* name)
{
  return pfnFunctionName(name);
}

void  __cdecl   erase_vac(Name* name)
{
  pfnErase(name);
}

/*****************************************************************************/

