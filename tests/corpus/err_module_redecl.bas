'@fbc:fail
' The module-level x survives a shadowing block; a second module-level
' declaration is a duplicate (error 4, fbc-probed).
dim x as integer
if true then
    dim x as string
end if
dim x as integer