/*
 * dxgi.dll forwarding thunks.
 *
 * Every export jumps straight to the matching function in the real System32\dxgi.dll. The thunks
 * are signature-agnostic: all argument registers and the stack are left untouched, so undocumented
 * exports (DXGID3D10CreateDevice and friends) are forwarded exactly. On the first call the real DLL
 * is resolved by vu_dxgi_resolve(); the four integer and four float argument registers are saved
 * around that call.
 */
    .intel_syntax noprefix
    .text

.macro VU_THUNK name, idx
    .globl vu_thunk_\name
    .p2align 4
vu_thunk_\name:
    mov rax, qword ptr [rip + vu_dxgi_real + 8*\idx]
    test rax, rax
    jz 1f
    jmp rax
1:
    push rcx
    push rdx
    push r8
    push r9
    sub rsp, 104                        /* 32 shadow + 64 xmm save + 8 align -> rsp % 16 == 0 */
    movdqu xmmword ptr [rsp + 32], xmm0
    movdqu xmmword ptr [rsp + 48], xmm1
    movdqu xmmword ptr [rsp + 64], xmm2
    movdqu xmmword ptr [rsp + 80], xmm3
    mov ecx, \idx
    call vu_dxgi_resolve
    movdqu xmm0, xmmword ptr [rsp + 32]
    movdqu xmm1, xmmword ptr [rsp + 48]
    movdqu xmm2, xmmword ptr [rsp + 64]
    movdqu xmm3, xmmword ptr [rsp + 80]
    add rsp, 104
    pop r9
    pop r8
    pop rdx
    pop rcx
    jmp rax
.endm

/* Indices must match kDxgiExportNames[] in proxy.cpp */
VU_THUNK CreateDXGIFactory,               0
VU_THUNK CreateDXGIFactory1,              1
VU_THUNK CreateDXGIFactory2,              2
VU_THUNK DXGIGetDebugInterface1,          3
VU_THUNK DXGIDeclareAdapterRemovalSupport, 4
VU_THUNK DXGIDisableVBlankVirtualization, 5
VU_THUNK DXGID3D10CreateDevice,           6
VU_THUNK DXGID3D10CreateLayeredDevice,    7
VU_THUNK DXGID3D10ETWRundown,             8
VU_THUNK DXGID3D10GetLayeredDeviceSize,   9
VU_THUNK DXGID3D10RegisterLayers,         10
VU_THUNK DXGIDumpJournal,                 11
VU_THUNK DXGIReportAdapterConfiguration,  12
VU_THUNK CompatString,                    13
VU_THUNK CompatValue,                     14
VU_THUNK ApplyCompatResolutionQuirking,   15
VU_THUNK SetAppCompatStringPointer,       16
VU_THUNK UpdateHMDEmulationStatus,        17
VU_THUNK PIXBeginCapture,                 18
VU_THUNK PIXEndCapture,                   19
VU_THUNK PIXGetCaptureState,              20
