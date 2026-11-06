'@fbc:pass
extern sharedCount as integer
dim sharedCount as integer
extern "C"
    declare function strlen cdecl (byval s as zstring ptr) as integer
end extern
print sharedCount
