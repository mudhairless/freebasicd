'@fbc:pass
sub counter()
    static n as integer
    n += 1
    print n
end sub

counter()
counter()