// warning: line 26, column 23: MOV   o[TEX0].xy, v[8]; (Vertex attribute register v[8] (TEX0) will be mapped to hardware register v[9])
// cgc version 3.1.0013, build date Apr 18 2012
// command line args: -profile vp20
// source file: polymost_vs.vs.cg
//vendor NVIDIA Corporation
//version 3.1.0.13
//profile vp20
//program main
//semantic main.u_mvp
//semantic main.u_colour
//var float4 I.pos : $vin.POSITION : ATTR0 : 0 : 1
//var float2 I.tex : $vin.TEXCOORD : TEXCOORD0 : 0 : 1
//var float4x4 u_mvp :  : c[0], 4 : 1 : 1
//var float4 u_colour :  : c[4] : 2 : 1
//var float4 main.pos : $vout.POSITION : HPOS : -1 : 1
//var float4 main.col : $vout.COLOR : COL0 : -1 : 1
//var float2 main.tex0 : $vout.TEXCOORD0 : TEX0 : -1 : 1
// 9 instructions, 2 R-regs
0x00000000, 0x004c2055, 0x0836186c, 0x2f0007f8,
0x00000000, 0x008c0000, 0x0836186c, 0x1f0007f8,
0x00000000, 0x008c40aa, 0x0836186c, 0x1f0007f8,
0x00000000, 0x006c601b, 0x0436106c, 0x3f0007f8,
0x00000000, 0x0400001b, 0x08361300, 0x101807f8,
0x00000000, 0x0040001b, 0x0400286c, 0x2070e800,
0x00000000, 0x0020001b, 0x0436106c, 0x20701800,
0x00000000, 0x002c801b, 0x0c36106c, 0x2070f818,
0x00000000, 0x0020121b, 0x0836106c, 0x2070c849,
