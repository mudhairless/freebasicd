'@fbc:fail
' A same-scope redefinition inside one declaration block is still an error 4.
scope
    dim x as string
    dim x as integer
end scope