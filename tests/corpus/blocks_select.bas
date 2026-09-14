'@fbc:pass
dim q as integer = 5
select case q
case 1, 2
    print "low"
case 3 to 4
    print "mid"
case is > 4
    print "high"
case else
    print "other"
end select