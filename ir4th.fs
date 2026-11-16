\ ir4th.fs - Gforth compatibility prelude for IR4th v0.3

\ Allow direct self-recursion in ': ... ;' words
: : ['] : execute reveal ;

\ At compile time in word bodies, A: is a no-op prefix before an address literal
: A: ; immediate

\ Fixed-size named data buffers & initialized data:
: buffer: ( n "name" -- ) create align allot ;
: i8, ( n -- ) c, ;
: i16, ( n -- ) dup c, 8 rshift c, ;

\ External declarations (resolved to pre-defined Gforth words)
: putchar ( c -- c ) dup emit ;
: getchar ( -- c ) key ;
: extern ( "name" "{" ... "}" -- )
  parse-name 2drop
  begin
    begin parse-name dup 0= while 2drop refill 0= abort" unexpected EOF in 'extern'" repeat
    s" }" str=
  until ;

\ Platform-aware require: checks <dir>gforth/<name> then <dir><name>
create ir4-req-path 256 allot
variable ir4-req-len
variable ir4-dir-len

: ir4-dirname ( addr len -- addr dirlen )
  begin dup 0> while
    2dup + 1- c@ [char] / = if exit then
    1-
  repeat ;

: ir4-app ( addr len -- )
  dup ir4-req-len @ + 250 > abort" required path too long"
  ir4-req-path ir4-req-len @ + swap dup ir4-req-len +! move ;

: ir4-try-req ( dir-a dir-u mid-a mid-u name-a name-u -- flag )
  0 ir4-req-len !
  2>r 2>r dup ir4-dir-len ! ir4-app 2r> ir4-app 2r> ir4-app
  ir4-req-path ir4-req-len @
  file-status nip 0= if
    ir4-req-path ir4-req-len @ ir4-dir-len @ /string required true
  else
    false
  then ;

: require ( "name" -- )
  parse-name 2>r
  sourcefilename ir4-dirname 2dup s" gforth/" 2r@ ir4-try-req if
    2drop 2r> 2drop exit
  then
  s" " 2r> ir4-try-req 0= abort" cannot open required file" ;

\ Override '{ ... }' to support '| <locals>' zero-initialization in Gforth 0.7.3
create ir4-hdr 256 allot
variable ir4-hlen
variable ir4-nloc
variable ir4-mode \ 0=inputs, 1=locals, 2=outputs

: ir4-append ( addr len -- )
  dup ir4-hlen @ + 250 > abort" IR4th header too long"
  ir4-hdr ir4-hlen @ + swap dup ir4-hlen +! move
  bl ir4-hdr ir4-hlen @ + c! 1 ir4-hlen +! ;

' { constant ir4-orig-{

: { ( -- )
  0 ir4-hlen !
  0 ir4-nloc !
  0 ir4-mode !
  begin
    begin parse-name dup 0= while 2drop refill 0= abort" unexpected EOF in '{ ... }'" repeat
    2dup s" }" str= if 2drop true else
      2dup s" |" str= if 2drop 1 ir4-mode ! false else
      2dup s" --" str= if 2drop 2 ir4-mode ! false else
        ir4-mode @ 2 < if
          2dup s" W:" str= >r
          2dup s" A:" str= >r
          2dup s" F:" str= r> r> or or if
            2drop
          else
            ir4-mode @ 1 = if 1 ir4-nloc +! then
            ir4-append
          then
        else
          2drop
        then
        false
      then then
    then
  until
  s" -- }" ir4-append
  ir4-nloc @ 0 ?do 0 postpone literal loop
  ir4-orig-{ execute
  ir4-hdr ir4-hlen @ evaluate
; immediate

: a+ + ;
: a- - ;
: adiff - ;
: a= = ;
: a<> <> ;
: au< u< ;
: au> u> ;
: a@ @ ;
: a! ! ;
: sext $FF and dup $80 and if -256 or then ;
: i8@ c@ sext ;
: u8@ c@ ;
: i8! c! ;
: i16@ sw@ ;
: u16@ uw@ ;
: i16! w! ;
: w>a ;
: a>w ;
: arshift 0 ?do 2/ loop ;

\ Bridge Gforth's separate F-stack to IR4th's unified stack (32-bit IEEE float)
fvariable ir4-ftmp
: ir4-fpop ( w -- F: r ) ir4-ftmp ! ir4-ftmp sf@ ;
: ir4-fpush ( F: r -- w ) 0 ir4-ftmp ! ir4-ftmp sf! ir4-ftmp @ ;
: ir4-f2pop ( w1 w2 -- F: r1 r2 ) >r ir4-fpop r> ir4-fpop ;
: F: parse-name >float 0= abort" invalid float literal" ir4-fpush state @ if postpone literal then ; immediate
' f+ constant ir4-gf-f+   : f+ ir4-f2pop ir4-gf-f+ execute ir4-fpush ;
' f- constant ir4-gf-f-   : f- ir4-f2pop ir4-gf-f- execute ir4-fpush ;
' f* constant ir4-gf-f*   : f* ir4-f2pop ir4-gf-f* execute ir4-fpush ;
' f/ constant ir4-gf-f/   : f/ ir4-f2pop ir4-gf-f/ execute ir4-fpush ;
' fnegate constant ir4-gf-fnegate : fnegate ir4-fpop ir4-gf-fnegate execute ir4-fpush ;
' f= constant ir4-gf-f=   : f= ir4-f2pop ir4-gf-f= execute ;
' f<> constant ir4-gf-f<> : f<> ir4-f2pop ir4-gf-f<> execute ;
' f< constant ir4-gf-f<   : f< ir4-f2pop ir4-gf-f< execute ;
' f> constant ir4-gf-f>   : f> ir4-f2pop ir4-gf-f> execute ;
: w>f s>f ir4-fpush ;
: f>w ir4-fpop f>s ;
: f! ( w addr -- ) swap ir4-ftmp ! ir4-ftmp swap 4 move ;
: f@ ( addr -- w ) 0 ir4-ftmp ! ir4-ftmp 4 move ir4-ftmp @ ;
