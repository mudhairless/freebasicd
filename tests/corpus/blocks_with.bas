'@fbc:pass
type rect
    w as integer
    h as integer
end type

dim r as rect
with r
    .w = 3
    .h = 4
end with
print r.w * r.h