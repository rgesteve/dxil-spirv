#version 460

layout(location = 0) flat in ivec4 A;
layout(location = 0) out int SV_Target;

void main()
{
    SV_Target = int((int(uint(A.x)) < int(uint(A.y))) ? uint(A.z) : uint(A.w));
}


#if 0
// SPIR-V disassembly
; SPIR-V
; Version: 1.3
; Generator: Unknown(30017); 21022
; Bound: 35
; Schema: 0
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %3 "main" %8 %10
OpExecutionMode %3 OriginUpperLeft
OpName %3 "main"
OpName %8 "A"
OpName %10 "SV_Target"
OpDecorate %8 Flat
OpDecorate %8 Location 0
OpDecorate %10 Location 0
%1 = OpTypeVoid
%2 = OpTypeFunction %1
%5 = OpTypeInt 32 1
%6 = OpTypeVector %5 4
%7 = OpTypePointer Input %6
%8 = OpVariable %7 Input
%9 = OpTypePointer Output %5
%10 = OpVariable %9 Output
%11 = OpTypePointer Input %5
%13 = OpTypeInt 32 0
%14 = OpConstant %13 0
%18 = OpConstant %13 1
%22 = OpConstant %13 2
%26 = OpConstant %13 3
%29 = OpTypeBool
%3 = OpFunction %1 None %2
%4 = OpLabel
OpBranch %33
%33 = OpLabel
%12 = OpAccessChain %11 %8 %14
%15 = OpLoad %5 %12
%16 = OpBitcast %13 %15
%17 = OpAccessChain %11 %8 %18
%19 = OpLoad %5 %17
%20 = OpBitcast %13 %19
%21 = OpAccessChain %11 %8 %22
%23 = OpLoad %5 %21
%24 = OpBitcast %13 %23
%25 = OpAccessChain %11 %8 %26
%27 = OpLoad %5 %25
%28 = OpBitcast %13 %27
%30 = OpSLessThan %29 %16 %20
%31 = OpSelect %13 %30 %24 %28
%32 = OpBitcast %5 %31
OpStore %10 %32
OpReturn
OpFunctionEnd
#endif
