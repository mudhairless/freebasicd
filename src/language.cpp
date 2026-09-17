#include "language.h"
#include "symbols.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

// Length of the `$lang` metacommand keyword ('$' + "lang").
#define LANG_DIRECTIVE_LEN 5

namespace fblang {

namespace {

constexpr char const *kReserved[] = {
    "abs",
    "abstract",
    "access",
    "acos",
    "alias",
    "allocate",
    "and",
    "andalso",
    "any",
    "append",
    "as",
    "asc",
    "asin",
    "asm",
    "assert",
    "assertwarn",
    "atan2",
    "atn",
    "base",
    "beep",
    "bin",
    "binary",
    "bit",
    "bitreset",
    "bitset",
    "bload",
    "boolean",
    "bsave",
    "byref",
    "byte",
    "byval",
    "call",
    "callocate",
    "case",
    "cast",
    "cbool",
    "cbyte",
    "cdbl",
    "cdecl",
    "chain",
    "chdir",
    "chr",
    "cint",
    "circle",
    "class",
    "clear",
    "clng",
    "clngint",
    "close",
    "cls",
    "color",
    "command",
    "common",
    "condbroadcast",
    "condcreate",
    "conddestroy",
    "condsignal",
    "condwait",
    "const",
    "constructor",
    "continue",
    "cos",
    "cptr",
    "cshort",
    "csign",
    "csng",
    "csrlin",
    "cubyte",
    "cuint",
    "culng",
    "culngint",
    "cunsg",
    "curdir",
    "cushort",
    "cvd",
    "cvi",
    "cvl",
    "cvlongint",
    "cvs",
    "cvshort",
    "data",
    "date",
    "deallocate",
    "declare",
    "defbyte",
    "defdbl",
    "defined",
    "defint",
    "deflng",
    "deflongint",
    "defshort",
    "defsng",
    "defstr",
    "defubyte",
    "defuint",
    "defulongint",
    "defushort",
    "delete",
    "destructor",
    "dim",
    "dir",
    "do",
    "double",
    "draw",
    "dylibfree",
    "dylibload",
    "dylibsymbol",
    "else",
    "elseif",
    "encoding",
    "end",
    "endif",
    "enum",
    "environ",
    "eof",
    "eqv",
    "erase",
    "erfn",
    "erl",
    "ermn",
    "err",
    "error",
    "exec",
    "exepath",
    "exit",
    "exp",
    "export",
    "extends",
    "extern",
    "false",
    "field",
    "fix",
    "flip",
    "for",
    "frac",
    "fre",
    "freefile",
    "function",
    "get",
    "getjoystick",
    "getkey",
    "getmouse",
    "gosub",
    "goto",
    "hex",
    "hibyte",
    "hiword",
    "if",
    "iif",
    "imageconvertrow",
    "imagecreate",
    "imagedestroy",
    "imageinfo",
    "imp",
    "implements",
    "import",
    "inkey",
    "inp",
    "input",
    "instr",
    "instrrev",
    "int",
    "integer",
    "is",
    "kill",
    "lbound",
    "lcase",
    "left",
    "len",
    "let",
    "lib",
    "line",
    "lobyte",
    "loc",
    "local",
    "locate",
    "lock",
    "lof",
    "log",
    "long",
    "longint",
    "loop",
    "loword",
    "lpos",
    "lprint",
    "lset",
    "ltrim",
    "mid",
    "mkd",
    "mkdir",
    "mki",
    "mkl",
    "mklongint",
    "mks",
    "mkshort",
    "mod",
    "multikey",
    "mutexcreate",
    "mutexdestroy",
    "mutexlock",
    "mutexunlock",
    "name",
    "namespace",
    "new",
    "next",
    "not",
    "object",
    "oct",
    "offsetof",
    "open",
    "operator",
    "or",
    "orelse",
    "out",
    "output",
    "overload",
    "paint",
    "palette",
    "pascal",
    "pcopy",
    "peek",
    "pmap",
    "point",
    "pointcoord",
    "pointer",
    "poke",
    "pos",
    "preserve",
    "preset",
    "print",
    "private",
    "procptr",
    "property",
    "pset",
    "ptr",
    "public",
    "put",
    "random",
    "randomize",
    "read",
    "reallocate",
    "redim",
    "rem",
    "reset",
    "restore",
    "resume",
    "return",
    "rgb",
    "rgba",
    "right",
    "rmdir",
    "rnd",
    "rset",
    "rtrim",
    "run",
    "sadd",
    "scope",
    "screen",
    "screencontrol",
    "screencopy",
    "screenevent",
    "screenglproc",
    "screeninfo",
    "screenlist",
    "screenlock",
    "screenptr",
    "screenres",
    "screenset",
    "screensync",
    "screenunlock",
    "seek",
    "select",
    "setdate",
    "setenviron",
    "setmouse",
    "settime",
    "sgn",
    "shared",
    "shell",
    "shl",
    "short",
    "shr",
    "sin",
    "single",
    "sizeof",
    "sleep",
    "space",
    "spc",
    "sqr",
    "static",
    "stdcall",
    "step",
    "stop",
    "str",
    "string",
    "strptr",
    "sub",
    "swap",
    "system",
    "tab",
    "tan",
    "then",
    "threadcall",
    "threadcreate",
    "threadwait",
    "time",
    "timer",
    "to",
    "trim",
    "true",
    "type",
    "typeof",
    "ubound",
    "ubyte",
    "ucase",
    "uinteger",
    "ulong",
    "ulongint",
    "union",
    "unlock",
    "unsigned",
    "until",
    "ushort",
    "using",
    "val",
    "valint",
    "vallng",
    "valuint",
    "valulng",
    "var",
    "varptr",
    "view",
    "virtual",
    "wait",
    "wbin",
    "wchr",
    "wend",
    "whex",
    "while",
    "width",
    "window",
    "windowtitle",
    "winput",
    "with",
    "woct",
    "write",
    "wspace",
    "wstr",
    "wstring",
    "xor",
    "zstring",
};

constexpr char const *kBuiltinTypes[] = {
    "any",   "boolean",  "byte",   "double",  "integer",
    "long",  "longint",  "object", "pointer", "ptr",
    "short", "single",   "string", "ubyte",   "uinteger",
    "ulong", "ulongint", "ushort", "wstring", "zstring",
};

// Reserved words that are never legitimate identifiers start at this point;
// keep children under a cheap `int` table.
struct BlockRow {
  char const *opener; // lowercase opening word
  BlockKind kind;
  char const *close; // closer after "end" (needsEnd) or the bare closer
  bool needsEnd;
};

constexpr BlockRow kBlockOpeners[] = {
    {"sub", BlockKind::Sub, "sub", true},
    {"function", BlockKind::Function, "function", true},
    {"property", BlockKind::Property, "property", true},
    {"operator", BlockKind::Operator, "operator", true},
    {"constructor", BlockKind::Constructor, "constructor", true},
    {"destructor", BlockKind::Destructor, "destructor", true},
    {"type", BlockKind::Type, "type", true},
    {"union", BlockKind::Union, "union", true},
    {"enum", BlockKind::Enum, "enum", true},
    {"namespace", BlockKind::Namespace, "namespace", true},
    {"scope", BlockKind::Scope, "scope", true},
    {"if", BlockKind::If, "if", true},
    {"select", BlockKind::Select, "select", true},
    {"with", BlockKind::With, "with", true},
    {"extern", BlockKind::Extern, "extern", true},
    {"asm", BlockKind::Asm, "asm", true},
    {"for", BlockKind::For, "next", false},
    {"while", BlockKind::While, "wend", false},
    {"do", BlockKind::Do, "loop", false},
};

// Words that only ever appear as closers (never open a block).
constexpr BlockRow kCloserOnly[] = {
    {"next", BlockKind::For, "next", false},
    {"wend", BlockKind::While, "wend", false},
    {"loop", BlockKind::Do, "loop", false},
};

// FreeBASIC wiki page suffixes that do not match the naive `KeyPg<Word>` rule.
struct DocsPage {
  char const *word;
  char const *page;
};
constexpr DocsPage kDocsPages[] = {
    {"and", "OpAnd"},
    {"andalso", "OpAndAlso"},
    {"condbroadcast", "CondBroadcast"},
    {"condcreate", "CondCreate"},
    {"conddestroy", "CondDestroy"},
    {"condsignal", "CondSignal"},
    {"condwait", "CondWait"},
    {"delete", "OpDelete"},
    {"eqv", "OpEqv"},
    {"get", "Getfileio"},
    {"if", "Ifthen"},
    {"imageconvertrow", "ImageConvertRow"},
    {"imagedestroy", "ImageDestroy"},
    {"imageinfo", "ImageInfo"},
    {"imp", "OpImp"},
    {"line", "Linegraphics"},
    {"lobyte", "LoByte"},
    {"loword", "LoWord"},
    {"mid", "Midfunction"},
    {"mod", "OpModulus"},
    {"mutexcreate", "MutexCreate"},
    {"mutexdestroy", "MutexDestroy"},
    {"mutexlock", "MutexLock"},
    {"mutexunlock", "MutexUnlock"},
    {"new", "OpNew"},
    {"not", "OpNot"},
    {"or", "OpOr"},
    {"orelse", "OpOrElse"},
    {"pointcoord", "PointCoord"},
    {"pointer", "Ptr"},
    {"procptr", "OpProcptr"},
    {"put", "Putfileio"},
    {"screen", "Screengraphics"},
    {"seek", "Seekset"},
    {"select", "Selectcase"},
    {"shl", "OpShiftLeft"},
    {"shr", "OpShiftRight"},
    {"strptr", "OpStrptr"},
    {"threadcall", "ThreadCall"},
    {"threadcreate", "ThreadCreate"},
    {"threadwait", "ThreadWait"},
    {"varptr", "OpVarptr"},
    {"view", "Viewgraphics"},
    {"xor", "OpXor"},
};

// Intrinsic catalog: one row per base name, key-sorted. Function rows carry a
// canonical call signature; Statement rows their usage syntax. `page` is the
// KeyPg suffix (the URL is built at the language boundary).
constexpr Intrinsic kIntrinsics[] = {
    {"abs", false, IntrinsicKind::Function,
     "Abs( number As Integer ) As Integer", "Abs"},
    {"acos", false, IntrinsicKind::Function,
     "Acos( number As Double ) As Double", "Acos"},
    {"allocate", false, IntrinsicKind::Function,
     "Allocate( count As Uinteger ) As Any Ptr", "Allocate"},
    {"arraylen", false, IntrinsicKind::Function,
     "Arraylen( arrayname() As Any ) As Uinteger", "ArrayLen"},
    {"arraysize", false, IntrinsicKind::Function,
     "Arraysize( arrayname() As Any ) As Uinteger", "ArraySize"},
    {"asc", false, IntrinsicKind::Function,
     "Asc( str As String, position As Integer ) As Ulong", "Asc"},
    {"asin", false, IntrinsicKind::Function,
     "Asin( number As Double ) As Double", "Asin"},
    {"assert", false, IntrinsicKind::Statement, "Assert expression", "Assert"},
    {"assertwarn", false, IntrinsicKind::Statement, "Assertwarn expression",
     "Assertwarn"},
    {"atan2", false, IntrinsicKind::Function,
     "Atan2( y As Double, x As Double ) As Double", "Atan2"},
    {"atn", false, IntrinsicKind::Function, "Atn( number As Double ) As Double",
     "Atn"},
    {"beep", false, IntrinsicKind::Statement, "Beep()", "Beep"},
    {"bin", true, IntrinsicKind::Function, "Bin$( number As Ubyte ) As String",
     "Bin"},
    {"bit", false, IntrinsicKind::Function,
     "Bit( value As Integer, bit_number As Integer ) As Integer", "Bit"},
    {"bitreset", false, IntrinsicKind::Function,
     "Bitreset( value As Integer, bit_number As Integer ) As Integer",
     "Bitreset"},
    {"bitset", false, IntrinsicKind::Function,
     "Bitset( value As Integer, bit_number As Integer ) As Integer", "Bitset"},
    {"bload", false, IntrinsicKind::Statement,
     "Bload( filename As String, dest As Any Ptr, pal As Any Ptr )", "Bload"},
    {"bsave", false, IntrinsicKind::Statement,
     "Bsave( filename As String, source As Any Ptr, size As Ulong, pal As Any "
     "Ptr, bitsperpixel As Long )",
     "Bsave"},
    {"callocate", false, IntrinsicKind::Function,
     "Callocate( num_elements As Uinteger, size As Uinteger ) As Any Ptr",
     "Callocate"},
    {"cast", false, IntrinsicKind::Function,
     "Cast( datatype, expression ) As datatype", "Cast"},
    {"cbool", false, IntrinsicKind::Function, "Cbool( expression ) As Boolean",
     "Cbool"},
    {"cbyte", false, IntrinsicKind::Function, "Cbyte( expression ) As Byte",
     "Cbyte"},
    {"cdbl", false, IntrinsicKind::Function, "Cdbl( expression ) As Double",
     "Cdbl"},
    {"chain", false, IntrinsicKind::Statement, "Chain( program As String )",
     "Chain"},
    {"chdir", false, IntrinsicKind::Statement, "Chdir( path As String )",
     "Chdir"},
    {"chr", true, IntrinsicKind::Function,
     "Chr$( ch As Integer, ... ) As String", "Chr"},
    {"cint", false, IntrinsicKind::Function, "Cint( expression ) As Integer",
     "Cint"},
    {"circle", false, IntrinsicKind::Statement,
     "Circle [ Step ] ( x, y ), radius [, color [, start, end [, aspect [, "
     "F]]]]",
     "Circle"},
    {"clear", false, IntrinsicKind::Statement,
     "Clear( dst As Any, value As Long, bytes As Uinteger )", "Clear"},
    {"clng", false, IntrinsicKind::Function, "Clng( expression ) As Long",
     "Clng"},
    {"clngint", false, IntrinsicKind::Function,
     "Clngint( expression ) As Longint", "Clngint"},
    {"close", false, IntrinsicKind::Statement, "Close [ #filenum ]", "Close"},
    {"cls", false, IntrinsicKind::Statement, "Cls( mode As Long )", "Cls"},
    {"color", false, IntrinsicKind::Statement,
     "Color( foreground As Ulong, background As Ulong )", "Color"},
    {"command", true, IntrinsicKind::Function,
     "Command$( index As Long ) As String", "Command"},
    {"condbroadcast", false, IntrinsicKind::Function,
     "Condbroadcast( handle As Any Ptr )", "CondBroadcast"},
    {"condcreate", false, IntrinsicKind::Function, "Condcreate() As Any Ptr",
     "CondCreate"},
    {"conddestroy", false, IntrinsicKind::Function,
     "Conddestroy( handle As Any Ptr )", "CondDestroy"},
    {"condsignal", false, IntrinsicKind::Function,
     "Condsignal( handle As Any Ptr )", "CondSignal"},
    {"condwait", false, IntrinsicKind::Function,
     "Condwait( handle As Any Ptr, mutex As Any Ptr )", "CondWait"},
    {"cos", false, IntrinsicKind::Function, "Cos( angle As Double ) As Double",
     "Cos"},
    {"cptr", false, IntrinsicKind::Function,
     "Cptr( datatype, expression ) As datatype", "Cptr"},
    {"cshort", false, IntrinsicKind::Function, "Cshort( expression ) As Short",
     "Cshort"},
    {"csign", false, IntrinsicKind::Function,
     "Csign( expression As datatype ) As datatype", "Csign"},
    {"csng", false, IntrinsicKind::Function, "Csng( expression ) As Single",
     "Csng"},
    {"csrlin", false, IntrinsicKind::Function, "Csrlin() As Long", "Csrlin"},
    {"cubyte", false, IntrinsicKind::Function, "Cubyte( expression ) As Ubyte",
     "Cubyte"},
    {"cuint", false, IntrinsicKind::Function, "Cuint( expression ) As Uinteger",
     "Cuint"},
    {"culng", false, IntrinsicKind::Function, "Culng( expression ) As Ulong",
     "Culng"},
    {"culngint", false, IntrinsicKind::Function,
     "Culngint( expression ) As Ulongint", "Culngint"},
    {"cunsg", false, IntrinsicKind::Function,
     "Cunsg( expression As datatype ) As datatype", "Cunsg"},
    {"curdir", false, IntrinsicKind::Function, "Curdir() As String", "Curdir"},
    {"cushort", false, IntrinsicKind::Function,
     "Cushort( expression ) As Ushort", "Cushort"},
    {"cvd", false, IntrinsicKind::Function, "Cvd( l As Longint ) As Double",
     "Cvd"},
    {"cvi", false, IntrinsicKind::Function, "Cvi( n As Integer ) As Single",
     "Cvi"},
    {"cvl", false, IntrinsicKind::Function, "Cvl( sng As Single ) As Long",
     "Cvl"},
    {"cvlongint", false, IntrinsicKind::Function,
     "Cvlongint( dbl As Double ) As Longint", "Cvlongint"},
    {"cvs", false, IntrinsicKind::Function, "Cvs( I As Integer ) As Single",
     "Cvs"},
    {"cvshort", false, IntrinsicKind::Function,
     "Cvshort( str As String ) As Short", "Cvshort"},
    {"date", true, IntrinsicKind::Function, "Date$() As String", "Date"},
    {"dateadd", false, IntrinsicKind::Function,
     "Dateadd( interval As String, number As Double, date_serial As Double ) "
     "As Double",
     "DateAdd"},
    {"datediff", false, IntrinsicKind::Function,
     "Datediff( interval As String, serial1 As Double, serial2 As Double, "
     "firstdayofweek As Long, firstdayofyear As Long ) As Longint",
     "DateDiff"},
    {"datepart", false, IntrinsicKind::Function,
     "Datepart( interval As String, date_serial As Double, firstdayofweek As "
     "Long, firstdayofyear As Long ) As Long",
     "DatePart"},
    {"dateserial", false, IntrinsicKind::Function,
     "Dateserial( year As Long, month As Long, day As Long ) As Long",
     "DateSerial"},
    {"datevalue", false, IntrinsicKind::Function,
     "Datevalue( date_String As String ) As Long", "DateValue"},
    {"day", false, IntrinsicKind::Function,
     "Day( date_serial As Double ) As Long", "Day"},
    {"deallocate", false, IntrinsicKind::Statement,
     "Deallocate( Pointer As Any Ptr )", "Deallocate"},
    {"dir", false, IntrinsicKind::Function,
     "Dir( item_spec [, attrib_mask ] [, out_attrib ] ) As String", "Dir"},
    {"draw", false, IntrinsicKind::Statement, "Draw stringexpression", "Draw"},
    {"drawstring", false, IntrinsicKind::Statement,
     "Draw String [ buffer, ] [ Step ] ( x, y ), text [, color ]",
     "DrawString"},
    {"environ", true, IntrinsicKind::Function,
     "Environ$( varname As String ) As String", "Environ"},
    {"eof", false, IntrinsicKind::Function, "Eof( filenum As Long ) As Long",
     "Eof"},
    {"erase", false, IntrinsicKind::Statement, "Erase( array As Any, ... )",
     "Erase"},
    {"erfn", false, IntrinsicKind::Function, "Erfn() As Zstring Ptr", "Erfn"},
    {"erl", false, IntrinsicKind::Function, "Erl() As Long", "Erl"},
    {"ermn", false, IntrinsicKind::Function, "Ermn() As Zstring Ptr", "Ermn"},
    {"err", false, IntrinsicKind::Function, "Err() As Integer", "Err"},
    {"error", false, IntrinsicKind::Statement, "Error( errno As Long )",
     "Error"},
    {"exec", false, IntrinsicKind::Statement,
     "Exec( program As String, arguments As String )", "Exec"},
    {"exepath", false, IntrinsicKind::Function, "Exepath() As String",
     "Exepath"},
    {"exp", false, IntrinsicKind::Function, "Exp( number As Double ) As Double",
     "Exp"},
    {"fileattr", false, IntrinsicKind::Function,
     "Fileattr( filenum As Long, returntype As Long ) As Integer", "Fileattr"},
    {"filecopy", false, IntrinsicKind::Statement,
     "Filecopy( source As Zstring Ptr, destination As Zstring Ptr )",
     "Filecopy"},
    {"filedatetime", false, IntrinsicKind::Function,
     "Filedatetime( filename As Zstring Ptr ) As Double", "Filedatetime"},
    {"fileexists", false, IntrinsicKind::Function,
     "Fileexists( filename As Zstring Ptr ) As Long", "Fileexists"},
    {"fileflush", false, IntrinsicKind::Statement,
     "Fileflush( filenum As Long, systembuffers As Long )", "Fileflush"},
    {"filelen", false, IntrinsicKind::Function,
     "Filelen( filename As String ) As Longint", "Filelen"},
    {"fileseteof", false, IntrinsicKind::Statement,
     "Fileseteof( filenum As Long )", "Fileseteof"},
    {"fix", false, IntrinsicKind::Function, "Fix( number As Single ) As Single",
     "Fix"},
    {"flip", false, IntrinsicKind::Statement,
     "Flip( frompage As Long, topage As Long )", "Flip"},
    {"format", true, IntrinsicKind::Function,
     "Format$( numerical_expression As Double, formatting_expression As String "
     ") As String",
     "Format"},
    {"frac", false, IntrinsicKind::Function,
     "Frac( number As Double ) As Double", "Frac"},
    {"fre", false, IntrinsicKind::Statement, "Fre( value As Long )", "Fre"},
    {"freefile", false, IntrinsicKind::Function, "Freefile() As Long",
     "Freefile"},
    {"get", false, IntrinsicKind::Statement,
     "Get #filenum, position, data [, amount [, bytesread ]]", "Getfileio"},
    {"getjoystick", false, IntrinsicKind::Function,
     "Getjoystick( id As Long, buttons As Integer, a1 As Single, a2 As Single, "
     "a3 As Single, a4 As Single, a5 As Single, a6 As Single, a7 As Single, a8 "
     "As Single ) As Long",
     "Getjoystick"},
    {"getkey", false, IntrinsicKind::Function, "Getkey() As Long", "Getkey"},
    {"getmouse", false, IntrinsicKind::Statement,
     "Getmouse( x As Long, y As Long, wheel As Long, buttons As Long, clip As "
     "Long )",
     "Getmouse"},
    {"hex", true, IntrinsicKind::Function, "Hex$( number As Ubyte ) As String",
     "Hex"},
    {"hibyte", false, IntrinsicKind::Function,
     "Hibyte( expr As Uinteger ) As Ubyte", "Hibyte"},
    {"hiword", false, IntrinsicKind::Function,
     "Hiword( expr As Uinteger ) As Ushort", "Hiword"},
    {"hour", false, IntrinsicKind::Function,
     "Hour( date_serial As Double ) As Long", "Hour"},
    {"imageconvertrow", false, IntrinsicKind::Statement,
     "Imageconvertrow( src, src_bpp, dst, dst_bpp, width [, isrgb ] )",
     "ImageConvertRow"},
    {"imagecreate", false, IntrinsicKind::Function,
     "Imagecreate( width, height [, color ] ) As Any Ptr", "Imagecreate"},
    {"imagedestroy", false, IntrinsicKind::Statement,
     "Imagedestroy( image As Any Ptr )", "ImageDestroy"},
    {"imageinfo", false, IntrinsicKind::Function,
     "Imageinfo( image As Any Ptr, width As Long, height As Long, bypp As "
     "Long, pitch As Long, pixdata As Any Ptr, size As Long ) As Long",
     "ImageInfo"},
    {"inkey", true, IntrinsicKind::Function, "Inkey$() As String", "Inkey"},
    {"input", true, IntrinsicKind::Function, "Input$( n As Integer ) As String",
     "Inputnum"},
    {"instr", false, IntrinsicKind::Function,
     "Instr( str As String, substring As String ) As Integer", "Instr"},
    {"instrrev", false, IntrinsicKind::Function,
     "Instrrev( str As String, substring As String, start As Integer ) As "
     "Integer",
     "Instrrev"},
    {"int", false, IntrinsicKind::Function, "Int( number As Single ) As Single",
     "Int"},
    {"isdate", false, IntrinsicKind::Function,
     "Isdate( stringdate As String ) As Long", "IsDate"},
    {"isredirected", false, IntrinsicKind::Function,
     "Isredirected( is_input As Long ) As Long", "Isredirected"},
    {"kill", false, IntrinsicKind::Statement, "Kill( filename As String )",
     "Kill"},
    {"lbound", false, IntrinsicKind::Function,
     "Lbound( array() As Any, dimension As Integer ) As Integer", "Lbound"},
    {"lcase", true, IntrinsicKind::Function,
     "Lcase$( str As String, mode As Long ) As String", "Lcase"},
    {"left", true, IntrinsicKind::Function,
     "Left$( str As String, n As Integer ) As String", "Left"},
    {"len", false, IntrinsicKind::Function,
     "Len( expression As String ) As Integer", "Len"},
    {"line", false, IntrinsicKind::Statement,
     "Line [ Step ] ( x1, y1 )-[ Step ] ( x2, y2 ) [, color [, B | BF [, style "
     "]]]",
     "Linegraphics"},
    {"lobyte", false, IntrinsicKind::Function,
     "Lobyte( expr As Uinteger ) As Ubyte", "LoByte"},
    {"loc", false, IntrinsicKind::Function, "Loc( filenum As Long ) As Longint",
     "Loc"},
    {"locate", false, IntrinsicKind::Statement,
     "Locate( row As Long, column As Long, state As Long, start As Long, stop "
     "As Long )",
     "Locate"},
    {"lock", false, IntrinsicKind::Statement, "Lock #filenum, [ start ] To end",
     "Lock"},
    {"lof", false, IntrinsicKind::Function, "Lof( filenum As Long ) As Longint",
     "Lof"},
    {"log", false, IntrinsicKind::Function, "Log( number As Double ) As Double",
     "Log"},
    {"loword", false, IntrinsicKind::Function,
     "Loword( expr As Uinteger ) As Ushort", "LoWord"},
    {"lset", false, IntrinsicKind::Statement,
     "Lset( dst As String, src As String )", "Lset"},
    {"ltrim", true, IntrinsicKind::Function,
     "Ltrim$( str As String, trimset As String ) As String", "Ltrim"},
    {"mid", true, IntrinsicKind::Function,
     "Mid$( str As String, start As Integer ) As String", "Midfunction"},
    {"minute", false, IntrinsicKind::Function,
     "Minute( date_serial As Double ) As Long", "Minute"},
    {"mkd", true, IntrinsicKind::Function, "Mkd$( number As Double ) As String",
     "Mkd"},
    {"mkdir", false, IntrinsicKind::Statement, "Mkdir( folder As String )",
     "Mkdir"},
    {"mki", true, IntrinsicKind::Function,
     "Mki$( number As Integer ) As String", "Mki"},
    {"mkl", true, IntrinsicKind::Function, "Mkl$( number As Long ) As String",
     "Mkl"},
    {"mklongint", true, IntrinsicKind::Function,
     "Mklongint$( number As Longint ) As String", "Mklongint"},
    {"mks", true, IntrinsicKind::Function, "Mks$( number As Single ) As String",
     "Mks"},
    {"mkshort", true, IntrinsicKind::Function,
     "Mkshort$( number As Short ) As String", "Mkshort"},
    {"month", false, IntrinsicKind::Function,
     "Month( date_serial As Double ) As Long", "Month"},
    {"monthname", false, IntrinsicKind::Function,
     "Monthname( month As Long, abbreviate As Long ) As String", "Monthname"},
    {"multikey", false, IntrinsicKind::Function,
     "Multikey( scancode As Long ) As Long", "Multikey"},
    {"mutexcreate", false, IntrinsicKind::Function, "Mutexcreate() As Any Ptr",
     "MutexCreate"},
    {"mutexdestroy", false, IntrinsicKind::Statement,
     "Mutexdestroy( id As Any Ptr )", "MutexDestroy"},
    {"mutexlock", false, IntrinsicKind::Statement, "Mutexlock( id As Any Ptr )",
     "MutexLock"},
    {"mutexunlock", false, IntrinsicKind::Statement,
     "Mutexunlock( id As Any Ptr )", "MutexUnlock"},
    {"name", false, IntrinsicKind::Statement,
     "Name( oldname As String, newname As String )", "Name"},
    {"now", false, IntrinsicKind::Function, "Now() As Double", "Now"},
    {"oct", true, IntrinsicKind::Function, "Oct$( number As Ubyte ) As String",
     "Oct"},
    {"offsetof", false, IntrinsicKind::Statement,
     "Offsetof( typename, fieldname ) As Integer", "Offsetof"},
    {"paint", false, IntrinsicKind::Statement,
     "Paint [ Step ] ( x, y ) [, color [, bordercolor ]]", "Paint"},
    {"palette", false, IntrinsicKind::Statement,
     "Palette [ Get | Using ] [ index, color ]", "Palette"},
    {"pcopy", false, IntrinsicKind::Statement,
     "Pcopy( source As Long, destination As Long )", "Pcopy"},
    {"peek", false, IntrinsicKind::Function,
     "Peek( address As Any Ptr ) As Ubyte", "Peek"},
    {"pmap", false, IntrinsicKind::Function,
     "Pmap( coord As Single, func As Long ) As Single", "Pmap"},
    {"point", false, IntrinsicKind::Function, "Point( x, y ) As Ulong",
     "Point"},
    {"pointcoord", false, IntrinsicKind::Function,
     "Pointcoord( func As Long ) As Single", "PointCoord"},
    {"poke", false, IntrinsicKind::Statement,
     "Poke( address As Any Ptr, value As Ubyte )", "Poke"},
    {"pos", false, IntrinsicKind::Function, "Pos() As Long", "Pos"},
    {"preset", false, IntrinsicKind::Statement,
     "Preset [ Step ] ( x, y ) [, color ]", "Preset"},
    {"print", false, IntrinsicKind::Statement,
     "Print [ #filenum, ] [ expressionlist ] [; | ,]", "Print"},
    {"procptr", false, IntrinsicKind::Function,
     "Procptr( procname ) As Any Ptr", "OpProcptr"},
    {"pset", false, IntrinsicKind::Statement,
     "Pset [ Step ] ( x, y ) [, color ]", "Pset"},
    {"put", false, IntrinsicKind::Statement,
     "Put #filenum, position, data [, amount ]", "Putfileio"},
    {"randomize", false, IntrinsicKind::Statement,
     "Randomize( seed As Double, algorithm As Long )", "Randomize"},
    {"reallocate", false, IntrinsicKind::Function,
     "Reallocate( Pointer As Any Ptr, count As Uinteger ) As Any Ptr",
     "Reallocate"},
    {"redim", false, IntrinsicKind::Statement, "Redim array( bounds ) As Type",
     "Redim"},
    {"reset", false, IntrinsicKind::Statement, "Reset()", "Reset"},
    {"resume", false, IntrinsicKind::Statement, "Resume [ Next ]", "Resume"},
    {"rgb", false, IntrinsicKind::Function, "Rgb( red, green, blue ) As Ulong",
     "Rgb"},
    {"rgba", false, IntrinsicKind::Function,
     "Rgba( red, green, blue, alpha ) As Ulong", "Rgba"},
    {"right", true, IntrinsicKind::Function,
     "Right$( str As String, n As Integer ) As String", "Right"},
    {"rmdir", false, IntrinsicKind::Statement, "Rmdir( folder As String )",
     "Rmdir"},
    {"rnd", false, IntrinsicKind::Function, "Rnd( seed As Single ) As Double",
     "Rnd"},
    {"rset", false, IntrinsicKind::Statement,
     "Rset( dst As String, src As String )", "Rset"},
    {"rtrim", true, IntrinsicKind::Function,
     "Rtrim$( str As String, trimset As String ) As String", "Rtrim"},
    {"run", false, IntrinsicKind::Statement,
     "Run( program As String, arguments As String )", "Run"},
    {"sadd", false, IntrinsicKind::Function,
     "Sadd( str As String ) As Zstring Ptr", "Sadd"},
    {"screen", false, IntrinsicKind::Statement,
     "Screen mode [, [ depth ] [, [ num_pages ] [, [ flags ]]]]",
     "Screengraphics"},
    {"screencontrol", false, IntrinsicKind::Statement,
     "Screencontrol( what [, param1 [, param2 [, param3 [, param4 ]]]] )",
     "Screencontrol"},
    {"screencopy", false, IntrinsicKind::Function,
     "Screencopy( from_page As Long, to_page As Long ) As Long", "Screencopy"},
    {"screenevent", false, IntrinsicKind::Function,
     "Screenevent( event As Any Ptr ) As Long", "Screenevent"},
    {"screenglproc", false, IntrinsicKind::Statement,
     "Screenglproc( procname As String )", "Screenglproc"},
    {"screeninfo", false, IntrinsicKind::Statement,
     "Screeninfo( [ w ] [, h ] [, depth ] [, bpp ] [, pitch ] [, rate ] [, "
     "driver ] )",
     "Screeninfo"},
    {"screenlist", false, IntrinsicKind::Function,
     "Screenlist( depth As Long ) As Long", "Screenlist"},
    {"screenlock", false, IntrinsicKind::Statement, "Screenlock()",
     "Screenlock"},
    {"screenptr", false, IntrinsicKind::Function, "Screenptr() As Any Ptr",
     "Screenptr"},
    {"screenres", false, IntrinsicKind::Statement,
     "Screenres( width As Long, height As Long, depth As Long, num_pages As "
     "Long, flags As Long, refresh_rate As Long )",
     "Screenres"},
    {"screenset", false, IntrinsicKind::Statement,
     "Screenset( work_page As Long, visible_page As Long )", "Screenset"},
    {"screensync", false, IntrinsicKind::Statement, "Screensync()",
     "Screensync"},
    {"screenunlock", false, IntrinsicKind::Statement,
     "Screenunlock( startline As Long, endline As Long )", "Screenunlock"},
    {"second", false, IntrinsicKind::Function,
     "Second( date_serial As Double ) As Long", "Second"},
    {"seek", false, IntrinsicKind::Statement, "Seek #filenum, position",
     "Seekset"},
    {"setdate", false, IntrinsicKind::Statement, "Setdate( newdate As String )",
     "Setdate"},
    {"setenviron", false, IntrinsicKind::Statement,
     "Setenviron( varexpression As String )", "Setenviron"},
    {"setmouse", false, IntrinsicKind::Statement,
     "Setmouse( x As Long, y As Long, visibility As Long, clip As Long )",
     "Setmouse"},
    {"settime", false, IntrinsicKind::Statement, "Settime( newtime As String )",
     "Settime"},
    {"sgn", false, IntrinsicKind::Function, "Sgn( number )", "Sgn"},
    {"shell", false, IntrinsicKind::Statement, "Shell( command As String )",
     "Shell"},
    {"sin", false, IntrinsicKind::Function, "Sin( angle As Double ) As Double",
     "Sin"},
    {"sizeof", false, IntrinsicKind::Statement,
     "Sizeof( datatype | variable ) As Integer", "Sizeof"},
    {"space", true, IntrinsicKind::Function,
     "Space$( count As Integer ) As String", "Space"},
    {"spc", false, IntrinsicKind::Function, "Spc( n As Integer ) As String",
     "Spc"},
    {"sqr", false, IntrinsicKind::Function, "Sqr( number As Double ) As Double",
     "Sqr"},
    {"stick", false, IntrinsicKind::Function, "Stick( axis As Long ) As Long",
     "Stick"},
    {"stop", false, IntrinsicKind::Statement, "Stop( retval As Long )", "Stop"},
    {"str", true, IntrinsicKind::Function, "Str$( n As Byte ) As String",
     "Str"},
    {"strig", false, IntrinsicKind::Function, "Strig( button As Long ) As Long",
     "Strig"},
    {"string", true, IntrinsicKind::Function,
     "String$( count As Integer, ch_code As Long ) As String",
     "StringFunction"},
    {"strptr", false, IntrinsicKind::Function, "Strptr( lhs ) As Any Ptr",
     "OpStrptr"},
    {"swap", false, IntrinsicKind::Statement, "Swap( a As Any, b As Any )",
     "Swap"},
    {"system", false, IntrinsicKind::Statement, "System( retval As Long )",
     "System"},
    {"tab", false, IntrinsicKind::Function,
     "Tab( column As Integer ) As String", "Tab"},
    {"tan", false, IntrinsicKind::Function, "Tan( angle As Double ) As Double",
     "Tan"},
    {"threadcall", false, IntrinsicKind::Function,
     "Threadcall subname([ paramlist ]) As Any Ptr", "ThreadCall"},
    {"threadcreate", false, IntrinsicKind::Function,
     "Threadcreate( subname [, paramlist ] ) As Any Ptr", "ThreadCreate"},
    {"threaddetach", false, IntrinsicKind::Statement,
     "Threaddetach( id As Any Ptr )", "ThreadDetach"},
    {"threadself", false, IntrinsicKind::Function, "Threadself() As Any Ptr",
     "ThreadSelf"},
    {"threadwait", false, IntrinsicKind::Statement,
     "Threadwait( id As Any Ptr )", "ThreadWait"},
    {"time", true, IntrinsicKind::Function, "Time$() As String", "Time"},
    {"timer", false, IntrinsicKind::Function, "Timer() As Double", "Timer"},
    {"timeserial", false, IntrinsicKind::Function,
     "Timeserial( hour As Long, minute As Long, second As Long ) As Double",
     "Timeserial"},
    {"timevalue", false, IntrinsicKind::Function,
     "Timevalue( timestring As String ) As Double", "TimeValue"},
    {"trim", true, IntrinsicKind::Function,
     "Trim$( str As String, trimset As String ) As String", "Trim"},
    {"typeof", false, IntrinsicKind::Statement, "Typeof( expression )",
     "Typeof"},
    {"ubound", false, IntrinsicKind::Function,
     "Ubound( array() As Any, dimension As Integer ) As Integer", "Ubound"},
    {"ucase", true, IntrinsicKind::Function,
     "Ucase$( str As String, mode As Long ) As String", "Ucase"},
    {"unlock", false, IntrinsicKind::Statement,
     "Unlock #filenum, [ start ] To end", "Unlock"},
    {"val", false, IntrinsicKind::Function, "Val( str As String ) As Double",
     "Val"},
    {"valint", false, IntrinsicKind::Function,
     "Valint( strnum As String ) As Long", "Valint"},
    {"vallng", false, IntrinsicKind::Function,
     "Vallng( strnum As String ) As Longint", "Vallng"},
    {"valuint", false, IntrinsicKind::Function,
     "Valuint( strnum As String ) As Ulong", "Valuint"},
    {"valulng", false, IntrinsicKind::Function,
     "Valulng( strnum As String ) As Ulongint", "Valulng"},
    {"varptr", false, IntrinsicKind::Function, "Varptr( variable ) As Any Ptr",
     "OpVarptr"},
    {"view", false, IntrinsicKind::Statement,
     "View [ Screen ] ( x1, y1 )-( x2, y2 ) [, color [, border ]]",
     "Viewgraphics"},
    {"wbin", false, IntrinsicKind::Function,
     "Wbin( number As Ubyte ) As Wstring", "Wbin"},
    {"wchr", false, IntrinsicKind::Function,
     "Wchr( ch As Integer, ... ) As Wstring", "Wchr"},
    {"weekday", false, IntrinsicKind::Function,
     "Weekday( serial As Double, firstdayofweek As Long ) As Long", "Weekday"},
    {"weekdayname", false, IntrinsicKind::Function,
     "Weekdayname( weekday As Long, abbreviate As Long, firstdayofweek As Long "
     ") As String",
     "Weekdayname"},
    {"whex", false, IntrinsicKind::Function,
     "Whex( number As Ubyte ) As Wstring", "Whex"},
    {"width", false, IntrinsicKind::Statement,
     "Width [ #filenum, ] columns [, rows ]", "Width"},
    {"window", false, IntrinsicKind::Statement,
     "Window [ Screen ] ( x1, y1 )-( x2, y2 )", "Window"},
    {"windowtitle", false, IntrinsicKind::Statement,
     "Windowtitle( title As String )", "Windowtitle"},
    {"woct", false, IntrinsicKind::Function,
     "Woct( number As Ubyte ) As Wstring", "Woct"},
    {"write", false, IntrinsicKind::Statement,
     "Write [ #filenum, ] expressionlist", "Write"},
    {"wspace", false, IntrinsicKind::Function,
     "Wspace( count As Integer ) As Wstring", "Wspace"},
    {"wstr", false, IntrinsicKind::Function, "Wstr( n As Byte ) As Wstring",
     "Wstr"},
    {"wstring", false, IntrinsicKind::Function,
     "Wstring( count As Integer, ch_code As Long ) As Wstring",
     "WstringFunction"},
    {"year", false, IntrinsicKind::Function,
     "Year( date_serial As Double ) As Long", "Year"},
};

constexpr bool intrinsicsSorted() {
  for (std::size_t i = 1; i < std::size(kIntrinsics); ++i) {
    if (!(kIntrinsics[i - 1].key < kIntrinsics[i].key)) {
      return false;
    }
  }
  return true;
}
static_assert(intrinsicsSorted(), "kIntrinsics must be sorted by key");

} // namespace

bool isReservedWord(std::string_view word) {
  // Reserve suffixes never apply to keywords; look the bare word up.
  return std::binary_search(std::begin(kReserved), std::end(kReserved),
                            std::string(word));
}

std::vector<std::string_view> reservedWords() {
  std::vector<std::string_view> out;
  out.reserve(std::size(kReserved));
  for (char const *w : kReserved) {
    out.emplace_back(w);
  }
  return out;
}

std::string keywordDocsUrl(std::string_view word) {
  std::string const lower = toLowerChars(word);
  if (!isReservedWord(lower)) {
    return {};
  }
  auto const *it = std::partition_point(
      std::begin(kDocsPages), std::end(kDocsPages),
      [&](DocsPage const &d) { return std::string_view(d.word) < lower; });
  std::string page;
  if (it != std::end(kDocsPages) && it->word == lower) {
    page = it->page;
  } else {
    page = lower;
    page[0] =
        static_cast<char>(std::toupper(static_cast<unsigned char>(lower[0])));
  }
  return "https://www.freebasic.net/wiki/KeyPg" + page;
}

bool isBuiltinType(std::string_view wordLower) {
  return std::any_of(std::begin(kBuiltinTypes), std::end(kBuiltinTypes),
                     [&](char const *t) { return wordLower == t; });
}

namespace {

bool isIdentStart(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool isIdentChar(char c) { return isIdentStart(c) || (c >= '0' && c <= '9'); }

} // namespace

Intrinsic const *intrinsicFor(std::string_view nameWithSuffix) {
  std::string key = toLowerChars(nameWithSuffix);
  if (!key.empty() && isSuffixChar(key.back())) {
    key.pop_back();
  }
  if (key.empty()) {
    return nullptr;
  }
  auto const *it =
      std::partition_point(std::begin(kIntrinsics), std::end(kIntrinsics),
                           [&](Intrinsic const &i) { return i.key < key; });
  if (it != std::end(kIntrinsics) && it->key == key) {
    return it;
  }
  return nullptr;
}

std::vector<Intrinsic const *> intrinsics() {
  std::vector<Intrinsic const *> out;
  out.reserve(std::size(kIntrinsics));
  for (Intrinsic const &i : kIntrinsics) {
    out.push_back(&i);
  }
  return out;
}

std::string intrinsicDocsUrl(Intrinsic const &fn) {
  if (!fn.page.empty()) {
    return "https://www.freebasic.net/wiki/KeyPg" + std::string(fn.page);
  }
  return keywordDocsUrl(fn.key);
}

std::vector<std::string_view> signatureParamLabels(Intrinsic const &fn) {
  std::vector<std::string_view> out;
  std::string_view const signature = fn.signature;
  std::size_t const open = signature.find('(');
  if (open == std::string_view::npos) {
    return out;
  }
  std::size_t const close = signature.rfind(')');
  if (close == std::string_view::npos || close <= open) {
    return out;
  }
  std::string_view const inner = signature.substr(open + 1, close - open - 1);
  std::size_t start = 0;
  while (start < inner.size()) {
    std::size_t const comma = inner.find(',', start);
    std::string_view piece = inner.substr(start, comma == std::string_view::npos
                                                     ? std::string_view::npos
                                                     : comma - start);
    while (!piece.empty() && piece.front() == ' ') {
      piece.remove_prefix(1);
    }
    while (!piece.empty() && piece.back() == ' ') {
      piece.remove_suffix(1);
    }
    if (!piece.empty()) {
      if (piece.find("...") != std::string_view::npos) {
        out.emplace_back("...");
      } else {
        std::size_t b = 0;
        while (b < piece.size() && !isIdentStart(piece[b])) {
          ++b;
        }
        std::size_t e = b;
        while (e < piece.size() && isIdentChar(piece[e])) {
          ++e;
        }
        out.emplace_back(piece.substr(b, e - b));
      }
    }
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }
  return out;
}

bool statementPosition(std::vector<Token> const &tokens, std::uint32_t off) {
  Token const *last = nullptr;
  for (Token const &t : tokens) {
    if (t.beg >= off) {
      break;
    }
    if (t.end >= off &&
        (t.kind == TokenKind::Identifier || t.kind == TokenKind::Keyword)) {
      // The word being typed ends at (or spans) the cursor: judge the context
      // before it, not the partial token itself.
      continue;
    }
    switch (t.kind) {
    case TokenKind::Comment:
    case TokenKind::DocComment:
    case TokenKind::Preprocessor:
    case TokenKind::Meta:
    case TokenKind::Eof:
      continue;
    default:
      last = &t;
      break;
    }
  }
  if (last == nullptr || last->kind == TokenKind::Newline) {
    return true;
  }
  if (last->kind == TokenKind::Symbol && last->text() == ":") {
    return true;
  }
  if (last->kind == TokenKind::Keyword) {
    std::string const lower = toLowerChars(std::string(last->text()));
    return lower == "then" || lower == "else";
  }
  return false;
}

namespace {

BlockCloser fromRow(const BlockRow &r) {
  BlockCloser c;
  c.kind = r.kind;
  c.closeWord = r.close;
  c.needsEnd = r.needsEnd;
  return c;
}

} // namespace

bool blockForOpener(std::string_view wordLower, BlockCloser *out) {
  auto const *const it =
      std::find_if(std::begin(kBlockOpeners), std::end(kBlockOpeners),
                   [&](BlockRow const &r) { return wordLower == r.opener; });
  if (it != std::end(kBlockOpeners)) {
    if (out != nullptr) {
      *out = fromRow(*it);
    }
    return true;
  }
  return false;
}

bool blockForCloser(std::string_view wordLower, BlockCloser *out) {
  if (out != nullptr) {
    *out = {};
  }
  for (const auto &r : kBlockOpeners) {
    if (r.needsEnd && wordLower == r.close) {
      if (out != nullptr) {
        *out = fromRow(r);
      }
      return true;
    }
  }
  auto const *const it =
      std::find_if(std::begin(kCloserOnly), std::end(kCloserOnly),
                   [&](BlockRow const &r) { return wordLower == r.opener; });
  if (it != std::end(kCloserOnly)) {
    if (out != nullptr) {
      *out = fromRow(*it);
    }
    return true;
  }
  return false;
}

std::string closerDisplay(const BlockCloser &closer) {
  std::string s;
  if (closer.needsEnd) {
    s = "END ";
  }
  for (char const c : closer.closeWord) {
    s.push_back(static_cast<char>(c - 'a' + 'A'));
  }
  return s;
}

std::string_view preprocessorWord(std::string_view line) {
  // line starts at '#', skip the '#', any whitespace, then take the word.
  size_t i = 1;
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
    ++i;
  }
  size_t const beg = i;
  while (i < line.size() &&
         ((line[i] >= 'a' && line[i] <= 'z') ||
          (line[i] >= 'A' && line[i] <= 'Z') || line[i] == '_')) {
    ++i;
  }
  if (i == beg) {
    return {};
  }
  // Directive names are compared case-insensitively downstream; return raw
  // and let callers lowercase.
  return line.substr(beg, i - beg);
}

bool isSuffixChar(char c) {
  return c == '$' || c == '%' || c == '&' || c == '!' || c == '#';
}

std::vector<std::string_view> symbolOperators() {
  // Order matters only for readability; callers that need longest-match
  // ordering sort themselves. Mirrors Lexer::lexSymbol() (src/lexer.cpp):
  // multi-char symbols first, then single chars. `&` is the address-of
  // operator (never a suffix), `?` the PRINT shortcut, `.` member access,
  // `...` the variadic marker. `^`, `|`, `~` and `=` lex through the default
  // single-char case.
  static constexpr std::string_view const kOperators[] = {
      "...", ".", "->", "-=", "-", "+=", "+", "*=", "*", "/=", "/", "\\=", "\\",
      "&=",  "&", "<=", "<>", "<", ">=", ">", "=",  "^", "|",  "~", "@",   "?"};
  return {std::begin(kOperators), std::end(kOperators)};
}

bool isCombinedAssignKeyword(std::string_view wordLower) {
  return wordLower == "and=" || wordLower == "or=" || wordLower == "xor=" ||
         wordLower == "eqv=" || wordLower == "imp=" || wordLower == "mod=" ||
         wordLower == "shl=" || wordLower == "shr=";
}

bool langFromDirective(std::string_view line, LangMode *out) {
  // Line starts at '#'. Expect `#LANG "name"`.
  size_t i = 1;
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
    ++i;
  }
  size_t const wbeg = i;
  while (i < line.size() && ((line[i] >= 'a' && line[i] <= 'z') ||
                             (line[i] >= 'A' && line[i] <= 'Z'))) {
    ++i;
  }
  std::string_view const word = line.substr(wbeg, i - wbeg);
  if (word.size() != 4 || (word[0] != 'l' && word[0] != 'L') ||
      (word[1] != 'a' && word[1] != 'A') ||
      (word[2] != 'n' && word[2] != 'N') ||
      (word[3] != 'g' && word[3] != 'G')) {
    return false;
  }
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
    ++i;
  }
  if (i >= line.size() || line[i] != '"') {
    return false;
  }
  ++i;
  size_t const sbeg = i;
  while (i < line.size() && line[i] != '"') {
    ++i;
  }
  if (i >= line.size()) {
    return false;
  }
  std::string_view const name = line.substr(sbeg, i - sbeg);

  LangMode mode;
  if (name == "fb") {
    mode = LangMode::Fb;
  } else if (name == "fblite") {
    mode = LangMode::FbLite;
  } else if (name == "qb") {
    mode = LangMode::Qb;
  } else if (name == "deprecated") {
    mode = LangMode::Deprecated;
  } else {
    return false;
  }
  if (out != nullptr) {
    *out = mode;
  }
  return true;
}

