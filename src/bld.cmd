@echo off
REM
rem   This is a simple batch file to build Remap using both VACPP & GCC
rem   (see remap.c for an explanation of why both are required).
rem   It calls separate .cmd files to setup the environment for each
rem   compiler.  Modify or 'rem' the appropriate lines as needed.
rem   Note:  SET/ENDLOCAL doesn't handle BEGINLIBPATH, so we have to.
rem
rem VACPP: No default libraries, Decorate external names, __cdecl linkage,
rem        Output remap_vac.o
rem
SET BEGINSAVE=%BEGINLIBPATH%
SETLOCAL
call G:\Ibmcxxo\bin\setenv.cmd
@echo on
icc /Gn+ /Gy+ /Mc /O+ /Q /Sm /Ss /W3 /Foremap_vac.o /C remap_vac.c
@IF ERRORLEVEL 1 goto end
@ENDLOCAL
@rem
@rem GCC: OMF format, Optimized, No-strict-aliasing (to suppress a warning msg),
@rem      Link in libiberty (the gcc3 demangler), Output remap.exe
@rem
@SET BEGINLIBPATH=%BEGINSAVE%
@SETLOCAL
@call G:\MOZTOOLS\setmoz441.cmd > nul
@echo on
gcc -c -Wall -Zomf -O2 -fno-strict-aliasing remap.c
@IF ERRORLEVEL 1 goto end
g++ -o remap.exe -s -Zomf -Zmap -Zlinker /EXEPACK:2 remap.o remap_vac.o -llibiberty remap.def
@IF ERRORLEVEL 1 goto end
mapsym remap
:end
@ENDLOCAL
@SET BEGINLIBPATH=%BEGINSAVE%
@SET BEGINSAVE=
