'@fbc:fail
' M23 G6 statement-specific checks: one diagnostic per broken statement.
dim a(1) as integer
redim a(2)
dim b() as integer = { 1, 2 }
dim shared s as string = "hi"
common c(1) as integer
private redim d(1)
seek #