const char *langName(LangMode mode) {
  switch (mode) {
  case LangMode::Fb:
    return "fb";
  case LangMode::FbLite:
    return "fblite";
  case LangMode::Qb:
    return "qb";
  case LangMode::Deprecated:
    return "deprecated";
  }
  return "fb";
}

bool langFromMetaDirective(std::string_view text, LangMode *out) {
  // `$`-metacommands live inside comments: `'$LANG: "qb"` or `rem $LANG:"qb"`.
  // The comment body is passed in; scan for `$lang` (case-insensitive)
  // followed by whitespace, an optional ':', and a quoted dialect name.
  // Case-insensitive compare: ORing an ASCII letter with the space bit (0x20)
  // lowercases it, so `want` can be given lowercase.
  auto ci = [](char c, char want) { return (c | ' ') == want; };
  auto ws = [](char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  };
  size_t const n = text.size();
  for (size_t i = 0; i + LANG_DIRECTIVE_LEN - 1 < n; ++i) {
    if (text[i] != '$' || !ci(text[i + 1], 'l') || !ci(text[i + 2], 'a') ||
        !ci(text[i + 3], 'n') || !ci(text[i + 4], 'g')) {
      continue;
    }
    size_t j = i + LANG_DIRECTIVE_LEN;
    while (j < n && ws(text[j])) {
      ++j;
    }
    if (j < n && text[j] == ':') {
      ++j;
      while (j < n && ws(text[j])) {
        ++j;
      }
    }
    if (j >= n || text[j] != '"') {
      continue;
    }
    ++j;
    size_t const sbeg = j;
    while (j < n && text[j] != '"') {
      ++j;
    }
    if (j >= n) {
      return false;
    }
    std::string_view const name = text.substr(sbeg, j - sbeg);
    LangMode mode;
    if (name == "fb") {
      mode = LangMode::Fb;
    } else if (name == "fblite") {
      mode = LangMode::FbLite;
    } else if (name == "qb") {
      mode = LangMode::Qb;
    } else if (name == "deprecated") {
      mode = LangMode::Deprecated;
    } else {
      continue;
    }
    if (out != nullptr) {
      *out = mode;
    }
    return true;
  }
  return false;
}

} // namespace fblang
