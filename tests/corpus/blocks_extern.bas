'@fbc:pass
extern "C"
    declare function strlen cdecl (byval s as zstring ptr) as integer
end extern
print "ok"