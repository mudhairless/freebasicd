'@fbc:pass
' An Asm body whose mnemonics collide with FreeBASIC keywords (`sub`,
' `call`) and whose label is a colon line. fbc passes the body through to
' the assembler, so the parser must read it as raw text: the `sub`
' instruction is not a Sub declaration, and the asm label is not a
' FreeBASIC label.

dim shared ticks as long

sub noop()
end sub

sub poll()
    asm
        call noop
        jmp counted
        sub eax, 1
        counted:
        add eax, 1
        mov [ticks], eax
    end asm
end sub

poll()
print ticks
