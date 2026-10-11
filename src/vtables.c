#define CINTERFACE
#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11_4.h>
#include <d3d10.h>

#include "vtables.h"

void vu_get_ctx_fns(void *ctxp, void *ctx1p, VuCtxFns *out)
{
    ID3D11DeviceContext *ctx = (ID3D11DeviceContext *)ctxp;
    ID3D11DeviceContext1 *ctx1 = (ID3D11DeviceContext1 *)ctx1p;
    out->PSSetShaderResources = (void *)ctx->lpVtbl->PSSetShaderResources;
    out->CSSetShaderResources = (void *)ctx->lpVtbl->CSSetShaderResources;
    out->Draw = (void *)ctx->lpVtbl->Draw;
    out->DrawIndexed = (void *)ctx->lpVtbl->DrawIndexed;
    out->DrawInstanced = (void *)ctx->lpVtbl->DrawInstanced;
    out->DrawIndexedInstanced = (void *)ctx->lpVtbl->DrawIndexedInstanced;
    out->DrawAuto = (void *)ctx->lpVtbl->DrawAuto;
    out->DrawInstancedIndirect = (void *)ctx->lpVtbl->DrawInstancedIndirect;
    out->DrawIndexedInstancedIndirect = (void *)ctx->lpVtbl->DrawIndexedInstancedIndirect;
    out->ClearState = (void *)ctx->lpVtbl->ClearState;
    out->CopyResource = (void *)ctx->lpVtbl->CopyResource;
    out->CopySubresourceRegion = (void *)ctx->lpVtbl->CopySubresourceRegion;
    out->CopySubresourceRegion1 = ctx1 ? (void *)ctx1->lpVtbl->CopySubresourceRegion1 : NULL;
    out->FinishCommandList = (void *)ctx->lpVtbl->FinishCommandList;
    out->ExecuteCommandList = (void *)ctx->lpVtbl->ExecuteCommandList;
    out->vtbl = (void **)ctx->lpVtbl;
    out->vtblCount = (int)(sizeof(ID3D11DeviceContextVtbl) / sizeof(void *));
}

void vu_get_dev_fns(void *devp, void *dev1p, VuDevFns *out)
{
    ID3D11Device *dev = (ID3D11Device *)devp;
    ID3D11Device1 *dev1 = (ID3D11Device1 *)dev1p;
    out->OpenSharedResource = (void *)dev->lpVtbl->OpenSharedResource;
    out->OpenSharedResource1 = dev1 ? (void *)dev1->lpVtbl->OpenSharedResource1 : NULL;
    out->OpenSharedResourceByName = dev1 ? (void *)dev1->lpVtbl->OpenSharedResourceByName : NULL;
    out->CreateDeferredContext = (void *)dev->lpVtbl->CreateDeferredContext;
    out->CreateDeferredContext1 = dev1 ? (void *)dev1->lpVtbl->CreateDeferredContext1 : NULL;
}

void vu_get_mt_fns(void *mt11p, void *mt10p, VuMtFns *out)
{
    ID3D11Multithread *mt11 = (ID3D11Multithread *)mt11p;
    ID3D10Multithread *mt10 = (ID3D10Multithread *)mt10p;
    out->SetMultithreadProtected11 = mt11 ? (void *)mt11->lpVtbl->SetMultithreadProtected : NULL;
    out->SetMultithreadProtected10 = mt10 ? (void *)mt10->lpVtbl->SetMultithreadProtected : NULL;
}

int vu_mt11_get(void *mt11p)
{
    ID3D11Multithread *mt11 = (ID3D11Multithread *)mt11p;
    return mt11->lpVtbl->GetMultithreadProtected(mt11) ? 1 : 0;
}

int vu_mt11_set(void *mt11p, int on)
{
    ID3D11Multithread *mt11 = (ID3D11Multithread *)mt11p;
    return mt11->lpVtbl->SetMultithreadProtected(mt11, on ? TRUE : FALSE) ? 1 : 0;
}
