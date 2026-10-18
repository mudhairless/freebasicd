/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The canonical-spelling table behind CaseStyle::Canonical: catalog words
// whose documented spelling differs from plain TitleCase.
//
// Provenance: the FreeBASIC wiki page for a word (the local mirror under
// compiler/doc/manual/markdown, whose line 4 is the page's display spelling),
// cross-checked against the texinfo manual built from the same pages where a
// page title carries a disambiguator ("Pset (Statement)") or the prose
// disagrees with the title ("PSet" 58 to 16 in the texinfo corpus, so PSet
// wins). Words whose docs always stay lowercase (the calling conventions
// cdecl/stdcall/pascal, the preprocessor predicate defined, the C-varargs
// helpers) carry their lowercase spelling so Canonical never mangles them.
//
// To regenerate the candidate list after an fbc/wiki update: extract the
// keyword, type, and intrinsic tables from language.cpp, title-case each key,
// and diff against the wiki page line 4 for KeyPg<Word>.md; entries whose
// value equals titleCase(key) do not belong here. Re-verify the "keep
// lowercase" entries the same way — the counts above came from that diff.

#include "format/spelling.h"

namespace fbformat {
namespace {

struct SpellingRow {
  char const *lower;
  char const *canonical;
};

// Sorted by `lower` (ASCII); looked up with a binary search.
constexpr SpellingRow kCanonical[] = {
    {"__fastcall", "__fastcall"},
    {"__thiscall", "__thiscall"},
    {"arraylen", "ArrayLen"},
    {"arraysize", "ArraySize"},
    {"assertwarn", "AssertWarn"},
    {"bitreset", "BitReset"},
    {"bitset", "BitSet"},
    {"bload", "BLoad"},
    {"bsave", "BSave"},
    {"byref", "ByRef"},
    {"byval", "ByVal"},
    {"callocate", "CAllocate"},
    {"cbool", "CBool"},
    {"cbyte", "CByte"},
    {"cdbl", "CDbl"},
    {"cdecl", "cdecl"},
    {"chdir", "ChDir"},
    {"cint", "CInt"},
    {"clng", "CLng"},
    {"clngint", "CLngInt"},
    {"condbroadcast", "CondBroadcast"},
    {"condcreate", "CondCreate"},
    {"conddestroy", "CondDestroy"},
    {"condsignal", "CondSignal"},
    {"condwait", "CondWait"},
    {"cptr", "CPtr"},
    {"cshort", "CShort"},
    {"csign", "CSign"},
    {"csng", "CSng"},
    {"csrlin", "CsrLin"},
    {"cubyte", "CUByte"},
    {"cuint", "CUInt"},
    {"culng", "CULng"},
    {"culngint", "CULngInt"},
    {"cunsg", "CUnsg"},
    {"curdir", "CurDir"},
    {"cushort", "CUShort"},
    {"cva_arg", "cva_arg"},
    {"cva_copy", "cva_copy"},
    {"cva_end", "cva_end"},
    {"cva_start", "cva_start"},
    {"cvd", "CVD"},
    {"cvi", "CVI"},
    {"cvl", "CVL"},
    {"cvlongint", "CVLongInt"},
    {"cvs", "CVS"},
    {"cvshort", "CVShort"},
    {"dateadd", "DateAdd"},
    {"datediff", "DateDiff"},
    {"datepart", "DatePart"},
    {"dateserial", "DateSerial"},
    {"datevalue", "DateValue"},
    {"defbyte", "DefByte"},
    {"defdbl", "DefDbl"},
    {"defined", "defined"},
    {"defint", "DefInt"},
    {"deflng", "DefLng"},
    {"deflongint", "DefLongInt"},
    {"defshort", "DefShort"},
    {"defsng", "DefSng"},
    {"defstr", "DefStr"},
    {"defubyte", "DefUByte"},
    {"defuint", "DefUInt"},
    {"defulng", "DefULng"},
    {"defushort", "DefUShort"},
    {"draw", "Draw"},
    {"drawstring", "DrawString"},
    {"dylibfree", "DyLibFree"},
    {"dylibload", "DyLibLoad"},
    {"dylibsymbol", "DyLibSymbol"},
    {"elseif", "ElseIf"},
    {"endif", "EndIf"},
    {"endmacro", "EndMacro"},
    {"eof", "EOF"},
    {"exepath", "ExePath"},
    {"fileattr", "FileAttr"},
    {"filecopy", "FileCopy"},
    {"filedatetime", "FileDateTime"},
    {"fileexists", "FileExists"},
    {"fileflush", "FileFlush"},
    {"filelen", "FileLen"},
    {"fileseteof", "FileSetEof"},
    {"freefile", "FreeFile"},
    {"get", "Get"},
    {"getjoystick", "GetJoystick"},
    {"getkey", "GetKey"},
    {"getmouse", "GetMouse"},
    {"gosub", "GoSub"},
    {"hibyte", "HiByte"},
    {"hiword", "HiWord"},
    {"ifdef", "IfDef"},
    {"ifndef", "IfNotDef"},
    {"iif", "IIf"},
    {"imageconvertrow", "ImageConvertRow"},
    {"imagecreate", "ImageCreate"},
    {"imagedestroy", "ImageDestroy"},
    {"imageinfo", "ImageInfo"},
    {"inclib", "Inclib"},
    {"instr", "InStr"},
    {"instrrev", "InStrRev"},
    {"isdate", "IsDate"},
    {"isredirected", "IsRedirected"},
    {"lang", "Lang"},
    {"lbound", "LBound"},
    {"lcase", "LCase"},
    {"libpath", "LibPath"},
    {"line", "Line"},
    {"lobyte", "LoByte"},
    {"loc", "LOC"},
    {"lof", "LOF"},
    {"longint", "LongInt"},
    {"loword", "LoWord"},
    {"lpos", "LPos"},
    {"lprint", "LPrint"},
    {"lset", "LSet"},
    {"ltrim", "LTrim"},
    {"macro", "Macro"},
    {"mid", "Mid"},
    {"mkd", "MKD"},
    {"mkdir", "MkDir"},
    {"mki", "MKI"},
    {"mkl", "MKL"},
    {"mklongint", "MKLongInt"},
    {"mks", "MKS"},
    {"mkshort", "MKShort"},
    {"monthname", "MonthName"},
    {"multikey", "MultiKey"},
    {"mutexcreate", "MutexCreate"},
    {"mutexdestroy", "MutexDestroy"},
    {"mutexlock", "MutexLock"},
    {"mutexunlock", "MutexUnlock"},
    {"offsetof", "OffsetOf"},
    {"pascal", "pascal"},
    {"pcopy", "PCopy"},
    {"pmap", "PMap"},
    {"point", "Point"},
    {"pointcoord", "PointCoord"},
    {"pointer", "Pointer"},
    {"preset", "PReset"},
    {"print", "Print"},
    {"procptr", "ProcPtr"},
    {"pset", "PSet"},
    {"ptr", "Ptr"},
    {"put", "Put"},
    {"redim", "ReDim"},
    {"rgb", "RGB"},
    {"rgba", "RGBA"},
    {"rmdir", "RmDir"},
    {"rset", "RSet"},
    {"rtrim", "RTrim"},
    {"sadd", "SAdd"},
    {"screen", "Screen"},
    {"screencontrol", "ScreenControl"},
    {"screencopy", "ScreenCopy"},
    {"screenevent", "ScreenEvent"},
    {"screenglproc", "ScreenGLProc"},
    {"screeninfo", "ScreenInfo"},
    {"screenlist", "ScreenList"},
    {"screenlock", "ScreenLock"},
    {"screenptr", "ScreenPtr"},
    {"screenres", "ScreenRes"},
    {"screenset", "ScreenSet"},
    {"screensync", "ScreenSync"},
    {"screenunlock", "ScreenUnlock"},
    {"seek", "Seek"},
    {"setdate", "SetDate"},
    {"setenviron", "SetEnviron"},
    {"setmouse", "SetMouse"},
    {"settime", "SetTime"},
    {"sizeof", "SizeOf"},
    {"stdcall", "stdcall"},
    {"strptr", "StrPtr"},
    {"threadcall", "ThreadCall"},
    {"threadcreate", "ThreadCreate"},
    {"threaddetach", "ThreadDetach"},
    {"threadself", "ThreadSelf"},
    {"threadwait", "ThreadWait"},
    {"timeserial", "TimeSerial"},
    {"timevalue", "TimeValue"},
    {"typeof", "TypeOf"},
    {"ubound", "UBound"},
    {"ubyte", "UByte"},
    {"ucase", "UCase"},
    {"uinteger", "UInteger"},
    {"ulong", "ULong"},
    {"ulongint", "ULongInt"},
    {"undef", "Undef"},
    {"ushort", "UShort"},
    {"va_first", "va_first"},
    {"valint", "ValInt"},
    {"vallng", "ValLng"},
    {"valuint", "ValUInt"},
    {"valulng", "ValULng"},
    {"varptr", "VarPtr"},
    {"view", "View"},
    {"wbin", "WBin"},
    {"wchr", "WChr"},
    {"weekdayname", "WeekdayName"},
    {"whex", "WHex"},
    {"windowtitle", "WindowTitle"},
    {"winput", "Winput"},
    {"woct", "WOct"},
    {"wspace", "WSpace"},
    {"wstr", "WStr"},
    {"wstring", "WString"},
    {"zstring", "ZString"},
};

constexpr bool rowsSorted() {
  for (size_t i = 1; i < sizeof(kCanonical) / sizeof(kCanonical[0]); ++i) {
    std::string_view prev(kCanonical[i - 1].lower);
    std::string_view cur(kCanonical[i].lower);
    if (!(prev < cur)) {
      return false;
    }
  }
  return true;
}
static_assert(rowsSorted(), "kCanonical must be sorted by lower");

} // namespace

std::string_view canonicalSpelling(std::string_view wordLower) {
  size_t lo = 0;
  size_t hi = sizeof(kCanonical) / sizeof(kCanonical[0]);
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    std::string_view key(kCanonical[mid].lower);
    if (key < wordLower) {
      lo = mid + 1;
    } else if (wordLower < key) {
      hi = mid;
    } else {
      return kCanonical[mid].canonical;
    }
  }
  return "";
}

std::string titleCase(std::string_view word) {
  std::string out(word);
  for (char &c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  if (!out.empty() && out[0] >= 'a' && out[0] <= 'z') {
    out[0] = static_cast<char>(out[0] - 'a' + 'A');
  }
  return out;
}

} // namespace fbformat
